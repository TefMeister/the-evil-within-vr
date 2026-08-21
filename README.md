# the-evil-within-vr-mod

A work-in-progress VR mod for **The Evil Within** (2014, Tango Gameworks /
Bethesda), a game built on a heavily modified id Tech 5 engine with no existing
VR modding framework.

> ## ⚠️ This is NOT playable in VR yet
>
> **Do not install this expecting to play The Evil Within in a headset — it will
> not do that right now.** There is currently no VR output, no stereo rendering,
> no head tracking, and nothing shows up in a headset. This repository is an
> in-progress reverse-engineering effort, published in the open so the work is
> visible and backed up — not a released VR mod.
>
> **This notice will be changed only when the mod genuinely renders the game in a
> VR headset.** Until you see that update here, treat this as developer
> work-in-progress, not something to play.

**Status: pre-release, in active development — not VR-ready.** No playable
release yet. The current sub-project is the *stereo 6DOF core* — rendering the
game from two eye viewpoints per frame and presenting it side-by-side on a flat
monitor, as the foundation for full VR support (head tracking and headset output
come after that). Progress so far is the plumbing: a proxy DLL that loads into
the game, Direct3D 11 hooks, and locating the camera matrices — no VR yet.

This repository holds **only files we create** for the mod. No game files are
included, and none ever will be — the mod requires you to own a legitimate copy
of the game and redistributes no original assets.

For development history and reverse-engineering findings, see
[the-evil-within-vr-dev-archive](https://github.com/TefMeister/the-evil-within-vr-dev-archive).
For readable field notes, see
[the-evil-within-vr-modding-notes](https://github.com/TefMeister/the-evil-within-vr-modding-notes).

## The five repositories for The Evil Within VR

Everything for this game lives in five repositories, each with one job — so you
always know where to look. You are in **the-evil-within-vr-mod**.

| Repository | What lives here |
| --- | --- |
| **the-evil-within-vr-mod** ← you are here | The mod itself — a `winmm.dll` proxy driving stereo via D3D11 hooks (pre-release; source). |
| [the-evil-within-vr-dev-archive](https://github.com/TefMeister/the-evil-within-vr-dev-archive) | Full development history — snapshots, probes, dead ends, raw recon. |
| [the-evil-within-vr-modding-notes](https://github.com/TefMeister/the-evil-within-vr-modding-notes) | Readable field notes / progress ledger. |
| [the-evil-within-vr-staging](https://github.com/TefMeister/the-evil-within-vr-staging) 🔒 | **Private** — unverified WIP builds, cross-machine handoff. |
| [the-evil-within-vr-engine-research](https://github.com/TefMeister/the-evil-within-vr-engine-research) | Distilled engine reference (dossier) + reusable VR RE playbook. |

## Layout

- `proxy-winmm/` — the mod itself: a `winmm.dll` proxy that loads into the game
  and drives stereo rendering via MinHook-based Direct3D 11 hooks. See
  `proxy-winmm/USAGE.md` for install and usage once a build exists.

## Credits

See [CREDITS.md](CREDITS.md) — every source, tool, and prior work this project
builds on, credited by name, plus a standing notice on getting credited and on
respecting creators' wishes.

## Contributing & policy

See [CONTRIBUTING.md](CONTRIBUTING.md) — how we credit and link sources, our
**study-everything-public but write-our-own-code** rule (we copy no one else's
source code or files, any license or price), the terms for reusing our work
(free, with credit), and how to request a correction or removal.
