# the-evil-within-vr-modding-notes

Readable, trial-and-error field notes from an AI-assisted effort to build a
full 6DOF VR mod for **The Evil Within** (2014), a game running on a heavily
modified id Tech 5 engine with no existing VR modding framework.

These notes are the distilled, human-readable version of the work: what we
tried, what we learned, and the gotchas worth remembering. For the full raw
archive (probes, snapshots, and the messy in-progress history), see
[the-evil-within-vr-dev-archive](https://github.com/TefMeister/the-evil-within-vr-dev-archive).
For actual working mod releases, see
[the-evil-within-vr-mod](https://github.com/TefMeister/the-evil-within-vr-mod).

**No game files are included here — only material we wrote ourselves.** The
memory addresses, cvar names, and engine details recorded in these notes are
interoperability facts about how the game runs, not game assets.

## Contents

- `00-status.md` — where the project stands right now.
- `01-tooling-setup.md` — the environment, and the Steam DRM gotcha.
- `02-static-recon.md` — what the executable's strings told us before running it.
- `03-live-debug-findings.md` — what the running game told us under a debugger.

## Credits

See [CREDITS.md](CREDITS.md) — every source, tool, and prior work this project
builds on, credited by name, plus a standing notice on getting credited and on
respecting creators' wishes.
