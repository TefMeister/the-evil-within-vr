#include "framecapture.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#define COBJMACROS
#include <d3d11.h>
#include <dxgi.h>

#include "log.h"
#include "d3d_capture.h"

#define FRAMECAPTURE_CHECK_FRAMES 30

static int g_fc_env_checked = 0;
static int g_fc_enabled = 0;

/* Fills `out` with %LOCALAPPDATA%\TEWVR\capture.txt, or an empty string on
 * any failure. Same contract as seqdump.c's seqarm_path(). */
static void capture_trigger_path(wchar_t *out, size_t out_count) {
    wchar_t la[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        out[0] = L'\0';
        return;
    }
    swprintf(out, out_count, L"%s\\TEWVR\\capture.txt", la);
}

static void capture_out_dir(wchar_t *out, size_t out_count) {
    wchar_t la[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        out[0] = L'\0';
        return;
    }
    swprintf(out, out_count, L"%s\\TEWVR\\captures", la);
}

/* Writes `width`x`height` BGRA8/RGBA8 pixel data (mapped staging texture,
 * row pitch `row_pitch`) to a 24-bit uncompressed BMP at `path`. `swap_rb`
 * selects R8G8B8A8 (true, needs an R/B swap per pixel to match BMP's B,G,R
 * byte order) vs B8G8R8A8 (false, already in BMP order - copied verbatim).
 * Returns 1 on success, 0 on any failure (never crashes; caller logs). */
static int write_bmp(const wchar_t *path, const unsigned char *pixels,
                      UINT width, UINT height, UINT row_pitch, int swap_rb) {
    FILE *f = NULL;
    /* Round width*3 UP to the next multiple of 4 (BMP row-alignment rule).
     * NOTE: do the "+3 then mask" rounding BEFORE any division - dividing
     * by 4 first and then masking the low bits off the ALREADY-DIVIDED
     * value does not multiply back up, it just corrupts the number (this
     * was the bug that shipped: width=1280 -> intended 3840, got 960,
     * silently corrupting every size field derived from it). */
    UINT out_row_bytes = (width * 3u + 3u) & ~3u;
    UINT pad = out_row_bytes - width * 3u;
    DWORD image_size = out_row_bytes * height;
    DWORD file_size = 14u + 40u + image_size;
    unsigned char file_hdr[14];
    unsigned char info_hdr[40];
    unsigned char zero_pad[4] = {0, 0, 0, 0};
    UINT y;

    log_msg("framecapture: write_bmp %ux%u row_pitch=%u out_row_bytes=%u pad=%u image_size=%lu file_size=%lu",
             width, height, row_pitch, out_row_bytes, pad,
             (unsigned long)image_size, (unsigned long)file_size);

    f = _wfopen(path, L"wb");
    if (f == NULL) {
        log_msg("framecapture: _wfopen(%ls, wb) failed (errno=%d: %s)",
                 path, errno, strerror(errno));
        return 0;
    }

    file_hdr[0] = 'B'; file_hdr[1] = 'M';
    memcpy(file_hdr + 2, &file_size, 4);
    file_hdr[6] = file_hdr[7] = file_hdr[8] = file_hdr[9] = 0;
    {
        DWORD offbits = 14u + 40u;
        memcpy(file_hdr + 10, &offbits, 4);
    }

    memset(info_hdr, 0, sizeof(info_hdr));
    {
        DWORD biSize = 40, biPlanes_bitcount, biCompression = 0;
        LONG biWidth = (LONG)width, biHeight = (LONG)height; /* positive => bottom-up rows */
        WORD planes = 1, bitcount = 24;
        memcpy(info_hdr + 0, &biSize, 4);
        memcpy(info_hdr + 4, &biWidth, 4);
        memcpy(info_hdr + 8, &biHeight, 4);
        memcpy(info_hdr + 12, &planes, 2);
        memcpy(info_hdr + 14, &bitcount, 2);
        memcpy(info_hdr + 16, &biCompression, 4);
        memcpy(info_hdr + 20, &image_size, 4);
        (void)biPlanes_bitcount;
    }

    if (fwrite(file_hdr, 1, sizeof(file_hdr), f) != sizeof(file_hdr) ||
        fwrite(info_hdr, 1, sizeof(info_hdr), f) != sizeof(info_hdr)) {
        log_msg("framecapture: fwrite(header) failed (errno=%d: %s, ferror=%d)",
                 errno, strerror(errno), ferror(f));
        fclose(f);
        return 0;
    }

    /* BMP stores rows bottom-to-top. */
    for (y = 0; y < height; y++) {
        const unsigned char *src_row = pixels + (size_t)(height - 1 - y) * row_pitch;
        UINT x;
        for (x = 0; x < width; x++) {
            const unsigned char *px = src_row + (size_t)x * 4;
            unsigned char bgr[3];
            if (swap_rb) {
                bgr[0] = px[2]; /* B <- source B (byte 2 of R,G,B,A) */
                bgr[1] = px[1]; /* G */
                bgr[2] = px[0]; /* R <- source R (byte 0) */
            } else {
                bgr[0] = px[0]; /* already B,G,R,... */
                bgr[1] = px[1];
                bgr[2] = px[2];
            }
            if (fwrite(bgr, 1, 3, f) != 3) {
                log_msg("framecapture: fwrite(pixel) failed at row=%u col=%u (errno=%d: %s, ferror=%d)",
                         y, x, errno, strerror(errno), ferror(f));
                fclose(f);
                return 0;
            }
        }
        if (pad > 0 && fwrite(zero_pad, 1, pad, f) != pad) {
            log_msg("framecapture: fwrite(row padding) failed at row=%u pad=%u (errno=%d: %s, ferror=%d)",
                     y, pad, errno, strerror(errno), ferror(f));
            fclose(f);
            return 0;
        }
    }

    fclose(f);
    return 1;
}

