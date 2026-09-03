# Vertex-shader MVP reflection table (copied here 2026-09-01, **actually committed 2026-09-03**)

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

## ⚠️ It was not actually in git until 2026-09-03 — the rescue silently failed

The 2026-09-01 copy landed in the working tree but **`dev-archive/.gitignore`'s blanket `*.log`
rule swallowed it**, so `git add` skipped it without complaint and the file stayed exactly what
the rescue was meant to stop it being: a single-disk file. It was found on 2026-09-03 while
clearing `D:\TheEvilWithinVR\captures\`, by checking `git ls-files --error-unmatch` rather than
trusting that the copy implied a commit. `.gitignore` now carries `!recon/**/*.log` so recon
evidence cannot be lost this way again `[verified-live 2026-09-03]`.

**The lesson generalises:** copying a file into a repo is not saving it. Only
`git ls-files --error-unmatch <path>` (or seeing it in a pushed commit) proves it is stored.

## The other two variants are redundant — measured, not assumed

`D:\TheEvilWithinVR\captures\` also held `task5-smoke/mvp_offsets.log` and
`task5-smoke/mvp_offsets-fixround.log`. Neither was kept, because both are strict **subsets** of
this file `[verified-numerically 2026-09-03, n=3 tables]`: 167 shaders each against this one's 168,
**zero** shaders present in a smoke table and absent here, and **zero** disagreements between the
two smoke tables on any shader they share. This file is the superset, so nothing was discarded.

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
