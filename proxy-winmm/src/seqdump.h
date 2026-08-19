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
 *   ID3D11DeviceContext1::VSSetConstantBuffers1 (if the dummy context QIs
 *     to ID3D11DeviceContext1 at install time) - detects/records whether
 *     the engine binds cb ranges with D3D11.1-style offsets.
 *
 * One line per event to %LOCALAPPDATA%\TEWVR\seqdump.log, with a
 * monotonically increasing sequence number and thread id on every line.
 * Capture arms 300 frames after the first Present (skip loading screens),
 * then captures the first 40,000 events before writing a final
 * "SEQDUMP COMPLETE" line and going silent.
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
 * armed, and performs the arm transition itself (300 frames after the
 * first call this function ever sees) - including the one-line
 * "does the REAL game context QI to ID3D11DeviceContext1" detection the
 * brief asks for at arm time. `frame_number` is hooks.c's own g_frame
 * counter (any monotonically increasing per-Present counter works; only
 * relative deltas matter here). */
void seqdump_on_present(UINT64 frame_number);
