#include "mvp_patch.h"

#include <stdlib.h>
#include <string.h>

#include "MinHook.h"
#include "minhook_glue.h"
#include "log.h"
#include "d3d_capture.h"
#include "mvptable.h"
#include "camera.h"

/*
 * ---- Step 0: chosen read mechanism, and why ----
 *
 * The brief laid out three candidates for reading a draw's bound VS-slot0
 * cb0 content without a per-draw GPU stall or a deferred-context/worker-
 * thread threading violation:
 *   (a) capture the pool buffers' CPU-writable pointer directly (if a
 *       fresh Map ever fires on them after our hooks are live - e.g. on a
 *       level/scene transition);
 *   (b) hook ID3D11Device::CreateBuffer and Map the pool buffers ourselves
 *       at creation time, mirroring the engine's own persistent-map
 *       pattern;
 *   (c) a batched once-per-frame staging-copy snapshot on the IMMEDIATE
 *       context right after Present, accepting one frame of staleness.
 *
 * CHOSEN: (c). Reasoning:
 *   - This task's constraints explicitly forbid driving/witnessing a live
 *     gameplay session (only a menu smoke test is available for
 *     self-verification here), and both (a) and (b) hinge on live,
 *     gameplay-only, previously-UNOBSERVED behaviour: (a) needs a fresh Map
 *     to ever fire on the pool buffers after our hooks install - the Task
 *     6 discovery ledger's own gameplay captures (WITH our hooks already
 *     live) never saw one, so (a) can only be confirmed by a scene
 *     transition during a human-witnessed session, which is exactly the
 *     Step 3 check this module hands off. (b) requires verifying the
 *     engine tolerates a FOREIGN Map on its own buffer without conflicting
 *     with its own persistent-map setup - the brief itself flags this as
 *     something to "test carefully"; getting it wrong risks a hard crash
 *     or D3D11 validation failure the very first time the pool buffers are
 *     created, with no way for this implementer to catch that without
 *     exactly the live gameplay test it is told not to attempt.
 *   - (c) needs no such live-only assumption: it reuses the EXACT
 *     CopySubresourceRegion + Map(READ) + Unmap sequence the Task 6
 *     discovery probe (TEWVR_CBPEEK, seqdump.c) already proved reads
 *     correct MVP content from these buffers - just relocated from
 *     "per-draw, on whichever deferred worker thread happens to be
 *     recording" (unsafe: GPU round-trip off the immediate context from a
 *     worker thread, ~1900x/frame) to "once per frame, on the immediate
 *     context, right after Present" (single well-known thread, ~O(10)
 *     copies/frame - see mvp_patch_on_present()). This directly resolves
 *     both risks the brief calls out for CBPEEK's original per-draw use.
 *   - It also structurally fixes the CBPEEK review's Important
 *     dangling-pointer finding: every buffer identity this module ever
 *     touches is added to g_pool[] via pool_find_or_register_locked(),
 *     which takes ownership of exactly one AddRef'd COM reference per
 *     identity (from the VSGetConstantBuffers() call that discovered it)
 *     for the pool's entire lifetime - never a raw pointer cached without
 *     an owned reference.
 *   - Cost accepted: one frame of staleness on patched MVP content (a
 *     buffer's very first frame renders unpatched, and every subsequent
 *     frame reads the PREVIOUS frame's snapshot) - the brief explicitly
 *     accepts this for the rotation proof.
 *   - Gate self-check available without live gameplay: mvp_patch_on_present()
 *     times its own batched refresh and logs (a) periodically, and (b)
 *     immediately if a single refresh exceeds a stall threshold - see
 *     MVP_STALL_WARN_MS below. The menu smoke test (Step 3's own report)
 *     is the evidence available at implementation time; a controller
 *     revisiting this during/after the human-witnessed gameplay check can
 *     grep tewvr.log for "cb0 snapshot refresh" lines to confirm the gate
 *     holds under real deferred-draw load, and to see whether option (a)
 *     would in fact have been viable (a MISSING Map on the pool buffers
 *     during a scene transition would show up there as evidence for a
 *     future optimization, without this module depending on it).
 *
 * ---- Threading: per-thread scratch-buffer rings ----
 *
 * World draws are recorded from up to 6 deferred-context worker threads
 * concurrently (Task 5 finding) plus the immediate/render thread - up to 7
 * threads calling Hook_DrawIndexed/Hook_Draw at once. A single shared
 * scratch-buffer ring (round-robin index shared across threads) would let
 * two threads' Map/memcpy/Unmap/Bind sequences collide on the SAME
 * ID3D11Buffer object if the ring wraps while an earlier draw's sequence on
 * a different thread is still in flight - and at observed draw rates
 * (~1900 draws/frame across ~7 threads) a 64-deep ring can wrap in well
 * under a millisecond, making this a real risk, not a theoretical one.
 * Fixed by giving each thread its OWN private sub-ring (MVP_SLOTS_PER_THREAD
 * buffers), assigned once via a TLS slot on that thread's first patched
 * draw (mvp_alloc_scratch_index()) - no two threads ever touch the same
 * buffer object, and the common-case per-draw cost is one TLS read plus one
 * InterlockedIncrement, no lock.
 */

