#include "hooks.h"
#include <d3d11.h>

#include "MinHook.h"
#include "minhook_glue.h"
#include "log.h"

Present_t g_present_orig = NULL;

static UINT64 g_frame = 0;
static int g_hooks_active = 0;

static HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if ((g_frame++ % 120) == 0) {
        log_msg("Present hook alive: frame %llu", (unsigned long long)g_frame);
    }
    return g_present_orig(sc, sync, flags);
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
}

void hooks_remove(void) {
    if (!g_hooks_active) {
        return;
    }

    mh_glue_shutdown();
    g_hooks_active = 0;
}
