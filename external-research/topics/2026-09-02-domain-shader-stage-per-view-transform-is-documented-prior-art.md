# Domain-shader-stage per-view transform is documented, patented prior art — a template for the tessellation coverage gap

**Bears on:** `ENGINE-DOSSIER.md` §8 and §12 — the two skinned-mesh vertex shaders with no
`SV_Position` in their output signature, where the final clip-space position is computed
downstream in a **Domain Shader**, invisible to the current VS-only `mvp_patch` mechanism.

## The question

`mvp_patch` today only patches vertex-shader-bound constant buffers. When tessellation is
active, the VS never produces a final position at all — it hands control points to the
fixed-function tessellator, which hands barycentric-interpolated points to the **domain
shader (DS)**, which computes the actual output position. Is there any public precedent for
applying a per-eye/stereo transform *at the domain shader stage itself*, or is this uncharted
territory for a from-scratch mod?

## What's public

**It is a documented, shipped mechanism — not uncharted.** NVIDIA's own patent **US10068366B2,
"Stereo multi-projection implemented using a graphics processing pipeline"** (assignee: NVIDIA
Corp) `[reported 2026-09-02]` describes exactly this shape of solution, as the mechanism behind
Simultaneous Multi-Projection (SMP)-style stereo/multi-view rendering:

> "the domain shading stage is implemented by a streaming multiprocessor that executes a domain
> shader that includes instructions that generate multiple position vectors for each vertex
> corresponding to each view of the at least two views."

And, on the actual mechanism:

> "the patches operated on by the domain shading stage may be specified in barycentric
> coordinates and the domain shader may use constants associated with an associated patch in
> order to translate the barycentric coordinates to actual coordinates for a particular view
> (i.e., ⟨x, y, z, w⟩)."

In plain terms: rather than only ever computing one final position per tessellated vertex, a
domain shader can be written (or, per the patent, hardware-scheduled) to run once per view,
reading **per-view constants** and applying them at the exact point where it turns
barycentric-interpolated patch data into a final clip-space position — the DS-stage equivalent
of this project's own VS-stage `K_eye` left-multiply (§6). The output then carries one position
vector per view rather than one.

This confirms the *shape* of a fix, at the architecture level: a Domain-Shader path needs its
own constant-buffer discovery (same reflection-driven approach already used for VS `constantBufferV`
rows) and its own per-eye write, structurally parallel to what `mvp_patch` already does for the
VS — not a fundamentally different problem, just one stage later in the pipeline.

**Confirmed separately** (Microsoft's own D3D11 pipeline docs, `[reported]`): the domain shader
"is invoked for each point the fixed-function tessellator generates" and "will see as its input
all the hull shader's output control points and all the output patch constant data; the shader
evaluates the patch at its location" — i.e. the DS's inputs are exactly the tessellator's
barycentric output plus HS patch-constant data, matching the patent's description of what it
transforms.

## What's still unknown for this project specifically

- Whether *this game's* two no-`SV_Position` vertex shaders actually feed a Domain Shader with
  its own separate constant buffer holding view/projection-relevant data, or something more
  indirect (unmeasured — the dossier's own framing, "likely candidate: detailed character
  skin/face geometry," is still `[hypothesis]`).
- No reflection dump of any Domain Shader in this game has been taken; the DS's own
  `constantBufferX`-equivalent naming and row layout is completely unknown.
- This is deferred work, not scoped for the stereo-core milestone (dossier §12) — this finding
  doesn't change that scoping, it just means the eventual work has a known template rather than
  being a cold-start problem.

## What did NOT pan out

Searched for id Tech 5's own public threading/job-system documentation (the SIGGRAPH talk
"id Tech 5 Challenges: From Texture Virtualization to Massive Parallelization" by
J.M.P. van Waveren) for anything settling whether the dossier §7/§12 shadow writer-concurrency
pattern (buffers handed between worker threads across frames, but never observed genuinely
concurrent) is a structural guarantee of id Tech 5's job system or coincidental on this
hardware. **No usable result:** the SIGGRAPH deck is image-only slides (not machine-extractable
text), and every secondary source found (Wikipedia, HotHardware's "Rage: The Tech Behind id Tech
5", ModDB, Gamicus) restates only that "jobs are categorized and assigned to
threads/SPEs" with no detail on per-resource ownership or write concurrency. `[checked
2026-09-02, no public answer]` — this stays exactly where the dossier already has it: measured
safe on this machine, not structurally proven, and not further resolvable from public sources.

## Sources

- US Patent 10068366B2, "Stereo multi-projection implemented using a graphics processing
  pipeline" (NVIDIA Corp): https://patents.google.com/patent/US10068366B2/en
- Microsoft Learn / Win32 docs, "Direct3D 11 Advanced Stages: Hull Shader Design" (control
  point / patch-constant data flow into the domain shader):
  https://github.com/MicrosoftDocs/win32/blob/docs/desktop-src/direct3d11/direct3d-11-advanced-stages-hull-shader-design.md
- J.M.P. van Waveren (id Software), "id Tech 5 Challenges: From Texture Virtualization to
  Massive Parallelization" (SIGGRAPH, Beyond Programmable Shading course) — checked, no
  extractable detail: https://mrl.cs.vsb.cz/people/gaura/agu/05-JP_id_Tech_5_Challenges.pdf
- HotHardware, "Rage: The Tech Behind id Tech 5" — checked, no relevant detail beyond
  "jobs assigned to SPEs": https://hothardware.com/reviews/rage-the-tech-behind-id-tech-5

## ✅ Outcome 2026-09-03 — folded into dossier §12 verbatim (from `inbox/`)

The Domain-Shader bullet in §12 now records that picking that gap up is **not a cold start**: reflect
the DS's own constant buffer(s) for a per-view/projection row — the same discipline as the VS
`constantBufferV` search — then apply the per-eye write at the DS stage, with the patent cited as a
documented template and tagged `[reported]` (prior art, not something this project has tried). The
second half — nothing public on whether id Tech 5's job system structurally guarantees non-concurrent
cross-thread constant-buffer writes — was correct to suggest no dossier change; that risk stays worded
"measured safe, not structurally proven".

A side effect of that same session, recorded here because it changes what is worth researching: **the
game ships its shader bytecode on disk** — `base/common.tangoresource` holds 76
`generated/renderprogs/shader_retail/pc/*.shaderbin2` entries, and by the end of the day the container
was fully parsed and **2,785 DXBC shaders with RDEF intact** were extracted, `constantBufferV` named
with `mvpmatrixx/y/z/w` at explicit offsets in 1,208 of them, matching the runtime table 167/168 by
hash with zero disagreements `[verified-numerically 2026-09-03]`. The two research targets the
morning drop opened (container layout; what `.shaderbin2` wraps) were **withdrawn the same day as
solved in-house** — do not spend a sweep on them. What remains is small: see the 2026-09-03 topic on
the 249 entries that will not inflate.