/* vtable indices, cross-checked against shaderdump.c's and seqdump.c's own
 * already-verified counts for ID3D11DeviceContext (both independently
 * landed DrawIndexed=12, Draw=13 counting from QueryInterface=0). */
#define VTBL_CTX_DRAWINDEXED 12
#define VTBL_CTX_DRAW        13

typedef void(STDMETHODCALLTYPE *DrawIndexed_t)(ID3D11DeviceContext *, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE *Draw_t)(ID3D11DeviceContext *, UINT, UINT);

/* Fixed snapshot/scratch window: generously above every reflected cb0 size
 * observed in Task 4/5/6 discovery (96/160/224B, mvp rows at worst +144..
 * +192), and a multiple of 16 (D3D11 constant-buffer size requirement). */
#define MVP_CB_BYTES 512

/* Distinct VS-slot0 buffer IDENTITIES ever seen (Task 6 discovery: ~6
 * deferred pool buffers + a handful of small immediate-context ones -
 * 32 is comfortable headroom). */
#define MVP_POOL_MAX 32

/* Per-thread scratch-buffer sub-rings: MVP_THREADS_MAX comfortably exceeds
 * the 7 threads (6 deferred workers + 1 immediate/render) Task 5's
 * discovery captures ever actually observed; MVP_SLOTS_PER_THREAD matches
 * the brief's "start at 64" total ring size (8*8=64). */
#define MVP_THREADS_MAX 8
#define MVP_SLOTS_PER_THREAD 8
#define MVP_SCRATCH_TOTAL (MVP_THREADS_MAX * MVP_SLOTS_PER_THREAD)

#define MVP_STAGING_BYTES (MVP_POOL_MAX * MVP_CB_BYTES)

/* Step 0 stall gate: a single per-frame snapshot refresh taking longer than
 * this is worth a loud log line - see mvp_patch_on_present(). */
#define MVP_STALL_WARN_MS 10

/* ---- rate limiter (same burst-then-every-Nth scheme as seqdump.c's
 * seq_rate_limit_should_fire() - copied locally rather than shared, since
 * that one is private to seqdump.c and this module must stay independent
 * of whether TEWVR_SEQDUMP is even compiled/active). ---- */
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
static volatile LONG g_overflow_log_count = 0;
static volatile LONG g_refresh_info_log_count = 0;

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
static int g_installed = 0;

/* ---- cb0 identity pool (Step 0's per-frame snapshot cache) ---- */

