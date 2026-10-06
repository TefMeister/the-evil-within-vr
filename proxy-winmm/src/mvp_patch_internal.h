/* mvp_patch_internal.h - shared definitions for the mvp_patch module.
 *
 * Split out of mvp_patch.c on 2026-09-30, MOVE-ONLY: every constant, type and
 * comment below is the original text; the only changes are that state and
 * helpers one of the mvp_*.c files now needs from another are declared here
 * `extern` instead of being file-static. Not a public interface - only the
 * mvp_*.c files include it. See mvp_patch.c for the module overview. */
#ifndef MVP_PATCH_INTERNAL_H
#define MVP_PATCH_INTERNAL_H

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
#include "test_k.h"
#include "stereo_afr.h"

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

#define MVP_SEEN_CB_MAX      2048
#define MVP_MISS_COMBO_MAX   24
#define MVP_MISS_SAMPLE_MASK 0xFF /* GetDesc on ~1 in 256 pool-misses (~140/s at observed rates) */
#define MVP_REGISTER_PROBE_MASK 0x3F /* try registering a missed buffer on ~1 in 64 misses */

struct DirectPoolEntry {
    ID3D11Buffer *buf; /* NULL until registered; written LAST (see
                           mvp_direct_pool_try_capture()) - a non-NULL read
                           here is this slot's "ready" signal */
    void *cpu_ptr;      /* live CPU-readable copy of this buffer's content:
                            fix round 4 makes this our own shadow, kept
                            current by Hook_UpdateSubresource */
    UINT byte_width;
};

/* ---- cross-file state and helpers ---- */

/* Since the 2026-09-30 split the block below is a set of extern declarations
 * (it was a block of tentative definitions inside mvp_patch.c). The four that
 * never had an initialised definition - g_diag_dyn_empty, g_diag_dyn_patched, g_dyn_pool_count, g_dyn_shadow_writes -
 * are now defined, zero-initialised as before, in mvp_dynamic.c. */
/* Tentative declarations so the diagnostic reporter below can read state
 * that is defined further down (C tentative definitions - the real
 * definitions, with initializers, follow in their own sections). */
extern int g_direct_pool_count;
extern int g_dyn_pool_count;                  /* 2026-09-04c, defined with the dynamic pool below */
extern volatile LONG g_diag_dyn_patched;
extern volatile LONG g_diag_dyn_empty;
extern volatile LONG g_dyn_shadow_writes;
extern volatile LONG g_diag_map_overflow;
/* 2026-09-05 pairing triplet; defined with the map table below. Tentative
 * declarations, because the periodic reporter above reads them. */
extern volatile LONG g_diag_dyn_map_seen;
extern volatile LONG g_diag_dyn_unmap_seen;
extern volatile LONG g_diag_dyn_unmap_nomap;
extern int g_update_target_count;
extern volatile LONG g_shadow_writes;
extern volatile LONG g_diag_shadow_empty;
extern volatile LONG g_diag_shadow_partial;
extern volatile LONG g_diag_shadow_multiwriter;
extern volatile LONG g_diag_latehook_after_valid;
extern volatile LONG g_diag_shadow_concurrent;
extern volatile LONG g_diag_shadow_torn;

/* defined in mvp_diag.c */
extern volatile LONG g_cb_create_const;
extern volatile LONG g_cb_create_total;
extern volatile LONG g_diag_bounds_fail;
extern volatile LONG g_diag_no_slot0_buf;
extern volatile LONG g_diag_no_vs;
extern volatile LONG g_diag_not_installed;
extern volatile LONG g_diag_patched;
extern volatile LONG g_diag_pool_miss;
extern volatile LONG g_diag_scratch_alloc_fail;
extern volatile LONG g_diag_scratch_map_fail;
extern volatile LONG g_diag_scratch_not_ready;
extern volatile LONG g_diag_shader_no_mvp;
extern volatile LONG g_diag_shader_rows_incomplete;
extern volatile LONG g_diag_shader_unknown;
extern volatile LONG g_map_fail_log_count;
extern volatile LONG g_miss_combo_count;
extern volatile LONG g_miss_sample_tick;
extern volatile LONG g_offset_oob_log_count;
extern volatile LONG g_pool_miss_first_logged;
extern volatile LONG g_update_bug_log_count;
void mvp_diag_maybe_report(void);
void mvp_diag_report_misses(void);
void mvp_miss_sample(ID3D11Buffer *buf, uint64_t vs_hash, UINT count, int indexed);
int mvp_rate_limit_should_fire(volatile LONG *counter);
void mvp_seen_cb_record(ID3D11Buffer *buf, const D3D11_BUFFER_DESC *d);

