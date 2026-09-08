# 2026-09-08 — `Unmap` was never hooked: the table was capped at four. The dynamic cb0 path now works, and a virtual pad drives this game

`/lm`, dev PC, fully autonomous. Two launches.

Evidence: `dev-archive/recon/2026-09-08-unmap-was-never-hooked-hook-table-capped-at-4/`
(both runs' full logs, and the same pause menu before and after).

Two independent blockers fell this session. Either alone would have been the write-up.

---

## 1. ⭐⭐⭐ The starred `[FLAT]` row is ANSWERED: `⇒ FIXED`, on every criterion the board set

The board's reading table said: *"maps>0, unmaps≈maps, no-map≈0 with `shadow writes`>0 ⇒ FIXED"*.

Same save, same scene, old build then patched `[verified-live 2026-09-08]`:

| counter | OLD build | PATCHED build |
| --- | --- | --- |
| maps seen | 4,340,705 | 1,803,322 |
| **unmaps seen** | **0** | **1,803,322 — exactly equal** |
| unmaps with no recorded map | 0 | 0 |
| shadow writes | 0 | **1,803,322** |
| **draws patched via the dynamic path** | **0** | **194,814** |
| map-table overflows | 4,339,927 | **0** |
| registered-but-unfed skips | 382,742 | 0 |

Shadow concurrency also reads clean: cross-thread writes 1,095,018, **CONCURRENT 0, torn 0** — the
single-writer precondition the code warns about holds on this machine.

### The cause was neither of the two the board was carrying

The 2026-09-05 entry left two candidates for `unmaps==0`: **(a)** deferred contexts use a different
vtable flavour and `Map`/`Unmap` are never late-hooked, or **(b)** on a deferred context `Unmap` can
legitimately be a no-op. Both were reasonable. Both are wrong.

**The proxy had been printing the real cause at startup, every run, and nobody had read it:**

```
mvp_patch: internal hooked-function table full (4); refusing to enable
           mvp_patch ID3D11DeviceContext::Unmap
mvp_patch: Map/Unmap hook failed - the per-shader DYNAMIC cb0 path is INERT this run
```

`mvp_patch.c` has **five** `hook_one()` call sites — `DrawIndexed`, `Draw`, `CreateBuffer`, `Map`,
`Unmap` — against `#define MVP_MAX_HOOKED_FUNCS 4`. The fifth is refused every time, and it is
always `Unmap`, because `Unmap` is hooked last. `Map` records a map-table entry, `Unmap` frees it,
and `Unmap` never existed — so `unmaps` stayed 0 and the table overflowed without bound.

**Raised to 8.** One line.

⭐ **This also retroactively vindicates the 2026-09-05 `(context,resource)` re-keying, which had
never once been exercised.** Overflows went 4,339,927 → **0**. The re-key was right; it had simply
never run. And the overflow symptom the 09-04d entry chased was never a table-sizing problem at all.

### Visual confirmation, and it is the cleanest possible A/B

Same save, same screen, only the build differs: **the pause menu rendered perfectly normally on the
old build, and is rotated into vertical slivers on the patched one** — because `TEST_YAW=90` now
reaches draws the dynamic cb0 path had never patched. That is exactly the board's *"the previously
unrotated fragments rotating"*, and it needed no interpretation.

The world itself renders black at `TEST_YAW=90` with both paths patched — checked that the window
was **visible, foreground and not minimised** before reading anything into it, because a minimised
window makes BitBlt return black and this build is known to auto-minimise on focus steal.

⚠️ **What this does NOT show:** that the rotation is *correct*, only that it now reaches the dynamic
path. Judging the covered/uncovered split needs `TEST_YAW=90` in a scene you can still see — which
is the standing "clean visible-rotation confirmation" row, now more useful than ever.

---

## 2. ⭐⭐ A VIRTUAL XInput pad drives this game, where `SendInput` cannot

`[verified-live 2026-09-08, n=2 launches, full menu→gameplay→exit]`

2026-09-07 established that **`SendInput` does not reach this game at all** — `Enter` would not even
dismiss the photosensitivity splash — and withdrew the earlier "verified" keyboard bindings as
having been a connected DualSense all along. The `[PD]` fix on the board was to port a
`GetDeviceState` injector into the winmm proxy: real work.

**It was not needed.** `EvilWithin.exe` imports `XINPUT1_3`, and a **ViGEm virtual pad** is that same
API with no code at all. It drove:

- the photosensitivity splash (`A`) — the exact screen `SendInput` could not pass;
- the attract screen (`A`);
- the title menu, selecting `CONTINUE` with the highlight verified first;
- the pause menu (`START`), and navigation within it;
- `TITLE MENU` → confirm `YES`, back to the title screen;
- the title menu → `EXIT` → confirm `YES` → **clean process exit, no taskkill**.

The game's own HUD switched its prompts to `(A) SELECT / (B) BACK` and raised a
*"Controller Connected — Xbox 360 controller"* toast, so it bound the virtual pad as a real one.

**It is also safer than the real pad.** The DualSense's stick drift walked the highlight from
`CONTINUE` onto `NEW GAME` on 2026-09-07 while only `Enter`s were sent. A virtual pad's sticks sit
dead-centre and drift is structurally impossible.

### ⚠️ The trap: the first input after each pad connect is SWALLOWED

Measured directly. Five `DPAD_DOWN` presses moved the pause-menu highlight **three** rows; two more
presses in a fresh session moved it **zero**. Inside a single pad lifetime, with a settle wait, each
press moved exactly one row — and the first one after connect moved none:

| step (one pad lifetime) | highlight |
| --- | --- |
| after 6 s settle | RESTART CHAPTER |
| +1 `DPAD_DOWN` | RESTART CHAPTER — **swallowed** |
| +2 `DPAD_DOWN` | OPTIONS |
| + left stick | TITLE MENU |

**This nearly restarted the chapter.** Following the profile's keyboard route — "Down ×5 to TITLE
MENU" — and committing blind would have landed `A` on **RESTART CHAPTER**. Only the
capture-and-verify rule caught it.

**The working pattern:** do a whole navigation inside ONE pad lifetime, and open each session with a
throwaway press that cannot move a vertical list (`DPAD_RIGHT`) to absorb the swallowed input.

---

## 3. Automation, scored

| # | capability | verdict |
| --- | --- | --- |
| 1 | menu → gameplay | ✅ **newly possible** — splash → attract → title → `CONTINUE` → Ch.1 loaded, entirely by virtual pad |
| 2 | commands | the proxy's channel is `tewvr.ini`, read at load; no console on this title |
| 3 | character + camera | not exercised — the run reached a cutscene and the counters were the objective |
| 4 | self-close | ✅ **clean menu quit on run 1**; ⚠️ **run 2 was force-terminated** — see below |

⚠️ **Run 2 ended with `Stop-Process`, and that is a deviation worth stating plainly.** With the fix
live, `TEST_YAW=90` rotates the pause menu into unreadable slivers, so no highlight can be verified
— and reaching `TITLE MENU` blind means passing `RESTART CHAPTER`, which the standing rules forbid.
Graceful exit was genuinely blocked *by our own patch*. Precedent: `doom-2016-vr` 2026-09-04, where a
modal dialog blocked the graceful path.

**A cheaper way out exists next time:** set `TEST_YAW = 0`, relaunch, and the menus are readable
again — the ini is read at start, so it cannot be done live.

---

## 4. What is NOT established

- **That the rotation is correct** — only that the dynamic path now reaches draws. The covered vs
  uncovered split still needs a scene that stays visible.
- Whether `draws patched via it = 194,814` is *all* the draws that path should serve, or a subset.
- Whether the home PC behaves the same — it has neither this fix nor the 2026-09-07 rebuild.
- Anything about the `GetDeviceState` injector. It is not disproved; it is **no longer needed for
  menu automation**, which was its entire stated purpose on the board.
- Whether a virtual pad can drive *gameplay* (movement, camera) on this game — only menus were
  exercised.