struct CbPoolEntry {
    ID3D11Buffer *buf; /* NULL until registered; then holds ONE owned ref
                           for this entry's lifetime (see file header) */
    UINT byte_width;   /* the bound buffer's own ByteWidth (informational,
                           and bounds how much of it we ever copy/read) */
    unsigned char snapshot[MVP_CB_BYTES];
    int snapshot_valid;
};
static struct CbPoolEntry g_pool[MVP_POOL_MAX];
static int g_pool_count = 0;
static int g_pool_full_warned = 0;
static CRITICAL_SECTION g_pool_cs;
static int g_pool_cs_ready = 0;

/* ---- scratch buffer pool + staging snapshot buffer (lazy, real device) ---- */

static ID3D11Buffer *g_scratch[MVP_SCRATCH_TOTAL];
static volatile LONG g_scratch_ready = 0;
static ID3D11Buffer *g_staging = NULL;

/* ---- per-thread scratch-ring assignment (see file header) ---- */

static DWORD g_tls_index = TLS_OUT_OF_INDEXES;
static volatile LONG g_next_bucket = 0;
static volatile LONG g_bucket_next[MVP_THREADS_MAX];

/* ---- test matrix K (TEWVR_TEST_YAW) ---- */

static Mat4 g_K;

/* ================= install-time hook helper ================= */

/* Create -> register -> enable, same ORDERING discipline as Task 5
 * addendum 3's g_hooked_funcs table in seqdump.c (register the freshly-
 * created, not-yet-enabled hook before MH_EnableHook, so nothing can ever
 * reach the detour before it is discoverable; fail loud + MH_RemoveHook()
 * rather than leave an enabled-but-undispatchable hook live). Unlike
 * seqdump's table, this one does not need per-vtable late-hooking or any
 * locking: DrawIndexed/Draw share ONE underlying code address between the
 * immediate and every deferred-context vtable flavor (seqdump.c's own
 * confirmed finding - see its "Single-target hooks" comment), so hooking
 * via the dummy context's vtable ONCE, here, covers every ctx flavor for
 * the rest of the process's lifetime. This whole function only ever runs
 * from mvp_patch_install(), once, single-threaded, on the bootstrap
 * thread - strictly before the game's own render loop (and therefore any
 * real draw) can start, so no concurrent access to g_hooked[] is possible. */
