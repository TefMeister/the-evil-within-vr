#pragma once

#define COBJMACROS
#include <windows.h>
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
 * Entirely inert unless TEWVR_SHADERDUMP=1 when shaderdump_install() runs.
 * Same lifecycle contract as cbdump: install with the dummy device/context
 * (vtables only, nothing retained), remove after mh_glue_shutdown().
 */

void shaderdump_install(ID3D11Device *dummy_dev, ID3D11DeviceContext *dummy_ctx);
void shaderdump_remove(void);
