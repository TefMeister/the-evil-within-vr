#pragma once

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>

/*
 * Task 5 feature 1: TEWVR_SEQDUMP - ordered per-draw D3D11 event stream.
 *
 * Purpose: reveal the exact fill -> bind -> draw ordering the engine uses
 * for vertex-shader slot-0 constant buffers, so a later task can pick the
 * patch point for a per-eye MVP matrix.
 *
 * Hooks (same shared-vtable trick as cbdump.c/shaderdump.c, all its own
 * independent detours - deliberately NOT sharing cbdump's/shaderdump's
 * hook bodies, so this module hooks exactly what it needs regardless of
 * whether TEWVR_DUMP/TEWVR_SHADERDUMP are also set):
 *   ID3D11DeviceContext::Map / Unmap / UpdateSubresource - filtered to
 *     constant buffers (BindFlags & D3D11_BIND_CONSTANT_BUFFER) with
 *     ByteWidth <= 8192.
 *   ID3D11DeviceContext::VSSetConstantBuffers - slot bindings.
 *   ID3D11DeviceContext::VSSetShader - shader-ptr + hash (via
 *     shaderdump_hash_for_shader(), reusing shaderdump.c's ptr->hash map -
 *     see shaderdump.c's g_cvs_hook_active for why that map is populated
 *     even in a TEWVR_SEQDUMP-only run).
 *   ID3D11DeviceContext::DrawIndexed/Draw/DrawIndexedInstanced/DrawInstanced.
 *   ID3D11DeviceContext::FinishCommandList/ExecuteCommandList (review
 *     addendum 2 - see below) - to detect deferred-context command-list
 *     recording/submission.
 *   ID3D11DeviceContext::RSSetViewports/OMSetRenderTargets (addendum 3 -
 *     see below) - low-frequency state events, hooked on both the
 *     immediate vtable and any deferred vtable addendum 3 discovers.
 *   ID3D11DeviceContext1::VSSetConstantBuffers1 (if the dummy context QIs
 *     to ID3D11DeviceContext1 at install time) - detects/records whether
 *     the engine binds cb ranges with D3D11.1-style offsets.
 *
 * Addendum 3 (post-review, gameplay capture): the gameplay capture proved
 * the world-geometry bulk is recorded on DEFERRED contexts, whose
 * VSSetShader/VSSetConstantBuffers/Map/Unmap/RSSetViewports/
 * OMSetRenderTargets calls our immediate-vtable-only hooks above could
 * not see - deferred contexts use a genuinely different vtable than the
 * immediate context for those six methods specifically (DrawIndexed/
 * Draw/DrawIndexedInstanced/DrawInstanced/FinishCommandList/
 * ExecuteCommandList happen to share one implementation address across
 * both flavors, which is why those hooks already saw deferred-context
 * calls even before this addendum). Three additions:
 *   1. Late-hooking: Hook_DrawIndexed/Draw/DrawIndexedInst/DrawInst and
 *      Hook_FinishCommandList now call seq_maybe_late_hook_deferred_ctx()
 *      on the `ctx` they were invoked with; the first time an unseen ctx
 *      pointer's vtable differs from the immediate context's, this
 *      MinHook-installs fresh hooks on THAT vtable's VSSetShader/
 *      VSSetConstantBuffers/Map/Unmap/RSSetViewports/OMSetRenderTargets
 *      slots, reusing the exact same detour functions (they already log
 *      the ctx they were called with, so events self-identify).
 *   2. RSSETVP/OMSETRT events (see above) - hooked on both the immediate
 *      and any late-hooked deferred vtable.
 *   3. TEWVR_SKIPCL live visual toggle: while
 *      %LOCALAPPDATA%\TEWVR\skipcl.txt exists, Hook_ExecuteCommandList
 *      drops the command list instead of executing it (logging
 *      "EXECUTECMDLIST SKIPPED" while capture is active) - a live
 *      render-pass on/off switch for a human watching the game, working
 *      whenever TEWVR_SEQDUMP=1 regardless of capture/arm state.
 *      seqdump_clear_stale_skipcl() deletes any leftover skipcl.txt at
 *      DLL startup, same as seqarm.txt.
 *
 * One line per event to %LOCALAPPDATA%\TEWVR\seqdump.log, with a
 * monotonically increasing sequence number, thread id, AND the
 * ID3D11DeviceContext(/1) pointer the event fired on (`ctx=0x...`) on
 * every line - added post-Task-5-review (addendum 2) after a gameplay
 * capture showed draws arriving from six distinct threads while every
 * state-setting event (VSSETSHADER/VSSETCB/MAP) stayed on the render
 * thread, suggesting deferred contexts recording command lists on worker
 * threads; the ctx= field plus the new FinishCommandList/ExecuteCommandList
 * events let a later analysis pass confirm or refute that directly from
 * the log instead of guessing from thread ids alone.
 *
 * By default, capture arms 300 frames after the first Present (skip
 * loading screens); then captures the first 40,000 events before writing
 * a final "SEQDUMP COMPLETE" line and going silent.
 *
 * Addendum (post-Task-5 review): TEWVR_SEQDUMP_ARMFILE=1 (only meaningful
 * alongside TEWVR_SEQDUMP=1) replaces the frame-301 auto-arm with a
 * live/manual arm - every ~30 frames, Hook_Present checks (a cheap
 * existence check, not an open) whether %LOCALAPPDATA%\TEWVR\seqarm.txt
 * exists, and arms the instant it appears, logging an
 * "ARMED by seqarm.txt at frame N" line. This lets a controller reach
 * real gameplay first (past the menu/loading screens that would otherwise
 * burn the 40,000-event budget) and only then start the capture by
 * creating the file. seqdump_clear_stale_armfile() deletes any leftover
 * seqarm.txt at DLL startup so a file left over from a previous session
 * can't prematurely arm a later one. Default behaviour (ARMFILE unset)
 * is unchanged.
 *
 * Task 6 (discovery brief, "1920B buffer" puzzle): the addendum-3 late-hook
 * discovery point (seq_maybe_late_hook_deferred_ctx()) was only ever
 * reached from the Draw/FinishCommandList hooks, so a dedicated
 * fill/streaming deferred context that only ever calls Map/Unmap/
 * VSSetConstantBuffers (never Draws) stayed invisible. Feature 1: Hook_Map/
 * Hook_Unmap/Hook_VSSetCB now call seq_maybe_late_hook_deferred_ctx() too,
 * and the UNMAP content fingerprint is extended from 4 to up to 8 floats,
 * to help correlate fills to bound slot0/2/3 pointers by content. Feature 2
 * (TEWVR_CBPEEK=1, its own separate cheap-off gate on top of
 * TEWVR_SEQDUMP=1): for the first 300 deferred DrawIndexed calls after
 * capture arms, reads the ACTUAL bound content of VS slots 0/2/3 at draw
 * time - via a CopySubresourceRegion + Map(READ) into one lazily-created
 * 2048-byte D3D11_USAGE_STAGING buffer on the IMMEDIATE context - and logs
 * a CBPEEK line (slot, resource ptr, size, FNV-1a64 content hash, and the 4
 * floats at the reflected mvpmatrixx offset for the currently-bound VS).
 * This crosses threads (deferred-context worker thread driving the
 * immediate context); see seqdump.c's CBPEEK state-block comment for the
 * residual cross-thread hazard this cannot fully close, and why it's
 * opt-in and capped.
 *
 * Entirely inert unless TEWVR_SEQDUMP=1 when seqdump_install() runs -
 * off by default, so normal play (and every other TEWVR_* mode) is
 * unaffected. Fail-safe throughout: any failure (log open, MinHook init,
 * individual hook create/enable, D3D11.1 QueryInterface) logs once via
 * log_msg() and continues with whatever subset succeeded - never crashes,
 * never prevents hooks.c's own Present hook or any other module's hooks
 * from installing.
 */

