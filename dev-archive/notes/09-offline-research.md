# Offline research pass — re-mining existing captures + static binary analysis

Session type: pure offline research, no game execution. Motivation: Task 6's
code landed review-clean on `stereo-6dof-core`, but the runtime yaw-rotation
proof (Step 3) needs a human-witnessed gameplay session that wasn't available
this session. Rather than wait idle, this pass re-mines data already captured
during Tasks 5-6 and does static analysis of the game binary and its already-
dumped shader blobs, to de-risk Step 3 and extend the engine model generally.

## 1. The buffer-identity ambiguity is resolved — with good news

Task 6's fix round flagged an open risk: the `CreateBuffer` hook's candidate
filter (`D3D11_BIND_CONSTANT_BUFFER`, `D3D11_USAGE_DYNAMIC`,
`D3D11_CPU_ACCESS_WRITE`, `ByteWidth` in `[512, 8192]`) had, in a menu-only
smoke test, caught 16 buffers of 512-608 bytes and none near the ~1920 bytes
expected for the world MVP pool. Re-mining Task 5's own gameplay captures
(`captures/task5-gameplay/seqdump-ctx3.log`, a full ctx-tagged gameplay
session, 40,003 events) answers this directly, without needing a new run.

**There are two unrelated families of ~1920-byte constant buffers in this
engine**, and conflating them is an easy trap:

- **The true world MVP pool**: exactly 5 distinct 1920-byte buffer identities
  observed bound at **VS slot 0** across the deferred worker contexts in this
  capture (`0x1557B872C38`, `0x1557B874138`, `0x1557B874BB8`, `0x1557B91ADB8`,
  `0x1557B91B038` — one short of the ~6 the discovery ledger describes, most
  likely because only 5 of the 6 worker threads happened to draw in this
  particular capture window). **None of these five addresses appear on a
  single `MAP` line anywhere in the 40,003-event capture** — independent,
  cross-session confirmation of the "persistently mapped, never explicitly
  touched" finding that originally came from a *different* capture
  (`task6-gameplay-seqdump.log`). Good: this is now corroborated from two
  separate sessions, not just one.
- **A coincidentally-also-1920-byte pair of buffers** (`0x155BE0BD238` and
  `0x155BE0BC7B8`) that genuinely *is* Mapped — once per `Present`, ping-
  ponging between the two addresses, `D3D11_MAP_WRITE_DISCARD` (`maptype=4`),
  on the **immediate** context. Every one of its ~20 `UNMAP` events in this
  capture starts with the same four floats (`[1.0000, 0.0000, 0.0000,
  0.0000]`) — a fixed leading pattern, not per-object data. It is bound not
  at slot 0 but at **slot 2/3**, and only on the deferred *worker* contexts
  as a read-only input (`VSSETCB start=2/3 ... :1920`), never at slot 0.
  Almost certainly a per-frame global (lighting/environment) buffer, not
  camera or geometry data.

**Why this matters for Step 3**: the two are cleanly separable by *how* and
*where* they're used, not by size — the true pool is (a) bound at slot 0, (b)
on a deferred context, (c) at draw time, and (d) never Mapped; the impostor
fails all four tests. `mvp_patch`'s per-draw lookup (`mvp_direct_pool_find`)
is keyed by whatever is *actually bound at slot 0* at `Hook_DrawIndexed`
time, so even if the `CreateBuffer` candidate pool also captures this (and
other) same-size impostors, they should simply sit unused — the risk is pool
*capacity* being spent on impostors before real gameplay creates the true
pool buffers, not a wrong patch being applied. Worth watching for in Step 3's
log (see the pending controller diagnostics already documented for this
task), but the mechanism's correctness does not depend on avoiding impostors,
only its capacity headroom does.

**A related family of per-frame global buffers, found for free while
chasing this**: the same immediate-context, once-per-`Present`, `[1,0,0,0,...]`-
prefixed `MAP`/`UNMAP` pattern recurs at several other sizes, most cleanly
double-buffered like the 1920B pair: **4992 B** (2 addresses), **5760 B** (2
addresses), **8064 B** (2 addresses). Two more sizes appear with a single,
non-ping-ponged address each: **656 B** and **1264 B**. Two sizes instead show
*many* distinct addresses (11 for 384 B, 16 for 768 B) — consistent with a
genuine per-object pool rather than one global buffer; 384 B matches the
already-documented "per-object cloth model matrix" dead end (section 11 of
`ENGINE-DOSSIER.md`), so 768 B is plausibly a related per-object structure.
None of this changes any conclusion, but it's a useful map of "what else the
`CreateBuffer` candidate filter's `[512,8192]` range will also catch" —
folded into the dossier's open risks.

## 2. The 8064-byte buffer is very likely the joint palette, now with real numbers

