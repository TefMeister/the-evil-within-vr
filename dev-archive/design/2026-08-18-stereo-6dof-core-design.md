# Design: Stereo 6DOF Core (Sub-project 1)

**Project:** The Evil Within VR mod
**Sub-project:** 1 of 5 — the stereo rendering foundation
**Date:** 2026-08-18
**Status:** Approved design, pending implementation plan

## Summary

This sub-project builds the foundation the whole VR mod rests on: rendering the
game's scene from two eye viewpoints per frame, with correct per-eye view and
projection matrices, and presenting the result as a side-by-side stereo image.

The milestone deliberately stops at **stereo correctness on the flat monitor**.
Head tracking and OpenVR compositor submission are explicitly out of scope for
this sub-project; they are the next stage. The core is, however, architected so
those slot in without restructuring (see "The VR-submit seam").

## Goals

- Render the game scene twice per frame, once per eye, with correct per-eye
  view and projection matrices.
- Present the two eyes side-by-side on the game's own back-buffer (flat
  monitor), verifiably correct: parallax in the right direction, both eyes
  showing offset viewpoints, geometry neither frozen nor black.
- Do this through a proxy DLL that loads cleanly past the Steam DRM, with a
  fail-safe fallback to the game's normal mono rendering if anything goes wrong.
- Leave a clean seam for the next stage to add head tracking and OpenVR submit.

## Non-goals (this sub-project)

- Head tracking / 6DOF head pose from a headset (next stage).
- OpenVR / SteamVR compositor submission (next stage).
- Motion controllers, gameplay interactions, body, roomscale (later stages).
- Performance optimization. Per the standing rules, the development PC is
  low-powered and stereo double-rendering will be slow there; that is expected
  and non-diagnostic. Correctness is judged by eye on the dev PC; performance is
  judged later on the home PC.

## Key decisions (approved)

- **VR runtime target (for the next stage):** OpenVR / SteamVR. Chosen to match
  the proven Quest 3 + Virtual Desktop + SteamVR setup from the Psychonauts mod.
  Not exercised in this sub-project, but the submit seam is shaped for it.
- **Milestone scope:** stereo correctness first, validated side-by-side on the
  monitor, to isolate the reverse-engineering risk from the runtime-bridge risk.
- **Injection vector:** proxy `winmm.dll` as the loader, plus MinHook trampoline
  hooks on the specific D3D11 / DXGI methods we need. (Refined from `dxgi.dll`
  during planning — see the Architecture section for why.)
- **Stereo mechanism:** true double-render (real geometry rendered twice from
  two viewpoints), not depth-buffer reprojection. True double-render is the only
  path to correct depth everywhere and is the masterpiece-grade approach; its
  only real cost is performance, which the standing rules tell us to ignore on
  the dev PC.

## Architecture

### Injection vector: proxy `winmm.dll` loader + MinHook

The game loads `winmm.dll` itself (confirmed in the live module list during the
feasibility spike). A proxy DLL placed next to the game executable is loaded as
part of normal startup — **after** the Steam CEG DRM has already unwrapped the
real code in memory — so it sidesteps the launch-time anti-debug entirely, with
no external injector required. This mirrors the proven proxy pattern from the
Psychonauts mod (there a `d3d9.dll` proxy).

**Why `winmm.dll` rather than `dxgi.dll`:** the original spec named `dxgi.dll`,
since DXGI owns the swap-chain and `Present`. In planning, that proved fragile:
`dxgi.dll` exposes internal *ordinal* exports that `d3d11.dll` depends on, and a
proxy that forwards those imperfectly breaks device creation. `winmm.dll` is a
much safer loader — all of its exports are named and trivially forwarded, and we
do not need to intercept any winmm call; we only need our code running in the
process. We then reach D3D11 through MinHook rather than through the proxy
itself (next paragraph). The two concerns — getting our code loaded, and hooking
the renderer — are cleanly separated.

Responsibilities of the proxy:

1. Forward every real `winmm.dll` export the game imports to the genuine system
   DLL (resolved at runtime by absolute path, so no system DLL is redistributed),
   so the game runs unchanged whether the mod is active, inactive, or falling
   back.
2. On load, spawn a bootstrap thread that installs the D3D11 hooks.

