#pragma once

#include <windows.h>

/*
 * SPIKE / NOT YET REVIEWED — Playbook Phase 2.3 (frame capture to disk),
 * explored 2026-08-21 at the user's request ("get the game running
 * yourself"), so a controller session can judge a visual question (e.g. did
 * TEWVR_TEST_YAW visibly rotate the world) from a captured image file
 * instead of a human watching the screen. Follows the same env-var-gated,
 * file-triggered-arm convention as seqdump.c's TEWVR_SEQDUMP_ARMFILE.
 *
 * Enable with TEWVR_FRAMECAPTURE=1. While enabled, every ~30 frames this
 * cheaply checks for %LOCALAPPDATA%\TEWVR\capture.txt (a GetFileAttributesW
 * existence check, not an open — same cost profile as seqdump's armfile
 * poll). When found: deletes the trigger file, copies the current
 * back-buffer to a BMP under %LOCALAPPDATA%\TEWVR\captures\, and logs the
 * path. Never crashes: any D3D or file-system failure is logged once and
 * skipped, falling through to normal rendering.
 *
 * Does NOT need the dummy-device vtable install contract other modules in
 * this file use (cbdump/shaderdump/seqdump/mvp_patch) — it hooks nothing;
 * it only reads g_d3d (already populated by d3d_capture_from_present()) on
 * the existing Present hook's thread, so there is nothing to "install".
 */
void framecapture_on_present(UINT64 frame_number);
