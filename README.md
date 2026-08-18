# the-evil-within-vr-mod

A work-in-progress VR mod for **The Evil Within** (2014, Tango Gameworks /
Bethesda), a game built on a heavily modified id Tech 5 engine with no existing
VR modding framework.

**Status: pre-release, in active development.** No playable release yet. The
current sub-project is the *stereo 6DOF core* — rendering the game from two eye
viewpoints per frame and presenting it side-by-side on a flat monitor, as the
foundation for full VR support (head tracking and headset output come next).

This repository holds **only files we create** for the mod. No game files are
included, and none ever will be — the mod requires you to own a legitimate copy
of the game and redistributes no original assets.

For development history and reverse-engineering findings, see
[the-evil-within-vr-dev-archive](https://github.com/TefMeister/the-evil-within-vr-dev-archive).
For readable field notes, see
[the-evil-within-vr-modding-notes](https://github.com/TefMeister/the-evil-within-vr-modding-notes).

## Layout

- `proxy-winmm/` — the mod itself: a `winmm.dll` proxy that loads into the game
  and drives stereo rendering via MinHook-based Direct3D 11 hooks. See
  `proxy-winmm/USAGE.md` for install and usage once a build exists.

## Credits

See [CREDITS.md](CREDITS.md) — every source, tool, and prior work this project
builds on, credited by name, plus a standing notice on getting credited and on
respecting creators' wishes.
