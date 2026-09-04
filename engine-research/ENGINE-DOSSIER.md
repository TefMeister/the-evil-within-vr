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

## 3a. On-disk shader archive (NEW 2026-09-03, `/pd`, static — partially reversed)

The game **does** ship its shaders on disk, which nothing in this dossier had previously recorded.
This matters because §12's coverage-gap work is blocked only on obtaining vertex-shader bytecode,
and the assumption throughout was that a runtime dump is the only source.

- **Where.** `base/common.tangoresource` (50,008,892 bytes) contains **76 entries** named
  `generated/renderprogs/shader_retail/pc/<name>.shaderbin2`, each paired with a `_ws` variant.
  `[verified-numerically 2026-09-03]` The exe corroborates the vocabulary: it contains the literals
  `renderprogs` (×13), `shaderbin` (×2) and `tangoresource` (×1).
- **Container format — SOLVED 2026-09-03.** `[verified-numerically 2026-09-03]`
  ```
  +0        u32   magic 0x2394ABCD
  +4        u32   BE entry count (0x00002D2D = 11565 for common.tangoresource)
  +8        TOC:  count x { u32 LE len, name, u32 LE len, name, u32 hash }   -> ends 0x1902E8
  0x1902F0  data: entries back-to-back, each a HEADERLESS RAW DEFLATE stream
  0x2F77AB8 offset table: 9001 x { u32 BE file offset, u32 BE csize, u32 BE usize, u32 BE id }
  ```
  The offset table is what makes entries addressable. **Why the walk is believed rather than assumed:**
  a chain of `offset + csize == next offset` proves nothing on its own, because the walker stops when
  that breaks. The independent checks are that the first record's offset is exactly `TOC_END + 8`, and
  that the **count 9001 is written as a `u32` BE at `0x2E945C2` — precisely the byte where the last
  record says the data ends**, in a small metadata block that also records the size of the other
  trailing table. 47,203,026 compressed bytes expand to 133,454,381. **249 of 9001 entries (2.8%) fail to inflate** and are unexplained; they are not
  shaders (no shader was lost — see the hash match below), but the residual is real and unclosed.
  Entries carry no per-entry header: an entry that holds a shader begins
  `u32 hash, u32 BE size, DXBC…`.
  ⚠️ **The `id` field was NOT decoded to a TOC name.** Entries were identified by *content* (a DXBC
  signature) and by *hash*, never by filename, so which entry is which `.shaderbin2` is unknown — and
  the 76 `.shaderbin2` TOC names and the 2,785 DXBC-bearing entries have not been reconciled. Nothing
  below depends on that mapping, but do not assume it exists.
- **The shaders are DXBC, with reflection intact.** `[verified-numerically 2026-09-03]`
  2,785 entries in `common.tangoresource` contain DXBC; extracted, they are **2,785 DXBC containers
  with RDEF reflection**, 603 distinct constant-buffer layouts. `constantBufferV` — this dossier's own
  name for the per-draw MVP buffer (§6/§7) — appears in **1,208** of them, bound at **`cb0`**, and its
  rows are **named**: `mvpmatrixx`/`y`/`z`/`w` with explicit byte offsets, alongside
  `vertexxyzscale`, `vertexxyzbias`, `vertexstscalebias`, `fogstart/end/scale`.
- **⇒ The §12 shader-bytecode blocker is retired.** See §12; the short version is that every
  shader's exact MVP row offsets are now readable off disk, no launch involved, and the runtime
  table this project already had cross-checks against them with **zero disagreements**.
