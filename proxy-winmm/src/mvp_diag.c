/* Split out of mvp_patch.c on 2026-09-30 (move-only; see mvp_patch.c for the
 * module overview). Diagnostics: skip-reason counters, the rate limiter, CreateBuffer
 * coverage records, the pool-miss sampler and the periodic DIAG report. */

#include "mvp_patch_internal.h"

/* ---- rate limiter (same burst-then-every-Nth scheme as seqdump.c's
 * seq_rate_limit_should_fire() - copied locally rather than shared, since
 * that one is private to seqdump.c and this module must stay independent
 * of whether TEWVR_SEQDUMP is even active). ---- */
#define MVP_RATE_LIMIT_BURST 10
#define MVP_RATE_LIMIT_EVERY_NTH 500
int mvp_rate_limit_should_fire(volatile LONG *counter) {
    LONG n = InterlockedIncrement(counter);
    if (n <= MVP_RATE_LIMIT_BURST) {
        return 1;
    }
    return (n % MVP_RATE_LIMIT_EVERY_NTH) == 0;
}
volatile LONG g_map_fail_log_count = 0;
volatile LONG g_offset_oob_log_count = 0;
volatile LONG g_update_bug_log_count = 0;

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

volatile LONG g_diag_not_installed = 0;      /* exit: !g_installed (should be ~0 during real play) */
volatile LONG g_diag_no_vs = 0;               /* exit: VSGetShader returned NULL */
volatile LONG g_diag_shader_unknown = 0;      /* exit: shader untracked by mvptable at all */
volatile LONG g_diag_shader_no_mvp = 0;       /* exit: shader known, has no mvpmatrix at all (expected for post/depth-only shaders) */
volatile LONG g_diag_shader_rows_incomplete = 0; /* exit: shader known, has mvpx, but not all four mvpmatrix rows were found */
volatile LONG g_diag_no_slot0_buf = 0;        /* exit: nothing bound at VS slot 0 */
volatile LONG g_diag_pool_miss = 0;           /* exit: bound slot0 buffer is not in the direct-map pool - THE key counter for this round's mystery */
volatile LONG g_diag_bounds_fail = 0;         /* exit: Important #4's bounds check failed (also has its own immediate rate-limited log) */
volatile LONG g_diag_scratch_not_ready = 0;   /* exit: lazy scratch-pool init not (yet) successful */
volatile LONG g_diag_scratch_alloc_fail = 0;  /* exit: per-thread scratch index/buffer unavailable */
volatile LONG g_diag_scratch_map_fail = 0;    /* exit: scratch Map(WRITE_DISCARD) failed (also has its own immediate rate-limited log) */
volatile LONG g_diag_patched = 0;             /* success: a draw was actually patched */

volatile LONG g_pool_miss_first_logged = 0; /* sticky one-shot gate for the detailed first-miss log */
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
volatile LONG g_miss_combo_count = 0;
volatile LONG g_miss_sample_tick = 0;

volatile LONG g_cb_create_total = 0; /* every CreateBuffer call our hook saw */
volatile LONG g_cb_create_const = 0; /* ...of which were constant buffers */

/* Lock-free append-only record of one created constant buffer. Overflow is
 * silently ignored (bounded diagnostic, never an error path). */
void mvp_seen_cb_record(ID3D11Buffer *buf, const D3D11_BUFFER_DESC *d) {
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

void mvp_miss_sample(ID3D11Buffer *buf, uint64_t vs_hash, UINT count, int indexed) {
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
void mvp_diag_report_misses(void) {
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
void mvp_diag_maybe_report(void) {
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
