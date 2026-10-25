# 2026-10-11 home PC: Tefa's first look in the real headset

Build `winmm.dll` faeaff947610 (b9c20fb line + own hidden window for the startup test device).
Settings: STEREO=1 K=2.0 C=0.98, OPENXR=1, HEADTRACK=1. Game ran 3440x1440 (not the 1280x720 window).

## What Tefa saw (this is the observation)
- Frame rate poor (log: ~44 Presents/s, so ~22 per eye with AFR).
- Controller did not work; had to play with keyboard and mouse.
- Menus in both eyes, but convergence far too wide: two separate screens seen at once, very uncomfortable.
- In gameplay: a sideways, flickering screen stuck to the view; the picture is still 2D; the world moves
  with the head instead of staying still.

## What the log says
- Session running, both eyes shared, no headset errors.
- mvp_patch patches 25k-67k draws per 5 s in gameplay, so world draws ARE being reached.
- head_track measured the lens only once in gameplay (65.0 x 30.9 deg, picture aspect 2.306, 3440x1421 picture),
  against every second on 2026-10-10 in the cutscene. Not yet understood.

## First earlier failure the same evening
The first two launches ran flat: the startup test device failed with E_ACCESSDENIED (0x80070005) when made on
the desktop window, so no hooks went in. Fixed by using its own hidden window (commit on stereo-6dof-core).
