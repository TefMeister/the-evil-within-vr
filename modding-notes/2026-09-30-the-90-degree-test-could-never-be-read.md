# 2026-09-30 — the 90° test could never be read; a tilt can

`/pd`, dev PC. **The game was not launched, and nothing here has been run in the game.** Code on
`stereo-6dof-core` (`09afdde`, `62f04a9`).

## The finding

The board's top row asked for "a clean visible-rotation read" with `TEST_YAW = 90`: reach a
well-lit scene and list what rotates and what stays put. **That read was never possible.**

`TEST_YAW` is applied **after projection** (`mvp' = K · mvp`, clip space), as the dossier already
said. What had not been worked out is what that does at 90°: `x' = z`. Every vertex's screen
position becomes its **depth ratio** `z/w`, which is nearly the same number for everything in view.
So:

- the whole frame collapses into **one narrow vertical strip**, and
- `z' = −x` sends about half of the scene **outside the depth range**, where it is clipped.

`tools/test_k_test.c` measures it with an ordinary 60° 16:9 projection over 17,640 points spread
through the view `[verified-numerically 2026-09-30]`:

| | points on screen | where the survivors land |
| --- | --- | --- |
| before | 17,640 | the whole screen |
| after `TEST_YAW = 90` (normal depth) | 9,240 | x/w 0.950 to 1.000, a strip 2.5% of the width at the right edge |
| after `TEST_YAW = 90` (reversed depth) | 9,240 | x/w 0.000 to 0.050, a strip 2.5% of the width at the centre |
| a flat menu row | every point | one single column |

That is exactly the 2026-09-08 observation, "the pause menu rotated into vertical slivers" and a
black world `[verified-live 2026-09-08]`. The explanation is `[verified-numerically 2026-09-30]` for the
maths. That the game's slivers come from *this* is `[inferred-static 2026-09-30]`, since the real
projection was not read.

## What was built

Two new test settings that touch only clip `x` and `y`, so depth and `w` are untouched and nothing
extra is clipped. The picture stays readable: whatever the patch reaches moves, and whatever it misses
stays put.

- **`TEST_ROLL`**: tilts the picture about the screen centre. Aspect-corrected (`TEST_ASPECT`,
  default 16:9), so it is a rigid tilt, not a shear: on-screen distances are kept exactly and the turn is
  exactly the asked angle `[verified-numerically 2026-09-30]`.
- **`TEST_SHIFT`**: slides the picture sideways by the same amount near and far. That is how a per-eye
  image shift looks at infinity `[verified-numerically 2026-09-30]`.
- `K = Shift · Roll · Yaw`, all zero means identity (checked). The setup moved out of `mvp_patch.c`
  into a new small `test_k.c`, so the oversized file shrank by 12 lines rather than grew.
- Build: clean at `-Wall -Wextra`, all four `winmm` imports the game needs are exported
  `[compile-verified 2026-09-30]`. Test: 12/12 `[verified-numerically 2026-09-30]`.

## Deployed on the dev PC

`winmm.dll` `5b3fbab74352` (355,328 B) and `tewvr.ini` with `TEST_YAW = 0`, `TEST_ROLL = 15`. The
previous pair is backed up in `TheEvilWithin/_backup-2026-09-30-pd/`. Recorded in
`deployed/DESKTOP-V8GTSIR/`.

## What is NOT established

- That the game shows a tilted picture. Only a launch shows that.
- The game's real projection and depth direction. The strip's position depends on it; the conclusion
  that 90° can never be read does not.
- `TEST_ASPECT` assumes 16:9. In a 1280×720 window that is right; another window shape shears the tilt
  slightly, which is harmless for a coverage check.

## The next launch, and what each outcome means

Launch with the deployed `tewvr.ini`, reach a lit gameplay scene, take one screenshot:

- **The world tilts about 15° and some things stay level** → the level ones are the coverage gap. List
  them. This is the answer the old row wanted.
- **Everything tilts, the menu included** → full coverage of what is on screen. The menus are patched
  too, so a later HUD/menu exclusion is needed before real stereo.
- **Nothing tilts** → the new K is not reaching the draws. Read `tewvr.log` for the "test matrix K
  ACTIVE" line first.
- **Black or broken picture** → the tilt maths is wrong in the game even though it passed on paper.
  Stop and compare with the 12 checks.