static int hook_one(void *target, void *detour, void **out_orig, const char *name) {
    void *orig = NULL;

    if (!mh_glue_create(target, detour, &orig, name)) {
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

    if (!mh_glue_enable(target, name)) {
        g_hooked_count--; /* undo registration; mh_glue_enable() already removed the (never-live) hook */
        return 0;
    }

    *out_orig = orig;
    return 1;
}

/* ================= cb0 identity pool ================= */

/* Finds `buf` in the pool, or registers it if unseen (up to MVP_POOL_MAX).
 * On a FRESH registration, this function takes ownership of the caller's
 * reference to `buf` (the caller must NOT release it) - exactly one
 * persistent AddRef per pool entry for the entry's lifetime. On a cache
 * hit, or when the table is full, `*out_is_new` is 0 and the caller must
 * release its own (redundant, or orphaned) reference itself. Returns the
 * slot index, or -1 if the table is full (nothing was registered).
 * Caller holds g_pool_cs. */
static int pool_find_or_register_locked(ID3D11Buffer *buf, UINT byte_width, int *out_is_new) {
    int i;
    for (i = 0; i < g_pool_count; i++) {
        if (g_pool[i].buf == buf) {
            *out_is_new = 0;
            return i;
        }
    }
    if (g_pool_count >= MVP_POOL_MAX) {
        if (!g_pool_full_warned) {
            g_pool_full_warned = 1;
            log_msg("mvp_patch: cb0 identity pool full (%d); further distinct VS-slot0 buffers "
                     "won't be patched (fail-safe: only their own draws render unpatched)",
                     MVP_POOL_MAX);
        }
        *out_is_new = 0;
        return -1;
    }
    {
        int idx = g_pool_count;
        g_pool[idx].buf = buf;
        g_pool[idx].byte_width = byte_width;
        g_pool[idx].snapshot_valid = 0;
        g_pool_count++;
        *out_is_new = 1;
        return idx;
    }
}

/* ================= per-thread scratch-ring allocation ================= */

/* Returns a scratch-buffer index in [0, MVP_SCRATCH_TOTAL), guaranteed to
 * never collide with an index concurrently in use by another thread (see
 * file header) - or -1 if TLS was never set up. Lock-free on the hot path
 * (a TLS read plus one InterlockedIncrement); only the FIRST call from a
 * given thread pays a bucket assignment (also lock-free, via
 * InterlockedIncrement on g_next_bucket). */
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
                         "further threads share the last bucket - patch stays CORRECT "
                         "(WRITE_DISCARD renaming), may rarely contend under unusually many threads",
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

/* ================= lazy real-device resource creation ================= */

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

static int ensure_staging_ready(ID3D11Device *dev) {
    D3D11_BUFFER_DESC sd;
    HRESULT hr;

    memset(&sd, 0, sizeof(sd));
    sd.ByteWidth = MVP_STAGING_BYTES;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = ID3D11Device_CreateBuffer(dev, &sd, NULL, &g_staging);
    if (FAILED(hr) || g_staging == NULL) {
        log_msg("mvp_patch: CreateBuffer for %d-byte staging snapshot buffer failed (hr=0x%08lX); "
                 "per-draw MVP override DISABLED this session",
                 MVP_STAGING_BYTES, (unsigned long)hr);
        return 0;
    }
    return 1;
}

/* ================= the per-draw patch itself ================= */

/* Shared core for Hook_DrawIndexed/Hook_Draw. On success, patches VS slot 0
 * to a scratch buffer holding the K-rotated MVP (with everything else
 * about cb0 unchanged), fills `*out_orig_buf` with the buffer the caller
 * must rebind to slot 0 AFTER calling the original draw, and returns 1.
 * On ANY failure or "not safely patchable" condition, does nothing and
 * returns 0 - the caller then just calls the original draw unmodified,
 * exactly like an unpatched build. Never blocks, retries, or delays the
 * draw itself. */
static int mvp_patch_prepare(ID3D11DeviceContext *ctx, ID3D11Buffer **out_orig_buf) {
    ID3D11VertexShader *vs = NULL;
    ID3D11Buffer *buf = NULL;
    int offs[4];
    int idx, is_new;
    unsigned char local_snapshot[MVP_CB_BYTES];
    int have_snapshot;
    UINT byte_width;
    D3D11_BUFFER_DESC bd;
    int scratch_index;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    Mat4 mvp, patched;
    int row;

    *out_orig_buf = NULL;

    if (!g_installed || !g_scratch_ready) {
        return 0;
    }
    if (!d3d_capture_ready()) {
        return 0;
    }

    ID3D11DeviceContext_VSGetShader(ctx, &vs, NULL, NULL);
    if (vs == NULL) {
        return 0;
    }
    if (!mvp_row_offsets_for_shader(vs, offs)) {
        ID3D11VertexShader_Release(vs);
        return 0; /* unknown shader, or non-contiguous rows we refuse to guess at */
    }
    ID3D11VertexShader_Release(vs);

    if (offs[0] < 0 || offs[3] + 16 > MVP_CB_BYTES) {
        return 0; /* rows fall outside our fixed window - not observed, stay fail-safe anyway */
    }

    ID3D11DeviceContext_VSGetConstantBuffers(ctx, 0, 1, &buf);
    if (buf == NULL) {
        return 0; /* nothing bound at slot 0 */
    }
    ID3D11Buffer_GetDesc(buf, &bd);
    byte_width = bd.ByteWidth;
    if (byte_width == 0) {
        ID3D11Buffer_Release(buf);
        return 0;
    }

    EnterCriticalSection(&g_pool_cs);
    idx = pool_find_or_register_locked(buf, byte_width, &is_new);
    have_snapshot = 0;
    if (idx >= 0 && g_pool[idx].snapshot_valid) {
        have_snapshot = 1;
        memcpy(local_snapshot, g_pool[idx].snapshot, MVP_CB_BYTES);
    }
    LeaveCriticalSection(&g_pool_cs);

    if (!is_new) {
        /* Cache hit (pool already owns a ref), or table was full (nothing
         * anywhere owns a ref) - either way this call's own
         * VSGetConstantBuffers() reference is redundant/orphaned and must
         * be released here. */
        ID3D11Buffer_Release(buf);
    }
    /* else: pool now owns `buf`'s reference - do NOT release it. This is
     * the structural fix for the CBPEEK review's dangling-pointer finding
     * (Important): no raw ID3D11Buffer* is ever cached without an owned
     * COM reference from here on. */

    if (idx < 0 || !have_snapshot) {
        /* Pool full, or this is the first frame this identity has ever
         * been seen (no snapshot refreshed for it yet) - fail-safe skip;
         * self-corrects on the next Present's refresh. */
        return 0;
    }

    for (row = 0; row < 4; row++) {
        const float *f = (const float *)(local_snapshot + offs[row]);
        mvp.m[row][0] = f[0];
        mvp.m[row][1] = f[1];
        mvp.m[row][2] = f[2];
        mvp.m[row][3] = f[3];
    }
    patched = mat4_mul(g_K, mvp);
    for (row = 0; row < 4; row++) {
        float *f = (float *)(local_snapshot + offs[row]);
        f[0] = patched.m[row][0];
        f[1] = patched.m[row][1];
        f[2] = patched.m[row][2];
        f[3] = patched.m[row][3];
    }

    scratch_index = mvp_alloc_scratch_index();
    if (scratch_index < 0 || g_scratch[scratch_index] == NULL) {
        return 0;
    }

    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_scratch[scratch_index], 0,
                                 D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr) || mapped.pData == NULL) {
        if (mvp_rate_limit_should_fire(&g_map_fail_log_count)) {
            log_msg("mvp_patch: scratch Map(WRITE_DISCARD) failed (hr=0x%08lX); draw falls through unpatched",
                     (unsigned long)hr);
        }
        return 0;
    }
    memcpy(mapped.pData, local_snapshot, MVP_CB_BYTES);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_scratch[scratch_index], 0);

    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_scratch[scratch_index]);

    *out_orig_buf = buf; /* identity only, for the post-draw rebind - pool keeps it alive */
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

    patched = mvp_patch_prepare(ctx, &orig_buf);

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

    patched = mvp_patch_prepare(ctx, &orig_buf);

    orig(ctx, VertexCount, StartVertexLocation);

    if (patched) {
        ID3D11Buffer *restore = orig_buf;
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &restore);
    }
}

