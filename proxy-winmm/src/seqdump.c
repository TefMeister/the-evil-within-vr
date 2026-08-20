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
#define VTBL_CTX_OMSETRENDERTARGETS   33
#define VTBL_CTX_RSSETVIEWPORTS       44
#define VTBL_CTX_UPDATESUBRESOURCE    48
/* ExecuteCommandList / FinishCommandList (Task 5 review addendum 2):
 * counted directly (0-based) from the llvm-mingw toolchain's own d3d11.h
 * ID3D11DeviceContextVtbl struct, method-by-method from QueryInterface=0 -
 * not recomputed by formula, to avoid a miscount. ExecuteCommandList lands
 * at index 58; FinishCommandList is the LAST method in the whole vtable,
 * at index 114. OMSetRenderTargets (33) and RSSetViewports (44) (addendum
 * 3) were counted the same pass. Cross-check: this same counting pass
 * also lands VSSetConstantBuffers at 7 and UpdateSubresource at 48,
 * exactly matching cbdump.c's/shaderdump.c's already-verified indices for
 * those two - confirming the count is right before trusting the new
 * ones. */
#define VTBL_CTX_EXECUTECOMMANDLIST   58
#define VTBL_CTX_FINISHCOMMANDLIST    114
/* ID3D11DeviceContext1 vtable slot for VSSetConstantBuffers1 - given
 * verbatim by the task-5 brief, not recomputed here. */
#define VTBL_CTX1_VSSETCONSTANTBUFFERS1 120

#define SEQDUMP_MAX_EVENTS  40000
#define SEQDUMP_ARM_FRAMES  300
#define SEQDUMP_ARMFILE_CHECK_FRAMES 30
#define SEQ_CB_MAX_BYTEWIDTH 8192
#define SEQ_SKIPCL_RECHECK_CALLS 30

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
typedef void(STDMETHODCALLTYPE *RSSetViewports_t)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
typedef void(STDMETHODCALLTYPE *OMSetRenderTargets_t)(ID3D11DeviceContext *, UINT,
                                                       ID3D11RenderTargetView *const *,
                                                       ID3D11DepthStencilView *);

/* Forward declarations: seq_maybe_late_hook_deferred_ctx() (Task 5
 * addendum 3, defined well before these in file order alongside the rest
 * of the deferred-context-discovery helpers) needs to pass these six
 * detour functions to MinHook by address. */
static HRESULT STDMETHODCALLTYPE Hook_Map(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT,
                                           D3D11_MAPPED_SUBRESOURCE *);
