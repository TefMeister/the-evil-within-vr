#pragma once

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>

/*
 * Task 6: per-draw MVP override - the Playbook Phase 4 keystone
 * camera-ownership proof.
 *
 * Design (see task-6-brief.md / notes/06-camera-matrix-discovery.md for the
 * full writeup): the world's opaque geometry is recorded almost entirely on
 * DEFERRED contexts (Task 5 finding); every world-geometry vertex shader
 * reads its per-object MVP from VS constant-buffer slot 0 (cb0) at a
 * per-shader reflected byte offset (mvptable.c). This module hooks
 * ID3D11DeviceContext::DrawIndexed/Draw ONCE (their underlying code address
 * is shared between the immediate and every deferred-context vtable
 * flavor, per seqdump.c's own confirmed finding - no per-vtable late-hook
 * table is needed here, unlike VSSetShader/VSSetConstantBuffers/Map/Unmap).
 * At each hooked call: read the currently-bound VS + slot-0 buffer's MVP
 * rows (via a once-per-frame content snapshot - see mvp_patch.c's Step 0
 * comment for why, not a per-draw GPU readback), left-multiply by a
 * constant test matrix K (TEWVR_TEST_YAW; identity if unset), write the
 * patched rows into our own scratch constant buffer, rebind VS slot 0 to
 * it, call the original draw, then rebind the engine's own buffer - so the
 * substitution is what gets recorded into the command list and replayed.
 * Any draw whose shader/buffer state can't be safely read or patched falls
 * through to the ORIGINAL, unmodified draw call - this module never
 * blocks, delays, or corrupts a draw it cannot confidently patch.
 */

/* Installs the DrawIndexed/Draw hooks via `dummy_ctx`'s vtable (same
 * throwaway-device contract as cbdump_install()/shaderdump_install()/
 * seqdump_install() - vtable read only, nothing retained). Safe to call
 * more than once (no-op after the first); safe to call with a NULL
 * `dummy_ctx` (logs and does nothing). All patching itself stays inert
 * until d3d_capture_ready() is true (the real device/context, needed to
 * create the scratch/staging buffers, is not available before the game's
 * first real Present) - so hooking can happen this early with no risk. */
void mvp_patch_install(ID3D11DeviceContext *dummy_ctx);

/* Tears down this module's own state (scratch/staging/pool buffers,
 * critical sections). Safe to call even if mvp_patch_install() was never
 * called or failed. Must run after mh_glue_shutdown() has disabled the
 * DrawIndexed/Draw trampolines (hooks_remove() already orders this
 * correctly for the other _remove() functions; mvp_patch_remove() follows
 * the same contract), so no in-flight hook can touch state that has just
 * been torn down. */
void mvp_patch_remove(void);

/* Called from Hook_Present on every frame (cheap no-op until
 * mvp_patch_install() has succeeded and d3d_capture_ready() is true).
 * Refreshes this frame's CPU-side snapshot of every known VS-slot0 cb0
 * buffer identity via one batched staging-copy readback on the IMMEDIATE
 * context - the chosen Step 0 read mechanism; see mvp_patch.c's top
 * comment for why per-draw / persistent-pointer alternatives were not
 * chosen. The refreshed snapshot feeds the NEXT frame's DrawIndexed/Draw
 * patch reads (one frame of staleness, accepted by the brief for this
 * proof). */
void mvp_patch_on_present(void);
