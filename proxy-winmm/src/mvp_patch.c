#include "mvp_patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MinHook.h"
#include "minhook_glue.h"
#include "log.h"
#include "config.h"
#include "mvptable.h"
#include "shaderdump.h"
#include "camera.h"

/*
 * ---- Step 0: chosen read mechanism, and why (REVISED after review) ----
 *
 * The brief laid out three candidates for reading a draw's bound VS-slot0
 * cb0 content without a per-draw GPU stall or a deferred-context/worker-
 * thread threading violation: (a) capture the pool buffers' CPU-writable
 * pointer via a fresh Map, if one ever fires after our hooks are live; (b)
 * hook ID3D11Device::CreateBuffer and Map the pool buffers ourselves at
 * creation time, mirroring the engine's own persistent-map pattern; (c) a
 * batched once-per-frame staging-copy snapshot on the immediate context.
 *
 * This implementer FIRST chose (c) - reasoning that (a)/(b) both hinge on
 * live, gameplay-only behaviour this task's constraints (no driving/
 * watching a real play session) could not verify. That reasoning about
 * verifiability was sound, but (c) itself was WRONG: Task 5's discovery
 * found ~1900 draws/frame sharing ~6 deferred pool buffer identities (each
 * 96-224B of actual shader-visible content within a larger physical
 * buffer) - roughly 300 draws per identity per frame. A single
 * once-per-frame snapshot per identity means all ~300 of those draws would
 * have been patched with the SAME stale content - not "one frame of
 * staleness" (which correctly describes a per-OBJECT value lagging by one
 * frame) but "one value substituted for ~300 different objects' actual
 * per-draw data," corrupting every OTHER per-object constant living in
 * that same cb0 window too (material params etc., not just MVP). Caught in
 * review before any live test masked it as "the world didn't rotate."
 *
 * ============================================================
 * FIX ROUND 4 (2026-08-21): THE CHOICE BELOW IS SUPERSEDED.
 *
 * Option (b) - Map the world cb0 buffer once at creation and keep the CPU
 * pointer forever - is IMPOSSIBLE for the real target buffers. Measured
 * during a visually-confirmed gameplay session, the buffers actually bound
 * at VS slot 0 for world draws are:
 *
 *     ByteWidth=1920  Usage=D3D11_USAGE_DEFAULT  CPUAccessFlags=0
 *
 * A DEFAULT / no-CPU-access resource cannot be Mapped at all. The 1920-byte
 * DYNAMIC/CPU_WRITE buffers the filter described below was successfully
 * capturing are a same-size DECOY set, never bound at slot 0 for a world
 * draw (their pointers are disjoint from the five 1920-byte slot-0 pointers
 * in task6-gameplay-seqdump.log).
 *
 * CURRENT MECHANISM - call it option (d), UpdateSubresource shadowing:
 * UpdateSubresource is the only CPU write path for a DEFAULT buffer, so
 * that call is where the live MVP content is observable.
 * Hook_UpdateSubresource copies the caller's source bytes into our own
 * per-buffer CPU shadow before calling through, and the draw path reads the
 * shadow exactly where it used to read the mapped pointer. Everything
 * downstream (row extraction, K*mvp, scratch buffer, rebind) is unchanged.
 * Buffers are registered from the DRAW path, not from CreateBuffer, because
 * a descriptor alone cannot distinguish the world pool from same-shaped
 * decoys. See the two large fix-round-4 blocks further down this file for
 * the full evidence, and Fix round 4 in task-6-report.md.
 *
 * Also retired by this change: the "the engine's own later Map could
 * conflict with ours" residual risk. This module no longer Maps the
 * engine's buffers at all.
 *
 * The historical reasoning below is retained because it explains why the
 * earlier mechanisms were chosen and how each was disproved.
 * ============================================================
 *
 * SUPERSEDED CHOICE: (b). A CreateBuffer hook (VTBL_DEV_CREATEBUFFER=3, cross-
 * checked against shaderdump.c's own already-verified ID3D11Device vtable
 * count) filters for buffers matching the world-pool's known profile
 * (D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE,
 * ByteWidth in [MVP_POOL_BUF_MIN_BYTES, MVP_POOL_BUF_MAX_BYTES] - wide
 * enough to catch the ~1920B pool buffers observed in Task 6 discovery,
 * narrow enough to exclude the small 96-224B per-shader cb0 buffers the
 * engine explicitly Maps/Unmaps every draw, which this mechanism must NOT
 * touch - see below). On a match, mvp_direct_pool_try_capture() gets the
 * buffer's own device's immediate context (via
 * ID3D11Device::GetImmediateContext - available directly from the `dev`
 * parameter Hook_CreateBuffer already has, with no dependency on this
 * module having captured the game's real device via Present first: our
 * hook fires synchronously, inline with the CreateBuffer call itself,
 * strictly before the engine's own code can do anything else with the
 * freshly-created buffer - including its own Map, if it ever tries one),
 * issues our OWN Map(WRITE_DISCARD) immediately, and - on success - AddRefs
 * the buffer for a persistent reference and keeps the returned CPU pointer
 * forever (never Unmaps), mirroring the engine's own apparent pattern
 * (Task 6 discovery: these buffers are never observed being explicitly
 * Mapped/Unmapped by the engine at all - CPU content changes with no
 * Map/Unmap/UpdateSubresource event ever appearing in a full gameplay
 * capture, meaning whatever "map" IT establishes almost certainly also
 * happens once, near creation).
 *
 * This gives every patchable draw an EXACT, LIVE read of its actual
 * per-object cb0 content - no staleness, no shared-snapshot corruption,
 * and no per-draw or per-frame GPU work at all (the CPU pointer is plain
 * process memory once captured).
 *
 * Residual risk this implementer could NOT close by static reasoning alone
 * (explicitly flagged by the brief for option (b) - "verify the engine
 * tolerates a foreign Map... test carefully"): IF the engine's own
 * persistent-pointer setup for one of these buffers involves ITS OWN Map
 * call arriving AFTER ours (rather than obtaining its pointer some other
 * way - e.g. already having captured it before CreateBuffer even returns
 * to its caller, which our hook cannot precede), our foreign,
 * never-Unmapped Map would make that later engine Map call fail
 * (D3D11 forbids re-Map on an already-mapped subresource) - and if the
 * engine's own code does not check that HRESULT, it could write through
 * garbage/stale mapped-subresource state instead of the real buffer,
 * corrupting or crashing. Task 6 discovery's evidence (never observing an
 * explicit engine Map on these specific buffers, across a full gameplay
 * capture with our Map/Unmap hooks already live) argues AGAINST the
 * engine ever calling Map on them at all after creation, which would make
 * this risk moot - but that evidence predates this mechanism actually
 * mapping them first, so it is not a full proof. mvp_direct_pool_try_capture()
 * logs loudly (STEP0 line) on every successful capture and on every
 * foreign-Map failure, and mvp_patch_prepare() falls through to unpatched,
 * never crashes or corrupts state, for any buffer this mechanism did not
 * successfully capture - so a bounded menu smoke test can directly observe
 * whether CreateBuffer fires for matching candidates and whether the
 * foreign Map succeeds, without needing to reach real gameplay to validate
 * the MECHANISM itself (only the final yaw-rotation proof needs gameplay).
 *
 * Buffers NOT matching the filter (the small immediate-context cb0
 * buffers) are simply never captured here, so their draws always fall
 * through unpatched - this mechanism intentionally covers only the deferred
 * world-geometry draws the Step 3 keystone proof needs to rotate, per the
 * SKIPCL finding of what the deferred lists actually carry.
 *
 * ---- Threading: per-thread scratch-buffer rings ----
 *
 * Unchanged from the first pass - see mvp_alloc_scratch_index()'s own
 * comment: world draws are recorded from up to 6 deferred-context worker
 * threads concurrently plus the immediate/render thread; a single shared
 * scratch-buffer ring could let two threads' Map/memcpy/Unmap/Bind
 * sequences collide on the same ID3D11Buffer object. Each thread gets its
 * own private sub-ring instead, assigned once via a TLS slot.
 */

/* vtable indices, cross-checked against shaderdump.c's and seqdump.c's own
 * already-verified counts. ID3D11DeviceContext: DrawIndexed=12, Draw=13.
 * ID3D11Device: 3 IUnknown slots then the interface's own methods in
 * declaration order - CreateBuffer=3 (shaderdump.c's own comment: "3
 * IUnknown slots, then ... CreateBuffer=3 ... CreateVertexShader=12"). */
#define VTBL_CTX_DRAWINDEXED 12
#define VTBL_CTX_DRAW        13
#define VTBL_DEV_CREATEBUFFER 3
/* Fix round 4: index 48, matching cbdump.c's and seqdump.c's already-
 * verified ID3D11DeviceContext::UpdateSubresource slot. */
#define VTBL_CTX_MAP         14
#define VTBL_CTX_UNMAP       15
#define VTBL_CTX_UPDATESUBRESOURCE 48

typedef void(STDMETHODCALLTYPE *DrawIndexed_t)(ID3D11DeviceContext *, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE *Draw_t)(ID3D11DeviceContext *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *CreateBuffer_t)(ID3D11Device *, const D3D11_BUFFER_DESC *,
                                                    const D3D11_SUBRESOURCE_DATA *, ID3D11Buffer **);
