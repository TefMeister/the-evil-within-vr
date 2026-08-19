# 06c — Shader disassembly: the geometry transform, found (Task 4 closed)

**Date:** 2026-08-19 (second session). **Status: Task 4 RESOLVED.**

After visual probing hit its limits (see 06b), we switched to shader-level RE, and it
answered the question definitively in one short capture session.

## Method

New temporary instrumentation in the winmm proxy (`shaderdump.c`, gated on
`TEWVR_SHADERDUMP=1`), using the same shared-vtable MinHook technique as the
Present hook and cbdump:

1. **`ID3D11Device::CreateVertexShader`** — every unique vertex-shader DXBC blob
   (FNV-1a-64 deduped) is written to `%LOCALAPPDATA%\TEWVR\shaders\vs_<hash>.dxbc`.
   The hook installs at process start, well before the game creates its device, so
   nothing is missed.
2. **`VSSetShader` + all four `Draw*` entry points** — per-shader draw counts and
   index/vertex volume, plus a one-time snapshot of the cbuffer sizes bound to VS
   slots 0–5 at each shader's first draw. The top ten by index volume are logged to
   `tewvr.log` every five seconds.
3. **`tools/dxbc_disasm.c`** — a standalone helper that disassembles the dumped
   blobs offline via `D3DDisassemble` (d3dcompiler_47).

One ~30-second gameplay walk produced 167 unique vertex shaders and clean
draw-volume rankings.

## Finding

World geometry is transformed by **per-draw MVP rows, named in the shader
reflection data**: every material vertex shader declares a slot-0 cbuffer
`constantBufferV` whose members are engine parameters with names like:

```
float4 vertexxyzscale;   // dequantise compressed vertex positions
float4 vertexxyzbias;
float4 mvpmatrixx;       // rows of M = P * V * Mmodel
float4 mvpmatrixy;
float4 mvpmatrixz;
float4 mvpmatrixw;
float4 modelmatrixx;     // model -> world (3x4)
...
```

`SV_Position` is computed as four `dp4`s of the (optionally skinned) model-space
position against the four `mvpmatrix*` rows. This is id Tech 5's renderprog
parameter system (`rpMVPmatrixX/Y/Z/W` in the RAGE/Doom 3 BFG sources), carried
into Tango's STEM engine on D3D11.

Key numbers:

- **145 of 167** captured vertex shaders consume `mvpmatrixx/y/z/w`.
- The byte offset of the rows varies per shader layout (32 is the commonest;
  observed range 0–192) — but every DXBC blob's RDEF chunk names it exactly.
- The 22 shaders without it are post-process/fullscreen work (`invaspectratio`,
  noise, `inversemvpmatrix`/`prevmvpmatrix` for SMAA and motion vectors,
  clip-space passthroughs) or depth-only variants that keep just `mvpmatrixw`.
- Skinning happens in model space *before* the MVP multiply (`jointBufferV`,
  the 8,064-byte palettes we saw at slots 2/3), so it is unaffected by any
  camera-side change.
- This also confirms 06b's conclusion: no shared cbuffer moves geometry; the
  96-byte most-bound buffer feeds lighting only.

## Why this is very good news for stereo

The dreaded "per-object engine" case turns out to have a uniform escape hatch.
Every draw's clip-space transform ends in `M = P * V * Mmodel`. To render an eye
we need `P_eye * T_eye * V * Mmodel` (with `T_eye` the half-IPD translation in
view space), and:

```
P_eye * T_eye * V * Mmodel  =  (P_eye * T_eye * P^-1) * (P * V * Mmodel)
                            =  K_eye * M
```

`K_eye` is **one constant 4x4 per eye per frame, identical for every object**.
So the stereo override needs no per-object knowledge at all: left-multiply each
per-draw MVP by `K_eye` before the constants reach the GPU. `P` is recoverable
from `g_fov` plus the aspect ratio (or from shaders that receive
`projectionmatrixz` directly).

## Open questions for Task 5/6

- Per-draw ordering: does the engine bind the vertex shader before filling
  `constantBufferV` (Map/DISCARD on rotating buffers vs `UpdateSubresource`)?
  That decides where the `K_eye` multiply gets applied (Unmap-time patch using
  the bound shader's reflection offset, vs hooking the engine's CPU-side MVP
  composition).
- Build the shader-hash → `mvpmatrix*` offset table from the dumped blobs (or
  run `D3DReflect` at `CreateVertexShader` time).
- The post shaders consuming `inversemvpmatrix`/`prevmvpmatrix` will eventually
  need consistent per-eye treatment (motion vectors, SMAA) — parked for later.
