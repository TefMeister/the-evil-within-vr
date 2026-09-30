/* Split out of mvp_patch.c on 2026-09-30 (move-only; see mvp_patch.c for the
 * module overview). The per-shader DYNAMIC cb0 pool and its Map/Unmap shadow source,
 * including the (context, resource) in-flight map table. */

#include "mvp_patch_internal.h"

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
int g_dyn_pool_count;
volatile LONG g_diag_dyn_patched;
volatile LONG g_diag_dyn_empty;
volatile LONG g_dyn_shadow_writes;

/* Lock-free, same contract as mvp_direct_pool_find(): returns the SLOT so the
 * caller can seqlock its read, or -1. */
int mvp_dyn_pool_find(ID3D11Buffer *buf, UINT *out_byte_width) {
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
void mvp_try_register_bound_dynamic(ID3D11Buffer *buf) {
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

volatile LONG g_diag_map_overflow = 0;   /* should now stay 0 */
/* The triplet that says which failure we are in, if we are still in one. */
volatile LONG g_diag_dyn_map_seen = 0;    /* Maps on a registered dyn cb0 buffer */
volatile LONG g_diag_dyn_unmap_seen = 0;  /* Unmaps on one */
volatile LONG g_diag_dyn_unmap_nomap = 0; /* ... that had no in-flight map recorded */


Map_t   g_map_orig = NULL;
Unmap_t g_unmap_orig = NULL;

HRESULT STDMETHODCALLTYPE Hook_Map(ID3D11DeviceContext *ctx, ID3D11Resource *res,
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

void STDMETHODCALLTYPE Hook_Unmap(ID3D11DeviceContext *ctx, ID3D11Resource *res, UINT sub) {
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