- **⚠️ A methodology note worth more than the finding.** The first scan looked for zlib-framed
  streams (`0x78 0x9C` etc.), found 3,489 candidate positions, decompressed exactly one, and would
  have been written up as "the archive is not zlib". That test **could not have produced a positive
  result**, because the data is headerless raw deflate. The retry with `wbits=-15` found it
  immediately. A negative result is only evidence if the test could have found the thing.

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
  - **Update 2026-09-03/04:** the scattered-row half is closed (§12) — 34 of 34 verified live.
    `pool_miss` is **a different render path, not a ceiling** (2026-09-03f): the pool only
    registers the large shared `DEFAULT` world buffer, and a miss on a small per-shader
    `DYNAMIC` cb0 is expected. Whether the missed draws carry world geometry was unmeasurable
    because the bucketed miss table printed only while `patched == 0`; **since 2026-09-04 it
    prints every ~30 s and at shutdown, and each bucket also records draw sizes and vertex-shader
    hashes** — see §12.
  - **✅ NOW MEASURED LIVE 2026-09-04b (`/lm`, Chapter 1): the missed `pool_miss` buckets DO carry
    real world geometry — the residual is NOT harmless.** Per ~5 s: **~590,000 draws patched** vs only
    **~13,600 `shader_no_mvp`** skips (the sole skip reason; `not_installed`/`no_vs`/`shader_unknown`/
    `rows_incomplete`/`no_slot0` all 0). The one `DEFAULT`/`Usage=0` bucket is `ByteWidth=1920` — inside
    the known 1856–1984 B world-pool range, 2 misses, expected. But several `DYNAMIC`/`Usage=2` cb0
    buckets carry substantial geometry: `combo[5]` (BW 160) `geom(count>=300)=750–1016, max_count=39102`;
    `combo[7]` (BW 224) `geom=1001`; `combo[3]` (BW 272) `non-indexed=109, max_count=120000`; `combo[4]`
    (BW 304) `geom=54 non-indexed=54, max_count=75000`. `[verified-live 2026-09-04, n=1 launch]` These are
    MVP-bearing draws whose cb0 is a per-shader `DYNAMIC` buffer the pool patch never intercepts — not
    no-MVP draws (skinning is pre-MVP, §6). **Visible confirmation:** with `TEST_YAW=90`, Chapter 1's
    rainy opening rendered as a radically transformed scene (patched ~75% rotated) with unrotated
    fragments and an upright, detached character head — i.e. the pool_miss geometry rendering unrotated.
    **⇒ a stereo build must extend coverage to the per-shader DYNAMIC cb0 path, not only the shared
    DEFAULT pool.** Notes: `modding-notes/2026-09-04b-config-arm-confirmed-yaw-visible-poolmiss-carries-geometry.md`.
  - **⛔️ 2026-09-04d (`/lm`): the first DYNAMIC cb0 coverage build (2026-09-04c, `winmm.dll` 352,768 B) PATCHES NOTHING — defeated by multi-threaded rendering.** In-scene (Chapter 1, `TEST_YAW=90`): the path registers buffers (pool 54/64) but reports **`shadow writes=0, draws patched via it=0`** with **`pending-map table overflows` climbing past 2.7 million** `[verified-live 2026-09-04, n=1]`. Named cause in the log: **`thread-ring pool exhausted (8 distinct threads seen); further threads share the last bucket`** — TEW's renderer maps/unmaps these DYNAMIC buffers from MORE than 8 threads, so the Map→Unmap→shadow pairing (a fixed 8-slot per-thread ring) overflows and never completes, and no DYNAMIC write is ever shadowed. The DEFAULT-pool patch is unaffected (232k–444k draws patched per 5 s; world cb0 pool holds 6). So the ~25% gap is still open and the scene is visually unchanged. **Fix (`[PD]`): size the pending-map/thread-ring pool to TEW's real thread count (>8), or re-key the Map→Unmap→shadow pairing on the mapped pointer / buffer handle instead of a per-thread ring, so it survives the concurrency.** Notes: `modding-notes/2026-09-04d-dynamic-cb0-path-defeated-by-thread-ring-overflow.md`; evidence `dev-archive/recon/2026-09-04d-dynamic-cb0-path-defeated-by-thread-ring-overflow/`.

### ⭐ 7b. The coverage gap has a second interception path now: per-shader DYNAMIC cb0 (2026-09-04c, `/pd`, no launch)