/* defined in mvp_shadow.c */
extern struct DirectPoolEntry g_direct_pool[MVP_POOL_SLOTS_TOTAL];
extern CRITICAL_SECTION g_direct_pool_cs;
extern int g_direct_pool_cs_ready;
extern int g_direct_pool_full_warned;
extern CRITICAL_SECTION g_latehook_cs;
extern int g_latehook_cs_ready;
extern volatile LONG g_seen_ctx_count;
extern int g_seen_ctx_full_warned;
extern unsigned char g_shadow[MVP_POOL_SLOTS_TOTAL][MVP_CB_BYTES];
extern volatile LONG g_shadow_seq[MVP_POOL_SLOTS_TOTAL];
extern volatile LONG g_shadow_valid[MVP_POOL_SLOTS_TOTAL];
int mvp_direct_pool_find(ID3D11Buffer *buf, UINT *out_byte_width);
int mvp_hook_update_target_locked(void *target);
void mvp_maybe_late_hook_ctx(ID3D11DeviceContext *ctx);
void mvp_shadow_note_writer(int slot);
int mvp_shadow_read(int slot, void *dst, UINT bytes);
void mvp_try_register_bound_buffer(ID3D11Buffer *buf);

/* defined in mvp_dynamic.c */
HRESULT STDMETHODCALLTYPE Hook_Map(ID3D11DeviceContext *ctx, ID3D11Resource *res,
                                          UINT sub, D3D11_MAP type, UINT flags,
                                          D3D11_MAPPED_SUBRESOURCE *mapped);
void STDMETHODCALLTYPE Hook_Unmap(ID3D11DeviceContext *ctx, ID3D11Resource *res, UINT sub);
extern Map_t   g_map_orig;
extern Unmap_t g_unmap_orig;
int mvp_dyn_pool_find(ID3D11Buffer *buf, UINT *out_byte_width);
void mvp_try_register_bound_dynamic(ID3D11Buffer *buf);

/* defined in mvp_scratch.c */
int ensure_scratch_ready(ID3D11Device *dev);
extern ID3D11Buffer *g_scratch[MVP_SCRATCH_TOTAL];
extern CRITICAL_SECTION g_scratch_init_cs;
extern int g_scratch_init_cs_ready;
extern volatile LONG g_scratch_ready;
extern DWORD g_tls_index;
int mvp_alloc_scratch_index(void);

/* defined in mvp_install.c */
extern CreateBuffer_t g_createbuffer_orig;
extern Draw_t g_draw_orig;
extern DrawIndexed_t g_drawindexed_orig;
extern int g_installed;

/* defined in mvp_patch.c */
HRESULT STDMETHODCALLTYPE Hook_CreateBuffer(ID3D11Device *dev, const D3D11_BUFFER_DESC *pDesc,
                                                    const D3D11_SUBRESOURCE_DATA *pInitialData,
                                                    ID3D11Buffer **ppBuffer);
void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext *ctx, UINT VertexCount,
                                         UINT StartVertexLocation);
void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext *ctx, UINT IndexCount,
                                                UINT StartIndexLocation, INT BaseVertexLocation);
extern Mat4 g_K;

#endif /* MVP_PATCH_INTERNAL_H */
