/* map_pairing_test.c - does the 2026-09-05 (context, resource) pairing table
 * actually survive TEW's renderer, without the game?
 *
 * Builds and runs on any host:
 *     gcc -O2 -Wall -Wextra -o map_pairing_test tools/map_pairing_test.c && ./map_pairing_test
 *
 * It includes src/mvp_maptab.h, so it tests THE SHIPPED HASH, not a copy.
 *
 * What it is testing, and why each one matters:
 *
 *  1. The failure that actually happened. Replay the OLD design - a 32-entry
 *     table keyed on `res` alone - against the measured 2026-09-04d workload
 *     and confirm it saturates. If this does not overflow, the diagnosis in
 *     the write-up is wrong and the fix is aimed at nothing.
 *
 *  2. The same workload through the NEW table must place every map with room
 *     to spare, and must never exceed the probe budget.
 *
 *  3. Concurrent maps OF THE SAME BUFFER from different contexts must land in
 *     different buckets. This is the specific collision the old key could not
 *     express, and it is the reason the key is a pair.
 *
 *  4. Pairing must be exact: every Unmap finds its own map, gets back its own
 *     pData, and frees exactly one entry - never another context's.
 *
 *  5. Sustained churn: many map/unmap rounds must not leak entries. The old
 *     table's real sin was filling and never draining.
 *
 * No D3D11, no threads: the table's thread-safety rests on the CAS ordering
 * argued in mvp_patch.c, which a single-threaded test cannot check. What this
 * CAN check is the part that was actually wrong - the key and the geometry.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../src/mvp_maptab.h"

static int g_fail = 0;
static int g_checks = 0;

static void check(int cond, const char *what) {
    g_checks++;
    if (!cond) { printf("  FAIL: %s\n", what); g_fail++; }
}

/* ---- the measured 2026-09-04d workload ----------------------------------
 * 54 of 64 dynamic cb0 buffers registered; the engine records world draws
 * from ~6 deferred worker contexts plus the immediate context. Pointers are
 * synthesised the way a real allocator hands them out: 16-byte aligned, in a
 * couple of clustered arenas, so the high bits are shared and the low four
 * bits are always zero. A hash that only looked strong on random inputs
 * would fail here, which is the point. */
#define NCTX 7
#define NBUF 54

static void *g_ctx[NCTX];
static void *g_buf[NBUF];

static void make_workload(void) {
    int i;
    for (i = 0; i < NCTX; i++)          /* contexts: one arena, 0x400 apart */
        g_ctx[i] = (void *)(size_t)(0x000001D4C0A00000ull + (unsigned long long)i * 0x400ull);
    for (i = 0; i < NBUF; i++)          /* buffers: another arena, 0x80 apart */
        g_buf[i] = (void *)(size_t)(0x000001D4C1200000ull + (unsigned long long)i * 0x80ull);
}

/* ---- test 1: the OLD design, replayed --------------------------------- */

#define OLD_PENDING_MAPS 32

static void test_old_design_saturates(void) {
    void *old[OLD_PENDING_MAPS];
    int overflow = 0, placed_total = 0;
    int c, b, matches = 0, i;

    printf("1. the OLD 32-entry table keyed on `res` alone, same workload\n");
    memset(old, 0, sizeof(old));

    /* BUFFER-MAJOR on purpose: each buffer is mapped by every context before
     * the next buffer starts. That is what a frame of parallel command-list
     * recording looks like, and it is the case the old key could not express.
     * (Context-major order would saturate the table before any buffer got a
     * second entry, and would hide the ambiguity behind the overflow.) */
    for (b = 0; b < NBUF; b++) {
        for (c = 0; c < NCTX; c++) {
            int put = 0;
            for (i = 0; i < OLD_PENDING_MAPS; i++) {
                if (old[i] == NULL) { old[i] = g_buf[b]; put = 1; break; }
            }
            if (put) placed_total++; else overflow++;
        }
    }
    printf("   placed=%d  overflow=%d  (of %d maps)\n",
           placed_total, overflow, NCTX * NBUF);
    check(overflow > 0, "the old table overflows on the measured workload");
    check(placed_total == OLD_PENDING_MAPS,
          "the old table fills to exactly its 32 slots and then rejects everything");

    /* The second sin, now actually demonstrated: an Unmap keyed on `res`
     * alone frees whichever entry matches first, which need not be the one
     * this context created. */
    for (i = 0; i < OLD_PENDING_MAPS; i++) if (old[i] == g_buf[0]) matches++;
    printf("   buffer[0] occupies %d old-table entries at once -> an Unmap keyed\n"
           "   on `res` alone cannot know which context's map it is freeing\n", matches);
    check(matches == NCTX,
          "one buffer really does occupy one entry per context (ambiguous free)");
    printf("\n");
}

