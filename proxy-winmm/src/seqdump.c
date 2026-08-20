#include "seqdump.h"

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#define COBJMACROS
#include <d3d11_1.h>

#include "MinHook.h"
#include "minhook_glue.h"
#include "log.h"
#include "d3d_capture.h"
#include "shaderdump.h"

/* ---- vtable indices (same verified counting scheme as cbdump.c/shaderdump.c) ---- */
#define VTBL_CTX_VSSETCONSTANTBUFFERS 7
#define VTBL_CTX_VSSETSHADER          11
#define VTBL_CTX_DRAWINDEXED          12
#define VTBL_CTX_DRAW                 13
#define VTBL_CTX_MAP                  14
#define VTBL_CTX_UNMAP                15
#define VTBL_CTX_DRAWINDEXEDINST      20
#define VTBL_CTX_DRAWINST             21
#define VTBL_CTX_UPDATESUBRESOURCE    48
/* ExecuteCommandList / FinishCommandList (Task 5 review addendum 2):
 * counted directly (0-based) from the llvm-mingw toolchain's own d3d11.h
 * ID3D11DeviceContextVtbl struct, method-by-method from QueryInterface=0 -
 * not recomputed by formula, to avoid a miscount. ExecuteCommandList lands
 * at index 58; FinishCommandList is the LAST method in the whole vtable,
 * at index 114. Cross-check: this same counting pass also lands
 * VSSetConstantBuffers at 7 and UpdateSubresource at 48, exactly matching
 * cbdump.c's/shaderdump.c's already-verified indices for those two -
 * confirming the count is right before trusting the two new ones. */
#define VTBL_CTX_EXECUTECOMMANDLIST   58
#define VTBL_CTX_FINISHCOMMANDLIST    114
/* ID3D11DeviceContext1 vtable slot for VSSetConstantBuffers1 - given
 * verbatim by the task-5 brief, not recomputed here. */
#define VTBL_CTX1_VSSETCONSTANTBUFFERS1 120

#define SEQDUMP_MAX_EVENTS  40000
#define SEQDUMP_ARM_FRAMES  300
#define SEQDUMP_ARMFILE_CHECK_FRAMES 30
#define SEQ_CB_MAX_BYTEWIDTH 8192

typedef HRESULT(STDMETHODCALLTYPE *Map_t)(ID3D11DeviceContext *, ID3D11Resource *, UINT,
                                           D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
typedef void(STDMETHODCALLTYPE *Unmap_t)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
typedef void(STDMETHODCALLTYPE *UpdateSubresource_t)(ID3D11DeviceContext *, ID3D11Resource *, UINT,
                                                       const D3D11_BOX *, const void *, UINT, UINT);
typedef void(STDMETHODCALLTYPE *VSSetCB_t)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
typedef void(STDMETHODCALLTYPE *VSSetShader_t)(ID3D11DeviceContext *, ID3D11VertexShader *,
                                                ID3D11ClassInstance *const *, UINT);
typedef void(STDMETHODCALLTYPE *DrawIndexed_t)(ID3D11DeviceContext *, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE *Draw_t)(ID3D11DeviceContext *, UINT, UINT);
typedef void(STDMETHODCALLTYPE *DrawIndexedInst_t)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE *DrawInst_t)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE *VSSetCB1_t)(ID3D11DeviceContext1 *, UINT, UINT, ID3D11Buffer *const *,
                                             const UINT *, const UINT *);
typedef HRESULT(STDMETHODCALLTYPE *FinishCommandList_t)(ID3D11DeviceContext *, WINBOOL, ID3D11CommandList **);
typedef void(STDMETHODCALLTYPE *ExecuteCommandList_t)(ID3D11DeviceContext *, ID3D11CommandList *, WINBOOL);

static Map_t g_map_orig = NULL;
static Unmap_t g_unmap_orig = NULL;
static UpdateSubresource_t g_update_orig = NULL;
static VSSetCB_t g_vsset_cb_orig = NULL;
static VSSetShader_t g_vsset_orig = NULL;
static DrawIndexed_t g_drawindexed_orig = NULL;
static Draw_t g_draw_orig = NULL;
static DrawIndexedInst_t g_drawindexedinst_orig = NULL;
static DrawInst_t g_drawinst_orig = NULL;
static VSSetCB1_t g_vsset_cb1_orig = NULL;
static FinishCommandList_t g_finishcmdlist_orig = NULL;
static ExecuteCommandList_t g_executecmdlist_orig = NULL;

