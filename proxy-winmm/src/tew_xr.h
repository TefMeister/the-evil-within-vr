#pragma once

#include "d3d_capture.h"

/*
 * OpenXR output for the alternate-frame eyes (2026-10-06, /pd reader, dev PC). Ported from Alan Wake's aw_xr.c, with
 * the same threading: the headset thread has its own D3D11 device and session; the game thread only hands each eye's
 * picture over through a shared texture on the graphics card (tew_xr_eyes.c). Changed 2026-10-06 after a live stall.
 *
 * Settings (tewvr.ini, TEWVR_ prefix optional, read once at start):
 *   OPENXR = 1                    turn it on (off by default: nothing is loaded, nothing changes)
 *   OPENXR_RUNTIME_JSON = <path>  use this OpenXR runtime for this game only (sets XR_RUNTIME_JSON in this process)
 *   OPENXR_TEST_PATTERN = 1       fill the eye images with solid colours (left red, right blue) instead of the game
 *                                 picture: separates "layer not shown" from "picture not delivered"
 *   OPENXR_LAYERS = projection    submit each eye as a projection view instead of the default floating screen
 *                                 (only looks right once the game camera uses the headset's eye lenses - not yet)
 *
 * Needs openxr_loader.dll (64-bit) beside EvilWithin.exe; without it, a log line says so and the game runs as usual.
 */

/* Read the settings; if OPENXR = 1, start the background thread that loads the runtime. Call once, after the
 * Present hook is in (not from DllMain). */
void tew_xr_init(void);

/* At every Present, on the game's render thread, BEFORE the real Present, and only for the game's own swapchain:
 * copies this frame's picture into its eye's shared texture for the headset thread. Never calls OpenXR and never
 * waits (a busy texture is skipped). No-op when off or before the session runs. */
void tew_xr_on_present(IDXGISwapChain *sc);

/* Thread id of the headset thread (0 when OpenXR is off). Any Present on it belongs to the runtime. */
DWORD tew_xr_headset_tid(void);

/* Stops further headset work. Safe from DllMain (does not tear the session down under the loader lock). */
void tew_xr_shutdown(void);
