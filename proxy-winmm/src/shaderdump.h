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
 * Entirely inert unless TEWVR_SHADERDUMP=1 OR TEWVR_SEQDUMP=1 when
 * shaderdump_install() runs (Task 5 added TEWVR_SEQDUMP as a second trigger:
 * the CreateVertexShader hook - hash tracking + mvptable.c's mvp-offset
 * reflection - is needed by seqdump's VSSETSHADER event and mvp_offsets.log
 * even when full shaderdump stats/blob-dump are off; see shaderdump.c's
 * g_cvs_hook_active vs g_sd_active split). Same lifecycle contract as
 * cbdump: install with the dummy device/context (vtables only, nothing
 * retained), remove after mh_glue_shutdown().
 */

void shaderdump_install(ID3D11Device *dummy_dev, ID3D11DeviceContext *dummy_ctx);
void shaderdump_remove(void);

/*
 * Task 5: looks up the FNV-1a64 hash recorded for a vertex-shader object
 * pointer at CreateVertexShader time (see Hook_CreateVS in shaderdump.c).
 * Returns 0 if the shader is untracked (hook never fired for it, table
 * full, or the CreateVertexShader hook was never installed at all - e.g.
 * both TEWVR_SHADERDUMP and TEWVR_SEQDUMP unset). Thread-safe; used by
 * seqdump.c (VSSETSHADER event) and mvptable.c (mvp_offset_for_shader()).
 */
uint64_t shaderdump_hash_for_shader(const void *vs_ptr);
