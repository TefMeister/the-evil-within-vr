# 2026-09-04c (`/pd`, dev PC, static only) — the per-shader DYNAMIC cb0 path is built, and it addresses 42% of the live shader table

**The game was not launched, and nothing here has been run.** The board's `[PD]` row is closed with
built, deployed, compile-verified code. What it does to the picture is the next launch's question.

---

## 1. What the row asked, and what the recon says

2026-09-04b ran the bucketing added that morning and answered the standing question: the missed
draws are **not** harmless. `combo[5]` (ByteWidth 160, indexed geometry 750–1016), `combo[7]` (224,
geom 1001), `combo[3]` (272, non-indexed up to 120,000 vertices), `combo[4]` (304, up to 75,000) —
all `Usage=2`, DYNAMIC `[verified-live 2026-09-04]`. Those are world meshes rendering at the wrong
per-eye orientation.

The recon half, from files already on disk `[measured 2026-09-04, n=167 shaders]`:

| | |
| --- | --- |
| shaders in the accumulated runtime table | 167 |
| no `mvpmatrix` at all | 22 |
| **MVP-bearing** | **145** |
| **declaring `cb0` at one of the four missed sizes** | **61 — 42.1% of the MVP-bearing set** |
| of those, scattered z/w layout | 8 (already handled since 2026-09-03) |

Breakdown: `cb0=224` 33 shaders, `cb0=160` 18, `cb0=272` 8, `cb0=304` 2. **Every one already has a
complete reflected four-row set recorded**, so they are patchable the instant the buffer becomes
reachable — the shaders were never the obstacle.

⚠️ **These are SHADER counts, not DRAW counts.** The ~74–77% draw-coverage figure in the dossier is
a different population and the two must not be mixed. What this bounds is how much of the shader
table the new path can address, and it corroborates the size of the gap from an independent
direction. Analysis and the rescued table: `dev-archive/recon/2026-09-04c-dynamic-cb0-coverage/`.

## 2. Why these draws were out of reach, and the fix

The existing pool tracks only the large shared **DEFAULT** world buffer, because `UpdateSubresource`
is the only CPU write path a `DEFAULT` buffer has, and that is what feeds its shadow. The
registration filter says so in as many words: *"A DYNAMIC buffer would never receive a shadow write
and would only waste a slot."* Correct — and it is why a quarter of the draws were never candidates.

A DYNAMIC buffer is written through `Map(WRITE_DISCARD)`/`Unmap`. **So the fix is one more shadow
source, not a new patch mechanism:** hook `Map`/`Unmap`, copy the mapped contents into a shadow at
`Unmap` while the pointer is still valid, and the existing draw-time path — look up the shadow,
bounds-check the rows, multiply by `K`, write a scratch buffer, rebind slot 0 — then works unchanged.

**The slots are a partition of the same arrays, not a second pool.** `[0, 32)` stays the DEFAULT
pool; `[32, 96)` is the new one. That way every seqlock, validity and tearing guarantee already
written and reviewed applies to the new path without being duplicated, and the hot DEFAULT lookup
does not get slower because each `find` scans only its own range.

Registration follows the module's own existing rule — **offered from the draw path, never from the
descriptor alone**, because a descriptor cannot tell a world `cb0` from any other buffer of the same
shape, but "bound at VS slot 0 for a draw whose shader has known mvp rows" can. A newly seen buffer
is therefore unpatched for its first draw and patched from the next `Map`/`Unmap` on; that is
self-correcting within a frame or two.

Vtable slots 14 (`Map`) and 15 (`Unmap`) were **read from the SDK header's own
`ID3D11DeviceContextVtbl`**, not assumed, the same way 12/13/48 already were.

## 3. What is verified, and what is guessed

`[compile-verified 2026-09-04]` — builds clean at `-Wall -Wextra`.

**Two compile-time assertions, both proved to fire** by deliberately breaking the values and
confirming the named assertion is what fails `[verified-numerically 2026-09-04, n=2 negative
controls]`:

- a dynamic buffer wider than the shadow window would be truncated at `Unmap` and then patched from
  a partial copy;
- if the two pool sizes and the array size ever drift apart, a dynamic slot index would run off the
  end of every shadow array at once.

**One real bug caught in my own code before it shipped:** the pending-map table originally claimed
its slot with the 32-bit `InterlockedCompareExchange` on a resource **pointer**. This is a 64-bit
process, so that truncates the pointer and would match the wrong buffer at `Unmap` — patching one
mesh's matrix from another's constants. Now `InterlockedCompareExchangePointer`.

**⚠️ The pool size is a guess.** 64 slots, because the live evidence names four distinct
(size, usage) combos but says nothing about how many distinct **buffers** back them — an engine may
use a handful of dynamic ring buffers or one per material. It logs when it fills, exactly as the
DEFAULT pool does, so the first run says whether 64 was right.

**⚠️ A known risk, not designed away.** Two deferred contexts may legally `Map` the same buffer at
once (`WRITE_DISCARD` renames per context), and a shadow keyed by buffer pointer cannot represent
both. The existing seqlock detects that as a concurrent write and the draw falls through unpatched —
fail-safe rather than wrong — and `g_diag_shadow_concurrent` counts it. **If that counter moves,
this path needs a per-context shadow, not a per-buffer one**, and that is a redesign rather than a
tweak.

**NOT established:** that any additional geometry is patched. The mechanism is built and the shaders
are known-patchable; only a run shows whether the shadow arrives with the right contents at the
right time.

**Deployed** to `TheEvilWithin\winmm.dll` (352,768 B); the previous build is kept as
`winmm.dll.bak-2026-09-04c-pre-dynpool` and one copy reverts. `tewvr.ini` still carries
`TEST_YAW = 90`, which is exactly what this change wants tested — see below.

## 4. What the next launch answers

The yaw test is already armed and confirmed working, so this needs **one ordinary Steam launch**.
Play a minute somewhere with visible world geometry, quit, read `%LOCALAPPDATA%\TEWVR\tewvr.log`.

| line | meaning |
| --- | --- |
| `registered DYNAMIC cb0 buffer #N (…)` appearing a few times early | the draw-path registration works; N+1 tells you how many distinct buffers there really are |
| `DIAG dynamic cb0 path: … draws patched via it=` **climbing** | **the point of the change** — those are draws that used to land in `pool_miss` |
| `pool_miss` **falling** by roughly the same amount | corroboration from the other side; if `dyn_patched` climbs but `pool_miss` does not fall, the two are counting different draws and the accounting is wrong |
| `registered-but-unfed skips=` staying non-zero | registration happens but the writes are not seen — the game fills those buffers some other way, and `Map`/`Unmap` is the wrong source |
| `DYNAMIC cb0 pool full (64 slots)` | the guess was too small; raise it and rebuild |
| `CONCURRENT writers on world cb0 shadow slot` | the deferred-context risk above is real here; this path needs per-context shadows before its output can be trusted |
| **on screen**, with the 90° yaw | **more of the world should rotate than before.** The comparison is against the three identity baseline frames and the 2026-09-04b observation of what stayed put. Geometry that still refuses to rotate is what remains uncovered |

⚠️ If the game crashes or hangs on this build, the suspect is the new `Map`/`Unmap` hook — revert to
`winmm.dll.bak-2026-09-04c-pre-dynpool`, which is the confirmed-working config-file build from this
morning, and the game returns to its previous coverage rather than to a broken state.