static int g_seq_installed = 0;
static int g_seq_hooks_ok = 0; /* at least one of the hooks above installed + log open */

/* Guards g_seq_fp, the sequence counter, the event/complete counters, and
 * the Map->Unmap latch table. Contention is expected to be near-zero (the
 * immediate context's calls all happen on the game's single render thread
 * by D3D11 convention, same assumption cbdump.c documents), but this is
 * diagnostic instrumentation capped at 40,000 events - a lock costs
 * nothing here. */
static CRITICAL_SECTION g_seq_cs;
static int g_seq_cs_ready = 0;

static FILE *g_seq_fp = NULL;
static UINT64 g_seq_seqno = 0;
static UINT64 g_seq_event_count = 0;
static int g_seq_complete = 0;

static int g_seq_first_frame_set = 0;
static UINT64 g_seq_first_frame = 0;
static int g_seq_armed = 0;
static int g_seq_armfile_mode = 0; /* TEWVR_SEQDUMP_ARMFILE=1: arm via seqarm.txt instead of frame-301 */

/* Map->Unmap float shadow: "shadow only the most recent MAP per thread - a
 * single-slot latch is fine" (brief). Small fixed table keyed by thread id;
 * each thread gets exactly one slot, overwritten by its next MAP. */
#define SEQ_LATCH_SIZE 8
struct MapLatch {
    DWORD tid;   /* 0 == free slot (0 is never a real thread id) */
    ID3D11Resource *res;
    void *data;
    UINT byteWidth;
};
static struct MapLatch g_latch[SEQ_LATCH_SIZE];

/* ---- helpers ---- */

static int seq_active(void) {
    return g_seq_hooks_ok && g_seq_armed && !g_seq_complete;
}

/* Fills `out` with %LOCALAPPDATA%\TEWVR\seqarm.txt, or an empty string on
 * any failure (LOCALAPPDATA unset/too long). Shared by the ARMFILE arm
 * check and seqdump_clear_stale_armfile(). */
static void seqarm_path(wchar_t *out, size_t out_count) {
    wchar_t la[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        out[0] = L'\0';
        return;
    }
    swprintf(out, out_count, L"%s\\TEWVR\\seqarm.txt", la);
}

/* Only ID3D11Buffer resources with BindFlags & D3D11_BIND_CONSTANT_BUFFER
 * and ByteWidth <= 8192, per the brief - deliberately not cbdump.c's
 * size-range filter (that one doesn't check BindFlags at all). */
