# Live debug findings — what the running game told us

With the game running under the debugger (attached after a normal Steam launch),
we confirmed the static findings and learned a few new things.

## No ASLR — addresses stay put

The executable loaded at `0x7FF62BB20000`, and it was the **same address on two
separate launches**. In other words, the image is not relocated at runtime.
This is a gift for reverse engineering: any address we note down during
development is still valid the next time we run the game.

## The render path is real, and it has its own thread

`d3d11.dll` and `dxgi.dll` are both loaded in the live process, and there is a
dedicated OS thread literally named "Render Thread". That thread is where a
present/context hook for stereo rendering would live. (Aside: x64dbg logs a
flood of harmless `SetThreadName` exceptions from this thread — the standard
`0x406D1388` naming exception — which look alarming but are nothing.)

Both `xinput1_3.dll` and `dinput8.dll` are also loaded, confirming the input
layer we will later redirect to motion controllers.

## We found the cvar table in live memory

To turn the `g_fov` string's file offset into a runtime address, we worked out
the `.rdata` section's offset-to-address delta (about `+0xE00` for this build)
and read the predicted spot. There it was: the bytes for `"g_fov"` at
`0x7FF62D0F3E18`, immediately followed by `g_skipViewEffects` — the exact
neighbour we had seen in the static scan.

That confirms the engine's named-cvar table is present, intact, and sitting at a
stable, known address in the running game. Getting from a cvar's name to its
live value is a further pointer hop, which we have left for the implementation
phase; for a feasibility check, finding the intact table is the point.

## What we take away

- **For stereo:** hook Direct3D 11 on the render thread. This is a known class
  of problem with existing mods to learn from, even though the work is real.
- **For the easy tier:** FOV, first-person, HUD, shadow, and eye offsets are
  exposed as cvars in an intact, addressable table.

As with the rest of these notes, everything here was observed in the live
process's own memory. No game file was read from disk, copied, or changed.
