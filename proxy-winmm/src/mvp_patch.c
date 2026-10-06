#include "mvp_patch_internal.h"


/*
 * ---- Step 0: chosen read mechanism, and why (REVISED after review) ----
 *
 * The brief laid out three candidates for reading a draw's bound VS-slot0
 * cb0 content without a per-draw GPU stall or a deferred-context/worker-
 * thread threading violation: (a) capture the pool buffers' CPU-writable
 * pointer via a fresh Map, if one ever fires after our hooks are live; (b)
 * hook ID3D11Device::CreateBuffer and Map the pool buffers ourselves at
 * creation time, mirroring the engine's own persistent-map pattern; (c) a
 * batched once-per-frame staging-copy snapshot on the immediate context.
 *
 * This implementer FIRST chose (c) - reasoning that (a)/(b) both hinge on
 * live, gameplay-only behaviour this task's constraints (no driving/
 * watching a real play session) could not verify. That reasoning about
 * verifiability was sound, but (c) itself was WRONG: Task 5's discovery
 * found ~1900 draws/frame sharing ~6 deferred pool buffer identities (each
 * 96-224B of actual shader-visible content within a larger physical
 * buffer) - roughly 300 draws per identity per frame. A single
 * once-per-frame snapshot per identity means all ~300 of those draws would
 * have been patched with the SAME stale content - not "one frame of
 * staleness" (which correctly describes a per-OBJECT value lagging by one
 * frame) but "one value substituted for ~300 different objects' actual
 * per-draw data," corrupting every OTHER per-object constant living in
 * that same cb0 window too (material params etc., not just MVP). Caught in
 * review before any live test masked it as "the world didn't rotate."
 *
 * ============================================================
 * FIX ROUND 4 (2026-08-21): THE CHOICE BELOW IS SUPERSEDED.
 *
 * Option (b) - Map the world cb0 buffer once at creation and keep the CPU
 * pointer forever - is IMPOSSIBLE for the real target buffers. Measured
 * during a visually-confirmed gameplay session, the buffers actually bound
 * at VS slot 0 for world draws are:
 *
 *     ByteWidth=1920  Usage=D3D11_USAGE_DEFAULT  CPUAccessFlags=0
 *
 * A DEFAULT / no-CPU-access resource cannot be Mapped at all. The 1920-byte
 * DYNAMIC/CPU_WRITE buffers the filter described below was successfully
 * capturing are a same-size DECOY set, never bound at slot 0 for a world
 * draw (their pointers are disjoint from the five 1920-byte slot-0 pointers
 * in task6-gameplay-seqdump.log).
 *
 * CURRENT MECHANISM - call it option (d), UpdateSubresource shadowing:
 * UpdateSubresource is the only CPU write path for a DEFAULT buffer, so
 * that call is where the live MVP content is observable.
 * Hook_UpdateSubresource copies the caller's source bytes into our own
 * per-buffer CPU shadow before calling through, and the draw path reads the
 * shadow exactly where it used to read the mapped pointer. Everything
 * downstream (row extraction, K*mvp, scratch buffer, rebind) is unchanged.
 * Buffers are registered from the DRAW path, not from CreateBuffer, because
 * a descriptor alone cannot distinguish the world pool from same-shaped
 * decoys. See the two large fix-round-4 blocks further down this file for
 * the full evidence, and Fix round 4 in task-6-report.md.
 *
 * Also retired by this change: the "the engine's own later Map could
 * conflict with ours" residual risk. This module no longer Maps the
 * engine's buffers at all.
 *
 * The historical reasoning below is retained because it explains why the
 * earlier mechanisms were chosen and how each was disproved.
 * ============================================================
 *
 * SUPERSEDED CHOICE: (b). A CreateBuffer hook (VTBL_DEV_CREATEBUFFER=3, cross-
 * checked against shaderdump.c's own already-verified ID3D11Device vtable
 * count) filters for buffers matching the world-pool's known profile
 * (D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE,
 * ByteWidth in [MVP_POOL_BUF_MIN_BYTES, MVP_POOL_BUF_MAX_BYTES] - wide
 * enough to catch the ~1920B pool buffers observed in Task 6 discovery,
 * narrow enough to exclude the small 96-224B per-shader cb0 buffers the
 * engine explicitly Maps/Unmaps every draw, which this mechanism must NOT
 * touch - see below). On a match, mvp_direct_pool_try_capture() gets the
 * buffer's own device's immediate context (via
 * ID3D11Device::GetImmediateContext - available directly from the `dev`
 * parameter Hook_CreateBuffer already has, with no dependency on this
 * module having captured the game's real device via Present first: our
 * hook fires synchronously, inline with the CreateBuffer call itself,
 * strictly before the engine's own code can do anything else with the
 * freshly-created buffer - including its own Map, if it ever tries one),
 * issues our OWN Map(WRITE_DISCARD) immediately, and - on success - AddRefs
 * the buffer for a persistent reference and keeps the returned CPU pointer
 * forever (never Unmaps), mirroring the engine's own apparent pattern
 * (Task 6 discovery: these buffers are never observed being explicitly
 * Mapped/Unmapped by the engine at all - CPU content changes with no
 * Map/Unmap/UpdateSubresource event ever appearing in a full gameplay
 * capture, meaning whatever "map" IT establishes almost certainly also
 * happens once, near creation).
 *
 * This gives every patchable draw an EXACT, LIVE read of its actual
 * per-object cb0 content - no staleness, no shared-snapshot corruption,
 * and no per-draw or per-frame GPU work at all (the CPU pointer is plain
 * process memory once captured).
 *
 * Residual risk this implementer could NOT close by static reasoning alone
 * (explicitly flagged by the brief for option (b) - "verify the engine
 * tolerates a foreign Map... test carefully"): IF the engine's own
 * persistent-pointer setup for one of these buffers involves ITS OWN Map
 * call arriving AFTER ours (rather than obtaining its pointer some other
 * way - e.g. already having captured it before CreateBuffer even returns
 * to its caller, which our hook cannot precede), our foreign,
 * never-Unmapped Map would make that later engine Map call fail
 * (D3D11 forbids re-Map on an already-mapped subresource) - and if the
 * engine's own code does not check that HRESULT, it could write through
 * garbage/stale mapped-subresource state instead of the real buffer,
 * corrupting or crashing. Task 6 discovery's evidence (never observing an
 * explicit engine Map on these specific buffers, across a full gameplay
 * capture with our Map/Unmap hooks already live) argues AGAINST the
 * engine ever calling Map on them at all after creation, which would make
 * this risk moot - but that evidence predates this mechanism actually
 * mapping them first, so it is not a full proof. mvp_direct_pool_try_capture()
 * logs loudly (STEP0 line) on every successful capture and on every
 * foreign-Map failure, and mvp_patch_prepare() falls through to unpatched,
 * never crashes or corrupts state, for any buffer this mechanism did not
 * successfully capture - so a bounded menu smoke test can directly observe
 * whether CreateBuffer fires for matching candidates and whether the
 * foreign Map succeeds, without needing to reach real gameplay to validate
 * the MECHANISM itself (only the final yaw-rotation proof needs gameplay).
 *
 * Buffers NOT matching the filter (the small immediate-context cb0
 * buffers) are simply never captured here, so their draws always fall
 * through unpatched - this mechanism intentionally covers only the deferred
 * world-geometry draws the Step 3 keystone proof needs to rotate, per the
 * SKIPCL finding of what the deferred lists actually carry.
 *
 * ---- Threading: per-thread scratch-buffer rings ----
 *
 * Unchanged from the first pass - see mvp_alloc_scratch_index()'s own
 * comment: world draws are recorded from up to 6 deferred-context worker
 * threads concurrently plus the immediate/render thread; a single shared
 * scratch-buffer ring could let two threads' Map/memcpy/Unmap/Bind
 * sequences collide on the same ID3D11Buffer object. Each thread gets its
 * own private sub-ring instead, assigned once via a TLS slot.
 */

