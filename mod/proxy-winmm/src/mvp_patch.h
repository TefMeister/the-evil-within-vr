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
 * It also hooks ID3D11Device::CreateBuffer to persistently self-Map the
 * small pool of world-geometry cb0 buffers at creation time (Step 0 - see
 * mvp_patch.c's top comment for the full read-mechanism reasoning and why
 * a once-per-frame snapshot, tried first, was rejected as producing
 * confidently-wrong data for the majority of draws sharing a buffer
 * identity). At each hooked Draw/DrawIndexed call: read the currently-bound
 * VS + slot-0 buffer's MVP rows LIVE from that persistent CPU pointer (no
 * staleness, no per-draw GPU op), left-multiply by a constant test matrix K
 * (TEWVR_TEST_YAW; identity if unset), write the patched rows into a
 * per-thread scratch constant buffer, rebind VS slot 0 to it, call the
 * original draw, then rebind the engine's own buffer - so the substitution
 * is what gets recorded into the command list and replayed. Any draw whose
 * shader/buffer state can't be safely read or patched (including a buffer
 * this module never got a persistent pointer for - e.g. the small,
 * per-draw-Mapped immediate-context cb0 buffers, which are NOT covered by
 * this mechanism, only the world's own draws are) falls through to the
 * ORIGINAL, unmodified draw call - this module never blocks, delays, or
 * corrupts a draw it cannot confidently patch.
 */

/* Installs the CreateBuffer + DrawIndexed/Draw hooks via `dummy_dev`'s and
 * `dummy_ctx`'s vtables (same throwaway-device contract as cbdump_install()/
 * shaderdump_install()/seqdump_install() - vtable read only, nothing
 * retained from the dummy objects themselves). Safe to call more than once
 * (no-op after the first); safe to call with a NULL `dummy_dev`/`dummy_ctx`
 * (logs and does nothing). If DrawIndexed/Draw are already hooked by
 * another module (TEWVR_SEQDUMP=1 or TEWVR_SHADERDUMP=1 both hook the same
 * addresses first), this logs a loud, explicitly-named conflict and leaves
 * mvp_patch entirely disabled for the session rather than silently doing
 * nothing. */
void mvp_patch_install(ID3D11Device *dummy_dev, ID3D11DeviceContext *dummy_ctx);

/* Tears down this module's own state (releases every persistently-mapped
 * world cb0 buffer, the scratch-buffer ring, critical sections). Safe to
 * call even if mvp_patch_install() was never called or failed. Must run
 * after mh_glue_shutdown() has disabled the CreateBuffer/DrawIndexed/Draw
 * trampolines (hooks_remove() already orders this correctly for the other
 * _remove() functions; mvp_patch_remove() follows the same contract), so no
 * in-flight hook can touch state that has just been torn down. */
void mvp_patch_remove(void);
