#include "shaderdump.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "MinHook.h"
#include "minhook_glue.h"
#include "log.h"
#include "mvptable.h"

/* ---- vtable indices (same verified counting scheme as cbdump.c) ----
 * ID3D11Device: 3 IUnknown slots, then the interface's own methods in
 * declaration order (CreateBuffer=3 ... CreateInputLayout=11,
 * CreateVertexShader=12).
 * ID3D11DeviceContext: 3 IUnknown + 4 ID3D11DeviceChild = 7, then
 * VSSetConstantBuffers=7, PSSetShaderResources=8, PSSetShader=9,
 * PSSetSamplers=10, VSSetShader=11, DrawIndexed=12, Draw=13, Map=14,
 * Unmap=15, PSSetConstantBuffers=16, IASetInputLayout=17,
 * IASetVertexBuffers=18, IASetIndexBuffer=19, DrawIndexedInstanced=20,
 * DrawInstanced=21. */
#define VTBL_DEV_CREATEVERTEXSHADER 12
#define VTBL_CTX_VSSETSHADER        11
#define VTBL_CTX_DRAWINDEXED        12
#define VTBL_CTX_DRAW               13
#define VTBL_CTX_DRAWINDEXEDINST    20
#define VTBL_CTX_DRAWINST           21

typedef HRESULT(STDMETHODCALLTYPE *CreateVS_t)(ID3D11Device *, const void *, SIZE_T,
                                                ID3D11ClassLinkage *, ID3D11VertexShader **);
typedef void(STDMETHODCALLTYPE *VSSetShader_t)(ID3D11DeviceContext *, ID3D11VertexShader *,
                                                ID3D11ClassInstance *const *, UINT);
typedef void(STDMETHODCALLTYPE *DrawIndexed_t)(ID3D11DeviceContext *, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE *Draw_t)(ID3D11DeviceContext *, UINT, UINT);
typedef void(STDMETHODCALLTYPE *DrawIndexedInst_t)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE *DrawInst_t)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);

static CreateVS_t g_createvs_orig = NULL;
static VSSetShader_t g_vsset_orig = NULL;
static DrawIndexed_t g_drawindexed_orig = NULL;
static Draw_t g_draw_orig = NULL;
static DrawIndexedInst_t g_drawindexedinst_orig = NULL;
static DrawInst_t g_drawinst_orig = NULL;

static int g_sd_installed = 0;
static int g_sd_active = 0;       /* full TEWVR_SHADERDUMP stats/blob-dump mode */
static int g_cvs_hook_active = 0; /* CreateVertexShader hook installed at all (Task 5:
                                      TEWVR_SHADERDUMP=1 OR TEWVR_SEQDUMP=1) - gates hash
                                      tracking + mvptable's mvp-offset reflection, a
                                      strict superset of g_sd_active */

/* Guards every table below. Creates happen on loading threads while draws
 * happen on the render thread, so unlike cbdump this lock genuinely earns
 * its keep. Temporary instrumentation; not a hot path we need lock-free. */
static CRITICAL_SECTION g_sd_cs;
static int g_sd_cs_ready = 0;

/* shader-object -> stats, open-addressed hash map keyed by pointer.
 * Power-of-two size; a AAA title ships a few thousand vertex shaders, so
 * 8192 slots keeps the load factor comfortable. */
#define SD_MAP_SIZE 8192
#define SD_SLOTS_SNAPPED 6
struct ShaderEntry {
    ID3D11VertexShader *vs; /* NULL == free slot */
    uint64_t hash;          /* FNV-1a 64 of the DXBC blob; 0 if unknown (created
                                before our hook, or hook missed it) */
    UINT64 draws;
    UINT64 indices;         /* total index/vertex count attributed */
    UINT cb_size[SD_SLOTS_SNAPPED]; /* ByteWidth per VS cb slot at first draw; 0 = none */
    int cb_snapped;
};
static struct ShaderEntry g_shaders[SD_MAP_SIZE];
static int g_shader_count = 0;
static int g_map_full_warned = 0;

/* Unique dumped blob hashes (dedupe). Creates are rare; linear scan fine. */
#define SD_DUMPED_MAX 8192
static uint64_t g_dumped[SD_DUMPED_MAX];
static int g_dumped_count = 0;

