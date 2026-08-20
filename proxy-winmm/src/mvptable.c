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
    int mvpx_offset; /* -1 == not found for this shader */
    int contiguous;  /* Task 6: 1 iff mvpmatrixy/z/w were confirmed to sit at
                         mvpx_offset+16/+32/+48 (see mvptable_on_shader_created()'s
                         own contiguity check, previously computed and logged but
                         discarded - now kept so mvp_row_offsets_for_shader() can
                         refuse to hand out y/z/w offsets it isn't sure of). */
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
static int mvp_record_locked(uint64_t hash, UINT cb0_size, int mvpx_offset, int contiguous) {
    struct MvpEntry *e = mvp_find_locked(hash);
    if (e != NULL) {
        return 1;
    }
    e = mvp_find_or_add_locked(hash);
    if (e != NULL) {
        e->cb0_size = cb0_size;
        e->mvpx_offset = mvpx_offset;
        e->contiguous = contiguous;
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

void mvptable_on_shader_created(uint64_t hash, const void *bytecode, SIZE_T length) {
    ID3D11ShaderReflection *refl = NULL;
    ID3D11ShaderReflectionConstantBuffer *cb = NULL;
    D3D11_SHADER_BUFFER_DESC bufDesc;
    HRESULT hr;
    int cb0_size = 0;
    int mvpx = -1;
    int contiguous = 0;
    int already;

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

    hr = g_d3dreflect(bytecode, length, &IID_ID3D11ShaderReflection, (void **)&refl);
    if (FAILED(hr) || refl == NULL) {
        log_msg("mvptable: D3DReflect failed for hash=%016llX (hr=0x%08lX)",
                 (unsigned long long)hash, (unsigned long)hr);
        return;
    }

    memset(&bufDesc, 0, sizeof(bufDesc));
    reflect_find_cb0(refl, &bufDesc, &cb);

    if (cb != NULL) {
        ID3D11ShaderReflectionVariable *vx;
        cb0_size = (int)bufDesc.Size;
        vx = cb->lpVtbl->GetVariableByName(cb, "mvpmatrixx");
        if (vx != NULL) {
            D3D11_SHADER_VARIABLE_DESC vd;
            if (SUCCEEDED(vx->lpVtbl->GetDesc(vx, &vd))) {
                static const char *const kNext[3] = { "mvpmatrixy", "mvpmatrixz", "mvpmatrixw" };
                int k, ok = 1;

                mvpx = (int)vd.StartOffset;
                for (k = 0; k < 3; k++) {
                    ID3D11ShaderReflectionVariable *vk = cb->lpVtbl->GetVariableByName(cb, kNext[k]);
                    D3D11_SHADER_VARIABLE_DESC vkd;
                    if (vk == NULL || FAILED(vk->lpVtbl->GetDesc(vk, &vkd)) ||
                        vkd.StartOffset != vd.StartOffset + 16u * (UINT)(k + 1)) {
                        ok = 0;
                        break;
                    }
                }
                contiguous = ok;
                if (!ok) {
                    log_msg("mvptable: WARNING hash=%016llX mvpmatrixx at +%d but y/z/w are "
                             "not contiguous at +16/+32/+48 (recorded anyway)",
                             (unsigned long long)hash, mvpx);
                }
            }
        }
    }

    refl->lpVtbl->Release(refl);

    /* Re-check-and-reserve atomically under one lock acquisition (TOCTOU
     * fix, post-review): the fast-path check above was released before
     * this (necessarily unlocked, D3DReflect-calling) reflection work ran,
     * so another thread could have reflected and recorded the identical
     * hash in the meantime (two threads racing CreateVertexShader with
     * the same bytecode). mvp_record_locked() reports whether THIS thread
     * won that race; only the winner writes the log line, so
     * mvp_offsets.log can never get two lines for the same hash. */
    EnterCriticalSection(&g_mvp_cs);
    already = mvp_record_locked(hash, (UINT)cb0_size, mvpx, contiguous);
    if (!already && g_mvp_fp != NULL) {
        fprintf(g_mvp_fp, "hash=%016llX cb0=%d mvpx=%d contiguous=%d\n",
                 (unsigned long long)hash, cb0_size, mvpx, contiguous);
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

/* Task 6: the CB0-content patch mechanism (mvp_patch.c) needs the byte
 * offset of ALL FOUR mvpmatrix{x,y,z,w} rows, not just x - and, per the
 * mvp_offsets.log WARNINGs observed during Task 6 discovery gameplay
 * capture, y/z/w are NOT always contiguous at mvpx_offset+16/+32/+48 (some
 * shaders reflect them elsewhere, e.g. +64/+96/+144). mvp_offset_for_shader()
 * alone cannot tell a caller which case it is looking at, so patching would
 * silently corrupt (not crash) draws using a non-contiguous shader. This
 * function refuses to hand out row offsets at all unless the contiguous
 * layout was confirmed at reflection time, so mvp_patch.c can fall through
 * to "skip, draw unpatched" - the same fail-safe posture as an unknown
 * shader - for the shaders it can't safely reach yet, rather than guessing.
 * Returns 1 and fills row_offsets[0..3] = {mvpx, mvpx+16, mvpx+32, mvpx+48}
 * only when the shader is known, has a reflected mvpx_offset, AND was
 * confirmed contiguous; returns 0 (row_offsets untouched) otherwise. */
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
    if (e != NULL && e->mvpx_offset >= 0 && e->contiguous) {
        row_offsets[0] = e->mvpx_offset;
        row_offsets[1] = e->mvpx_offset + 16;
        row_offsets[2] = e->mvpx_offset + 32;
        row_offsets[3] = e->mvpx_offset + 48;
        ok = 1;
    }
    LeaveCriticalSection(&g_mvp_cs);

    return ok;
}