static int resource_is_cb_le8192(ID3D11Resource *res, UINT *out_bw) {
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
    ID3D11Buffer_Release(buf);

    if (!(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) {
        return 0;
    }
    if (desc.ByteWidth > SEQ_CB_MAX_BYTEWIDTH) {
        return 0;
    }
    if (out_bw) {
        *out_bw = desc.ByteWidth;
    }
    return 1;
}

/* ---- Map->Unmap latch (caller holds g_seq_cs) ---- */

static void latch_store_locked(DWORD tid, ID3D11Resource *res, void *data, UINT bw) {
    int i, free_slot = -1;
    for (i = 0; i < SEQ_LATCH_SIZE; i++) {
        if (g_latch[i].tid == tid) {
            g_latch[i].res = res;
            g_latch[i].data = data;
            g_latch[i].byteWidth = bw;
            return;
        }
        if (free_slot < 0 && g_latch[i].tid == 0) {
            free_slot = i;
        }
    }
    /* More than SEQ_LATCH_SIZE distinct threads mapping CBs concurrently is
     * not expected on a single-render-thread D3D11 immediate context;
     * overwrite slot 0 rather than drop silently if it ever happens - still
     * "simple" per the brief, and this is discovery instrumentation, not
     * the mod proper. */
    if (free_slot < 0) {
        free_slot = 0;
    }
    g_latch[free_slot].tid = tid;
    g_latch[free_slot].res = res;
    g_latch[free_slot].data = data;
    g_latch[free_slot].byteWidth = bw;
}

/* Single-shot: on a match, clears the slot and returns 1. */
static int latch_take_locked(DWORD tid, ID3D11Resource *res, void **out_data, UINT *out_bw) {
    int i;
    for (i = 0; i < SEQ_LATCH_SIZE; i++) {
        if (g_latch[i].tid == tid) {
            if (g_latch[i].res == res) {
                *out_data = g_latch[i].data;
                *out_bw = g_latch[i].byteWidth;
                g_latch[i].tid = 0;
                g_latch[i].res = NULL;
                return 1;
            }
            return 0; /* this thread's latch holds a different resource: miss */
        }
    }
    return 0;
}

/* ---- event log writer ----
 * Two-phase API so multi-field events (VSSETCB, VSSETCB1) can build their
 * line with several fprintf calls under one lock, while simple one-line
 * events go through seq_line()'s single vfprintf. Both funnel through the
 * same sequence numbering / 40,000-event cap / COMPLETE-line logic.
 *
 * Task 5 review addendum 2: every event line now also carries the D3D11
 * device-context pointer the event fired on (right after tid, before the
 * event body), so a later analysis pass can tell immediate-context state
 * calls apart from deferred-context command-list recording on other
 * threads. `ctx_ptr` is a plain `const void *` (never dereferenced here -
 * only its address is printed) so callers can pass an
 * ID3D11DeviceContext*, an ID3D11DeviceContext1*, or NULL (PRESENT has no
 * natural ctx argument; see seqdump_on_present()) without casting. */

static int seq_begin_locked(const void *ctx_ptr) {
    if (!seq_active() || g_seq_fp == NULL) {
        return 0;
    }
    EnterCriticalSection(&g_seq_cs);
    if (g_seq_complete) {
        LeaveCriticalSection(&g_seq_cs);
        return 0;
    }
    g_seq_seqno++;
    fprintf(g_seq_fp, "[%llu][tid=%lu][ctx=0x%llX] ", (unsigned long long)g_seq_seqno,
             (unsigned long)GetCurrentThreadId(), (unsigned long long)(uintptr_t)ctx_ptr);
    return 1; /* still holding g_seq_cs */
}

/* Caller still holds g_seq_cs (from seq_begin_locked()); this releases it. */
static void seq_finish_locked(int flush_now) {
    fprintf(g_seq_fp, "\n");
    g_seq_event_count++;
    if (flush_now) {
        fflush(g_seq_fp);
    }
    if (g_seq_event_count >= SEQDUMP_MAX_EVENTS) {
        fprintf(g_seq_fp, "SEQDUMP COMPLETE (events=%llu)\n", (unsigned long long)g_seq_event_count);
        fflush(g_seq_fp);
        g_seq_complete = 1;
    }
    LeaveCriticalSection(&g_seq_cs);
}

static void seq_line(const void *ctx_ptr, int flush_now, const char *fmt, ...) {
    va_list ap;
    if (!seq_begin_locked(ctx_ptr)) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(g_seq_fp, fmt, ap);
    va_end(ap);
    seq_finish_locked(flush_now);
}

/* ---- detours ---- */

static HRESULT STDMETHODCALLTYPE Hook_Map(ID3D11DeviceContext *ctx, ID3D11Resource *res, UINT sub,
                                           D3D11_MAP mapType, UINT flags,
                                           D3D11_MAPPED_SUBRESOURCE *mapped) {
    HRESULT hr = g_map_orig(ctx, res, sub, mapType, flags, mapped);

    if (seq_active() && SUCCEEDED(hr) && mapped != NULL && mapped->pData != NULL) {
        UINT bw;
        if (resource_is_cb_le8192(res, &bw)) {
            DWORD tid = GetCurrentThreadId();
            EnterCriticalSection(&g_seq_cs);
            latch_store_locked(tid, res, mapped->pData, bw);
            LeaveCriticalSection(&g_seq_cs);
            seq_line(ctx, 0, "MAP res=0x%llX bytewidth=%u maptype=%d",
                      (unsigned long long)(uintptr_t)res, bw, (int)mapType);
        }
    }

    return hr;
}

static void STDMETHODCALLTYPE Hook_Unmap(ID3D11DeviceContext *ctx, ID3D11Resource *res, UINT sub) {
    if (seq_active()) {
        UINT bw;
        if (resource_is_cb_le8192(res, &bw)) {
            DWORD tid = GetCurrentThreadId();
            void *data = NULL;
            UINT latched_bw = 0;
            int got;

            /* Read the shadowed data BEFORE calling through: the mapped
             * pointer is only guaranteed valid until the real Unmap runs
             * (same ordering constraint cbdump.c documents). */
            EnterCriticalSection(&g_seq_cs);
            got = latch_take_locked(tid, res, &data, &latched_bw);
            LeaveCriticalSection(&g_seq_cs);

            if (got && data != NULL && latched_bw >= 16) {
                const float *f = (const float *)data;
                seq_line(ctx, 0, "UNMAP res=0x%llX bytewidth=%u floats=[%.4f %.4f %.4f %.4f]",
                          (unsigned long long)(uintptr_t)res, bw,
                          (double)f[0], (double)f[1], (double)f[2], (double)f[3]);
            } else {
                /* Latch miss (brief: "if the latch misses, log UNMAP
                 * without floats"), or a mapped CB too small to hold 4
                 * floats. */
                seq_line(ctx, 0, "UNMAP res=0x%llX bytewidth=%u floats=<none>",
                          (unsigned long long)(uintptr_t)res, bw);
            }
        }
    }

    g_unmap_orig(ctx, res, sub);
}

static void STDMETHODCALLTYPE Hook_UpdateSubresource(ID3D11DeviceContext *ctx, ID3D11Resource *dst,
                                                       UINT dstSub, const D3D11_BOX *box,
                                                       const void *src, UINT rowPitch, UINT depthPitch) {
    if (seq_active() && src != NULL) {
        UINT bw;
        if (resource_is_cb_le8192(dst, &bw)) {
            if (bw >= 16) {
                const float *f = (const float *)src;
                seq_line(ctx, 0, "UPDATESUB res=0x%llX bytewidth=%u floats=[%.4f %.4f %.4f %.4f]",
                          (unsigned long long)(uintptr_t)dst, bw,
                          (double)f[0], (double)f[1], (double)f[2], (double)f[3]);
            } else {
                seq_line(ctx, 0, "UPDATESUB res=0x%llX bytewidth=%u floats=<none>",
                          (unsigned long long)(uintptr_t)dst, bw);
            }
        }
    }

    g_update_orig(ctx, dst, dstSub, box, src, rowPitch, depthPitch);
}

static void STDMETHODCALLTYPE Hook_VSSetCB(ID3D11DeviceContext *ctx, UINT startSlot, UINT numBuffers,
                                            ID3D11Buffer *const *ppCB) {
    if (ppCB != NULL && seq_begin_locked(ctx)) {
        UINT n = (numBuffers > 4) ? 4 : numBuffers;
        UINT i;
        fprintf(g_seq_fp, "VSSETCB start=%u num=%u", startSlot, numBuffers);
        for (i = 0; i < n; i++) {
            ID3D11Buffer *b = ppCB[i];
            UINT bw = 0;
            if (b != NULL) {
                D3D11_BUFFER_DESC d;
                ID3D11Buffer_GetDesc(b, &d);
                bw = d.ByteWidth;
            }
            fprintf(g_seq_fp, " slot%u=0x%llX:%u", startSlot + i, (unsigned long long)(uintptr_t)b, bw);
        }
        seq_finish_locked(0);
    }

    g_vsset_cb_orig(ctx, startSlot, numBuffers, ppCB);
}

static void STDMETHODCALLTYPE Hook_VSSetShader(ID3D11DeviceContext *ctx, ID3D11VertexShader *vs,
                                                ID3D11ClassInstance *const *inst, UINT n) {
    if (seq_active()) {
        uint64_t hash = shaderdump_hash_for_shader(vs);
        if (hash != 0) {
            seq_line(ctx, 0, "VSSETSHADER ptr=0x%llX hash=%016llX",
                      (unsigned long long)(uintptr_t)vs, (unsigned long long)hash);
        } else {
            seq_line(ctx, 0, "VSSETSHADER ptr=0x%llX hash=?", (unsigned long long)(uintptr_t)vs);
        }
    }

    g_vsset_orig(ctx, vs, inst, n);
}

static void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext *ctx, UINT idxCount, UINT startIdx,
                                                INT baseVtx) {
    if (seq_active()) {
        seq_line(ctx, 0, "DRAW variant=DrawIndexed count=%u", idxCount);
    }
    g_drawindexed_orig(ctx, idxCount, startIdx, baseVtx);
}

