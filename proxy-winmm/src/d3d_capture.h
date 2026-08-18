#pragma once

#define COBJMACROS
#include <windows.h>
#include <stdbool.h>
#include <d3d11.h>
#include <dxgi.h>

/*
 * Captured handles to the game's REAL Direct3D device/context/swap-chain,
 * populated once (on the first Hook_Present call) from the IDXGISwapChain
 * the game itself passes into the hook - as opposed to hooks.c's throwaway
 * 1x1 dummy device/swapchain, which exists only to resolve the shared
 * Present vtable slot and is released immediately after.
 *
 * Later tasks (the stereo present hook itself) render through these
 * handles, so their names/types here are a fixed interface - do not rename
 * without checking every later task's dependency on this exact shape.
 */
struct D3DCapture {
    IDXGISwapChain *sc;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
};

/* The single, process-wide capture instance. Zero-initialised (all NULL/0)
 * until d3d_capture_from_present() succeeds. */
extern struct D3DCapture g_d3d;

/* True once g_d3d has been fully and successfully populated (sc, dev, ctx
 * all non-NULL, width/height/format read). False otherwise - including
 * partial-failure cases, which never leave g_d3d half-populated as "ready". */
bool d3d_capture_ready(void);

/*
 * Populates g_d3d from `sc` - the real IDXGISwapChain the game passed into
 * Hook_Present. Safe to call every frame: it is a no-op (returns
 * immediately) once d3d_capture_ready() is already true.
 *
 * On any failure (GetDevice, GetImmediateContext, or GetDesc returning a
 * failure HRESULT), logs the reason once via log_msg() and leaves
 * d3d_capture_ready() false; never crashes and never partially "succeeds".
 * Must be called from the game's render thread (the same thread
 * Hook_Present runs on), same as every other g_d3d access.
 */
void d3d_capture_from_present(IDXGISwapChain *sc);
