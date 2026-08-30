# 03 — Live debug findings

Findings from the game running under x64dbg (attached after a normal Steam
launch, per note 01).

## Module layout and ASLR

- Base address of `EvilWithin.exe`: `0x7FF62BB20000`.
- This base was **identical across two separate launches**, i.e. the image is
  not being relocated by ASLR at runtime. Consequently, relative virtual
  addresses (RVAs) found during development remain valid between runs, which is
  a significant convenience for reverse engineering.
- The PE has 9 sections. The `.text` (code) section begins at RVA `0x1000`.

## Loaded modules of interest

Confirmed loaded into the live process:

- `d3d11.dll` and `dxgi.dll` — the Direct3D 11 render path is real and active.
- `xinput1_3.dll` and `dinput8.dll` — both input APIs are in use.
- `steam_api64.dll` — Steamworks integration.
- `bink2w64.dll` — Bink video playback.

A dedicated OS thread is named "Render Thread" (observed via the standard
`SetThreadName` exception, `0x406D1388`, which x64dbg logs harmlessly and
repeatedly). The existence of a separate, named render thread confirms where a
present/context hook for stereo rendering would live.

## Cvar table located in memory

Mapping the `g_fov` string from its raw file offset to its runtime address:

- The `.rdata` section's virtual-address-minus-file-offset delta is about
  `+0xE00` for this build.
- Applying that delta, the `g_fov` name string was read at virtual address
  `0x7FF62D0F3E18` (base + `0x15D3E18`).
- The bytes there are `67 5F 66 6F 76 00 00 00` (`"g_fov"`) immediately followed
  by `g_skipViewEffects` — the exact adjacency seen in the static string scan.

This confirms the engine's named-cvar table is present, intact, and at a stable
address in the live process. Reaching a specific cvar's value object from its
name is a pointer cross-reference, deferred to the implementation phase; for the
feasibility question, locating the intact table at a known address is enough.

## What this means for the mod

- **Stereo 6DOF core:** a Direct3D 11 present/context hook is the correct
  injection point, on the render thread identified here. This is the same class
  of problem solved by existing D3D VR-injection mods, so there is prior art to
  learn from, even though the work itself is substantial.
- **The "easy tier":** FOV, first-person, HUD, shadow, and view nodal offsets
  are exposed as cvars in an intact, addressable table, so they are reachable
  through the engine's own systems.

No game files were read from disk, copied, or modified. All work here operated
on the live process's own code and data in memory, which the debugger observes
in place.
