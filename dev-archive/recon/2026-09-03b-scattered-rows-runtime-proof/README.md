# The scattered-row patch, verified in the running game (2026-09-03)

First live run of the build deployed at 13:58 on 2026-09-03 (the one that keeps reflection's row
offsets instead of discarding them, and tests every row against the buffer bound rather than only
`offs[3]+16`).

| file | what it is |
|---|---|
| `mvp_offsets-2026-09-03-scattered-rows-run.log` | 167 lines, one per shader, now carrying the appended `rows=x,y,z,w` field |
| `mvp_patch-diag-lines.log` | every `mvp_patch` DIAG / installed / TEWVR_TEST_YAW line from the session |

Both were written to `%LOCALAPPDATA%\TEWVR\`, which is outside every repository — hence this copy.

## What they show

**`shader_rows_incomplete = 0`** where the old `shader_noncontig` counter refused 34 shaders, and
**`bounds_fail = 0`**, so the latent out-of-bounds write does not fire. Steady-state DIAG line:

```
patched=88421 | skipped: shader_no_mvp=13829 shader_rows_incomplete=0
               no_slot0_buf=0 pool_miss=24574 bounds_fail=0
```

**The runtime `rows=` values match the static table exactly** — of 33 scattered shaders this scene
drew, 33 are in `../2026-09-03-tangoresource-and-branch-merge/2026-09-03-scattered-mvp-row-offsets.txt`
and **33 agree, 0 disagree**. No contiguous-flagged line had rows other than `x,+16,+32,+48`.

## ⚠️ What they do NOT show

`TEWVR_TEST_YAW` was unset, so **K = identity**: the read/patch/rebind path ran on every patchable
draw but nothing on screen changed. This is a proof of the MECHANISM and of the ROW OFFSETS, **not
of the visible camera override**. The proxy states this itself in the log.

`pool_miss = 24574` against `patched = 88421` is now the dominant limit on draw coverage. It is a
buffer-identity problem, unrelated to row layout, and untouched by this work.

Full analysis: [`modding-notes/2026-09-03e-scattered-rows-verified-live-and-the-mechanism-runs-clean.md`](../../../modding-notes/2026-09-03e-scattered-rows-verified-live-and-the-mechanism-runs-clean.md)