static void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext *ctx, UINT vtxCount, UINT startVtx) {
    if (seq_active()) {
        seq_line(ctx, 0, "DRAW variant=Draw count=%u", vtxCount);
    }
    g_draw_orig(ctx, vtxCount, startVtx);
}

static void STDMETHODCALLTYPE Hook_DrawIndexedInst(ID3D11DeviceContext *ctx, UINT idxPerInst,
                                                    UINT instCount, UINT startIdx, INT baseVtx,
                                                    UINT startInst) {
    if (seq_active()) {
        seq_line(ctx, 0, "DRAW variant=DrawIndexedInstanced count=%u instances=%u", idxPerInst, instCount);
    }
    g_drawindexedinst_orig(ctx, idxPerInst, instCount, startIdx, baseVtx, startInst);
}

static void STDMETHODCALLTYPE Hook_DrawInst(ID3D11DeviceContext *ctx, UINT vtxPerInst, UINT instCount,
                                             UINT startVtx, UINT startInst) {
    if (seq_active()) {
        seq_line(ctx, 0, "DRAW variant=DrawInstanced count=%u instances=%u", vtxPerInst, instCount);
    }
    g_drawinst_orig(ctx, vtxPerInst, instCount, startVtx, startInst);
}

static void STDMETHODCALLTYPE Hook_VSSetCB1(ID3D11DeviceContext1 *ctx, UINT startSlot, UINT numBuffers,
                                             ID3D11Buffer *const *ppCB, const UINT *pFirstConstant,
                                             const UINT *pNumConstants) {
    if (ppCB != NULL && seq_begin_locked(ctx)) {
        UINT n = (numBuffers > 4) ? 4 : numBuffers;
        UINT i;
        fprintf(g_seq_fp, "VSSETCB1 start=%u num=%u", startSlot, numBuffers);
        for (i = 0; i < n; i++) {
            ID3D11Buffer *b = ppCB[i];
            UINT bw = 0;
            UINT firstConst = pFirstConstant ? pFirstConstant[i] : 0;
            UINT numConst = pNumConstants ? pNumConstants[i] : 0;
            if (b != NULL) {
                D3D11_BUFFER_DESC d;
                ID3D11Buffer_GetDesc(b, &d);
                bw = d.ByteWidth;
            }
            fprintf(g_seq_fp, " slot%u=0x%llX:%u firstConst=%u numConst=%u", startSlot + i,
                     (unsigned long long)(uintptr_t)b, bw, firstConst, numConst);
        }
        seq_finish_locked(0);
    }

    g_vsset_cb1_orig(ctx, startSlot, numBuffers, ppCB, pFirstConstant, pNumConstants);
}