/* ---- test matrix K (TEWVR_TEST_YAW) ---- */

Mat4 g_K;

/* ================= the per-draw patch itself ================= */

/* Shared core for Hook_DrawIndexed/Hook_Draw. On success, patches VS slot 0
 * to a scratch buffer holding the K-rotated MVP (with everything else
 * about cb0 unchanged), fills `*out_orig_buf` with the buffer the caller
 * must rebind to slot 0 AFTER calling the original draw, and returns 1. On
 * ANY failure or "not safely patchable" condition, does nothing and
 * returns 0 - the caller then just calls the original draw unmodified.
 * Never blocks, retries, or delays the draw itself; never partially
 * applies a patch (VSSetConstantBuffers to the scratch buffer is the LAST
 * thing this function does, only after every prior step already
 * succeeded).
 *
 * Task 6 fix round 3: every exit point below increments exactly one
 * diagnostic counter (see the block above hook_one()) - purely for
 * observability, no behaviour change. mvp_diag_maybe_report() is called
 * once, unconditionally, near the top, so it runs regardless of which exit
 * this particular call takes. */
static int mvp_patch_prepare(ID3D11DeviceContext *ctx, ID3D11Buffer **out_orig_buf,
                             UINT draw_count, int draw_indexed) {
    ID3D11VertexShader *vs = NULL;
    const void *vs_key = NULL; /* pointer VALUE only, kept for the sampled miss path's hash lookup */
    ID3D11Buffer *buf = NULL;
    int offs[4];
    int shadow_slot;
    UINT byte_width;
    unsigned char local_copy[MVP_CB_BYTES];
    UINT copy_bytes;
    int scratch_index;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    Mat4 mvp, patched;
    int row;

    *out_orig_buf = NULL;

    mvp_diag_maybe_report();

    if (!g_installed) {
        InterlockedIncrement(&g_diag_not_installed);
        return 0;
    }

    ID3D11DeviceContext_VSGetShader(ctx, &vs, NULL, NULL);
    if (vs == NULL) {
        InterlockedIncrement(&g_diag_no_vs);
        return 0;
    }
    if (!mvp_row_offsets_for_shader(vs, offs)) {
        /* Diagnostic-only second lookup, only on this (already-failing)
         * path, to classify WHY - see mvptable.h's MvpShaderStatus. */
        switch (mvp_shader_status_for_shader(vs)) {
            case MVP_SHADER_NO_MVP:
                InterlockedIncrement(&g_diag_shader_no_mvp);
                break;
            case MVP_SHADER_ROWS_INCOMPLETE:
                InterlockedIncrement(&g_diag_shader_rows_incomplete);
                break;
            case MVP_SHADER_UNKNOWN:
            case MVP_SHADER_OK: /* shouldn't happen here (mvp_row_offsets_for_shader
                                    would have succeeded), but count it as
                                    "unknown" rather than silently drop it */
            default:
                InterlockedIncrement(&g_diag_shader_unknown);
                break;
        }
        ID3D11VertexShader_Release(vs);
        return 0; /* unknown shader, or an incomplete set of rows we refuse to guess at */
    }
    vs_key = vs;
    ID3D11VertexShader_Release(vs);

    ID3D11DeviceContext_VSGetConstantBuffers(ctx, 0, 1, &buf);
    if (buf == NULL) {
        InterlockedIncrement(&g_diag_no_slot0_buf);
        return 0; /* nothing bound at slot 0 */
    }

    shadow_slot = mvp_direct_pool_find(buf, &byte_width);
    if (shadow_slot < 0) {
        /* Not one of the persistently-mapped world cb0 buffers (e.g. a
         * small immediate-context cb0 - this mechanism intentionally does
         * not cover those, only the deferred world draws). Fail-safe skip,
         * same posture as an unknown shader. THE key counter for this
         * round's mystery: if this is the dominant skip reason during real
         * gameplay, the shader IS known/patchable but its bound buffer is
         * something other than what CreateBuffer captured - logged once,
         * in full, the first time it happens, so a re-round-trip isn't
         * needed to see what that buffer actually looks like. */
        UINT dyn_bw = 0;
        UINT tick;
        int dyn_slot = mvp_dyn_pool_find(buf, &dyn_bw);
        if (dyn_slot >= 0) {
            /* 2026-09-04c: a per-shader DYNAMIC cb0 whose Map/Unmap writes we
             * shadow. From here the path is IDENTICAL to the DEFAULT pool's -
             * the bounds check, the seqlocked read, the K multiply, the scratch
             * write and the rebind all follow below unchanged. */
            shadow_slot = dyn_slot;
            byte_width = dyn_bw;
            InterlockedIncrement(&g_diag_dyn_patched);
            goto have_shadow;
        }
        InterlockedIncrement(&g_diag_pool_miss);
        tick = (UINT)InterlockedIncrement(&g_miss_sample_tick);
        if ((tick & MVP_REGISTER_PROBE_MASK) == 0) {
            /* Fix round 4b: this is where world cb0 buffers get into the
             * pool - see mvp_try_register_bound_buffer(). */
            mvp_try_register_bound_buffer(buf);
            /* 2026-09-04c: and the per-shader DYNAMIC ones, which the DEFAULT
             * filter deliberately rejects. Same "register from the draw path,
             * never from the descriptor alone" rule. */
            mvp_try_register_bound_dynamic(buf);
        }
        if ((tick & MVP_MISS_SAMPLE_MASK) == 0) {
            /* fix round 4: what IS this buffer, and did our hook see it
             * created? 2026-09-04: and what is the DRAW - how big, which
             * shader - so the missed path can be told from world geometry.
             * The hash lookup is by pointer value in shaderdump's table,
             * which is exactly how mvp_row_offsets_for_shader() found this
             * shader a moment ago; the object itself is not touched. */
            mvp_miss_sample(buf, shaderdump_hash_for_shader(vs_key), draw_count, draw_indexed);
        }
        if (InterlockedCompareExchange(&g_pool_miss_first_logged, 1, 0) == 0) {
            D3D11_BUFFER_DESC bd;
            memset(&bd, 0, sizeof(bd));
            ID3D11Buffer_GetDesc(buf, &bd);
            log_msg("mvp_patch: DIAG first pool-miss: a KNOWN/patchable shader's bound slot0 "
                     "buf=%p is not (yet) in the world cb0 pool (pool currently holds %d "
                     "identities). That buffer's real desc: ByteWidth=%u Usage=%d BindFlags=0x%X "
                     "CPUAccessFlags=0x%X MiscFlags=0x%X - to be registered it must be "
                     "CONSTANT_BUFFER, Usage=DEFAULT(0), CPUAccessFlags=0 and %d-%d bytes "
                     "(checked from the draw path by mvp_try_register_bound_buffer, NOT at "
                     "CreateBuffer time). A miss here is expected and harmless for the small "
                     "per-shader cb0s (64/96/128/160/176/224/272B) this mechanism does not cover",
                     (void *)buf, g_direct_pool_count, bd.ByteWidth, (int)bd.Usage, bd.BindFlags,
                     bd.CPUAccessFlags, bd.MiscFlags, MVP_POOL_BUF_MIN_BYTES, MVP_POOL_BUF_MAX_BYTES);
        }
        ID3D11Buffer_Release(buf);
        return 0;
    }

have_shadow:
    /* Important finding #4 fix: bound-check the shader's reflected row
     * offsets against the REAL bound buffer's own size (captured at
     * CreateBuffer time), not just our fixed MVP_CB_BYTES window - a
     * shader/buffer size mismatch must fall through unpatched, never
     * silently read/write past the real buffer (which would previously
     * have applied a confidently-wrong, zero-filled "patch" instead of
     * skipping - the one path that violated the fail-safe requirement). */
    {
        /* 2026-09-03: check EVERY row, not just offs[3].
         *
         * This used to test `offs[0] < 0 || offs[3] + 16 > limit`, which was
         * correct only while row offsets were guaranteed contiguous and
         * ascending, so that offs[3] was necessarily the highest. Now that
         * mvp_row_offsets_for_shader() hands out the offsets reflection
         * actually reported, that no longer holds - the dominant
         * non-contiguous layout in this game is mvpmatrixz and mvpmatrixw
         * TRANSPOSED, i.e. {b, b+16, b+48, b+32}, where offs[2] is the highest
         * and offs[3] is not. Testing only offs[3] would have let a row run
         * past the end of the bound buffer while the check reported success -
         * exactly the silent out-of-bounds write the original check existed to
         * prevent. */
        int r, worst_end = 0, bad = 0;
        for (r = 0; r < 4; r++) {
            int end = offs[r] + 16;
            if (offs[r] < 0 || (UINT)end > byte_width || (UINT)end > MVP_CB_BYTES) {
                bad = 1;
            }
            if (end > worst_end) {
                worst_end = end;
            }
        }
        if (bad) {
            InterlockedIncrement(&g_diag_bounds_fail);
            ID3D11Buffer_Release(buf);
            if (mvp_rate_limit_should_fire(&g_offset_oob_log_count)) {
                log_msg("mvp_patch: shader's reflected mvp row offsets (%d,%d,%d,%d; furthest row "
                         "ends at byte %d) exceed the bound buffer's real size (%u bytes) or our "
                         "%d-byte window; draw falls through unpatched (fail-safe)",
                         offs[0], offs[1], offs[2], offs[3], worst_end, byte_width, MVP_CB_BYTES);
            }
            return 0;
        }
    }

    /* Fix round 5 (minor): copy and forward only the bound buffer's real
     * size, not the whole MVP_CB_BYTES window. The shader cannot legally
     * read past the bound buffer's own extent, and the bounds check above
     * has already guaranteed every mvp row lies inside copy_bytes, so the
     * tail was pure waste on a path that runs ~110k times/second. The tail
     * zero-fill went with it for the same reason. */
    copy_bytes = (byte_width < MVP_CB_BYTES) ? byte_width : MVP_CB_BYTES;
    if (!mvp_shadow_read(shadow_slot, local_copy, copy_bytes)) {
        /* A writer touched this shadow during our copy - the snapshot may be
         * torn, so skip rather than apply it (fix round 5, finding #4). */
        InterlockedIncrement(&g_diag_shadow_torn);
        ID3D11Buffer_Release(buf);
        return 0;
    }

    /* This call's VSGetConstantBuffers() reference is always redundant now
     * - the world cb0 pool holds its OWN separate reference, taken once at
     * registration time, independent of any specific draw's binding. */
    ID3D11Buffer_Release(buf);

    for (row = 0; row < 4; row++) {
        const float *f = (const float *)(local_copy + offs[row]);
        mvp.m[row][0] = f[0];
        mvp.m[row][1] = f[1];
        mvp.m[row][2] = f[2];
        mvp.m[row][3] = f[3];
    }
    patched = mat4_mul(*stereo_afr_k_for(&mvp), mvp);   /* g_K, or this frame's eye; menus/HUD stay mono (stereo_afr.c) */
    for (row = 0; row < 4; row++) {
        float *f = (float *)(local_copy + offs[row]);
        f[0] = patched.m[row][0];
        f[1] = patched.m[row][1];
        f[2] = patched.m[row][2];
        f[3] = patched.m[row][3];
    }

    if (!g_scratch_ready) {
        /* Double-checked locking (Important finding #6): without
         * g_scratch_init_cs here, two threads racing the first patchable
         * draw could both observe g_scratch_ready==0 and both create a
         * full MVP_SCRATCH_TOTAL-buffer set, leaking one set (the loser's
         * g_scratch[] writes would be silently overwritten by the
         * winner's, with no Release ever reaching the loser's buffers). */
        EnterCriticalSection(&g_scratch_init_cs);
        if (!g_scratch_ready) {
            ID3D11Device *dev = NULL;
            ID3D11DeviceContext_GetDevice(ctx, &dev);
            if (dev != NULL) {
                if (ensure_scratch_ready(dev)) {
                    InterlockedExchange(&g_scratch_ready, 1);
                    log_msg("mvp_patch: scratch pool ready (%d buffers, %dB each)",
                             MVP_SCRATCH_TOTAL, MVP_CB_BYTES);
                }
                ID3D11Device_Release(dev);
            }
        }
        LeaveCriticalSection(&g_scratch_init_cs);
        if (!g_scratch_ready) {
            InterlockedIncrement(&g_diag_scratch_not_ready);
            return 0;
        }
    }

    scratch_index = mvp_alloc_scratch_index();
    if (scratch_index < 0 || g_scratch[scratch_index] == NULL) {
        InterlockedIncrement(&g_diag_scratch_alloc_fail);
        return 0;
    }

    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_scratch[scratch_index], 0,
                                 D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr) || mapped.pData == NULL) {
        InterlockedIncrement(&g_diag_scratch_map_fail);
        if (mvp_rate_limit_should_fire(&g_map_fail_log_count)) {
            log_msg("mvp_patch: scratch Map(WRITE_DISCARD) failed (hr=0x%08lX); draw falls through unpatched",
                     (unsigned long)hr);
        }
        return 0;
    }
    memcpy(mapped.pData, local_copy, copy_bytes); /* see copy_bytes note above */
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_scratch[scratch_index], 0);

    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_scratch[scratch_index]);

    *out_orig_buf = buf; /* identity only, for the post-draw rebind - the direct pool (and the
                             engine's own binding) keep the underlying object alive independently */
    InterlockedIncrement(&g_diag_patched);
    return 1;
}