static ID3D11VertexShader *g_current_vs = NULL;

static wchar_t g_shader_dir[MAX_PATH]; /* %LOCALAPPDATA%\TEWVR\shaders */
static int g_shader_dir_ok = 0;

static ULONGLONG g_top_log_ms = 0;

/* ---- helpers ---- */

static uint64_t fnv1a64(const void *data, SIZE_T len) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = 1469598103934665603ULL;
    SIZE_T i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static struct ShaderEntry *find_or_add_locked(ID3D11VertexShader *vs) {
    /* Pointer-identity key; never dereferenced. Fibonacci-ish mix of the
     * pointer bits for the initial probe slot. */
    uint64_t k = (uint64_t)(uintptr_t)vs;
    UINT idx = (UINT)((k * 11400714819323198485ULL) >> 51) & (SD_MAP_SIZE - 1);
    UINT i;
    for (i = 0; i < SD_MAP_SIZE; i++) {
        struct ShaderEntry *e = &g_shaders[(idx + i) & (SD_MAP_SIZE - 1)];
        if (e->vs == vs) {
            return e;
        }
        if (e->vs == NULL) {
            if (g_shader_count >= SD_MAP_SIZE - (SD_MAP_SIZE / 8)) {
                break; /* keep load factor sane; treat as full */
            }
            e->vs = vs;
            g_shader_count++;
            return e;
        }
    }
    if (!g_map_full_warned) {
        g_map_full_warned = 1;
        log_msg("shaderdump: shader table full (%d); further shaders untracked", g_shader_count);
    }
    return NULL;
}

/* Read-only counterpart to find_or_add_locked(): same open-addressing probe
 * sequence, but never inserts. Correct as a "not present" test on a NULL
 * slot only because entries are never removed while the table is active
 * (matches find_or_add_locked's own assumption). Used by
 * shaderdump_hash_for_shader() (Task 5: seqdump.c's VSSETSHADER event and
 * mvptable.c's mvp_offset_for_shader() both need read-only ptr->hash
 * lookups from outside this file). */
static struct ShaderEntry *find_locked(ID3D11VertexShader *vs) {
    uint64_t k = (uint64_t)(uintptr_t)vs;
    UINT idx = (UINT)((k * 11400714819323198485ULL) >> 51) & (SD_MAP_SIZE - 1);
    UINT i;
    for (i = 0; i < SD_MAP_SIZE; i++) {
        struct ShaderEntry *e = &g_shaders[(idx + i) & (SD_MAP_SIZE - 1)];
        if (e->vs == vs) {
            return e;
        }
        if (e->vs == NULL) {
            return NULL;
        }
    }
    return NULL;
}

uint64_t shaderdump_hash_for_shader(const void *vs_ptr) {
    struct ShaderEntry *e;
    uint64_t h = 0;

    if (!g_sd_cs_ready || vs_ptr == NULL) {
        return 0;
    }

    EnterCriticalSection(&g_sd_cs);
    e = find_locked((ID3D11VertexShader *)vs_ptr);
    if (e != NULL) {
        h = e->hash;
    }
    LeaveCriticalSection(&g_sd_cs);

    return h;
}

static int hash_already_dumped_locked(uint64_t h) {
    int i;
    for (i = 0; i < g_dumped_count; i++) {
        if (g_dumped[i] == h) {
            return 1;
        }
    }
    if (g_dumped_count < SD_DUMPED_MAX) {
        g_dumped[g_dumped_count++] = h;
    }
    return 0;
}

/* Writes one DXBC blob to shaders\vs_<hash>.dxbc. Caller holds no lock
 * (file I/O off the lock; dedupe decision was made under it). */
static void dump_blob(uint64_t h, const void *data, SIZE_T len) {
    wchar_t path[MAX_PATH];
    FILE *fp;
    if (!g_shader_dir_ok) {
        return;
    }
    swprintf(path, MAX_PATH, L"%s\\vs_%016llX.dxbc", g_shader_dir, (unsigned long long)h);
    fp = _wfopen(path, L"wb");
    if (!fp) {
        return;
    }
    fwrite(data, 1, len, fp);
    fclose(fp);
}

