#include "mvptable.h"

#include <stdio.h>
#include <string.h>

#define COBJMACROS
#include <windows.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>

#include "log.h"
#include "shaderdump.h"

/* D3DReflect's own prototype (d3dcompiler.h) matches this exactly; typedef'd
 * locally only so GetProcAddress's return value has a concrete type to cast
 * to. */
typedef HRESULT(WINAPI *D3DReflect_t)(const void *data, SIZE_T data_size, REFIID riid, void **reflector);

/* Open-addressed hash(blob) -> {cb0 size, mvpx offset} table, same probe
 * scheme as shaderdump.c's shader-ptr table. 4096 slots comfortably covers
 * every distinct vertex-shader blob a title like this ships (shaderdump.c
 * sizes its own ptr-keyed table at 8192 for the same reason). hash==0 is
 * the free-slot sentinel - like shaderdump.c's NULL-pointer sentinel, this
 * assumes no real DXBC blob ever hashes to exactly 0 under FNV-1a64, which
 * has never been observed and is astronomically unlikely. */
#define MVP_TABLE_SIZE 4096
struct MvpEntry {
    uint64_t hash;
    UINT cb0_size;
    int mvpx_offset; /* -1 == not found for this shader; == rows[0] when valid */
    int rows[4];     /* 2026-09-03: the ACTUAL reflected byte offsets of
                         mvpmatrix{x,y,z,w}, -1 for any row not present. The
                         previous version computed these, compared them against
                         {x,+16,+32,+48}, kept only the boolean and threw the
                         offsets away - which is why every non-contiguous shader
                         had to be skipped. */
    int rows_valid;  /* 1 iff all four rows were found (they need not be
                         contiguous; the caller bounds-checks each one). */
};
static struct MvpEntry g_mvp[MVP_TABLE_SIZE];
static int g_mvp_count = 0;
static int g_mvp_full_warned = 0;

static CRITICAL_SECTION g_mvp_cs;
static int g_mvp_cs_ready = 0;

static FILE *g_mvp_fp = NULL;

static D3DReflect_t g_d3dreflect = NULL;
static int g_d3dreflect_tried = 0; /* LoadLibrary/GetProcAddress attempted once */
static int g_d3dreflect_ok = 0;

/* ---- table helpers (caller holds g_mvp_cs) ---- */

static struct MvpEntry *mvp_find_locked(uint64_t hash) {
    UINT idx = (UINT)((hash * 11400714819323198485ULL) >> 51) & (MVP_TABLE_SIZE - 1);
    UINT i;
    for (i = 0; i < MVP_TABLE_SIZE; i++) {
        struct MvpEntry *e = &g_mvp[(idx + i) & (MVP_TABLE_SIZE - 1)];
        if (e->hash == hash) {
            return e;
        }
        if (e->hash == 0) {
            return NULL; /* no deletions ever happen, so a free slot means "not present" */
        }
    }
    return NULL;
}

static struct MvpEntry *mvp_find_or_add_locked(uint64_t hash) {
    UINT idx = (UINT)((hash * 11400714819323198485ULL) >> 51) & (MVP_TABLE_SIZE - 1);
    UINT i;
    for (i = 0; i < MVP_TABLE_SIZE; i++) {
        struct MvpEntry *e = &g_mvp[(idx + i) & (MVP_TABLE_SIZE - 1)];
        if (e->hash == hash) {
            return e;
        }
        if (e->hash == 0) {
            if (g_mvp_count >= MVP_TABLE_SIZE - (MVP_TABLE_SIZE / 8)) {
                break; /* keep load factor sane; treat as full */
            }
            e->hash = hash;
            g_mvp_count++;
            return e;
        }
    }
    if (!g_mvp_full_warned) {
        g_mvp_full_warned = 1;
        log_msg("mvptable: table full (%d); further shaders won't get mvp-offset entries", g_mvp_count);
    }
    return NULL;
}

/* Atomic check-and-reserve, caller holds g_mvp_cs - mirrors shaderdump.c's
 * hash_already_dumped_locked() (check-and-mark under one lock acquisition,
 * so nothing outside the lock can observe a state where the table says
 * "known" but the log line hasn't been written yet, or vice versa).
 * Returns 1 if `hash` was already present (another thread reflected this
 * exact blob and recorded it first - e.g. concurrent CreateVertexShader
 * calls on the same DXBC bytecode racing past the earlier, unlocked,
 * fast-path check in mvptable_on_shader_created()); the caller must then
 * skip writing another mvp_offsets.log line for it. Returns 0 and inserts
 * the entry otherwise - the caller (still holding the lock) is then, and
 * only then, responsible for the log line. */
