# 07 — Double-render strategy: how the second eye gets produced (Task 5 closed)

**Date:** 2026-08-20. **Status: Task 5 RESOLVED.** Decision: **re-execute the engine's
per-frame deferred command lists once per eye, with a per-eye camera patch**, not a
draw-call replay or an engine scene-render re-invocation. The evidence below is what
forced that choice.

Task 4 (see 06c) established *what* to change per eye: left-multiply every per-draw MVP by
one constant `K_eye`. Task 5 asked the separate question of *how to make the engine draw
the whole scene a second time* so that patch has two eyes to apply to. Answering it meant
learning how this engine actually submits a frame — which turned out to be the biggest
structural finding of the project so far.

## Method

Extended the winmm proxy's instrumentation (`seqdump.c`, gated on `TEWVR_SEQDUMP=1`) into an
ordered, timestamped event stream — every `Map`/`Unmap`/`UpdateSubresource`,
`VSSetConstantBuffers`, `VSSetShader`, `Draw*`, `Present`, plus (added as the picture
developed) `FinishCommandList`, `ExecuteCommandList`, `RSSetViewports`,
`OMSetRenderTargets`. Each event carries its sequence number, thread id, and the **D3D11
context pointer** it was issued on. A file-triggered arm (`seqarm.txt`) lets us start the
40,000-event capture from outside the process once real gameplay is on screen, rather than
burning the budget on menus and loading. Two gameplay captures plus one live visual
experiment settled it.

## Finding 1 — the engine records the frame on deferred contexts, in parallel

The menu looked simple: one thread, fills always preceding binds. Gameplay did not. Draw
calls arrive from **six different D3D11 device contexts on six worker threads**, while
*all* state setup — `VSSetShader`, `VSSetConstantBuffers`, `Map`/`Unmap` of the constant
buffers — and every `Present` happen on a single seventh context.

The tell is the command-list traffic. The six worker contexts each end their work with
`FinishCommandList`; the single main context issues `ExecuteCommandList` (≈200–300 per
frame) and nothing else records. That is the textbook D3D11 **deferred-context** pattern:
worker threads record command lists in parallel, the immediate context replays them. The
recorded state calls run on the *deferred* contexts, whose vtable method implementations
differ from the immediate context's, which is why our first-pass hooks (installed on the
immediate context's vtable) saw the worker *draws* but none of their *state* — we had to
late-hook each deferred context's own vtable the moment it first appeared.

Timing within a frame: command lists are recorded and executed inside the same frame
(finish→execute gap a median of ~135 events), the pool of command-list objects is reused
frame to frame, and constant-buffer fills never occur after the frame's last
`ExecuteCommandList` (0 of 7,588) — so a list's contents are baked at record time, not
late-bound.

## Finding 2 — the world lives in the deferred lists (the decisive experiment)

We needed to know *what* those deferred lists actually draw, versus what the immediate
context draws directly. Rather than infer it, we tested it live: a `skipcl.txt` toggle
makes the `ExecuteCommandList` hook drop the original call while the file exists. Flipped it
on for ~12 seconds during gameplay (the user watching the screen; the game did not crash)
and flipped it back.

**Result (user-observed):** with the command lists skipped, the world went *mostly black* —
all environment geometry and Sebastian's body vanished. What remained was his **hair**,
plus **windows and light sources**.

Corroborated by the render-target sizes from the same capture: the main scene is drawn to
1280×720 colour targets (several formats — the G-buffer/HDR chain) with a 1280×720 depth
target; shadow passes are depth-only at 512²/1024²/2048²; the small 160×90…640×360 targets
are the bloom/SSAO downscale chain.

Conclusion, no longer a guess: **the deferred command lists carry the bulk of the frame —
opaque world geometry and character bodies (≈7× the index volume of the immediate path).**
The immediate context draws only the remainder: a separate hair pass, emissive/lights/
windows, and HUD/post.

## Why this decides the strategy

A stereo camera override **must reach the deferred-recorded draws.** Patching only what the
immediate context submits — which is where the shared camera constants are set — would move
the hair and the lights but leave the entire world locked to one eye. That single fact
eliminates the "just override the shared camera cbuffer" shortcut for good, and it also
rules out a naive draw-call replay that only sees immediate-context draws.

Three mechanisms were on the table:

- **A — re-invoke the engine's scene-render function a second time** (the plan's original
  preferred path). Rejected: there is no single engine scene-render call to re-enter; the
  frame is composed as parallel command-list recording across six threads. Re-invoking that
  safely from a Present-time hook is far more invasive than the plan assumed.
- **B — patch at record time, forcing a second recording per eye.** Rejected: the engine
  records each list exactly once. We do not control the record loop, so we cannot make it
  record twice without engine-side hooking we have no handle on.
- **C — re-execute the already-recorded command lists a second time per eye**, with a
  per-eye viewport (each half of the back-buffer) and the per-eye `K_eye` applied to the
  constants the lists consume. **Chosen.** It rides the engine's own submission path, needs
  no per-object knowledge (Task 4), and the `ExecuteCommandList` hook where we'd drive it is
  already written and proven controllable (the skip experiment used exactly that seam).

## The one crux this hands to Task 7

Command lists bake their *bindings* at record time, including which dynamic constant buffers
they reference. The open question is what a **second** `ExecuteCommandList` of the same list
observes: does it re-read the *current* contents of those dynamic constant buffers (so we
can rewrite the MVP constants between the two executes and get two eyes for free), or did
the Map/DISCARD at record time pin per-object contents that a re-execute would just reuse
unchanged? This is the first thing Task 7 must resolve empirically. If re-execute reuses
pinned contents, the fallback is to record our own second pass — replaying the captured draw
sequence with per-eye constants — using the per-shader `mvpmatrix*` offset table we already
build at `CreateVertexShader` time (Task 4/5 tooling).

Either way, the double-render **mechanism** is settled: per-eye re-execution on the
immediate context, half-back-buffer viewports, `K_eye` on the constants. That is Task 5's
deliverable.

## Tooling state

All discovery hooks live behind env vars (`TEWVR_SEQDUMP`, `TEWVR_SEQDUMP_ARMFILE`,
`TEWVR_SKIPCL`) and default off; the shader-hash → `mvpmatrix*` offset table
(`mvptable.c`, via `D3DReflect` at shader-creation time) is populated and queryable for
Task 6/7. These are temporary instrumentation and will be narrowed when the real per-eye
path is built. Raw captures are archived under `captures/task5-gameplay/`.