/* ================= public API ================= */

void mvp_patch_install(ID3D11DeviceContext *dummy_ctx) {
    void **vtbl;
    void *di_orig = NULL;
    void *d_orig = NULL;
    int ok_di, ok_d;
    char flagbuf[32];
    DWORD len;
    float deg;

    if (g_installed) {
        return;
    }

    if (dummy_ctx == NULL) {
        log_msg("mvp_patch: install called with NULL dummy context; per-draw MVP override DISABLED this session");
        return;
    }

    if (!mh_glue_init()) {
        log_msg("mvp_patch: MinHook init failed; per-draw MVP override DISABLED this session");
        return;
    }

    InitializeCriticalSection(&g_pool_cs);
    g_pool_cs_ready = 1;

    g_tls_index = TlsAlloc();
    if (g_tls_index == TLS_OUT_OF_INDEXES) {
        log_msg("mvp_patch: TlsAlloc failed (gle=%lu); per-draw MVP override DISABLED this session",
                 (unsigned long)GetLastError());
        DeleteCriticalSection(&g_pool_cs);
        g_pool_cs_ready = 0;
        return;
    }

    vtbl = *(void ***)dummy_ctx;

    ok_di = hook_one(vtbl[VTBL_CTX_DRAWINDEXED], (void *)&Hook_DrawIndexed, &di_orig,
                      "mvp_patch ID3D11DeviceContext::DrawIndexed");
    ok_d = hook_one(vtbl[VTBL_CTX_DRAW], (void *)&Hook_Draw, &d_orig,
                     "mvp_patch ID3D11DeviceContext::Draw");

    if (!ok_di && !ok_d) {
        log_msg("mvp_patch: failed to hook both DrawIndexed and Draw; per-draw MVP override DISABLED this session");
        TlsFree(g_tls_index);
        g_tls_index = TLS_OUT_OF_INDEXES;
        DeleteCriticalSection(&g_pool_cs);
        g_pool_cs_ready = 0;
        return;
    }

    g_drawindexed_orig = (DrawIndexed_t)di_orig;
    g_draw_orig = (Draw_t)d_orig;

    /* TEWVR_TEST_YAW: read once here, synchronously, on the bootstrap
     * thread - strictly before the hooks just enabled above can possibly
     * be reached by a real draw (the game's own device/render loop has not
     * started yet at this point in the call chain), so no lazy-init race
     * is needed for g_K. */
    len = GetEnvironmentVariableA("TEWVR_TEST_YAW", flagbuf, sizeof(flagbuf));
    deg = (len > 0 && len < sizeof(flagbuf)) ? (float)atof(flagbuf) : 0.0f;
    if (deg != 0.0f) {
        g_K = mat4_rotation_y(deg);
        log_msg("mvp_patch: TEWVR_TEST_YAW=%.3f -> test rotation K ACTIVE (every patchable draw rotated)",
                 (double)deg);
    } else {
        g_K = mat4_identity();
        log_msg("mvp_patch: TEWVR_TEST_YAW unset/0 -> K = identity "
                 "(read/patch/rebind mechanism still exercised every draw; no visible change expected)");
    }

    g_installed = 1;
    log_msg("mvp_patch: installed (DrawIndexed=%d Draw=%d); scratch/staging pools created "
             "lazily on the real device once d3d_capture_ready()", ok_di, ok_d);
}

