#pragma once

#include "camera.h"

/*
 * The test matrix K that mvp_patch.c left-multiplies onto every patched
 * draw's MVP (mvp' = K * mvp). Split out of mvp_patch.c on 2026-09-30 so the
 * three test knobs live in one small file instead of growing a 2,400-line
 * one.
 *
 * Knobs (environment or tewvr.ini, TEWVR_ prefix optional, read once):
 *   TEST_YAW    degrees, the original clip-space yaw. Proves the patch
 *               lands, but collapses the frame into slivers (camera.h).
 *   TEST_ROLL   degrees, tilts the picture about the screen centre. Readable.
 *   TEST_SHIFT  normalised screen units, slides the picture sideways.
 *   TEST_ASPECT back-buffer width / height for TEST_ROLL (default 16/9).
 *
 * K = Shift * Roll * Yaw (yaw applied first). All unset/0 gives identity.
 */

/* Pure: builds K from the four numbers. No logging, no config reads, so
 * tools/test_k_test.c can test the exact shipped maths. */
Mat4 test_k_build(float yaw_deg, float roll_deg, float shift_ndc, float aspect);

/* Reads the knobs, logs what is armed, and returns K. Call once, on the
 * bootstrap thread, before any hooked draw can run. */
Mat4 test_k_from_config(void);
