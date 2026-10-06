#include "hooks.h"
#include <d3d11.h>

#include "MinHook.h"
#include "minhook_glue.h"
#include "log.h"
#include "d3d_capture.h"
#include "cbdump.h"
#include "shaderdump.h"
#include "seqdump.h"
#include "mvp_patch.h"
#include "framecapture.h"
#include "stereo_afr.h"
#include "tew_xr.h"

Present_t g_present_orig = NULL;

#define HOOKS_FOREIGN_LOG_EVERY 600  /* non-game Presents between count lines */

static UINT64 g_frame = 0;
static int g_hooks_active = 0;

/*
 * ONLY THE GAME'S SWAPCHAIN RUNS OUR PER-FRAME WORK (2026-10-06, reader). The Present hook sits on the vtable every
 * swapchain in the process shares, and the OpenXR simulator's preview window is a swapchain INSIDE this process: its
 * Presents flipped the stereo eye and made tew_xr resize its eye images 1280x720 <-> 1728x824 every few frames.
 * The game's swapchain is the first one presented (captured by d3d_capture before any XR session can exist, since
 * the session is created on the game's captured device). A later swapchain counts as the game's only if it draws
 * into the SAME window (the game recreating its swapchain); it is then adopted. Everything else just presents.
 * The device is NOT a test: the simulator presents on the game's own device. See also is_runtime_present() (v2).
 */
static HWND g_game_hwnd;                   /* the window of the first captured swapchain */
static IDXGISwapChain *g_foreign_last;     /* last non-game swapchain seen, so GetDesc runs once per swapchain */
static volatile LONG g_foreign_presents;   /* non-game Presents passed straight through */

/* v2 (2026-10-06, live finding): the OWN-DEVICE build started its session before the game's first Present, so the
 * simulator's preview Presented first - from inside our xrEndFrame, on the headset thread - and was taken for the
 * game's swapchain (log: window "OpenXR Simulator (Mouse Look + WASD)", 1728x784). A Present on the headset thread,
 * or into a window that thread created, is the runtime's, never the game's. */
static int is_runtime_present(IDXGISwapChain *sc) {
    DWORD xr_tid = tew_xr_headset_tid();
    DXGI_SWAP_CHAIN_DESC d;
    if (xr_tid == 0) return 0;
    if (GetCurrentThreadId() == xr_tid) return 1;
    if (d3d_capture_ready()) return 0;          /* after capture, is_game_swapchain() decides */
    ZeroMemory(&d, sizeof(d));
    return SUCCEEDED(IDXGISwapChain_GetDesc(sc, &d)) && d.OutputWindow &&
           GetWindowThreadProcessId(d.OutputWindow, NULL) == xr_tid;
}

static int is_game_swapchain(IDXGISwapChain *sc) {
    DXGI_SWAP_CHAIN_DESC d;
    if (sc == g_d3d.sc) return 1;
    if (sc == g_foreign_last) return 0;
    ZeroMemory(&d, sizeof(d));
    if (g_game_hwnd != NULL && SUCCEEDED(IDXGISwapChain_GetDesc(sc, &d)) && d.OutputWindow == g_game_hwnd) {
        log_msg("hooks: the game recreated its swap-chain (%p -> %p, %ux%u, same window); adopting it",
                (void *)g_d3d.sc, (void *)sc, (unsigned)d.BufferDesc.Width, (unsigned)d.BufferDesc.Height);
        g_d3d.sc = sc;
        g_d3d.width = d.BufferDesc.Width;
        g_d3d.height = d.BufferDesc.Height;
        g_d3d.format = d.BufferDesc.Format;
        return 1;
    }
    g_foreign_last = sc;
    log_msg("hooks: Present from a swap-chain that is not the game's (%p, window %p, %ux%u) - passed straight "
            "through, no per-frame work (e.g. the OpenXR simulator's preview)",
            (void *)sc, (void *)d.OutputWindow, (unsigned)d.BufferDesc.Width, (unsigned)d.BufferDesc.Height);
    return 0;
}