/* Snapshot the ByteWidths bound to VS cb slots 0..5 right now. Calls the
 * context's own (unhooked) VSGetConstantBuffers; releases the refs it
 * hands back. Caller must NOT hold g_sd_cs (COM call). */
static void snapshot_slots(ID3D11DeviceContext *ctx, UINT out_sizes[SD_SLOTS_SNAPPED]) {
    ID3D11Buffer *bufs[SD_SLOTS_SNAPPED];
    UINT i;
    memset(bufs, 0, sizeof(bufs));
    ID3D11DeviceContext_VSGetConstantBuffers(ctx, 0, SD_SLOTS_SNAPPED, bufs);
    for (i = 0; i < SD_SLOTS_SNAPPED; i++) {
        out_sizes[i] = 0;
        if (bufs[i] != NULL) {
            D3D11_BUFFER_DESC d;
            ID3D11Buffer_GetDesc(bufs[i], &d);
            out_sizes[i] = d.ByteWidth;
            ID3D11Buffer_Release(bufs[i]);
        }
    }
}

static void log_top_shaders_locked(void) {
    ULONGLONG now = GetTickCount64();
    struct ShaderEntry top[10];
    int i, j;
    if (now - g_top_log_ms < 5000) {
        return;
    }
    g_top_log_ms = now;
    memset(top, 0, sizeof(top));
    for (i = 0; i < SD_MAP_SIZE; i++) {
        if (g_shaders[i].vs == NULL) {
            continue;
        }
        for (j = 0; j < 10; j++) {
            if (g_shaders[i].indices > top[j].indices) {
                int k;
                for (k = 9; k > j; k--) {
                    top[k] = top[k - 1];
                }
                top[j] = g_shaders[i];
                break;
            }
        }
    }
    log_msg("SHADERDUMP top vertex shaders by index volume (%d tracked):", g_shader_count);
    for (j = 0; j < 10 && top[j].indices > 0; j++) {
        log_msg("  #%d vs_%016llX draws=%llu indices=%llu slots=[%u,%u,%u,%u,%u,%u]",
                 j + 1, (unsigned long long)top[j].hash,
                 (unsigned long long)top[j].draws, (unsigned long long)top[j].indices,
                 top[j].cb_size[0], top[j].cb_size[1], top[j].cb_size[2],
                 top[j].cb_size[3], top[j].cb_size[4], top[j].cb_size[5]);
    }
}

/* Common tally for all four draw entry points. `count` is the index count
 * (indexed draws) or vertex count (non-indexed) of one instance times the
 * instance count - a good-enough "geometry volume" proxy either way. */
static void tally_draw(ID3D11DeviceContext *ctx, UINT64 count) {
    ID3D11VertexShader *vs;
    struct ShaderEntry *e;
    int need_snapshot = 0;

    EnterCriticalSection(&g_sd_cs);
    vs = g_current_vs;
    e = (vs != NULL) ? find_or_add_locked(vs) : NULL;
    if (e != NULL) {
        e->draws++;
        e->indices += count;
        if (!e->cb_snapped) {
            e->cb_snapped = 1;
            need_snapshot = 1;
        }
    }
    log_top_shaders_locked();
    LeaveCriticalSection(&g_sd_cs);

    if (need_snapshot) {
        UINT sizes[SD_SLOTS_SNAPPED];
        snapshot_slots(ctx, sizes);
        EnterCriticalSection(&g_sd_cs);
        /* e stays valid: entries are never removed while active. */
        memcpy(e->cb_size, sizes, sizeof(sizes));
        LeaveCriticalSection(&g_sd_cs);
    }
}

/* ---- detours ---- */

