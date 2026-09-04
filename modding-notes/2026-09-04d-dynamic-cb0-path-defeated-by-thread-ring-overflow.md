# 2026-09-04d (`/lm`, dev PC, FULLY AUTONOMOUS) — the DYNAMIC cb0 coverage build patches NOTHING: it is defeated by TEW's multi-threaded Map/Unmap

**One launch, the headline `[FLAT]` row answered — as a clean negative with a precise cause.** The
user launched from Steam; Claude drove splash → title → CONTINUE → Chapter 1, read the diagnostics,
and quit through the game's own two-stage menu to desktop. Build under test: `winmm.dll` 352,768 B
(the 2026-09-04c DYNAMIC cb0 build); `tewvr.ini` `TEST_YAW=90`. Evidence:
`dev-archive/recon/2026-09-04d-dynamic-cb0-path-defeated-by-thread-ring-overflow/`.

---

## 1. The result: zero DYNAMIC draws patched

The 2026-09-04c `/pd` build added a path to cover the per-shader DYNAMIC cb0 buffers (the ~25%
`pool_miss` gap, 42% of the shader table) by shadowing their `Map(WRITE_DISCARD)`/`Unmap` writes.
Live in Chapter 1 it does not work:

```
DIAG dynamic cb0 path: pool=54/64 buffers, shadow writes=0, draws patched via it=0,
     registered-but-unfed skips=159884, pending-map table overflows=2787733
```

`[verified-live 2026-09-04, n=1 launch]` — the buffers register (54 of 64 slots), but **not one
shadow write is captured and not one draw is patched through the path**, while the pending-map
table overflows into the millions.

## 2. The cause, named in the log

```
mvp_patch: thread-ring pool exhausted (8 distinct threads seen); further threads share the last
           bucket - patch stays correct via WRITE_DISCARD renaming, may rarely contend under [load]
```

The Map→Unmap→shadow pairing uses a **per-thread ring pool sized for 8 threads**. TEW's renderer
maps and unmaps these DYNAMIC buffers from **more than 8 threads**, so the ring is exhausted, the
pending-map table (which holds a Map awaiting its matching Unmap so the write can be shadowed)
overflows — past **2.7 million** and climbing — and the pairing never completes. No write is
shadowed, so no DYNAMIC draw is ever patched. The "may rarely contend" comment underestimated it:
under real load it does not rarely contend, it collapses.

## 3. What still works, and what this means

- **The DEFAULT-pool patch is unaffected**: 232,000–444,000 draws patched per 5 s, world cb0 pool
  holds its 6 buffers. So the original ~75% coverage (the shared DEFAULT world buffer via
  `UpdateSubresource`) is intact. The regression is confined to the new DYNAMIC extension.
- **The ~25% gap is still open.** Because 0 DYNAMIC draws are patched, the visible result at
  `TEST_YAW=90` is unchanged from 2026-09-04b — the same fragmented Chapter 1 scene, DEFAULT
  geometry rotated, DYNAMIC geometry (world meshes in the missed buckets) still not.

## 4. The fix (`[PD]`, no launch to design)

The mechanism is right (shadow the DYNAMIC `Map`/`Unmap`); its concurrency model is wrong. Either:
- **Size the pending-map / thread-ring pool to TEW's real thread count** (measure it — the log says
  "8 distinct threads seen" is already exceeded; instrument the true max), or
- **Re-key the Map→Unmap→shadow pairing on the mapped pointer or the buffer handle** rather than a
  fixed per-thread ring, so matching is O(1) by identity and cannot overflow under any thread count.

The second is the robust fix — a per-thread ring will always be a guess against an engine's thread
pool. Either way it is a build + one re-launch to re-test, with `tewvr.ini` `TEST_YAW=90` still the
right probe: success shows the previously-unrotated fragments now rotating and the `draws patched
via it` counter leaving zero.

## 5. Automation on TEW, scored (§5a)

1. **Menu → gameplay: proven** — splash → title → CONTINUE → Chapter 1 (~55 s load).
2. **Commands: proven (config + log channel)** — `tewvr.ini` armed the yaw; the DIAG lines are the
   readout.
3. **Character + camera: not exercised** — the diagnostics are read from the log, not from driving.
4. **Self-close: proven** — pause → TITLE MENU (YES) → title → EXIT (YES) → desktop, the two-stage
   route recorded in the profile. Second clean run of it.

## 6. What is NOT established

- Whether the fixed pairing will patch the DYNAMIC draws (needs the rebuild + re-launch).
- A clean enumerated visible rotation — still gated behind reaching a non-STEM-transition scene;
  moot until the DYNAMIC path patches, since today it would show only the unchanged DEFAULT split.

## 7. Gate

The DYNAMIC-cb0 `[FLAT]` row is answered (it patches nothing, and why). Next is `[PD]`: fix the
Map/Unmap pairing's concurrency (re-key on pointer/handle), then a `[FLAT]` re-launch. The
`stereo-6dof-core` merge remains a `[USER]` decision. Nothing needs the headset.