static int mvp_record_locked(uint64_t hash, UINT cb0_size, int mvpx_offset, const int rows[4]) {
    struct MvpEntry *e = mvp_find_locked(hash);
    int k;
    if (e != NULL) {
        return 1;
    }
    e = mvp_find_or_add_locked(hash);
    if (e != NULL) {
        e->cb0_size = cb0_size;
        e->mvpx_offset = mvpx_offset;
        e->rows_valid = 1;
        for (k = 0; k < 4; k++) {
            e->rows[k] = rows[k];
            if (rows[k] < 0) {
                e->rows_valid = 0;
            }
        }
    }
    return 0;
}

/* ---- D3DReflect lazy load ---- */

static int ensure_d3dreflect(void) {
    HMODULE mod;

    if (g_d3dreflect_tried) {
        return g_d3dreflect_ok;
    }
    g_d3dreflect_tried = 1;

    mod = LoadLibraryW(L"d3dcompiler_47.dll");
    if (mod == NULL) {
        log_msg("mvptable: LoadLibrary(d3dcompiler_47.dll) failed (gle=%lu); "
                 "mvp-offset reflection disabled for this session", (unsigned long)GetLastError());
        return 0;
    }

    g_d3dreflect = (D3DReflect_t)GetProcAddress(mod, "D3DReflect");
    if (g_d3dreflect == NULL) {
        log_msg("mvptable: GetProcAddress(D3DReflect) failed; mvp-offset reflection disabled for this session");
        return 0;
    }

    g_d3dreflect_ok = 1;
    log_msg("mvptable: D3DReflect resolved from d3dcompiler_47.dll");
    return 1;
}

/* ---- log file ---- */

static int mvp_open_logfile(void) {
    wchar_t local_appdata[MAX_PATH];
    wchar_t dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", local_appdata, MAX_PATH);

    if (len == 0 || len >= MAX_PATH) {
        return 0;
    }

    swprintf(dir, MAX_PATH, L"%s\\TEWVR", local_appdata);
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return 0;
    }

    swprintf(path, MAX_PATH, L"%s\\mvp_offsets.log", dir);
    g_mvp_fp = _wfopen(path, L"w"); /* fresh per-session file, same convention as cbdump.log */
    return g_mvp_fp != NULL;
}

/* ---- reflection ---- */

/* Finds the cb0 constant buffer: `constantBufferV` by name first (the name
 * Task 4's RDEF inspection established), falling back to whichever cbuffer
 * is bound to register b0 via GetResourceBindingDesc. On success, `*out_cb`
 * is a borrowed reflection pointer (owned by `refl`; do not Release it
 * separately) and `*out_desc` is filled in. `*out_cb` stays NULL if no cb0
 * could be identified either way. */
static void reflect_find_cb0(ID3D11ShaderReflection *refl, D3D11_SHADER_BUFFER_DESC *out_desc,
                              ID3D11ShaderReflectionConstantBuffer **out_cb) {
    ID3D11ShaderReflectionConstantBuffer *cb;

    *out_cb = NULL;

    cb = refl->lpVtbl->GetConstantBufferByName(refl, "constantBufferV");
    if (cb != NULL && SUCCEEDED(cb->lpVtbl->GetDesc(cb, out_desc))) {
        *out_cb = cb;
        return;
    }

    {
        D3D11_SHADER_DESC sd;
        UINT i;
        memset(&sd, 0, sizeof(sd));
        if (FAILED(refl->lpVtbl->GetDesc(refl, &sd))) {
            return;
        }
        for (i = 0; i < sd.BoundResources; i++) {
            D3D11_SHADER_INPUT_BIND_DESC bind;
            if (FAILED(refl->lpVtbl->GetResourceBindingDesc(refl, i, &bind))) {
                continue;
            }
            if (bind.Type == D3D_SIT_CBUFFER && bind.BindPoint == 0) {
                cb = refl->lpVtbl->GetConstantBufferByName(refl, bind.Name);
                if (cb != NULL && SUCCEEDED(cb->lpVtbl->GetDesc(cb, out_desc))) {
                    *out_cb = cb;
                }
                return;
            }
        }
    }
}