/* ---- the NEW table, modelled exactly as mvp_patch.c uses it ------------ */

struct Ent { const void *res; const void *ctx; void *data; int slot; };
static struct Ent g_tab[MVP_MAPTAB_SIZE];

static int tab_insert(const void *ctx, const void *res, void *data, int slot) {
    unsigned h = mvp_map_hash(ctx, res);
    int p;
    for (p = 0; p < MVP_MAPTAB_PROBE; p++) {
        struct Ent *e = &g_tab[(h + (unsigned)p) & MVP_MAPTAB_MASK];
        if (e->res == NULL) {
            e->res = res; e->data = data; e->slot = slot; e->ctx = ctx;
            return p;                      /* probes used */
        }
    }
    return -1;                             /* overflow */
}

static void *tab_take(const void *ctx, const void *res, int *out_slot) {
    unsigned h = mvp_map_hash(ctx, res);
    int p;
    for (p = 0; p < MVP_MAPTAB_PROBE; p++) {
        struct Ent *e = &g_tab[(h + (unsigned)p) & MVP_MAPTAB_MASK];
        if (e->res == res && e->ctx == ctx) {
            void *d = e->data;
            if (out_slot) *out_slot = e->slot;
            e->data = NULL; e->ctx = NULL; e->res = NULL;
            return d;
        }
    }
    return NULL;
}

static int tab_occupancy(void) {
    int i, n = 0;
    for (i = 0; i < MVP_MAPTAB_SIZE; i++) if (g_tab[i].res) n++;
    return n;
}

/* ---- test 2: the same workload through the new table ------------------- */

static void test_new_table_holds(void) {
    int c, b, worst = 0, overflow = 0;

    printf("2. the NEW (context,resource) table, same workload\n");
    memset(g_tab, 0, sizeof(g_tab));

    for (c = 0; c < NCTX; c++) {
        for (b = 0; b < NBUF; b++) {
            int p = tab_insert(g_ctx[c], g_buf[b], (void *)(size_t)(0x7000 + c * 100 + b), b + 32);
            if (p < 0) overflow++; else if (p > worst) worst = p;
        }
    }
    printf("   placed=%d/%d  overflow=%d  worst probe depth=%d (budget %d)  occupancy=%d/%d\n",
           NCTX * NBUF - overflow, NCTX * NBUF, overflow, worst,
           MVP_MAPTAB_PROBE, tab_occupancy(), MVP_MAPTAB_SIZE);
    check(overflow == 0, "every map placed, no overflow");
    check(worst < MVP_MAPTAB_PROBE, "worst probe depth is inside the budget");
    check(tab_occupancy() == NCTX * NBUF, "occupancy equals the number of in-flight maps");
    printf("\n");
}

/* ---- test 3: the collision the old key could not express --------------- */

