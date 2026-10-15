# 2026-10-15: why the first real headset look failed (`/pd`, dev PC, game not launched)

Diagnosis of Tefa's four-way failure on 2026-10-11 (home PC, Quest 3), read from the code and the run notes
in `dev-archive/recon/2026-10-11-home-headset-first-look/` (on `stereo-6dof-core`). **The game was not
launched; nothing here has been run in the game.** The full run log stayed on the home PC; only the summary
is in git, which limits what could be checked.

## 1. "Menus in both eyes, but two separate screens at once": the depth setting (explained)

The home `tewvr.ini` had `STEREO_K = 2.0`, `STEREO_C = 0.98`. The eye matrix moves screen x by
`s·k·B/w + s·k·(A − c)` (proven by `tools/stereo_k_test.c`). The lens's A is 1.00000 (every measured lens,
2026-10-06 and 2026-10-10), so the second term never fades with distance: everything far away is pushed
**0.04 screen units (2% of the picture width) outward in each eye**. In a headset, where each eye's picture is placed at that eye's own
view, the right split for far-away things is zero; 2% per eye is about 1.5° outward per eye, which the eyes
cannot fuse, so they see two pictures. `[inferred-static 2026-10-15]`
- It fits the only measurement we have: on 2026-10-06 (dev PC, 1280 wide, same K and C) the far facade sat
  **50 px** apart; the formula gives 2 × 0.04 × 640 = **51.2 px** `[verified-numerically 2026-10-15]` (the
  measurement itself `[verified-live 2026-10-06, n=1]`).
- Why menus showed it: a menu with a 3D background is drawn with a perspective camera, so it gets the eye
  shift even though flat menu text does not.
- **Fix:** `STEREO_C = auto` (new; follows the measured A each frame) or `1`. Any other value now prints a
  WARNING line in the log. `K = 2.0` itself was a flat-screen test strength; its right headset value is open.

## 2. "Picture 2D, world moves with the head, a flickering screen stuck to the view": not settled

What would produce exactly this: world draws left out of both the eye shift and the head turn, while the
picture is still sent with the head's pose (so it rides along with the head). The code leaves a draw out when
its lens shape differs by more than 3% from the "main" lens (`head_track_other_camera`).
- The run notes give two shapes: the first head-tracked frame's picture rect implies **2.421** (3440 × 1421),
  gameplay's logged lens **2.306**, 4.7% apart, outside the 3% band. `[reported]` (from the summary)
- One way the main lens can be wrong: it was the median of each field taken separately over all samples. When
  a frame mixes cameras and none holds a majority, that builds a lens that is no camera at all
  (`tools/head_pick_test.c`: 1,173 of 5,000 such frames; with a majority camera the old method was already
  right). **Fixed** either way: the main lens is now the median of the biggest same-shape group.
  `[verified-numerically 2026-10-15]` (15,008 checks, planted old method caught)
- ⚠️ This is a **possibility, not the cause**: the 2.306/2.421 gap may equally be the lens changing between
  the first frame (menu or cutscene) and gameplay. Alternatives that would give the same picture: the head pose
  sent with a picture that was not turned (no lens on that frame), or the eye images not reaching the headset
  in step (`tew_share_capture` skips a busy eye texture).
- **What tells them apart:** every progress line now logs `head_track: since last line N draw(s) TURNED ...,
  M left alone as other cameras (shapes a..b)`. In gameplay, M much larger than N with shapes near the world's
  = the world is being left out (this cause). N large and M small = the world is turned, look elsewhere.
- "Lens measured only once" in the summary is most likely harmless: the lens line is only printed when the
  lens changes by more than 1%; cutscenes zoom (printed every second on 2026-10-10), gameplay does not.
  `[inferred-static 2026-10-15]`

## 3. "Controller did not work": the Xbox pad, so a fault (answered 2026-10-15)

Tefa: *"the xbox controller, i didn't expect motion controllers to work"*. The game reads the pad through XInput
(`XINPUT1_3`), and a virtual XInput pad drove it with the mod installed on 2026-09-08, so the proxy's forwarding is
not the first suspect. Possibilities, none checked `[hypothesis]`:
- **Focus:** the game ignores the pad when its window is not in front, and with the headset running something else
  held focus (the headset app, or the 3440 x 1440 fullscreen window). This build already auto-minimises on focus loss.
- **The headset side:** our OpenXR thread or the runtime taking the pad.
- **Steam Input** remapping the pad for the headset session.

**What tells them apart:** on the dev PC with the simulator (STEREO = 1, OPENXR = 1, HEADTRACK = 1, 1280 x 720 window),
press the virtual pad in the pause menu with the game in front, then behind another window. Works in front, fails
behind = focus. Fails in front too = the headset side; repeat with OPENXR = 0 to confirm.

## 4. "Frame rate poor (~22 per eye)": expected with this design

Alternate-frame stereo halves the rate per eye by design: ~44 game frames/s at 3440 × 1440 → ~22 per eye. The
1280 × 720 window (the standing rule) should raise it; whether the game itself caps the rate is not known (no
frame-cap setting found among its console settings; `r_swapInterval` exists). Measure gameplay frames/s at
1280 × 720 with the headset output on.

## Installed

Dev PC: `winmm.dll` `e21d0c95e25a` (backup `tewvr-backup-2026-10-15-pre-pick`), `STEREO_C = auto`, everything
else off as before. Home PC: reminder raised to install the same build and settings.