/* Task 5 review addendum 2: the gameplay capture showed draws arriving
 * from six distinct threads while every state-setting event (VSSETSHADER/
 * VSSETCB/MAP) stayed on the render thread - strongly suggesting deferred
 * contexts recording command lists on worker threads, then submitted via
 * ExecuteCommandList on the immediate context. These two hooks (plus the
 * ctx= field now on every event line) let a later analysis pass confirm
 * or refute that directly from seqdump.log, without guessing from thread
 * ids alone. */
static HRESULT STDMETHODCALLTYPE Hook_FinishCommandList(ID3D11DeviceContext *ctx,
                                                          WINBOOL restoreDeferredContextState,
                                                          ID3D11CommandList **ppCommandList) {
    HRESULT hr = g_finishcmdlist_orig(ctx, restoreDeferredContextState, ppCommandList);
    if (seq_active()) {
        ID3D11CommandList *cl = (ppCommandList != NULL) ? *ppCommandList : NULL;
        seq_line(ctx, 0, "FINISHCMDLIST restore=%d hr=0x%08lX cmdlist=0x%llX",
                  (int)restoreDeferredContextState, (unsigned long)hr,
                  (unsigned long long)(uintptr_t)cl);
    }
    return hr;
}

static void STDMETHODCALLTYPE Hook_ExecuteCommandList(ID3D11DeviceContext *ctx,
                                                        ID3D11CommandList *commandList,
                                                        WINBOOL restoreContextState) {
    if (seq_active()) {
        seq_line(ctx, 0, "EXECUTECMDLIST cmdlist=0x%llX restore=%d",
                  (unsigned long long)(uintptr_t)commandList, (int)restoreContextState);
    }
    g_executecmdlist_orig(ctx, commandList, restoreContextState);
}

/* ---- Present-driven arming (called from hooks.c's Hook_Present) ---- */

