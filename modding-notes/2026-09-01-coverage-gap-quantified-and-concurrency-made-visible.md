# 2026-09-01 — The coverage gap is 10 layouts, not 34 problems; and the concurrency counter now prints

**Date:** 2026-09-01, dev machine. **Static analysis only — the game was never launched** (a
parallel session owns the machine's one "game may run" slot). One code change, built clean, not run.

Two of the dossier's §12 Open Risks moved. Neither needed a launch.

---

## 1. The coverage gap, quantified: 10 layout shapes, one of which is 44% of it

§12 records a *"coverage gap (~74–77%) … non-contiguous shader row layouts, plus an unexplained
`pool_miss` residual"* and calls it *"unexplained"*. Re-analysing the existing reflection table
(`captures/task5-gameplay/mvp_offsets.log`, 168 shaders) makes the shader half of it small and
concrete.

| | shaders | share |
|---|---|---|
| contiguous — patched today | 112 | 66.7% |
| non-contiguous, **no MVP found at all** (`mvpx = -1`) | 22 | 13.1% |
| non-contiguous, **MVP present but rows scattered** | 34 | 20.2% |

The 22 with no MVP are the already-understood group: the Domain-Shader/tessellated geometry from
§8 plus non-world shaders. They are not a gap to close in `mvp_patch`.

**The 34 that matter collapse into only 10 distinct `(cb0 size, mvp base offset)` shapes:**

| `cb0` | `mvpx` | shaders | `mvpx` in float4 rows |
|---|---|---|---|
| 128 | 64 | **15** | 4 |
| 272 | 144 | 5 | 9 |
| 144 | 64 | 3 | 4 |
| 192 | 96 | 3 | 6 |
| 240 | 96 | 2 | 6 |
| 304 | 144 | 2 | 9 |
| 80 | 0 | 1 | 0 |
| 208 | 96 | 1 | 6 |
| 176 | 64 | 1 | 4 |
| 224 | 96 | 1 | 6 |

Cumulative shader coverage if the shapes are handled in that order:

```
top 1  shape  -> 75.6%      top 5  -> 83.3%
top 2  shapes -> 78.6%      top 8  -> 85.7%
top 3  shapes -> 80.4%      top 10 -> 86.9%   (the 22 no-MVP shaders are the remainder)
```

**One shape — `cb0=128, mvpx=64` — is 15 of the 34, 44% of the whole scattered-row gap.** Handling
that one layout is the single highest-value piece of work available on this risk.

Two structural observations that constrain what "non-contiguous" can mean here:

* **`mvpx` is always a whole number of float4 rows** (0, 64, 96, 144 → rows 0, 4, 6, 9). Nothing is
  mid-row, so this is not a packing/alignment artefact.
* **`cb0 - mvpx >= 64` in every one of the 10 shapes**, so there is always room for four rows after
  the base. The rows are not running off the end of the buffer — they are genuinely **interleaved
  with other constants**.

### ⚠️ What this does NOT establish

* **These are shader counts, not draw counts.** The dossier's 74–77% is measured over *draws*,
  weighted by how often each shader actually runs. The 44% figure above is 44% *of the scattered-row
  shaders*, and the busiest shape by shader count need not be the busiest by draw count. Do not
  quote "handling one layout closes 44% of the gap" as a draw-coverage claim.
* **It says nothing about the `pool_miss` residual**, which is the other named half of §12's gap and
  is a buffer-identity problem, not a shader-layout one.
* **The actual row offsets within each shape are still unknown.** The reflection table records only
  a base and a contiguity flag, so where rows 1–3 sit is not recoverable from it. That needs the
  vertex-shader bytecode, and **no shader bytecode was ever saved** — `captures/` holds only logs,
  images and scripts. Getting it means one launch with the existing `TEWVR_DUMP` / shader-dump path,
  then `proxy-winmm/tools/dxbc_disasm.c` offline.

`[measured 2026-09-01 from existing capture data, n=168 shaders, one gameplay session]` — a single
session's shader population, so a different area of the game could shift the distribution.

### The concrete next step this produces

One launch, dumping vertex-shader bytecode for the 15 shaders of shape `cb0=128, mvpx=64`. Their
row offsets are then a static exercise, and the reflection table grows from
`(base, contiguous)` to four explicit row offsets — which is the change `mvp_patch` needs anyway to
handle any scattered layout at all.

## 2. The shadow-concurrency counter now prints (code change, built, not run)

§12: *"The counter that would reveal a violation currently only prints during a diagnostic window,
not continuously — needs a one-line visibility fix before this can be trusted as 'monitored' rather
than 'measured once.'"*

Confirmed exactly, and worse than the wording suggests. `g_diag_shadow_concurrent`,
`g_diag_shadow_torn` and `g_diag_shadow_multiwriter` were reachable **only** through
`mvp_diag_report_misses()`, which is called under `pool_miss > 0 && patched == 0` — i.e. **only while
patching is failing outright.** In the normal, working case the safety counters were printed
**never**, so a single-writer violation on different hardware would have been completely silent.

Fixed on branch `stereo-6dof-core` (commit "report shadow-concurrency counters continuously"): they
now print on their own whenever non-zero, cumulatively — cumulative on purpose, because the question
is "has this ever happened on this machine", not "how often in the last window" — with an explicit
warning line when the precondition has actually been violated. **Visibility only; no behaviour
change.** Builds clean with the vendored llvm-mingw toolchain. Not run.

## 3. Two housekeeping findings the user should know about

* **`stereo-6dof-core` is stranded on the pre-consolidation path layout.** The branch's files live
  at `proxy-winmm/…` while `main` moved everything to `mod/proxy-winmm/…` on 2026-08-30, so a plain
  merge would read as 110 files deleted and 110 added. The branch itself is safe — it survived the
  consolidation and is on GitHub — but merging it will need a path rewrite or a deliberate
  `-X` strategy, not a straight `git merge`. Worth deciding before the branch grows further.
* **`D:\TheEvilWithinVR\captures\` is still the only copy of this project's evidence.** 65 MB, not
  in git, flagged in `MACHINES.md` and still unresolved. The analysis in §1 above was only possible
  because that folder still exists; `mvp_offsets.log` is 168 lines and would cost a full capture
  session to regenerate. Given XIII turned out today to have lost its entire source tree the same
  way, this is worth acting on: `mvp_offsets.log` and the `seqdump` logs are small enough to commit
  to `dev-archive/`; the images and multi-megabyte dumps are the part that needs a decision.

🤖 Static analysis of existing capture logs plus one visibility-only code change; the game was not
launched and no game file was modified.
