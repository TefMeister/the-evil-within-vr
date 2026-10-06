# 2026-10-06 (night, `/pd`, dev PC, Opus): head tracking built — the headset turns the picture

**The game was not launched, and nothing here has been run.**

Branch `stereo-6dof-core` `b4ab740` (proxy-winmm), `winmm.dll` `2b2b71f5fd58`, installed on the dev PC with
`HEADTRACK = 0` (off). Before it, `cd9efc2` brought the headset build that worked in the simulator earlier the same
evening (the `/lm` reader's `reader-xr-own-device-v2`) into the branch; it rebuilt byte-identical (`c0342d5c2975`).

## The idea

The patch only ever sees each draw's whole matrix, MVP = P·V·M (lens · camera · object). Turning the head by R must
give P·Rᵀ·V·M. That is K·MVP with **K = P·Rᵀ·P⁻¹**, one matrix for every draw, as long as P (the game's lens) is
known.

**The lens is measured, not guessed.** Every perspective MVP carries it in its rows:
row0 = sx·v0 + cx·v2, row1 = sy·v1 + cy·v2, row2 = A·v2 (+B), row3 = v2, where v0/v1/v2 are the camera axes in the
object's space. So cx = row0·row3/|row3|², sx = |row0 − cx·row3|/|row3| (same for y), A = row2·row3/|row3|². The
object's scale cancels. B never matters for a rotation. An unevenly scaled object fails two squareness checks and
is not used. A few draws per frame are sampled and the median taken.

**Other cameras are left alone.** Shadow maps and reflections draw with their own lenses. Square lenses are never
sampled, and once the main lens is known, any draw whose lens shape differs by more than 3% gets neither the head
turn nor the eye shift. With `HEADTRACK = 0` this filter is off, so plain stereo still shifts shadow passes.

**The headset is told where each picture was pointing.** The head pose used for a frame travels with that eye's
picture through the keyed-mutex handoff. The headset thread submits per-eye projection views with that orientation,
the game's own field of view (from the measured lens), and a picture rectangle cropped to the letterbox (the
game's lens aspect sy/sx against the back buffer's).

## Evidence

- `tools/head_k_test.c`: 3,000 random lenses, cameras and objects; the measured lens matches; K·MVP equals the
  independently built P·Rᵀ·V·M in screen x, y and depth; the identity head gives the identity; nine in ten unevenly
  scaled objects are rejected; head turned right → what was ahead moves left, head up → it moves down, head tilted
  left → the right side of the horizon drops; fov tangents and a 2.35:1 letterbox in 1280×720 come out right.
  **83,957 checks, 0 failed** `[verified-numerically 2026-10-06]`. A planted wrong-way rotation fails all three
  direction checks.
- Existing tests still pass: `stereo_flat_test` 16,002, `stereo_k_test`, `test_k_test` 12/12. 180 exports.
  Code shape scan clean `[compile-verified 2026-10-06]`.

## What is NOT established

- That the game's draws measure cleanly. The checks are tight (1e-3); if this game's matrices carry small errors,
  too few draws pass and no lens settles. The log says so: no `head_track: lens ...` line means this.
- Whether the letterbox is drawn as a smaller viewport (the code's assumption) or as bars over a full-height picture.
  The logged lens aspect decides it: about 2.35 means a viewport; about 1.78 means full height.
- Which way roll should go is fixed by the test against OpenXR's own conventions, but no headset has checked it.
- **Big head turns will show empty edges.** The game only draws what its own camera sees; turning the picture
  inside that view runs out after half the field of view. Turning the game's real camera is the lasting fix
  `[inferred-static 2026-10-06]`.

## The one test that answers it

`/lm`: `tewvr.ini` `STEREO = 1`, `OPENXR = 1`, `HEADTRACK = 1`, the 64-bit simulator json; reach the Chapter 1
street; read the log for the lens line; then turn the simulated head (yaw 20° right, pitch 10° up, roll 10°) and
capture both eyes after each. Expected: the street stays put in the world while the view turns (things slide left
on a right turn), menus and HUD stay level on the picture, shadows do not swim. A lens line with a nonsense field of
view, or no lens line at all, means the measurement, not the turning, is wrong.
