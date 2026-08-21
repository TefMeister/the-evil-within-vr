# The Evil Within — VR Engine Research

Reverse-engineering research toward a first-of-its-kind 6DOF VR conversion of
**The Evil Within (2014)**, whose engine (a heavily modified id Tech 5, Tango
Gameworks' "STEM") has never been brought into VR before.

This repository holds two things:

- **[`PLAYBOOK.md`](PLAYBOOK.md)** — a reusable, engine-agnostic, point-by-point
  method for taking *any* game whose engine nobody has converted to VR and
  getting it there. It is oriented around one North Star: **the game rendering
  in a headset with head tracking**, with everything else built on top. The same
  playbook is copied into each of our VR projects' research repos.
- **[`ENGINE-DOSSIER.md`](ENGINE-DOSSIER.md)** — the distilled, current-truth
  reference for *this* game's engine: renderer, threading model, how the camera
  transform reaches the GPU, the constant-buffer mechanism, the pass inventory,
  the console/cvar cheat sheet, and the dead ends that cost us time so they
  don't cost the next engine's.

The blow-by-blow development history lives in the sibling repositories
(`-dev-archive` for the messy in-progress record, `-modding-notes` for readable
field notes). This repo is the consolidated engine knowledge, not the diary.

## The five repositories for The Evil Within VR

Everything for this game lives in five repositories, each with one job — so you
always know where to look. You are in **the-evil-within-vr-engine-research**.

| Repository | What lives here |
| --- | --- |
| [the-evil-within-vr-mod](https://github.com/TefMeister/the-evil-within-vr-mod) | The mod itself — a `winmm.dll` proxy driving stereo via D3D11 hooks (pre-release; source). |
| [the-evil-within-vr-dev-archive](https://github.com/TefMeister/the-evil-within-vr-dev-archive) | Full development history — snapshots, probes, dead ends, raw recon. |
| [the-evil-within-vr-modding-notes](https://github.com/TefMeister/the-evil-within-vr-modding-notes) | Readable field notes / progress ledger. |
| [the-evil-within-vr-staging](https://github.com/TefMeister/the-evil-within-vr-staging) 🔒 | **Private** — unverified WIP builds, cross-machine handoff. |
| **the-evil-within-vr-engine-research** ← you are here | Distilled engine reference (dossier) + reusable VR RE playbook. |

## Status

Engine model largely built; the camera transform is fully located and the
per-eye override mechanism identified. Next is the keystone proof — taking
control of the world's camera — followed by stereo and the VR runtime. See the
dossier's status line and open-risks section.

## Scope, ethics, and legality

- This is a **non-commercial fan project**. It requires owning a legitimate copy
  of the game and **redistributes no original game assets** — only files we
  create. See [`.gitignore`](.gitignore).
- The techniques here (DLL proxying, hooking, injection, shader reflection)
  resemble malware only in tooling; the context is personal modding of a game we
  own.
- We **credit everyone** whose work or research this builds on, and we honour
  correction/removal requests from actual rights holders. See
  [`CREDITS.md`](CREDITS.md).

## Templates

New engine? Start its dossier from
[`templates/per-engine-research-template.md`](templates/per-engine-research-template.md).
