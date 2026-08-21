# 10 — The Phase 4 keystone proof (Task 6 resolved)

**Date:** 2026-08-21. **Status: Task 6 RESOLVED — the Playbook Phase 4 keystone
gate is passed.** The mod owns and can rewrite the world's per-draw camera
transform on real, deferred-recorded, in-game geometry. This is the single
feasibility proof the whole VR conversion rested on.

## Three false starts, and why they were false

Notes 06c/08 established *what* to patch (a per-draw MVP in each shader's
slot-0 cbuffer) and *where* it lives (a small pool of buffers bound during
deferred-context world draws). What remained was *how to read it cheaply and
correctly enough to patch it live* — and every attempt to answer that question
before this session turned out to be wrong in a way that only real gameplay
testing exposed:

1. **A once-per-frame batched snapshot** of the pool buffers. Wrong because
   ~1900 draws/frame share only ~6 buffer identities — a once-per-frame
   snapshot gets reused unchanged across ~300 different draws, corrupting
   every per-object constant in that window, not just the MVP rows.
2. **A persistent `Map` captured at `CreateBuffer` time.** Wrong for a
   fundamental reason discovered only this session: the real world cb0
   buffers are created `D3D11_USAGE_DEFAULT` with `CPUAccessFlags=0` — a
   buffer of that usage class **cannot be `Map`ped under any circumstance**,
   by the D3D11 API's own rules. Every "successfully captured 1920-byte
   buffer" from the two rounds that used this approach was actually capturing
   a same-size **decoy** buffer pool that happens to also be 1920 bytes but is
   never bound at VS slot 0 during a real world draw. Filtering by byte-width
   alone (even a very tight band) cannot tell the two pools apart, because the
   only real distinguishing property — "is this buffer actually used as an
   MVP source" — is only observable at the point of use, not at creation.
3. **A hook-install timing race**, briefly suspected as the cause of the
   above symptom. Directly falsified: hooks were measured live at 453 ms
   after `DllMain` and 2.78 s before the first draw, and a buffer-identity
   cross-check showed the hook never missed a buffer of the type it was
   watching for. The buffers it was watching for were simply the wrong ones.

## The real mechanism

- **Read path:** hook `ID3D11DeviceContext::UpdateSubresource` (the only CPU
  write path a `D3D11_USAGE_DEFAULT` buffer supports) and shadow every write
  into a CPU-side cache keyed by buffer identity. A partial-region
  `UpdateSubresource` (a non-null `pDstBox`) is handled with a bounds-checked
  offset `memcpy`, not just detected and refused.
- **Registration:** moved to the *draw* path, not buffer creation. A buffer
  identity is only ever registered as a real target when it is actually bound
  at VS slot 0 for a draw whose current shader has a known, contiguous
  `mvpmatrix` offset. Decoys — buffers that merely happen to match a size
  filter — structurally cannot occupy a pool slot under this scheme, because
  they are never bound that way.
- **Patch:** at each `DrawIndexed`/`Draw`, read the shadowed MVP rows, apply
  `K * mvp` (`K` a per-eye or, for this proof, a test rotation), write the
  result into our own scratch constant buffer, rebind VS slot 0 to it, then
  call the original draw — so the substitution is what gets recorded into the
  command list and replayed.
- **Concurrency:** the shadow's single-writer-per-buffer assumption, taken on
  faith in earlier rounds, turned out to be **false** in real gameplay —
  448,201 of 560,109 shadow writes were cross-thread (worker threads hand
  these buffers between each other across frames). A per-slot seqlock now
  detects this rather than assuming it away: writes are frequently
  cross-thread but were never observed *concurrent* (zero torn reads across
  every session tested), and the fail-safe path (skip the patch on any
  detected tear) is exercised in code even though it has not yet fired live.

## The proof

Three frames, captured via an autonomous self-launch harness (`EvilWithin.exe`
launched directly with `+devmapjump st06_asylummain`, no human at the
keyboard):

- **Baseline** (mechanism installed, no test rotation): the player character
  in a normal room — lockers, a door, a light fixture.
- **Control** (mechanism installed, `K` = identity): visually indistinguishable
  from baseline down to film-grain noise and idle-animation sway — proof the
  patch mechanism itself introduces no corruption.
- **Test** (`K` = a 90° rotation): the same room, drastically reoriented. Most
  of the frame goes black because the test rotation is applied as a
  clip-space post-multiply (`K · mvp`), which swaps the depth and horizontal
  axes and clips most geometry away — an expected, predicted consequence of
  using a rough test transform rather than a proper view-space rotation, not
  a rendering fault.

The proof that this is a real, selective transform and not corruption: a
lighting effect with no `mvpmatrix` (left deliberately unpatched, since it
carries no camera-space geometry) sits at the same screen position — within
3 pixels — in both the baseline and the rotated frame, while every
MVP-bearing object visibly moved. An unrelated scene, a moved camera, or
generic rendering corruption would not produce that signature.

## Known gaps carried into Task 7/8

- **`K` here is a raw clip-space post-multiply, not the documented per-eye
  maths.** The real `K_eye = P_eye · T_eye(±IPD/2) · P⁻¹` (note 06c) must be
  used for actual stereo — this proof only needed to show the substitution
  mechanism works, not to look correct.
- **Coverage is ~74–77%, not complete.** Roughly a quarter of MVP-bearing
  draws still render unpatched: some because their shader's MVP rows are
  not contiguous (a known, older finding — the current mechanism only
  patches contiguous layouts) and some because their bound buffer was never
  captured (`pool_miss`) for reasons not yet root-caused. In a stereo build
  this fraction would render at the wrong per-eye orientation.
- **Writer/writer exclusion is absent**, justified by live measurement rather
  than by construction. If the engine's buffer hand-off pattern ever changes
  (different hardware, different level, a future patch), the seqlock's
  detection could in principle miss three-or-more-way concurrent writers; a
  real per-slot lock would become necessary if `CONCURRENT shadow writes`
  is ever observed non-zero. The counter that would show this currently only
  prints during a diagnostic window, not continuously — a known, tracked gap.
- Two post-process shader classes (SMAA/motion vectors, consuming
  `inversemvpmatrix`/`prevmvpmatrix`) and a small set of tessellated
  skinned-mesh shaders (which compute final position in a Domain Shader, not
  the Vertex Shader — see 09-offline-research.md) remain outside this
  mechanism's reach, as previously flagged.

No game files were read, copied, or modified. All instrumentation and the
patch mechanism live in our own injected module, operating on interoperability
data (buffer contents at the D3D11 API boundary) exposed by our own hooks.