static void note_game_window(void) {
    DXGI_SWAP_CHAIN_DESC d;
    char title[128] = "", cls[128] = "";
    ZeroMemory(&d, sizeof(d));
    if (g_d3d.sc == NULL || FAILED(IDXGISwapChain_GetDesc(g_d3d.sc, &d))) return;
    g_game_hwnd = d.OutputWindow;
    GetWindowTextA(g_game_hwnd, title, sizeof(title));
    GetClassNameA(g_game_hwnd, cls, sizeof(cls));
    log_msg("hooks: game swap-chain %p draws to window %p, title \"%s\", class \"%s\"; only it runs per-frame work",
            (void *)g_d3d.sc, (void *)g_game_hwnd, title, cls);
}

static HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if (is_runtime_present(sc)) {
        if ((InterlockedIncrement(&g_foreign_presents) % HOOKS_FOREIGN_LOG_EVERY) == 1)
            log_msg("hooks: %ld headset-runtime Present(s) passed through (headset thread or its window)",
                    (long)g_foreign_presents);
        return g_present_orig(sc, sync, flags);
    }
    if (!d3d_capture_ready()) {
        /* First-Present-only; d3d_capture_from_present() is itself a no-op
         * once ready, but the ready-check here avoids the call overhead on
         * every subsequent frame. */
        d3d_capture_from_present(sc);
        if (d3d_capture_ready()) note_game_window();
    }
    if (!d3d_capture_ready() || !is_game_swapchain(sc)) {
        if (d3d_capture_ready() && (InterlockedIncrement(&g_foreign_presents) % HOOKS_FOREIGN_LOG_EVERY) == 0)
            log_msg("hooks: %ld non-game Present(s) passed through so far", (long)g_foreign_presents);
        return g_present_orig(sc, sync, flags);
    }

    if ((g_frame++ % 120) == 0) {
        log_msg("Present hook alive: frame %llu", (unsigned long long)g_frame);
    }

    /* Task 5 TEWVR_SEQDUMP=1 event stream: cheap no-op unless seqdump's
     * hooks installed successfully. Drives the "arm 300 frames after the
     * first Present" logic and emits the PRESENT frame-boundary marker. */
    seqdump_on_present(g_frame);

    /* SPIKE (2026-08-21, not yet reviewed): TEWVR_FRAMECAPTURE=1 back-buffer
     * capture-to-disk, file-triggered via capture.txt. See framecapture.h. */
    framecapture_on_present(g_frame);

    /* 2026-10-06: OPENXR = 1 copies this frame to its eye's headset image and runs one headset frame, BEFORE the
     * real Present (the back buffer still holds this frame). No-op when off. */
    tew_xr_on_present(sc);

    /* 2026-10-06: alternate-frame stereo flips the eye here; the frame being presented keeps its eye. */
    {
        HRESULT hr = g_present_orig(sc, sync, flags);
        stereo_afr_on_present();
        return hr;
    }
}

/*
 * Creates a hidden 1x1 dummy device+swapchain purely to read the shared
 * IDXGISwapChain vtable, then releases it immediately. The vtable (not the
 * instance) is what MinHook patches, and it is shared by every swapchain
 * the process creates, so a trampoline installed over this throwaway
 * instance's Present still intercepts the game's real swapchain later.
 * Returns the resolved Present pointer, or NULL on any failure.
 */
