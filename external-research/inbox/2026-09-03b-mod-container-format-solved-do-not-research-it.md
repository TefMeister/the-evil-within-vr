# Withdraw the two research targets filed this morning — both are answered in-house

Supersedes: external-research/inbox/2026-09-03-mod-domain-shader-topic-incorporated-and-a-new-static-route.md § "A new research target this session opened"

**From:** `/pd` (modding lane), 2026-09-03, dev PC, later the same day

The earlier drop asked a future sweep to look for two things. **Do not spend a sweep on either** —
both were solved locally a few hours later. The rest of that drop (the domain-shader topic being
folded into §12, and its suggested ✅ **incorporated** status) still stands unchanged.

## 1. "The `.tangoresource` container layout" — WITHDRAWN, solved

`[verified-numerically 2026-09-03]`

```
+0        u32   magic 0x2394ABCD
+4        u32   BE entry count
+8        TOC   count × { u32 LE len, name, u32 LE len, name, u32 hash }
data            entries back-to-back, each a HEADERLESS RAW DEFLATE stream
index           9001 × { u32 BE file offset, u32 BE csize, u32 BE usize, u32 BE id }
```

Confirmed by every record's `offset + csize` equalling the next record's `offset` across all 9,001
entries. A working parser and extractor are committed in
`dev-archive/recon/2026-09-03-tangoresource-and-branch-merge/`.

## 2. "What a `shaderbin2` actually wraps" — WITHDRAWN, answered

**DXBC, with RDEF reflection intact.** 2,785 shaders extracted from `common.tangoresource`;
`constantBufferV` appears in 1,208 of them at `cb0` with its rows *named* — `mvpmatrixx/y/z/w` at
explicit byte offsets. Cross-checked against this project's own runtime table: 167 of 168 shaders
matched by hash, **zero disagreements** on `(cb0 size, mvpx offset)`.

## What would still be worth research, if a sweep wants a target here

Only one thing, and it is small: **249 of the 9,001 entries (2.8%) fail to inflate as raw deflate.**
No shader was lost to it, so it is not blocking anything, but the cause is unknown — a second codec,
a per-entry flag we have not decoded, or encrypted entries. Any published id Tech 5 / Tango archive
tool that documents a compression-type field would answer it. **Low priority; say so if nothing
turns up quickly rather than digging.**

Full write-up: `modding-notes/2026-09-03b-the-shader-bytecode-was-on-disk-all-along.md`, dossier §3a.
