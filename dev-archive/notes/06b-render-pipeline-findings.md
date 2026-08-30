# 06b — Render-pipeline findings (Task 4, corrected)

This note supersedes the camera-matrix claim in
[06-camera-matrix-discovery.md](06-camera-matrix-discovery.md). Task 4 is **not
finished**: we have characterised the shared constant buffers and narrowed the
problem, but the world-geometry transform is not yet pinned down.

## What went wrong with the first attempt

The first approach dumped constant buffers and searched their contents for a
matrix that was orthonormal (a rotation) and changed as the camera moved. That
found a 384-byte buffer — but live testing (perturbing it and watching the
screen) showed it moved a **single object** (a cloth mesh on the player), not
the world. **Lesson: a matrix that rotates with the camera can be an object's
model matrix, not the view.** Content heuristics alone can't tell them apart;
you have to perturb the buffer and see what actually moves on screen.

## The method that gave real answers

Two tools, used together:

1. **Bind counting** (`TEWVR_FINDCAM=1`): hook `VSSetConstantBuffers` and count
   how many draws bind each constant buffer to each vertex-shader slot. A shared
   per-frame buffer is bound for almost every draw; a per-object buffer is bound
   only for its own draws.
2. **Live non-uniform perturbation** (`probe.txt` live-control): wobble a chosen
   buffer's contents each frame and watch the screen. A **key gotcha**: a
   *uniform* scale of a projection/view-projection matrix is **invisible** —
   it cancels in the perspective divide (x/w, y/w). You must perturb
   **non-uniformly** to see any effect.

## What each shared buffer actually is

By perturbing each of the most-bound buffers and watching the game (in real
gameplay, confirmed by the player):

| Buffer | Bound to | Effect when perturbed | So it is… |
|---|---|---|---|
| 96 bytes | VS slot 0, ~717k draws (most-bound) | lens flares, illumination, brightness, camera-angle-dependent lighting | **frame / lighting constants** (holds camera data used for *lighting*) |
| 64 bytes | VS slot 0, ~49k | hue shifts, colour cycling, UI colour | **colour grading / post** |
| 128 bytes | VS slot 0, ~45k | lights flashing on/off | **lighting** |
| 8064 bytes | VS slots 2/3 | (not fully tested) | likely **skinning / bone palettes** |
| 384 bytes | per-object (14 instances) | one object deforms | **per-object model matrix** |

**Crucially, no single shared buffer moves the world geometry.** The walls and
floor never deform, no matter which shared buffer we perturb — only lighting,
colour, or individual objects change.

## Working conclusion

The engine (a heavily modified id Tech 5) appears to transform world geometry
with **per-object matrices computed on the CPU**, not a single shared camera
matrix. The camera's view data exists (in the 96-byte lighting buffer) but the
geometry does not read it directly for positioning. This is the harder class of
engine for VR: stereo has to offset each object's transform per eye, rather than
one camera matrix.

## Next step (not visual probing)

Visual probing has reached its limit — it is imprecise, and the game pauses when
it loses focus. The definitive next move is **shader-level reverse engineering**:
attach x64dbg to a live world-geometry draw and trace exactly which constant
buffer and matrix produce the vertex's clip-space position, or disassemble the
world-geometry vertex shader. That tells us precisely what to modify for stereo,
and it does not depend on eyeballing perturbations.

No game files were read or copied. All findings come from our own instrumentation
of the running process's constant-buffer traffic.