typedef HRESULT (STDMETHODCALLTYPE *Map_t)(ID3D11DeviceContext *, ID3D11Resource *, UINT,
                                            D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
typedef void (STDMETHODCALLTYPE *Unmap_t)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
typedef void(STDMETHODCALLTYPE *UpdateSubresource_t)(ID3D11DeviceContext *, ID3D11Resource *, UINT,
                                                      const D3D11_BOX *, const void *, UINT, UINT);

/* Fixed local-copy/scratch window.
 *
 * FIX ROUND 4: raised 512 -> 2048. This MUST be >= the real world cb0's
 * size (1920 bytes), not merely >= the largest reflected mvp row offset:
 * mvp_patch_prepare() binds a scratch buffer *in place of* the engine's own
 * cb0, so a 512-byte scratch standing in for a 1920-byte cb0 would leave
 * bytes 512..1919 of that constant buffer undefined (reads past a bound
 * constant buffer's end return zero) - i.e. every OTHER per-object constant
 * in the world cb0 would be silently zeroed for every patched draw. That
 * would have rendered visibly-broken geometry the moment patching started
 * working, and read as "camera ownership failed". 2048 is the next multiple
 * of 16 above 1920 with a little headroom. */
#define MVP_CB_BYTES 2048

/* CreateBuffer filter: a TIGHT band around the world cb0 pool buffers'
 * real size, not a wide "roughly constant-buffer-pool-shaped" range.
 *
 * Fix round 1 used [512, 8192] ("wide enough to catch ~1920B, narrow
 * enough to exclude 96-224B per-shader cb0s") and a real human-witnessed
 * gameplay session (Step 3, TEWVR_TEST_YAW=20 confirmed active, 15,600+
 * frames, world did NOT rotate) proved that band far too wide: real
 * gameplay creates many OTHER dynamic constant buffers in the 512-8192
 * range with nothing to do with the world MVP pool (lighting, cloth,
 * particles, per-object data of various kinds) - observed sizes 512, 528,
 * 544, 560, 608, 624, 656, 704, 720, 752, 784, 864, 912, 1168, 1184, 1200,
 * 1264, 1392, 1536 - and they filled the entire MVP_DIRECT_POOL_MAX (then
 * 48) capacity before the real ~1920B buffers were ever created, silently
 * dropping the actual target forever for the rest of the session
 * (mvp_direct_pool_try_capture()'s "pool full" branch).
 *
 * Re-verified the real target's size independently from two historical
 * capture logs before narrowing this band:
 *   - D:\TheEvilWithinVR\captures\task6-cbpeek-seqdump.log: every one of
 *     192 CBPEEK content-read lines for the world MVP buffer reads
 *     size=1920, with ZERO variance (the other sizes present in that log -
 *     384x40, 96x10, 64x1 - are the slot2/3 skinning-decoy pair and small
 *     per-shader cb0s, not this buffer).
 *   - D:\TheEvilWithinVR\captures\task6-gameplay-seqdump.log: every
 *     VSSETCB line binding a buffer to VS slot 0 shows either exactly
 *     :1920 (2992 occurrences - the world pool) or one of the small
 *     per-shader sizes already characterised by Task 4/5 (96/224/128/64/
 *     16/160/80/272/176/144, all comfortably excluded by any band starting
 *     above ~512) - NEVER any value near-but-not-exactly 1920.
 * Both logs agree: the real buffer's size is exactly 1920 bytes, with no
 * observed variance across ~3200 combined samples from two different
 * capture sessions.
 *
 * Given that, [1856, 1984] (1920 +/- 64, both multiples of 16 - the
 * required D3D11 constant-buffer alignment) is the chosen band: tight
 * enough to leave a >300-byte margin on both sides of every OTHER buffer
 * size observed competing for this filter in real gameplay (nearest
 * competitor below is 1536 - a 320-byte gap to 1856; nothing was ever
 * observed between 1536 and 1920 at all, though the fix-round-1 pool
 * filling up before 1920 ever appeared means that gap is not fully
 * verified empty), while still tolerating a modest amount of per-level/
 * scene size variation (e.g. a different level's material layout adding a
 * few more constants to the pool's declared size) that an exact
 * `== 1920` match would not survive. If a future session's evidence shows
 * the real buffer size varying outside this band (e.g. a DLC level with a
 * differently-sized pool), widen it then, informed by that evidence -
 * exactly the same way this band itself was chosen. */
#define MVP_POOL_BUF_MIN_BYTES 1856
#define MVP_POOL_BUF_MAX_BYTES 1984

/* FIX ROUND 5, finding #3: make the band's relationship to the scratch
 * window a CHECKED invariant rather than a coincidence.
 *
 * mvp_patch_prepare() binds a scratch buffer of MVP_CB_BYTES *in place of*
 * the engine's own cb0. If a registered buffer were ever larger than the
 * scratch buffer, the bytes past MVP_CB_BYTES would read as zero in the
 * shader - silently zeroing real per-object constants on every patched
 * draw. That is exactly the bug fix round 4 found (MVP_CB_BYTES was 512
 * against a 1920-byte cb0), and right now the ONLY thing preventing its
 * return is that 1984 happens to be less than 2048 - while the comment
 * directly above actively invites widening the band on future evidence.
 *
 * This static assertion makes that a build error instead of a silent
 * rendering corruption. If you widen MVP_POOL_BUF_MAX_BYTES past
 * MVP_CB_BYTES, you MUST raise MVP_CB_BYTES to match (and keep it a
 * multiple of 16). Implemented as a negative-array-size typedef so it works
 * on this C89/C99-era toolchain without <assert.h> or _Static_assert. */
typedef char mvp_assert_scratch_covers_pool_band[
    (MVP_POOL_BUF_MAX_BYTES <= MVP_CB_BYTES) ? 1 : -1];
/* Constant buffers must also be a multiple of 16 bytes. */
typedef char mvp_assert_cb_bytes_is_16_aligned[
    ((MVP_CB_BYTES % 16) == 0) ? 1 : -1];

/* Distinct candidate buffer identities this module will ever try to
 * persistently map. Task 5/6 discovery observed ~6 for the specific world
 * MVP pool (one per deferred worker thread). Fix round 1 sized this
 * generously (48) specifically to survive a WIDE, decoy-prone filter;
 * now that the filter above is a tight band only the real pool (and
 * anything else that also happens to be almost exactly 1920 bytes, which
 * no capture has ever shown) can match, decoys are no longer expected to
 * contend for slots at all. Reduced to 16 - still ~2.5x headroom over the
 * expected steady-state ~6, enough to tolerate the pool being recreated
 * (a fresh set of ~6 buffers) once or twice across a session (e.g. on a
 * level transition) without the old, now-orphaned identities' slots ever
 * being reclaimed (this module has no eviction - see the Concerns note in
 * the report about revisiting that if a session ever legitimately needs
 * more than 16). Each entry is tiny (a pointer + a CPU pointer + a UINT),
 * so this remains cheap either way.
 *
 * FIX ROUND 4b UPDATE: the reasoning above (a tight descriptor band keeps
 * decoys out) turned out to be wrong on real evidence - the corrected
 * DEFAULT/1920B band still matched 16 buffers before the main menu had
 * finished, filling this pool with decoys exactly as the wide band did.
 * Registration therefore moved to the draw path, where the criterion is
 * "actually bound at VS slot 0 for a draw with a usable mvpmatrix" - so
 * decoys structurally cannot occupy slots any more. 32 is generous headroom
 * over the ~5-6 buffers a level actually uses, leaving room for several
 * level transitions' worth of orphaned identities (this module still has no
 * eviction - see the report's Concerns). */
#define MVP_DIRECT_POOL_MAX 32

/* ---- the per-shader DYNAMIC cb0 path (2026-09-04c) ------------------------
 *
 * WHY THIS EXISTS. The pool above tracks only the large shared DEFAULT world
 * buffer, because UpdateSubresource is the only CPU write path a DEFAULT
 * buffer has and that is what feeds its shadow. Everything else counted as
 * `pool_miss` - about a quarter of MVP-bearing draws, the coverage gap in
 * dossier S6/S11.
 *
 * 2026-09-04b measured live what those missed draws actually are, using the
 * bucketing added that morning: they are NOT harmless small post/UI draws.
 * `combo[5]` (ByteWidth 160, indexed geometry 750-1016), `combo[7]` (224,
 * geom 1001), `combo[3]` (272, non-indexed up to 120,000 vertices),
 * `combo[4]` (304, up to 75,000) - all `Usage=2` (DYNAMIC)
 * `[verified-live 2026-09-04]`. Those are world meshes, and they render at
 * the wrong per-eye orientation today.
 *
 * The disk census agrees on the shapes: cb0=160 mvpx=32 (127 shaders),
 * cb0=224 mvpx=32 (240), cb0=272 mvpx=32 (33) and mvpx=144 (18), cb0=304
 * mvpx=144 (9) - all MVP-bearing, offsets already known
 * `[inferred-static 2026-09-03, from common.tangoresource]`. So the shaders
 * are patchable; only the buffer was out of reach.
 *
 * A DYNAMIC buffer is written through Map(WRITE_DISCARD)/Unmap, so the fix is
 * one more shadow SOURCE, not a new patch mechanism: hook Map/Unmap, copy the
 * mapped contents into a shadow at Unmap while the pointer is still valid, and
 * the existing draw-time path (look up shadow, apply K, write a scratch
 * buffer, rebind slot 0) then works unchanged.
 *
 * The slots are a PARTITION of the same arrays rather than a second pool, so
 * every seqlock, validity and tearing guarantee already written and reviewed
 * applies to them without being duplicated: [0, MVP_DIRECT_POOL_MAX) is the
 * DEFAULT pool, [MVP_DIRECT_POOL_MAX, +MVP_DYN_POOL_MAX) is this one. The two
 * finds scan only their own range, so the hot DEFAULT lookup does not get
 * slower.
 *
 * ⚠️ SIZING IS A GUESS. 64 is chosen because the live evidence names four
 * distinct (size, usage) combos but says nothing about how many distinct
 * BUFFERS back them - an engine may use a handful of dynamic ring buffers or
 * one per material. The pool logs when it fills, exactly as the DEFAULT one
 * does, so the first run says whether 64 was right.
 *
 * ⚠️ A KNOWN RISK, NOT DESIGNED AWAY. Two deferred contexts may legally Map
 * the same ID3D11Buffer at the same time (WRITE_DISCARD renames per context),
 * and a shadow keyed by buffer pointer cannot represent both. The existing
 * seqlock detects that as a concurrent write and the draw falls through
 * unpatched, which is fail-safe rather than wrong - and
 * `g_diag_shadow_concurrent` counts it, so a run says whether it happens here.
 * If it does, this path needs a per-context shadow, not a per-buffer one. */
#define MVP_DYN_POOL_MAX  64
#define MVP_DYN_MAX_BYTES 512   /* observed 160..304; 512 leaves headroom without
                                   admitting the large shared buffers */
#define MVP_POOL_SLOTS_TOTAL (MVP_DIRECT_POOL_MAX + MVP_DYN_POOL_MAX)

/* 2026-09-04c, the DYNAMIC cb0 partition. Two ways this could go wrong
 * silently, both now build errors:
 *  - a dynamic buffer wider than the shadow window would be truncated at
 *    Unmap and then patched from a partial copy;
 *  - if the two pool sizes and the array size ever drift apart, a dynamic
 *    slot index would run off the end of every shadow array at once. */
typedef char mvp_assert_dyn_fits_the_shadow[
    (MVP_DYN_MAX_BYTES <= MVP_CB_BYTES) ? 1 : -1];
typedef char mvp_assert_pool_partition_adds_up[
    (MVP_POOL_SLOTS_TOTAL == MVP_DIRECT_POOL_MAX + MVP_DYN_POOL_MAX) ? 1 : -1];


/* Per-thread scratch-buffer sub-rings: MVP_THREADS_MAX comfortably exceeds
 * the 7 threads (6 deferred workers + 1 immediate/render) Task 5's
 * discovery captures ever actually observed; MVP_SLOTS_PER_THREAD matches
 * the brief's "start at 64" total ring size (8*8=64). */
#define MVP_THREADS_MAX 8
#define MVP_SLOTS_PER_THREAD 8
#define MVP_SCRATCH_TOTAL (MVP_THREADS_MAX * MVP_SLOTS_PER_THREAD)

/* ---- rate limiter (same burst-then-every-Nth scheme as seqdump.c's
 * seq_rate_limit_should_fire() - copied locally rather than shared, since
 * that one is private to seqdump.c and this module must stay independent
 * of whether TEWVR_SEQDUMP is even active). ---- */
#define MVP_RATE_LIMIT_BURST 10
#define MVP_RATE_LIMIT_EVERY_NTH 500
static int mvp_rate_limit_should_fire(volatile LONG *counter) {
    LONG n = InterlockedIncrement(counter);
    if (n <= MVP_RATE_LIMIT_BURST) {
        return 1;
    }
    return (n % MVP_RATE_LIMIT_EVERY_NTH) == 0;
}
static volatile LONG g_map_fail_log_count = 0;
static volatile LONG g_offset_oob_log_count = 0;
static volatile LONG g_update_bug_log_count = 0;

/* ---- per-draw skip-reason diagnostics (Task 6 fix round 3) ----
 * Added after a real gameplay session (TEWVR_TEST_YAW=90, correct
 * candidate buffers now captured per fix round 2's filter fix) still never
 * fired `mvp_patch: scratch pool ready` even once - i.e. no draw was ever
 * actually patched, with zero visibility into why. mvp_patch_prepare() is
 * a genuine hot path (~1900 calls/frame across ~7 threads), so this is a
 * counter-and-periodic-summary design, NOT per-occurrence logging: every
 * exit point in mvp_patch_prepare() increments exactly one
 * InterlockedIncrement'd counter (cheap, lock-free), and
 * mvp_diag_maybe_report() - called once per mvp_patch_prepare() invocation
 * - does a cheap elapsed-time check and only actually reads/resets/logs
 * the counters at most once every MVP_DIAG_REPORT_INTERVAL_MS, gated so
 * only one thread ever performs a given report (InterlockedCompareExchange
 * gate) even though many threads call this every draw. Diagnostic-only:
 * removing this whole block would not change patch behaviour at all, only
 * observability. */
#define MVP_DIAG_REPORT_INTERVAL_MS 5000

static volatile LONG g_diag_not_installed = 0;      /* exit: !g_installed (should be ~0 during real play) */
static volatile LONG g_diag_no_vs = 0;               /* exit: VSGetShader returned NULL */
static volatile LONG g_diag_shader_unknown = 0;      /* exit: shader untracked by mvptable at all */
static volatile LONG g_diag_shader_no_mvp = 0;       /* exit: shader known, has no mvpmatrix at all (expected for post/depth-only shaders) */
static volatile LONG g_diag_shader_rows_incomplete = 0; /* exit: shader known, has mvpx, but not all four mvpmatrix rows were found */
static volatile LONG g_diag_no_slot0_buf = 0;        /* exit: nothing bound at VS slot 0 */
static volatile LONG g_diag_pool_miss = 0;           /* exit: bound slot0 buffer is not in the direct-map pool - THE key counter for this round's mystery */
static volatile LONG g_diag_bounds_fail = 0;         /* exit: Important #4's bounds check failed (also has its own immediate rate-limited log) */
static volatile LONG g_diag_scratch_not_ready = 0;   /* exit: lazy scratch-pool init not (yet) successful */
static volatile LONG g_diag_scratch_alloc_fail = 0;  /* exit: per-thread scratch index/buffer unavailable */
static volatile LONG g_diag_scratch_map_fail = 0;    /* exit: scratch Map(WRITE_DISCARD) failed (also has its own immediate rate-limited log) */
static volatile LONG g_diag_patched = 0;             /* success: a draw was actually patched */

static volatile LONG g_pool_miss_first_logged = 0; /* sticky one-shot gate for the detailed first-miss log */
#define MVP_MISS_REPORT_EVERY 6 /* bucketed miss table every 6th 5-second window (~30 s) while misses continue */
static LONG g_miss_report_tick = 0; /* only touched under g_diag_report_gate */
static volatile LONG g_diag_report_gate = 0;       /* ensures only one thread performs a given periodic report */
static ULONGLONG g_diag_last_report_ms = 0;        /* only ever touched by whichever thread currently holds g_diag_report_gate */

/* ---- fix round 4 diagnostics: WHAT are the missed buffers, and did our
 * CreateBuffer hook ever see them created? ----
 *
 * Round 3's counters proved pool_miss dominates during real gameplay
 * (~180k/5s, patched=0, with 4 correct 1920B buffers in the pool). That
 * leaves exactly two possible explanations, and they need opposite fixes:
 *
 *   (A) Our CreateBuffer hook DID see the bound buffer get created, but the
 *       [1856,1984]-byte DYNAMIC/CPU_WRITE filter rejected it -> the filter
 *       (or the whole "the world pool is DYNAMIC" premise) is wrong.
 *   (B) Our CreateBuffer hook NEVER saw it created at all -> the hook does
 *       not cover whatever device/vtable the engine really creates its
 *       world buffers on; a filter change would be useless.
 *
 * `g_seen_cb[]` records EVERY constant buffer that passes through
 * Hook_CreateBuffer (pointer + desc, bounded, no AddRef - this is a
 * pointer-identity diagnostic only, and a freed-then-reused pointer can at
 * worst mislabel one sample). A sampled pool-miss then looks the missed
 * buffer up in it, which discriminates (A) from (B) directly. */
/* Tentative declarations so the diagnostic reporter below can read state
 * that is defined further down (C tentative definitions - the real
 * definitions, with initializers, follow in their own sections). */
static int g_direct_pool_count;
static int g_dyn_pool_count;                  /* 2026-09-04c, defined with the dynamic pool below */
static volatile LONG g_diag_dyn_patched;
static volatile LONG g_diag_dyn_empty;
static volatile LONG g_dyn_shadow_writes;
static volatile LONG g_diag_map_overflow;
/* 2026-09-05 pairing triplet; defined with the map table below. Tentative
 * declarations, because the periodic reporter above reads them. */
static volatile LONG g_diag_dyn_map_seen;
static volatile LONG g_diag_dyn_unmap_seen;
static volatile LONG g_diag_dyn_unmap_nomap;
static int g_update_target_count;
static volatile LONG g_shadow_writes;
static volatile LONG g_diag_shadow_empty;
static volatile LONG g_diag_shadow_partial;
static volatile LONG g_diag_shadow_multiwriter;
static volatile LONG g_diag_latehook_after_valid;
static volatile LONG g_diag_shadow_concurrent;
static volatile LONG g_diag_shadow_torn;

#define MVP_SEEN_CB_MAX      2048
#define MVP_MISS_COMBO_MAX   24
#define MVP_MISS_SAMPLE_MASK 0xFF /* GetDesc on ~1 in 256 pool-misses (~140/s at observed rates) */
#define MVP_REGISTER_PROBE_MASK 0x3F /* try registering a missed buffer on ~1 in 64 misses */

struct SeenCb {
    ID3D11Buffer *buf;
    UINT byte_width, usage, bind, cpu;
};
static struct SeenCb g_seen_cb[MVP_SEEN_CB_MAX];
static volatile LONG g_seen_cb_count = 0;

/* 2026-09-04: what the missed DRAWS look like, per combo - the question the
 * size/usage buckets alone cannot answer ("do the missed draws carry world
 * geometry?"). Two cheap proxies for "geometry", both from arguments the
 * draw hook already has in hand:
 *   - the draw's index/vertex count: a full-screen quad is 3-6, a particle
 *     or UI sprite a few dozen, a world mesh hundreds to tens of thousands;
 *   - the VERTEX SHADER's hash, which names the shader family offline via
 *     dev-archive/recon/2026-09-03-tangoresource-and-branch-merge/ (the
 *     runtime hash matches the on-disk archive 167/168) - a missed draw
 *     using the same world-mesh shader that is patched elsewhere IS world
 *     geometry, whatever its size. */
#define MVP_MISS_GEOM_MIN_COUNT 300 /* >= 100 triangles indexed: "real mesh", not a sprite/quad */
#define MVP_MISS_TINY_MAX_COUNT 6   /* <= 2 triangles: full-screen pass or a billboard */
#define MVP_MISS_SHADERS_PER_COMBO 8
struct MissCombo {
    UINT byte_width, usage, bind, cpu;
    LONG seen_created;   /* sampled misses whose buffer WAS created through our hook */
    LONG unseen_created; /* sampled misses whose buffer was NEVER seen by our hook */
    LONG draws_geom;     /* sampled misses with count >= MVP_MISS_GEOM_MIN_COUNT */
    LONG draws_tiny;     /* sampled misses with count <= MVP_MISS_TINY_MAX_COUNT */
    LONG draws_nonindexed; /* sampled misses that came through Draw, not DrawIndexed */
    volatile LONG max_count; /* largest index/vertex count seen in this combo */
    uint64_t sh_hash[MVP_MISS_SHADERS_PER_COMBO]; /* distinct vertex shaders seen missing here */
    LONG sh_hits[MVP_MISS_SHADERS_PER_COMBO];
    volatile LONG sh_count;
    LONG sh_overflow;    /* sampled misses whose shader did not fit the per-combo set */
};
static struct MissCombo g_miss_combo[MVP_MISS_COMBO_MAX];
static volatile LONG g_miss_combo_count = 0;
static volatile LONG g_miss_sample_tick = 0;

static volatile LONG g_cb_create_total = 0; /* every CreateBuffer call our hook saw */
static volatile LONG g_cb_create_const = 0; /* ...of which were constant buffers */

/* Lock-free append-only record of one created constant buffer. Overflow is
 * silently ignored (bounded diagnostic, never an error path). */
static void mvp_seen_cb_record(ID3D11Buffer *buf, const D3D11_BUFFER_DESC *d) {
    LONG idx = InterlockedIncrement(&g_seen_cb_count) - 1;
    if (idx < 0 || idx >= MVP_SEEN_CB_MAX) {
        return;
    }
    g_seen_cb[idx].byte_width = d->ByteWidth;
    g_seen_cb[idx].usage = (UINT)d->Usage;
    g_seen_cb[idx].bind = d->BindFlags;
    g_seen_cb[idx].cpu = d->CPUAccessFlags;
    g_seen_cb[idx].buf = buf; /* published last, same convention as the direct pool */
}

static int mvp_seen_cb_contains(ID3D11Buffer *buf) {
    LONG i, n = g_seen_cb_count;
    if (n > MVP_SEEN_CB_MAX) {
        n = MVP_SEEN_CB_MAX;
    }
    for (i = 0; i < n; i++) {
        if (g_seen_cb[i].buf == buf) {
            return 1;
        }
    }
    return 0;
}

/* Called on ~1 in 256 pool-misses. Buckets the missed buffer's real desc by
 * distinct (size, usage, bind, cpu) combo and tallies whether our hook ever
 * saw it created. Lock-free; a rare duplicate combo row under a race is
 * harmless for a diagnostic. */
/* Per-combo draw-shape + shader tallies (2026-09-04). Lock-free like the
 * rest; a lost increment or a duplicate shader row under a race is harmless
 * for a diagnostic. */
static void mvp_miss_combo_tally(struct MissCombo *c, int seen, uint64_t vs_hash, UINT count,
                                 int indexed) {
    LONG j, sn, cur;

    InterlockedIncrement(seen ? &c->seen_created : &c->unseen_created);
    if (count >= MVP_MISS_GEOM_MIN_COUNT) {
        InterlockedIncrement(&c->draws_geom);
    } else if (count <= MVP_MISS_TINY_MAX_COUNT) {
        InterlockedIncrement(&c->draws_tiny);
    }
    if (!indexed) {
        InterlockedIncrement(&c->draws_nonindexed);
    }
    do {
        cur = c->max_count;
        if ((LONG)count <= cur) {
            break;
        }
    } while (InterlockedCompareExchange(&c->max_count, (LONG)count, cur) != cur);

    if (vs_hash == 0) {
        return; /* untracked shader - cannot name it, nothing to record */
    }
    sn = c->sh_count;
    if (sn > MVP_MISS_SHADERS_PER_COMBO) {
        sn = MVP_MISS_SHADERS_PER_COMBO;
    }
    for (j = 0; j < sn; j++) {
        if (c->sh_hash[j] == vs_hash) {
            InterlockedIncrement(&c->sh_hits[j]);
            return;
        }
    }
    j = InterlockedIncrement(&c->sh_count) - 1;
    if (j < 0 || j >= MVP_MISS_SHADERS_PER_COMBO) {
        InterlockedIncrement(&c->sh_overflow);
        return;
    }
    c->sh_hits[j] = 1;
    c->sh_hash[j] = vs_hash; /* published last */
}

static void mvp_miss_sample(ID3D11Buffer *buf, uint64_t vs_hash, UINT count, int indexed) {
    D3D11_BUFFER_DESC bd;
    LONG i, n, idx;
    int seen;

    memset(&bd, 0, sizeof(bd));
    ID3D11Buffer_GetDesc(buf, &bd);
    seen = mvp_seen_cb_contains(buf);

    n = g_miss_combo_count;
    if (n > MVP_MISS_COMBO_MAX) {
        n = MVP_MISS_COMBO_MAX;
    }
    for (i = 0; i < n; i++) {
        if (g_miss_combo[i].byte_width == bd.ByteWidth && g_miss_combo[i].usage == (UINT)bd.Usage &&
            g_miss_combo[i].bind == bd.BindFlags && g_miss_combo[i].cpu == bd.CPUAccessFlags) {
            mvp_miss_combo_tally(&g_miss_combo[i], seen, vs_hash, count, indexed);
            return;
        }
    }

    idx = InterlockedIncrement(&g_miss_combo_count) - 1;
    if (idx < 0 || idx >= MVP_MISS_COMBO_MAX) {
        return; /* table full - the first 24 distinct combos are plenty to diagnose with */
    }
    g_miss_combo[idx].usage = (UINT)bd.Usage;
    g_miss_combo[idx].bind = bd.BindFlags;
    g_miss_combo[idx].cpu = bd.CPUAccessFlags;
    mvp_miss_combo_tally(&g_miss_combo[idx], seen, vs_hash, count, indexed);
    g_miss_combo[idx].byte_width = bd.ByteWidth; /* published last (its non-zero-ness is not
                                                     relied on, but keeps the same convention) */
}

/* Dumps the miss-combo table + CreateBuffer coverage counters. Called only
 * from the (already rate-limited, single-threaded-by-gate) periodic report. */
static void mvp_diag_report_misses(void) {
    LONG i, n = g_miss_combo_count;
    if (n > MVP_MISS_COMBO_MAX) {
        n = MVP_MISS_COMBO_MAX;
    }
    log_msg("mvp_patch: DIAG CreateBuffer coverage: total_calls_seen=%ld of which constant_buffers=%ld "
             "(recorded identities=%ld, cap %d); world cb0 pool holds %d; UpdateSubresource targets "
             "hooked=%d, shadow writes=%ld (partial-box=%ld), draws skipped for an empty shadow=%ld, "
             "cross-thread shadow writes=%ld, CONCURRENT shadow writes=%ld, draws skipped for a torn "
             "shadow=%ld, late-hooks after a shadow was already valid=%ld",
             g_cb_create_total, g_cb_create_const, g_seen_cb_count, MVP_SEEN_CB_MAX, g_direct_pool_count,
             g_update_target_count, g_shadow_writes, g_diag_shadow_partial, g_diag_shadow_empty,
             g_diag_shadow_multiwriter, g_diag_shadow_concurrent, g_diag_shadow_torn,
             g_diag_latehook_after_valid);
    for (i = 0; i < n; i++) {
        const struct MissCombo *c = &g_miss_combo[i];
        LONG sampled = c->seen_created + c->unseen_created;
        LONG j, sn = c->sh_count;
        char shaders[MVP_MISS_SHADERS_PER_COMBO * 28 + 32];
        int pos = 0;

        if (sn > MVP_MISS_SHADERS_PER_COMBO) {
            sn = MVP_MISS_SHADERS_PER_COMBO;
        }
        shaders[0] = '\0';
        for (j = 0; j < sn && c->sh_hash[j] != 0; j++) {
            pos += snprintf(shaders + pos, sizeof(shaders) - (size_t)pos, "%s%016llX x%ld", j ? " " : "",
                            (unsigned long long)c->sh_hash[j], c->sh_hits[j]);
            if ((size_t)pos >= sizeof(shaders)) {
                break;
            }
        }
        log_msg("mvp_patch: DIAG miss-combo[%ld]: ByteWidth=%u Usage=%u BindFlags=0x%X CPUAccess=0x%X "
                 "| sampled misses: created-through-our-hook=%ld NEVER-seen-by-our-hook=%ld",
                 i, c->byte_width, c->usage, c->bind, c->cpu, c->seen_created, c->unseen_created);
        /* 2026-09-04: the draw-shape and shader-identity half. `sampled` is
         * 1-in-256 of the real miss count for this combo, so multiply by
         * ~256 for absolute draws; the RATIOS are what matter. */
        log_msg("mvp_patch: DIAG miss-combo[%ld] draws: sampled=%ld geom(count>=%d)=%ld tiny(count<=%d)=%ld "
                 "non-indexed=%ld max_count=%ld | vertex shaders: %s%s",
                 i, sampled, MVP_MISS_GEOM_MIN_COUNT, c->draws_geom, MVP_MISS_TINY_MAX_COUNT,
                 c->draws_tiny, c->draws_nonindexed, c->max_count,
                 shaders[0] ? shaders : "(none tracked)",
                 c->sh_overflow ? " (+more, per-combo shader set full)" : "");
    }
}

/* Cheap on every call (one InterlockedCompareExchange, and - in the
 * overwhelming majority of calls - one GetTickCount64 + comparison, then
 * done); only the one call per MVP_DIAG_REPORT_INTERVAL_MS that actually
 * wins the time check does the (still cheap - 12 InterlockedExchange
 * snapshot-and-reset ops plus one log_msg) reporting work. Snapshot-and-
 * RESET (InterlockedExchange, not a plain read) means a concurrent
 * increment from another thread mid-report is never lost, only deferred
 * to the next report - each individual counter's read+reset is atomic;
 * only the reported COUNTS' relative timing across different counters is
 * approximate, which is immaterial for a periodic diagnostic summary. */
static void mvp_diag_maybe_report(void) {
    ULONGLONG now;
    LONG not_installed, no_vs, shader_unknown, shader_no_mvp, shader_rows_incomplete,
        no_slot0_buf, pool_miss, bounds_fail, scratch_not_ready, scratch_alloc_fail,
        scratch_map_fail, patched;

    if (InterlockedCompareExchange(&g_diag_report_gate, 1, 0) != 0) {
        return; /* another thread is already inside this function right now */
    }

    now = GetTickCount64();
    if (now - g_diag_last_report_ms < MVP_DIAG_REPORT_INTERVAL_MS) {
        InterlockedExchange(&g_diag_report_gate, 0);
        return;
    }
    g_diag_last_report_ms = now;

    not_installed = InterlockedExchange(&g_diag_not_installed, 0);
    no_vs = InterlockedExchange(&g_diag_no_vs, 0);
    shader_unknown = InterlockedExchange(&g_diag_shader_unknown, 0);
    shader_no_mvp = InterlockedExchange(&g_diag_shader_no_mvp, 0);
    shader_rows_incomplete = InterlockedExchange(&g_diag_shader_rows_incomplete, 0);
    no_slot0_buf = InterlockedExchange(&g_diag_no_slot0_buf, 0);
    pool_miss = InterlockedExchange(&g_diag_pool_miss, 0);
    bounds_fail = InterlockedExchange(&g_diag_bounds_fail, 0);
    scratch_not_ready = InterlockedExchange(&g_diag_scratch_not_ready, 0);
    scratch_alloc_fail = InterlockedExchange(&g_diag_scratch_alloc_fail, 0);
    scratch_map_fail = InterlockedExchange(&g_diag_scratch_map_fail, 0);
    patched = InterlockedExchange(&g_diag_patched, 0);

    InterlockedExchange(&g_diag_report_gate, 0);

    if (not_installed || no_vs || shader_unknown || shader_no_mvp || shader_rows_incomplete ||
        no_slot0_buf || pool_miss || bounds_fail || scratch_not_ready || scratch_alloc_fail ||
        scratch_map_fail || patched) {
        log_msg("mvp_patch: DIAG last ~%dms: patched=%ld | skipped: not_installed=%ld no_vs=%ld "
                 "shader_unknown=%ld shader_no_mvp=%ld shader_rows_incomplete=%ld no_slot0_buf=%ld "
                 "pool_miss=%ld bounds_fail=%ld scratch_not_ready=%ld scratch_alloc_fail=%ld "
                 "scratch_map_fail=%ld",
                 MVP_DIAG_REPORT_INTERVAL_MS, patched, not_installed, no_vs, shader_unknown,
                 shader_no_mvp, shader_rows_incomplete, no_slot0_buf, pool_miss, bounds_fail,
                 scratch_not_ready, scratch_alloc_fail, scratch_map_fail);
        /* The DYNAMIC cb0 path (2026-09-04c). `dyn_patched` climbing is the
         * whole point of it: those are draws that used to land in pool_miss.
         * `dyn_empty` should fall to ~0 after the first frames - a buffer is
         * registered at a draw and only fed at the NEXT Map/Unmap, so a steady
         * non-zero means registration is happening but the writes are not
         * being seen, i.e. the game fills those buffers some other way. */
        if (g_dyn_pool_count || g_diag_dyn_patched || g_diag_dyn_empty) {
            log_msg("mvp_patch: DIAG dynamic cb0 path: pool=%d/%d buffers, shadow writes=%ld, "
                    "draws patched via it=%ld, registered-but-unfed skips=%ld, "
                    "map-table overflows=%ld",
                    g_dyn_pool_count, MVP_DYN_POOL_MAX, g_dyn_shadow_writes,
                    InterlockedExchange(&g_diag_dyn_patched, 0),
                    InterlockedExchange(&g_diag_dyn_empty, 0), g_diag_map_overflow);
            /* 2026-09-05. This line alone says which world we are in, and it is
             * the reason to read the log before touching any code again:
             *   maps>0, unmaps~=maps, no-map~0, shadow writes>0  -> FIXED.
             *   maps>0, unmaps==0                                -> our Unmap hook
             *       never sees these buffers. Deferred contexts use a DIFFERENT
             *       vtable flavor for Map/Unmap, and unlike UpdateSubresource
             *       these two are hooked ONCE on the immediate context and are
             *       never late-hooked. That is a different fix, not a tuning knob.
             *   maps>0, unmaps>0, no-map==unmaps                 -> the pairing key
             *       is still wrong; the (ctx,res) assumption is what to doubt.
             *   overflows>0                                      -> 24 probes were
             *       not enough, i.e. far more concurrent maps than believed. */
            log_msg("mvp_patch: DIAG dynamic cb0 pairing: maps seen=%ld, unmaps seen=%ld, "
                    "unmaps with no recorded map=%ld (all cumulative; overflows above should "
                    "stay 0 now the table is keyed on (context,resource))",
                    g_diag_dyn_map_seen, g_diag_dyn_unmap_seen, g_diag_dyn_unmap_nomap);
        }
        if (pool_miss > 0 && (patched == 0 || (++g_miss_report_tick % MVP_MISS_REPORT_EVERY) == 0)) {
            /* Round 4 printed this ONLY while patching was failing outright
             * (misses but no successes), so once patching worked the
             * bucketed table was never seen again - and the one question it
             * exists to answer (2026-09-03f: do the MISSED draws carry world
             * geometry, or are they the expected small per-shader dynamic
             * cb0s?) went unmeasured in every working session. Since
             * 2026-09-04 it also prints every MVP_MISS_REPORT_EVERY windows
             * while misses continue, and once more at shutdown. Same
             * failing-case behaviour as before. */
            mvp_diag_report_misses();
        }
    }

    /* Shadow-concurrency visibility (2026-09-01).
     *
     * The single-writer assumption behind the cb0 shadow is FALSE - writes are
     * routinely cross-thread - and it is only the observation that they are
     * never truly CONCURRENT that makes the shadow safe. That observation held
     * on the dev machine in every session tested, but it is a measurement, not
     * a structural guarantee, and different hardware or thread scheduling
     * could break it.
     *
     * The counters that would reveal a violation were reachable only through
     * mvp_diag_report_misses() above, which runs only while
     * `pool_miss > 0 && patched == 0` - i.e. only while patching is failing
     * outright. So in the normal, working case the safety counters were
     * printed exactly never, and a violation on another machine would have
     * been silent. Reported here instead, unconditionally on non-zero and
     * independently of whether anything else was logged, so "measured safe"
     * can become "monitored".
     *
     * Cumulative, deliberately not reset: unlike the per-window skip counters
     * above, the question these answer is "has this EVER happened on this
     * machine", not "how often in the last window". */
    if (g_diag_shadow_concurrent || g_diag_shadow_torn || g_diag_shadow_multiwriter) {
        log_msg("mvp_patch: DIAG shadow concurrency (cumulative): cross-thread writes=%ld, "
                "CONCURRENT writes=%ld, draws skipped for a torn shadow=%ld%s",
                g_diag_shadow_multiwriter, g_diag_shadow_concurrent, g_diag_shadow_torn,
                (g_diag_shadow_concurrent || g_diag_shadow_torn)
                    ? "  <== the single-writer precondition has been VIOLATED on this machine; "
                      "the shadow needs real synchronisation (per-context shadows) before this "
                      "build can be trusted here"
                    : "  (cross-thread only, no concurrency observed - the expected state)");
    }
    /* Deliberately silent if EVERY counter is 0 (e.g. at the menu, before
     * any draw reaches mvp_patch_prepare() at all, or between reports with
     * genuinely zero activity) - matches this module's existing
     * quiet-unless-something-happened logging style. */
}

/* ---- install-time hook bookkeeping ---- */

#define MVP_MAX_HOOKED_FUNCS 4
struct HookedFunc {
    void *addr;
    void *orig;
};
static struct HookedFunc g_hooked[MVP_MAX_HOOKED_FUNCS];
static int g_hooked_count = 0;

static DrawIndexed_t g_drawindexed_orig = NULL;
static Draw_t g_draw_orig = NULL;
static CreateBuffer_t g_createbuffer_orig = NULL;
static int g_installed = 0;

/* ---- direct persistent-pointer pool (Step 0's chosen mechanism) ---- */

struct DirectPoolEntry {
    ID3D11Buffer *buf; /* NULL until registered; written LAST (see
                           mvp_direct_pool_try_capture()) - a non-NULL read
                           here is this slot's "ready" signal */
    void *cpu_ptr;      /* live CPU-readable copy of this buffer's content:
                            fix round 4 makes this our own shadow, kept
                            current by Hook_UpdateSubresource */
    UINT byte_width;
};

/* Fix round 4: CPU shadow storage, one MVP_CB_BYTES window per pool slot.
 * Statically allocated (64KB total at the current caps: 32 slots x 2048B)
 * so registration needs no allocator and can never fail for lack of memory.
 *
 * ===== LOAD-BEARING PRECONDITION (fix round 5, finding #4) =====
 *
 * Each shadow slot is a single flat buffer with NO lock, NO seqlock and NO
 * per-context keying. That is correct ONLY under this precondition:
 *
 *     each world cb0 buffer identity is written (UpdateSubresource) and
 *     read (draw) by exactly ONE thread.
 *
 * The evidence for it is good but circumstantial: the engine allocates one
 * world cb0 per deferred worker thread (6 buffers observed, registered from
 * 5-6 distinct thread ids, each worker recording into its own context), so
 * in practice slot i only ever belongs to one worker. It has never been
 * enforced, and until this comment it was never even stated.
 *
 * If the precondition is ever violated, the failure mode is NOT a benign
 * skip: a draw could read a TORN shadow (half of update N, half of N+1) or
 * another thread's MVP, and APPLY it - a wrong-data path, which is exactly
 * what this module's fail-safe rule forbids. It would also be invisible,
 * because nothing here would notice.
 *
 * Rather than pay a lock on a path that runs ~110k times/second, this is
 * DETECTED cheaply instead: the first writer's thread id is recorded per
 * slot, and a later write from a different thread raises a one-shot
 * warning (see mvp_shadow_note_writer). That is a per-write integer compare
 * against a value that is almost always already in cache - and if the
 * warning ever fires, the precondition above is broken and this shadow
 * needs real synchronisation (a seqlock or per-context shadows) before
 * anything else in the module can be trusted. */
static unsigned char g_shadow[MVP_POOL_SLOTS_TOTAL][MVP_CB_BYTES];
/* Thread id of the first writer of each shadow slot (0 = none yet), and a
 * one-shot gate so the cross-thread warning cannot spam the log. */
static volatile LONG g_shadow_writer_tid[MVP_POOL_SLOTS_TOTAL];
static volatile LONG g_shadow_multiwriter_warned = 0;
static volatile LONG g_diag_shadow_multiwriter = 0; /* writes seen from a second thread */

/* ===== per-slot seqlock (fix round 5, added after the precondition above
 * was MEASURED to be false) =====
 *
 * The very first run carrying the finding-#4 detector reported 448,201
 * cross-thread writes out of 560,109 - ~80% of world cb0 updates come from
 * a thread other than that slot's first writer. The single-writer
 * precondition is not merely unproven, it is FALSE: the engine's worker
 * threads share these buffers.
 *
 * That makes an unsynchronised shadow a live wrong-data hazard rather than
 * a theoretical one, so the READER is now protected by a per-slot seqlock:
 * the writer makes the counter odd before its memcpy and even after, and
 * the reader re-reads it around its own copy and REFUSES to patch if the
 * value was odd or changed across the copy. A racing draw therefore skips
 * (the fail-safe path) instead of applying a torn matrix.
 *
 * The parity of the writer's own increment is free extra information: if
 * incrementing yields an EVEN value the counter was already odd, meaning
 * another writer was inside its memcpy at that instant - genuine
 * write/write concurrency, as opposed to threads merely taking turns across
 * frames. That distinction decides whether writer/writer exclusion (a real
 * per-slot lock) is needed on top of this, so it is counted rather than
 * assumed either way.
 *
 * IMPORTANT: a seqlock alone does NOT make concurrent WRITERS safe - it
 * only guarantees the reader never consumes a torn snapshot. If
 * g_diag_shadow_concurrent is ever non-zero, writer/writer exclusion is
 * still required and this is not finished. */
static volatile LONG g_shadow_seq[MVP_POOL_SLOTS_TOTAL];
static volatile LONG g_diag_shadow_concurrent = 0; /* a writer found another writer mid-copy */
static volatile LONG g_diag_shadow_torn = 0;       /* draws skipped: seqlock reported an unstable read */
static volatile LONG g_shadow_concurrent_warned = 0;
/* 0 until Hook_UpdateSubresource has written real content into that slot's
 * shadow. mvp_direct_pool_find() refuses to hand out a not-yet-written
 * shadow: patching a draw from an all-zero shadow would apply a confidently
 * wrong zero matrix instead of falling through unpatched. */
static volatile LONG g_shadow_valid[MVP_POOL_SLOTS_TOTAL];
static volatile LONG g_shadow_writes = 0;   /* UpdateSubresource calls that fed a pool shadow */
static volatile LONG g_diag_shadow_empty = 0; /* draws skipped because the shadow had no content yet */
static volatile LONG g_diag_shadow_partial = 0; /* partial-box (offset) shadow updates handled */
/* Fix round 5, finding #5: counts contexts whose UpdateSubresource hook went
 * live only AFTER some shadow was already valid - the window in which that
 * context's earlier updates were missed, leaving a shadow STALE (not empty),
 * which is a wrong-data path rather than a skip path. See the note in
 * mvp_maybe_late_hook_ctx(). */
static volatile LONG g_diag_latehook_after_valid = 0;
static struct DirectPoolEntry g_direct_pool[MVP_POOL_SLOTS_TOTAL];
static int g_direct_pool_count = 0; /* forward-declared above for the round-4 diagnostics */
static int g_direct_pool_full_warned = 0;
static CRITICAL_SECTION g_direct_pool_cs; /* guards reservation of a new slot only - see below */
static int g_direct_pool_cs_ready = 0;

/* ---- scratch buffer pool (lazy, real device obtained from a live ctx) ---- */

static ID3D11Buffer *g_scratch[MVP_SCRATCH_TOTAL];
static volatile LONG g_scratch_ready = 0;
static CRITICAL_SECTION g_scratch_init_cs; /* serializes the lazy-create-on-first-use race
                                               (Important finding #6: without this, two
                                               threads racing the first patchable draw could
                                               both see g_scratch_ready==0 and both create a
                                               full 64-buffer set, leaking one set) */
static int g_scratch_init_cs_ready = 0;

/* ---- per-thread scratch-ring assignment (see file header) ---- */

static DWORD g_tls_index = TLS_OUT_OF_INDEXES;
static volatile LONG g_next_bucket = 0;
static volatile LONG g_bucket_next[MVP_THREADS_MAX];
static volatile LONG g_overflow_log_count = 0;

/* ---- test matrix K (TEWVR_TEST_YAW) ---- */

static Mat4 g_K;

/* ================= install-time hook helper ================= */

/* Create -> publish -> enable, same ordering GOAL as Task 5 addendum 3's
 * g_hooked_funcs table in seqdump.c, applied here to BOTH this module's
 * own tiny bookkeeping table AND (unlike the first pass of this file) the
 * caller's static "original" variable itself: `*out_orig` is written
 * BEFORE MH_EnableHook() runs, not after hook_one() returns - closing the
 * same "hook live but its original not yet discoverable" window
 * minhook_glue.h's own split-create/enable contract warns about (a plain
 * static write is fine as long as it happens-before the enable, which the
 * FIRST version of this function did not guarantee: it returned `orig` via
 * `*out_orig` only at the very end, after mh_glue_enable() had already
 * made the target live). Unlike seqdump's table, this one does not need
 * per-vtable late-hooking or a lock on the hot path: DrawIndexed/Draw
 * share ONE underlying code address between the immediate and every
 * deferred-context vtable flavor (seqdump.c's own confirmed finding), and
 * CreateBuffer is a device (not per-context) method, so hooking each ONCE,
 * here, covers every ctx/device flavor for the rest of the process's
 * lifetime. This function only ever runs from mvp_patch_install(), once,
 * single-threaded, on the bootstrap thread - strictly before the game's
 * own render loop can start, so g_hooked[] itself needs no lock either.
 *
 * Important finding #7: if `target` is ALREADY hooked (MH_ERROR_ALREADY_
 * CREATED - almost certainly TEWVR_SEQDUMP=1 or TEWVR_SHADERDUMP=1, both of
 * which hook this same Draw/DrawIndexed address first), this logs an
 * explicit, named conflict rather than just the generic MinHook status
 * string - so a human debugging "the world didn't rotate" by turning on
 * one of those diagnostic flags for more visibility sees exactly why
 * mvp_patch went silent, instead of two unrelated-looking log lines. */
static int hook_one(void *target, void *detour, void **out_orig, const char *name) {
    void *orig = NULL;
    MH_STATUS st;

    st = MH_CreateHook(target, detour, &orig);
    if (st != MH_OK) {
        if (st == MH_ERROR_ALREADY_CREATED) {
            log_msg("mvp_patch: CONFLICT - %s is ALREADY HOOKED by another module before mvp_patch "
                     "could hook it (almost certainly TEWVR_SEQDUMP=1 or TEWVR_SHADERDUMP=1 - both "
                     "hook this exact Draw/DrawIndexed address first). mvp_patch's real per-draw MVP "
                     "override is DISABLED for this entire session as a result. If you are trying to "
                     "see the world rotate, unset those TEWVR_* diagnostic flags and relaunch.",
                     name);
        } else {
            log_msg("mvp_patch: MH_CreateHook(%s) failed: %s", name, MH_StatusToString(st));
        }
        return 0;
    }

    if (g_hooked_count >= MVP_MAX_HOOKED_FUNCS) {
        MH_RemoveHook(target);
        log_msg("mvp_patch: internal hooked-function table full (%d); refusing to enable %s",
                 MVP_MAX_HOOKED_FUNCS, name);
        return 0;
    }
    g_hooked[g_hooked_count].addr = target;
    g_hooked[g_hooked_count].orig = orig;
    g_hooked_count++;

    /* Publish to the caller's static BEFORE enabling - see this function's
     * own comment above. */
    *out_orig = orig;

    if (!mh_glue_enable(target, name)) {
        g_hooked_count--;
        *out_orig = NULL;
        return 0;
    }

    return 1;
}

/* ================= direct persistent-pointer pool ================= */

/* Returns the persistently-mapped CPU pointer for `buf` and fills
 * `*out_byte_width` with the buffer's real size (as captured at
 * CreateBuffer time), or returns NULL if `buf` was never captured (not a
 * world-pool buffer this mechanism covers - e.g. a small immediate-context
 * cb0). Lock-free: g_direct_pool_count only ever grows and existing
 * entries' fields are written once, before `buf` (the "ready" signal) is
 * published - see mvp_direct_pool_try_capture(). A draw racing a
 * concurrent capture can only ever see a benign miss (a not-yet-published
 * slot's `buf` still reads NULL, which never equals a real buffer
 * pointer), never a torn/inconsistent read. */
static int mvp_direct_pool_find(ID3D11Buffer *buf, UINT *out_byte_width) {
    int i, n;
    n = g_direct_pool_count;
    for (i = 0; i < n; i++) {
        if (g_direct_pool[i].buf == buf) {
            if (!g_shadow_valid[i]) {
                /* Registered, but Hook_UpdateSubresource has not yet fed it
                 * any content - fall through unpatched rather than patch
                 * from an all-zero shadow (fix round 4). */
                InterlockedIncrement(&g_diag_shadow_empty);
                return -1;
            }
            *out_byte_width = g_direct_pool[i].byte_width;
            return i; /* fix round 5: the SLOT, so the caller can seqlock the read */
        }
    }
    return -1;
}

/* Seqlock read side (fix round 5). Copies `bytes` from shadow slot `slot`
 * into `dst`, returning 1 only if no writer touched that slot at any point
 * during the copy. Returns 0 on a racing write - the caller MUST then fall
 * through unpatched, which is the fail-safe path: a torn MVP applied to a
 * draw is exactly the outcome this module forbids. */
static int mvp_shadow_read(int slot, void *dst, UINT bytes) {
    LONG s0, s1;

    s0 = g_shadow_seq[slot];
    if (s0 & 1) {
        return 0; /* a writer is mid-copy right now */
    }
    memcpy(dst, g_shadow[slot], bytes);
    /* InterlockedCompareExchange as an acquire fence: on x86/x64 loads are
     * not reordered with loads, but this also stops the COMPILER hoisting
     * the second read above the memcpy. */
    s1 = InterlockedCompareExchange(&g_shadow_seq[slot], s0, s0);
    return (s0 == s1);
}

/* Registers one world cb0 buffer identity: reserves a slot (locked, so two
 * concurrent registrations can never collide on the same index), takes our
 * own AddRef on the buffer, and points the slot at its zeroed CPU shadow.
 * The shadow stays invalid until Hook_UpdateSubresource first fills it.
 * Since fix round 4b this is called from the DRAW path, not from
 * Hook_CreateBuffer. Never crashes, never blocks a draw. */
static void mvp_direct_pool_try_capture(ID3D11Buffer *buf, UINT byte_width) {
    int idx, i;

    EnterCriticalSection(&g_direct_pool_cs);
    /* Dedupe: this is now called repeatedly from the draw path (see
     * mvp_try_register_bound_buffer), so the same buffer will be offered
     * many times before and after it is registered.
     *
     * Known benign publish window (fix round 5, minor - flagged, not fixed):
     * the dedupe scan reads g_direct_pool[i].buf, which the registering
     * thread writes LAST, outside this lock. Two threads offering the same
     * buffer within that window can both pass the scan and register it
     * twice. Impact is bounded and harmless: one wasted slot, and the two
     * entries hold identical content (both shadows are fed by the same
     * UpdateSubresource calls). Fixing it properly means moving the slot
     * publish inside the lock, which is easy but touches the lock-free read
     * path's invariants; deferred to the whole-branch review rather than
     * changed at the same time as the round-5 fixes. It did not occur in
     * any observed run (all 6 buffers registered exactly once). */
    for (i = 0; i < g_direct_pool_count; i++) {
        if (g_direct_pool[i].buf == buf) {
            LeaveCriticalSection(&g_direct_pool_cs);
            return;
        }
    }
    if (g_direct_pool_count >= MVP_DIRECT_POOL_MAX) {
        LeaveCriticalSection(&g_direct_pool_cs);
        if (!g_direct_pool_full_warned) {
            g_direct_pool_full_warned = 1;
            log_msg("mvp_patch: world cb0 pool full (%d); further matching constant buffers won't "
                     "get a shadow (their draws render unpatched)", MVP_DIRECT_POOL_MAX);
        }
        return;
    }
    idx = g_direct_pool_count++;
    LeaveCriticalSection(&g_direct_pool_cs);

    ID3D11Buffer_AddRef(buf); /* our OWN reference, independent of the engine's - released only in mvp_patch_remove() */

    memset(g_shadow[idx], 0, MVP_CB_BYTES);
    g_direct_pool[idx].cpu_ptr = g_shadow[idx];
    g_direct_pool[idx].byte_width = byte_width;
    /* `buf` written LAST: mvp_direct_pool_find() (lock-free) treats a
     * non-NULL buf as "this slot is ready", so cpu_ptr/byte_width must
     * already be visible first - same "publish readiness last" convention
     * already used elsewhere in this codebase (d3d_capture.c's
     * g_d3d_ready, shaderdump.c's g_current_vs). */
    g_direct_pool[idx].buf = buf;

    log_msg("mvp_patch: STEP0 - registered world cb0 buffer #%d (%u bytes, buf=%p); its content is "
             "shadowed on every UpdateSubresource, so its draws are patchable with no per-frame GPU work",
             idx, byte_width, (void *)buf);
}

/* FIX ROUND 4b: register world cb0 buffers from the DRAW path, not from
 * CreateBuffer.
 *
 * Registering at CreateBuffer time has now failed three separate ways
 * (rounds 1, 2 and the first half of round 4) for the same structural
 * reason: at creation time a buffer's descriptor is the ONLY thing we know
 * about it, and the descriptor alone does not distinguish the world cb0
 * pool from same-shaped buffers the engine creates for other purposes. The
 * first measurement of the corrected DEFAULT/1920B filter found 16 matching
 * buffers created before the main menu even finished - filling the pool
 * with decoys all over again, exactly as the wide DYNAMIC filter did in
 * round 2.
 *
 * The draw path knows something CreateBuffer never can: this buffer is
 * actually bound at VS slot 0 for a draw whose shader has a usable
 * mvpmatrix. That is the real definition of "a world cb0 buffer", so it is
 * the right place to decide. The pool then only ever holds buffers that
 * genuinely matter (~5-6 per level, matching the five distinct 1920-byte
 * slot-0 pointers in task6-gameplay-seqdump.log), decoys can never crowd it
 * out no matter how many the engine creates, and the CreateBuffer filter
 * stops being load-bearing at all.
 *
 * Cost: one ID3D11Buffer::GetDesc on a sampled fraction of pool misses
 * (~1 in 64, a few hundred per second at observed draw rates). The same
 * handful of buffers recur thousands of times per second, so every one of
 * them is registered within a few milliseconds of the level starting to
 * render. Registration is idempotent (the dedupe scan above). */
static void mvp_try_register_bound_buffer(ID3D11Buffer *buf) {
    D3D11_BUFFER_DESC bd;

    memset(&bd, 0, sizeof(bd));
    ID3D11Buffer_GetDesc(buf, &bd);

    if (!(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) {
        return;
    }
    /* DEFAULT + no CPU access is the measured profile of the real world
     * cb0 buffers, and is also exactly the class that UpdateSubresource is
     * the only CPU write path for - i.e. the class this module's shadow
     * mechanism can actually track. A DYNAMIC buffer would never receive a
     * shadow write and would only waste a slot. */
    if (bd.Usage != D3D11_USAGE_DEFAULT || bd.CPUAccessFlags != 0) {
        return;
    }
    if (bd.ByteWidth < MVP_POOL_BUF_MIN_BYTES || bd.ByteWidth > MVP_POOL_BUF_MAX_BYTES) {
        return;
    }
    mvp_direct_pool_try_capture(buf, bd.ByteWidth);
}

/* ============ fix round 4: UpdateSubresource shadowing ============
 *
 * WHY this replaces the persistent-Map read mechanism entirely.
 *
 * Fix round 3's counters, run against a visually-confirmed real gameplay
 * session (frame capture showing world geometry), reported ~180,000
 * pool_miss/5s with patched=0 and 4 "captured" 1920-byte buffers in the
 * pool. Fix round 4 added a sampled descriptor histogram of the buffers
 * actually bound at VS slot 0 on those missed draws, plus a record of every
 * constant buffer our CreateBuffer hook ever saw created. The result was
 * unambiguous:
 *
 *   miss-combo[1]: ByteWidth=1920 Usage=0 BindFlags=0x4 CPUAccess=0x0
 *                  | created-through-our-hook=6261  NEVER-seen=0
 *
 * Usage=0 is D3D11_USAGE_DEFAULT and CPUAccess=0 is no CPU access at all.
 * Two conclusions follow, and they retire two earlier theories outright:
 *
 *  1. Our CreateBuffer hook has COMPLETE coverage - every missed buffer,
 *     across every size class, reported NEVER-seen-by-our-hook=0. There is
 *     no hook-install timing race and no missed CreateBuffer call. (The
 *     hooks in fact go live ~450ms after DllMain and ~2.8s before the first
 *     draw, measured from tewvr.log timestamps.)
 *  2. The real world cb0 buffers are DEFAULT/no-CPU-access, so they CANNOT
 *     be Mapped at all. Step 0's option (b) - "Map the buffer once at
 *     creation and keep the CPU pointer forever" - is not merely suboptimal
 *     for them, it is impossible. The 1920-byte DYNAMIC buffers rounds 2/3
 *     were successfully capturing are a same-size decoy set that is never
 *     bound at slot 0 for a world draw (confirmed disjoint from the five
 *     1920-byte slot-0 pointers in task6-gameplay-seqdump.log).
 *
 * A DEFAULT constant buffer can only be written from the CPU with
 * UpdateSubresource, so that call is where the live MVP content is
 * observable. This module now shadows it: Hook_UpdateSubresource copies the
 * caller's source bytes into our own per-buffer CPU shadow, and the
 * draw-time path reads the shadow exactly where it used to read the mapped
 * pointer. Everything downstream (row extraction, K multiply, scratch
 * buffer, rebind) is unchanged.
 *
 * Dropping the foreign Map also removes, by construction, the residual
 * "the engine's own later Map could conflict with ours" risk that every
 * previous round had to carry as an open concern.
 *
 * MULTI-VTABLE DISPATCH: unlike DrawIndexed/Draw (one shared code address
 * across every context flavor - seqdump.c's confirmed finding),
 * UpdateSubresource is in the group seqdump found needs per-vtable
 * late-hooking (alongside Map/Unmap/VSSetShader/VSSetConstantBuffers).
 * That is almost certainly why seqdump's own gameplay capture recorded
 * ZERO UpdateSubresource events despite thousands of world draws: it only
 * ever hooked the immediate flavor, and the engine issues these updates on
 * its deferred worker contexts. So this module hooks UpdateSubresource once
 * on the immediate/dummy vtable at install, and additionally late-hooks it
 * on each new deferred context vtable the first time that context reaches
 * our Draw/DrawIndexed detour. Originals are resolved by target ADDRESS
 * (read back out of the calling context's own vtable slot, which MinHook
 * leaves untouched since it patches code, not vtables) - the same
 * address-keyed dispatch seqdump.c uses for exactly this reason. */

/* KNOWN GAP (fix round 5, minor - flagged, not fixed): only
 * ID3D11DeviceContext::UpdateSubresource (vtable 48) is hooked.
 * ID3D11DeviceContext1::UpdateSubresource1 (a separate, later vtable slot)
 * is NOT. If the engine ever routed a world cb0 update through the "1"
 * variant, that update would be missed and the shadow would go stale. No
 * evidence it does - the measured shadow-write volume (550k+ per session)
 * accounts for the observed update traffic, and this engine's context usage
 * is plain ID3D11DeviceContext throughout - but the gap is unremarked
 * elsewhere, so it is recorded here. */
#define MVP_UPDATE_TARGETS_MAX 8
#define MVP_SEEN_CTX_MAX       32

struct UpdateTarget {
    void *addr;                /* the vtable slot's code address (the MinHook target) */
    UpdateSubresource_t orig;  /* its trampoline */
};
static struct UpdateTarget g_update_targets[MVP_UPDATE_TARGETS_MAX];
static int g_update_target_count = 0;
static void *g_seen_ctx[MVP_SEEN_CTX_MAX];
/* Append-only; read lock-free on the draw hot path, appended only under
 * g_latehook_cs. Volatile because the fast-path scan reads it without the
 * lock - see mvp_maybe_late_hook_ctx(). */
static volatile LONG g_seen_ctx_count = 0;
static CRITICAL_SECTION g_latehook_cs; /* guards APPENDS to g_update_targets[]/g_seen_ctx[] only */
static int g_latehook_cs_ready = 0;
static int g_seen_ctx_full_warned = 0;

static void STDMETHODCALLTYPE Hook_UpdateSubresource(ID3D11DeviceContext *, ID3D11Resource *, UINT,
                                                      const D3D11_BOX *, const void *, UINT, UINT);

/* Resolve the trampoline for whichever UpdateSubresource address the
 * calling context's vtable actually points at. Lock-free read: the table
 * only ever grows, and each row's `addr` is published last. */
static UpdateSubresource_t mvp_update_orig_for(ID3D11DeviceContext *ctx) {
    void *target = (*(void ***)ctx)[VTBL_CTX_UPDATESUBRESOURCE];
    int i, n = g_update_target_count;
    for (i = 0; i < n; i++) {
        if (g_update_targets[i].addr == target) {
            return g_update_targets[i].orig;
        }
    }
    return NULL;
}

/* Hooks `target` for UpdateSubresource if that exact address is not hooked
 * by this module already. Idempotent and address-keyed, so calling it for a
 * vtable that shares an address with one already covered is a cheap no-op.
 * Must be called with g_latehook_cs held. Returns 1 if the address is
 * covered on return (whether by this call or an earlier one). */
static int mvp_hook_update_target_locked(void *target) {
    void *orig = NULL;
    MH_STATUS st;
    int i;

    if (target == NULL) {
        return 0;
    }
    for (i = 0; i < g_update_target_count; i++) {
        if (g_update_targets[i].addr == target) {
            return 1; /* already covered */
        }
    }
    if (g_update_target_count >= MVP_UPDATE_TARGETS_MAX) {
        return 0;
    }

    st = MH_CreateHook(target, (void *)&Hook_UpdateSubresource, &orig);
    if (st != MH_OK || orig == NULL) {
        log_msg("mvp_patch: MH_CreateHook(UpdateSubresource @ %p) failed: %s", target,
                 MH_StatusToString(st));
        return 0;
    }
    /* Publish the trampoline BEFORE enabling, so the detour can never run
     * with no original to call through to (same ordering discipline as
     * hook_one()). */
    g_update_targets[g_update_target_count].orig = (UpdateSubresource_t)orig;
    g_update_targets[g_update_target_count].addr = target;
    g_update_target_count++;

    st = MH_EnableHook(target);
    if (st != MH_OK) {
        g_update_target_count--;
        g_update_targets[g_update_target_count].addr = NULL;
        MH_RemoveHook(target);
        log_msg("mvp_patch: MH_EnableHook(UpdateSubresource @ %p) failed: %s", target,
                 MH_StatusToString(st));
        return 0;
    }
    log_msg("mvp_patch: UpdateSubresource hooked at %p (target #%d)", target, g_update_target_count - 1);
    return 1;
}

/* Called from the Draw/DrawIndexed detours - i.e. from the single hottest
 * function in the renderer, ~74,000 times per second across ~7 concurrent
 * recording threads. The first time a given context pointer is seen, make
 * sure ITS vtable's UpdateSubresource is hooked too.
 *
 * FIX ROUND 5, finding #1: this used to take g_latehook_cs unconditionally,
 * on every single draw. That funnelled all ~74k calls/s from all 7 threads
 * through ONE global lock, serialising the exact deferred-recording
 * parallelism the engine goes out of its way to provide - in the hot path,
 * to protect a table that is written at most ~7 times in a whole session.
 * The earlier comment costed the linear scan and simply missed the lock.
 *
 * Now: a lock-free fast-path scan, with the lock taken ONLY on a genuine
 * miss (the first sighting of a new context). This is safe because
 * g_seen_ctx[] is strictly append-only for the life of the module and each
 * entry is fully written before g_seen_ctx_count is advanced to publish it
 * (see below) - so a racing reader either does not see a slot at all, or
 * sees it complete. It never sees a torn or half-published entry. A reader
 * that races an in-progress append can at worst miss a context that another
 * thread is registering right now, and then take the slow path itself,
 * where the locked re-check makes the operation idempotent. */
static void mvp_maybe_late_hook_ctx(ID3D11DeviceContext *ctx) {
    void **vtbl;
    LONG i, n;

    if (ctx == NULL || !g_latehook_cs_ready) {
        return;
    }

    /* ---- lock-free fast path: the overwhelmingly common case ---- */
    n = g_seen_ctx_count;
    for (i = 0; i < n; i++) {
        if (g_seen_ctx[i] == (void *)ctx) {
            return; /* already inspected - no lock, no work */
        }
    }

    /* ---- slow path: first sighting of this context (at most ~7 per session) ---- */
    EnterCriticalSection(&g_latehook_cs);
    /* Re-check under the lock: another thread may have registered this same
     * context between our fast-path scan and acquiring the lock. */
    for (i = 0; i < g_seen_ctx_count; i++) {
        if (g_seen_ctx[i] == (void *)ctx) {
            LeaveCriticalSection(&g_latehook_cs);
            return;
        }
    }
    if (g_seen_ctx_count >= MVP_SEEN_CTX_MAX) {
        if (!g_seen_ctx_full_warned) {
            g_seen_ctx_full_warned = 1;
            log_msg("mvp_patch: seen-ctx table full (%d); further unknown contexts won't be "
                     "inspected for UpdateSubresource late-hooking", MVP_SEEN_CTX_MAX);
        }
        LeaveCriticalSection(&g_latehook_cs);
        return;
    }

    /* FIX ROUND 5, finding #5: detect the stale-shadow window.
     *
     * This context is drawing for the first time, which is only NOW causing
     * its vtable's UpdateSubresource to be hooked. Any UpdateSubresource
     * this context issued before this moment was missed. If some shadow is
     * ALREADY valid, a missed update means that shadow is STALE rather than
     * empty - and a stale shadow is patched from, using old data. That is a
     * wrong-data path, not the fail-safe skip path the empty-shadow guard
     * provides (the round-4 report described this window incorrectly). It is
     * narrow and transient (it can only happen in the level-start window,
     * before each worker's first draw) but it is not nothing, so count it. */
    {
        int s;
        for (s = 0; s < g_direct_pool_count; s++) {
            if (g_shadow_valid[s]) {
                InterlockedIncrement(&g_diag_latehook_after_valid);
                log_msg("mvp_patch: NOTE - context %p had its UpdateSubresource hooked only on its "
                         "first draw, while %d world cb0 shadow(s) were already valid. Any update "
                         "this context issued before now was missed, so a shadow may be STALE (old "
                         "data, still patched from) rather than empty for a frame or two.",
                         (void *)ctx, g_direct_pool_count);
                break;
            }
        }
    }

    /* Hook FIRST, then publish. If we published the context before hooking,
     * another thread's fast-path scan could see this context as "already
     * inspected" while its UpdateSubresource hook was not yet live. */
    vtbl = *(void ***)ctx;
    mvp_hook_update_target_locked(vtbl[VTBL_CTX_UPDATESUBRESOURCE]);

    g_seen_ctx[g_seen_ctx_count] = (void *)ctx;
    /* Publish by advancing the count only after the slot is fully written -
     * this is what makes the lock-free fast-path scan above valid. The
     * InterlockedIncrement is used for its release semantics, not for
     * mutual exclusion (we already hold the lock). */
    InterlockedIncrement(&g_seen_ctx_count);
    LeaveCriticalSection(&g_latehook_cs);
}

/* Fix round 5, finding #4: cheap single-writer verification for a shadow
 * slot. Records the first writing thread; if a LATER write to the same slot
 * comes from a different thread, the load-bearing precondition documented
 * at g_shadow's declaration is broken - raise a one-shot warning (and keep
 * a counter) so it can never happen silently. Cost on the hot path: one
 * cached read plus a compare in the common case. */
static void mvp_shadow_note_writer(int slot) {
    LONG me = (LONG)GetCurrentThreadId();
    LONG prev = g_shadow_writer_tid[slot];

    if (prev == me) {
        return; /* overwhelmingly the common case */
    }
    if (prev == 0) {
        /* First writer wins; a genuine tie here is itself a violation, and
         * the loser falls through to the warning below on its next write. */
        if (InterlockedCompareExchange(&g_shadow_writer_tid[slot], me, 0) == 0) {
            return;
        }
    }
    InterlockedIncrement(&g_diag_shadow_multiwriter);
    if (InterlockedCompareExchange(&g_shadow_multiwriter_warned, 1, 0) == 0) {
        log_msg("mvp_patch: WARNING - world cb0 shadow slot %d was written by thread %lu after "
                 "thread %lu. The unsynchronised single-writer precondition documented at g_shadow "
                 "is BROKEN: a draw can now read a torn or foreign MVP and APPLY it. This shadow "
                 "needs real synchronisation (seqlock or per-context shadows) before the patched "
                 "output can be trusted.",
                 slot, (unsigned long)me, (unsigned long)g_shadow_writer_tid[slot]);
    }
}

static void STDMETHODCALLTYPE Hook_UpdateSubresource(ID3D11DeviceContext *ctx, ID3D11Resource *pDstResource,
                                                      UINT DstSubresource, const D3D11_BOX *pDstBox,
                                                      const void *pSrcData, UINT SrcRowPitch,
                                                      UINT SrcDepthPitch) {
    UpdateSubresource_t orig = mvp_update_orig_for(ctx);

    /* Shadow BEFORE calling through: the engine's source buffer is only
     * guaranteed valid for the duration of this call. */
    if (pSrcData != NULL && DstSubresource == 0 && g_direct_pool_count > 0) {
        int i, n = g_direct_pool_count;
        for (i = 0; i < n; i++) {
            if (g_direct_pool[i].buf == (ID3D11Buffer *)pDstResource) {
                UINT cap = g_direct_pool[i].byte_width;
                UINT off = 0, bytes;

                if (cap > MVP_CB_BYTES) {
                    cap = MVP_CB_BYTES; /* also guaranteed unreachable by the static assert above */
                }

                /* FIX ROUND 5, finding #6: partial-box updates are now
                 * HANDLED rather than silently refused. For a BUFFER,
                 * D3D11_BOX::left/right are plain byte offsets, so a
                 * partial update is just a memcpy at an offset. Refusing
                 * them (the round-4 behaviour) left the shadow silently
                 * STALE with no counter and no log - the exact class of
                 * blind spot that cost this task three rounds. */
                if (pDstBox != NULL) {
                    if (pDstBox->right <= pDstBox->left) {
                        break; /* empty/degenerate box: nothing to copy */
                    }
                    off = pDstBox->left;
                    bytes = pDstBox->right - pDstBox->left;
                    if (off >= cap) {
                        break; /* entirely outside our window */
                    }
                    if (bytes > cap - off) {
                        bytes = cap - off; /* clamp to the shadow */
                    }
                    InterlockedIncrement(&g_diag_shadow_partial);
                } else {
                    bytes = cap;
                }

                mvp_shadow_note_writer(i);

                /* Seqlock write side: odd while the copy is in flight.
                 * InterlockedIncrement is a full barrier on x86/x64, so the
                 * memcpy cannot be hoisted above it or sunk below the
                 * closing increment. */
                if ((InterlockedIncrement(&g_shadow_seq[i]) & 1) == 0) {
                    /* We incremented to an EVEN value, so the counter was
                     * already odd: another thread is inside its own copy of
                     * this same slot right now. Genuine write/write
                     * concurrency - a seqlock does not make this safe. */
                    InterlockedIncrement(&g_diag_shadow_concurrent);
                    if (InterlockedCompareExchange(&g_shadow_concurrent_warned, 1, 0) == 0) {
                        log_msg("mvp_patch: WARNING - CONCURRENT writers on world cb0 shadow slot %d "
                                 "(thread %lu entered while another was mid-copy). The reader-side "
                                 "seqlock cannot make this safe: two writers can interleave into the "
                                 "same shadow. Writer/writer exclusion (a per-slot lock) is REQUIRED "
                                 "before this module's patched output can be trusted.",
                                 i, (unsigned long)GetCurrentThreadId());
                    }
                }

                memcpy(g_shadow[i] + off, pSrcData, bytes);

                /* A partial update only makes the shadow valid if the whole
                 * window has been populated at least once - otherwise the
                 * untouched remainder is still zeros, which must not be
                 * patched from. A whole-resource update always validates. */
                if (pDstBox == NULL) {
                    InterlockedExchange(&g_shadow_valid[i], 1); /* publish AFTER the copy */
                }
                InterlockedIncrement(&g_shadow_seq[i]); /* back to even: copy complete */
                InterlockedIncrement(&g_shadow_writes);
                break;
            }
        }
    }

    if (orig == NULL) {
        /* Unreachable in practice (every address we hook is published into
         * g_update_targets before its hook is enabled). Returning without
         * calling through would silently drop a real resource update, which
         * is far worse than the cost of one rate-limited complaint, so log
         * and drop only as an absolute last resort - there is no safe
         * "guess which original" here. */
        if (mvp_rate_limit_should_fire(&g_update_bug_log_count)) {
            log_msg("mvp_patch: BUG - no UpdateSubresource original for ctx=%p vtbl=%p; call dropped",
                     (void *)ctx, (void *)*(void ***)ctx);
        }
        return;
    }
    orig(ctx, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
}

/* ================= per-thread scratch-ring allocation ================= */

/* Returns a scratch-buffer index in [0, MVP_SCRATCH_TOTAL), guaranteed to
 * never collide with an index concurrently in use by another thread (see
 * file header) - or -1 if TLS was never set up. Lock-free on the hot path
 * (a TLS read plus one InterlockedIncrement); only the FIRST call from a
 * given thread pays a bucket assignment (also lock-free). */
static int mvp_alloc_scratch_index(void) {
    LONG_PTR raw;
    int bucket;
    LONG cursor;

    if (g_tls_index == TLS_OUT_OF_INDEXES) {
        return -1;
    }

    raw = (LONG_PTR)TlsGetValue(g_tls_index);
    if (raw == 0) {
        LONG assigned = InterlockedIncrement(&g_next_bucket) - 1;
        if (assigned >= MVP_THREADS_MAX) {
            assigned = MVP_THREADS_MAX - 1;
            if (mvp_rate_limit_should_fire(&g_overflow_log_count)) {
                log_msg("mvp_patch: thread-ring pool exhausted (%d distinct threads seen); "
                         "further threads share the last bucket - patch stays correct via "
                         "WRITE_DISCARD renaming, may rarely contend under unusually many threads",
                         MVP_THREADS_MAX);
            }
        }
        bucket = (int)assigned;
        TlsSetValue(g_tls_index, (LPVOID)(LONG_PTR)(assigned + 1)); /* +1: 0 means "unset" */
    } else {
        bucket = (int)(raw - 1);
    }

    cursor = InterlockedIncrement(&g_bucket_next[bucket]);
    return bucket * MVP_SLOTS_PER_THREAD + (int)(((ULONG)cursor) % (ULONG)MVP_SLOTS_PER_THREAD);
}

/* ================= lazy scratch-pool creation ================= */

static int ensure_scratch_ready(ID3D11Device *dev) {
    D3D11_BUFFER_DESC sd;
    HRESULT hr;
    int i;

    memset(&sd, 0, sizeof(sd));
    sd.ByteWidth = MVP_CB_BYTES;
    sd.Usage = D3D11_USAGE_DYNAMIC;
    sd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    for (i = 0; i < MVP_SCRATCH_TOTAL; i++) {
        hr = ID3D11Device_CreateBuffer(dev, &sd, NULL, &g_scratch[i]);
        if (FAILED(hr) || g_scratch[i] == NULL) {
            int j;
            log_msg("mvp_patch: CreateBuffer for scratch[%d/%d] failed (hr=0x%08lX); "
                     "per-draw MVP override DISABLED this session",
                     i, MVP_SCRATCH_TOTAL, (unsigned long)hr);
            for (j = 0; j < i; j++) {
                if (g_scratch[j] != NULL) {
                    ID3D11Buffer_Release(g_scratch[j]);
                    g_scratch[j] = NULL;
                }
            }
            return 0;
        }
    }
    return 1;
}

/* Like mvp_dyn_pool_find() but WITHOUT the validity check or its counter: the
 * Map hook needs the slot precisely so it can make it valid, and counting that
 * as "empty" every frame would bury the real diagnostic. */
static int mvp_dyn_pool_find_any(ID3D11Buffer *buf, UINT *out_byte_width) {
    int i, n = g_dyn_pool_count;
    for (i = 0; i < n; i++) {
        int slot = MVP_DIRECT_POOL_MAX + i;
        if (g_direct_pool[slot].buf == buf) {
            if (out_byte_width) *out_byte_width = g_direct_pool[slot].byte_width;
            return slot;
        }
    }
    return -1;
}
/* ================= the per-shader DYNAMIC cb0 pool ================= */
/* See the block comment above MVP_DYN_POOL_MAX for why this exists and what
 * it is NOT designed to survive. Slots live in [MVP_DIRECT_POOL_MAX,
 * MVP_DIRECT_POOL_MAX + g_dyn_pool_count) of the same arrays as the DEFAULT
 * pool, so they inherit its seqlock and validity discipline unchanged. */
/* Definitions for the tentative declarations near the top of the file (the
 * periodic diagnostic reporter runs above this point and reads them). */
static int  g_dyn_pool_full_warned = 0;

/* Lock-free, same contract as mvp_direct_pool_find(): returns the SLOT so the
 * caller can seqlock its read, or -1. */
static int mvp_dyn_pool_find(ID3D11Buffer *buf, UINT *out_byte_width) {
    int i, n = g_dyn_pool_count;
    for (i = 0; i < n; i++) {
        int slot = MVP_DIRECT_POOL_MAX + i;
        if (g_direct_pool[slot].buf == buf) {
            if (!g_shadow_valid[slot]) {
                /* Registered at a draw, but no Unmap has fed it yet - the
                 * first draw of a newly-seen buffer is always unpatched, and
                 * the next frame's Map/Unmap fixes it. Counted separately so
                 * "never fed" is distinguishable from "not registered". */
                InterlockedIncrement(&g_diag_dyn_empty);
                return -1;
            }
            *out_byte_width = g_direct_pool[slot].byte_width;
            return slot;
        }
    }
    return -1;
}

static void mvp_dyn_pool_try_capture(ID3D11Buffer *buf, UINT byte_width) {
    int idx, i, slot;

    EnterCriticalSection(&g_direct_pool_cs);
    for (i = 0; i < g_dyn_pool_count; i++) {
        if (g_direct_pool[MVP_DIRECT_POOL_MAX + i].buf == buf) {
            LeaveCriticalSection(&g_direct_pool_cs);
            return;
        }
    }
    if (g_dyn_pool_count >= MVP_DYN_POOL_MAX) {
        LeaveCriticalSection(&g_direct_pool_cs);
        if (!g_dyn_pool_full_warned) {
            g_dyn_pool_full_warned = 1;
            log_msg("mvp_patch: DYNAMIC cb0 pool full (%d slots). Further per-shader dynamic "
                     "constant buffers get no shadow and their draws render UNPATCHED. The size "
                     "was a guess (see the header); this line means it was too small - raise "
                     "MVP_DYN_POOL_MAX and rebuild.", MVP_DYN_POOL_MAX);
        }
        return;
    }
    idx = g_dyn_pool_count++;
    LeaveCriticalSection(&g_direct_pool_cs);

    slot = MVP_DIRECT_POOL_MAX + idx;
    ID3D11Buffer_AddRef(buf);   /* released in mvp_patch_remove() with the rest */
    memset(g_shadow[slot], 0, MVP_CB_BYTES);
    g_direct_pool[slot].cpu_ptr = g_shadow[slot];
    g_direct_pool[slot].byte_width = byte_width;
    InterlockedExchange(&g_shadow_valid[slot], 0);
    g_direct_pool[slot].buf = buf;   /* published LAST, same convention as the DEFAULT pool */

    log_msg("mvp_patch: registered DYNAMIC cb0 buffer #%d (%u bytes, buf=%p) - its Map/Unmap "
             "writes are now shadowed, so its draws become patchable from the next write on",
             idx, byte_width, (void *)buf);
}

/* Offered from the draw path only - never from CreateBuffer - for the same
 * reason the DEFAULT pool is: a descriptor alone cannot tell a world cb0 from
 * any other buffer of the same shape, but "bound at VS slot 0 for a draw whose
 * shader has known mvp rows" can. */
static void mvp_try_register_bound_dynamic(ID3D11Buffer *buf) {
    D3D11_BUFFER_DESC bd;

    memset(&bd, 0, sizeof(bd));
    ID3D11Buffer_GetDesc(buf, &bd);

    if (!(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;
    if (bd.Usage != D3D11_USAGE_DYNAMIC) return;
    if (!(bd.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE)) return;
    if (bd.ByteWidth == 0 || bd.ByteWidth > MVP_DYN_MAX_BYTES) return;

    mvp_dyn_pool_try_capture(buf, bd.ByteWidth);
}

/* ---- Map / Unmap: the shadow source for the pool above ---- */

/* 2026-09-05: RE-KEYED ON (context, resource) IDENTITY.
 *
 * What was here: a GLOBAL 32-entry table, claimed by CAS on `res` and searched
 * at Unmap by a linear scan for `res`. Live on 2026-09-04d it produced
 * `shadow writes=0` with `pending-map table overflows=2787733` - the table was
 * permanently full and no Map/Unmap pair ever completed.
 *
 * ⚠️ The 2026-09-04d write-up blamed "a per-thread ring pool sized for 8
 * threads", quoting the log line `thread-ring pool exhausted (8 distinct
 * threads seen)`. That line comes from the DRAW-TIME SCRATCH-BUFFER rings -
 * a different subsystem, whose own comment says the patch stays correct
 * through it via WRITE_DISCARD renaming. The pairing that failed was never a
 * per-thread ring; it was this table. Sizing a thread pool would not have
 * fixed it. `[inferred-static 2026-09-05]` - read out of the code, not
 * measured live.
 *
 * Why 32 entries keyed on `res` alone could not work, and it is written five
 * hundred lines above this in the file: "Two deferred contexts may legally Map
 * the same ID3D11Buffer at the same time (WRITE_DISCARD renames per context)."
 * TEW records world draws from ~6 deferred workers plus the immediate context.
 * So at any instant the SAME buffer can hold several in-flight maps, each with
 * its own pData - several entries with an identical `res`. A linear scan for
 * `res` then matches the wrong one, and with the buffers registered 54-deep the
 * 32 slots fill and never drain.
 *
 * The identity of an in-flight map is therefore NOT the buffer. It is the PAIR
 * (context, resource): D3D11 permits a given context only one outstanding Map
 * of a given subresource, so that pair is unique by construction and no thread
 * count can exhaust anything.
 *
 * Open-addressed, linear-probed, 1024 slots for a working set of at most a few
 * dozen. Lock-free and O(1) expected.
 *
 * Ordering that makes the lock-free part safe:
 *   claim  - CAS `res` (entry is now {res=X, ctx=NULL}), THEN write ctx/data/slot;
 *   free   - clear ctx and data FIRST, release `res` LAST;
 *   match  - require BOTH `res` and `ctx`.
 * A prober that catches a half-claimed entry sees ctx==NULL, matches nothing and
 * probes on. The only Unmap that can match an entry runs on the thread that
 * made it, after Map returned, so it always sees the fields fully written.
 *
 * The geometry and the hash live in mvp_maptab.h so that
 * tools/map_pairing_test.c exercises THE SHIPPED FUNCTION rather than a
 * transcription of it - the Far Cry 2 lesson, where a Python transcription
 * passed and compiling the real stereo.c into a harness was what found the
 * bugs. */
#include "mvp_maptab.h"

struct PendingMap {
    ID3D11Resource      * volatile res;   /* claim key; NULL = free */
    ID3D11DeviceContext *ctx;             /* NULL while half-claimed */
    void                *data;
    int                  slot;
};
static struct PendingMap g_map_tab[MVP_MAPTAB_SIZE];

static volatile LONG g_diag_map_overflow = 0;   /* should now stay 0 */
/* The triplet that says which failure we are in, if we are still in one. */
static volatile LONG g_diag_dyn_map_seen = 0;    /* Maps on a registered dyn cb0 buffer */
static volatile LONG g_diag_dyn_unmap_seen = 0;  /* Unmaps on one */
static volatile LONG g_diag_dyn_unmap_nomap = 0; /* ... that had no in-flight map recorded */


static Map_t   g_map_orig = NULL;
static Unmap_t g_unmap_orig = NULL;

static HRESULT STDMETHODCALLTYPE Hook_Map(ID3D11DeviceContext *ctx, ID3D11Resource *res,
                                          UINT sub, D3D11_MAP type, UINT flags,
                                          D3D11_MAPPED_SUBRESOURCE *mapped) {
    Map_t orig = g_map_orig;
    HRESULT hr;

    if (orig == NULL) return E_FAIL;
    hr = orig(ctx, res, sub, type, flags, mapped);

    /* Only subresource 0 of a buffer already in the dynamic pool is of
     * interest, and only when the caller actually got a pointer. Everything
     * else - textures, staging reads, unregistered buffers - falls straight
     * through having cost one comparison. */
    if (SUCCEEDED(hr) && sub == 0 && mapped && mapped->pData && res && ctx) {
        UINT bw = 0;
        int slot = mvp_dyn_pool_find_any((ID3D11Buffer *)res, &bw);
        if (slot >= 0) {
            unsigned h = mvp_map_hash(ctx, res);
            int p, placed = 0;
            InterlockedIncrement(&g_diag_dyn_map_seen);
            for (p = 0; p < MVP_MAPTAB_PROBE; p++) {
                struct PendingMap *e = &g_map_tab[(h + (unsigned)p) & MVP_MAPTAB_MASK];
                /* Pointer-width CAS: this is a 64-bit process, so claiming the
                 * slot with the 32-bit InterlockedCompareExchange would
                 * truncate the resource pointer and match the wrong buffer at
                 * Unmap. */
                if (InterlockedCompareExchangePointer((void * volatile *)&e->res,
                                                      res, NULL) == NULL) {
                    /* Claimed. ctx is still NULL here, so a prober looking for
                     * some other (ctx,res) cannot mistake this for a hit. */
                    e->data = mapped->pData;
                    e->slot = slot;
                    e->ctx  = ctx;          /* publishes the entry LAST */
                    placed = 1;
                    break;
                }
            }
            if (!placed) InterlockedIncrement(&g_diag_map_overflow);
        }
    }
    return hr;
}

static void STDMETHODCALLTYPE Hook_Unmap(ID3D11DeviceContext *ctx, ID3D11Resource *res, UINT sub) {
    Unmap_t orig = g_unmap_orig;
    int i;

    /* Copy BEFORE forwarding: once the real Unmap returns, the pointer the
     * game wrote into is no longer ours to read. */
    if (sub == 0 && res && ctx) {
        unsigned h = mvp_map_hash(ctx, res);
        int found = 0;
        for (i = 0; i < MVP_MAPTAB_PROBE; i++) {
            struct PendingMap *e = &g_map_tab[(h + (unsigned)i) & MVP_MAPTAB_MASK];
            /* BOTH halves, or a half-claimed entry (ctx==NULL) could match. */
            if (e->res == res && e->ctx == ctx) {
                int slot = e->slot;
                void *src = e->data;
                UINT cap = g_direct_pool[slot].byte_width;
                if (cap > MVP_CB_BYTES) cap = MVP_CB_BYTES;
                if (src && cap) {
                    mvp_shadow_note_writer(slot);
                    /* Same seqlock write side as the UpdateSubresource path,
                     * including the concurrency detection - see there for why
                     * an even result means two writers are interleaving.
                     * ⚠️ NOTE this fixes the PAIRING only. The shadow is still
                     * one window PER BUFFER, so two deferred contexts writing
                     * the same buffer in the same instant still cannot both be
                     * represented - that is the pre-existing known risk logged
                     * above MVP_DYN_POOL_MAX, and g_diag_shadow_concurrent is
                     * still its readout. Fixing the pairing is what lets that
                     * counter finally mean something: until now it could not
                     * fire, because no write ever reached here at all. */
                    if ((InterlockedIncrement(&g_shadow_seq[slot]) & 1) == 0) {
                        InterlockedIncrement(&g_diag_shadow_concurrent);
                    }
                    memcpy(g_shadow[slot], src, cap);
                    InterlockedExchange(&g_shadow_valid[slot], 1);
                    InterlockedIncrement(&g_shadow_seq[slot]);
                    InterlockedIncrement(&g_dyn_shadow_writes);
                }
                /* Free in the reverse order of the claim: scrub the payload
                 * first, release the claim key last. */
                e->data = NULL;
                e->ctx  = NULL;
                InterlockedExchangePointer((void * volatile *)&e->res, NULL);
                InterlockedIncrement(&g_diag_dyn_unmap_seen);
                found = 1;
                break;
            }
        }
        /* Only worth the pool scan when the table missed. Distinguishes "our
         * Unmap hook never sees these buffers" from "it does, but the Map side
         * never recorded them" - two different bugs with the same symptom. */
        if (!found && mvp_dyn_pool_find_any((ID3D11Buffer *)res, NULL) >= 0) {
            InterlockedIncrement(&g_diag_dyn_unmap_seen);
            InterlockedIncrement(&g_diag_dyn_unmap_nomap);
        }
    }
    if (orig) orig(ctx, res, sub);
}

/* ================= the per-draw patch itself ================= */

/* Shared core for Hook_DrawIndexed/Hook_Draw. On success, patches VS slot 0
 * to a scratch buffer holding the K-rotated MVP (with everything else
 * about cb0 unchanged), fills `*out_orig_buf` with the buffer the caller
 * must rebind to slot 0 AFTER calling the original draw, and returns 1. On
 * ANY failure or "not safely patchable" condition, does nothing and
 * returns 0 - the caller then just calls the original draw unmodified.
 * Never blocks, retries, or delays the draw itself; never partially
 * applies a patch (VSSetConstantBuffers to the scratch buffer is the LAST
 * thing this function does, only after every prior step already
 * succeeded).
 *
 * Task 6 fix round 3: every exit point below increments exactly one
 * diagnostic counter (see the block above hook_one()) - purely for
 * observability, no behaviour change. mvp_diag_maybe_report() is called
 * once, unconditionally, near the top, so it runs regardless of which exit
 * this particular call takes. */
static int mvp_patch_prepare(ID3D11DeviceContext *ctx, ID3D11Buffer **out_orig_buf,
                             UINT draw_count, int draw_indexed) {
    ID3D11VertexShader *vs = NULL;
    const void *vs_key = NULL; /* pointer VALUE only, kept for the sampled miss path's hash lookup */
    ID3D11Buffer *buf = NULL;
    int offs[4];
    int shadow_slot;
    UINT byte_width;
    unsigned char local_copy[MVP_CB_BYTES];
    UINT copy_bytes;
    int scratch_index;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    Mat4 mvp, patched;
    int row;

    *out_orig_buf = NULL;

    mvp_diag_maybe_report();

    if (!g_installed) {
        InterlockedIncrement(&g_diag_not_installed);
        return 0;
    }

    ID3D11DeviceContext_VSGetShader(ctx, &vs, NULL, NULL);
    if (vs == NULL) {
        InterlockedIncrement(&g_diag_no_vs);
        return 0;
    }
    if (!mvp_row_offsets_for_shader(vs, offs)) {
        /* Diagnostic-only second lookup, only on this (already-failing)
         * path, to classify WHY - see mvptable.h's MvpShaderStatus. */
        switch (mvp_shader_status_for_shader(vs)) {
            case MVP_SHADER_NO_MVP:
                InterlockedIncrement(&g_diag_shader_no_mvp);
                break;
            case MVP_SHADER_ROWS_INCOMPLETE:
                InterlockedIncrement(&g_diag_shader_rows_incomplete);
                break;
            case MVP_SHADER_UNKNOWN:
            case MVP_SHADER_OK: /* shouldn't happen here (mvp_row_offsets_for_shader
                                    would have succeeded), but count it as
                                    "unknown" rather than silently drop it */
            default:
                InterlockedIncrement(&g_diag_shader_unknown);
                break;
        }
        ID3D11VertexShader_Release(vs);
        return 0; /* unknown shader, or an incomplete set of rows we refuse to guess at */
    }
    vs_key = vs;
    ID3D11VertexShader_Release(vs);

    ID3D11DeviceContext_VSGetConstantBuffers(ctx, 0, 1, &buf);
    if (buf == NULL) {
        InterlockedIncrement(&g_diag_no_slot0_buf);
        return 0; /* nothing bound at slot 0 */
    }

    shadow_slot = mvp_direct_pool_find(buf, &byte_width);
    if (shadow_slot < 0) {
        /* Not one of the persistently-mapped world cb0 buffers (e.g. a
         * small immediate-context cb0 - this mechanism intentionally does
         * not cover those, only the deferred world draws). Fail-safe skip,
         * same posture as an unknown shader. THE key counter for this
         * round's mystery: if this is the dominant skip reason during real
         * gameplay, the shader IS known/patchable but its bound buffer is
         * something other than what CreateBuffer captured - logged once,
         * in full, the first time it happens, so a re-round-trip isn't
         * needed to see what that buffer actually looks like. */
        UINT dyn_bw = 0;
        UINT tick;
        int dyn_slot = mvp_dyn_pool_find(buf, &dyn_bw);
        if (dyn_slot >= 0) {
            /* 2026-09-04c: a per-shader DYNAMIC cb0 whose Map/Unmap writes we
             * shadow. From here the path is IDENTICAL to the DEFAULT pool's -
             * the bounds check, the seqlocked read, the K multiply, the scratch
             * write and the rebind all follow below unchanged. */
            shadow_slot = dyn_slot;
            byte_width = dyn_bw;
            InterlockedIncrement(&g_diag_dyn_patched);
            goto have_shadow;
        }
        InterlockedIncrement(&g_diag_pool_miss);
        tick = (UINT)InterlockedIncrement(&g_miss_sample_tick);
        if ((tick & MVP_REGISTER_PROBE_MASK) == 0) {
            /* Fix round 4b: this is where world cb0 buffers get into the
             * pool - see mvp_try_register_bound_buffer(). */
            mvp_try_register_bound_buffer(buf);
            /* 2026-09-04c: and the per-shader DYNAMIC ones, which the DEFAULT
             * filter deliberately rejects. Same "register from the draw path,
             * never from the descriptor alone" rule. */
            mvp_try_register_bound_dynamic(buf);
        }
        if ((tick & MVP_MISS_SAMPLE_MASK) == 0) {
            /* fix round 4: what IS this buffer, and did our hook see it
             * created? 2026-09-04: and what is the DRAW - how big, which
             * shader - so the missed path can be told from world geometry.
             * The hash lookup is by pointer value in shaderdump's table,
             * which is exactly how mvp_row_offsets_for_shader() found this
             * shader a moment ago; the object itself is not touched. */
            mvp_miss_sample(buf, shaderdump_hash_for_shader(vs_key), draw_count, draw_indexed);
        }
        if (InterlockedCompareExchange(&g_pool_miss_first_logged, 1, 0) == 0) {
            D3D11_BUFFER_DESC bd;
            memset(&bd, 0, sizeof(bd));
            ID3D11Buffer_GetDesc(buf, &bd);
            log_msg("mvp_patch: DIAG first pool-miss: a KNOWN/patchable shader's bound slot0 "
                     "buf=%p is not (yet) in the world cb0 pool (pool currently holds %d "
                     "identities). That buffer's real desc: ByteWidth=%u Usage=%d BindFlags=0x%X "
                     "CPUAccessFlags=0x%X MiscFlags=0x%X - to be registered it must be "
                     "CONSTANT_BUFFER, Usage=DEFAULT(0), CPUAccessFlags=0 and %d-%d bytes "
                     "(checked from the draw path by mvp_try_register_bound_buffer, NOT at "
                     "CreateBuffer time). A miss here is expected and harmless for the small "
                     "per-shader cb0s (64/96/128/160/176/224/272B) this mechanism does not cover",
                     (void *)buf, g_direct_pool_count, bd.ByteWidth, (int)bd.Usage, bd.BindFlags,
                     bd.CPUAccessFlags, bd.MiscFlags, MVP_POOL_BUF_MIN_BYTES, MVP_POOL_BUF_MAX_BYTES);
        }
        ID3D11Buffer_Release(buf);
        return 0;
    }

have_shadow:
    /* Important finding #4 fix: bound-check the shader's reflected row
     * offsets against the REAL bound buffer's own size (captured at
     * CreateBuffer time), not just our fixed MVP_CB_BYTES window - a
     * shader/buffer size mismatch must fall through unpatched, never
     * silently read/write past the real buffer (which would previously
     * have applied a confidently-wrong, zero-filled "patch" instead of
     * skipping - the one path that violated the fail-safe requirement). */
    {
        /* 2026-09-03: check EVERY row, not just offs[3].
         *
         * This used to test `offs[0] < 0 || offs[3] + 16 > limit`, which was
         * correct only while row offsets were guaranteed contiguous and
         * ascending, so that offs[3] was necessarily the highest. Now that
         * mvp_row_offsets_for_shader() hands out the offsets reflection
         * actually reported, that no longer holds - the dominant
         * non-contiguous layout in this game is mvpmatrixz and mvpmatrixw
         * TRANSPOSED, i.e. {b, b+16, b+48, b+32}, where offs[2] is the highest
         * and offs[3] is not. Testing only offs[3] would have let a row run
         * past the end of the bound buffer while the check reported success -
         * exactly the silent out-of-bounds write the original check existed to
         * prevent. */
        int r, worst_end = 0, bad = 0;
        for (r = 0; r < 4; r++) {
            int end = offs[r] + 16;
            if (offs[r] < 0 || (UINT)end > byte_width || (UINT)end > MVP_CB_BYTES) {
                bad = 1;
            }
            if (end > worst_end) {
                worst_end = end;
            }
        }
        if (bad) {
            InterlockedIncrement(&g_diag_bounds_fail);
            ID3D11Buffer_Release(buf);
            if (mvp_rate_limit_should_fire(&g_offset_oob_log_count)) {
                log_msg("mvp_patch: shader's reflected mvp row offsets (%d,%d,%d,%d; furthest row "
                         "ends at byte %d) exceed the bound buffer's real size (%u bytes) or our "
                         "%d-byte window; draw falls through unpatched (fail-safe)",
                         offs[0], offs[1], offs[2], offs[3], worst_end, byte_width, MVP_CB_BYTES);
            }
            return 0;
        }
    }

    /* Fix round 5 (minor): copy and forward only the bound buffer's real
     * size, not the whole MVP_CB_BYTES window. The shader cannot legally
     * read past the bound buffer's own extent, and the bounds check above
     * has already guaranteed every mvp row lies inside copy_bytes, so the
     * tail was pure waste on a path that runs ~110k times/second. The tail
     * zero-fill went with it for the same reason. */
    copy_bytes = (byte_width < MVP_CB_BYTES) ? byte_width : MVP_CB_BYTES;
    if (!mvp_shadow_read(shadow_slot, local_copy, copy_bytes)) {
        /* A writer touched this shadow during our copy - the snapshot may be
         * torn, so skip rather than apply it (fix round 5, finding #4). */
        InterlockedIncrement(&g_diag_shadow_torn);
        ID3D11Buffer_Release(buf);
        return 0;
    }

    /* This call's VSGetConstantBuffers() reference is always redundant now
     * - the world cb0 pool holds its OWN separate reference, taken once at
     * registration time, independent of any specific draw's binding. */
    ID3D11Buffer_Release(buf);

    for (row = 0; row < 4; row++) {
        const float *f = (const float *)(local_copy + offs[row]);
        mvp.m[row][0] = f[0];
        mvp.m[row][1] = f[1];
        mvp.m[row][2] = f[2];
        mvp.m[row][3] = f[3];
    }
    patched = mat4_mul(g_K, mvp);
    for (row = 0; row < 4; row++) {
        float *f = (float *)(local_copy + offs[row]);
        f[0] = patched.m[row][0];
        f[1] = patched.m[row][1];
        f[2] = patched.m[row][2];
        f[3] = patched.m[row][3];
    }

    if (!g_scratch_ready) {
        /* Double-checked locking (Important finding #6): without
         * g_scratch_init_cs here, two threads racing the first patchable
         * draw could both observe g_scratch_ready==0 and both create a
         * full MVP_SCRATCH_TOTAL-buffer set, leaking one set (the loser's
         * g_scratch[] writes would be silently overwritten by the
         * winner's, with no Release ever reaching the loser's buffers). */
        EnterCriticalSection(&g_scratch_init_cs);
        if (!g_scratch_ready) {
            ID3D11Device *dev = NULL;
            ID3D11DeviceContext_GetDevice(ctx, &dev);
            if (dev != NULL) {
                if (ensure_scratch_ready(dev)) {
                    InterlockedExchange(&g_scratch_ready, 1);
                    log_msg("mvp_patch: scratch pool ready (%d buffers, %dB each)",
                             MVP_SCRATCH_TOTAL, MVP_CB_BYTES);
                }
                ID3D11Device_Release(dev);
            }
        }
        LeaveCriticalSection(&g_scratch_init_cs);
        if (!g_scratch_ready) {
            InterlockedIncrement(&g_diag_scratch_not_ready);
            return 0;
        }
    }

    scratch_index = mvp_alloc_scratch_index();
    if (scratch_index < 0 || g_scratch[scratch_index] == NULL) {
        InterlockedIncrement(&g_diag_scratch_alloc_fail);
        return 0;
    }

    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_scratch[scratch_index], 0,
                                 D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr) || mapped.pData == NULL) {
        InterlockedIncrement(&g_diag_scratch_map_fail);
        if (mvp_rate_limit_should_fire(&g_map_fail_log_count)) {
            log_msg("mvp_patch: scratch Map(WRITE_DISCARD) failed (hr=0x%08lX); draw falls through unpatched",
                     (unsigned long)hr);
        }
        return 0;
    }
    memcpy(mapped.pData, local_copy, copy_bytes); /* see copy_bytes note above */
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_scratch[scratch_index], 0);

    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_scratch[scratch_index]);

    *out_orig_buf = buf; /* identity only, for the post-draw rebind - the direct pool (and the
                             engine's own binding) keep the underlying object alive independently */
    InterlockedIncrement(&g_diag_patched);
    return 1;
}

/* ================= detours ================= */

static void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext *ctx, UINT IndexCount,
                                                UINT StartIndexLocation, INT BaseVertexLocation) {
    DrawIndexed_t orig = g_drawindexed_orig;
    ID3D11Buffer *orig_buf = NULL;
    int patched;

    if (orig == NULL) {
        return; /* should never happen once installed */
    }

    /* Fix round 4: DrawIndexed/Draw share one code address across every
     * context flavor, so they are the only place a deferred context is
     * guaranteed to reach us - use that to late-hook ITS vtable's
     * UpdateSubresource. Cheap after the first call per context. */
    mvp_maybe_late_hook_ctx(ctx);

    patched = mvp_patch_prepare(ctx, &orig_buf, IndexCount, 1);

    orig(ctx, IndexCount, StartIndexLocation, BaseVertexLocation);

    if (patched) {
        ID3D11Buffer *restore = orig_buf;
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &restore);
    }
}

