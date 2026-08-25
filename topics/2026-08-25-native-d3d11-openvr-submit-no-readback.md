# Being native D3D11 means headset submission can skip the CPU-readback/interop dance other fronts in this portfolio needed

**Status:** 🆕 new · **Priority:** very high — directly targets "getting it showing up inside the
headset" (sub-project 2), and simplifies the plan already sketched in `modding-notes/00-status.md`
("head tracking and OpenVR compositor submission... side-by-side on the flat monitor to prove stereo
correctness before adding... OpenVR compositor submission").

## What was found

OpenVR's compositor API natively accepts Direct3D 11 textures for eye submission —
`IVRCompositor::Submit()` takes an `ETextureType` parameter, and `TextureType_DirectX` is the
documented value for handing over a live `ID3D11Texture2D*` (via the standard `Texture_t` struct
wrapping the texture pointer and the D3D11 device). This is a first-class, intended usage pattern —
not a workaround — confirmed directly in the public `openvr.h` header and the OpenVR wiki's
compositor overview documentation.

## Why this matters enormously for this project specifically

Every other D3D9/D3D8-era project in this portfolio (Far Cry 2, XIII) had to solve a real, nontrivial
problem to get a legacy renderer's frame into SteamVR: Far Cry 2's plan is CPU `GetRenderTargetData`
readback (with a documented D3D9Ex→D3D11 shared-surface GPU-only path as a harder future
optimization, complete with a keyed-mutex incompatibility gotcha specific to that older API pairing);
XIII's Milestone 1 does the same CPU-readback dance for its D3D8 backbuffer. **The Evil Within doesn't
have this problem at all** — it's already a native D3D11 renderer. Once this project has two
correctly-rendered per-eye `ID3D11Texture2D` surfaces (which is exactly what the stereo 6DOF core
work already in progress produces — dual-eye rendering via the recorded-command-list replay
mechanism), **those textures can be submitted to `IVRCompositor::Submit()` directly, with
`TextureType_DirectX`, no CPU round-trip, no cross-API shared-surface interop, no D3D9Ex complexity
at all.** This is the simplest possible case of the "get it in the headset" problem this portfolio
has encountered so far — the hard part for this project is almost entirely on the *rendering* side
(already being solved), not the *submission* side.

## Concrete next step

When sub-project 2 (OpenVR compositor submission) begins, submit the two rendered eye textures
directly via `IVRCompositor::Submit(..., TextureType_DirectX, ...)` rather than designing around any
CPU-readback or shared-surface bridge — that entire category of complexity documented for this
portfolio's D3D9/D3D8 fronts (Far Cry 2, XIII) doesn't apply here. Confirm the exact `Texture_t`
struct fields and any D3D11-specific caveats (texture format, `MULTISAMPLE` restrictions if MSAA is
in use) directly against the current `openvr.h` header when implementation starts.

## Sources

- https://raw.githubusercontent.com/ValveSoftware/openvr/master/headers/openvr.h
- https://github.com/ValveSoftware/openvr/wiki/IVRCompositor_Overview