static void STDMETHODCALLTYPE Hook_Unmap(ID3D11DeviceContext *, ID3D11Resource *, UINT);
static void STDMETHODCALLTYPE Hook_VSSetCB(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
static void STDMETHODCALLTYPE Hook_VSSetShader(ID3D11DeviceContext *, ID3D11VertexShader *,
                                                ID3D11ClassInstance *const *, UINT);
static void STDMETHODCALLTYPE Hook_RSSetViewports(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
static void STDMETHODCALLTYPE Hook_OMSetRenderTargets(ID3D11DeviceContext *, UINT,
                                                       ID3D11RenderTargetView *const *,
                                                       ID3D11DepthStencilView *);

/* Single-target hooks: DrawIndexed/Draw/DrawIndexedInstanced/DrawInstanced/
 * FinishCommandList/ExecuteCommandList happen to share ONE implementation
 * address across the immediate and deferred vtable flavors (confirmed by
 * the addendum-2 capture: FinishCommandList fired with a ctx pointer
 * whose vtable was never explicitly hooked), so a single original
 * function pointer, captured once at install time on the immediate
 * vtable, is correct to call through regardless of which flavor of ctx
 * invoked the detour. VSSetShader/VSSetConstantBuffers/Map/Unmap/
 * RSSetViewports/OMSetRenderTargets are NOT guaranteed to share an
 * address across flavors (see the address-keyed g_hooked_funcs table
 * below) and so use a lookup-by-address dispatch instead of a single
 * static original. */
static UpdateSubresource_t g_update_orig = NULL;
static DrawIndexed_t g_drawindexed_orig = NULL;
static Draw_t g_draw_orig = NULL;
static DrawIndexedInst_t g_drawindexedinst_orig = NULL;
static DrawInst_t g_drawinst_orig = NULL;
static VSSetCB1_t g_vsset_cb1_orig = NULL;
static FinishCommandList_t g_finishcmdlist_orig = NULL;
static ExecuteCommandList_t g_executecmdlist_orig = NULL;

static int g_seq_installed = 0;
static int g_seq_hooks_ok = 0; /* at least one of the hooks above installed + log open */

/* Guards g_seq_fp, the sequence counter, the event/complete counters, the
 * Map->Unmap latch table, and (Task 5 addendum 3) the address-keyed
 * hooked-function table and the seen-ctx table. Contention is expected to
 * stay low even with deferred-context worker threads now in the mix
 * (each table is tiny and only scanned linearly), but this is diagnostic
 * instrumentation capped at 40,000 events - a lock costs nothing here. */
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

/* ---- address-keyed hooked-function table (Task 5 addendum 3) ----
 * The gameplay capture showed world-geometry draws recorded on deferred
 * contexts whose VSSetShader/VSSetConstantBuffers/Map/Unmap/
 * RSSetViewports/OMSetRenderTargets state calls our (immediate-vtable-
 * only) hooks never saw. The FIRST design here keyed the dispatch table
 * by the calling ctx's own VTABLE pointer, on the assumption that these
 * six functions never share an underlying implementation address between
 * the immediate and deferred vtable flavors (unlike the Draw variants,
 * FinishCommandList, and ExecuteCommandList, which do). A live smoke test
 * proved that assumption wrong for at least some of these six on this
 * engine/driver: real calls arrived on a deferred ctx whose vtable had
 * not yet been late-hooked, meaning the underlying address WAS already
 * hooked (via the immediate vtable) even though the deferred ctx's own
 * vtable pointer was never registered - the vtable-pointer-keyed lookup
 * returned "not found" and the call was WRONGLY dropped.
 *
 * Fixed by keying the table on the raw target ADDRESS instead of the
 * vtable pointer - the address is the one thing that is always correct
 * to key on, since MinHook hooks (and therefore every original/
 * trampoline) are fundamentally per-address, not per-vtable-instance.
 * Each detour looks up `(*(void***)ctx)[SLOT]` (the address THIS call
 * actually went through) and finds whichever original was registered for
 * that address, whether it was first hooked via the immediate vtable or
 * a late-hooked deferred one - correct regardless of how many distinct
 * vtable pointers happen to share that address. */
/* 64 comfortably exceeds 6 late-hookable functions * a handful of
 * distinct vtable flavors ever realistically expected (immediate + a
 * couple of deferred-context implementations); see register_hooked_func_
 * locked()'s fail-loud behavior at capacity (Task 5 review, Important
 * finding 2) for what happens if it's ever exceeded anyway. */
#define SEQ_MAX_HOOKED_FUNCS 64
struct HookedFunc {
    void *addr; /* NULL == free slot; the raw target function address */
    void *orig; /* the trampoline MinHook gave us for that address */
};
static struct HookedFunc g_hooked_funcs[SEQ_MAX_HOOKED_FUNCS];
static int g_hooked_funcs_count = 0;
static int g_hooked_funcs_full_warned = 0;
static void *g_immediate_vtbl = NULL; /* captured at install time from dummy_ctx */

/* Rate-limits the two classes of per-call diagnostic log lines this
 * module can hit on a hot path (Task 5 review, Important finding 3's
 * rate-limiting request): logs the first SEQ_RATE_LIMIT_BURST occurrences
 * of a given condition unconditionally, then only every
 * SEQ_RATE_LIMIT_EVERY_NTH-th occurrence after that. A persistent
 * per-draw/per-call miss could otherwise fire log_msg() (its own
 * critical section + file I/O) at thousands of lines/sec and stall the
 * render thread. `counter` is intentionally unlocked (InterlockedIncrement
 * on a plain LONG) - a rate limiter's own occasional off-by-one under
 * contention is immaterial; what matters is bounding the total volume. */
#define SEQ_RATE_LIMIT_BURST 10
#define SEQ_RATE_LIMIT_EVERY_NTH 500
static int seq_rate_limit_should_fire(volatile LONG *counter) {
    LONG n = InterlockedIncrement(counter);
    if (n <= SEQ_RATE_LIMIT_BURST) {
        return 1;
    }
    return (n % SEQ_RATE_LIMIT_EVERY_NTH) == 0;
}
static volatile LONG g_bug_log_count = 0;      /* "no original found, even after fallback" */
static volatile LONG g_fallback_log_count = 0; /* "primary lookup missed, fallback succeeded" */

/* Tracks which ctx pointers have already been inspected for late-hooking,
 * so the (locked) vtable comparison only ever runs once per distinct ctx
 * instance, not on every single Draw/FinishCommandList call. */
#define SEQ_SEEN_CTX_MAX 32
static void *g_seen_ctx[SEQ_SEEN_CTX_MAX];
static int g_seen_ctx_count = 0;
static int g_seen_ctx_full_warned = 0;

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

/* Live SKIPCL visual toggle (Task 5 addendum 3) - see Hook_ExecuteCommandList. */
static int g_skipcl_call_count = 0;
static int g_skipcl_active = 0;

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

/* Fills `out` with %LOCALAPPDATA%\TEWVR\skipcl.txt. Same contract as
 * seqarm_path(); shared by the live SKIPCL toggle and
 * seqdump_clear_stale_skipcl(). */
static void skipcl_path(wchar_t *out, size_t out_count) {
    wchar_t la[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        out[0] = L'\0';
        return;
    }
    swprintf(out, out_count, L"%s\\TEWVR\\skipcl.txt", la);
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

/* Describes a render-target/depth-stencil resource as "WxHxFMT", or "?" if
 * NULL or not a Texture2D (the overwhelming majority of render targets
 * and depth buffers are Texture2D; anything else is out of scope for
 * this diagnostic per the addendum - "if not a Texture2D, log rt0=?").
 * Always releases whatever it QIs. */
static void describe_texture2d(ID3D11Resource *res, char *buf, size_t buflen) {
    ID3D11Texture2D *tex = NULL;
    HRESULT hr;

    if (res == NULL) {
        snprintf(buf, buflen, "?");
        return;
    }

    hr = ID3D11Resource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex);
    if (FAILED(hr) || tex == NULL) {
        snprintf(buf, buflen, "?");
        return;
    }

    {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc(tex, &d);
        snprintf(buf, buflen, "%ux%ux%d", (unsigned)d.Width, (unsigned)d.Height, (int)d.Format);
    }
    ID3D11Texture2D_Release(tex);
}

/* ---- address-keyed hooked-function table (caller holds g_seq_cs unless noted) ---- */

static void *find_hooked_orig_locked(void *addr) {
    int i;
    for (i = 0; i < g_hooked_funcs_count; i++) {
        if (g_hooked_funcs[i].addr == addr) {
            return g_hooked_funcs[i].orig;
        }
    }
    return NULL;
}

/* Returns 1 on success, 0 if the table is already full (Task 5 review,
 * Important finding 2: the caller MUST NOT leave a hook enabled when
 * this fails - an enabled-but-unregistered hook would silently swallow
 * every future call through it forever, since no lookup could ever find
 * its original again). */
static int register_hooked_func_locked(void *addr, void *orig) {
    if (g_hooked_funcs_count >= SEQ_MAX_HOOKED_FUNCS) {
        return 0;
    }
    g_hooked_funcs[g_hooked_funcs_count].addr = addr;
    g_hooked_funcs[g_hooked_funcs_count].orig = orig;
    g_hooked_funcs_count++;
    return 1;
}

/* Removes the entry for `addr`, if present. Used only on the rare
 * mh_glue_enable() failure path immediately after a successful
 * register_hooked_func_locked() (see hook_or_reuse_locked_guard()), so a
 * future hook attempt on this same address can't reuse a trampoline
 * pointer MinHook's own internal cleanup may already have freed.
 * Swap-with-last removal - this table is never iterated for anything but
 * exact-address lookups, so entry order doesn't matter. */
static void unregister_hooked_func_locked(void *addr) {
    int i;
    for (i = 0; i < g_hooked_funcs_count; i++) {
        if (g_hooked_funcs[i].addr == addr) {
            g_hooked_funcs[i] = g_hooked_funcs[g_hooked_funcs_count - 1];
            g_hooked_funcs_count--;
            return;
        }
    }
}

/* Hooks `target` with `detour`, OR - if that exact address is already
 * hooked (a different vtable sharing the function's address, or a race
 * with another thread hooking the same address concurrently) - reuses
 * the EXISTING original instead of attempting (and safely failing) a
 * redundant MH_CreateHook call. Always fills `*out_orig` with a valid
 * original and returns 1 on success (fresh hook OR reuse); returns 0 on
 * genuine failure (MH_CreateHook/MH_EnableHook failed with no racing
 * thread's concurrent attempt succeeding either, OR the hooked-function
 * table is full).
 *
 * Task 5 review, Important finding 1: create and register BEFORE
 * enabling (via the split mh_glue_create()/mh_glue_enable() - see
 * minhook_glue.h), not create-and-enable-then-register. The original
 * version here called the combined mh_glue_create_and_enable() and only
 * afterward took g_seq_cs to publish into g_hooked_funcs - between
 * MH_EnableHook() returning and the publish, the detour was already LIVE
 * but undiscoverable, so any thread calling that address in that window
 * got dropped (a real crash risk for Hook_Map specifically: engines
 * rarely check Map's HRESULT, so an uninitialized D3D11_MAPPED_SUBRESOURCE
 * silently read as valid is a realistic crash, not benign data loss).
 * Registering before enabling closes that window entirely: the table
 * entry always exists before the target's code can possibly redirect to
 * `detour`.
 *
 * Important finding 2: if registration fails (table full), the hook is
 * NEVER enabled - MH_RemoveHook() tears down the still-inert
 * (never-enabled) hook immediately, and this logs loudly rather than
 * silently leaving an undispatchable-but-live hook.
 *
 * Caller holds no lock (this takes g_seq_cs itself, briefly, around each
 * table access - the MinHook calls themselves run unlocked). */
static int hook_or_reuse_locked_guard(void *target, void *detour, void **out_orig, const char *name) {
    void *existing;

    EnterCriticalSection(&g_seq_cs);
    existing = find_hooked_orig_locked(target);
    LeaveCriticalSection(&g_seq_cs);

    if (existing != NULL) {
        *out_orig = existing;
        return 1;
    }

    {
        void *orig = NULL;
        int created = mh_glue_create(target, detour, &orig, name);
        int win;

        if (!created) {
            /* MH_CreateHook itself failed - possibly MH_ERROR_ALREADY_
             * CREATED because a racing thread is concurrently creating
             * this exact target right now. Re-check before giving up. */
            EnterCriticalSection(&g_seq_cs);
            existing = find_hooked_orig_locked(target);
            LeaveCriticalSection(&g_seq_cs);
            if (existing != NULL) {
                *out_orig = existing;
                return 1;
            }
            return 0;
        }

        /* Created (trampoline/orig filled) but NOT YET enabled - the
         * target's code is still untouched, so no call can reach
         * `detour` yet. Register now, while it is still safe to fail
         * without anything having been dropped. */
        EnterCriticalSection(&g_seq_cs);
        existing = find_hooked_orig_locked(target);
        win = (existing == NULL) ? register_hooked_func_locked(target, orig) : 0;
        LeaveCriticalSection(&g_seq_cs);

        if (existing != NULL) {
            /* A racing thread already created+registered+enabled this
             * exact target first. Our own MH_CreateHook succeeded but is
             * a now-redundant, never-enabled duplicate - tear it down
             * (safe: it was never made live) and use the winner's
             * original. */
            MH_RemoveHook(target);
            *out_orig = existing;
            return 1;
        }

        if (!win) {
            /* Table genuinely full (Important finding 2): refuse to
             * enable an address we could never dispatch again. Tear down
             * the still-inert hook and fail loudly, once. */
            MH_RemoveHook(target);
            if (!g_hooked_funcs_full_warned) {
                g_hooked_funcs_full_warned = 1;
                log_msg("seqdump: g_hooked_funcs table full (%d); refusing to enable %s - "
                         "that address will run UNHOOKED from here on (safe, just unobserved)",
                         SEQ_MAX_HOOKED_FUNCS, name);
            }
            return 0;
        }

        /* Registered - only now is it safe to make the target live. */
        if (!mh_glue_enable(target, name)) {
            EnterCriticalSection(&g_seq_cs);
            unregister_hooked_func_locked(target);
            LeaveCriticalSection(&g_seq_cs);
            return 0;
        }

        *out_orig = orig;
        return 1;
    }
}

/* Looks up the original for the D3D11 method at `slot_index` on `ctx`'s
 * OWN vtable, falling back to the immediate vtable's original for that
 * same slot if the primary (ctx-vtable) lookup misses.
 *
 * Task 5 review, Important finding 3: a third-party D3D11 vtable
 * patcher installed after this DLL (Steam overlay, RivaTuner/RTSS,
 * ReShade, etc. commonly hook/rewrite individual vtable slots) could
 * rewrite a slot we've already hooked, changing which address a FUTURE
 * call for that same ctx actually goes through - the dynamic
 * `(*(void***)ctx)[slot_index]` re-read this module relies on for
 * dispatch would then miss even though the ctx itself is perfectly
 * legitimate. Rather than dropping the call outright, fall back to the
 * still-registered immediate-vtable original for the same slot, which is
 * correct in the overwhelming majority of real-world cases (the called
 * method's actual behavior for a given ctx flavor does not change just
 * because a third party rewrote the vtable pointer that reaches it -
 * only the ADDRESS through which OUR code would have found the original
 * changes). Logs (rate-limited) when the fallback is actually used, so
 * this staying silent means it never fired. */
static void *seq_resolve_orig(ID3D11DeviceContext *ctx, int slot_index) {
    void **vtbl = *(void ***)ctx;
    void *primary_target = vtbl[slot_index];
    void *orig;
    int used_fallback = 0;

    EnterCriticalSection(&g_seq_cs);
    orig = find_hooked_orig_locked(primary_target);
    if (orig == NULL && g_immediate_vtbl != NULL) {
        void *fallback_target = ((void **)g_immediate_vtbl)[slot_index];
        if (fallback_target != primary_target) {
            orig = find_hooked_orig_locked(fallback_target);
            if (orig != NULL) {
                used_fallback = 1;
            }
        }
    }
    LeaveCriticalSection(&g_seq_cs);

    if (used_fallback && seq_rate_limit_should_fire(&g_fallback_log_count)) {
        log_msg("seqdump: vtable slot %d for ctx=%p (vtbl=%p) didn't resolve directly "
                 "(possible third-party vtable patch - Steam overlay/RTSS/ReShade); "
                 "used immediate-vtable fallback", slot_index, (void *)ctx, (void *)vtbl);
    }

    return orig;
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
 * Task 5 review addendum 2: every event line also carries the D3D11
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

/* ---- deferred-context discovery + late hooking (Task 5 addendum 3) ----
 *
 * Reads `ctx`'s own vtable pointer and, if it differs from the immediate
 * context's (captured at seqdump_install() time as g_immediate_vtbl),
 * MinHook-installs fresh hooks on THAT vtable's VSSetShader/
 * VSSetConstantBuffers/Map/Unmap/RSSetViewports/OMSetRenderTargets slots -
 * reusing the exact same detour functions (they already log the `ctx`
 * argument they were called with, so events self-identify which flavor
 * produced them; see the address-keyed g_hooked_funcs dispatch each of
 * those detours does).
 *
 * Called from Hook_DrawIndexed/Draw/DrawIndexedInst/DrawInst and
 * Hook_FinishCommandList - "the FINISHCMDLIST and DRAW hooks are the ones
 * deferred contexts reach" per the addendum, because those six functions'
 * addresses happen to be SHARED between immediate and deferred vtables
 * (unlike the six being late-hooked here), so our existing single-target
 * hooks on them already fire for deferred-context calls without any
 * special handling. ("Those six functions" = the four Draw variants plus
 * FinishCommandList and ExecuteCommandList.)
 *
 * Runs regardless of seq_active() (deliberately NOT gated on
 * armed/capture state) - the goal is for a deferred vtable to already be
 * hooked by the time capture arms, not to race the arm. Cheap and
 * fail-safe throughout, exactly like every other hook-install path in
 * this codebase: any failure just logs and leaves that vtable's state
 * calls unseen; never crashes; never blocks or alters the calling draw/
 * FinishCommandList call itself (the caller still calls through to the
 * real original normally either way, this function only adds hooks for
 * FUTURE calls on other functions). Safe to call from inside a live
 * detour - MH_CreateHook/MH_EnableHook at runtime on a currently-
 * unrelated target is an ordinary MinHook operation. */
static void seq_maybe_late_hook_deferred_ctx(ID3D11DeviceContext *ctx) {
    void **vtbl;
    int is_new;

    if (ctx == NULL || !g_seq_hooks_ok) {
        return;
    }

    EnterCriticalSection(&g_seq_cs);
    is_new = 1;
    {
        int i;
        for (i = 0; i < g_seen_ctx_count; i++) {
            if (g_seen_ctx[i] == (void *)ctx) {
                is_new = 0;
                break;
            }
        }
        if (is_new) {
            if (g_seen_ctx_count < SEQ_SEEN_CTX_MAX) {
                g_seen_ctx[g_seen_ctx_count++] = (void *)ctx;
            } else {
                is_new = 0; /* table full: stop retrying vtable inspection for further unknown ctx pointers */
                if (!g_seen_ctx_full_warned) {
                    g_seen_ctx_full_warned = 1;
                    log_msg("seqdump: seen-ctx table full (%d); further unknown contexts "
                             "won't be inspected for late-hooking", SEQ_SEEN_CTX_MAX);
                }
            }
        }
    }
    LeaveCriticalSection(&g_seq_cs);

    if (!is_new) {
        return;
    }

    vtbl = *(void ***)ctx;
    if ((void *)vtbl == g_immediate_vtbl) {
        return; /* same flavor as the immediate context: already covered */
    }

    /* hook_or_reuse_locked_guard() is idempotent and address-keyed, so
     * calling it again for a vtable/address combination already covered
     * (by an earlier ctx instance sharing this same vtable, or because
     * one of these addresses happens to be shared with the immediate
     * vtable - the bug this address-keyed design fixes) is always safe
     * and cheap: it just returns the existing original without touching
     * MinHook again. No separate "is this vtable already registered"
     * gate is needed before calling it. */
    {
        VSSetShader_t vsset_o = NULL;
        VSSetCB_t vscb_o = NULL;
        Map_t map_o = NULL;
        Unmap_t unmap_o = NULL;
        RSSetViewports_t rsvp_o = NULL;
        OMSetRenderTargets_t omrt_o = NULL;
        int ok_vsset, ok_vscb, ok_map, ok_unmap, ok_rsvp, ok_omrt;

        ok_vsset = hook_or_reuse_locked_guard(vtbl[VTBL_CTX_VSSETSHADER], (void *)&Hook_VSSetShader,
                                               (void **)&vsset_o,
                                               "seqdump (deferred) ID3D11DeviceContext::VSSetShader");
        ok_vscb = hook_or_reuse_locked_guard(vtbl[VTBL_CTX_VSSETCONSTANTBUFFERS], (void *)&Hook_VSSetCB,
                                              (void **)&vscb_o,
                                              "seqdump (deferred) ID3D11DeviceContext::VSSetConstantBuffers");
        ok_map = hook_or_reuse_locked_guard(vtbl[VTBL_CTX_MAP], (void *)&Hook_Map, (void **)&map_o,
                                             "seqdump (deferred) ID3D11DeviceContext::Map");
        ok_unmap = hook_or_reuse_locked_guard(vtbl[VTBL_CTX_UNMAP], (void *)&Hook_Unmap, (void **)&unmap_o,
                                               "seqdump (deferred) ID3D11DeviceContext::Unmap");
        ok_rsvp = hook_or_reuse_locked_guard(vtbl[VTBL_CTX_RSSETVIEWPORTS], (void *)&Hook_RSSetViewports,
                                              (void **)&rsvp_o,
                                              "seqdump (deferred) ID3D11DeviceContext::RSSetViewports");
        ok_omrt = hook_or_reuse_locked_guard(vtbl[VTBL_CTX_OMSETRENDERTARGETS], (void *)&Hook_OMSetRenderTargets,
                                              (void **)&omrt_o,
                                              "seqdump (deferred) ID3D11DeviceContext::OMSetRenderTargets");

        log_msg("seqdump: inspected deferred-context vtable %p (immediate vtbl is %p): "
                 "vsset=%d vscb=%d map=%d unmap=%d rsvp=%d omrt=%d (1=hooked-or-already-covered)",
                 (void *)vtbl, g_immediate_vtbl, ok_vsset, ok_vscb, ok_map, ok_unmap, ok_rsvp, ok_omrt);
        (void)vsset_o;
        (void)vscb_o;
        (void)map_o;
        (void)unmap_o;
        (void)rsvp_o;
        (void)omrt_o;
    }
}

/* Live SKIPCL visual probe (Task 5 addendum 3) - reuses cbdump.c's
 * probe.txt live-control pattern (a small file whose mere existence
 * toggles behavior, re-checked periodically rather than every call).
 * Re-checks skipcl.txt's existence every SEQ_SKIPCL_RECHECK_CALLS
 * ExecuteCommandList calls (a cheap GetFileAttributesW, not an open),
 * caching the result between checks. Must work regardless of seqdump's
 * own armed/capture state - the skip behavior itself is a standalone
 * live visual toggle for whenever TEWVR_SEQDUMP=1, not gated on capture
 * being active (only the SKIPPED *log line* is gated on that, per the
 * addendum: "respect the 40k cap by logging these only while capture
 * active"). */
static int seq_skipcl_should_skip(void) {
    wchar_t path[MAX_PATH];

    g_skipcl_call_count++;
    if (g_skipcl_call_count != 1 && (g_skipcl_call_count % SEQ_SKIPCL_RECHECK_CALLS) != 0) {
        return g_skipcl_active;
    }

    skipcl_path(path, MAX_PATH);
    g_skipcl_active = (path[0] != L'\0' && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES);
    return g_skipcl_active;
}

/* ---- detours ---- */

static HRESULT STDMETHODCALLTYPE Hook_Map(ID3D11DeviceContext *ctx, ID3D11Resource *res, UINT sub,
                                           D3D11_MAP mapType, UINT flags,
                                           D3D11_MAPPED_SUBRESOURCE *mapped) {
    Map_t orig = (Map_t)seq_resolve_orig(ctx, VTBL_CTX_MAP);
    HRESULT hr;

    if (orig == NULL) {
        /* Should be unreachable even after seq_resolve_orig()'s fallback
         * (every address we ever pass to MH_CreateHook for this detour
         * is registered before it can be reached - see
         * hook_or_reuse_locked_guard()'s doc comment). Map has a return
         * value + out-param the caller depends on; we cannot safely
         * fabricate a mapped pointer, so the fail-safe options here are:
         * a generic failure HRESULT (rather than guessing which original
         * to call), AND zeroing *mapped first so a caller that does NOT
         * check the HRESULT (common in practice) sees NULL pData instead
         * of uninitialized stack garbage it might dereference as if it
         * were a real mapped pointer. */
        if (seq_rate_limit_should_fire(&g_bug_log_count)) {
            log_msg("seqdump: BUG - no Map original for ctx=%p vtbl=%p (even after fallback); "
                     "call dropped", (void *)ctx, *(void ***)ctx);
        }
        if (mapped != NULL) {
            memset(mapped, 0, sizeof(*mapped));
        }
        return E_FAIL;
    }

    hr = orig(ctx, res, sub, mapType, flags, mapped);

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
    Unmap_t orig;

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

    orig = (Unmap_t)seq_resolve_orig(ctx, VTBL_CTX_UNMAP);

    if (orig != NULL) {
        orig(ctx, res, sub);
    } else if (seq_rate_limit_should_fire(&g_bug_log_count)) {
        log_msg("seqdump: BUG - no Unmap original for ctx=%p vtbl=%p (even after fallback); "
                 "call dropped", (void *)ctx, *(void ***)ctx);
    }
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
    VSSetCB_t orig;

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

    orig = (VSSetCB_t)seq_resolve_orig(ctx, VTBL_CTX_VSSETCONSTANTBUFFERS);

    if (orig != NULL) {
        orig(ctx, startSlot, numBuffers, ppCB);
    } else if (seq_rate_limit_should_fire(&g_bug_log_count)) {
        log_msg("seqdump: BUG - no VSSetConstantBuffers original for ctx=%p vtbl=%p "
                 "(even after fallback); call dropped", (void *)ctx, *(void ***)ctx);
    }
}

static void STDMETHODCALLTYPE Hook_VSSetShader(ID3D11DeviceContext *ctx, ID3D11VertexShader *vs,
                                                ID3D11ClassInstance *const *inst, UINT n) {
    VSSetShader_t orig;

    if (seq_active()) {
        uint64_t hash = shaderdump_hash_for_shader(vs);
        if (hash != 0) {
            seq_line(ctx, 0, "VSSETSHADER ptr=0x%llX hash=%016llX",
                      (unsigned long long)(uintptr_t)vs, (unsigned long long)hash);
        } else {
            seq_line(ctx, 0, "VSSETSHADER ptr=0x%llX hash=?", (unsigned long long)(uintptr_t)vs);
        }
    }

    orig = (VSSetShader_t)seq_resolve_orig(ctx, VTBL_CTX_VSSETSHADER);

    if (orig != NULL) {
        orig(ctx, vs, inst, n);
    } else if (seq_rate_limit_should_fire(&g_bug_log_count)) {
        log_msg("seqdump: BUG - no VSSetShader original for ctx=%p vtbl=%p (even after fallback); "
                 "call dropped", (void *)ctx, *(void ***)ctx);
    }
}

static void STDMETHODCALLTYPE Hook_RSSetViewports(ID3D11DeviceContext *ctx, UINT numViewports,
                                                    const D3D11_VIEWPORT *viewports) {
    RSSetViewports_t orig;

    if (seq_active()) {
        if (numViewports > 0 && viewports != NULL) {
            seq_line(ctx, 0, "RSSETVP num=%u vp0=%dx%d@%d,%d", numViewports,
                      (int)viewports[0].Width, (int)viewports[0].Height,
                      (int)viewports[0].TopLeftX, (int)viewports[0].TopLeftY);
        } else {
            seq_line(ctx, 0, "RSSETVP num=%u vp0=?", numViewports);
        }
    }

    orig = (RSSetViewports_t)seq_resolve_orig(ctx, VTBL_CTX_RSSETVIEWPORTS);

    if (orig != NULL) {
        orig(ctx, numViewports, viewports);
    } else if (seq_rate_limit_should_fire(&g_bug_log_count)) {
        log_msg("seqdump: BUG - no RSSetViewports original for ctx=%p vtbl=%p (even after "
                 "fallback); call dropped", (void *)ctx, *(void ***)ctx);
    }
}

static void STDMETHODCALLTYPE Hook_OMSetRenderTargets(ID3D11DeviceContext *ctx, UINT numViews,
                                                        ID3D11RenderTargetView *const *rtvs,
                                                        ID3D11DepthStencilView *dsv) {
    OMSetRenderTargets_t orig;

    if (seq_active()) {
        ID3D11RenderTargetView *rtv0 = (numViews > 0 && rtvs != NULL) ? rtvs[0] : NULL;
        ID3D11Resource *rtres = NULL;
        ID3D11Resource *dsres = NULL;
        char rt0buf[32];
        char dsbuf[32];

        if (rtv0 != NULL) {
            ID3D11RenderTargetView_GetResource(rtv0, &rtres);
        }
        describe_texture2d(rtres, rt0buf, sizeof(rt0buf));
        if (rtres != NULL) {
            ID3D11Resource_Release(rtres);
        }

        if (dsv != NULL) {
            ID3D11DepthStencilView_GetResource(dsv, &dsres);
        }
        describe_texture2d(dsres, dsbuf, sizeof(dsbuf));
        if (dsres != NULL) {
            ID3D11Resource_Release(dsres);
        }

        seq_line(ctx, 0, "OMSETRT nrt=%u rtv0=0x%llX dsv=0x%llX rt0=%s ds=%s", numViews,
                  (unsigned long long)(uintptr_t)rtv0, (unsigned long long)(uintptr_t)dsv, rt0buf, dsbuf);
    }

    orig = (OMSetRenderTargets_t)seq_resolve_orig(ctx, VTBL_CTX_OMSETRENDERTARGETS);

    if (orig != NULL) {
        orig(ctx, numViews, rtvs, dsv);
    } else if (seq_rate_limit_should_fire(&g_bug_log_count)) {
        log_msg("seqdump: BUG - no OMSetRenderTargets original for ctx=%p vtbl=%p (even after "
                 "fallback); call dropped", (void *)ctx, *(void ***)ctx);
    }
}

static void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext *ctx, UINT idxCount, UINT startIdx,
                                                INT baseVtx) {
    seq_maybe_late_hook_deferred_ctx(ctx);
    if (seq_active()) {
        seq_line(ctx, 0, "DRAW variant=DrawIndexed count=%u", idxCount);
    }
    g_drawindexed_orig(ctx, idxCount, startIdx, baseVtx);
}

static void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext *ctx, UINT vtxCount, UINT startVtx) {
    seq_maybe_late_hook_deferred_ctx(ctx);
    if (seq_active()) {
        seq_line(ctx, 0, "DRAW variant=Draw count=%u", vtxCount);
    }
    g_draw_orig(ctx, vtxCount, startVtx);
}

static void STDMETHODCALLTYPE Hook_DrawIndexedInst(ID3D11DeviceContext *ctx, UINT idxPerInst,
                                                    UINT instCount, UINT startIdx, INT baseVtx,
                                                    UINT startInst) {
    seq_maybe_late_hook_deferred_ctx(ctx);
    if (seq_active()) {
        seq_line(ctx, 0, "DRAW variant=DrawIndexedInstanced count=%u instances=%u", idxPerInst, instCount);
    }
    g_drawindexedinst_orig(ctx, idxPerInst, instCount, startIdx, baseVtx, startInst);
}

static void STDMETHODCALLTYPE Hook_DrawInst(ID3D11DeviceContext *ctx, UINT vtxPerInst, UINT instCount,
                                             UINT startVtx, UINT startInst) {
    seq_maybe_late_hook_deferred_ctx(ctx);
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
 * ids alone. Addendum 3 additionally uses this hook (and the Draw* hooks
 * above) as the discovery point for late-hooking deferred-context
 * vtables - see seq_maybe_late_hook_deferred_ctx(). */
static HRESULT STDMETHODCALLTYPE Hook_FinishCommandList(ID3D11DeviceContext *ctx,
                                                          WINBOOL restoreDeferredContextState,
                                                          ID3D11CommandList **ppCommandList) {
    HRESULT hr;

    seq_maybe_late_hook_deferred_ctx(ctx);

    hr = g_finishcmdlist_orig(ctx, restoreDeferredContextState, ppCommandList);
    if (seq_active()) {
        ID3D11CommandList *cl = (ppCommandList != NULL) ? *ppCommandList : NULL;
        seq_line(ctx, 0, "FINISHCMDLIST restore=%d hr=0x%08lX cmdlist=0x%llX",
                  (int)restoreDeferredContextState, (unsigned long)hr,
                  (unsigned long long)(uintptr_t)cl);
    }
    return hr;
}

/* Task 5 addendum 3: TEWVR_SKIPCL live visual probe. While
 * %LOCALAPPDATA%\TEWVR\skipcl.txt exists, this hook does NOT call through
 * to the real ExecuteCommandList - the recorded command list (typically a
 * chunk of world geometry, per the controller's gameplay analysis) is
 * simply dropped every frame, letting a human watching the game toggle
 * whole render passes on/off live by creating/deleting one file. */
static void STDMETHODCALLTYPE Hook_ExecuteCommandList(ID3D11DeviceContext *ctx,
                                                        ID3D11CommandList *commandList,
                                                        WINBOOL restoreContextState) {
    if (seq_skipcl_should_skip()) {
        if (seq_active()) {
            seq_line(ctx, 0, "EXECUTECMDLIST SKIPPED cmdlist=0x%llX restore=%d",
                      (unsigned long long)(uintptr_t)commandList, (int)restoreContextState);
        }
        return; /* do NOT call through - this IS the skip */
    }

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
    int ok_finishcl, ok_executecl, ok_rsvp, ok_omrt;
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
    g_immediate_vtbl = (void *)vtbl;

    /* VSSetShader/VSSetConstantBuffers/Map/Unmap/RSSetViewports/
     * OMSetRenderTargets (Task 5 addendum 3): hooked here on the
     * IMMEDIATE vtable via the same address-keyed hook_or_reuse_locked_
     * guard() seq_maybe_late_hook_deferred_ctx() uses for any deferred
     * vtable it discovers later - each of the six detours looks up its
     * original by the address at the CALLING ctx's own vtable slot (see
     * g_hooked_funcs's doc comment for why address-keyed, not vtable-
     * pointer-keyed, dispatch is required here). */
    {
        VSSetShader_t vsset_o = NULL;
        VSSetCB_t vscb_o = NULL;
        Map_t map_o = NULL;
        Unmap_t unmap_o = NULL;
        RSSetViewports_t rsvp_o = NULL;
        OMSetRenderTargets_t omrt_o = NULL;

        ok_map = hook_or_reuse_locked_guard(vtbl[VTBL_CTX_MAP], (void *)&Hook_Map, (void **)&map_o,
                                             "seqdump ID3D11DeviceContext::Map");
        ok_unmap = hook_or_reuse_locked_guard(vtbl[VTBL_CTX_UNMAP], (void *)&Hook_Unmap, (void **)&unmap_o,
                                               "seqdump ID3D11DeviceContext::Unmap");
        ok_vscb = hook_or_reuse_locked_guard(vtbl[VTBL_CTX_VSSETCONSTANTBUFFERS], (void *)&Hook_VSSetCB,
                                              (void **)&vscb_o,
                                              "seqdump ID3D11DeviceContext::VSSetConstantBuffers");
        ok_vsset = hook_or_reuse_locked_guard(vtbl[VTBL_CTX_VSSETSHADER], (void *)&Hook_VSSetShader,
                                               (void **)&vsset_o, "seqdump ID3D11DeviceContext::VSSetShader");
        ok_rsvp = hook_or_reuse_locked_guard(vtbl[VTBL_CTX_RSSETVIEWPORTS], (void *)&Hook_RSSetViewports,
                                              (void **)&rsvp_o, "seqdump ID3D11DeviceContext::RSSetViewports");
        ok_omrt = hook_or_reuse_locked_guard(vtbl[VTBL_CTX_OMSETRENDERTARGETS], (void *)&Hook_OMSetRenderTargets,
                                              (void **)&omrt_o, "seqdump ID3D11DeviceContext::OMSetRenderTargets");
        (void)vsset_o;
        (void)vscb_o;
        (void)map_o;
        (void)unmap_o;
        (void)rsvp_o;
        (void)omrt_o;
    }

    ok_update = mh_glue_create_and_enable(vtbl[VTBL_CTX_UPDATESUBRESOURCE], (void *)&Hook_UpdateSubresource,
                                           (void **)&g_update_orig,
                                           "seqdump ID3D11DeviceContext::UpdateSubresource");
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
                     ok_di || ok_d || ok_dii || ok_dinst || ok_finishcl || ok_executecl ||
                     ok_rsvp || ok_omrt;

    log_msg("seqdump: hooks map=%d unmap=%d update=%d vscb=%d vsset=%d di=%d d=%d dii=%d dinst=%d "
             "vscb1=%d finishcl=%d executecl=%d rsvp=%d omrt=%d -> %s (arm=%s, caps at %d events)",
             ok_map, ok_unmap, ok_update, ok_vscb, ok_vsset, ok_di, ok_d, ok_dii, ok_dinst, ok_vscb1,
             ok_finishcl, ok_executecl, ok_rsvp, ok_omrt,
             g_seq_hooks_ok ? "ACTIVE" : "inactive",
             g_seq_armfile_mode ? "seqarm.txt file-trigger" : "frame-301 auto",
             SEQDUMP_MAX_EVENTS);

    if (g_seq_hooks_ok && g_seq_fp != NULL) {
        fprintf(g_seq_fp, "TEWVR seqdump session start (TEWVR_SEQDUMP=1%s); hooks: map=%d unmap=%d "
                 "update=%d vscb=%d vsset=%d di=%d d=%d dii=%d dinst=%d vscb1=%d finishcl=%d executecl=%d "
                 "rsvp=%d omrt=%d\n",
                 g_seq_armfile_mode ? ", TEWVR_SEQDUMP_ARMFILE=1 (arm via seqarm.txt)" : "",
                 ok_map, ok_unmap, ok_update, ok_vscb, ok_vsset, ok_di, ok_d, ok_dii, ok_dinst, ok_vscb1,
                 ok_finishcl, ok_executecl, ok_rsvp, ok_omrt);
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

void seqdump_clear_stale_skipcl(void) {
    wchar_t path[MAX_PATH];

    skipcl_path(path, MAX_PATH);
    if (path[0] == L'\0') {
        return;
    }

    if (!DeleteFileW(path)) {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
            log_msg("seqdump: failed to delete stale skipcl.txt (gle=%lu)", (unsigned long)err);
        }
    } else {
        log_msg("seqdump: deleted stale skipcl.txt left over from a previous session");
    }
}