static void test_same_buffer_different_contexts(void) {
    unsigned seen[NCTX];
    int i, j, distinct = 1;

    printf("3. one buffer mapped by all %d contexts at once -> distinct buckets\n", NCTX);
    for (i = 0; i < NCTX; i++) seen[i] = mvp_map_hash(g_ctx[i], g_buf[0]);
    for (i = 0; i < NCTX && distinct; i++)
        for (j = i + 1; j < NCTX; j++)
            if (seen[i] == seen[j]) { distinct = 0; break; }

    printf("   buckets:");
    for (i = 0; i < NCTX; i++) printf(" %u", seen[i]);
    printf("\n");
    check(distinct, "the same buffer from different contexts hashes to different buckets");

    /* The converse must hold too, or the key is not really a pair. */
    check(mvp_map_hash(g_ctx[0], g_buf[0]) != mvp_map_hash(g_ctx[0], g_buf[1]),
          "different buffers on one context hash apart");
    check(mvp_map_hash(g_ctx[0], g_buf[0]) == mvp_map_hash(g_ctx[0], g_buf[0]),
          "the hash is stable for a given pair");
    printf("\n");
}

/* ---- test 4: pairing is exact ------------------------------------------ */

static void test_pairing_is_exact(void) {
    int c, b, wrong = 0, missing = 0;

    printf("4. every Unmap recovers its OWN pData and slot\n");
    memset(g_tab, 0, sizeof(g_tab));

    for (c = 0; c < NCTX; c++)
        for (b = 0; b < NBUF; b++)
            tab_insert(g_ctx[c], g_buf[b], (void *)(size_t)(0x7000 + c * 100 + b), b + 32);

    for (c = 0; c < NCTX; c++) {
        for (b = 0; b < NBUF; b++) {
            int slot = -1;
            void *want = (void *)(size_t)(0x7000 + c * 100 + b);
            void *got = tab_take(g_ctx[c], g_buf[b], &slot);
            if (!got) missing++;
            else if (got != want || slot != b + 32) wrong++;
        }
    }
    printf("   missing=%d  wrong-payload=%d  occupancy after=%d\n",
           missing, wrong, tab_occupancy());
    check(missing == 0, "no map went unfound");
    check(wrong == 0, "no Unmap collected another context's pData or slot");
    check(tab_occupancy() == 0, "the table drains completely");

    /* An Unmap for a pair that was never mapped must return nothing rather
     * than stealing a neighbour's entry. */
    tab_insert(g_ctx[0], g_buf[0], (void *)0x1234, 32);
    check(tab_take(g_ctx[1], g_buf[0], NULL) == NULL,
          "a different context's Unmap does not steal the entry");
    check(tab_take(g_ctx[0], g_buf[0], NULL) == (void *)0x1234,
          "the owning context still finds it afterwards");
    printf("\n");
}

/* ---- test 5: churn does not leak --------------------------------------- */

static void test_churn_does_not_leak(void) {
    int round, c, b, overflow = 0, leaked = 0;

    printf("5. 20000 map/unmap rounds over the whole working set\n");
    memset(g_tab, 0, sizeof(g_tab));

    for (round = 0; round < 20000; round++) {
        for (c = 0; c < NCTX; c++) {
            b = (round + c * 7) % NBUF;
            if (tab_insert(g_ctx[c], g_buf[b], (void *)(size_t)(0x9000 + round), b + 32) < 0)
                overflow++;
        }
        for (c = 0; c < NCTX; c++) {
            b = (round + c * 7) % NBUF;
            if (tab_take(g_ctx[c], g_buf[b], NULL) == NULL) leaked++;
        }
    }
    printf("   overflow=%d  unmatched=%d  occupancy after=%d\n",
           overflow, leaked, tab_occupancy());
    check(overflow == 0, "no overflow across sustained churn");
    check(leaked == 0, "every round paired");
    check(tab_occupancy() == 0, "no entry leaked after 20000 rounds");
    printf("\n");
}

int main(void) {
    printf("TEW dynamic cb0 Map/Unmap pairing - static check, no game\n");
    printf("table: %d slots, %d-probe budget\n\n", MVP_MAPTAB_SIZE, MVP_MAPTAB_PROBE);

    make_workload();
    test_old_design_saturates();
    test_new_table_holds();
    test_same_buffer_different_contexts();
    test_pairing_is_exact();
    test_churn_does_not_leak();

    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
