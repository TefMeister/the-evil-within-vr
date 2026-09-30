# 2026-09-30 — the tilt read: menus and the safe room tilt, the street does not

`/lm`, dev PC, two launches, driven with a virtual controller. Tefa stepped in once: "use the saved game,
this beginning sequence is taking too long". That changed the route to LOAD GAME → the Chapter 2 safe room.

## What was run

`winmm.dll` with `TEST_ROLL = 15` (a readable 15° tilt), first the 2026-09-30 `/pd` build (`5b3fbab74352`),
then the helper's split build (`81129b70db31`). Screenshots: `dev-archive/recon/2026-09-30-tilt-read/`.

## What tilted and what did not

| Where | World | Menus / 2D | Text |
| --- | --- | --- | --- |
| Logos, warning, title, options, save list | (none) | tilted | tilted, except hint lines |
| Chapter 1 street (gameplay) | **level**, with tilted ghost copies of fences and the police car | pause menu tilted | hint line level |
| Chapter 2 safe room (gameplay) | **tilted**, also after a camera turn | (none) | (none) |

All `[verified-live 2026-09-30]`, one scene each.

## What it means

- The patch reaches the 3D world in the safe room. The camera override works where it lands.
- On the Chapter 1 street it does not reach what we see, even though its counters said nearly every draw
  with a camera matrix was patched. So the counters do not tell us "the visible world is covered". Something
  on that street is drawn another way, or drawn again unpatched after ours. The ghost copies are the clue:
  the same object in two passes, one patched and one not.
- Menus are patched too. Real stereo will need them left alone (or treated separately), since a menu tilted
  or doubled per eye is wrong.

## Also done

- **The file split** (the helper's work) is live-tested and on `stereo-6dof-core` (`c1f2373`); the
  biggest file went from 2,464 lines to six files under 800.
- **Music muted** through the game's own Options (Music Volume 0, saved as `s_volume_music "-60"`).
- **Faster automation:** load the Chapter 2 save instead of CONTINUE (which sat on an old Chapter 1
  quick-save); close with WM_CLOSE (4 s). Loading overwrote the Chapter 1 autosave, so CONTINUE now goes to
  Chapter 2.

## What is NOT established

- Why the Chapter 1 street escapes. Open: other shaders, a redraw pass, tessellation (the dossier's Domain
  Shader gap). Each would show differently in a draw capture of that scene.
- Whether the safe room is fully covered or only mostly; one tilt screenshot cannot show a small miss.