/* Attempts to install the seqdump hooks. Must be called with a live
 * ID3D11DeviceContext whose vtable is safe to read - hooks.c passes its
 * throwaway dummy device's context, same as cbdump_install()/
 * shaderdump_install(). `dummy_ctx` itself is never retained beyond
 * reading its vtable pointer (and briefly QueryInterface-ing it for
 * ID3D11DeviceContext1) and is safe to release immediately after this
 * call returns. No-ops entirely if TEWVR_SEQDUMP is unset or not exactly
 * "1". */
void seqdump_install(ID3D11DeviceContext *dummy_ctx);

/* Tears down seqdump's own state (closes seqdump.log, deletes its critical
 * section). Safe to call even if seqdump_install() was never called or
 * found TEWVR_SEQDUMP unset (pure no-op then). Must be called AFTER
 * mh_glue_shutdown() has disabled every trampoline (hooks_remove() does
 * this ordering), so no in-flight hook can touch state that has just been
 * torn down. */
void seqdump_remove(void);

/* Called from hooks.c's Hook_Present on every frame (cheap no-op unless
 * seqdump's hooks installed successfully). Emits the PRESENT event once
 * armed, and performs the arm transition itself - by default 300 frames
 * after the first call this function ever sees (including the one-line
 * "does the REAL game context QI to ID3D11DeviceContext1" detection the
 * brief asks for at arm time), or - if TEWVR_SEQDUMP_ARMFILE=1 - the
 * instant %LOCALAPPDATA%\TEWVR\seqarm.txt appears (checked every ~30
 * frames). `frame_number` is hooks.c's own g_frame counter (any
 * monotonically increasing per-Present counter works; only relative
 * deltas matter here). */
void seqdump_on_present(UINT64 frame_number);

/* Deletes any stale %LOCALAPPDATA%\TEWVR\seqarm.txt left over from a
 * previous TEWVR_SEQDUMP_ARMFILE=1 run, so it can't prematurely arm a
 * later run before the controller creates a fresh one. Call once,
 * unconditionally, at DLL startup (dllmain.c's DLL_PROCESS_ATTACH) -
 * regardless of whether TEWVR_SEQDUMP is set this run, since a later run
 * within the same game session might set TEWVR_SEQDUMP_ARMFILE=1 and
 * should not see a leftover file. Fail-safe: the common case (file
 * absent, or %LOCALAPPDATA%\TEWVR doesn't exist yet) is silent; only an
 * unexpected deletion failure logs once. Never crashes. */
void seqdump_clear_stale_armfile(void);

/* Deletes any stale %LOCALAPPDATA%\TEWVR\skipcl.txt left over from a
 * previous session's TEWVR_SKIPCL live-toggle use (addendum 3), so a
 * leftover file can't silently start dropping command lists the instant
 * a later run's ExecuteCommandList hook installs. Same call-site and
 * fail-safe contract as seqdump_clear_stale_armfile() - call once,
 * unconditionally, at DLL startup. */
void seqdump_clear_stale_skipcl(void);