static HRESULT STDMETHODCALLTYPE Hook_CreateVS(ID3D11Device *dev, const void *bytecode,
                                                SIZE_T length, ID3D11ClassLinkage *linkage,
                                                ID3D11VertexShader **out_vs) {
    HRESULT hr = g_createvs_orig(dev, bytecode, length, linkage, out_vs);

    if (g_cvs_hook_active && SUCCEEDED(hr) && out_vs != NULL && *out_vs != NULL &&
        bytecode != NULL && length > 0 && length < (SIZE_T)16 * 1024 * 1024) {
        uint64_t h = fnv1a64(bytecode, length);
        struct ShaderEntry *e;
        int fresh;

        EnterCriticalSection(&g_sd_cs);
        e = find_or_add_locked(*out_vs);
        if (e != NULL) {
            e->hash = h;
        }
        /* Blob-to-disk dumping is still full-TEWVR_SHADERDUMP-only (short-
         * circuit means hash_already_dumped_locked()'s insert-on-miss side
         * effect only runs in that mode too, same as before Task 5). */
        fresh = g_sd_active && !hash_already_dumped_locked(h);
        LeaveCriticalSection(&g_sd_cs);

        if (fresh) {
            dump_blob(h, bytecode, length);
        }

        /* Task 5 feature 2: mvp-offset reflection via D3DReflect. Runs
         * whenever this hook is installed at all (TEWVR_SHADERDUMP=1 or
         * TEWVR_SEQDUMP=1), independent of full shaderdump stats/blob-dump
         * activation. mvptable_on_shader_created() dedupes by hash and is
         * fail-safe (D3DReflect unavailable, reflection failure, etc. all
         * log once and return). */
        mvptable_on_shader_created(h, bytecode, length);
    }
    return hr;
}

static void STDMETHODCALLTYPE Hook_VSSetShader(ID3D11DeviceContext *ctx, ID3D11VertexShader *vs,
                                                ID3D11ClassInstance *const *inst, UINT n) {
    /* Plain aligned pointer store; the draw hooks re-read it under the lock
     * but a torn value is impossible and staleness across the same render
     * thread cannot happen. */
    g_current_vs = vs;
    g_vsset_orig(ctx, vs, inst, n);
}

static void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext *ctx, UINT idxCount,
                                                UINT startIdx, INT baseVtx) {
    if (g_sd_active) {
        tally_draw(ctx, idxCount);
    }
    g_drawindexed_orig(ctx, idxCount, startIdx, baseVtx);
}

static void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext *ctx, UINT vtxCount, UINT startVtx) {
    if (g_sd_active) {
        tally_draw(ctx, vtxCount);
    }
    g_draw_orig(ctx, vtxCount, startVtx);
}

static void STDMETHODCALLTYPE Hook_DrawIndexedInst(ID3D11DeviceContext *ctx, UINT idxPerInst,
                                                    UINT instCount, UINT startIdx, INT baseVtx,
                                                    UINT startInst) {
    if (g_sd_active) {
        tally_draw(ctx, (UINT64)idxPerInst * (instCount ? instCount : 1));
    }
    g_drawindexedinst_orig(ctx, idxPerInst, instCount, startIdx, baseVtx, startInst);
}

static void STDMETHODCALLTYPE Hook_DrawInst(ID3D11DeviceContext *ctx, UINT vtxPerInst,
                                             UINT instCount, UINT startVtx, UINT startInst) {
    if (g_sd_active) {
        tally_draw(ctx, (UINT64)vtxPerInst * (instCount ? instCount : 1));
    }
    g_drawinst_orig(ctx, vtxPerInst, instCount, startVtx, startInst);
}

/* ---- setup / teardown ---- */

