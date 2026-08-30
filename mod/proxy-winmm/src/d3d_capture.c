#include "d3d_capture.h"

#include "log.h"

/* IID_ID3D11Device. Normally pulled from -ldxguid (see build.ps1); defined
 * locally as a fallback is NOT needed here because dxguid links cleanly
 * under this llvm-mingw toolchain (confirmed: libdxguid.a present for the
 * x86_64-w64-mingw32 target) - see task-3-report.md for the verification. */

struct D3DCapture g_d3d;

static bool g_d3d_ready = false;

bool d3d_capture_ready(void) {
    return g_d3d_ready;
}

void d3d_capture_from_present(IDXGISwapChain *sc) {
    HRESULT hr;
    DXGI_SWAP_CHAIN_DESC scd;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;

    if (g_d3d_ready) {
        return; /* already captured */
    }

    if (sc == NULL) {
        log_msg("d3d_capture: Hook_Present called with a NULL swap-chain; skipping capture");
        return;
    }

    hr = IDXGISwapChain_GetDevice(sc, &IID_ID3D11Device, (void **)&dev);
    if (FAILED(hr) || dev == NULL) {
        log_msg("d3d_capture: IDXGISwapChain::GetDevice failed (hr=0x%08lX)", (unsigned long)hr);
        return;
    }

    ID3D11Device_GetImmediateContext(dev, &ctx);
    if (ctx == NULL) {
        log_msg("d3d_capture: ID3D11Device::GetImmediateContext returned NULL");
        ID3D11Device_Release(dev);
        return;
    }

    ZeroMemory(&scd, sizeof(scd));
    hr = IDXGISwapChain_GetDesc(sc, &scd);
    if (FAILED(hr)) {
        log_msg("d3d_capture: IDXGISwapChain::GetDesc failed (hr=0x%08lX)", (unsigned long)hr);
        ID3D11DeviceContext_Release(ctx);
        ID3D11Device_Release(dev);
        return;
    }

    /* All calls succeeded - publish the capture as one unit. g_d3d is only
     * ever written from the render thread (Hook_Present's thread), so no
     * lock is needed here, same assumption hooks.c already documents for
     * g_frame. */
    g_d3d.sc = sc;
    g_d3d.dev = dev;
    g_d3d.ctx = ctx;
    g_d3d.width = scd.BufferDesc.Width;
    g_d3d.height = scd.BufferDesc.Height;
    g_d3d.format = scd.BufferDesc.Format;

    g_d3d_ready = true;

    log_msg("d3d_capture: captured real swap-chain %ux%u format=%d (dev=%p ctx=%p sc=%p)",
            (unsigned int)g_d3d.width, (unsigned int)g_d3d.height, (int)g_d3d.format,
            (void *)g_d3d.dev, (void *)g_d3d.ctx, (void *)g_d3d.sc);
    log_msg("d3d_capture: d3d_capture_ready() == true");
}
