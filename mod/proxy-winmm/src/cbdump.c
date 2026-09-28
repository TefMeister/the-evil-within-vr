#include "cbdump.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

#include "MinHook.h"
#include "minhook_glue.h"
#include "log.h"
#include "config.h"

/* ---- ID3D11DeviceContext vtable indices (verified against d3d11.h: 3
 * IUnknown slots + 4 ID3D11DeviceChild slots = 7, then the interface's own
 * methods in declaration order). ---- */
#define VTBL_IDX_VSSETCONSTANTBUFFERS 7  /* not hooked - see header/report */
#define VTBL_IDX_MAP                  14
#define VTBL_IDX_UNMAP                15
#define VTBL_IDX_UPDATESUBRESOURCE    48

typedef HRESULT(STDMETHODCALLTYPE *Map_t)(ID3D11DeviceContext *, ID3D11Resource *, UINT,
                                           D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
typedef void(STDMETHODCALLTYPE *Unmap_t)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
typedef void(STDMETHODCALLTYPE *UpdateSubresource_t)(ID3D11DeviceContext *, ID3D11Resource *, UINT,
                                                       const D3D11_BOX *, const void *, UINT, UINT);

static Map_t g_map_orig = NULL;
static Unmap_t g_unmap_orig = NULL;
static UpdateSubresource_t g_update_orig = NULL;

/* VSSetConstantBuffers: to find the shared camera buffer deterministically by
 * counting which constant buffer is bound to the vertex shader for the most
 * draws (the per-frame camera/viewProj is bound to a fixed slot every draw). */
typedef void(STDMETHODCALLTYPE *VSSetCB_t)(ID3D11DeviceContext *, UINT, UINT,
                                            ID3D11Buffer *const *);
static VSSetCB_t g_vscb_orig = NULL;
#define BIND_TABLE 256
struct BindEntry { ID3D11Resource *res; UINT count; UINT size; UINT slot; };
static struct BindEntry g_binds[BIND_TABLE];
static ULONGLONG g_bind_log_ms = 0;
static int g_bind_logging = 0; /* enabled when TEWVR_FINDCAM=1 */

/* Guards g_map_table, the throttle table, g_cb_total_records, g_cb_capped,
 * and every write to g_cb_fp. Contention is expected to be near-zero (the
 * immediate context's Map/Unmap/UpdateSubresource calls all happen on the
 * game's single render thread by D3D11 convention), but a lock costs
 * nothing here and this is temporary instrumentation, not a hot path we
 * need to keep lock-free. */
static CRITICAL_SECTION g_cb_cs;
static int g_cb_cs_ready = 0;

static FILE *g_cb_fp = NULL;
static int g_cb_active = 0;   /* at least one hook installed + log open */
static int g_cb_installed = 0; /* cbdump_install() ran past the env-flag check */

static ULONGLONG g_cb_start_ms = 0;
static int g_cb_total_records = 0;
static int g_cb_capped = 0;
static int g_table_full_warned = 0;

/* Discovery probe (TEWVR_TARGETSIZE / TEWVR_TARGETOFF): if TARGETSIZE is set,
 * oscillate-scale the 16 floats at float-offset TARGETOFF of every constant
 * buffer of exactly that byte size, to find which buffer/offset the visible
 * geometry transforms through (the whole scene warps when it is the camera
 * view/viewProj). Temporary; not part of the mod proper. */
static int g_target_size = 0;
static int g_target_off = 0;

#define CBDUMP_MAX_RECORDS 40000
#define CBDUMP_MAX_MS      600000
#define CBDUMP_MAX_PER_SEC 20

/* Fixed table tracking currently-mapped candidate buffers, keyed by
 * resource pointer, so Unmap can find the data Map exposed (pData becomes
 * invalid the instant the real Unmap runs, so the dump must happen from
 * inside Hook_Unmap, using the pointer Hook_Map recorded, before calling
 * through to the original Unmap). */
#define MAP_TABLE_SIZE 16
struct MapEntry {
    ID3D11Resource *res; /* NULL == free slot */
    void *data;
    UINT byteWidth;
};
static struct MapEntry g_map_table[MAP_TABLE_SIZE];

/* Per-distinct-ByteWidth throttle state, "~4 records/sec per distinct
 * ByteWidth" per the brief. Small linear table; camera-cbuffer candidates
 * only span a handful of distinct sizes in practice. */
#define THROTTLE_TABLE_SIZE 16
struct ThrottleEntry {
    UINT byteWidth; /* 0 == free slot (0 is never a valid ByteWidth here) */
    ULONGLONG windowStartMs;
    int countInWindow;
};
static struct ThrottleEntry g_throttle[THROTTLE_TABLE_SIZE];

/* ---- helpers (all assume g_cb_cs is already held unless noted) ---- */

static int resource_is_camera_sized_buffer(ID3D11Resource *res, UINT *out_bw) {
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Buffer *buf = NULL;
    D3D11_BUFFER_DESC desc;
    HRESULT hr;

    if (res == NULL) {
        return 0;
    }

    ID3D11Resource_GetType(res, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) {
        return 0;
    }

    hr = ID3D11Resource_QueryInterface(res, &IID_ID3D11Buffer, (void **)&buf);
    if (FAILED(hr) || buf == NULL) {
        return 0;
    }

    ID3D11Buffer_GetDesc(buf, &desc);
    ID3D11Buffer_Release(buf); /* matches the QueryInterface AddRef; we only
                                   needed `res` as a pointer-identity key,
                                   never a held reference. */

    if (desc.ByteWidth < 64 || desc.ByteWidth > 512) {
        return 0;
    }

    if (out_bw) {
        *out_bw = desc.ByteWidth;
    }
    return 1;
}

/* Records (or overwrites, if already present) a mapped candidate. If the
 * table is full, drops it silently after a one-time warning to
 * tewvr.log - the corresponding Unmap will simply find no entry and do
 * nothing, which is safe (never crashes, just misses that one capture). */
static void track_mapped_resource_locked(ID3D11Resource *res, void *data, UINT byteWidth) {
    int i, free_slot = -1;

    for (i = 0; i < MAP_TABLE_SIZE; i++) {
        if (g_map_table[i].res == res) {
            g_map_table[i].data = data;
            g_map_table[i].byteWidth = byteWidth;
            return;
        }
        if (free_slot < 0 && g_map_table[i].res == NULL) {
            free_slot = i;
        }
    }

    if (free_slot < 0) {
        if (!g_table_full_warned) {
            g_table_full_warned = 1;
            log_msg("cbdump: map-tracking table full (%d entries); some Map/Unmap "
                     "captures will be dropped (best-effort discovery instrumentation)",
                     MAP_TABLE_SIZE);
        }
        return;
    }

    g_map_table[free_slot].res = res;
    g_map_table[free_slot].data = data;
    g_map_table[free_slot].byteWidth = byteWidth;
}

/* Returns the slot index for `res`, or -1 if not tracked. */
static int find_mapped_resource_locked(ID3D11Resource *res) {
    int i;
    for (i = 0; i < MAP_TABLE_SIZE; i++) {
        if (g_map_table[i].res == res) {
            return i;
        }
    }
    return -1;
}

static void clear_mapped_slot_locked(int slot) {
    g_map_table[slot].res = NULL;
    g_map_table[slot].data = NULL;
    g_map_table[slot].byteWidth = 0;
}

/* Returns 1 if this ByteWidth is allowed to log another record right now
 * (and consumes one slot in its per-second budget), 0 if throttled. */
static int throttle_allow_locked(UINT byteWidth) {
    int i, free_slot = -1;
    ULONGLONG now = GetTickCount64();

    for (i = 0; i < THROTTLE_TABLE_SIZE; i++) {
        if (g_throttle[i].byteWidth == byteWidth) {
            if (now - g_throttle[i].windowStartMs >= 1000) {
                g_throttle[i].windowStartMs = now;
                g_throttle[i].countInWindow = 0;
            }
            if (g_throttle[i].countInWindow >= CBDUMP_MAX_PER_SEC) {
                return 0;
            }
            g_throttle[i].countInWindow++;
            return 1;
        }
        if (free_slot < 0 && g_throttle[i].byteWidth == 0) {
            free_slot = i;
        }
    }

    /* First time we've seen this ByteWidth. If the table is full, just
     * allow it through unthrottled rather than dropping it silently -
     * distinct ByteWidths are few in practice, and never throttling a
     * size we have no budget-tracking slot for is the safer failure. */
    if (free_slot < 0) {
        return 1;
    }

    g_throttle[free_slot].byteWidth = byteWidth;
    g_throttle[free_slot].windowStartMs = now;
    g_throttle[free_slot].countInWindow = 1;
    return 1;
}

static void write_matrix_rows_locked(const void *data, UINT byteWidth) {
    UINT dumpBytes = (byteWidth < 512) ? byteWidth : 512;
    UINT nFloats = dumpBytes / 4;
    const float *f = (const float *)data;
    UINT row;

    for (row = 0; row * 4 < nFloats; row++) {
        UINT col;
        UINT colsInRow = nFloats - row * 4;
        if (colsInRow > 4) {
            colsInRow = 4;
        }
        fprintf(g_cb_fp, "  r%u:", row);
        for (col = 0; col < colsInRow; col++) {
            fprintf(g_cb_fp, " %9.4f", (double)f[row * 4 + col]);
        }
        fprintf(g_cb_fp, "\n");
    }
}

/* Central sink for both capture paths (Map/Unmap and UpdateSubresource).
 * Applies the global record/time cap and the per-ByteWidth throttle, then
 * writes one human-readable record. Safe to call often; cheap no-op once
 * capped. */
static void cbdump_record(const char *api, UINT byteWidth, const void *resPtr, const void *data) {
    if (!g_cb_active || data == NULL) {
        return;
    }

    EnterCriticalSection(&g_cb_cs);

    if (g_cb_capped) {
        LeaveCriticalSection(&g_cb_cs);
        return;
    }

    if (g_cb_total_records >= CBDUMP_MAX_RECORDS ||
        (GetTickCount64() - g_cb_start_ms) >= CBDUMP_MAX_MS) {
        g_cb_capped = 1;
        fprintf(g_cb_fp, "dump capped (records=%d elapsed_ms=%llu)\n",
                g_cb_total_records, (unsigned long long)(GetTickCount64() - g_cb_start_ms));
        fflush(g_cb_fp);
        LeaveCriticalSection(&g_cb_cs);
        return;
    }

    if (!throttle_allow_locked(byteWidth)) {
        LeaveCriticalSection(&g_cb_cs);
        return;
    }

    fprintf(g_cb_fp, "[%s] res=0x%llX size=%u\n", api,
            (unsigned long long)(uintptr_t)resPtr, (unsigned)byteWidth);
    write_matrix_rows_locked(data, byteWidth);
    fflush(g_cb_fp);

    g_cb_total_records++;

    LeaveCriticalSection(&g_cb_cs);
}

/* ---- detours ---- */

static void count_bind(ID3D11Buffer *buf, UINT slot) {
    ID3D11Resource *res = (ID3D11Resource *)buf;
    int i, free_slot = -1;
    D3D11_BUFFER_DESC d;
    if (buf == NULL) {
        return;
    }
    for (i = 0; i < BIND_TABLE; i++) {
        if (g_binds[i].res == res) {
            g_binds[i].count++;
            return;
        }
        if (free_slot < 0 && g_binds[i].res == NULL) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return;
    }
    ID3D11Buffer_GetDesc(buf, &d);
    g_binds[free_slot].res = res;
    g_binds[free_slot].count = 1;
    g_binds[free_slot].size = d.ByteWidth;
    g_binds[free_slot].slot = slot;
}

static void log_top_binds(void) {
    ULONGLONG now = GetTickCount64();
    int i, j;
    struct BindEntry top[8];
    if (now - g_bind_log_ms < 2000) {
        return;
    }
    g_bind_log_ms = now;
    memset(top, 0, sizeof(top));
    for (i = 0; i < BIND_TABLE; i++) {
        if (g_binds[i].res == NULL) {
            continue;
        }
        for (j = 0; j < 8; j++) {
            if (g_binds[i].count > top[j].count) {
                int k;
                for (k = 7; k > j; k--) {
                    top[k] = top[k - 1];
                }
                top[j] = g_binds[i];
                break;
            }
        }
    }
    log_msg("FINDCAM top VS constant buffers by bind-count:");
    for (j = 0; j < 8 && top[j].count > 0; j++) {
        log_msg("  #%d res=0x%llX size=%u slot=%u binds=%u", j + 1,
                 (unsigned long long)(uintptr_t)top[j].res, top[j].size, top[j].slot,
                 top[j].count);
    }
}

static void STDMETHODCALLTYPE Hook_VSSetCB(ID3D11DeviceContext *ctx, UINT StartSlot,
                                            UINT NumBuffers, ID3D11Buffer *const *ppCB) {
    if (g_bind_logging && ppCB != NULL) {
        UINT i;
        for (i = 0; i < NumBuffers && (StartSlot + i) < 6; i++) {
            count_bind(ppCB[i], StartSlot + i);
        }
        log_top_binds();
    }
    g_vscb_orig(ctx, StartSlot, NumBuffers, ppCB);
}

static HRESULT STDMETHODCALLTYPE Hook_Map(ID3D11DeviceContext *ctx, ID3D11Resource *pResource,
                                           UINT Subresource, D3D11_MAP MapType, UINT MapFlags,
                                           D3D11_MAPPED_SUBRESOURCE *pMappedResource) {
    HRESULT hr = g_map_orig(ctx, pResource, Subresource, MapType, MapFlags, pMappedResource);

    if (SUCCEEDED(hr) && pMappedResource != NULL && pMappedResource->pData != NULL) {
        UINT bw;
        if (resource_is_camera_sized_buffer(pResource, &bw)) {
            EnterCriticalSection(&g_cb_cs);
            track_mapped_resource_locked(pResource, pMappedResource->pData, bw);
            LeaveCriticalSection(&g_cb_cs);
        }
    }

    return hr;
}

/* Live probe control: re-read %LOCALAPPDATA%\TEWVR\probe.txt ("<size> <off>")
 * about twice a second so the probe target can be swept without relaunching
 * (get into a level once, then change targets by rewriting the file). */
static ULONGLONG g_probe_check_ms = 0;
static void maybe_reload_probe(void) {
    ULONGLONG now = GetTickCount64();
    wchar_t la[MAX_PATH], path[MAX_PATH];
    FILE *fp;
    int sz = 0, off = 0;
    if (now - g_probe_check_ms < 500) {
        return;
    }
    g_probe_check_ms = now;
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) {
        return;
    }
    swprintf(path, MAX_PATH, L"%s\\TEWVR\\probe.txt", la);
    fp = _wfopen(path, L"r");
    if (!fp) {
        return;
    }
    if (fscanf(fp, "%d %d", &sz, &off) >= 1) {
        g_target_size = sz;
        g_target_off = off;
    }
    fclose(fp);
}

