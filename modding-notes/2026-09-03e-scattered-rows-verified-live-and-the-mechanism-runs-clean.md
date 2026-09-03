# 2026-09-03e (`/lm`, dev PC, live, fully autonomous) — the scattered-row patch is verified live, and the mechanism runs clean

First live run of the build `/pd` deployed at 13:58 today. User launched, said "all yours", left.
Claude drove the photosensitivity splash → title menu → gameplay, played, read the logs, and closed
the game through its own menus.

**All three acceptance criteria in the board's `[FLAT]` row passed.** One thing the row asked for is
*not* answered and is called out plainly in §4.

---

## 1. ✅ `shader_rows_incomplete = 0` — the scattered-row fix works in the running game

The `mvp_patch` DIAG line, ~5 s window, steady state:

```
patched=88421 | skipped: not_installed=0 no_vs=0 shader_unknown=0 shader_no_mvp=13829
shader_rows_incomplete=0 no_slot0_buf=0 pool_miss=24574 bounds_fail=0
scratch_not_ready=0 scratch_alloc_fail=0 scratch_map_fail=0
```

`[verified-live 2026-09-03]`

- **`shader_rows_incomplete = 0`**, where the old `shader_noncontig` counter was refusing **34
  shaders**. The offsets reflection always reported are now kept and used.
- **`bounds_fail = 0`** — the latent out-of-bounds write `/pd` found (the check tested only
  `offs[3]+16`, correct only while offsets ascend, while the dominant layout here is z/w transposed
  so `offs[2]` is highest) does not fire. Every row is now tested and none is rejected.
- Every other failure mode is zero: no scratch exhaustion, no unmapped buffers, no unknown shaders.
- **`CONCURRENT writes = 0`** across 3,315,308 cross-thread writes — the shadow-concurrency counter
  is live and reports the expected state, not silence.

## 2. ✅ The runtime `rows=` field matches the static table exactly

`mvp_offsets.log` gained the appended `rows=x,y,z,w` field. Compared against
`dev-archive/recon/2026-09-03-tangoresource-and-branch-merge/2026-09-03-scattered-mvp-row-offsets.txt`:

| | |
|---|---|
| runtime lines carrying `rows=` | **145** |
| runtime lines flagged non-contiguous | **33** |
| of those, present in the static scattered table | **33 / 33** |
| `rows=` **agree** | **33** |
| `rows=` **disagree** | **0** |
| contiguous-flagged lines whose rows are *not* `x,+16,+32,+48` | **0** |

`[verified-live 2026-09-03, n=33 scattered shaders]`

The static table holds 34; this scene drew 33 of them, so the missing one is scene coverage, not a
mismatch. **The offline harness's `agree=167 disagree=0` now has a live counterpart.**

## 3. Draw coverage this session, and what still limits it

`patched=88421`, `shader_no_mvp=13829`, `pool_miss=24574` → **69.7 % of all draws**, or **78.3 %
excluding the no-MVP group** (those have no matrix to patch, so they are not a miss)
`[measured 2026-09-03, one scene]`.

⚠️ **`pool_miss` is now the whole story.** It is 24,574 against 88,421 patched — far larger than
anything the row work addressed. It is a **buffer-identity** problem (a known/patchable shader's
bound slot-0 buffer is not in the direct-map pool), entirely separate from row layout, and the
scattered-row fix was never going to touch it. **That, not shader coverage, is the next ceiling.**

⚠️ Also: 69.7 % is a **draw** figure from one scene and is not comparable to the 86.9 % **shader**
figure from the static census. Different denominators; do not put them in the same sentence.

## 4. ⚠️ What this run did NOT prove — the visible camera override

The board row called this *"the human-witnessed runtime proof of the camera override"*. **It is not
that**, and the proxy says so itself in the log:

```
mvp_patch: TEWVR_TEST_YAW unset/0 -> K = identity
(read/patch/rebind mechanism still exercised every patchable draw; no visible change expected)
```

So this run proves the **mechanism** — read, patch, rebind, in bounds, on 88 k draws per five
seconds — and proves the **row offsets are right**. It does not prove the picture changes, because
the transform applied was the identity.

**`TEWVR_TEST_YAW` is read at process start**, so arming it cannot be done mid-session; it needs a
fresh launch. Prepared for that: `staging/the-evil-within-vr/tools/launch-yaw-test.bat`, which sets
the variable and launches (argument-tested; `off` or a numeric yaw both work).

**What the yaw run would actually add, and it is worth having:** with a 90° rotation the *newly
covered* 34 scattered shaders should rotate **with** the rest of the world. Anything that stays put
is geometry the patch still misses — a test the identity run structurally cannot perform. Three
baseline frames were captured this session for the comparison.

## 5. The game drove itself end to end

Automation, scored against the four capabilities:

| capability | state |
|---|---|
| menu → gameplay | ✅ splash → title menu → CONTINUE → gameplay |
| console / exec commands | untested here (env vars + log are the channel; no console needed) |
| move character + camera | ✅ WASD + mouse, a full movement pass to exercise shaders |
| close the game itself | ✅ pause → TITLE MENU → confirm → EXIT → confirm, process gone in ~2 s |

> ### ⚠️ Both TEW confirmation dialogs default to **NO**
> "Are you sure you want to quit?" highlights **NO**, not YES — in the pause-menu route *and* again
> on the title-menu EXIT. A blind `ENTER` cancels and looks like the keypress was ignored. Press
> **UP** first, then verify the highlight before committing. Two destructive-adjacency traps also
> exist: `NEW GAME` sits directly under `CONTINUE` on the title menu, and `RESTART CHAPTER` sits in
> the path to `TITLE MENU` on the pause menu.

## 6. What is NOT established

- **The visible camera override on this build** — §4. Needs one relaunch with the yaw armed.
- **Anything about VR or comfort.** Flat-screen only, no headset.
- **`pool_miss` is unexplained here.** This session measured it; it did not investigate it.
- **One scene, one chapter** (Ch.1, "An Emergency Call"). The 33-of-34 scattered coverage is what
  this scene drew, and a different scene would draw a different subset.
- The draw-coverage percentage is from a single ~5 s DIAG window in steady state, not an average
  over the session.
