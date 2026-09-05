# The DYNAMIC cb0 pairing was never a thread ring — it was a 32-entry table keyed on the wrong thing

`/pd`, home PC, 2026-09-05. **The game was not launched; nothing here was run.**

Closes the `[PD]` row from 2026-09-04d. Also **corrects that row's own diagnosis**, which is the
more important half.

---

## 1. The correction, first

The 2026-09-04d write-up recorded the cause as:

> The Map→Unmap→shadow pairing uses a **per-thread ring pool sized for 8 threads**. TEW's renderer
> maps and unmaps these DYNAMIC buffers from **more than 8 threads**, so the ring is exhausted…

**That is wrong**, and acting on it would not have fixed anything.

It came from reading the log line `thread-ring pool exhausted (8 distinct threads seen)` as if it
described the pairing. It does not. That line belongs to the **draw-time scratch-buffer rings** — a
different subsystem, documented under *"Threading: per-thread scratch-buffer rings"* about 1,550
lines earlier in `mvp_patch.c`, and its own message says the patch *stays correct* through that
condition via `WRITE_DISCARD` renaming. Two unrelated mechanisms were conflated because their
symptoms appeared in the same DIAG block.

The pairing that actually failed was this, and it is not a ring and not per-thread
`[verified-numerically 2026-09-05, read out of the shipped source]`:

```c
#define MVP_PENDING_MAPS 32
static struct PendingMap g_pending_map[MVP_PENDING_MAPS];   /* GLOBAL, flat */
```

claimed at `Map` by a CAS on the resource pointer, and searched at `Unmap` by a **linear scan for
that same resource pointer**.

**The lesson worth keeping:** the 09-04d session had a real measurement (`shadow writes=0`,
`overflows=2787733`) and a log line that *looked* like it explained the measurement. It did not
explain it; it merely co-occurred with it. Neither number was wrong — the reading was.

## 2. What was actually wrong

The file already said so, five hundred lines above the table, as a known risk recorded on
2026-09-04c:

> ⚠️ A KNOWN RISK, NOT DESIGNED AWAY. Two deferred contexts may legally Map the same
> `ID3D11Buffer` at the same time (`WRITE_DISCARD` renames per context) …