static int shaderdump_prepare_dir(void) {
    wchar_t la[MAX_PATH];
    wchar_t tewvr[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return 0;
    }
    swprintf(tewvr, MAX_PATH, L"%s\\TEWVR", la);
    if (!CreateDirectoryW(tewvr, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return 0;
    }
    swprintf(g_shader_dir, MAX_PATH, L"%s\\TEWVR\\shaders", la);
    if (!CreateDirectoryW(g_shader_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return 0;
    }
    return 1;
}

void shaderdump_install(ID3D11Device *dummy_dev, ID3D11DeviceContext *dummy_ctx) {
    char flag[8];
    DWORD len;
    void **dev_vtbl, **ctx_vtbl;
    int shaderdump_on, seqdump_on;
    int ok_create, ok_set, ok_di, ok_d, ok_dii, ok_dinst;

    if (g_sd_installed) {
        return;
    }
    g_sd_installed = 1;

    len = GetEnvironmentVariableA("TEWVR_SHADERDUMP", flag, sizeof(flag));
    shaderdump_on = (len > 0 && len < sizeof(flag) && strcmp(flag, "1") == 0);
    len = GetEnvironmentVariableA("TEWVR_SEQDUMP", flag, sizeof(flag));
    seqdump_on = (len > 0 && len < sizeof(flag) && strcmp(flag, "1") == 0);

    if (!shaderdump_on && !seqdump_on) {
        return; /* off by default: touch nothing */
    }

    if (dummy_dev == NULL || dummy_ctx == NULL) {
        log_msg("shaderdump: shader hooks requested but dummy device/context is NULL; skipping");
        return;
    }

    g_shader_dir_ok = shaderdump_prepare_dir();
    if (!g_shader_dir_ok) {
        log_msg("shaderdump: could not create shaders dir; blob dumping disabled (stats still on)");
    }

    if (!mh_glue_init()) {
        log_msg("shaderdump: MinHook init failed; skipping shader hooks");
        return;
    }

    InitializeCriticalSection(&g_sd_cs);
    g_sd_cs_ready = 1;

    dev_vtbl = *(void ***)dummy_dev;
    ctx_vtbl = *(void ***)dummy_ctx;

    /* CreateVertexShader: needed for shader-ptr->hash tracking (seqdump's
     * VSSETSHADER event) and the Task 5 mvp-offset reflection table
     * (mvptable.c) whenever EITHER mode is on - not just full
     * TEWVR_SHADERDUMP stats/blob-dump mode. */
    ok_create = mh_glue_create_and_enable(dev_vtbl[VTBL_DEV_CREATEVERTEXSHADER],
                                           (void *)&Hook_CreateVS, (void **)&g_createvs_orig,
                                           "ID3D11Device::CreateVertexShader");
    g_cvs_hook_active = ok_create;
    if (g_cvs_hook_active) {
        mvptable_init();
    }

    if (shaderdump_on) {
        ok_set = mh_glue_create_and_enable(ctx_vtbl[VTBL_CTX_VSSETSHADER],
                                            (void *)&Hook_VSSetShader, (void **)&g_vsset_orig,
                                            "ID3D11DeviceContext::VSSetShader");
        ok_di = mh_glue_create_and_enable(ctx_vtbl[VTBL_CTX_DRAWINDEXED],
                                           (void *)&Hook_DrawIndexed, (void **)&g_drawindexed_orig,
                                           "ID3D11DeviceContext::DrawIndexed");
        ok_d = mh_glue_create_and_enable(ctx_vtbl[VTBL_CTX_DRAW],
                                          (void *)&Hook_Draw, (void **)&g_draw_orig,
                                          "ID3D11DeviceContext::Draw");
        ok_dii = mh_glue_create_and_enable(ctx_vtbl[VTBL_CTX_DRAWINDEXEDINST],
                                            (void *)&Hook_DrawIndexedInst,
                                            (void **)&g_drawindexedinst_orig,
                                            "ID3D11DeviceContext::DrawIndexedInstanced");
        ok_dinst = mh_glue_create_and_enable(ctx_vtbl[VTBL_CTX_DRAWINST],
                                              (void *)&Hook_DrawInst, (void **)&g_drawinst_orig,
                                              "ID3D11DeviceContext::DrawInstanced");

        /* Stats need at least VSSetShader plus one draw hook. Partial
         * success still yields usable data, so activate on any draw path
         * being live. */
        g_sd_active = ok_set && (ok_di || ok_d || ok_dii || ok_dinst);
    } else {
        /* TEWVR_SEQDUMP-only run: seqdump.c hooks VSSetShader/Draw* itself
         * (its own independent detours, same shared-vtable trick) - no
         * need to install shaderdump's copies too. */
        ok_set = ok_di = ok_d = ok_dii = ok_dinst = 0;
        g_sd_active = 0;
    }

    log_msg("shaderdump: hooks create=%d set=%d di=%d d=%d dii=%d dinst=%d "
             "(shaderdump=%s seqdump=%s) -> stats %s",
             ok_create, ok_set, ok_di, ok_d, ok_dii, ok_dinst,
             shaderdump_on ? "on" : "off", seqdump_on ? "on" : "off",
             g_sd_active ? "ACTIVE" : "inactive");
}

void shaderdump_remove(void) {
    if (!g_sd_cs_ready) {
        return;
    }
    g_sd_active = 0;
    g_cvs_hook_active = 0;
    mvptable_shutdown();
    DeleteCriticalSection(&g_sd_cs);
    g_sd_cs_ready = 0;
}
