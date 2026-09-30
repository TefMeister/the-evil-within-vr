/* Split out of mvp_patch.c on 2026-09-30 (move-only; see mvp_patch.c for the
 * module overview). The cb0 shadow storage (shared by both pool partitions), the DEFAULT
 * world-buffer pool, and its UpdateSubresource shadow source with late-hooking. */

#include "mvp_patch_internal.h"

/* ---- direct persistent-pointer pool (Step 0's chosen mechanism) ---- */



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
unsigned char g_shadow[MVP_POOL_SLOTS_TOTAL][MVP_CB_BYTES];
/* Thread id of the first writer of each shadow slot (0 = none yet), and a
 * one-shot gate so the cross-thread warning cannot spam the log. */
static volatile LONG g_shadow_writer_tid[MVP_POOL_SLOTS_TOTAL];
static volatile LONG g_shadow_multiwriter_warned = 0;
volatile LONG g_diag_shadow_multiwriter = 0; /* writes seen from a second thread */

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
volatile LONG g_shadow_seq[MVP_POOL_SLOTS_TOTAL];
volatile LONG g_diag_shadow_concurrent = 0; /* a writer found another writer mid-copy */
volatile LONG g_diag_shadow_torn = 0;       /* draws skipped: seqlock reported an unstable read */
static volatile LONG g_shadow_concurrent_warned = 0;
/* 0 until Hook_UpdateSubresource has written real content into that slot's
 * shadow. mvp_direct_pool_find() refuses to hand out a not-yet-written
 * shadow: patching a draw from an all-zero shadow would apply a confidently
 * wrong zero matrix instead of falling through unpatched. */
volatile LONG g_shadow_valid[MVP_POOL_SLOTS_TOTAL];
volatile LONG g_shadow_writes = 0;   /* UpdateSubresource calls that fed a pool shadow */
volatile LONG g_diag_shadow_empty = 0; /* draws skipped because the shadow had no content yet */
volatile LONG g_diag_shadow_partial = 0; /* partial-box (offset) shadow updates handled */
/* Fix round 5, finding #5: counts contexts whose UpdateSubresource hook went
 * live only AFTER some shadow was already valid - the window in which that
 * context's earlier updates were missed, leaving a shadow STALE (not empty),
 * which is a wrong-data path rather than a skip path. See the note in
 * mvp_maybe_late_hook_ctx(). */
volatile LONG g_diag_latehook_after_valid = 0;
struct DirectPoolEntry g_direct_pool[MVP_POOL_SLOTS_TOTAL];
int g_direct_pool_count = 0; /* forward-declared above for the round-4 diagnostics */
int g_direct_pool_full_warned = 0;
CRITICAL_SECTION g_direct_pool_cs; /* guards reservation of a new slot only - see below */
int g_direct_pool_cs_ready = 0;

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
int mvp_direct_pool_find(ID3D11Buffer *buf, UINT *out_byte_width) {
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
int mvp_shadow_read(int slot, void *dst, UINT bytes) {
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
void mvp_try_register_bound_buffer(ID3D11Buffer *buf) {
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
int g_update_target_count = 0;
static void *g_seen_ctx[MVP_SEEN_CTX_MAX];
/* Append-only; read lock-free on the draw hot path, appended only under
 * g_latehook_cs. Volatile because the fast-path scan reads it without the
 * lock - see mvp_maybe_late_hook_ctx(). */
volatile LONG g_seen_ctx_count = 0;
CRITICAL_SECTION g_latehook_cs; /* guards APPENDS to g_update_targets[]/g_seen_ctx[] only */
int g_latehook_cs_ready = 0;
int g_seen_ctx_full_warned = 0;

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
int mvp_hook_update_target_locked(void *target) {
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
void mvp_maybe_late_hook_ctx(ID3D11DeviceContext *ctx) {
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
void mvp_shadow_note_writer(int slot) {
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