TEW records world draws from roughly **six deferred worker contexts plus the immediate context**
(the dossier's own §5 figure, and the same 7 the scratch pool is sized for). So at any instant the
*same buffer* can carry several in-flight maps, each with its own `pData`. Against a table keyed on
the buffer alone that produces two failures at once:

1. **Several entries share one key.** An `Unmap` scanning for the resource matches whichever came
   first — not necessarily the one this context created — so the wrong `pData` is shadowed and the
   wrong entry freed.
2. **The table saturates.** 54 registered buffers × 7 contexts is far past 32 slots, so it fills
   and never drains, and every subsequent `Map` is an overflow. That is the 2.7 million.

**The identity of an in-flight map is therefore not the buffer. It is the pair `(context,
resource)`** — unique by construction, because D3D11 permits one context only one outstanding map
of a given subresource. No thread count can exhaust that.

## 3. The fix

`g_pending_map` is replaced by an **open-addressed, linear-probed table hashed on both halves of
`(ctx, res)`**: 1024 slots, 24-probe budget, lock-free, O(1) expected.

Geometry and hash moved to `src/mvp_maptab.h` for one reason — so the test below exercises **the
shipped hash** rather than a copy of it. That is the Far Cry 2 lesson applied deliberately: there,
a Python transcription of `stereo.c` passed its check, and compiling the *real* `stereo.c` into a
harness was what caught two bugs.

The lock-free ordering is the part that has to be argued rather than tested here:

- **claim** — CAS `res` (the entry is now `{res=X, ctx=NULL}`), *then* write `ctx`/`data`/`slot`;
- **free** — clear `ctx` and `data` first, release `res` last;
- **match** — require **both** `res` and `ctx`.

A prober that catches a half-claimed entry sees `ctx == NULL`, matches nothing, and probes on. The
only `Unmap` that can match a given entry runs on the thread that made it, after `Map` returned, so
it always observes the fields fully written.

## 4. Verified without the game

`proxy-winmm/tools/map_pairing_test.c` — **17 checks, all passing**, clean at
`-Wall -Wextra -Wpedantic`. `[verified-numerically 2026-09-05]`

It synthesises the measured 09-04d workload (7 contexts × 54 buffers) with **realistically shaped
pointers** — 16-byte aligned, in two clustered arenas, sharing high bits — because a hash that only
looks strong on random inputs would fail exactly here.

| test | result |
| --- | --- |
| **the OLD design, replayed** | 32 placed, **346 overflowed** of 378 maps; one buffer holding **7 entries at once** |
| the new table, same workload | **378/378 placed**, 0 overflow, worst probe depth **6** of a 24 budget |
| one buffer, 7 contexts | **7 distinct buckets** |
| pairing exactness | 0 missing, 0 wrong payload, table drains to empty |
| 20,000 rounds of churn | 0 overflow, 0 unmatched, **0 leaked** |

The first row is the one that matters most: it reproduces the observed failure from the old code's
own logic, so the diagnosis in §2 is not merely a story that fits.

## 5. What is NOT fixed, and is not being claimed

**The shadow is still one window per buffer.** Two contexts writing the same buffer in the same
instant still cannot both be represented — that is the pre-existing known risk quoted in §2, and
`g_diag_shadow_concurrent` remains its readout. This change repairs the **pairing**, not the
**shadow**. What it does do is make that counter meaningful for the first time: until now no write
reached it at all, so a zero told you nothing.

Whether the fixed pairing actually patches the DYNAMIC draws is **unknown until a launch**. The
static work proves the table cannot overflow and pairs correctly; it cannot prove the hooks see the
traffic.

## 6. The diagnostic that decides the next step without another guessing round

A new DIAG line prints three cumulative counters:

```
mvp_patch: DIAG dynamic cb0 pairing: maps seen=N, unmaps seen=N, unmaps with no recorded map=N
```

| reading | meaning |
| --- | --- |
| maps > 0, unmaps ≈ maps, no-map ≈ 0, **shadow writes > 0** | **fixed** — proceed to judging the picture |
| maps > 0, **unmaps == 0** | our `Unmap` hook never sees these buffers. Deferred contexts use a different vtable flavour, and unlike `UpdateSubresource`, `Map`/`Unmap` are hooked **once** on the immediate context and never late-hooked. **A different fix, not a tuning knob.** |
| maps > 0, unmaps > 0, no-map == unmaps | the pairing key is still wrong; doubt the `(ctx,res)` assumption |
| overflows > 0 | 24 probes were not enough — far more concurrent maps than believed |

That table is the reason to read the log before touching this code again.

## 7. Build and deploy

- `mvp_patch.c` compiles with **zero warnings at `-Wall -Wextra`**; full proxy builds clean.
  `[compile-verified 2026-09-05]`
- **All four `winmm` exports `EvilWithin.exe` actually imports** — `timeGetTime`, `PlaySoundA`,
  `timeBeginPeriod`, `timeEndPeriod` — are present in the built proxy (180 exports total).
  `[verified-numerically 2026-09-05]`
- **Deployed on the HOME PC, which had nothing at all before this**: `TheEvilWithin\winmm.dll`
  (353,792 B, hash-verified against the build) and `tewvr.ini` with `TEST_YAW = 90`. Nothing was
  overwritten — there was no previous file to back up. To revert: delete both.

⚠️ **The build is from the `stereo-6dof-core` branch**, because that is where the live source is.
A build from `main` today is still a Task-4-era diagnostic proxy with no camera override in it.

## 8. The merge shape, re-measured

The standing `[USER]` row says its file counts are a floor. Re-measured today in a throwaway clone,
against the branch as it now stands: **21 added, 5 modified, 0 deleted**, exactly **one** conflict
(`README.md`, whose branch side is the obsolete pre-consolidation nav page — resolve `--ours`), and
**nothing left at the old `proxy-winmm/` path**. `[verified-numerically 2026-09-05, n=2 independent
measurements on different days and machines]` The count rose from 19 to 21 because this session
added two files; the shape is unchanged. Still **not merged** — that is the user's call.
