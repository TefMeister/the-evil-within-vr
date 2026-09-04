# 2026-09-04b (`/lm`, dev PC, FULLY AUTONOMOUS) — config-file arming CONFIRMED, the yaw override is visibly active, and the pool_miss residual is NOT harmless: it carries real world geometry

**One launch, both open `[FLAT]` rows answered.** The user launched from Steam normally; Claude drove
title → CONTINUE → into Chapter 1, read the log, judged the 90° yaw by eye, read the bucketed
pool-miss table, and quit through the game's own menus to desktop. Windowed, letterboxed 2.35:1.
Evidence: `dev-archive/recon/2026-09-04-config-arm-and-yaw-visible-and-pool-miss/`.

---

## 1. Config-file arming — CONFIRMED, the three-launches-failed saga is closed `[verified-live 2026-09-04]`

The yaw test failed to arm three launches running (2026-09-03e/f/g) because the game was started
from Steam and every knob was read only from the environment. The 2026-09-04 `/pd` change made the
proxy read `tewvr.ini`. This launch, started from Steam, logged:

```
config: read D:\...\TheEvilWithin\tewvr.ini: 1 setting(s) taken
config: TEWVR_TEST_YAW=90 (from D:\...\tewvr.ini)
mvp_patch: TEWVR_TEST_YAW=90.000 -> test rotation K ACTIVE (every patchable draw rotated)
```

So a Steam launch now arms the test with no special launcher. That mechanism is proven.

## 2. The pool_miss residual carries real geometry — the row does NOT close as "harmless" `[verified-live 2026-09-04, n=1]`

The bucketed miss table (new 2026-09-04, prints while patching works) settles the dossier's
open question — "do the missed draws carry world geometry?" — as **yes**.

Per ~5 s census: **patched ≈ 590,000** draws; **skipped ≈ 13,600, all `shader_no_mvp`** (the only
non-zero skip reason — `not_installed`, `no_vs`, `shader_unknown`, `rows_incomplete`, `no_slot0` are
all 0). So the shaders the proxy *classifies* as no-MVP are correctly skipped.

But the `pool_miss` breakdown (buffers seen bound at cb0 that were not the tracked world pool) shows
MVP-bearing geometry going unpatched:

| bucket | ByteWidth | Usage | notable draws |
| --- | --- | --- | --- |
| combo[1] | 1920 | 0 (DEFAULT) | 2 misses — inside the known 1856–1984 B world-pool range, expected |
| combo[5] | 160 | 2 (DYNAMIC) | `geom(count>=300)=750–1016`, `max_count=39102` |
| combo[7] | 224 | 2 | `geom=1001`, `max_count=39102` |
| combo[3] | 272 | 2 | `non-indexed=109`, `max_count=120000` |
| combo[4] | 304 | 2 | `geom=54 non-indexed=54`, `max_count=75000` |
| combo[0,2,6,9,…] | 64–240 | 2 | mostly `tiny`/`max_count≈36` — small, plausibly harmless |

The large-geometry DYNAMIC buckets (5, 7, 3, 4) are the finding: these are **MVP-bearing draws whose
cb0 is a per-shader `DYNAMIC` buffer the pool-based patch never intercepts** — this is the known
~74–77% coverage gap (dossier §6/§11), not a no-MVP case (skinning is pre-MVP, so skinned meshes
still go through an MVP). The only `DEFAULT` bucket is the expected 1920 B one.

## 3. Visible 90° yaw — active, and the residual shows `[verified-live 2026-09-04, n=1]`

With `TEST_YAW=90`, Chapter 1 "An Emergency Call" (confirmed by the pause header) rendered as a
**radically transformed scene** — its rainy street opening became an unrecognisable stormy sea/sky —
with **unrotated fragments and an upright, detached character head** floating in it. Read together
with §2: the ~75% of geometry in the tracked DEFAULT pool rotated, and the ~25% in the missed
per-shader DYNAMIC cb0 buffers rendered unrotated, which is what the detached/fragmented look is.

⚠️ **Honest limit:** the Chapter 1 opening is dark, rainy and heavily post-processed, so I could not
cleanly enumerate *which* elements rotated vs. not — the scene was too distorted. The claim rests on
the log (pool_miss carries geometry) plus the gross visible transformation, not on a tidy
side-by-side. A static, well-lit scene with a clear horizon and a standing character would show it
cleanly and is a cheap future confirmation.

## 4. What this means for the mod

The MVP-patch approach works and rotates the bulk of the world, but a stereo build **cannot ship at
~75% coverage** — a quarter of world geometry would sit at the wrong depth/orientation. The next
technical front is extending coverage to the **per-shader DYNAMIC cb0 path** (the pool_miss buckets):
identify how those draws bind their MVP and intercept them the way the DEFAULT pool is intercepted.
That is `[PD]` recon + build, no launch needed to start.

## 5. Automation on TEW, scored (§5a)

1. **Menu → gameplay: proven** — Enter (splash) → title (PRESS ANY KEY) → title menu (CONTINUE) →
   ~55 s load → Chapter 1.
2. **Commands: proven (the config/log channel)** — `tewvr.ini` + `%LOCALAPPDATA%\TEWVR\tewvr.log`;
   the log named the config source and the pool-miss table.
3. **Character + camera: partial** — mouse-look moved the view (delta jumped), but the scene was too
   distorted to drive meaningfully; character movement not exercised.
4. **Self-close: proven** — pause → TITLE MENU (confirm YES) → title screen → EXIT (confirm YES) →
   desktop, all through the game's own menus. Process gone.

## 6. What is NOT established

- A clean, enumerated visible rotation (which elements rotate vs not) — needs a static well-lit scene.
- How the per-shader DYNAMIC cb0 draws bind their MVP (the coverage-gap fix).
- Character-movement drive on this game.

## 7. Gate

Both `[FLAT]` rows are answered. New `[PD]`: extend MVP coverage to the per-shader DYNAMIC cb0
(pool_miss) path. The `stereo-6dof-core` merge remains a `[USER]` decision. A clean static-scene
rotation shot is a cheap optional `[FLAT]`. Nothing needs the headset yet.
