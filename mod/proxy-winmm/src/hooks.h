#pragma once

#define COBJMACROS
#include <windows.h>
#include <dxgi.h>

/*
 * IDXGISwapChain::Present hook.
 *
 * hooks_install() creates a throwaway 1x1 D3D11 device+swapchain purely to
 * read the (shared, per-process) IDXGISwapChain vtable, grabs
 * vtable[8] == Present, then installs a MinHook trampoline over it. Because
 * the vtable is shared across every swapchain instance in the process, the
 * trampoline also intercepts the game's real swapchain even though it was
 * never touched directly.
 *
 * Must be called off the DllMain path (see dllmain.c's bootstrap thread) -
 * it waits for d3d11.dll and creates a D3D11 device, neither of which is
 * safe to do while the loader lock is held.
 */

/* Signature of IDXGISwapChain::Present. Later tasks (the stereo present
 * hook itself) call through g_present_orig to reach the real
 * driver/game Present after doing their own work. */
typedef HRESULT(STDMETHODCALLTYPE *Present_t)(IDXGISwapChain *, UINT, UINT);

/* The original (trampoline) Present, filled in by MH_CreateHook() inside
 * hooks_install(). NULL until a hook is successfully installed. */
extern Present_t g_present_orig;

/* Resolves Present via a dummy swapchain and installs the MinHook
 * trampoline. On any failure (device creation, MinHook init, hook
 * create/enable), logs the reason once and returns with no hook installed
 * - the game keeps running normally (mono). Safe to call more than once;
 * a no-op if a hook is already active. */
void hooks_install(void);

/* Disables and removes the Present hook (if any) and uninitialises
 * MinHook. Safe to call even if hooks_install() was never called or
 * failed. Must be called before log_shutdown() in DLL_PROCESS_DETACH. */
void hooks_remove(void);