void mvptable_init(void) {
    if (g_mvp_cs_ready) {
        return; /* shaderdump_install() only calls this once per process, but stay idempotent */
    }
    InitializeCriticalSection(&g_mvp_cs);
    g_mvp_cs_ready = 1;

    if (!mvp_open_logfile()) {
        log_msg("mvptable: failed to open mvp_offsets.log; table stays in-memory only");
    }
}

void mvptable_shutdown(void) {
    if (!g_mvp_cs_ready) {
        return;
    }
    if (g_mvp_fp != NULL) {
        fclose(g_mvp_fp);
        g_mvp_fp = NULL;
    }
    DeleteCriticalSection(&g_mvp_cs);
    g_mvp_cs_ready = 0;
}

/* Reflect one DXBC blob: cb0's size, and the byte offset of each of
 * mvpmatrix{x,y,z,w} (-1 for a row that is not there). Returns 1 if a cb0 was
 * found at all.
 *
 * Split out of mvptable_on_shader_created() on 2026-09-03 so this exact code -
 * not a transcription of it - can be run offline over shaders extracted from
 * the game's archives. It touches no table, no log and no global state beyond
 * the lazily-loaded D3DReflect pointer.
 *
 * The rows are reported as reflection gives them. Whether they happen to be
 * contiguous is the caller's business, not a reason to withhold them. */
int mvptable_reflect_rows(const void *bytecode, SIZE_T length, int *out_cb0_size, int out_rows[4]) {
    static const char *const kRowNames[4] = {
        "mvpmatrixx", "mvpmatrixy", "mvpmatrixz", "mvpmatrixw"
    };
    ID3D11ShaderReflection *refl = NULL;
    ID3D11ShaderReflectionConstantBuffer *cb = NULL;
    D3D11_SHADER_BUFFER_DESC bufDesc;
    HRESULT hr;
    int k, found_cb = 0;

    if (out_cb0_size != NULL) {
        *out_cb0_size = 0;
    }
    for (k = 0; k < 4; k++) {
        out_rows[k] = -1;
    }
    if (bytecode == NULL || length == 0 || !ensure_d3dreflect()) {
        return 0;
    }

    hr = g_d3dreflect(bytecode, length, &IID_ID3D11ShaderReflection, (void **)&refl);
    if (FAILED(hr) || refl == NULL) {
        return 0;
    }

    memset(&bufDesc, 0, sizeof(bufDesc));
    reflect_find_cb0(refl, &bufDesc, &cb);
    if (cb != NULL) {
        found_cb = 1;
        if (out_cb0_size != NULL) {
            *out_cb0_size = (int)bufDesc.Size;
        }
        for (k = 0; k < 4; k++) {
            ID3D11ShaderReflectionVariable *v = cb->lpVtbl->GetVariableByName(cb, kRowNames[k]);
            D3D11_SHADER_VARIABLE_DESC vd;
            if (v != NULL && SUCCEEDED(v->lpVtbl->GetDesc(v, &vd))) {
                out_rows[k] = (int)vd.StartOffset;
            }
        }
    }

    refl->lpVtbl->Release(refl);
    return found_cb;
}

void mvptable_on_shader_created(uint64_t hash, const void *bytecode, SIZE_T length) {
    int rows[4];
    int cb0_size = 0;
    int mvpx;
    int contiguous;
    int already;
    int k;

    if (!g_mvp_cs_ready || bytecode == NULL || length == 0) {
        return;
    }

    EnterCriticalSection(&g_mvp_cs);
    already = (mvp_find_locked(hash) != NULL);
    LeaveCriticalSection(&g_mvp_cs);
    if (already) {
        return; /* already reflected + recorded this exact blob */
    }

    if (!ensure_d3dreflect()) {
        return; /* failure already logged once, inside ensure_d3dreflect() */
    }

    if (!mvptable_reflect_rows(bytecode, length, &cb0_size, rows)) {
        /* Deliberately vague, because these two really are indistinguishable
         * here: D3DReflect itself can fail, or it can succeed on a shader that
         * simply has no cb0. The pre-2026-09-03 code logged the first case
         * only, which read as a stronger claim than it was. */
        log_msg("mvptable: no cb0 for hash=%016llX (D3DReflect failed, or this shader has none)",
                 (unsigned long long)hash);
        /* Still recorded below, with every row -1, so this hash is not
         * re-reflected on every subsequent CreateVertexShader for it. */
    }
    mvpx = rows[0];

    /* Contiguity is now DIAGNOSTIC ONLY - it is logged because the historical
     * mvp_offsets.log format carries it and old logs are still parsed against
     * it, but it no longer gates anything. Before 2026-09-03 a 0 here meant the
     * draw was skipped. */
    contiguous = (mvpx >= 0);
    for (k = 1; k < 4; k++) {
        if (rows[k] != mvpx + 16 * k) {
            contiguous = 0;
        }
    }

    EnterCriticalSection(&g_mvp_cs);
    already = mvp_record_locked(hash, (UINT)cb0_size, mvpx, rows);
    if (!already && g_mvp_fp != NULL) {
        fprintf(g_mvp_fp, "hash=%016llX cb0=%d mvpx=%d contiguous=%d rows=%d,%d,%d,%d\n",
                 (unsigned long long)hash, cb0_size, mvpx, contiguous,
                 rows[0], rows[1], rows[2], rows[3]);
        fflush(g_mvp_fp);
    }
    LeaveCriticalSection(&g_mvp_cs);
}

