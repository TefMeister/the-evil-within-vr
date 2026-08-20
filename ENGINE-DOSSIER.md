# Engine Dossier — The Evil Within (2014) (id Tech 5 / Tango "STEM")

> Distilled current truth about this game's engine, as learned through the
> `PLAYBOOK.md` phases. Blow-by-blow history lives in the `-dev-archive` and
> `-modding-notes` repos; this is the consolidated reference.

**Status:** Phase 3→4 (engine model built; keystone camera-override *code*
landed and reviewed clean 2026-08-20, runtime proof itself still pending a
human-witnessed session). **VR-readiness verdict:** **feasible** — camera
transform fully located, override mechanism identified, implemented, and
statically de-risked by an offline research pass (2026-08-20: buffer-identity
ambiguity resolved, SMAA/motion-vector shader fully characterised, a
tessellation/Domain-Shader gap identified for future work — see dev-archive
`notes/09-offline-research.md`); keystone proof and VR runtime still to do.

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
- **Chosen patch point (implemented, Task 6):** hook `ID3D11Device::CreateBuffer`
  to catch the pool buffers at creation time and capture a direct, `AddRef`'d
  CPU pointer via a foreign `Map` issued inline with creation (never `Unmap`d,
  mirroring the engine's own pattern) — proven live: 16/16 foreign `Map` calls
  succeeded in a menu-session smoke test, no conflict with the engine's own
  access observed. At each deferred draw: read the bound slot-0 MVP through
  that pointer, left-multiply by `K_eye`, write the patched cb0 into **our
  own** scratch buffer (a per-thread TLS ring, to avoid two worker threads
  ever `Map`ping the same scratch object), rebind slot 0 before the original
  draw → our buffer is recorded into the command list. (An earlier once-per-
  frame batched-staging-copy design was tried and rejected: too coarse to
  supply genuinely distinct per-draw data — see dead ends below.)
- **1920 bytes is not a unique fingerprint for the world pool** — a *different*,
  unrelated pair of 1920-byte buffers also exists (a per-frame global,
  refreshed once per `Present` via real `Map`/`Unmap` on the immediate
  context, bound at VS slot 2/3, not slot 0). The two are reliably told apart
  by binding slot + context (deferred, slot 0, never `Map`ped = the real
  pool; immediate, slot 2/3, `Map`ped every frame = the impostor), not by
  size alone. See §11.

## 8. Pass inventory (by render target)
- Main scene: 1280×720 colour (formats 28/10/24/61/2 = G-buffer/HDR/aux) with
  1280×720 depth (fmt 44 = D24S8).
- Shadow passes: depth-only, square, 256²–2048² (fmt 53).
- Post/AA: downscaled 160×90 / 320×180 / 640×360 (bloom/SSAO chain). The
  SMAA/motion-vector shader is now fully characterised by static disassembly
  (shader hash `736130AA89FA0E59`, `d3dcompiler_47.dll` via the project's
  offline `dxbc_disasm` tool, no game execution needed): `constantBufferV`
  holds `inversemvpmatrixx/y/z/w` at offsets 0/16/32/48 and
  `prevmvpmatrixx/y/z/w` at 64/80/96/112, plus `smaajitter` at 128. Body:
  reprojects a full-screen-triangle position through inverse-MVP then
  prev-MVP (two `dp4` chains) — standard temporal reprojection for motion
  vectors. Needs consistent per-eye treatment later (deferred, not scoped
  for the stereo-core milestone).
- HUD/post drawn on the immediate context, separable from the world.
- **Tessellation pipeline confirmed** (`r_allowTessellation` cvar exists).
  At least two skinned-mesh vertex shaders (`AF80AA9287F65EA7`, and by buffer
  layout likely `62F67B34913B0238`) have **no `SV_Position` in their VS
  output signature at all** — only `TEXCOORD0-4`/`WORLDPOS` — meaning the
  final clip-space transform happens downstream in a **Domain Shader**, not
  the VS. `mvp_patch` only patches VS-bound constant buffers, so draws using
  this path will fail-safe-skip (shader correctly reports "no MVP rows
  found") rather than rotate with the rest of the world during any yaw
  proof. Likely candidate: detailed character skin/face geometry (these
  shaders declare `jointBufferV`, i.e. skinned meshes). Not a patch bug if
  observed — a known gap for future work (would need to hook the Domain
  Shader's own constant buffers the same way). See §12.
- `jointBufferV` (skinning palette) declared as `float4 matrices[768]`
  (12,288 B, room for 192 joints) in the reflection data of both skinned-mesh
  shaders above; one observed live instance is a smaller, double-buffered
  8,064-byte pair (126 joints) — refines the earlier "~8 KB" estimate with
  exact numbers.

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
| `noclip <on/off>`, `pm_noclipspeed` | free camera, no collision | testing |
| `g_permaGodMode` | removes death as a variable | safety net for manual testing |
| `com_captureFrames`, `com_capturePath`, `com_captureTGA`, `com_captureSamples` | built-in frame-capture-to-disk (the devs' own `AutoScreenShotSmokeTest`/`MegaScreenShot` system, per exe strings) | promising lead for a future Phase 2 harness — may be cheaper than a custom D3D staging-texture readback; not yet investigated |
| `com_skipIntroVideo`, `com_skipPressButtonScreen`, `com_skipSignInManager` | skip startup screens | fast deterministic launch (future harness) |
| `r_allowTessellation` | confirms a tessellation pipeline exists | see §8 Domain-Shader gap |
| `r_lod_fovScale` | FOV-dependent LOD bias | revisit once a wide VR FOV is in play |

A full string-scan of the exe (2026-08-20 offline research pass) found
**~1,982** candidate cvar/command names in total (`g_`/`pm_`/`r_`/`com_`/
`con_`/`cg_`/`si_`/`in_` prefixes) — the table above is a curated high-value
subset, not the full list. No stereo/VR strings of any kind were found
(`stereo3d`/`oculus`/`openvr`/`vive`/`steamvr`/`nvidia3d`/`stereoscopic` all
zero matches) — confirms there is no hidden/legacy stereo mode to shortcut
through.

## 10. Autonomous harness recipe (this game)
- **Not yet built (Playbook Phase 2 — deferred, not currently a near-term
  priority; see the 2026-08-20 ledger ruling: the heavy discovery grind that
  motivated wanting this is done, only a couple of human check-ins remain to
  reach the stereo-core milestone).** Current constraint: the game pauses
  when unfocused and rejects external SendInput/SetForegroundWindow, so
  gameplay captures have needed a human. Planned fix: drive input/camera from
  *inside* the injected process (hook xinput/dinput polling or the camera update;
  or use `devmapjump` + camera cvars), plus back-buffer capture to disk.
  **New lead (2026-08-20, unexplored):** the engine has its own built-in
  frame-capture system (`com_captureFrames`/`com_capturePath`/`com_captureTGA`,
  backing an internal `AutoScreenShotSmokeTest`/`MegaScreenShot` tool per exe
  strings) and a `noclip` console command — either could turn out to be
  cheaper than a from-scratch D3D11 staging-texture readback or an xinput
  hook, whenever this phase is picked up.
- Discovery instruments so far (env-gated, off by default): `TEWVR_SEQDUMP`
  (ordered per-draw event stream with ctx tags + command-list events),
  `TEWVR_SEQDUMP_ARMFILE` (file-triggered arm), `TEWVR_SKIPCL` (live skip of
  ExecuteCommandList — the experiment that mapped the frame), `TEWVR_CBPEEK`
  (draw-time constant-buffer content read), a shader-hash→mvp-offset reflection
  table, plus `shaderdump` + an offline DXBC disassembler.

## 11. Dead ends & false leads (save future time)
- A 384-byte "view matrix" (orthonormal + varying) was actually a **per-object
  cloth model matrix**, not the camera. Content heuristics match per-object
  matrices too — trust shader reflection, not heuristics. (2026-08-20: a
  gameplay capture found 11 distinct 384-byte buffer addresses, consistent
  with a genuine per-object pool, corroborating this.)
- The 96-byte most-bound VS slot-0 buffer feeds **lighting**, not geometry.
- **Uniformly scaling** a view/projection is *visually invisible* (cancels in
  the perspective divide) — perturb **non-uniformly** to see an effect. Stereo's
  horizontal per-eye shift is non-uniform, so it does show.
- The world buffer "never being written" was a **persistent-map memcpy**, not a
  missing writer — don't assume a bound-but-unmapped buffer is static.
- **Buffer *size* alone does not identify a buffer's role.** A coincidentally
  same-sized (1920 B) but functionally unrelated per-frame global buffer
  exists alongside the true world MVP pool (§7) — told apart by binding slot
  and context, not size. The same trap likely applies to the other per-frame
  global sizes found alongside it (656/768/1168/1264/4992/5760/8064 B) — see
  §7 and the 2026-08-20 offline-research dev-archive note for the full list.
  Always cross-check *where* (slot, context type) a buffer is bound, not just
  how big it is.
- A once-per-frame *batched* staging-copy snapshot of the world-pool buffers
  (an early Task 6 design) looked safe but was **data-insufficient**: ~1900
  draws/frame share ~6 buffer identities, so a once-per-frame snapshot gets
  reused unchanged across ~300 different draws, corrupting all but one.
  Caught by code review from the diff's own documented constants before ever
  running — replaced with a direct-pointer-capture-at-creation-time design.
  Lesson: "safe to execute" and "supplies correct data" are separate claims:
  check both.

## 12. Open risks toward the North Star
- Keystone proof (own the camera on the deferred-recorded world) — **code
  landed and reviewed clean (2026-08-20)**, runtime yaw-rotation proof itself
  still pending a human-witnessed gameplay session.
- Double-render on a deferred-context engine: re-execute command lists per eye
  vs. record a second pass — mechanism chosen but unproven at runtime. (Task 6
  and Task 7 are now expected to merge: patch-and-draw-twice-per-original-call
  at record time, once per eye, rather than re-executing a command list.)
- Persistent-map source read: the AddRef/lifetime bug is fixed (traced
  correct on every code path by review); cross-thread safety of the new
  `CreateBuffer`-hook's foreign `Map` (issued from whatever thread
  `CreateBuffer` fires on, not necessarily the render thread) is de-risked by
  a clean live smoke test (16/16 succeeded) but not fully proven — see the
  2026-08-20 offline-research note for what static evidence could and
  couldn't establish here.
- **New (2026-08-20): tessellated/Domain-Shader geometry is invisible to the
  current patch mechanism.** At least two skinned-mesh vertex shaders never
  compute `SV_Position` themselves (a Domain Shader does, downstream) — see
  §8. Expect this geometry not to rotate during the keystone proof; it's an
  understood gap, not a regression, but will need its own future work
  (hooking Domain Shader constant buffers) before it's covered.
- Post/AA (SMAA, motion vectors) per-eye consistency deferred — now fully
  characterised with exact shader offsets (§8), ready to implement whenever
  this is picked up.
- Buffer-identity ambiguity for the `CreateBuffer` candidate pool (§7, §11):
  resolved analytically via old capture data, but the true ~1920 B world-pool
  buffers have still never been *observed* captured by the live hook in a
  real gameplay session (only inferred to be catchable) — first thing for
  the pending Step 3 session to confirm.