void seqdump_on_present(UINT64 frame_number) {
    if (!g_seq_hooks_ok) {
        return;
    }

    if (!g_seq_first_frame_set) {
        g_seq_first_frame_set = 1;
        g_seq_first_frame = frame_number;
    }

    if (!g_seq_armed) {
        if (g_seq_armfile_mode) {
            /* Live/manual arm (Task 5 addendum): check every ~30 frames
             * whether seqarm.txt exists - a plain existence check
             * (GetFileAttributesW), not an open, so it is cheap enough to
             * poll from the render thread. Arms the instant it appears;
             * default frame-301 auto-arm logic below is skipped entirely
             * in this mode. */
            wchar_t path[MAX_PATH];

            if ((frame_number % SEQDUMP_ARMFILE_CHECK_FRAMES) != 0) {
                return;
            }
            seqarm_path(path, MAX_PATH);
            if (path[0] == L'\0' || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
                return; /* not armed yet */
            }

            g_seq_armed = 1;
            log_msg("seqdump: ARMED by seqarm.txt at frame %llu", (unsigned long long)frame_number);
            if (g_seq_fp != NULL) {
                EnterCriticalSection(&g_seq_cs);
                fprintf(g_seq_fp, "SEQDUMP ARMED by seqarm.txt at frame %llu\n",
                         (unsigned long long)frame_number);
                fflush(g_seq_fp);
                LeaveCriticalSection(&g_seq_cs);
            }
        } else {
            if (frame_number - g_seq_first_frame < SEQDUMP_ARM_FRAMES) {
                return;
            }
            g_seq_armed = 1;

            {
                /* Brief: "log ONE line at arm time saying whether the
                 * context QIs to ID3D11DeviceContext1" - this checks the
                 * REAL captured game context (as opposed to
                 * seqdump_install()'s own QI probe against the throwaway
                 * dummy context, used there to decide whether to hook
                 * VSSetConstantBuffers1 at all). */
                const char *support = "unknown (real context not yet captured)";
                if (d3d_capture_ready() && g_d3d.ctx != NULL) {
                    ID3D11DeviceContext1 *ctx1 = NULL;
                    HRESULT hr = ID3D11DeviceContext_QueryInterface(g_d3d.ctx, &IID_ID3D11DeviceContext1,
                                                                      (void **)&ctx1);
                    if (SUCCEEDED(hr) && ctx1 != NULL) {
                        support = "yes";
                        ID3D11DeviceContext1_Release(ctx1);
                    } else {
                        support = "no";
                    }
                }
                log_msg("seqdump: armed at frame %llu; real game context QIs to "
                         "ID3D11DeviceContext1 = %s", (unsigned long long)frame_number, support);
                if (g_seq_fp != NULL) {
                    EnterCriticalSection(&g_seq_cs);
                    fprintf(g_seq_fp, "SEQDUMP ARMED at frame %llu; real-context "
                             "QIs-to-ID3D11DeviceContext1=%s\n",
                             (unsigned long long)frame_number, support);
                    fflush(g_seq_fp);
                    LeaveCriticalSection(&g_seq_cs);
                }
            }
        }
    }

    /* Present takes no ID3D11DeviceContext argument (it's an
     * IDXGISwapChain method), so there is no natural ctx= value here. Use
     * the real captured immediate context if available - genuinely useful
     * (it's the context Present implicitly flushes/relates to) and keeps
     * the ctx= field's meaning consistent rather than leaving it blank;
     * NULL (printed as ctx=0x0) if the real context hasn't been captured
     * yet (should not happen once armed, since arming only ever occurs
     * well after the first Present, but stay fail-safe regardless). */
    seq_line(d3d_capture_ready() ? (const void *)g_d3d.ctx : NULL,
              1 /* flush at Present boundaries */, "PRESENT frame=%llu", (unsigned long long)frame_number);
}

/* ---- setup / teardown ---- */

static int seqdump_open_logfile(void) {
    wchar_t local_appdata[MAX_PATH];
    wchar_t dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", local_appdata, MAX_PATH);

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

    swprintf(path, MAX_PATH, L"%s\\seqdump.log", dir);
    g_seq_fp = _wfopen(path, L"w"); /* fresh per-session file, same convention as cbdump.log */
    return g_seq_fp != NULL;
}

