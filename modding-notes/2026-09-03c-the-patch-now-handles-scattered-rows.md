# The patch now handles scattered rows — and the old bounds check would have written past the buffer

**2026-09-03, dev PC, `/pd` (parallel development), third pass.**
**The game was not launched. Nothing here has been run against The Evil Within.**
Work done on a `git worktree` of `stereo-6dof-core`; nothing was merged, and the working clone at
`D:\TheEvilWithinVR\` was not touched.

The morning's passes established that "scattered" MVP rows are, in 33 of 34 cases, simply
`mvpmatrixz` and `mvpmatrixw` transposed. This pass makes the patch use that — and the interesting
part turned out not to be the feature.

---

## 1. The offsets were never missing. They were being discarded.

`§12` described the coverage gap as blocked on bytecode nobody had saved. Reading the code shows
something narrower and more annoying: `mvptable_on_shader_created()` **already** called
`GetVariableByName` for `mvpmatrixy`, `z` and `w` and read each one's `StartOffset` — and then
compared them against `x+16/+32/+48`, kept a single boolean, and threw all three offsets away.

```c
contiguous = ok;      /* ...and vd.StartOffset for y, z, w is gone */
```

`mvp_row_offsets_for_shader()` then had nothing to hand out but `{x, x+16, x+32, x+48}`, so it
refused every non-contiguous shader rather than guess. That was the right call given what was
stored, and the wrong thing to store.

**The change:** `MvpEntry` keeps `rows[4]` — the actual reflected offsets — plus a `rows_valid`
flag. `mvp_row_offsets_for_shader()` returns them whenever all four rows were found, contiguous or
not. Contiguity survives as a diagnostic only: it is still written to `mvp_offsets.log`, whose lines
gain a `rows=x,y,z,w` field, appended so that anything parsing the old fields keeps working.
`MVP_SHADER_NONCONTIGUOUS` becomes `MVP_SHADER_ROWS_INCOMPLETE`, which is what the remaining refusal
now means.

## 2. ⚠️ The bounds check was a latent out-of-bounds write

This is the part worth reading. `mvp_patch.c` guarded the write with:

```c
if (offs[0] < 0 || (UINT)(offs[3] + 16) > byte_width || ...)
```

Correct — while row offsets were guaranteed contiguous and therefore ascending, so `offs[3]` was
necessarily the highest. **The dominant non-contiguous layout in this game is exactly the one that
breaks that assumption:** `{b, b+16, b+48, b+32}`, where `offs[2]` is the highest and `offs[3]` is
*lower* than it. Handing those offsets to the existing check would have let `mvpmatrixz` be written
16 bytes past the end of the bound constant buffer **while the check reported success** — precisely
the silent out-of-bounds write that check exists to prevent.

It now tests every row, and reports all four offsets plus the furthest end byte when it refuses.

This is a good argument for the project's own standing rule about checking interactions: the feature
was three files of straightforward bookkeeping, and the only genuine hazard in it was in code nobody
was changing.

## 3. Verified against the game's own shaders, with no game running

`proxy-winmm/tools/reflect_rows_test.c` links the **shipped** `mvptable.c` and calls
`mvptable_reflect_rows()` — the function the runtime actually uses, not a transcription — over every
DXBC shader extracted from `base/common.tangoresource`, comparing each result against an independent
from-scratch RDEF parser (`dxbc-reflect.py`). Two unrelated reflectors, same bytes.

```
shaders reflected      : 2785
  no cb0 found         : 386
  all four rows found  : 1192  (contiguous 997, scattered 195)
  disagreements with the independent RDEF parser: 0

patchable under the OLD contiguous-only rule : 997
patchable under the NEW rule                 : 1192  (+195)
```

`[verified-numerically 2026-09-03, n=2785]` · the three changed files compile clean at
`-Wall -Wextra`, and the full DLL builds through `build.ps1` with no errors
`[compile-verified 2026-09-03]`

Against the 168 shaders a real gameplay session produced, this takes patchable shader coverage from
**112 to 146 of 168 — 66.7% → 86.9%**, the figure §12 predicted for handling all ten shapes.

⚠️ The 1,192 here is not the 1,208 of the morning's census. The test uses the proxy's own
`reflect_find_cb0()` rule — `constantBufferV` if present, else whatever binds `b0` — while the
census simply counted buffers named `constantBufferV`. The test's number is the one that describes
runtime behaviour.

## 4. What is NOT established

- **That patching at these offsets renders correctly.** Reflection gives the layout; only a run
  shows the patch behaves. Unchanged, and still the keystone `[FLAT]` row.
- **That coverage actually reaches 86.9% in a frame.** That is a *shader* count. Draw coverage is a
  different number, and the `pool_miss` residual — a buffer-identity problem, not a shader-layout one
  — is untouched by any of this.
- The one shader of the 168 that is not in `common.tangoresource` is still unaccounted for.

### The diagnostic that would show this is wrong rather than merely untuned

Next run, `mvp_offsets.log` gains a `rows=` field on every line. If a scattered shader's `rows=`
values disagree with the static table in
`dev-archive/recon/2026-09-03-tangoresource-and-branch-merge/2026-09-03-scattered-mvp-row-offsets.txt`,
then the runtime is reflecting a *different shader* than the one the archive holds under that hash —
which would mean the hash match, not the offsets, was wrong. If they agree and geometry is still
mis-oriented, the row *mapping* is wrong rather than the plumbing, and writing the rows in
contiguous order should reproduce the original breakage exactly.

Also worth watching: `shader_rows_incomplete` in the skip-reason line should now be near zero where
`shader_noncontig` used to carry the non-contiguous shaders, and `bounds_fail` should stay at zero.
A non-zero `bounds_fail` after this change means a shader whose rows genuinely do not fit its bound
buffer — worth seeing the log line, which now prints all four offsets.

## 4a. A merge trap the harness nearly introduced

The test first went into a new `proxy-winmm/test/` directory. Re-running the merge measurement from
this morning showed **2 files left at the old path** — git's directory-rename detection maps
`proxy-winmm/src/` and `proxy-winmm/tools/` onto their `mod/` counterparts because those directories
contain renamed files, but a **brand-new** directory has no counterpart to map from, so `test/` would
have landed outside `mod/` and the merge would no longer have been a single command.

Moved under the already-mapped `tools/` (which also holds `dxbc_disasm.c`), re-measured: **0 left at
the old path**. `build.ps1` globs `src/*.c` only, so nothing in `tools/` is ever linked into the DLL.

Worth keeping as a rule: while that branch is unmerged, **new files belong in directories that
already exist on both sides**, or the merge stops being free.

## Deployed

`TheEvilWithin\winmm.dll` carries the new build; the previous one is kept beside it as
`winmm.dll.bak-2026-09-03-pre-scattered-rows`, so one file copy reverts it.

## Files

- Branch `stereo-6dof-core`, tip `219d024` — `03c48ce` is the change itself, followed by the
  harness move and a log-wording fix — `mvptable.{c,h}`, `mvp_patch.c`, `tools/reflect_rows_test.c`,
  `tools/README-reflect-rows-test.md`.
  ⚠️ Still not merged into `main`; that remains a `[USER]` decision.
- `dev-archive/recon/2026-09-03-tangoresource-and-branch-merge/2026-09-03-reflect-rows-test-run.txt`
  and `make_expectations.py`.