void mvp_patch_remove(void) {
    int i;

    if (g_pool_cs_ready) {
        EnterCriticalSection(&g_pool_cs);
        for (i = 0; i < g_pool_count; i++) {
            if (g_pool[i].buf != NULL) {
                ID3D11Buffer_Release(g_pool[i].buf);
                g_pool[i].buf = NULL;
            }
        }
        g_pool_count = 0;
        LeaveCriticalSection(&g_pool_cs);
        DeleteCriticalSection(&g_pool_cs);
        g_pool_cs_ready = 0;
    }

    if (g_scratch_ready) {
        for (i = 0; i < MVP_SCRATCH_TOTAL; i++) {
            if (g_scratch[i] != NULL) {
                ID3D11Buffer_Release(g_scratch[i]);
                g_scratch[i] = NULL;
            }
        }
        if (g_staging != NULL) {
            ID3D11Buffer_Release(g_staging);
            g_staging = NULL;
        }
        InterlockedExchange(&g_scratch_ready, 0);
    }

    if (g_tls_index != TLS_OUT_OF_INDEXES) {
        TlsFree(g_tls_index);
        g_tls_index = TLS_OUT_OF_INDEXES;
    }

    g_drawindexed_orig = NULL;
    g_draw_orig = NULL;
    g_hooked_count = 0;
    g_installed = 0;
}