**The gap was never a shader problem.** `[measured 2026-09-04, n=167 shaders]` Of the accumulated
runtime shader table, 145 are MVP-bearing, and **61 of those — 42.1% — declare `cb0` at one of the
four sizes the 2026-09-04b bucketed dump named as carrying real geometry** (`cb0=224` 33 shaders,
`160` 18, `272` 8, `304` 2; 8 of the 61 use the scattered z/w layout, handled since 2026-09-03).
**Every one already has a complete reflected four-row set recorded**, so they were always patchable
— only the buffer was out of reach.
⚠️ SHADER counts, not DRAW counts: do not mix these with the ~74–77% draw figure, which is a
different population. Analysis and the rescued table:
`dev-archive/recon/2026-09-04c-dynamic-cb0-coverage/`.

**Why they were unreachable.** The pool tracks only the large shared **DEFAULT** world buffer,
because `UpdateSubresource` is the only CPU write path a DEFAULT buffer has and that is what feeds
its shadow — the registration filter rejects DYNAMIC buffers explicitly, and correctly, on those
grounds. A per-shader DYNAMIC `cb0` is written through `Map(WRITE_DISCARD)`/`Unmap`, which nothing
was watching.

**The fix is one more shadow SOURCE, not a new patch mechanism.** `Map`/`Unmap` (vtable slots 14 and
15, read from the SDK header rather than assumed) are hooked; at `Unmap` the mapped contents are
copied into a shadow while the pointer is still valid; and the existing draw-time path — bounds
check, seqlocked read, `K` multiply, scratch write, rebind — then runs unchanged. The new slots are
a **partition of the same arrays** (`[0,32)` DEFAULT, `[32,96)` DYNAMIC), so every seqlock and
validity guarantee already reviewed applies to them without duplication and the hot DEFAULT lookup
does not slow down. Registration is offered **from the draw path only**, never from a descriptor,
per this module's existing rule; a newly seen buffer is unpatched for its first draw and patched
from the next write on. `[compile-verified 2026-09-04]`, deployed (`winmm.dll` 352,768 B; previous
kept as `winmm.dll.bak-2026-09-04c-pre-dynpool`). **Not run.**

- **Two compile-time assertions guard the partition**, both proved to fire by deliberately breaking
  the values `[verified-numerically 2026-09-04, n=2 negative controls]`: a dynamic buffer wider than
  the shadow window, and the pool sizes drifting out of step with the array size.
- **A 64-bit pointer bug was caught before it shipped:** the pending-map table claimed its slot with
  the 32-bit `InterlockedCompareExchange` on a resource pointer, which truncates in this process and
  would have matched the wrong buffer at `Unmap` — patching one mesh from another's constants.
- ⚠️ **Pool size (64) is a guess** — the live evidence names four size/usage combos but not how many
  distinct buffers back them. It logs when it fills.
- ⚠️ **Known risk, not designed away:** two deferred contexts may legally `Map` the same buffer at
  once (`WRITE_DISCARD` renames per context) and a shadow keyed by buffer pointer cannot represent
  both. The seqlock degrades to fail-safe (draw falls through unpatched) and
  `g_diag_shadow_concurrent` counts it. **If that counter moves, this path needs per-context
  shadows** — a redesign, not a tweak.

Write-up, including the log lines to read and what each means: `modding-notes/2026-09-04c-the-dynamic-cb0-path-is-built-and-it-addresses-42-percent-of-the-shader-table.md`.

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
- **Every `TEWVR_*` knob is also read from a file since 2026-09-04** (branch
  `stereo-6dof-core` `3475717`, `src/config.c`): `tewvr.ini` beside `EvilWithin.exe`, then
  `%LOCALAPPDATA%\TEWVR\tewvr.ini`; the process environment still wins. `KEY = value` lines,
  prefix optional, read once at attach. Added because a **Steam launch inherits Steam's
  environment**, so the yaw test silently ran as identity three launches in a row
  (2026-09-03e/f/g) — a launcher script is not a mechanism, a file the proxy reads itself is.
  `tewvr.log` names the source of every knob it found. Parser
  `[verified-numerically 2026-09-04, n=14 checks]` via `tools/config_test.c`; the in-process read
  is `[compile-verified 2026-09-04]` only. Key list: `proxy-winmm/tewvr.ini.example`.

