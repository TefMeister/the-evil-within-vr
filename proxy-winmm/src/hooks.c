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

Present_t g_present_orig = NULL;

static UINT64 g_frame = 0;
static int g_hooks_active = 0;

static HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if (!d3d_capture_ready()) {
        /* First-Present-only; d3d_capture_from_present() is itself a no-op
         * once ready, but the ready-check here avoids the call overhead on
         * every subsequent frame. */
        d3d_capture_from_present(sc);
    }

    if ((g_frame++ % 120) == 0) {
        log_msg("Present hook alive: frame %llu", (unsigned long long)g_frame);
    }

    /* Task 5 TEWVR_SEQDUMP=1 event stream: cheap no-op unless seqdump's
     * hooks installed successfully. Drives the "arm 300 frames after the
     * first Present" logic and emits the PRESENT frame-boundary marker. */
    seqdump_on_present(g_frame);

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
    mh_glue_shutdown();
    cbdump_remove();
    shaderdump_remove();
    seqdump_remove();
    mvp_patch_remove();
    g_hooks_active = 0;
}
