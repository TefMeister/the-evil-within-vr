# Engine Dossier — The Evil Within (2014) (id Tech 5 / Tango "STEM")

> Distilled current truth about this game's engine, as learned through the
> `PLAYBOOK.md` phases. Blow-by-blow history lives in the `-dev-archive` and
> `-modding-notes` repos; this is the consolidated reference.

**Status:** Phase 3→4 (engine model mostly built; keystone camera-override proof
next). **VR-readiness verdict:** **feasible** — camera transform fully located,
override mechanism identified; keystone proof and VR runtime still to do.

## 1. Identity
- The Evil Within (2014), PC (Steam, app id 268050). 64-bit, ~38 MB exe.
- Owned copy confirmed. Not a port.

## 2. Engine lineage
- Heavily modified **id Tech 5** (Tango Gameworks' "STEM" engine), directed by
  Shinji Mikami. Retains id Tech console/cvar culture and the renderprog
  parameter system.
- Middleware: **Morpheme / NaturalMotion** animation; `cudart64_40_17.dll`
  (CUDA, likely megatexture transcode); Bink video (`bink2w64.dll`).
- id Tech-style megatexture/virtual-texture assets (`virtualtextures/`).

## 3. Binary & memory
- 64-bit. **Module base 0x7FF62BB20000, stable across launches — no ASLR**, so
  RVAs are stable dev-to-dev.
- Renderer: **Direct3D 11 + DXGI** (verified: `d3d11.dll` + `dxgi.dll` load
  live; dedicated named "Render Thread"). Also loads xinput1_3 + dinput8.
- Console/cvars intact: launch with `+com_allowconsole 1`, open with **Insert**.
  `listcmds *` / `listcmds safe`; `idStudio` dev mode; `devmapjump <stage>`.

## 4. DRM / anti-debug & injection foothold
- **Steam CEG-wrapped.** Launching the raw exe *under a launch-time debugger*
  triggers a "Steam Error" — DRM refuses to unwrap. **Workaround:** launch via
  Steam (or the exe with `steam_appid.txt`=268050 present), let DRM decrypt in
  memory, then **attach** x64dbg.
- **Injection foothold: proxy `winmm.dll`.** Forward **all ~180** winmm exports
  via generated asm jump thunks (bink2w64 also imports winmm, so partial
  forwarding breaks it). Loads after DRM unwrap; no external injector. Chosen
  over a `dxgi.dll` proxy (dxgi's ordinal exports make it fragile).
- D3D hooked via MinHook using a throwaway dummy device/swapchain to read the
  vtables (Present = swapchain vtable index 8).

## 5. Threading & frame structure
- **Deferred-context renderer.** Six worker-thread **deferred contexts** record
  command lists; the **immediate context** replays them (~200–300
  `ExecuteCommandList` per frame) and also draws a minority directly.
- Proven by experiment: skipping `ExecuteCommandList` blacks out the world
  (scenery + character bodies gone; only hair, emissive/lights, and HUD remain)
  — so **the deferred command lists carry the bulk of the world** (~7× the
  immediate path's index volume).
- Frame: workers record (bind pooled constant buffers + draw) → immediate
  replays the lists → present. Shared state (VSSetShader/VSSetConstantBuffers)
  is on the immediate context; deferred contexts bind + draw.

## 6. Camera & projection delivery (the crucial section)
- **Per-draw MVP**, id Tech renderprog style. Every material vertex shader
  declares a slot-0 cbuffer `constantBufferV` whose reflection names the params,
  including the four rows `mvpmatrixx/y/z/w` of `M = P·V·Mmodel`; `SV_Position`
  is four `dp4`s against those rows. 145/167 captured shaders use it; the rest
  are post/depth-only.
- Row offset is **per-shader** (commonly +32; observed 0–192) and **not always
  contiguous** at +16/+32/+48 — always use the reflected per-row offsets, never
  assume contiguity.
- Skinning happens in model space **before** the MVP multiply (`jointBufferV`,
  ~8 KB palettes at slots 2/3) → unaffected by any camera-side change.
- **No shared view/projection buffer** exists (an early theory; disproved).
- **Per-eye override maths (the big win):** since every draw ends in
  `M = P·V·Mmodel`, left-multiply every per-draw MVP by **one constant per-eye**
  `K_eye = P_eye · T_eye(±IPD/2 x) · P⁻¹`, identical for all draws — no
  per-object knowledge. `P` from `g_fov` + aspect (or a shader receiving
  `projectionmatrixz`).

## 7. Constant-buffer fill mechanism
- The world's per-object MVP lives in a **small pool of ~6 persistently-mapped
  1920-byte constant buffers** (one ring per worker thread), bound to VS slot 0.
- The engine writes them by **CPU memcpy into the persistent mapping** — **no
  per-draw `Map`/`Unmap`/`UpdateSubresource`**, so Map hooks never see the
  writes (the trap that stalled discovery). Detected by reading the bound
  buffer's bytes at draw time when no Map is ever observed.
- Contents *are* readable at record time (proven via staging copy). The physical
  buffer is 1920 B; the shader reads only its small declared cb0 from the front,
  MVP at the reflected offset.
- **Chosen patch point:** at each deferred draw, read the bound slot-0 MVP,
  left-multiply by `K_eye`, write the patched cb0 into **our own** buffer, and
  rebind slot 0 before the original draw → our buffer is recorded into the
  command list. Preferred source read: capture the persistent CPU pointer (cheap,
  no cross-thread GPU op); staging read-back is the hardened fallback.

## 8. Pass inventory (by render target)
- Main scene: 1280×720 colour (formats 28/10/24/61/2 = G-buffer/HDR/aux) with
  1280×720 depth (fmt 44 = D24S8).
- Shadow passes: depth-only, square, 256²–2048² (fmt 53).
- Post/AA: downscaled 160×90 / 320×180 / 640×360 (bloom/SSAO chain); SMAA +
  motion vectors consume `inversemvpmatrix` / `prevmvpmatrix` (need consistent
  per-eye treatment later).
- HUD/post drawn on the immediate context, separable from the world.

## 9. cvar / console cheat sheet
| command / cvar | effect | use |
|---|---|---|
| `+com_allowconsole 1` (launch) | enables console | open with Insert |
| `listcmds *` / `listcmds safe` | list commands | discovery |
| `devmapjump <stage>` | jump to a chapter | reach a scene fast |
| `idStudio` | dev/cheat mode | dev tools |
| `g_fov` | field of view | projection / per-eye P |
| `pm_thirdPerson*` (Range/Height/Angle/…) | third-person camera | first-person via collapse |
| `g_showHud`, `g_showPlayerShadow` | HUD / shadow toggles | view polish |
| `g_viewNodalX` / `g_viewNodalZ` | view-origin nodal offsets | view polish |
| `pm_crouch/normalviewheight`, `pm_min/maxviewpitch` | view height / pitch clamps | comfort |
| `g_stopTime`, `g_debugPlayer`, `g_skipViewEffects` | time-stop / debug / skip post | testing |

## 10. Autonomous harness recipe (this game)
- **Not yet built (Playbook Phase 2 — a priority).** Current constraint: the
  game pauses when unfocused and rejects external SendInput/SetForegroundWindow,
  so gameplay captures have needed a human. Planned fix: drive input/camera from
  *inside* the injected process (hook xinput/dinput polling or the camera update;
  or use `devmapjump` + camera cvars), plus back-buffer capture to disk.
- Discovery instruments so far (env-gated, off by default): `TEWVR_SEQDUMP`
  (ordered per-draw event stream with ctx tags + command-list events),
  `TEWVR_SEQDUMP_ARMFILE` (file-triggered arm), `TEWVR_SKIPCL` (live skip of
  ExecuteCommandList — the experiment that mapped the frame), `TEWVR_CBPEEK`
  (draw-time constant-buffer content read), a shader-hash→mvp-offset reflection
  table, plus `shaderdump` + an offline DXBC disassembler.

## 11. Dead ends & false leads (save future time)
- A 384-byte "view matrix" (orthonormal + varying) was actually a **per-object
  cloth model matrix**, not the camera. Content heuristics match per-object
  matrices too — trust shader reflection, not heuristics.
- The 96-byte most-bound VS slot-0 buffer feeds **lighting**, not geometry.
- **Uniformly scaling** a view/projection is *visually invisible* (cancels in
  the perspective divide) — perturb **non-uniformly** to see an effect. Stereo's
  horizontal per-eye shift is non-uniform, so it does show.
- The world buffer "never being written" was a **persistent-map memcpy**, not a
  missing writer — don't assume a bound-but-unmapped buffer is static.

## 12. Open risks toward the North Star
- Keystone proof (own the camera on the deferred-recorded world) not yet
  demonstrated — next up.
- Double-render on a deferred-context engine: re-execute command lists per eye
  vs. record a second pass — mechanism chosen but unproven at runtime.
- Persistent-map source read must be made safe (an unhardened probe crashed the
  game once); AddRef/lifetime + cross-thread handling required before reuse.
- Post/AA (SMAA, motion vectors) per-eye consistency deferred.