Reaching D3D11 without proxying DXGI: MinHook needs the addresses of the
swap-chain and device-context methods. We obtain them by momentarily creating a
throwaway `ID3D11Device` and `IDXGISwapChain`, reading the method pointers from
their vtables (which are shared by the game's real objects), and releasing the
throwaway objects. We then install trampoline hooks on the methods we need:
`IDXGISwapChain::Present` (frame boundary) and the constant-buffer upload path
used for the camera matrices (see below). The game's `ID3D11Device`,
`ID3D11DeviceContext`, and `IDXGISwapChain` are captured on the first hooked
`Present`.

The winmm forwarders are interface metadata we generate (export-name stubs), not
game content — safe to commit. The exact forwarding list is an implementation
detail for the plan.

### The reverse-engineering task (the heart of this sub-project)

Everything depends on locating two things in the running engine:

1. **The camera view and projection matrices** as uploaded to the GPU.
2. **A scene-render entry point that can be safely invoked twice per frame**,
   each time with a different camera, without corrupting engine state.

Method:

- Hook the D3D11 constant-buffer path (`ID3D11DeviceContext::VSSetConstantBuffers`,
  `UpdateSubresource`, and `Map` / `Unmap`) and scan uploaded buffer contents
  for 4x4 float matrix data that changes coherently as the camera moves.
- Corroborate candidates against the `g_fov` and camera cvars already located in
  memory during the spike (cvar name table confirmed at a stable address; no
  ASLR on this build, so addresses are stable between runs).
- Use x64dbg to trace the render-loop structure around `Present` on the named
  "Render Thread" to identify the scene-render call and confirm it can be
  re-entered safely.

Output of this step: the address/shape of the camera matrix constant buffer, and
the identified render entry, documented in the dev-archive and modding-notes.

### Per-eye rendering path

Once the camera matrix and render entry are known:

- **Frame boundary:** the `Present` hook marks the end of a frame.
- **Per eye (left, then right):**
  - Compute the eye view matrix: the base camera view translated by ±IPD/2
    along the camera's right axis. For this milestone the IPD is a fixed
    configurable value; there is no head pose from a headset yet.
  - Compute the eye projection matrix (per-eye, symmetric for the milestone;
    off-axis/asymmetric projection is deferred to the OpenVR stage where real
    HMD frustum tangents are available).
  - Override the camera matrices at the constant-buffer hook.
  - Invoke the scene render, targeting the left or right half of the back-buffer
    via a D3D11 viewport.
- After both eyes are rendered, the game's own `Present` displays the completed
  side-by-side image.

### The VR-submit seam (future-proofing)

The core renders into an abstract stereo sink interface (conceptually
`IStereoSink` with a per-eye "here is the rendered eye texture / viewport"
call). This sub-project provides one implementation: **BackbufferSink**, which
writes the two eyes side-by-side to the game's back-buffer for the monitor.

The next stage adds a second implementation, **OpenVRSink**, which submits each
eye texture to the SteamVR compositor and feeds head pose back into the per-eye
view-matrix computation — with no change to the core rendering logic. Keeping
this seam explicit now is what makes the staged plan clean.

### Configuration knobs

Runtime knobs via environment variables (and/or a simple ini), following the
`PSYVR_*` pattern from the Psychonauts mod. Proposed initial set:

- `TEWVR_ENABLE` — master on/off (default on when the proxy is present).
- `TEWVR_IPD` — eye separation (default a correctness-first neutral value).
- `TEWVR_FOV_SCALE` — per-eye FOV scale (default 1.0 = no-op).
- `TEWVR_CONVERGENCE` — convergence distance (default a neutral value).
- `TEWVR_SWAP_EYES` — debug toggle to swap left/right (default off).

All defaults are correctness-first; real tuning happens later on the home PC.

### Error handling — fail-safe

- If the proxy cannot resolve the real `winmm.dll`, or MinHook cannot install a
  hook, or the throwaway device cannot be created, or the camera matrix cannot be
  found, the mod **falls through to the game's normal mono rendering** and never
  crashes or hangs the game.
- Verbose logging to a file, with an abort-on-fail protocol: any unexpected
  state is logged clearly with enough context to diagnose, and the mod disables
  its stereo path rather than proceeding on bad assumptions.
- This matches the established safety protocol from prior projects.

## Data flow (one frame)

```
Present hook fires (frame boundary)
  └─ StereoCore.RenderFrame()
       ├─ for eye in { Left, Right }:
       │    ├─ viewMatrix  = BaseCameraView shifted by ±IPD/2
       │    ├─ projMatrix  = per-eye projection (symmetric, FOV_SCALE applied)
       │    ├─ install matrix override at camera constant-buffer hook
       │    ├─ set D3D11 viewport to left/right half of back-buffer
       │    └─ invoke engine scene-render  → BackbufferSink (this milestone)
       └─ return; game's real Present shows the side-by-side image
```

## Testing / validation

- **Milestone success criteria:** correct side-by-side stereo visible on the
  monitor — both eyes render the scene from horizontally offset viewpoints,
  parallax is in the correct direction (near objects diverge more than far
  ones), and geometry is neither frozen on one eye nor black.
- **How validated:** by eye on the development PC, focusing on correctness, not
  frame rate. A frozen or black eye, or inverted parallax, is a real bug; a low
  frame rate is not.
- **No headset required** to close this sub-project.
- Regression guard: with `TEWVR_ENABLE=off` (or the proxy removed), the game
  must render and play exactly as stock.

## Repository and build

- New proxy project (`proxy-winmm/`) in the **`the-evil-within-vr-mod`** repo (C,
  compiled as `winmm.dll` with the llvm-mingw toolchain, using MinHook), with a
  `build.ps1` build script. **This repo is push-gated: nothing is pushed to it
  without explicit approval.**
- Only files we create are committed. No game files, ever. A `.gitignore` guards
  against accidental inclusion of binaries/assets.
- The dev-archive and modding-notes repos are updated after each notable success
  or failure, per the standing rules.

## Risks and open questions

- **Primary risk:** safely invoking the engine's scene render twice per frame.
  Some engines cache per-frame state (culling results, visibility, command
  lists) in a way that makes a naive second invocation corrupt or degenerate.
  Mitigation: the RE step explicitly studies re-entrancy before we commit to the
  double-render mechanism; if full re-invocation proves unsafe, the fallback is
  to duplicate at the draw-call level (re-issue the frame's draws with the
  second eye's matrices bound), which is more invasive but avoids re-running
  engine-side per-frame setup.
- **Matrix layout:** row-major vs column-major and the exact transform
  decomposition must be confirmed empirically before eye offsets are applied.
- **Which constant-buffer upload path the engine actually uses** (map/discard vs
  UpdateSubresource) is unknown until hooked; the plan covers hooking all three
  candidates.

## Next stage (for context, not this sub-project)

Sub-project 2 adds the `OpenVRSink`: SteamVR compositor submission, real HMD
frustum projection, and 6DOF head pose feeding the per-eye view matrices —
reusing this core unchanged.
