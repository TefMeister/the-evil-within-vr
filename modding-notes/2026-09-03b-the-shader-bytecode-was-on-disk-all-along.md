# The shader bytecode was on disk all along — the coverage-gap blocker is gone

**2026-09-03, dev PC, `/pd` (parallel development), second pass.**
**The game was not launched. Nothing here has been run against The Evil Within.**

The earlier pass today found that the game ships its shaders inside `base/common.tangoresource` but
could not address individual entries, and left the honest verdict: *"whether a `.shaderbin2` contains
DXBC at all is unknown; it may be id Tech 5's own IR, in which case this route dies."*

It does not die. The container is now fully unpacked, the payloads **are** DXBC with reflection
intact, and §12's *"blocked on one launch to save vertex-shader bytecode"* is **retired** — not
worked around, removed.

---

## 1. The container, solved

`[verified-numerically 2026-09-03]`

```
+0        u32   magic 0x2394ABCD
+4        u32   BE entry count            (0x00002D2D = 11565 for common.tangoresource)
+8        TOC   count × { u32 LE len, name, u32 LE len, name, u32 hash }   → ends 0x1902E8
0x1902F0  data  entries back-to-back, each a HEADERLESS RAW DEFLATE stream
0x2F77AB8 index 9001 × { u32 BE file offset, u32 BE csize, u32 BE usize, u32 BE id }
```

The missing piece was the **offset table**, and it was found by asking a question with a checkable
answer: if a table of data offsets exists, the first entry's offset is a value we already know, so
search the file for `0x1902F0` as a 32-bit big-endian integer. Exactly one hit, at `0x2F77AB8`.

What makes it a parse rather than a guess is **not** the chained
`offset + csize == next offset`, tempting though that is to quote: the walker stops the moment that
chain breaks, so it can only ever hold for the records the walker chose to accept. Two checks
independent of the walk do the work instead. The first record's offset is exactly `TOC_END + 8`. And
the **count 9001 is stored as a big-endian `u32` at `0x2E945C2`, precisely the byte where the last
record says the data ends** — inside a small metadata block that also records the size of the other
trailing table. 47,203,026 compressed bytes expand to 133,454,381.

**Not decoded:** the `id` field was never mapped to a TOC name. Entries were identified by *content*
(a DXBC signature) and later by *hash* — never by filename — so which entry is which `.shaderbin2`
remains unknown, and the 76 `.shaderbin2` names and the 2,785 DXBC-bearing entries have not been
reconciled. Nothing in this note depends on that mapping, but it is a real hole.

**Unclosed residual:** 249 of 9,001 entries (2.8%) fail to inflate. No shader was lost to this — the
hash match below accounts for all but one of the 168 — but the failures are real and unexplained,
not dismissed.

## 2. The payloads are DXBC, and their MVP rows are *named*

Extracting every entry that contains a DXBC signature gives **2,785 DXBC containers with RDEF
reflection intact**, 603 distinct constant-buffer layouts. `constantBufferV` — this project's own
name for the per-draw MVP buffer since §6 — appears in **1,208** of them, bound at **`cb0`**:

```
cbuffer constantBufferV
{
  float4 vertexxyzscale;      // Offset:   0
  float4 vertexxyzbias;       // Offset:  16
  float4 mvpmatrixx;          // Offset:  32
  float4 mvpmatrixy;          // Offset:  48
  float4 mvpmatrixz;          // Offset:  64
  float4 mvpmatrixw;          // Offset:  80
  float4 vertexstscalebias;   // Offset:  96
  float4 fogstart / fogend / fogscale;
}
```

The rows are **named with explicit byte offsets**. The entire premise of the blocked row — that the
runtime table records only a base offset, so rows 1–3 need bytecode nobody ever saved — is answered
by a file that has been sitting in the install folder the whole time.

## 3. The disk and the runtime agree, exactly

This is the part that makes it trustworthy rather than merely promising.

The proxy keys shaders by **FNV-1a 64 of the DXBC blob** (`shaderdump.c` — note it uses a
non-standard offset basis, `1469598103934665603`, which had to be reproduced exactly). Recomputing
that hash over the extracted shaders and matching against the 168 rows of `mvp_offsets.log`:

| | |
|---|---|
| runtime shaders found on disk | **167 / 168** |
| of the 34 scattered ones | **34 / 34** |
| reflected `(cb0 size, mvpx)` vs runtime-recorded `(cb0, mvpx)` | **167 agree, 0 disagree** |

`[verified-numerically 2026-09-03, n=167]`

Two entirely unrelated methods — live D3D11 reflection through the proxy during gameplay, and
offline extraction from a shipped archive — produce the same table. That is the "second independent
*use*" kind of corroboration, not a second identical read.

⚠️ **One of the 168 is not in `common.tangoresource`.** `common` is one of about twenty archives and
the level-specific ones were not searched, so this is *expected* rather than *explained*. It was not
chased.

## 4. "Scattered" is almost always one specific thing: z and w are swapped

`[verified-numerically 2026-09-03, n=34]`

Of the 34 scattered shaders, **33 have their rows at `+0, +16, +48, +32`** relative to `mvpmatrixx` —
that is, `mvpmatrixz` and `mvpmatrixw` are transposed in the buffer. Exactly one is different:
`E73523999ED27D3E` (cb0=80), whose rows sit at `0, 32, 48, 64`.

So the coverage gap is not 34 bespoke layouts. It is **one rule plus one exception**. The full
per-hash table is in `dev-archive/recon/2026-09-03-tangoresource-and-branch-merge/`.

Free extra, from the wider on-disk population: of all 1,208 `constantBufferV` shaders, 957 (79.2%)
are contiguous, 195 (16.1%) scattered, 56 carry no `mvpmatrix` rows at all.
⚠️ **Do not compare that 79.2% with the runtime table's 66.7%** — different populations entirely
(every shader shipped in one archive, versus the 168 that one gameplay session happened to create).

## 5. What this does and does not change

**Does:** the shader-dump launch is no longer needed, for any shader, ever — and the coverage
question can now be answered for shaders a session never happens to exercise, which a runtime dump
could never do. §12's predicted "86.9% with all 10 shapes" is reachable with no game running.

**Does not:** nothing here shows that *writing* the four rows at these offsets renders correctly.
Reflection gives the layout; only a run shows the patch behaves. That is the existing keystone /
runtime-proof row, unchanged and still `[FLAT]`.

### The diagnostic that would show this derivation is wrong

If a scattered shader is patched using these offsets and its geometry still renders at the wrong
orientation while contiguous geometry is correct, the row *mapping* is wrong rather than the patch —
and the check is cheap: the 33-shader rule predicts that writing z at `base+48` and w at `base+32`
fixes them, so deliberately writing them the *contiguous* way should reproduce the original breakage
exactly. If both orders look the same, the shaders were never the problem and the residual is the
`pool_miss` buffer-identity issue instead, which is a different section.

## 6. The method mistake worth keeping

The earlier pass concluded "the archive is not zlib-compressed" from a scan that looked for
zlib-framed streams. That test **could not have produced a positive result** — the data is headerless
raw deflate, which has no such signature. Today's second mistake was the same shape: a sweep for
deflate streams at fixed 128 KB intervals found nothing, because stream starts are not aligned; the
answer came from looking for a *known value* (`0x1902F0`) instead of a *pattern*.

Both times, the fix was to ask what a positive result would look like before trusting a negative one.

## Files

- `dev-archive/recon/2026-09-03-tangoresource-and-branch-merge/`
  - `tangoresource_toc.py`, `tangoresource_entries.py`, `tangoresource_extract.py` — the parser and
    extractor (our own code; reusable against any `.tangoresource`).
  - `2026-09-03-scattered-mvp-row-offsets.txt` — the 34 shaders resolved to explicit row offsets.
  - `2026-09-03-runtime-to-disk-hash-match.txt` — the 167/168 match.
  - `2026-09-03-static-mvp-shape-census.txt` — the 1,208-shader census.
- **No game content is committed.** The extracted shader bytecode (11 MB) stays in scratch; only
  names, hashes, sizes and byte offsets — interface metadata — are recorded here.