static void STDMETHODCALLTYPE Hook_Unmap(ID3D11DeviceContext *ctx, ID3D11Resource *pResource,
                                          UINT Subresource) {
    int slot;
    void *data = NULL;
    UINT byteWidth = 0;
    int found = 0;

    maybe_reload_probe();

    EnterCriticalSection(&g_cb_cs);
    slot = find_mapped_resource_locked(pResource);
    if (slot >= 0) {
        data = g_map_table[slot].data;
        byteWidth = g_map_table[slot].byteWidth;
        found = 1;
        clear_mapped_slot_locked(slot);
    }
    LeaveCriticalSection(&g_cb_cs);

    /* Dump BEFORE calling through: the mapped pointer is only guaranteed
     * valid until the real Unmap runs. */
    if (found) {
        cbdump_record("Map/Unmap", byteWidth, pResource, data);

        /* Discovery probe: oscillate-scale the 16 floats at TARGETOFF of every
         * buffer of exactly TARGETSIZE bytes, before it commits. When that
         * matrix is the camera view/viewProj, the WHOLE scene pulses/warps. */
        if (g_target_size != 0 && (int)byteWidth == g_target_size && data != NULL) {
            float *f = (float *)data;
            int nf = (int)(byteWidth / 4);
            float t = (float)(GetTickCount64() % 2000u) / 2000.0f * 6.2831853f;
            int i;
            /* NON-uniform perturbation: a per-element phase-shifted, magnitude-
             * relative wobble. Unlike a uniform scale (which cancels in the
             * perspective divide for a view-projection matrix), this visibly
             * warps the scene if the buffer is any kind of transform. */
            for (i = g_target_off; i < nf; i++) {
                f[i] += (fabsf(f[i]) + 0.5f) * 0.6f * sinf(t + (float)i * 0.9f);
            }
        }
    }

    g_unmap_orig(ctx, pResource, Subresource);
}

