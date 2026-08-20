#pragma once

#define COBJMACROS
#include <windows.h>
#include <stdint.h>
#include <d3d11.h>

/*
 * TEMPORARY vertex-shader discovery instrumentation for Task 4
 * (shader-level RE: which cbuffer/matrix produces SV_Position for world
 * geometry). Three hooks, same shared-vtable trick as cbdump.c:
 *
 *   ID3D11Device::CreateVertexShader  - dumps every unique DXBC blob to
 *     %LOCALAPPDATA%\TEWVR\shaders\vs_<fnv1a64>.dxbc (deduped by hash) and
 *     remembers shader-object -> hash so draws can be attributed.
 *   ID3D11DeviceContext::VSSetShader  - tracks the currently bound VS.
 *   ID3D11DeviceContext::DrawIndexed / Draw / DrawIndexedInstanced /
 *     DrawInstanced - per-shader draw + index-count tallies, plus a one-time
 *     snapshot of which cbuffer sizes sit in VS slots 0..5 at that shader's
 *     first draw. Top shaders by index volume are logged to tewvr.log every
 *     few seconds; heavy indexed draws = world geometry.
 *
 * The CreateVertexShader hook (hash tracking + mvptable.c's mvp-offset
 * reflection) ALWAYS installs whenever shaderdump_install() runs, as of
 * Task 6 - mvp_patch.c's real per-draw MVP override depends on
 * mvp_offset_for_shader()/mvp_row_offsets_for_shader() being populated on
 * EVERY normal run, not just a diagnostic session (this was previously
 * gated behind TEWVR_SHADERDUMP=1/TEWVR_SEQDUMP=1 alongside everything
 * else here - see g_cvs_hook_active's comment in shaderdump.c). Only the
 * VSSetShader / Draw* / blob-dump / per-shader-stats hooks below stay
 * opt-in behind TEWVR_SHADERDUMP=1 (their original, still-diagnostic-only
 * purpose - see shaderdump.c's g_cvs_hook_active vs g_sd_active split).
 * Same lifecycle contract as cbdump: install with the dummy device/context
 * (vtables only, nothing retained), remove after mh_glue_shutdown().
 */

void shaderdump_install(ID3D11Device *dummy_dev, ID3D11DeviceContext *dummy_ctx);
void shaderdump_remove(void);

/*
 * Task 5: looks up the FNV-1a64 hash recorded for a vertex-shader object
 * pointer at CreateVertexShader time (see Hook_CreateVS in shaderdump.c).
 * Returns 0 if the shader is untracked (hook never fired for it yet, table
 * full, or - very rare after Task 6, only if the dummy device/context was
 * NULL or MinHook init failed - the CreateVertexShader hook never installed
 * at all). Thread-safe; used by seqdump.c (VSSETSHADER event) and
 * mvptable.c (mvp_offset_for_shader()/mvp_row_offsets_for_shader()).
 */
uint64_t shaderdump_hash_for_shader(const void *vs_ptr);
