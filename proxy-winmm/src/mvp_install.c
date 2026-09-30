/* Split out of mvp_patch.c on 2026-09-30 (move-only; see mvp_patch.c for the
 * module overview). Hook installation and teardown: hook_one(), mvp_patch_install() and
 * mvp_patch_remove(). */

#include "mvp_patch_internal.h"

/* ---- install-time hook bookkeeping ---- */

/* 5 hook_one() call sites in this file - DrawIndexed, Draw, CreateBuffer, Map,
 * Unmap - against a table of 4. The fifth was refused on every run, and it was
 * always Unmap, because Unmap is hooked last:
 *
 *   mvp_patch: internal hooked-function table full (4); refusing to enable
 *              mvp_patch ID3D11DeviceContext::Unmap
 *
 * That is the whole reason the DYNAMIC cb0 path reported maps>0 / unmaps==0 and
 * let the (context,resource) map table overflow without bound: Map records an
 * entry, Unmap frees it, and Unmap was never installed. It is NOT the
 * deferred-context vtable flavour and NOT a legitimately no-op Unmap - the two
 * standing hypotheses until the log was actually read on 2026-09-08. It also
 * means the 2026-09-05 (context,resource) re-keying had never once been
 * exercised.
 *
 * Headroom rather than exactly 5: an entry is two pointers of per-module
 * bookkeeping, and being one short cost a fortnight of wrong diagnoses. */
#define MVP_MAX_HOOKED_FUNCS 8
struct HookedFunc {
    void *addr;
    void *orig;
};
static struct HookedFunc g_hooked[MVP_MAX_HOOKED_FUNCS];
static int g_hooked_count = 0;

DrawIndexed_t g_drawindexed_orig = NULL;
Draw_t g_draw_orig = NULL;
CreateBuffer_t g_createbuffer_orig = NULL;
int g_installed = 0;

/* ================= install-time hook helper ================= */

/* Create -> publish -> enable, same ordering GOAL as Task 5 addendum 3's
 * g_hooked_funcs table in seqdump.c, applied here to BOTH this module's
 * own tiny bookkeeping table AND (unlike the first pass of this file) the
 * caller's static "original" variable itself: `*out_orig` is written
 * BEFORE MH_EnableHook() runs, not after hook_one() returns - closing the
 * same "hook live but its original not yet discoverable" window
 * minhook_glue.h's own split-create/enable contract warns about (a plain
 * static write is fine as long as it happens-before the enable, which the
 * FIRST version of this function did not guarantee: it returned `orig` via
 * `*out_orig` only at the very end, after mh_glue_enable() had already
 * made the target live). Unlike seqdump's table, this one does not need
 * per-vtable late-hooking or a lock on the hot path: DrawIndexed/Draw
 * share ONE underlying code address between the immediate and every
 * deferred-context vtable flavor (seqdump.c's own confirmed finding), and
 * CreateBuffer is a device (not per-context) method, so hooking each ONCE,
 * here, covers every ctx/device flavor for the rest of the process's
 * lifetime. This function only ever runs from mvp_patch_install(), once,
 * single-threaded, on the bootstrap thread - strictly before the game's
 * own render loop can start, so g_hooked[] itself needs no lock either.
 *
 * Important finding #7: if `target` is ALREADY hooked (MH_ERROR_ALREADY_
 * CREATED - almost certainly TEWVR_SEQDUMP=1 or TEWVR_SHADERDUMP=1, both of
 * which hook this same Draw/DrawIndexed address first), this logs an
 * explicit, named conflict rather than just the generic MinHook status
 * string - so a human debugging "the world didn't rotate" by turning on
 * one of those diagnostic flags for more visibility sees exactly why
 * mvp_patch went silent, instead of two unrelated-looking log lines. */