static void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext *ctx, UINT VertexCount,
                                         UINT StartVertexLocation) {
    Draw_t orig = g_draw_orig;
    ID3D11Buffer *orig_buf = NULL;
    int patched;

    if (orig == NULL) {
        return;
    }

    mvp_maybe_late_hook_ctx(ctx); /* see Hook_DrawIndexed */

    patched = mvp_patch_prepare(ctx, &orig_buf, VertexCount, 0);

    orig(ctx, VertexCount, StartVertexLocation);

    if (patched) {
        ID3D11Buffer *restore = orig_buf;
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &restore);
    }
}

static HRESULT STDMETHODCALLTYPE Hook_CreateBuffer(ID3D11Device *dev, const D3D11_BUFFER_DESC *pDesc,
                                                    const D3D11_SUBRESOURCE_DATA *pInitialData,
                                                    ID3D11Buffer **ppBuffer) {
    CreateBuffer_t orig = g_createbuffer_orig;
    HRESULT hr;

    if (orig == NULL) {
        return E_FAIL; /* should never happen once installed - see hook_one()'s ordering fix */
    }

    hr = orig(dev, pDesc, pInitialData, ppBuffer);

    InterlockedIncrement(&g_cb_create_total);
    if (SUCCEEDED(hr) && ppBuffer != NULL && *ppBuffer != NULL && pDesc != NULL &&
        (pDesc->BindFlags & D3D11_BIND_CONSTANT_BUFFER)) {
        /* fix round 4 diagnostic: remember EVERY constant buffer our hook
         * sees created, regardless of whether the capture filter accepts
         * it, so a later pool-miss can tell "filter rejected it" apart from
         * "our hook never saw it at all". */
        InterlockedIncrement(&g_cb_create_const);
        mvp_seen_cb_record(*ppBuffer, pDesc);
    }

    /* FIX ROUND 4b: this hook NO LONGER registers anything into the world
     * cb0 pool. Descriptor-only classification at creation time could not
     * distinguish the real pool from same-shaped decoys (it failed that way
     * in rounds 1, 2 and 4a), so registration moved to the draw path, where
     * "bound at VS slot 0 for a draw with a usable mvpmatrix" is available
     * as the actual criterion - see mvp_try_register_bound_buffer(). What
     * remains here is purely the diagnostic bookkeeping above, which is
     * what proved the hook's coverage is complete. */
    return hr;
}