/* ================= detours ================= */

void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext *ctx, UINT IndexCount,
                                                UINT StartIndexLocation, INT BaseVertexLocation) {
    DrawIndexed_t orig = g_drawindexed_orig;
    ID3D11Buffer *orig_buf = NULL;
    int patched;

    if (orig == NULL) {
        return; /* should never happen once installed */
    }

    /* Fix round 4: DrawIndexed/Draw share one code address across every
     * context flavor, so they are the only place a deferred context is
     * guaranteed to reach us - use that to late-hook ITS vtable's
     * UpdateSubresource. Cheap after the first call per context. */
    mvp_maybe_late_hook_ctx(ctx);

    patched = mvp_patch_prepare(ctx, &orig_buf, IndexCount, 1);

    orig(ctx, IndexCount, StartIndexLocation, BaseVertexLocation);

    if (patched) {
        ID3D11Buffer *restore = orig_buf;
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &restore);
    }
}

void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext *ctx, UINT VertexCount,
                                         UINT StartVertexLocation) {
    Draw_t orig = g_draw_orig;
    ID3D11Buffer *orig_buf = NULL;
    int patched;

    if (orig == NULL) {
        return;
    }

    mvp_maybe_late_hook_ctx(ctx); /* see Hook_DrawIndexed */

    patched = mvp_patch_prepare(ctx, &orig_buf, VertexCount, 0);

    orig(ctx, VertexCount, StartVertexLocation);

    if (patched) {
        ID3D11Buffer *restore = orig_buf;
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &restore);
    }
}