static void STDMETHODCALLTYPE Hook_UpdateSubresource(ID3D11DeviceContext *ctx,
                                                       ID3D11Resource *pDstResource,
                                                       UINT DstSubresource, const D3D11_BOX *pDstBox,
                                                       const void *pSrcData, UINT SrcRowPitch,
                                                       UINT SrcDepthPitch) {
    UINT bw;
    if (pSrcData != NULL && resource_is_camera_sized_buffer(pDstResource, &bw)) {
        cbdump_record("UpdateSubresource", bw, pDstResource, pSrcData);
    }

    g_update_orig(ctx, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
}

/* ---- setup / teardown ---- */

static int cbdump_open_logfile(void) {
    wchar_t local_appdata[MAX_PATH];
    wchar_t dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    DWORD len;

    len = GetEnvironmentVariableW(L"LOCALAPPDATA", local_appdata, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return 0;
    }

    swprintf(dir, MAX_PATH, L"%s\\TEWVR", local_appdata);
    if (!CreateDirectoryW(dir, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            return 0;
        }
    }

    swprintf(path, MAX_PATH, L"%s\\cbdump.log", dir);

    /* "w" (truncate): this is a per-session discovery dump, not an
     * append-forever log like tewvr.log - each capture run should show
     * only that run's records. */
    g_cb_fp = _wfopen(path, L"w");
    return g_cb_fp != NULL;
}

void cbdump_install(ID3D11DeviceContext *dummy_ctx) {
    char flag[8];
    DWORD len;
    void **vtbl;
    int ok_map, ok_unmap, ok_update;

    if (g_cb_installed) {
        return; /* hooks.c only calls this once, but stay idempotent */
    }
    g_cb_installed = 1;

    len = tewvr_getenv("TEWVR_DUMP", flag, sizeof(flag));
    if (len == 0 || len >= sizeof(flag) || strcmp(flag, "1") != 0) {
        return; /* off by default: touch nothing */
    }

    {
        char ts[32];
        DWORD tl = tewvr_getenv("TEWVR_TARGETSIZE", ts, sizeof(ts));
        if (tl > 0 && tl < sizeof(ts)) {
            g_target_size = atoi(ts);
            tl = tewvr_getenv("TEWVR_TARGETOFF", ts, sizeof(ts));
            if (tl > 0 && tl < sizeof(ts)) {
                g_target_off = atoi(ts);
            }
            log_msg("cbdump: probe target size=%d offset(float)=%d", g_target_size,
                     g_target_off);
        }
    }
    {
        char fc[8];
        DWORD fl = tewvr_getenv("TEWVR_FINDCAM", fc, sizeof(fc));
        if (fl > 0 && fl < sizeof(fc) && strcmp(fc, "1") == 0) {
            g_bind_logging = 1;
            log_msg("cbdump: FINDCAM on -> counting VS constant-buffer binds");
        }
    }

    if (dummy_ctx == NULL) {
        log_msg("cbdump: TEWVR_DUMP=1 but dummy context is NULL; skipping constant-buffer dump hooks");
        return;
    }

    if (!cbdump_open_logfile()) {
        log_msg("cbdump: failed to open cbdump.log; skipping constant-buffer dump hooks");
        return;
    }

    InitializeCriticalSection(&g_cb_cs);
    g_cb_cs_ready = 1;
    g_cb_start_ms = GetTickCount64();

    if (!mh_glue_init()) {
        log_msg("cbdump: MinHook init failed; skipping constant-buffer dump hooks");
        return;
    }

    vtbl = *(void ***)dummy_ctx;

    ok_map = mh_glue_create_and_enable(vtbl[VTBL_IDX_MAP], (void *)&Hook_Map,
                                        (void **)&g_map_orig, "ID3D11DeviceContext::Map");
    ok_unmap = mh_glue_create_and_enable(vtbl[VTBL_IDX_UNMAP], (void *)&Hook_Unmap,
                                          (void **)&g_unmap_orig, "ID3D11DeviceContext::Unmap");
    ok_update = mh_glue_create_and_enable(vtbl[VTBL_IDX_UPDATESUBRESOURCE], (void *)&Hook_UpdateSubresource,
                                           (void **)&g_update_orig, "ID3D11DeviceContext::UpdateSubresource");
    if (g_bind_logging) {
        mh_glue_create_and_enable(vtbl[VTBL_IDX_VSSETCONSTANTBUFFERS], (void *)&Hook_VSSetCB,
                                   (void **)&g_vscb_orig,
                                   "ID3D11DeviceContext::VSSetConstantBuffers");
    }

    if (!ok_map || !ok_unmap) {
        log_msg("cbdump: Map/Unmap hook pair incomplete (map=%d unmap=%d); "
                 "Map/Unmap capture path degraded/disabled", ok_map, ok_unmap);
    }
    if (!ok_update) {
        log_msg("cbdump: UpdateSubresource hook failed; UpdateSubresource capture path disabled");
    }

    /* VSSetConstantBuffers (vtable index 7) is intentionally NOT hooked -
     * the brief marks it optional/low-priority ("shows slot bindings but
     * not data") and skipping it keeps this temporary module smaller and
     * lower-risk, per the brief's explicit "may skip" allowance. */

    g_cb_active = ok_map || ok_unmap || ok_update;

    if (g_cb_active) {
        EnterCriticalSection(&g_cb_cs);
        fprintf(g_cb_fp, "TEWVR cbdump session start (TEWVR_DUMP=1); hooks: Map=%d Unmap=%d UpdateSubresource=%d\n",
                ok_map, ok_unmap, ok_update);
        fflush(g_cb_fp);
        LeaveCriticalSection(&g_cb_cs);
        log_msg("cbdump: constant-buffer dump hooks active (writing to cbdump.log)");
    } else {
        log_msg("cbdump: no constant-buffer dump hooks could be installed; cbdump.log will stay empty");
    }
}

void cbdump_remove(void) {
    if (!g_cb_cs_ready) {
        return; /* cbdump_install() was never called, or TEWVR_DUMP was unset */
    }

    g_cb_active = 0;

    if (g_cb_fp) {
        fclose(g_cb_fp);
        g_cb_fp = NULL;
    }

    DeleteCriticalSection(&g_cb_cs);
    g_cb_cs_ready = 0;
}