static int hook_one(void *target, void *detour, void **out_orig, const char *name) {
    void *orig = NULL;
    MH_STATUS st;

    st = MH_CreateHook(target, detour, &orig);
    if (st != MH_OK) {
        if (st == MH_ERROR_ALREADY_CREATED) {
            log_msg("mvp_patch: CONFLICT - %s is ALREADY HOOKED by another module before mvp_patch "
                     "could hook it (almost certainly TEWVR_SEQDUMP=1 or TEWVR_SHADERDUMP=1 - both "
                     "hook this exact Draw/DrawIndexed address first). mvp_patch's real per-draw MVP "
                     "override is DISABLED for this entire session as a result. If you are trying to "
                     "see the world rotate, unset those TEWVR_* diagnostic flags and relaunch.",
                     name);
        } else {
            log_msg("mvp_patch: MH_CreateHook(%s) failed: %s", name, MH_StatusToString(st));
        }
        return 0;
    }

    if (g_hooked_count >= MVP_MAX_HOOKED_FUNCS) {
        MH_RemoveHook(target);
        log_msg("mvp_patch: internal hooked-function table full (%d); refusing to enable %s",
                 MVP_MAX_HOOKED_FUNCS, name);
        return 0;
    }
    g_hooked[g_hooked_count].addr = target;
    g_hooked[g_hooked_count].orig = orig;
    g_hooked_count++;

    /* Publish to the caller's static BEFORE enabling - see this function's
     * own comment above. */
    *out_orig = orig;

    if (!mh_glue_enable(target, name)) {
        g_hooked_count--;
        *out_orig = NULL;
        return 0;
    }

    return 1;
}

/* ================= public API ================= */