## 11. Dead ends & false leads (save future time)
- **"`pool_miss` is harmless small dynamic cb0s" was half right and wholly misleading.** The misses
  really are per-shader DYNAMIC buffers rather than a failing DEFAULT path (2026-09-03f), but that
  did not make them unimportant: bucketing them showed they carry world meshes up to 120,000
  vertices `[verified-live 2026-09-04]`, and 42% of the MVP-bearing shader table declares `cb0` at
  those sizes. **A diagnostic that explains WHY a counter is high is not the same as showing the
  counter does not matter.**
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
  - **BOUNDED 2026-09-01 (the shader-layout half).**
    `[measured 2026-09-01 from `dev-archive/recon/2026-09-01-shader-reflection/mvp_offsets.log`,
    n=168 shaders, one gameplay session]` Of 168 vertex shaders: **112 (66.7%) contiguous** and
    patched today; **22 carry no MVP at all** (the Domain-Shader/tessellated group of §8 — not a
    gap to close here); **34 carry an MVP with scattered rows**, and those collapse into only
    **10 distinct `(cb0, mvpx)` shapes**. One shape, **`cb0=128 mvpx=64`, is 15 of the 34 (44%)**.
    Handling shapes in frequency order takes shader coverage 66.7% -> 75.6% (1 shape) -> 80.4%
    (3) -> 86.9% (all 10).
  - Two constraints on what "non-contiguous" can mean: **`mvpx` is always a whole number of
    float4 rows** (0/64/96/144), so this is not an alignment artefact; and **`cb0 - mvpx >= 64`
    in all 10 shapes**, so there is always room for four rows after the base — the rows are
    **interleaved with other constants**, not truncated.
  - **⚠️ These are SHADER counts; the 74–77% above is DRAW coverage.** The busiest shape by
    shader count need not be the busiest by draw count. Do not restate the 44% as a draw figure.
    Says nothing about the `pool_miss` residual, which is a buffer-identity problem, not a
    shader-layout one.
  - **The `pool_miss` residual, corrected and instrumented (2026-09-03f, 2026-09-04).** It is
    **not** a ceiling: the proxy's own first-miss diagnostic says a miss on the small per-shader
    `DYNAMIC` cb0s is expected — the pool registers only the large shared `DEFAULT` world buffer.
    Declared `cb0` across all 167 known shaders runs 0–352 B, none in the 1856–1984 pool window
    `[measured 2026-09-03, n=167]` (D3D11 allows a bound buffer larger than the declaration, so
    this is consistent). Its share swings 13% → 19% with scene and camera `[verified-live
    2026-09-03, n=3]` — a scene property, not a fixed limit. **The open question — do the missed
    draws carry world geometry? — was unanswerable only because of an `if`:** `mvp_patch.c` has
    bucketed sampled misses by `(ByteWidth, Usage, BindFlags, CPUAccess)` since fix round 4, but
    printed the table only under `pool_miss > 0 && patched == 0`, the same gate that hid the
    shadow-concurrency counters until 2026-09-01. **Since 2026-09-04** (branch `3475717`) the
    table prints every 6th window and at shutdown, and each bucket adds sampled draw sizes
    (`geom` ≥ 300 indices, `tiny` ≤ 6, `max_count`, indexed or not) plus up to 8 vertex-shader
    hashes — nameable offline through the archive tables. Reading guide:
    `modding-notes/2026-09-04-the-proxy-reads-a-config-file-and-the-miss-table-prints-while-patching-works.md`
    §2. `[compile-verified 2026-09-04]`; the columns are empty until a launch.
  - **~~Blocked on one launch~~ — RETIRED 2026-09-03, see the next bullet.** The reflection table records only a base offset and a contiguity
    flag, so rows 1-3 are not recoverable from it; that needs vertex-shader bytecode, and **none
    was ever saved**. One launch with the shader-dump path, then
    `proxy-winmm/tools/dxbc_disasm.c` offline, turns the table from `(base, contiguous)` into four
    explicit row offsets — which is the change `mvp_patch` needs to handle any scattered layout.
    - **✅ The offline half is now proven, 2026-09-03 (`/pd`, static).** `dxbc_disasm.c` had never
      been built and is referenced by no build script; it now **builds clean, zero warnings**
      `[compile-verified 2026-09-03]` and was run end-to-end against a real SM5 DXBC vertex shader,
      printing the input/output signatures and the instruction stream, including the literal
      `cb0[N]` row indices this work needs `[verified-numerically 2026-09-03, n=1]`. So the launch
      is the *only* missing input, not the launch plus an unproven tool.
      ⚠️ It lives **only on branch `stereo-6dof-core`** — it is not on `main`, along with 12 other
      source files. See the branch-merge finding in `modding-notes/2026-09-03-...`.
  - **✅ NO LONGER BLOCKED ON A LAUNCH AT ALL (2026-09-03, `/pd`, static).** The bytecode is on
    disk. `base/common.tangoresource` was fully unpacked (§3a) and yields **2,785 DXBC shaders with
    RDEF intact**, of which **1,208 declare `constantBufferV`** with its rows *named*
    (`mvpmatrixx/y/z/w`) at explicit byte offsets. `[verified-numerically 2026-09-03]`
    - **The runtime table and the disk agree exactly.** The proxy keys shaders by FNV-1a64 of the
      DXBC blob (`shaderdump.c`; note the non-standard offset basis `1469598103934665603`).
      Recomputing that over the extracted shaders matches **167 of the 168** rows in
      `mvp_offsets.log`, **including all 34 scattered ones**, and the reflected
      `(constantBufferV size, mvpmatrixx offset)` agrees with the runtime-recorded `(cb0, mvpx)` on
      **167 of 167 matched rows, zero disagreements**. `[verified-numerically 2026-09-03, n=167]`
      Two entirely different methods — live reflection through the proxy, and off-disk archive
      extraction — produce the same table.
      ⚠️ One of the 168 is absent from `common.tangoresource`; `common` is one of ~20 archives and
      level-specific ones were not searched, so this is expected rather than explained. Not chased.
    - **"Scattered" is almost always one specific thing: z and w are SWAPPED.** Of the 34,
      **33 have their rows at `+0, +16, +48, +32`** relative to `mvpmatrixx`; exactly one
      (`E73523999ED27D3E`, cb0=80) differs, at `0, 32, 48, 64`.
      `[verified-numerically 2026-09-03, n=34]` Full per-hash table:
      `dev-archive/recon/2026-09-03-tangoresource-and-branch-merge/2026-09-03-scattered-mvp-row-offsets.txt`.
    - **Wider census, for free:** across all 1,208 on-disk `constantBufferV` shaders, 957 (79.2%)
      are contiguous, 195 (16.1%) scattered, 56 carry no `mvpmatrix` rows.
      ⚠️ **Do not compare that 79.2% with the runtime table's 66.7%** — different populations (every
      shader shipped in one archive, versus the 168 a single gameplay session actually created).
    - **Still NOT established:** that writing the four rows at these offsets renders correctly.
      Reflection gives the layout; only a run shows the patch behaves. That is the existing
      keystone/runtime row, not a new blocker.
  - **✅ AND THE PATCH NOW USES THEM (2026-09-03, `/pd`, static, branch `stereo-6dof-core` `03c48ce`).**
    The offsets were never missing from the runtime either: `mvptable_on_shader_created()` already
    read `mvpmatrixy/z/w`'s `StartOffset`, compared them to `x+16/+32/+48`, kept the boolean and
    discarded the offsets — which is why `mvp_row_offsets_for_shader()` had to refuse anything
    non-contiguous. It now keeps all four and hands them out; contiguity is diagnostic only, and
    `mvp_offsets.log` lines gain a `rows=x,y,z,w` field (appended, so old parsers still work).
    - **⚠️ That change exposed a latent out-of-bounds write in `mvp_patch.c`.** Its bounds check
      tested only `offs[3] + 16` against the bound buffer's size — valid only while offsets ascend.
      The dominant layout here is z/w transposed, `{b, b+16, b+48, b+32}`, where `offs[2]` is the
      highest, so a row could have been written past the end of the buffer **while the check
      reported success**. It now tests every row. Found by checking the interaction, not by testing.
    - **Verified offline against the game's own shaders** by a harness that links the shipped
      `mvptable.c` and runs `mvptable_reflect_rows()` over all 2,785 archive shaders, compared
      against an independent from-scratch RDEF parser: **0 disagreements**, 1,192 shaders with a
      complete row set (997 contiguous, **195 scattered that were previously skipped**).
      `[verified-numerically 2026-09-03, n=2785]` `[compile-verified 2026-09-03]` Against the 168
      shaders one real session produced this is **112 → 146 of 168, i.e. 66.7% → 86.9%** — ⚠️ a
      SHADER count, not a draw count, and it does nothing for the `pool_miss` residual.
    - Deployed to `TheEvilWithin\winmm.dll`; previous build kept as
      `winmm.dll.bak-2026-09-03-pre-scattered-rows`. ⚠️ The commit is on the branch, which is still
      unmerged. Write-up: `modding-notes/2026-09-03c-the-patch-now-handles-scattered-rows.md`.