Static disassembly of shader `62F67B34913B0238` and `AF80AA9287F65EA7`
(both skinned-mesh shaders, see §3) reflects a `jointBufferV` cbuffer
declared as `float4 matrices[768]` = 12,288 bytes — i.e. room for up to 192
4x4 joint matrices. The observed 8064-byte double-buffered pair from §1
(`0x155D005C3B8` / `0x155D005D0B8`) is `8064 / 64 = 126` matrices — smaller
than the shader's declared maximum, consistent with one specific skeleton
(plausibly Sebastian's full body+face rig) being bound with only as many
joints as it actually has, while the shader is compiled generically to
support up to 192. This upgrades the existing dossier note ("`jointBufferV`,
~8 KB palettes") to precise, evidenced numbers.

## 3. Shader disassembly: the SMAA/motion-vector shader is now fully characterised

`ENGINE-DOSSIER.md` §8 flagged "SMAA + motion vectors consume
`inversemvpmatrix`/`prevmvpmatrix` — need consistent per-eye treatment later"
without ever having actually read that shader. It's on disk
(`shaderdump.c`'s `TEWVR_SHADERDUMP` output banks every distinct vertex
shader blob to `%LOCALAPPDATA%\TEWVR\shaders\*.dxbc`; 168 blobs are already
there from past sessions). Built the project's own offline `dxbc_disasm`
tool (`proxy-winmm/tools/dxbc_disasm.c`, wraps `d3dcompiler_47.dll`'s
`D3DDisassemble` — no game process involved) and read every one of the 22
shaders `mvptable` had marked `mvpx=-1` (no MVP rows found — the natural
post/AA/depth-only candidate set).

Shader `736130AA89FA0E59` is exactly that shader:

```
cbuffer constantBufferV {
  float4 inversemvpmatrixx;  // offset 0
  float4 inversemvpmatrixy;  // offset 16
  float4 inversemvpmatrixz;  // offset 32
  float4 inversemvpmatrixw;  // offset 48
  float4 prevmvpmatrixx;     // offset 64
  float4 prevmvpmatrixy;     // offset 80
  float4 prevmvpmatrixz;     // offset 96
  float4 prevmvpmatrixw;     // offset 112
  float4 smaajitter;         // offset 128
}
```

Its body: takes a single `POSITION`-only input (a full-screen
triangle/quad — no other vertex attributes), reprojects it through
`inverseMVP` then `prevMVP` (`dp4` against `cb0[0..3]` then `cb0[4..7]`) to
produce a previous-frame clip-space position alongside the current one —
textbook TAA/SMAA temporal reprojection for motion vectors. `smaajitter` at
offset 128 confirms the sub-pixel jitter tie-in. Exact offsets now recorded
for whenever per-eye motion-vector consistency is tackled (deferred, per the
existing plan — not scoped for the stereo-core milestone).

## 4. A real architectural gap: not every world shader computes `SV_Position` in the VS stage

Two of the 22 "no MVP" shaders (`AF80AA9287F65EA7`, and — inferred from its
buffer layout — likely `62F67B34913B0238`, both skinned-mesh shaders with
`jointBufferV`) have **no `SV_Position` in their output signature at all**.
Their outputs are `TEXCOORD0-4` + `WORLDPOS` — texture coordinates, a tangent
basis, and a world-space position, nothing D3D11 would accept as a rasteriser
input. The only way a vertex shader legitimately omits `SV_Position` is when
a **tessellation stage** (Hull + Domain shader) sits downstream and the
**Domain Shader** computes the final clip position instead. This is
corroborated independently: the binary's cvar list includes
`r_allowTessellation`, confirming the engine has a tessellation pipeline.

**Consequence for the stereo work**: `mvp_patch` only ever reads/patches
constant buffers bound to the **Vertex Shader** stage. For any draw using one
of these tessellated-mesh shaders, the actual camera transform happens later,
in a Domain Shader `mvp_patch` never touches. `mvp_row_offsets_for_shader()`
will correctly report "unknown" for these VS hashes (they have no MVP rows to
find), so `mvp_patch` will fail-safe and simply skip patching those draws —
**not** a bug, but it means: if any of Step 3's visible geometry uses
tessellation (a strong candidate: detailed character skin/face, given the
`jointBufferV` presence — possibly the protagonist's own model), expect that
specific geometry to **not** rotate with the rest of the world during the
`TEWVR_TEST_YAW` proof. That would look like a partial-rotation bug but is
actually an already-understood, pre-existing architectural gap — future work
would need to hook the Domain Shader's own constant buffers the same way.
Recorded as a new open risk in `ENGINE-DOSSIER.md` so it isn't mistaken for a
Task 6 regression if observed.

A separate, much simpler shader (`C989E1339BCD1117`, 468 bytes, 2
instructions) takes no input at all and outputs a hardcoded `(0,0,0,0)`
position — almost certainly an unused template/placeholder shader, not
worth further attention.

## 5. Cvar sweep: ~1,982 console variables exist, not the ~10 previously catalogued

A string-scan of `EvilWithin.exe` for id-Tech-style cvar name patterns
(`g_`, `pm_`, `r_`, `com_`, `con_`, `cg_`, `si_`, `in_` prefixes followed by
mixed-case identifiers) turned up **1,982 distinct candidate cvar/command
names** — the existing cheat sheet only ever recorded the handful found
during the original feasibility spike. Full list is not reproduced here
(too large and mostly irrelevant to VR); the high-value subset is folded
into `ENGINE-DOSSIER.md` §9. Highlights:

- **`noclip %s`** is a real console command (not just a cvar), alongside
  `pm_noclipspeed` — free camera movement without player collision, useful
  for any future manual or automated testing.
- **A built-in automated-screenshot smoke-test framework already exists**:
  strings like `"This is the logic for our AutoScreenShotSmokeTest"`,
  `MegaScreenShot`, `ScreenShotSmokeTestLogic`, alongside cvars
  `com_captureFrames`, `com_capturePath`, `com_captureTGA`,
  `com_captureSamples`. The developers apparently already built an in-engine
  frame-capture-to-disk system for their own QA. Not investigated further
  this session (out of scope — no Playbook Phase 2 harness work is planned
  right now), but worth flagging as a promising lead for whenever that phase
  is picked up: it may be considerably less work to drive the engine's own
  capture system via cvars than to build a D3D11 staging-texture readback
  from scratch.
- **`com_skipIntroVideo`, `com_skipPressButtonScreen`, `com_skipSignInManager`**
  — fast-launch cvars, same future-harness relevance.
- **`g_permaGodMode`** — useful safety net for any future manual testing
  session (removes death as a variable while poking at rendering).
- **No stereo/VR strings of any kind** (`stereo3d`, `oculus`, `openvr`,
  `vive`, `steamvr`, `nvidia3d`, `stereoscopic` — all zero matches). Confirms
  there is no legacy or hidden stereo-3D support to shortcut through; the
  from-scratch approach already under way is the only route.
- Confirmed `r_allowTessellation` exists (ties into finding 4 above) and
  `r_lod_fovScale` (FOV-dependent level-of-detail — worth remembering once
  a wide VR FOV is in play, LOD bias may need revisiting).

Partial level/stage-name evidence: stage IDs follow an `stNN_...` numbering
convention (`st00`, `st06_asylummain_...`, `st12`, `st13`, `st40_gamedesign_part_01_...`,
`st75` all appear as string fragments), confirming the numbering scheme
mentioned in passing elsewhere, though a full stage list wasn't recoverable
from exe strings alone (likely lives in a packed resource format, not scanned
this session).

## 6. Cross-thread context access: circumstantial evidence only, inconclusive

Task 6's re-review flagged an unresolved ⚠️: `Hook_CreateBuffer` calls
`GetImmediateContext()` + `Map()` from whatever thread `CreateBuffer` fires
on, which may not be the render thread — safe under D3D11's automatic
thread-safety unless the device was created
`D3D11_CREATE_DEVICE_SINGLETHREADED`. No static evidence of the actual
device-creation flags was found (they're a numeric constant at the call
site, not a string, and locating the exact `D3D11CreateDeviceAndSwapChain`
call site via blind disassembly of a 38 MB stripped exe wasn't attempted in
depth — diminishing returns for the time available).

One piece of circumstantial evidence *from data already on hand*: the same
seqdump gameplay capture shows the **immediate context receiving calls from
multiple different thread IDs at different points** (e.g. `tid=32176` issues
`RSSETVP`/`VSSETSHADER`/`DRAW` on `ctx=0x1557B215FF0`, and a few events later
`tid=34860` issues `MAP`/`UNMAP` on that *same* context pointer) across many
thousands of events, without the game ever crashing during any capture
session. That is at least consistent with the device *not* being
`SINGLETHREADED` (a genuinely single-threaded device would make cross-thread
calls to the same context a documented contract violation) — but it is the
engine's *own* cross-thread use of its own context, not proof that an
*external* caller (our hook) doing the same thing is equally safe. Left open
for Step 3 to watch for any D3D11 debug-layer warnings or instability, not
resolved here.

## What's still open for the human session

1. The actual yaw-rotation keystone proof itself (unchanged from before this
   research pass).
2. Whether the `CreateBuffer` candidate pool (cap 48) actually captures the
   true ~1920 B world-pool identities once real gameplay loads a level, now
   with a much more precise disambiguation method available if it doesn't
   (check the binding slot and whether the candidate is ever independently
   `Map`ped — a slot-2/3 or ever-Mapped candidate is not the world pool).
3. Whether any visible Step 3 geometry uses the tessellated/Domain-Shader
   path (finding 4) — if so, expect that geometry not to rotate, and don't
   mistake it for a patch bug.
4. The cross-thread device-flag question (finding 6) — genuinely unresolved,
   would need either a live D3D11 debug-layer capture or a deeper static
   disassembly effort than was justified this session.