void mvp_patch_install(ID3D11Device *dummy_dev, ID3D11DeviceContext *dummy_ctx) {
    void **ctx_vtbl;
    void **dev_vtbl;
    int ok_di, ok_d, ok_cb, ok_us;

    if (g_installed) {
        return;
    }

    if (dummy_dev == NULL || dummy_ctx == NULL) {
        log_msg("mvp_patch: install called with a NULL dummy device/context; per-draw MVP override DISABLED this session");
        return;
    }

    if (!mh_glue_init()) {
        log_msg("mvp_patch: MinHook init failed; per-draw MVP override DISABLED this session");
        return;
    }

    InitializeCriticalSection(&g_direct_pool_cs);
    g_direct_pool_cs_ready = 1;
    InitializeCriticalSection(&g_scratch_init_cs);
    g_scratch_init_cs_ready = 1;
    InitializeCriticalSection(&g_latehook_cs);
    g_latehook_cs_ready = 1;

    g_tls_index = TlsAlloc();
    if (g_tls_index == TLS_OUT_OF_INDEXES) {
        log_msg("mvp_patch: TlsAlloc failed (gle=%lu); per-draw MVP override DISABLED this session",
                 (unsigned long)GetLastError());
        DeleteCriticalSection(&g_direct_pool_cs);
        g_direct_pool_cs_ready = 0;
        DeleteCriticalSection(&g_scratch_init_cs);
        g_scratch_init_cs_ready = 0;
        DeleteCriticalSection(&g_latehook_cs);
        g_latehook_cs_ready = 0;
        return;
    }

    ctx_vtbl = *(void ***)dummy_ctx;
    dev_vtbl = *(void ***)dummy_dev;

    ok_di = hook_one(ctx_vtbl[VTBL_CTX_DRAWINDEXED], (void *)&Hook_DrawIndexed, (void **)&g_drawindexed_orig,
                      "mvp_patch ID3D11DeviceContext::DrawIndexed");
    ok_d = hook_one(ctx_vtbl[VTBL_CTX_DRAW], (void *)&Hook_Draw, (void **)&g_draw_orig,
                     "mvp_patch ID3D11DeviceContext::Draw");
    ok_cb = hook_one(dev_vtbl[VTBL_DEV_CREATEBUFFER], (void *)&Hook_CreateBuffer, (void **)&g_createbuffer_orig,
                      "mvp_patch ID3D11Device::CreateBuffer");

    /* 2026-09-04c: Map/Unmap, the shadow source for the per-shader DYNAMIC cb0
     * pool. Vtable slots 14/15, read from the SDK header's own
     * ID3D11DeviceContextVtbl rather than assumed. Not gated below: if these
     * fail the DEFAULT path is unaffected and the dynamic pool simply never
     * becomes valid, which its own counters report. */
    if (!hook_one(ctx_vtbl[VTBL_CTX_MAP], (void *)&Hook_Map, (void **)&g_map_orig,
                  "mvp_patch ID3D11DeviceContext::Map") ||
        !hook_one(ctx_vtbl[VTBL_CTX_UNMAP], (void *)&Hook_Unmap, (void **)&g_unmap_orig,
                  "mvp_patch ID3D11DeviceContext::Unmap")) {
        log_msg("mvp_patch: Map/Unmap hook failed - the per-shader DYNAMIC cb0 path is INERT this "
                "run (those draws render unpatched, exactly as before 2026-09-04c). The DEFAULT "
                "world-buffer path is unaffected.");
    }

    /* Fix round 4: the immediate/dummy flavor's UpdateSubresource. Deferred
     * contexts get theirs late-hooked on their first draw (see
     * mvp_maybe_late_hook_ctx) - this one covers the immediate context and
     * any deferred flavor that happens to share its address. */
    EnterCriticalSection(&g_latehook_cs);
    ok_us = mvp_hook_update_target_locked(ctx_vtbl[VTBL_CTX_UPDATESUBRESOURCE]);
    LeaveCriticalSection(&g_latehook_cs);

    /* FIX ROUND 5, finding #2: this gate's polarity used to be backwards.
     * It required ok_cb and ignored ok_us - but after round 4b,
     * Hook_CreateBuffer registers NOTHING (it is pure diagnostics), while
     * UpdateSubresource is the mechanism's ACTUAL hard dependency: without
     * it no shadow ever becomes valid, so no draw is ever patchable and the
     * module would log "installed" and then sit silently inert forever -
     * precisely the failure mode this gate exists to make loud.
     *
     * Hard requirements now: at least one draw hook (nothing to patch
     * without one) AND UpdateSubresource (no live content without it).
     * CreateBuffer is downgraded to a soft warning - losing it costs only
     * the coverage diagnostics, not the mechanism. */
    if (!ok_cb) {
        log_msg("mvp_patch: WARNING - could not hook ID3D11Device::CreateBuffer. The per-draw MVP "
                 "override does NOT depend on it (buffers are registered from the draw path since "
                 "fix round 4b); only the CreateBuffer-coverage diagnostics are lost. Continuing.");
    }

    if ((!ok_di && !ok_d) || !ok_us) {
        log_msg("mvp_patch: failed to hook enough of {DrawIndexed=%d Draw=%d UpdateSubresource=%d} "
                 "(CreateBuffer=%d, not required); per-draw MVP override DISABLED this session",
                 ok_di, ok_d, ok_us, ok_cb);
        TlsFree(g_tls_index);
        g_tls_index = TLS_OUT_OF_INDEXES;
        DeleteCriticalSection(&g_direct_pool_cs);
        g_direct_pool_cs_ready = 0;
        DeleteCriticalSection(&g_scratch_init_cs);
        g_scratch_init_cs_ready = 0;
        DeleteCriticalSection(&g_latehook_cs);
        g_latehook_cs_ready = 0;
        return;
    }

    /* The test matrix K (TEST_YAW / TEST_ROLL / TEST_SHIFT, test_k.c): read
     * once here, synchronously, on the bootstrap thread - strictly before
     * the hooks just enabled above can possibly be reached by a real draw. */
    g_K = test_k_from_config();

    g_installed = 1;
    log_msg("mvp_patch: installed (DrawIndexed=%d Draw=%d UpdateSubresource=%d; CreateBuffer=%d, "
             "diagnostics only); world cb0 buffers (%d-%d bytes, DEFAULT/no-CPU-access) are "
             "registered FROM THE DRAW PATH the first time one is seen bound at VS slot 0 for a "
             "draw with a usable mvpmatrix, and their content is shadowed on every "
             "UpdateSubresource; deferred contexts get their own UpdateSubresource late-hooked on "
             "their first draw; scratch pool created lazily on the first patchable draw",
             ok_di, ok_d, ok_us, ok_cb, MVP_POOL_BUF_MIN_BYTES, MVP_POOL_BUF_MAX_BYTES);
}

