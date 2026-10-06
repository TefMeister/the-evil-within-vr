#pragma once

#include "camera.h"

/*
 * Alternate-frame stereo (2026-10-06, /pd, dev PC): one frame for the left eye, the next for the right, by swapping
 * the matrix mvp_patch.c left-multiplies onto every patched draw (mvp' = K * mvp) at each Present.
 *
 * WHY THIS SHAPE. The patch is now proven to reach the whole visible world (the Chapter 1 street included, seen by
 * Tefa live on 2026-10-06), so a per-eye K is all two eyes need. One K per frame, flipped at Present, is the route
 * that needs nothing new from the engine (Alan Wake uses the same shape, live-verified the same day).
 *
 * THE EYE MATRIX, in clip space, with no knowledge of the game's projection:
 *     x' = x + s * k * (z - c * w)          (s = -1 left eye, +1 right eye; y, z, w unchanged)
 * For a D3D perspective projection z = A*w + B, so on screen
 *     x'/w = x/w + s*k*B/w + s*k*(A - c)
 * The first added term falls off with distance (1/w = 1/view depth): that is the eye-separation parallax. The
 * second is a constant screen shift: that is convergence, and c tunes it (c = A puts convergence at infinity).
 * k is the strength. A and B are the game's depth constants, which are not known here; with the knobs the result
 * is a correct-in-shape eye pair whose strength and convergence are tuned by eye. A wrong sign of k (reversed-Z
 * depth makes B the other sign) shows up as near things popping out the wrong way: flip STEREO_K's sign.
 *
 * Knobs (tewvr.ini, TEWVR_ prefix optional, read once): STEREO = 1 to turn it on; STEREO_K strength (default 0.02);
 * STEREO_C convergence (default 1.0). The test K (TEST_ROLL etc.) still applies on top: K_frame = K_eye * K_test.
 */

/* Pure: the clip-space eye matrix above. eye_sign = -1 or +1. */
Mat4 stereo_eye_k(float eye_sign, float k, float c);

/* Reads the knobs and builds both eyes' K_frame from the test K. Call once after g_K is set. */
void stereo_afr_init(Mat4 test_k);

/* Called at every Present: flips the eye. No-op unless STEREO = 1. */
void stereo_afr_on_present(void);

/* The K the patch should use right now: points at one of two prebuilt matrices, switched atomically, so a draw on
 * another thread never reads a half-written matrix. */
const Mat4 *stereo_afr_current_k(void);
