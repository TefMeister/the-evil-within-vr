# 08 — Where the world's per-object matrix actually lives (Task 6 discovery closed)

**Date:** 2026-08-20. **Status: Task 6 blocking question ANSWERED.** The per-object
model-view-projection matrix for the deferred-recorded world geometry lives in **VS slot 0
(`constantBufferV`/b0), at each shader's reflected `mvpmatrix` offset, inside a small pool of
persistently-mapped 1920-byte constant buffers.** We can read each draw's real matrix at
record time, which is exactly the seam a per-eye patch needs.

This note explains how we got there, because the path corrected two wrong assumptions.

## The puzzle we started with

Task 5 established that the world is drawn on six deferred contexts (worker threads) whose
command lists the immediate context replays. Instrumenting per-draw state showed every
deferred world draw *bound* a 1920-byte buffer to VS slot 0 — but that buffer was **never**
seen written: no `Map`, no `Unmap`, no `UpdateSubresource`, on any context. And 1920 bytes
didn't match the 96/160/224-byte `constantBufferV` the shaders reflect. So we could not
explain how each object got a different matrix.

## Two hypotheses, tested in order

**Hypothesis A — a hidden fill/streaming context we weren't hooking.** Our deferred-context
late-hooking only triggered when an unseen context issued a *draw* or *finish*; a context
that only ever *mapped* buffers would slip past. We broadened the late-hook to also trigger
from `Map`/`Unmap`/`VSSetConstantBuffers`, and recaptured.

Result: **refuted.** Still only the immediate context ever mapped anything, and its map sizes
were 96/32/16/224/656/128/48/64 — never 1920, never 384. No hidden context appeared. The
1920-byte buffers are genuinely never CPU-written through any mapping API during the frame.

**Hypothesis B — read the bytes directly.** We added `TEWVR_CBPEEK`: a staging buffer plus,
at the first few hundred deferred draws, a `CopySubresourceRegion` of the bound slot-0/2/3
buffers into staging, a `Map(READ)`, and a log of each buffer's content hash and the four
floats at the shader's reflected `mvpmatrix` offset.

Result: **answered.** Two consecutive world draws:

```
CBPEEK slot=0 res=0x…03538 size=1920 mvpoff=32 mvp=[-0.094  0.228 -0.040 -117.412]
CBPEEK slot=0 res=0x…02578 size=1920 mvpoff=32 mvp=[ 0.451  0.025  0.000  -49.676]
```

Different buffers, different matrices. Across ~150 reads: **6 distinct 1920-byte buffers,
carrying 16+ distinct contents.** Some buffers were rewritten in place between draws (a ring
whose contents cycle); others held one matrix across a batch of draws (a mesh drawn in parts,
or a shared transform). The `mvpmatrix` sits at the shader's reflected offset — mostly +32,
sometimes +48/+0 for other shader layouts.

## What it means

The world uses a **small pool of persistently-mapped dynamic constant buffers** — one ring
per worker thread (6 buffers, 6 threads). The engine maps each once and then writes
per-object matrices by CPU `memcpy` into the persistent pointer, never re-calling `Map`. That
is why every mapping hook came up empty, and it is a completely ordinary pattern for a
high-throughput id Tech-derived renderer. The 1920-byte physical size is just the ring
slot; the shader reads only its small declared `constantBufferV` from the front, with the
matrix at the reflected offset within it.

The earlier fear — that the matrix arrived by some mechanism we could neither see nor touch —
is gone. **At each deferred draw we can identify the bound slot-0 buffer and read the exact
matrix the object is about to use.**

## The patch point this hands to the implementation

Per eye, at each deferred `DrawIndexed` during recording: read the current matrix rows, left-
multiply by the constant per-eye `K_eye` (note 06c), write the patched `constantBufferV` into
**our own** constant buffer, and rebind slot 0 to it before calling the original draw. Our
buffer is what gets recorded into the command list, so the object draws from the shifted eye
on replay. This is simultaneously Task 6's proof-of-control (use a yaw `K` and the whole
world rotates) and the core of the stereo double-render, so Tasks 6 and 7 effectively merge
here. It also retires Task 5's open question about whether a second `ExecuteCommandList`
re-reads constants — this mechanism substitutes at record time and does not depend on that.

Implementation notes carried forward:

- Our substitute buffers need their own ring: within one frame many recorded draws must each
  reference distinct patched contents, so a single reused buffer will not do.
- Building `K_eye` needs the projection `P` (from `g_fov` + aspect, or recovered from a
  matched matrix/model pair).
- The read-at-draw path used for discovery (`TEWVR_CBPEEK`) crashed the game once *after* its
  capture completed — a deliberately-bounded probe that cached buffer pointers without a
  reference and copied cross-thread. Before the real patch path reuses any of that, the
  buffer lifetime (AddRef/Release) and the cross-thread copy must be made safe; the throwaway
  probe's tolerances do not carry over.
- Not every material shader lays `mvpmatrix` rows out contiguously at +16/+32/+48; the
  reflection table already records the true per-row offsets, and the patch must use them
  rather than assume contiguity.

## Tooling state

`TEWVR_SEQDUMP` (with the broadened late-hook trigger) and `TEWVR_CBPEEK` are committed,
env-gated, and off by default; both are discovery instruments and will be narrowed or removed
when the patch path is built. Captures archived under `captures/`.
