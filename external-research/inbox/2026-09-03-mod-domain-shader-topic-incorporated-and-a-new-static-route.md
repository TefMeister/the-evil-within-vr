# Modding verdict: the domain-shader topic is folded into §12 — and a new static route the sweep can help with

**From:** `/pd` (modding lane), 2026-09-03, dev PC
**About:** `topics/2026-09-02-domain-shader-stage-per-view-transform-is-documented-prior-art.md` (currently 🆕 new)
**Suggested INDEX status:** ✅ **incorporated**

## What was done with it

The `/gr` inbox drop was drained and its suggestion taken verbatim: `ENGINE-DOSSIER.md` §12's
Domain-Shader bullet now records that picking that gap up is **not a cold start** — the recipe is
"reflect the DS's own constant buffer(s) for a per-view/projection row, the same discipline as the VS
`constantBufferV` search, then apply the per-eye write at the DS stage", with the patent cited as a
documented template and tagged `[reported]` (it is prior art, not something this project has tried).

The drop's second half — nothing public on whether id Tech 5's job system structurally guarantees
non-concurrent cross-thread constant-buffer writes — was correct to suggest no dossier change. That
risk stays worded as "measured safe, not structurally proven".

## A new research target this session opened, if a future sweep wants one

The game **ships its shaders on disk**, which nothing in this project had recorded:
`base/common.tangoresource` holds **76 entries** named
`generated/renderprogs/shader_retail/pc/<name>.shaderbin2`. The container's TOC is now fully parsed
(11,565 records, matching its own header count) and the payload is **raw deflate**
`[verified-numerically 2026-09-03]` — but entry offsets are not in the TOC, and **whether
`.shaderbin2` holds DXBC or id Tech 5's own IR is unknown**.

Two things public research could genuinely help with, both engine-level rather than game-specific:

1. **The `.tangoresource` container layout** — magic `0x2394ABCD`, big-endian entry count, paired
   name records with a trailing `u32` hash, and a regular 10-byte-record index at the end of the
   file. Any published id Tech 5 / Tango extractor that names those fields would save reversing the
   offset table by hand.
2. **What a `shaderbin2` actually wraps.** id Tech 5's `renderprogs` are usually described in terms
   of its own shader language; whether the retail PC build stores compiled DXBC or an intermediate
   form decides whether this route retires a `[FLAT]` row or dies.

⚠️ Please treat both as *leads to check*, not as established: this session's own first attempt at the
compression was a badly-designed test (it looked only for zlib framing, which headerless raw deflate
does not have) and would have been recorded as a false negative had it not been retried.

Full write-up: `modding-notes/2026-09-03-branch-merge-tested-and-the-shaders-are-on-disk.md`,
dossier §3a.
