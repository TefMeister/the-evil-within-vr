#pragma once

#include <windows.h>
#include <stdint.h>

/*
 * Task 5 feature 2: runtime hash -> mvpmatrix-offset reflection table.
 *
 * At CreateVertexShader time (see Hook_CreateVS in shaderdump.c, which
 * calls mvptable_on_shader_created() for every shader it sees, whenever its
 * own hook is installed at all - TEWVR_SHADERDUMP=1 or TEWVR_SEQDUMP=1),
 * this module runtime-reflects the DXBC blob with D3DReflect (loaded from
 * the system d3dcompiler_47.dll) to find the `constantBufferV` constant
 * buffer (falling back to whichever cbuffer is bound to register b0) and
 * the byte offset of its `mvpmatrixx` row within it - the per-draw MVP
 * patch point a later task needs.
 *
 * Results are kept in an in-memory hash(blob)->{cb0 size, mvpx offset}
 * table (thread-safe for reads after creation - a critical section guards
 * every table access) and appended one line per newly-seen hash to
 * %LOCALAPPDATA%\TEWVR\mvp_offsets.log:
 *   hash=<hex> cb0=<bytes> mvpx=<offset or -1> contiguous=<0|1>
 *
 * Entirely fail-safe: D3DReflect unavailable, reflection failure, or
 * missing constant buffer/variables all log once (or once per shader) and
 * leave the table/log line recording mvpx=-1 - never crashes, never
 * retries a hash it has already recorded. This holds even under
 * concurrent CreateVertexShader calls for identical bytecode on different
 * threads: the table insert and the mvp_offsets.log line are written
 * together under one lock acquisition (mvp_record_locked(), post-Task-5-
 * review fix), so at most one line per hash is ever appended, never a
 * duplicate from a racing thread.
 */

/* Call once, before any mvptable_on_shader_created() calls, only when
 * shaderdump.c's CreateVertexShader hook is actually going to be installed
 * (TEWVR_SHADERDUMP=1 or TEWVR_SEQDUMP=1). Opens mvp_offsets.log (fresh per
 * session, same "w"/truncate convention as cbdump.log/shaders\*.dxbc) and
 * the table's critical section. Safe to call more than once (no-op after
 * the first). On log-open failure, logs once to tewvr.log and keeps the
 * in-memory table working without a log file. */
void mvptable_init(void);

/* Closes mvp_offsets.log and the critical section. Safe to call even if
 * mvptable_init() was never called (pure no-op then). Must run after
 * mh_glue_shutdown() has disabled the CreateVertexShader trampoline (same
 * ordering contract as cbdump_remove()/shaderdump_remove()), so no
 * in-flight Hook_CreateVS call can touch state that has just been torn
 * down. */
void mvptable_shutdown(void);

/* Reflects `bytecode`/`length` (a freshly-created vertex-shader DXBC blob)
 * if `hash` (its FNV-1a64, computed the same way shaderdump.c already
 * does for its own blob-dedup table) has not been recorded before, and
 * appends the result to mvp_offsets.log. No-op (returns immediately) if
 * mvptable_init() was never called/failed, `hash` is already known, or
 * D3DReflect could not be loaded. Never crashes. */
void mvptable_on_shader_created(uint64_t hash, const void *bytecode, SIZE_T length);

/* Looks up the mvpmatrixx byte offset for a vertex-shader object pointer
 * (as passed to CreateVertexShader/VSSetShader), via
 * shaderdump_hash_for_shader() then this module's own hash->offset table.
 * Returns -1 if unknown for any reason (shader untracked, hash never
 * reflected, cb0/mvpmatrixx not found in that shader). No consumer yet -
 * this task only makes it queryable for later tasks. Thread-safe for reads
 * after the corresponding CreateVertexShader call has returned (creations
 * happen early, per the brief). */
int mvp_offset_for_shader(const void *vs_ptr);

/* Task 6: like mvp_offset_for_shader(), but hands out ALL FOUR mvpmatrix
 * row offsets at once, and ONLY when they are known to be safely readable
 * as 4 contiguous 16-byte rows (mvpx_offset, +16, +32, +48) - refusing
 * (returning 0) for shaders whose y/z/w rows were reflected at some other,
 * non-contiguous layout, rather than guessing wrong offsets. This is the
 * function mvp_patch.c's real per-draw MVP override actually calls -
 * mvp_offset_for_shader() alone is not enough to patch all 4 rows safely.
 * Returns 1 and fills row_offsets[0..3] on success; returns 0 (leaving
 * row_offsets untouched) if the shader is untracked, has no reflected
 * mvpx_offset, or was not confirmed contiguous. See mvptable.c's own
 * comment above this function for the discovery finding that motivated it. */
int mvp_row_offsets_for_shader(const void *vs_ptr, int row_offsets[4]);

/* Task 6 fix round 3: DIAGNOSTIC-ONLY classification of why
 * mvp_row_offsets_for_shader() would refuse a given shader - added because
 * a real gameplay session showed mvp_patch's per-draw patch never firing
 * at all, with no visibility into which of several possible reasons was
 * responsible. mvp_row_offsets_for_shader() itself is unchanged (still the
 * function the real patch path calls); this exists purely so mvp_patch.c's
 * rate-limited skip-reason counters can report WHY, not just THAT, a
 * shader was refused. Not on any hot path by itself - mvp_patch.c only
 * calls this in the (already-failing) branch where
 * mvp_row_offsets_for_shader() just returned 0, so it costs one extra
 * table lookup only for draws that were already being skipped. */
enum MvpShaderStatus {
    MVP_SHADER_UNKNOWN = 0,       /* untracked: hash lookup failed, or this
                                      exact shader was never reflected */
    MVP_SHADER_NO_MVP = 1,        /* known, but this shader has no
                                      mvpmatrix at all (mvpx_offset == -1) -
                                      expected for post-process/depth-only
                                      shaders, per Task 4's finding */
    MVP_SHADER_NONCONTIGUOUS = 2, /* known, has an mvpx_offset, but y/z/w
                                      were not confirmed contiguous - the
                                      same refusal mvp_row_offsets_for_shader()
                                      already makes */
    MVP_SHADER_OK = 3             /* known, offsets safe to use - matches
                                      mvp_row_offsets_for_shader()'s success
                                      case; should not normally reach the
                                      diagnostic caller, which only calls
                                      this after mvp_row_offsets_for_shader()
                                      has already failed */
};

enum MvpShaderStatus mvp_shader_status_for_shader(const void *vs_ptr);