/* ORDERING REQUIREMENT (fix round 5, minor): mh_glue_shutdown() MUST have
 * disabled this module's hooks before this runs. Nothing here is safe
 * against a concurrently-live detour - it releases the pool's buffer
 * references and deletes the critical sections a live Hook_UpdateSubresource
 * or Hook_DrawIndexed would still be touching. hooks_remove() already calls
 * mh_glue_shutdown() first, and documents why; this comment records the
 * dependency on this side too, since it was previously implicit. */
void mvp_patch_remove(void) {
    int i;

    /* 2026-09-04: the bucketed miss table one last time, so a session's
     * whole picture is in the log however the periodic 30-second cadence
     * happened to line up with the quit. Runs before any trampoline is
     * torn down (hooks_remove() calls mh_glue_shutdown() first, and this
     * after it), so no draw can be mid-sample here. */
    if (g_miss_combo_count > 0) {
        log_msg("mvp_patch: DIAG final bucketed pool-miss table at shutdown (cumulative, 1-in-%d sampled):",
                MVP_MISS_SAMPLE_MASK + 1);
        mvp_diag_report_misses();
    }

    if (g_direct_pool_cs_ready) {
        EnterCriticalSection(&g_direct_pool_cs);
        /* The DYNAMIC pool holds its own AddRef per slot, exactly like the
         * DEFAULT one, and lives in the upper partition of the same array. */
        for (i = 0; i < g_dyn_pool_count; i++) {
            int slot = MVP_DIRECT_POOL_MAX + i;
            if (g_direct_pool[slot].buf != NULL) {
                ID3D11Buffer_Release(g_direct_pool[slot].buf);
                g_direct_pool[slot].buf = NULL;
                InterlockedExchange(&g_shadow_valid[slot], 0);
            }
        }
        g_dyn_pool_count = 0;
        for (i = 0; i < g_direct_pool_count; i++) {
            if (g_direct_pool[i].buf != NULL) {
                /* Fix round 4: nothing to Unmap any more - this module no
                 * longer Maps the engine's buffers at all, it shadows their
                 * UpdateSubresource writes into its own memory. */
                ID3D11Buffer_Release(g_direct_pool[i].buf);
                g_direct_pool[i].buf = NULL;
                InterlockedExchange(&g_shadow_valid[i], 0);
            }
        }
        g_direct_pool_count = 0;
        LeaveCriticalSection(&g_direct_pool_cs);
        DeleteCriticalSection(&g_direct_pool_cs);
        g_direct_pool_cs_ready = 0;
    }

    if (g_scratch_ready) {
        for (i = 0; i < MVP_SCRATCH_TOTAL; i++) {
            if (g_scratch[i] != NULL) {
                ID3D11Buffer_Release(g_scratch[i]);
                g_scratch[i] = NULL;
            }
        }
        InterlockedExchange(&g_scratch_ready, 0);
    }
    if (g_scratch_init_cs_ready) {
        DeleteCriticalSection(&g_scratch_init_cs);
        g_scratch_init_cs_ready = 0;
    }
    if (g_latehook_cs_ready) {
        /* Cleared before the CS goes away so no in-flight detour can start
         * a late-hook pass against a deleted critical section. */
        g_latehook_cs_ready = 0;
        DeleteCriticalSection(&g_latehook_cs);
    }
    g_update_target_count = 0;
    g_seen_ctx_count = 0;
    g_seen_ctx_full_warned = 0;

    if (g_tls_index != TLS_OUT_OF_INDEXES) {
        TlsFree(g_tls_index);
        g_tls_index = TLS_OUT_OF_INDEXES;
    }

    g_drawindexed_orig = NULL;
    g_draw_orig = NULL;
    g_createbuffer_orig = NULL;
    g_hooked_count = 0;
    g_direct_pool_full_warned = 0;
    g_installed = 0;
}
