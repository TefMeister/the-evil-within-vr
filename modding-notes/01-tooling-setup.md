# Tooling setup — and the DRM gotcha worth knowing

## The environment

- **Game:** The Evil Within (2014), Steam App ID `268050`. A 64-bit,
  Direct3D 11 executable of roughly 38 MB.
- **Debugger:** x64dbg (the `2026-05-27` snapshot), driven programmatically
  through the **x64dbg-automate** plugin (`v0.8.1`). The plugin's two files
  (`x64dbg-automate.dp64` and `libzmq-mt-4_3_5.dll`) go into x64dbg's
  `release\x64\plugins` folder.

## The gotcha: Steam DRM refuses a launch-time debugger

This one cost us a false start, so it is worth stating plainly.

`EvilWithin.exe` is wrapped in Steam's CEG DRM. If you launch the raw
executable **directly under the debugger**, the DRM stub refuses to decrypt and
throws a "Steam Error" dialog. Dropping a `steam_appid.txt` next to the
executable does **not** get you past this — the anti-tamper check is separate.

The reliable procedure is to let the game start normally first, then attach:

1. Launch through Steam: `steam://rungameid/268050`.
2. Wait for the "The Evil Within" window to appear. By now the DRM has already
   decrypted the real code into memory.
3. Attach the debugger to the live process.

Attaching to the already-running process works without complaint, because the
anti-debug check only runs at launch. This is not just a debugging trick: the
eventual VR mod will also need to inject into the already-running,
already-unwrapped process, so this is a permanent shape of the workflow.

## Turning on the console

- Add `+com_allowconsole 1` to the game's Steam launch options.
- In game, press **Insert** to open the console.
- `listcmds *` lists every command; `listcmds safe` lists the ones that do not
  trip cheat mode.

Nothing in this setup touches or copies any game file. It all works on the game
as installed and as running.