void seqdump_install(ID3D11DeviceContext *dummy_ctx) {
    char flag[8];
    DWORD len;
    void **vtbl;
    int ok_map, ok_unmap, ok_update, ok_vscb, ok_vsset, ok_di, ok_d, ok_dii, ok_dinst;
    int ok_finishcl, ok_executecl;
    int ok_vscb1 = 0;

    if (g_seq_installed) {
        return; /* hooks.c only calls this once, but stay idempotent */
    }
    g_seq_installed = 1;

    len = GetEnvironmentVariableA("TEWVR_SEQDUMP", flag, sizeof(flag));
    if (len == 0 || len >= sizeof(flag) || strcmp(flag, "1") != 0) {
        return; /* off by default: touch nothing */
    }

    if (dummy_ctx == NULL) {
        log_msg("seqdump: TEWVR_SEQDUMP=1 but dummy context is NULL; skipping event-stream hooks");
        return;
    }

    {
        char aflag[8];
        DWORD alen = GetEnvironmentVariableA("TEWVR_SEQDUMP_ARMFILE", aflag, sizeof(aflag));
        g_seq_armfile_mode = (alen > 0 && alen < sizeof(aflag) && strcmp(aflag, "1") == 0);
    }

    if (!seqdump_open_logfile()) {
        log_msg("seqdump: failed to open seqdump.log; skipping event-stream hooks");
        return;
    }

    InitializeCriticalSection(&g_seq_cs);
    g_seq_cs_ready = 1;

    if (!mh_glue_init()) {
        log_msg("seqdump: MinHook init failed; skipping event-stream hooks");
        return;
    }

    vtbl = *(void ***)dummy_ctx;

    ok_map = mh_glue_create_and_enable(vtbl[VTBL_CTX_MAP], (void *)&Hook_Map, (void **)&g_map_orig,
                                        "seqdump ID3D11DeviceContext::Map");
    ok_unmap = mh_glue_create_and_enable(vtbl[VTBL_CTX_UNMAP], (void *)&Hook_Unmap, (void **)&g_unmap_orig,
                                          "seqdump ID3D11DeviceContext::Unmap");
    ok_update = mh_glue_create_and_enable(vtbl[VTBL_CTX_UPDATESUBRESOURCE], (void *)&Hook_UpdateSubresource,
                                           (void **)&g_update_orig,
                                           "seqdump ID3D11DeviceContext::UpdateSubresource");
    ok_vscb = mh_glue_create_and_enable(vtbl[VTBL_CTX_VSSETCONSTANTBUFFERS], (void *)&Hook_VSSetCB,
                                         (void **)&g_vsset_cb_orig,
                                         "seqdump ID3D11DeviceContext::VSSetConstantBuffers");
    ok_vsset = mh_glue_create_and_enable(vtbl[VTBL_CTX_VSSETSHADER], (void *)&Hook_VSSetShader,
                                          (void **)&g_vsset_orig, "seqdump ID3D11DeviceContext::VSSetShader");
    ok_di = mh_glue_create_and_enable(vtbl[VTBL_CTX_DRAWINDEXED], (void *)&Hook_DrawIndexed,
                                       (void **)&g_drawindexed_orig, "seqdump ID3D11DeviceContext::DrawIndexed");
    ok_d = mh_glue_create_and_enable(vtbl[VTBL_CTX_DRAW], (void *)&Hook_Draw, (void **)&g_draw_orig,
                                      "seqdump ID3D11DeviceContext::Draw");
    ok_dii = mh_glue_create_and_enable(vtbl[VTBL_CTX_DRAWINDEXEDINST], (void *)&Hook_DrawIndexedInst,
                                        (void **)&g_drawindexedinst_orig,
                                        "seqdump ID3D11DeviceContext::DrawIndexedInstanced");
    ok_dinst = mh_glue_create_and_enable(vtbl[VTBL_CTX_DRAWINST], (void *)&Hook_DrawInst,
                                          (void **)&g_drawinst_orig, "seqdump ID3D11DeviceContext::DrawInstanced");
    ok_finishcl = mh_glue_create_and_enable(vtbl[VTBL_CTX_FINISHCOMMANDLIST], (void *)&Hook_FinishCommandList,
                                             (void **)&g_finishcmdlist_orig,
                                             "seqdump ID3D11DeviceContext::FinishCommandList");
    ok_executecl = mh_glue_create_and_enable(vtbl[VTBL_CTX_EXECUTECOMMANDLIST], (void *)&Hook_ExecuteCommandList,
                                              (void **)&g_executecmdlist_orig,
                                              "seqdump ID3D11DeviceContext::ExecuteCommandList");

    {
        /* Brief: "if the context supports ID3D11DeviceContext1 ... hook or
         * at least probe VSSetConstantBuffers1 ... via the same vtable
         * technique". Probed here against the DUMMY context (same
         * shared-vtable trick every other hook in this codebase uses) so
         * the hook installs are consistent regardless of when the real
         * game context becomes available; seqdump_on_present() separately
         * logs the REAL context's QI result at arm time per the brief's
         * literal wording. */
        ID3D11DeviceContext1 *ctx1 = NULL;
        HRESULT hr = ID3D11DeviceContext_QueryInterface(dummy_ctx, &IID_ID3D11DeviceContext1,
                                                          (void **)&ctx1);
        if (SUCCEEDED(hr) && ctx1 != NULL) {
            void **vtbl1 = *(void ***)ctx1;
            ok_vscb1 = mh_glue_create_and_enable(vtbl1[VTBL_CTX1_VSSETCONSTANTBUFFERS1],
                                                   (void *)&Hook_VSSetCB1, (void **)&g_vsset_cb1_orig,
                                                   "seqdump ID3D11DeviceContext1::VSSetConstantBuffers1");
            ID3D11DeviceContext1_Release(ctx1);
            log_msg("seqdump: dummy context QIs to ID3D11DeviceContext1; VSSetConstantBuffers1 hook %s",
                     ok_vscb1 ? "installed" : "failed");
        } else {
            log_msg("seqdump: dummy context does not QI to ID3D11DeviceContext1 (hr=0x%08lX); "
                     "VSSetConstantBuffers1 not hooked", (unsigned long)hr);
        }
    }

    g_seq_hooks_ok = ok_map || ok_unmap || ok_update || ok_vscb || ok_vsset ||
                     ok_di || ok_d || ok_dii || ok_dinst || ok_finishcl || ok_executecl;

    log_msg("seqdump: hooks map=%d unmap=%d update=%d vscb=%d vsset=%d di=%d d=%d dii=%d dinst=%d "
             "vscb1=%d finishcl=%d executecl=%d -> %s (arm=%s, caps at %d events)",
             ok_map, ok_unmap, ok_update, ok_vscb, ok_vsset, ok_di, ok_d, ok_dii, ok_dinst, ok_vscb1,
             ok_finishcl, ok_executecl,
             g_seq_hooks_ok ? "ACTIVE" : "inactive",
             g_seq_armfile_mode ? "seqarm.txt file-trigger" : "frame-301 auto",
             SEQDUMP_MAX_EVENTS);

    if (g_seq_hooks_ok && g_seq_fp != NULL) {
        fprintf(g_seq_fp, "TEWVR seqdump session start (TEWVR_SEQDUMP=1%s); hooks: map=%d unmap=%d "
                 "update=%d vscb=%d vsset=%d di=%d d=%d dii=%d dinst=%d vscb1=%d finishcl=%d executecl=%d\n",
                 g_seq_armfile_mode ? ", TEWVR_SEQDUMP_ARMFILE=1 (arm via seqarm.txt)" : "",
                 ok_map, ok_unmap, ok_update, ok_vscb, ok_vsset, ok_di, ok_d, ok_dii, ok_dinst, ok_vscb1,
                 ok_finishcl, ok_executecl);
        fflush(g_seq_fp);
    }
}

