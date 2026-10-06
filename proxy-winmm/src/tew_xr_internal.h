#pragma once

/*
 * Shared between tew_xr.c (OpenXR thread: instance, own device, session, frame loop) and tew_xr_eyes.c (the per-eye
 * picture handoff between the game's device and the headset thread's device). Not public: only tew_xr.h is.
 *
 * THE HANDOFF (2026-10-06, reader, "own device" build). One texture per eye, created on the GAME's device with
 * D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX and opened on the headset thread's device with OpenSharedResource. Both sides
 * take its keyed mutex (key 0) with a ZERO timeout and simply skip when the other side holds it:
 *   game thread, at Present:  try-lock -> copy back buffer in -> unlock -> bump that eye's serial
 *   headset thread, per frame: if the serial moved: try-lock -> copy into its own "held" texture -> unlock
 * so neither thread ever waits for the other, and the newest finished picture always wins. The keyed mutex also makes
 * the GPU order the two devices' copies correctly.
 */

#include "d3d_capture.h"   /* COBJMACROS, windows.h, d3d11.h, dxgi.h */

#include <stdint.h>

/* Call once, before either thread uses the handoff. */
void tew_share_init(void);

/* GAME THREAD, at Present, before the real Present: copy back buffer 0 into eye `eye` (0/1), or both when eye < 0.
 * Never blocks: a busy eye texture is skipped. Returns 1 if at least one eye was written. */
int tew_share_capture(IDXGISwapChain *sc, int eye, const float pose[4]);   /* pose: head pose it was drawn with, or NULL */

/* Headset-thread side: everything here belongs to the headset thread and its device only. */
typedef struct {
    LONG gen;                      /* which published set is open (0 = none) */
    UINT w, h;
    DXGI_FORMAT fmt;
    ID3D11Texture2D *shared[2];    /* the game's textures, opened on our device */
    IDXGIKeyedMutex *km[2];
    ID3D11Texture2D *held[2];      /* our own copy of each eye's newest picture */
    LONG seen[2];                  /* game serial last copied into held[] */
    int have[2];                   /* held[e] holds a picture */
    float pose[2][4];              /* the head pose held[e] was drawn with (OpenXR x, y, z, w) */
} TewXrEyes;

/* Open the newest published set if it changed. 1 = a new set is open (rebuild swapchains), 0 = unchanged,
 * -1 = nothing usable yet. */
int tew_share_open(TewXrEyes *x, ID3D11Device *dev);

/* Copy each eye's newest finished picture into held[] when the game has written a new one. Never blocks. */
void tew_share_pull(TewXrEyes *x, ID3D11DeviceContext *ctx);

/* Release everything in x (headset thread). */
void tew_share_xr_release(TewXrEyes *x);

/* Diagnostics (v2): read by the headset thread's progress line. Game-side counts are read without a lock. */
typedef struct {
    unsigned long game_copies, game_busy;      /* game thread: eye copies made / skipped because the lock was held */
    unsigned long xr_copies, xr_busy;          /* headset thread: pulls into held[] / skipped because the game held it */
    unsigned long open_fail;                   /* failed opens of a published set */
    long last_open_hr;                         /* HRESULT of the last OpenSharedResource */
    LONG serial[2];                            /* game's current serial per eye */
} TewShareStats;
void tew_share_stats(TewShareStats *out);

/* Swapchain format for this back-buffer format: same typeless family (CopyResource rule), _SRGB member first.
 * 0 if the runtime offers none. */
int64_t tew_eyes_pick_format(const int64_t *fmts, uint32_t n, DXGI_FORMAT bb);
