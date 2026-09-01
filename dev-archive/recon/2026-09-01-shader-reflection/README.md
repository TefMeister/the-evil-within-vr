# Vertex-shader MVP reflection table (rescued into git 2026-09-01)

`mvp_offsets.log` — 168 vertex shaders seen in one gameplay session, one line each:

```
hash=<vs hash> cb0=<cb0 size in bytes> mvpx=<byte offset of the MVP base, -1 = none found> contiguous=<0|1>
```

Produced by the `TEWVR` reflection pass during the Task 5 work. **This file lived only in
`D:\TheEvilWithinVR\captures\task5-gameplay\` — outside git, on one disk.** It is copied here
because the coverage-gap analysis in
`modding-notes/2026-09-01-coverage-gap-quantified-and-concurrency-made-visible.md` depends entirely
on it, and regenerating it costs a full capture session. The same day, XIII turned out to have lost
its whole source tree to exactly this pattern.

It is data this project generated, not game content, so committing it is within the
never-upload-game-files rule.

## What it shows

112 of 168 shaders (66.7%) carry the MVP as four contiguous float4 rows and are patched today.
22 carry no MVP at all (the Domain-Shader/tessellated group of dossier §8). The remaining 34 carry
an MVP with scattered rows, and those collapse into just **10 distinct `(cb0, mvpx)` shapes** — one
of which, `cb0=128 mvpx=64`, accounts for 15 of the 34.

Full analysis, caveats and the shader-count-vs-draw-count distinction: see the notes file above.

## Known limitation

The table records only a base offset and a contiguity flag, so **where rows 1–3 sit is not
recoverable from it.** That needs the vertex-shader bytecode, which was never saved. One launch with
the shader-dump path, then `proxy-winmm/tools/dxbc_disasm.c` offline, closes it.