/* ================= public API ================= */

void mvp_patch_install(ID3D11Device *dummy_dev, ID3D11DeviceContext *dummy_ctx) {
    void **ctx_vtbl;
    void **dev_vtbl;
    int ok_di, ok_d, ok_cb, ok_us;
    char flagbuf[32];
    DWORD len;
    float deg;

    if (g_installed) {
        return;
    }

    if (dummy_dev == NULL || dummy_ctx == NULL) {
        log_msg("mvp_patch: install called with a NULL dummy device/context; per-draw MVP override DISABLED this session");
        return;
    }

    if (!mh_glue_init()) {
        log_msg("mvp_patch: MinHook init failed; per-draw MVP override DISABLED this session");
        return;
    }

    InitializeCriticalSection(&g_direct_pool_cs);
    g_direct_pool_cs_ready = 1;
    InitializeCriticalSection(&g_scratch_init_cs);
    g_scratch_init_cs_ready = 1;
    InitializeCriticalSection(&g_latehook_cs);
    g_latehook_cs_ready = 1;

    g_tls_index = TlsAlloc();
    if (g_tls_index == TLS_OUT_OF_INDEXES) {
        log_msg("mvp_patch: TlsAlloc failed (gle=%lu); per-draw MVP override DISABLED this session",
                 (unsigned long)GetLastError());
        DeleteCriticalSection(&g_direct_pool_cs);
        g_direct_pool_cs_ready = 0;
        DeleteCriticalSection(&g_scratch_init_cs);
        g_scratch_init_cs_ready = 0;
        DeleteCriticalSection(&g_latehook_cs);
        g_latehook_cs_ready = 0;
        return;
    }

    ctx_vtbl = *(void ***)dummy_ctx;
    dev_vtbl = *(void ***)dummy_dev;

    ok_di = hook_one(ctx_vtbl[VTBL_CTX_DRAWINDEXED], (void *)&Hook_DrawIndexed, (void **)&g_drawindexed_orig,
                      "mvp_patch ID3D11DeviceContext::DrawIndexed");
    ok_d = hook_one(ctx_vtbl[VTBL_CTX_DRAW], (void *)&Hook_Draw, (void **)&g_draw_orig,
                     "mvp_patch ID3D11DeviceContext::Draw");
    ok_cb = hook_one(dev_vtbl[VTBL_DEV_CREATEBUFFER], (void *)&Hook_CreateBuffer, (void **)&g_createbuffer_orig,
                      "mvp_patch ID3D11Device::CreateBuffer");

    /* 2026-09-04c: Map/Unmap, the shadow source for the per-shader DYNAMIC cb0
     * pool. Vtable slots 14/15, read from the SDK header's own
     * ID3D11DeviceContextVtbl rather than assumed. Not gated below: if these
     * fail the DEFAULT path is unaffected and the dynamic pool simply never
     * becomes valid, which its own counters report. */
    if (!hook_one(ctx_vtbl[VTBL_CTX_MAP], (void *)&Hook_Map, (void **)&g_map_orig,
                  "mvp_patch ID3D11DeviceContext::Map") ||
        !hook_one(ctx_vtbl[VTBL_CTX_UNMAP], (void *)&Hook_Unmap, (void **)&g_unmap_orig,
                  "mvp_patch ID3D11DeviceContext::Unmap")) {
        log_msg("mvp_patch: Map/Unmap hook failed - the per-shader DYNAMIC cb0 path is INERT this "
                "run (those draws render unpatched, exactly as before 2026-09-04c). The DEFAULT "
                "world-buffer path is unaffected.");
    }

    /* Fix round 4: the immediate/dummy flavor's UpdateSubresource. Deferred
     * contexts get theirs late-hooked on their first draw (see
     * mvp_maybe_late_hook_ctx) - this one covers the immediate context and
     * any deferred flavor that happens to share its address. */
    EnterCriticalSection(&g_latehook_cs);
    ok_us = mvp_hook_update_target_locked(ctx_vtbl[VTBL_CTX_UPDATESUBRESOURCE]);
    LeaveCriticalSection(&g_latehook_cs);

    /* FIX ROUND 5, finding #2: this gate's polarity used to be backwards.
     * It required ok_cb and ignored ok_us - but after round 4b,
     * Hook_CreateBuffer registers NOTHING (it is pure diagnostics), while
     * UpdateSubresource is the mechanism's ACTUAL hard dependency: without
     * it no shadow ever becomes valid, so no draw is ever patchable and the
     * module would log "installed" and then sit silently inert forever -
     * precisely the failure mode this gate exists to make loud.
     *
     * Hard requirements now: at least one draw hook (nothing to patch
     * without one) AND UpdateSubresource (no live content without it).
     * CreateBuffer is downgraded to a soft warning - losing it costs only
     * the coverage diagnostics, not the mechanism. */
    if (!ok_cb) {
        log_msg("mvp_patch: WARNING - could not hook ID3D11Device::CreateBuffer. The per-draw MVP "
                 "override does NOT depend on it (buffers are registered from the draw path since "
                 "fix round 4b); only the CreateBuffer-coverage diagnostics are lost. Continuing.");
    }

    if ((!ok_di && !ok_d) || !ok_us) {
        log_msg("mvp_patch: failed to hook enough of {DrawIndexed=%d Draw=%d UpdateSubresource=%d} "
                 "(CreateBuffer=%d, not required); per-draw MVP override DISABLED this session",
                 ok_di, ok_d, ok_us, ok_cb);
        TlsFree(g_tls_index);
        g_tls_index = TLS_OUT_OF_INDEXES;
        DeleteCriticalSection(&g_direct_pool_cs);
        g_direct_pool_cs_ready = 0;
        DeleteCriticalSection(&g_scratch_init_cs);
        g_scratch_init_cs_ready = 0;
        DeleteCriticalSection(&g_latehook_cs);
        g_latehook_cs_ready = 0;
        return;
    }

    /* TEWVR_TEST_YAW: read once here, synchronously, on the bootstrap
     * thread - strictly before the hooks just enabled above can possibly
     * be reached by a real draw. */
    len = tewvr_getenv("TEWVR_TEST_YAW", flagbuf, sizeof(flagbuf));
    deg = (len > 0 && len < sizeof(flagbuf)) ? (float)atof(flagbuf) : 0.0f;
    if (deg != 0.0f) {
        g_K = mat4_rotation_y(deg);
        log_msg("mvp_patch: TEWVR_TEST_YAW=%.3f -> test rotation K ACTIVE (every patchable draw rotated)",
                 (double)deg);
    } else {
        g_K = mat4_identity();
        log_msg("mvp_patch: TEWVR_TEST_YAW unset/0 (neither the environment nor tewvr.ini set it) -> K = identity "
                 "(read/patch/rebind mechanism still exercised every patchable draw; no visible change expected)");
    }

    g_installed = 1;
    log_msg("mvp_patch: installed (DrawIndexed=%d Draw=%d UpdateSubresource=%d; CreateBuffer=%d, "
             "diagnostics only); world cb0 buffers (%d-%d bytes, DEFAULT/no-CPU-access) are "
             "registered FROM THE DRAW PATH the first time one is seen bound at VS slot 0 for a "
             "draw with a usable mvpmatrix, and their content is shadowed on every "
             "UpdateSubresource; deferred contexts get their own UpdateSubresource late-hooked on "
             "their first draw; scratch pool created lazily on the first patchable draw",
             ok_di, ok_d, ok_us, ok_cb, MVP_POOL_BUF_MIN_BYTES, MVP_POOL_BUF_MAX_BYTES);
}