void mvp_patch_on_present(void) {
    int i, n;
    ULONGLONG t0, t1;
    D3D11_BOX box;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;

    if (!g_installed) {
        return;
    }
    if (!d3d_capture_ready() || g_d3d.dev == NULL || g_d3d.ctx == NULL) {
        return;
    }

    if (!g_scratch_ready) {
        if (ensure_scratch_ready(g_d3d.dev) && ensure_staging_ready(g_d3d.dev)) {
            InterlockedExchange(&g_scratch_ready, 1);
            log_msg("mvp_patch: scratch pool (%d buffers, %dB each) + %dB staging snapshot buffer ready",
                     MVP_SCRATCH_TOTAL, MVP_CB_BYTES, MVP_STAGING_BYTES);
        } else {
            return; /* will retry next Present; cheap (two CreateBuffer attempts) */
        }
    }

    EnterCriticalSection(&g_pool_cs);
    n = g_pool_count;
    LeaveCriticalSection(&g_pool_cs);

    if (n <= 0 || g_staging == NULL) {
        return; /* nothing discovered to snapshot yet */
    }

    t0 = GetTickCount64();

    /* Batch every copy first (queued on the immediate context's command
     * stream), then a single Map(READ) - one stall point for up to
     * MVP_POOL_MAX buffers instead of one per buffer (see Step 0 comment
     * above). g_pool[i].buf for i<n is safe to read without the lock here:
     * entries are append-only (never removed/reordered while active), and
     * concurrent registrations only ever touch index >= n. */
    for (i = 0; i < n; i++) {
        ID3D11Buffer *buf = g_pool[i].buf;
        UINT bw;

        if (buf == NULL) {
            continue;
        }
        bw = (g_pool[i].byte_width < MVP_CB_BYTES) ? g_pool[i].byte_width : MVP_CB_BYTES;
        if (bw == 0) {
            continue;
        }

        memset(&box, 0, sizeof(box));
        box.left = 0;
        box.right = bw;
        box.top = 0;
        box.bottom = 1;
        box.front = 0;
        box.back = 1;

        ID3D11DeviceContext_CopySubresourceRegion(g_d3d.ctx, (ID3D11Resource *)g_staging, 0,
                                                   (UINT)i * MVP_CB_BYTES, 0, 0,
                                                   (ID3D11Resource *)buf, 0, &box);
    }

    hr = ID3D11DeviceContext_Map(g_d3d.ctx, (ID3D11Resource *)g_staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || mapped.pData == NULL) {
        if (mvp_rate_limit_should_fire(&g_map_fail_log_count)) {
            log_msg("mvp_patch: staging Map(READ) failed (hr=0x%08lX); this frame's cb0 snapshot "
                     "refresh skipped (stale content, if any, still used next frame - fail-safe)",
                     (unsigned long)hr);
        }
        return;
    }

    EnterCriticalSection(&g_pool_cs);
    for (i = 0; i < n && i < g_pool_count; i++) {
        UINT bw = (g_pool[i].byte_width < MVP_CB_BYTES) ? g_pool[i].byte_width : MVP_CB_BYTES;
        if (bw == 0) {
            continue;
        }
        memcpy(g_pool[i].snapshot, (const unsigned char *)mapped.pData + (size_t)i * MVP_CB_BYTES, bw);
        if (bw < MVP_CB_BYTES) {
            memset(g_pool[i].snapshot + bw, 0, MVP_CB_BYTES - bw);
        }
        g_pool[i].snapshot_valid = 1;
    }
    LeaveCriticalSection(&g_pool_cs);

    ID3D11DeviceContext_Unmap(g_d3d.ctx, (ID3D11Resource *)g_staging, 0);

    t1 = GetTickCount64();
    if (t1 - t0 > MVP_STALL_WARN_MS) {
        log_msg("mvp_patch: STEP0 GATE WATCH - cb0 snapshot refresh took %llums across %d buffers "
                 "(Present cadence may be affected)",
                 (unsigned long long)(t1 - t0), n);
    } else if (mvp_rate_limit_should_fire(&g_refresh_info_log_count)) {
        log_msg("mvp_patch: cb0 snapshot refresh: %d buffer(s), %llums (Step 0 gate watch, informational)",
                 n, (unsigned long long)(t1 - t0));
    }
}