/* Fail-safe by construction: every early-return just skips the capture and
 * logs why; nothing here can crash the game or leave D3D/file state
 * partially modified from the game's point of view (we only ever touch our
 * own staging texture and our own output file). */
static void do_capture(void) {
    ID3D11Texture2D *backbuffer = NULL;
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    int swap_rb;
    wchar_t out_dir[MAX_PATH];
    wchar_t out_path[MAX_PATH];

    if (!d3d_capture_ready()) {
        log_msg("framecapture: trigger seen but d3d_capture not ready yet; skipping");
        return;
    }

    if (g_d3d.format == DXGI_FORMAT_R8G8B8A8_UNORM || g_d3d.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        swap_rb = 1;
    } else if (g_d3d.format == DXGI_FORMAT_B8G8R8A8_UNORM || g_d3d.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        swap_rb = 0;
    } else {
        log_msg("framecapture: unsupported back-buffer format %d; skipping (only R/B8G8R8A8_UNORM[_SRGB] handled)",
                 (int)g_d3d.format);
        return;
    }

    hr = IDXGISwapChain_GetBuffer(g_d3d.sc, 0, &IID_ID3D11Texture2D, (void **)&backbuffer);
    if (FAILED(hr) || backbuffer == NULL) {
        log_msg("framecapture: GetBuffer(0) failed (hr=0x%08lX); skipping", (unsigned long)hr);
        return;
    }

    memset(&desc, 0, sizeof(desc));
    desc.Width = g_d3d.width;
    desc.Height = g_d3d.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = g_d3d.format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    hr = ID3D11Device_CreateTexture2D(g_d3d.dev, &desc, NULL, &staging);
    if (FAILED(hr) || staging == NULL) {
        log_msg("framecapture: CreateTexture2D(staging) failed (hr=0x%08lX); skipping", (unsigned long)hr);
        ID3D11Texture2D_Release(backbuffer);
        return;
    }

    ID3D11DeviceContext_CopyResource(g_d3d.ctx, (ID3D11Resource *)staging, (ID3D11Resource *)backbuffer);

    memset(&mapped, 0, sizeof(mapped));
    hr = ID3D11DeviceContext_Map(g_d3d.ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || mapped.pData == NULL) {
        log_msg("framecapture: Map(staging, READ) failed (hr=0x%08lX); skipping", (unsigned long)hr);
        ID3D11Texture2D_Release(staging);
        ID3D11Texture2D_Release(backbuffer);
        return;
    }

    capture_out_dir(out_dir, MAX_PATH);
    if (out_dir[0] == L'\0') {
        log_msg("framecapture: LOCALAPPDATA unavailable; skipping");
        ID3D11DeviceContext_Unmap(g_d3d.ctx, (ID3D11Resource *)staging, 0);
        ID3D11Texture2D_Release(staging);
        ID3D11Texture2D_Release(backbuffer);
        return;
    }
    /* Best-effort; if it already exists this just fails harmlessly and
     * _wfopen_s below will report the real problem if the dir truly isn't
     * there. */
    CreateDirectoryW(out_dir, NULL);
    swprintf(out_path, MAX_PATH, L"%s\\capture_%llu.bmp", out_dir,
             (unsigned long long)GetTickCount64());

    if (write_bmp(out_path, (const unsigned char *)mapped.pData, g_d3d.width, g_d3d.height,
                  mapped.RowPitch, swap_rb)) {
        log_msg("framecapture: wrote %ux%u capture to %ls", g_d3d.width, g_d3d.height, out_path);
    } else {
        log_msg("framecapture: failed to write BMP to %ls", out_path);
    }

    ID3D11DeviceContext_Unmap(g_d3d.ctx, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
    ID3D11Texture2D_Release(backbuffer);
}

void framecapture_on_present(UINT64 frame_number) {
    wchar_t path[MAX_PATH];

    if (!g_fc_env_checked) {
        char flag[8];
        DWORD len = GetEnvironmentVariableA("TEWVR_FRAMECAPTURE", flag, sizeof(flag));
        g_fc_enabled = (len > 0 && len < sizeof(flag) && strcmp(flag, "1") == 0);
        g_fc_env_checked = 1;
        if (g_fc_enabled) {
            log_msg("framecapture: enabled (TEWVR_FRAMECAPTURE=1); watching for "
                     "%%LOCALAPPDATA%%\\TEWVR\\capture.txt every %d frames",
                     FRAMECAPTURE_CHECK_FRAMES);
        }
    }
    if (!g_fc_enabled) {
        return;
    }
    if ((frame_number % FRAMECAPTURE_CHECK_FRAMES) != 0) {
        return;
    }

    capture_trigger_path(path, MAX_PATH);
    if (path[0] == L'\0' || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        return; /* not triggered */
    }

    /* Consume the trigger before capturing, so a slow/failed capture can't
     * cause the same trigger to be picked up twice. */
    DeleteFileW(path);
    log_msg("framecapture: triggered by capture.txt at frame %llu", (unsigned long long)frame_number);
    do_capture();
}
