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