void seqdump_remove(void) {
    if (!g_seq_cs_ready) {
        return; /* seqdump_install() was never called, or TEWVR_SEQDUMP was unset */
    }

    g_seq_hooks_ok = 0;

    if (g_seq_fp) {
        fclose(g_seq_fp);
        g_seq_fp = NULL;
    }

    DeleteCriticalSection(&g_seq_cs);
    g_seq_cs_ready = 0;
}

void seqdump_clear_stale_armfile(void) {
    wchar_t path[MAX_PATH];

    seqarm_path(path, MAX_PATH);
    if (path[0] == L'\0') {
        return; /* LOCALAPPDATA unavailable; nothing we can do, and nothing to warn about */
    }

    if (!DeleteFileW(path)) {
        DWORD err = GetLastError();
        /* ERROR_FILE_NOT_FOUND/ERROR_PATH_NOT_FOUND are the expected common
         * case (no stale file, or %LOCALAPPDATA%\TEWVR doesn't exist yet on
         * a fresh machine) - silent. Anything else is unexpected but still
         * fail-safe: log once and continue starting up regardless. */
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
            log_msg("seqdump: failed to delete stale seqarm.txt (gle=%lu)", (unsigned long)err);
        }
    } else {
        log_msg("seqdump: deleted stale seqarm.txt left over from a previous session");
    }
}
