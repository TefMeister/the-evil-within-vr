# Engine Dossier — The Evil Within (2014) (id Tech 5 / Tango "STEM")

> Distilled current truth about this game's engine, as learned through the
> `PLAYBOOK.md` phases. Blow-by-blow history lives in the `-dev-archive` and
> `-modding-notes` repos; this is the consolidated reference.

**Status:** *** Phase 4 COMPLETE (2026-08-21) — the keystone proof is passed.
*** The mod owns and can rewrite the world's per-draw camera transform on
real, deferred-recorded, in-game geometry, proven with a falsifiable visual
test (see §7, §12, and dev-archive `notes/10-keystone-proof-task6-resolved.md`
for the full account, including three false-start mechanisms that live
gameplay testing disproved before the real one was found). **VR-readiness
verdict: feasible, keystone risk retired.** Phase 5 (stereo on the flat
monitor) is next; real per-eye maths, a ~74–77% draw-coverage gap, and a
writer-concurrency risk (measured safe so far, not structurally guaranteed)
carry forward — see §12.

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

## 7. Constant-buffer fill mechanism (FINAL — proven, Task 6 closed 2026-08-21)
- The world's per-object MVP lives in a **small pool of ~6 constant buffers**
  (one ring per worker thread), bound to VS slot 0, sized 1920 B.
- **The buffers are created `D3D11_USAGE_DEFAULT` with `CPUAccessFlags=0` —
  they cannot be `Map`ped under any circumstance, by the D3D11 API's own
  rules.** This is *why* no Map/Unmap hook ever saw a write (not a timing or
  instrumentation gap, as earlier rounds assumed) and *why* every earlier
  "successfully captured 1920-byte buffer via a persistent foreign Map" was
  actually a same-size **decoy** pool, never bound at slot 0 for a real world
  draw (see §11 — that decoy pool is real and still exists; it is just not
  this one).
- The engine writes the real pool via **`UpdateSubresource`** — the only CPU
  write path a `DEFAULT`-usage buffer supports.
- **Chosen patch point (implemented, Task 6, final form):** hook
  `ID3D11DeviceContext::UpdateSubresource` and shadow every write (including
  correctly handling a partial-region write via `pDstBox`) into a CPU-side
  cache keyed by buffer identity. **Register** a buffer identity as a real
  target only at the *draw* path — a buffer bound at VS slot 0 for a draw
  whose current shader has a known, contiguous `mvpmatrix` offset — never at
  `CreateBuffer` time by descriptor alone (size/usage/bind-flags matching is
  not a sufficient fingerprint; see §11). At each deferred draw: read the
  shadowed MVP rows, left-multiply by `K` (a per-eye `K_eye` for real stereo;
  a test rotation for the keystone proof), write the patched cb0 into **our
  own** scratch buffer (a per-thread TLS ring), rebind slot 0 before the
  original draw → our buffer is recorded into the command list.
- **The shadow's single-writer-per-buffer assumption is measured false, not
  true.** Live gameplay showed 448,201 of 560,109 shadow writes were
  cross-thread (worker threads hand these buffers between each other across
  frames). A per-slot seqlock now *detects* rather than assumes this: writes
  are frequently cross-thread but were never observed genuinely *concurrent*
  (zero torn reads across every session tested) — the fail-safe skip path
  exists in code but has not yet fired live. If a target machine's threading
  ever produces real concurrent writers, this would need promoting to a real
  per-slot lock; the counter that would reveal that currently only prints
  during a diagnostic window, not continuously (a tracked, not yet closed,
  observability gap).
- **1920 bytes is not a unique fingerprint for the world pool** — a *different*,
  unrelated pair of 1920-byte buffers also exists (a per-frame global,
  refreshed once per `Present` via real `Map`/`Unmap` on the immediate
  context, bound at VS slot 2/3, not slot 0, and IS `Map`pable — a DYNAMIC
  buffer, unlike the real pool). The two are reliably told apart by binding
  slot + context + usage class, never by size alone. See §11.
- **Coverage is ~74–77%, not complete.** Roughly a quarter of MVP-bearing
  draws still render unpatched: non-contiguous-row shaders (an older, known
  limitation — the mechanism only patches contiguous layouts) plus a
  `pool_miss` fraction not yet root-caused. A stereo build would render this
  fraction at the wrong per-eye orientation until addressed.

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
- **Substantially working, as of 2026-08-21** (built opportunistically while
  debugging Task 6, not as planned Phase 2 work — see dev-archive
  `notes/10-keystone-proof-task6-resolved.md` and `task-6-report.md`'s fix
  round 4 for the full account).
- **Deterministic launch to real gameplay:** launch `EvilWithin.exe` *directly*
  (not via the `steam://` protocol handler) from its own directory, with
  `steam_appid.txt`=268050 present and the Steam client already running, args
  `+com_allowconsole 1 +devmapjump st06_asylummain` (a real, confirmed-valid
  stage name — the game's opening chapter, found via static string extraction
  from the exe; other candidates seen: `st12_gd`, `st13_prop`,
  `st40_gamedesign`). Reaches real gameplay in seconds, unattended.