/* ORDERING REQUIREMENT (fix round 5, minor): mh_glue_shutdown() MUST have
 * disabled this module's hooks before this runs. Nothing here is safe
 * against a concurrently-live detour - it releases the pool's buffer
 * references and deletes the critical sections a live Hook_UpdateSubresource
 * or Hook_DrawIndexed would still be touching. hooks_remove() already calls
 * mh_glue_shutdown() first, and documents why; this comment records the
 * dependency on this side too, since it was previously implicit. */
void mvp_patch_remove(void) {
    int i;

    /* 2026-09-04: the bucketed miss table one last time, so a session's
     * whole picture is in the log however the periodic 30-second cadence
     * happened to line up with the quit. Runs before any trampoline is
     * torn down (hooks_remove() calls mh_glue_shutdown() first, and this
     * after it), so no draw can be mid-sample here. */
    if (g_miss_combo_count > 0) {
        log_msg("mvp_patch: DIAG final bucketed pool-miss table at shutdown (cumulative, 1-in-%d sampled):",
                MVP_MISS_SAMPLE_MASK + 1);
        mvp_diag_report_misses();
    }

    if (g_direct_pool_cs_ready) {
        EnterCriticalSection(&g_direct_pool_cs);
        /* The DYNAMIC pool holds its own AddRef per slot, exactly like the
         * DEFAULT one, and lives in the upper partition of the same array. */
        for (i = 0; i < g_dyn_pool_count; i++) {
            int slot = MVP_DIRECT_POOL_MAX + i;
            if (g_direct_pool[slot].buf != NULL) {
                ID3D11Buffer_Release(g_direct_pool[slot].buf);
                g_direct_pool[slot].buf = NULL;
                InterlockedExchange(&g_shadow_valid[slot], 0);
            }
        }
        g_dyn_pool_count = 0;
        for (i = 0; i < g_direct_pool_count; i++) {
            if (g_direct_pool[i].buf != NULL) {
                /* Fix round 4: nothing to Unmap any more - this module no
                 * longer Maps the engine's buffers at all, it shadows their
                 * UpdateSubresource writes into its own memory. */
                ID3D11Buffer_Release(g_direct_pool[i].buf);
                g_direct_pool[i].buf = NULL;
                InterlockedExchange(&g_shadow_valid[i], 0);
            }
        }
        g_direct_pool_count = 0;
        LeaveCriticalSection(&g_direct_pool_cs);
        DeleteCriticalSection(&g_direct_pool_cs);
        g_direct_pool_cs_ready = 0;
    }

    if (g_scratch_ready) {
        for (i = 0; i < MVP_SCRATCH_TOTAL; i++) {
            if (g_scratch[i] != NULL) {
                ID3D11Buffer_Release(g_scratch[i]);
                g_scratch[i] = NULL;
            }
        }
        InterlockedExchange(&g_scratch_ready, 0);
    }
    if (g_scratch_init_cs_ready) {
        DeleteCriticalSection(&g_scratch_init_cs);
        g_scratch_init_cs_ready = 0;
    }
    if (g_latehook_cs_ready) {
        /* Cleared before the CS goes away so no in-flight detour can start
         * a late-hook pass against a deleted critical section. */
        g_latehook_cs_ready = 0;
        DeleteCriticalSection(&g_latehook_cs);
    }
    g_update_target_count = 0;
    g_seen_ctx_count = 0;
    g_seen_ctx_full_warned = 0;

    if (g_tls_index != TLS_OUT_OF_INDEXES) {
        TlsFree(g_tls_index);
        g_tls_index = TLS_OUT_OF_INDEXES;
    }

    g_drawindexed_orig = NULL;
    g_draw_orig = NULL;
    g_createbuffer_orig = NULL;
    g_hooked_count = 0;
    g_direct_pool_full_warned = 0;
    g_installed = 0;
}
