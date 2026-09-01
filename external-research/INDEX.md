# Research index

**Last `/gr` pass: 2026-09-01 (second pass, estate sweep) — CHECK-IN.** Inbox drained: `/gs`'s
report that the command-list topic tagged its central limitation with an invented name. Fixed to
`[reported]`, precision kept in the prose. No new research this pass.

_Earlier the same day — FULL:_ inbox drained — and the drop in it (a `/sr` sweep note about
`+com_allowconsole 1` and `noclip`) was **redundant**: §9 and §10 of our own `ENGINE-DOSSIER.md` have
documented that console, its unlock, its command set and a deterministic launch recipe using it since
2026-08-20/21, in more detail than the drop carried. Recorded rather than quietly binned, because the
lesson is the sweep's own rule — *read the target project's dossier before filing a lead into it.*
The research budget went to §12's live open risk instead; see the new topic below.

Every research topic gathered for this project, newest first. Each row links to a self-contained
write-up in `topics/`. Status tags:

- 🆕 **new** — found, not yet acted on by the modding side.
- 👀 **reviewed** — a modding session has read it and factored it into a decision, but nothing shipped from it yet.
- ✅ **incorporated** — directly led to a real change (code, a test, a note) in one of the other five repos; linked below.
- ❌ **dead end** — checked out, didn't pan out; kept for the record so it isn't re-investigated from scratch.

| Date | Topic | Status | Summary |
| --- | --- | --- | --- |
| 2026-09-01 | [Command-list re-execution cannot do per-eye — and two documented `ExecuteCommandList` hazards](topics/2026-09-01-command-list-reexecution-cannot-do-per-eye-and-two-documented-hazards.md) | 🆕 new | Targets §12's *"re-execute command lists per eye vs. record a second pass — mechanism chosen but unproven"*. From Microsoft's own D3D11 docs: **a command list is immutable** and replays its recorded commands verbatim, so re-execution renders the same view twice; the escape of updating a matrix between the two executes cannot work **on this engine specifically**, because §11 already measured ~1900 draws sharing ~6 per-draw buffer identities — the same root cause that killed the batched snapshot. So the chosen record-time patch-and-draw-twice is not a preference, it is the only viable one of the two. Two hazards for Task 7: **`RestoreContextState=FALSE` returns the context to its DEFAULT state** after execution (and the docs recommend FALSE), so nothing is inherited; and `ExecuteCommandList` **returns `void` and can silently decline to execute** on a query-validation condition, still performing a `ClearState` — an invisible discard that would present as a patching bug. Guard: enable the D3D11 debug layer for bring-up. API semantics verified from first-party docs; whether this game uses queries or passes TRUE/FALSE is unmeasured. |
| 2026-08-25 | [Native D3D11 OpenVR Submit — no readback needed](topics/2026-08-25-native-d3d11-openvr-submit-no-readback.md) | 🆕 new | Being already-native D3D11 means headset submission can use `IVRCompositor::Submit` with `TextureType_DirectX` directly on the rendered eye textures — none of the CPU-readback/D3D9Ex-shared-surface complexity Far Cry 2 and XIII needed applies here. |
| 2026-08-25 | [Submit must be called from the render thread](topics/2026-08-25-submit-must-be-called-from-render-thread.md) | 🆕 new | OpenVR requires Submit to be called from the same thread you render on — lines up naturally with this project's already-hooked Present thread, given the game's unusual 6-worker-thread command-recording architecture. A concrete design constraint for sub-project 2. |
| 2026-08-25 | [vorpX precedent + id Tech 5 caveat](topics/2026-08-25-vorpx-precedent-and-id-tech-5-caveat.md) | 🆕 new | vorpX works well for The Evil Within specifically (strong community feedback), but Wolfenstein: The New Order (also id Tech 5, different studio's fork) can't do true 3D — a reminder not to generalize from "id Tech 5" as a category in either direction. |

## How to add a topic

1. New file in `topics/`, named `YYYY-MM-DD-short-slug.md`.
2. One row added to the table above, newest at the top.
3. Update the status tag here as it moves through review → incorporated/dead-end (the modding side should update this when it acts on a lead, so the index reflects reality without the research side needing to poll).