HRESULT STDMETHODCALLTYPE Hook_CreateBuffer(ID3D11Device *dev, const D3D11_BUFFER_DESC *pDesc,
                                                    const D3D11_SUBRESOURCE_DATA *pInitialData,
                                                    ID3D11Buffer **ppBuffer) {
    CreateBuffer_t orig = g_createbuffer_orig;
    HRESULT hr;

    if (orig == NULL) {
        return E_FAIL; /* should never happen once installed - see hook_one()'s ordering fix */
    }

    hr = orig(dev, pDesc, pInitialData, ppBuffer);

    InterlockedIncrement(&g_cb_create_total);
    if (SUCCEEDED(hr) && ppBuffer != NULL && *ppBuffer != NULL && pDesc != NULL &&
        (pDesc->BindFlags & D3D11_BIND_CONSTANT_BUFFER)) {
        /* fix round 4 diagnostic: remember EVERY constant buffer our hook
         * sees created, regardless of whether the capture filter accepts
         * it, so a later pool-miss can tell "filter rejected it" apart from
         * "our hook never saw it at all". */
        InterlockedIncrement(&g_cb_create_const);
        mvp_seen_cb_record(*ppBuffer, pDesc);
    }

    /* FIX ROUND 4b: this hook NO LONGER registers anything into the world
     * cb0 pool. Descriptor-only classification at creation time could not
     * distinguish the real pool from same-shaped decoys (it failed that way
     * in rounds 1, 2 and 4a), so registration moved to the draw path, where
     * "bound at VS slot 0 for a draw with a usable mvpmatrix" is available
     * as the actual criterion - see mvp_try_register_bound_buffer(). What
     * remains here is purely the diagnostic bookkeeping above, which is
     * what proved the hook's coverage is complete. */
    return hr;
}