- **The game shows a "Photosensitivity Warning" splash on the way in that
  blocks progress until dismissed.** External `SendInput`/`SendKeys`
  *keyboard* input does **not** dismiss it; a synthetic **mouse click** does.
  (`com_skipIntroVideo`/`com_skipPressButtonScreen`/`com_skipSignInManager`
  launch-arg cvars were tried against this specific screen and did not work.)
  Whether this screen appears may be session-dependent — possibly tied to
  whether the previous session exited gracefully vs. was force-killed (a
  "seen this warning" flag that only persists on clean exit is the working
  theory; unconfirmed).
- **Frame capture to disk: built and proven end-to-end.** A `TEWVR_FRAMECAPTURE=1`-
  gated, file-triggered (drop `%LOCALAPPDATA%\TEWVR\capture.txt`) back-buffer
  capture to BMP, using a staging-texture readback off the already-captured
  device/context. A controller session can trigger a capture and then view
  the resulting image directly (no human needs to watch the screen) — this is
  exactly how the Task 6 keystone proof's images were produced and
  independently re-verified by two separate reviewers. **Not yet formalised
  as a reviewed task** — currently an uncommitted-then-locally-committed spike
  in the mod repo (`proxy-winmm/src/framecapture.c`/`.h`), pending its own
  SDD task brief + review pass before being treated as production code.
- **Still open:** in-process input injection for anything beyond a single
  dismissal click (real camera movement, menu navigation) — Playbook 2.2,
  not yet attempted. The engine's own built-in frame-capture system
  (`com_captureFrames`/`com_capturePath`/`com_captureTGA`) remains an
  unexplored, potentially-cheaper alternative to the custom D3D11 staging
  readback above.
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
- **A buffer's `Usage`/`CPUAccessFlags` must be checked before assuming any
  CPU-read mechanism (`Map`, or a "capture a pointer once" scheme) is even
  possible.** Two full fix rounds were spent capturing a persistently-mappable
  1920-byte decoy pool via a foreign `Map` at `CreateBuffer` time — it worked
  perfectly as a *mechanism*, live-tested with zero failures, and was still
  entirely wrong, because the real world pool is `D3D11_USAGE_DEFAULT` and
  categorically cannot be `Map`ped. The only way this was found was by
  checking the real buffers' actual creation-time descriptor against what the
  chosen read mechanism requires — not by the mechanism "working" in testing.
  A mechanism that runs cleanly can still be reading the wrong buffers.
- **Filtering `CreateBuffer` calls by descriptor alone (size/usage/bind-flags),
  however tight the band, cannot discriminate a real target from a decoy that
  happens to share the same profile.** The only property that actually
  identifies "this is the buffer we want" is *how it's used* — bound at a
  specific slot, for a draw whose shader has the property we care about —
  which is only observable at the point of use. Two different bugs in this
  project (the original wide-filter decoy-flooding in fix round 2, and the
  buffer-identity confusion in fix round 4) both trace back to this same
  root cause. Prefer registering/identifying resources by USE, not by
  creation-time descriptor, whenever the two might diverge.

## 12. Open risks toward the North Star
- ~~Keystone proof~~ **RESOLVED (2026-08-21).** Own-the-camera on the
  deferred-recorded world is proven, twice-independently-reviewed. See §7 and
  dev-archive `notes/10-keystone-proof-task6-resolved.md`.
- Double-render on a deferred-context engine: re-execute command lists per eye
  vs. record a second pass — mechanism chosen but unproven at runtime. (Task 6
  and Task 7 are confirmed to merge, now that Task 6's real patch point is
  known precisely: patch-and-draw-twice-per-original-call at record time, once
  per eye, rather than re-executing a command list.)
- **Coverage gap (~74–77%).** A real, measured fraction of MVP-bearing world
  draws render unpatched (non-contiguous shader row layouts, plus an
  unexplained `pool_miss` residual). Must be closed, or at least understood
  and bounded, before Task 7/8 — an unpatched draw in a stereo build renders
  at the wrong per-eye orientation, not just mono-incorrectly.
- **Shadow writer-concurrency risk, measured safe not structurally guaranteed.**
  The world cb0 shadow's single-writer assumption is false (writes are
  routinely cross-thread) but writes have never been observed truly
  concurrent (a seqlock would detect and fail-safe-skip if they were). This
  holds on the dev machine across every session tested; it is not proven to
  hold on different hardware/thread-scheduling. The counter that would reveal
  a violation currently only prints during a diagnostic window, not
  continuously — needs a one-line visibility fix before this can be trusted
  as "monitored" rather than "measured once."
- **New (2026-08-20): tessellated/Domain-Shader geometry is invisible to the
  current patch mechanism.** At least two skinned-mesh vertex shaders never
  compute `SV_Position` themselves (a Domain Shader does, downstream) — see
  §8. Confirmed as an understood gap, not a regression — this geometry
  renders unpatched, consistent with the coverage gap above.
- Post/AA (SMAA, motion vectors) per-eye consistency deferred — now fully
  characterised with exact shader offsets (§8), ready to implement whenever
  this is picked up.
- `K` in the keystone proof is a raw clip-space post-multiply (`K · mvp`),
  proven sufficient for an ownership proof but explicitly not the real
  per-eye transform — Task 7/8 must build the actual `K_eye` per §6's
  documented maths.