- **Shadow writer-concurrency risk, measured safe not structurally guaranteed.**
  The world cb0 shadow's single-writer assumption is false (writes are
  routinely cross-thread) but writes have never been observed truly
  concurrent (a seqlock would detect and fail-safe-skip if they were). This
  holds on the dev machine across every session tested; it is not proven to
  hold on different hardware/thread-scheduling.
  - **VISIBILITY FIXED 2026-09-01** (branch `stereo-6dof-core`, built clean, **not run**). The
    problem was worse than "only prints during a diagnostic window":
    `g_diag_shadow_concurrent` / `_torn` / `_multiwriter` were reachable **only** through
    `mvp_diag_report_misses()`, which runs under `pool_miss > 0 && patched == 0` — i.e. **only
    while patching is failing outright**, so in the normal working case they printed **never** and
    a violation on other hardware would have been silent. They now print on their own whenever
    non-zero, **cumulatively** (the question is "has this ever happened on this machine", not "how
    often in the last window"), with an explicit warning when the precondition has actually been
    violated. Visibility only, no behaviour change. This is now **monitored**, not "measured once"
    — but still only *measured safe* until a session actually reports zeroes.
- **New (2026-08-20): tessellated/Domain-Shader geometry is invisible to the
  current patch mechanism.** At least two skinned-mesh vertex shaders never
  compute `SV_Position` themselves (a Domain Shader does, downstream) — see
  §8. Confirmed as an understood gap, not a regression — this geometry
  renders unpatched, consistent with the coverage gap above.
  - **Not a cold start when it is picked up (external-research, 2026-09-02).** NVIDIA's own
    patent US10068366B2 documents a domain shader reading **per-view constants** to turn
    barycentric-interpolated tessellator output into per-eye clip-space positions — structurally
    the same shape as §6's VS-stage `K_eye` left-multiply, one stage later. So the recipe is
    "reflect the DS's own constant buffer(s) for a per-view/projection row, the same discipline as
    the VS `constantBufferV` search, then apply the per-eye write at the DS stage". `[reported]` —
    a documented template, not something this project has tried.
    Topic: `external-research/topics/2026-09-02-domain-shader-stage-per-view-transform-is-documented-prior-art.md`.
- Post/AA (SMAA, motion vectors) per-eye consistency deferred — now fully
  characterised with exact shader offsets (§8), ready to implement whenever
  this is picked up.
- `K` in the keystone proof is a raw clip-space post-multiply (`K · mvp`),
  proven sufficient for an ownership proof but explicitly not the real
  per-eye transform — Task 7/8 must build the actual `K_eye` per §6's
  documented maths.