static Present_t capture_present_via_dummy_swapchain(void) {
    DXGI_SWAP_CHAIN_DESC scd;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGISwapChain *sc = NULL;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr;
    Present_t present = NULL;

    ZeroMemory(&scd, sizeof(scd));
    scd.BufferCount = 1;
    scd.BufferDesc.Width = 1;
    scd.BufferDesc.Height = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = GetDesktopWindow(); /* transient; never presented */
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        &fl, 1, D3D11_SDK_VERSION,
        &scd, &sc, &dev, NULL, &ctx);

    if (FAILED(hr) || sc == NULL) {
        log_msg("hooks_install: D3D11CreateDeviceAndSwapChain failed (hr=0x%08lX)", (unsigned long)hr);
        goto cleanup;
    }

    {
        /* vtable[8] == IDXGISwapChain::Present. */
        void **vtbl = *(void ***)sc;
        present = (Present_t)vtbl[8];
    }

cleanup:
    if (ctx) {
        /* Temporary Task 4 discovery instrumentation (TEWVR_DUMP=1 only);
         * reads the same throwaway context's vtable this function already
         * created, before it is released below. See cbdump.h. */
        cbdump_install(ctx);
    }
    if (dev && ctx) {
        /* Temporary Task 4 shader-level RE instrumentation
         * (TEWVR_SHADERDUMP=1 only, but Task 5 also arms its
         * CreateVertexShader hook under TEWVR_SEQDUMP=1 - see
         * shaderdump.h); same throwaway-vtable contract. */
        shaderdump_install(dev, ctx);

        /* Task 6: the real per-draw MVP override (NOT a TEWVR_* diagnostic
         * mode - always installed). Same throwaway-vtable contract: reads
         * ID3D11Device::CreateBuffer's and ID3D11DeviceContext::
         * DrawIndexed/Draw's vtable slots off these dummy objects, retains
         * nothing from either. Needs `dev` too (unlike cbdump/shaderdump/
         * seqdump above) because its Step 0 read mechanism hooks
         * CreateBuffer on the device vtable - see mvp_patch.h. */
        mvp_patch_install(dev, ctx);
    }
    if (ctx) {
        /* Task 5 TEWVR_SEQDUMP=1 ordered event-stream instrumentation;
         * same throwaway-vtable contract as cbdump/shaderdump above. See
         * seqdump.h. */
        seqdump_install(ctx);
    }
    if (sc)  IDXGISwapChain_Release(sc);
    if (ctx) ID3D11DeviceContext_Release(ctx);
    if (dev) ID3D11Device_Release(dev);

    return present;
}

void hooks_install(void) {
    Present_t present;

    if (g_hooks_active) {
        return;
    }

    present = capture_present_via_dummy_swapchain();
    if (present == NULL) {
        log_msg("hooks_install: failed to resolve Present address; running mono (no hooks)");
        return;
    }
    log_msg("hooks_install: resolved IDXGISwapChain::Present at %p", (void *)present);

    if (!mh_glue_init()) {
        log_msg("hooks_install: MinHook init failed; running mono (no hooks)");
        return;
    }

    if (!mh_glue_create_and_enable((void *)present, (void *)&Hook_Present,
                                    (void **)&g_present_orig, "Present")) {
        log_msg("hooks_install: failed to create/enable Present hook; running mono (no hooks)");
        mh_glue_shutdown();
        return;
    }

    g_hooks_active = 1;
    log_msg("hooks_install: Present hook active");
    tew_xr_init();   /* OPENXR = 1 only: starts loading the headset runtime on its own thread */
}

void hooks_remove(void) {
    /* mh_glue_shutdown() is called unconditionally (it is itself idempotent
     * - a no-op if MinHook was never initialised) rather than gated on
     * g_hooks_active, because cbdump_install() (Task 4's temporary
     * constant-buffer dump hooks, TEWVR_DUMP=1 only) can have initialised
     * MinHook and installed its own hooks even in the rare case the
     * Present hook itself failed to install. This must run BEFORE
     * cbdump_remove() so no Map/Unmap/UpdateSubresource trampoline can
     * still be live when cbdump tears down its own state. */
    tew_xr_shutdown();
    mh_glue_shutdown();
    cbdump_remove();
    shaderdump_remove();
    seqdump_remove();
    mvp_patch_remove();
    g_hooks_active = 0;
}
