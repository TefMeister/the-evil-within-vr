#pragma once

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>

/*
 * TEMPORARY constant-buffer discovery instrumentation for Task 4
 * (camera-matrix discovery). Hooks ID3D11DeviceContext::Map/Unmap and
 * ::UpdateSubresource (shared vtable, same trick hooks.c uses for
 * IDXGISwapChain::Present) and, for any buffer resource sized 64..512
 * bytes (a plausible camera-cbuffer range), dumps its first 128 bytes as
 * float32 rows to a SEPARATE log file:
 *   %LOCALAPPDATA%\TEWVR\cbdump.log
 *
 * Entirely inert unless the environment variable TEWVR_DUMP is set to "1"
 * at the time cbdump_install() runs - off by default, so normal play is
 * completely unaffected. Intended to be narrowed/removed by a later task
 * once the camera matrix's buffer/offset/layout is identified; kept in its
 * own module (not entangled with hooks.c's Present hook) so that removal
 * is a clean deletion of this file plus its two call sites.
 */

/*
 * Attempts to install the cbdump hooks. Must be called with a live
 * ID3D11DeviceContext whose vtable is safe to read - hooks.c passes its
 * throwaway dummy device's context, the same shared-vtable trick used to
 * resolve Present, before releasing it. `ctx` itself is never retained or
 * dereferenced beyond reading its vtable pointer and is safe to release
 * immediately after this call returns.
 *
 * No-ops entirely (returns immediately, touches nothing - no MinHook
 * calls, no file I/O) if TEWVR_DUMP is unset or not exactly "1". On any
 * failure past that point (log file open, MinHook init, individual hook
 * create/enable), logs once to tewvr.log via log_msg() and continues with
 * whatever subset of hooks (if any) succeeded - never crashes, and never
 * prevents hooks.c's own Present hook from installing.
 */
void cbdump_install(ID3D11DeviceContext *dummy_ctx);

/*
 * Tears down cbdump's own state (closes cbdump.log, deletes its critical
 * section). Safe to call even if cbdump_install() was never called or
 * found TEWVR_DUMP unset (pure no-op in that case). Must be called AFTER
 * mh_glue_shutdown() has disabled the Map/Unmap/UpdateSubresource
 * trampolines (hooks_remove() does this ordering), so no in-flight hook
 * can touch cbdump state that has just been torn down.
 */
void cbdump_remove(void);
