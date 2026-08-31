# the-evil-within-vr — `dev-archive/`

Full development archive for The Evil Within VR mod — snapshots, probes, raw
recon data, build scripts, and the messy in-progress history behind the mod.
Nothing here is curated; if we created it or used it during development, it
lives here.

**No game files are stored in this folder — only material we created
ourselves.** No executables, assets, textures, or other content belonging to
the game or its owners is uploaded here or anywhere in this project.

For the readable field notes, see
[`modding-notes/`](../modding-notes/).
For actual working mod releases, see
[`mod/`](../mod/).

## The folders for The Evil Within VR

Everything for this game lives in one repository, one folder per job — so you
always know where to look. You are in **`dev-archive/`**.

| Folder | What lives here |
| --- | --- |
| [`mod/`](../mod/) | The mod itself — a `winmm.dll` proxy driving stereo via D3D11 hooks (pre-release; source). |
| **`dev-archive/`** ← you are here | Full development history — snapshots, probes, dead ends, raw recon. |
| [`modding-notes/`](../modding-notes/) | Readable field notes / progress ledger. |
| [staging/the-evil-within-vr](https://github.com/TefMeister/staging/tree/main/the-evil-within-vr) 🔒 | **Private** — unverified WIP builds, cross-machine handoff. |
| [`engine-research/`](../engine-research/) | Distilled engine reference (dossier) + reusable VR RE playbook. |
| [`external-research/`](../external-research/) | Ongoing public-research leads, gathered separately from hands-on modding work. |

## The project

This project aims to add full 6DOF VR support to **The Evil Within** (2014,
Tango Gameworks / Bethesda), a game built on a heavily modified id Tech 5
engine with no existing VR modding framework. Everything here is built from
scratch through reverse engineering.

The long-term feature goals are the ones that would make a survival-horror VR
conversion feel complete: first-person 6DOF, roomscale, motion-controlled
weapons with two-handing and holsters, manual reload and slide/pump actions,
motion-controlled melee, physical item pickup, full body with a correct shadow,
aim-down-sights, a 3D ammo counter, collision-based door pushing, and physical
crouch. These are staged; see the notes for what is actually confirmed versus
still aspirational.

## Credits

See [CREDITS.md](CREDITS.md) — every source, tool, and prior work this project
builds on, credited by name, plus a standing notice on getting credited and on
respecting creators' wishes.

## Contributing & policy

See [CONTRIBUTING.md](CONTRIBUTING.md) — how we credit and link sources, our
**study-everything-public but write-our-own-code** rule (we copy no one else's
source code or files, any license or price), the terms for reusing our work
(free, with credit), and how to request a correction or removal.
