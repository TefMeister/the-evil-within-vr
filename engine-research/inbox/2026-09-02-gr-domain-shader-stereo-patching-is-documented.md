# Domain-shader-stage per-eye patching has a documented template — bears on §8/§12's tessellation gap

**Date:** 2026-09-02 · **From:** `/gr` (this repo's own research lane) · **For:** the modding
session to fold into `ENGINE-DOSSIER.md` (create-only inbox drop; fold in and delete)

**Bears on:** §8 — "At least two skinned-mesh vertex shaders have no `SV_Position` in their VS
output signature... the final clip-space transform happens downstream in a Domain Shader... Not
a patch bug if observed — a known gap for future work (would need to hook the Domain Shader's
own constant buffers the same way)." And §12's listing of the same gap as deferred, not scoped
for the stereo-core milestone.

## What's new

NVIDIA's own patent (US10068366B2, "Stereo multi-projection implemented using a graphics
processing pipeline") documents exactly this shape of fix as a shipped mechanism: a domain
shader reading **per-view constants** to translate barycentric-interpolated tessellator output
into final per-eye clip-space positions, structurally parallel to this project's own VS-stage
`K_eye` left-multiply (§6). Full write-up:
`external-research/topics/2026-09-02-domain-shader-stage-per-view-transform-is-documented-prior-art.md`.

## Suggested dossier change

When §8/§12's Domain-Shader gap is eventually picked up, it isn't a cold-start problem — the
fix is "the same idea as §6/§7, one stage later": reflect the DS's own constant buffer(s) for a
per-view/projection-relevant row (same discipline as the VS `constantBufferV` row search), then
apply a per-eye write at the DS stage instead of (or in addition to) the VS. Worth a one-line
addition to §12's bullet noting this template exists, so a future session doesn't have to
re-establish that the approach is sound before starting.

(The other research target this pass — whether id Tech 5's job system structurally guarantees
non-concurrent cross-thread constant-buffer writes, bearing on §7/§12's shadow writer-concurrency
risk — came back with nothing public. No dossier change suggested there; it stays exactly as
"measured safe, not structurally proven.")
