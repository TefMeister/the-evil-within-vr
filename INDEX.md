# Research index

Every research topic gathered for this project, newest first. Each row links to a self-contained
write-up in `topics/`. Status tags:

- 🆕 **new** — found, not yet acted on by the modding side.
- 👀 **reviewed** — a modding session has read it and factored it into a decision, but nothing shipped from it yet.
- ✅ **incorporated** — directly led to a real change (code, a test, a note) in one of the other five repos; linked below.
- ❌ **dead end** — checked out, didn't pan out; kept for the record so it isn't re-investigated from scratch.

| Date | Topic | Status | Summary |
| --- | --- | --- | --- |
| 2026-08-25 | [Native D3D11 OpenVR Submit — no readback needed](topics/2026-08-25-native-d3d11-openvr-submit-no-readback.md) | 🆕 new | Being already-native D3D11 means headset submission can use `IVRCompositor::Submit` with `TextureType_DirectX` directly on the rendered eye textures — none of the CPU-readback/D3D9Ex-shared-surface complexity Far Cry 2 and XIII needed applies here. |
| 2026-08-25 | [Submit must be called from the render thread](topics/2026-08-25-submit-must-be-called-from-render-thread.md) | 🆕 new | OpenVR requires Submit to be called from the same thread you render on — lines up naturally with this project's already-hooked Present thread, given the game's unusual 6-worker-thread command-recording architecture. A concrete design constraint for sub-project 2. |
| 2026-08-25 | [vorpX precedent + id Tech 5 caveat](topics/2026-08-25-vorpx-precedent-and-id-tech-5-caveat.md) | 🆕 new | vorpX works well for The Evil Within specifically (strong community feedback), but Wolfenstein: The New Order (also id Tech 5, different studio's fork) can't do true 3D — a reminder not to generalize from "id Tech 5" as a category in either direction. |

## How to add a topic

1. New file in `topics/`, named `YYYY-MM-DD-short-slug.md`.
2. One row added to the table above, newest at the top.
3. Update the status tag here as it moves through review → incorporated/dead-end (the modding side should update this when it acts on a lead, so the index reflects reality without the research side needing to poll).
