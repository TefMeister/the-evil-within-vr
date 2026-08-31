# the-evil-within-vr — `modding-notes/`

Readable, trial-and-error field notes from an AI-assisted effort to build a
full 6DOF VR mod for **The Evil Within** (2014), a game running on a heavily
modified id Tech 5 engine with no existing VR modding framework.

These notes are the distilled, human-readable version of the work: what we
tried, what we learned, and the gotchas worth remembering. For the full raw
archive (probes, snapshots, and the messy in-progress history), see
[`dev-archive/`](../dev-archive/).
For actual working mod releases, see
[`mod/`](../mod/).

**No game files are included here — only material we wrote ourselves.** The
memory addresses, cvar names, and engine details recorded in these notes are
interoperability facts about how the game runs, not game assets.

## The folders for The Evil Within VR

Everything for this game lives in one repository, one folder per job — so you
always know where to look. You are in **`modding-notes/`**.

| Folder | What lives here |
| --- | --- |
| [`mod/`](../mod/) | The mod itself — a `winmm.dll` proxy driving stereo via D3D11 hooks (pre-release; source). |
| [`dev-archive/`](../dev-archive/) | Full development history — snapshots, probes, dead ends, raw recon. |
| **`modding-notes/`** ← you are here | Readable field notes / progress ledger. |
| [staging/the-evil-within-vr](https://github.com/TefMeister/staging/tree/main/the-evil-within-vr) 🔒 | **Private** — unverified WIP builds, cross-machine handoff. |
| [`engine-research/`](../engine-research/) | Distilled engine reference (dossier) + reusable VR RE playbook. |
| [`external-research/`](../external-research/) | Ongoing public-research leads, gathered separately from hands-on modding work. |

## Contents

- `00-status.md` — where the project stands right now.
- `01-tooling-setup.md` — the environment, and the Steam DRM gotcha.
- `02-static-recon.md` — what the executable's strings told us before running it.
- `03-live-debug-findings.md` — what the running game told us under a debugger.

## Credits

See [CREDITS.md](CREDITS.md) — every source, tool, and prior work this project
builds on, credited by name, plus a standing notice on getting credited and on
respecting creators' wishes.

## Contributing & policy

See [CONTRIBUTING.md](CONTRIBUTING.md) — how we credit and link sources, our
**study-everything-public but write-our-own-code** rule (we copy no one else's
source code or files, any license or price), the terms for reusing our work
(free, with credit), and how to request a correction or removal.
