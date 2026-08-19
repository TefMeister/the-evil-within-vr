# 06 — Finding the camera matrix (Task 4)

> **⚠️ CORRECTION (2026-08-19): the finding below is WRONG and superseded.**
> The 384-byte "view matrix at byte 48" described in this note turned out, on
> live visual testing, to be a **per-object model matrix** (a cloth mesh on the
> player character), not the camera. A matrix that is orthonormal and varies
> with the camera can just as easily be an object that moves with the camera.
> The corrected, still-in-progress findings are in
> [06b-render-pipeline-findings.md](06b-render-pipeline-findings.md). The
> original text is kept below as a record of the false lead.

---

The heart of the whole mod: locating the game's camera view matrix, so we can
override it per eye to create stereo. **The view matrix is found.** The
projection / combined view-projection matrix still needs one more capture (see
"What's still open").

## Method

1. **Instrumentation** (temporary, behind `TEWVR_DUMP=1`): hooks on
   `ID3D11DeviceContext::Map`/`Unmap` and `UpdateSubresource` dump every
   constant buffer in the 64–512 byte range to `%LOCALAPPDATA%\TEWVR\cbdump.log`,
   decoding the bytes as 4×4 float matrices. Per-frame camera buffers turned out
   to be dynamic (`Map`/`DISCARD`), so the `Map`/`Unmap` path is the one that
   fires.
2. **Capture**: a ~2.5-minute gameplay session (loading a save, walking, and
   panning the camera around) produced ~40,000 records across many buffers.
3. **Analysis** (offline, Python): for every buffer, and every 16-float window
   within it, test whether the upper-left 3×3 is orthonormal (a rotation) and
   whether it *changes over time* (as the camera turned). A view matrix is the
   unique thing that is both a valid rotation and continuously varying with the
   player's aim, with a translation that tracks the player's position.

## The finding

Across all 40,000 records, **exactly two windows** matched the "orthonormal and
varying" test — and they are two copies of the same buffer (the engine
double-buffers it):

- **Buffer size:** 384 bytes, dynamic (`Map`/`DISCARD`), uploaded every frame.
- **View matrix offset:** byte **48** within the buffer (float index 12).
- **Layout:** a row-major **3×4 affine** — three rows of `[rotation_row | translation]`:
  ```
  [ right.x  right.y  right.z | tx ]
  [ up.x     up.y     up.z    | ty ]
  [ fwd.x    fwd.y    fwd.z   | tz ]
  ```
  The 3×3 rotation stayed orthonormal (row norms 1, mutually perpendicular)
  while rotating as the camera was panned; the translation column drifted as the
  player walked. Both are exactly what a world-to-view matrix does.
- The matrices in this buffer appear to be **48-byte (3×4) aligned**: the block
  at bytes 0–48 was an identity transform, the view sits at bytes 48–96, and
  further transforms follow in the 96–384 region we did not capture.

Because only these two windows matched out of 40,000 records, the
identification is unambiguous — nothing else in the frame's constant data is a
continuously-rotating orthonormal matrix.

## What's still open

- **The projection (and any combined view-projection) matrix.** The first
  capture dumped only the first 128 bytes of each buffer, so the matrices beyond
  byte 128 in this 384-byte buffer were never seen. The projection almost
  certainly lives there. This matters for a specific reason: if the vertex
  shaders consume a **combined view-projection** matrix rather than the separate
  view matrix we found, then overriding the view matrix alone will not move the
  rendered image — we would need to override the combined matrix (or recompute
  it). Determining which matrix the shaders actually use is the next required
  step before the override work (Task 6).
- **A stable runtime identifier for the buffer.** The resource pointers seen
  this session (`0x21AD3E186B8` / `0x21AD3E1DD38`) are allocation addresses that
  will differ every launch. The override in Task 6 must recognise this buffer by
  a stable property — its size (384) combined with a content check (a
  384-byte dynamic cbuffer whose bytes 48–96 form an orthonormal matrix) — not
  by pointer.

## Next step

The instrumentation has been widened to dump full buffers (up to 512 bytes) and
its capture caps raised, so a single short gameplay capture will now record the
complete 384-byte camera buffer. Analysing that full capture will reveal the
projection / view-projection matrices and settle which one the render pipeline
consumes. After that, Task 6 can override the correct matrix per eye.

No game files were read, copied, or modified. The analysis operated only on our
own dump of constant-buffer contents — interoperability data about how the
engine feeds the GPU, captured from within our own hook.