int mvp_offset_for_shader(const void *vs_ptr) {
    uint64_t hash;
    struct MvpEntry *e;
    int result = -1;

    if (!g_mvp_cs_ready || vs_ptr == NULL) {
        return -1;
    }

    hash = shaderdump_hash_for_shader(vs_ptr);
    if (hash == 0) {
        return -1;
    }

    EnterCriticalSection(&g_mvp_cs);
    e = mvp_find_locked(hash);
    if (e != NULL) {
        result = e->mvpx_offset;
    }
    LeaveCriticalSection(&g_mvp_cs);

    return result;
}

/* Task 6: the CB0-content patch mechanism (mvp_patch.c) needs the byte offset
 * of ALL FOUR mvpmatrix{x,y,z,w} rows, not just x - and, per the
 * mvp_offsets.log WARNINGs observed during Task 6 discovery gameplay capture,
 * y/z/w are NOT always contiguous at mvpx_offset+16/+32/+48.
 *
 * The original response was to refuse those shaders outright, because the
 * table had kept only a contiguity boolean and the real offsets had been
 * discarded - so patching them would have meant guessing. That was the right
 * call given what was stored, and the wrong thing to store: reflection had the
 * four offsets in hand and threw three of them away.
 *
 * Since 2026-09-03 the table keeps all four, so this hands out what reflection
 * actually reported and nothing is guessed. Non-contiguity is no longer a
 * refusal; an incomplete set of rows still is. The fail-safe posture is
 * unchanged - a shader whose rows are not all known still falls through to
 * "skip, draw unpatched" - and mvp_patch.c bounds-checks every offset against
 * the real bound buffer before using any of them, which now matters more,
 * because the rows are no longer guaranteed to be in ascending order.
 *
 * Returns 1 and fills row_offsets[0..3] with the reflected offsets when the
 * shader is known and all four rows were found; returns 0 (row_offsets
 * untouched) otherwise. */
int mvp_row_offsets_for_shader(const void *vs_ptr, int row_offsets[4]) {
    uint64_t hash;
    struct MvpEntry *e;
    int ok = 0;

    if (!g_mvp_cs_ready || vs_ptr == NULL || row_offsets == NULL) {
        return 0;
    }

    hash = shaderdump_hash_for_shader(vs_ptr);
    if (hash == 0) {
        return 0;
    }

    EnterCriticalSection(&g_mvp_cs);
    e = mvp_find_locked(hash);
    if (e != NULL && e->rows_valid) {
        int k;
        for (k = 0; k < 4; k++) {
            row_offsets[k] = e->rows[k];
        }
        ok = 1;
    }
    LeaveCriticalSection(&g_mvp_cs);

    return ok;
}

enum MvpShaderStatus mvp_shader_status_for_shader(const void *vs_ptr) {
    uint64_t hash;
    struct MvpEntry *e;
    enum MvpShaderStatus status;

    if (!g_mvp_cs_ready || vs_ptr == NULL) {
        return MVP_SHADER_UNKNOWN;
    }

    hash = shaderdump_hash_for_shader(vs_ptr);
    if (hash == 0) {
        return MVP_SHADER_UNKNOWN;
    }

    EnterCriticalSection(&g_mvp_cs);
    e = mvp_find_locked(hash);
    if (e == NULL) {
        status = MVP_SHADER_UNKNOWN;
    } else if (e->mvpx_offset < 0) {
        status = MVP_SHADER_NO_MVP;
    } else if (!e->rows_valid) {
        status = MVP_SHADER_ROWS_INCOMPLETE;
    } else {
        status = MVP_SHADER_OK;
    }
    LeaveCriticalSection(&g_mvp_cs);

    return status;
}
