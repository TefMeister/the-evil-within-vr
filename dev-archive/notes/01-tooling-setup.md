# 01 — Tooling setup

The environment used for the feasibility spike, recorded so it can be
reproduced.

## Target

- **Game:** The Evil Within (2014), Steam App ID `268050`.
- **Executable:** `EvilWithin.exe`, 64-bit, roughly 38 MB.
- **Renderer:** Direct3D 11 (`d3d11.dll`, `dxgi.dll`, `d3dcompiler`).
- **Ships alongside:** `bink2w64.dll` (Bink video), `cudart64_40_17.dll` (CUDA
  runtime, most likely used for megatexture transcoding), `steam_api64.dll`.

## Debugger

- **x64dbg**, snapshot build `2026-05-27`, installed to
  `%LOCALAPPDATA%\x64dbg`.
- **x64dbg-automate** plugin, `v0.8.1-ghost_fungus`, installed by copying
  `x64dbg-automate.dp64` and `libzmq-mt-4_3_5.dll` into
  `release\x64\plugins`. This plugin exposes x64dbg's bridge over a local
  RPC channel, which is what lets the debugger be driven programmatically.

## The DRM workaround (important)

`EvilWithin.exe` is Steam CEG-wrapped. Launching the raw executable **directly
under the debugger** triggers a "Steam Error" dialog — the DRM stub refuses to
unwrap under a launch-time debugger, even with a `steam_appid.txt` present.

The working procedure is:

1. Launch the game normally through Steam: `steam://rungameid/268050`.
2. Wait for the game window ("The Evil Within") to appear; by this point the
   DRM has decrypted the real code in memory.
3. Attach the debugger to the running process (in x64dbg: `attach <pid_hex>`).

Attaching to the already-running process works cleanly — the anti-debug check
is a launch-time check only. The eventual VR injector will also target the
already-running, already-unwrapped process, so this is a permanent property of
the workflow rather than a one-off inconvenience.

## Console access

- Add `+com_allowconsole 1` to the game's Steam launch options.
- In game, open the console with the **Insert** key.
- `listcmds *` lists all commands; `listcmds safe` lists the commands that do
  not enable cheat mode.

No game files were copied, extracted, or modified during setup. Everything
above operates on the game as installed and running.
