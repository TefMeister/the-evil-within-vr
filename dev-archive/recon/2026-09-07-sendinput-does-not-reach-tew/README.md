# 2026-09-07 — `SendInput` does not reach The Evil Within, and run 1 was the controller

`/lm`, dev PC. Two launches, driven with explicit permission.

## What happened

The profile recorded `SendInput` scancodes as `verified-live 2026-09-03`. On this machine, today,
they do not reach the game at all.

**Run 1** (controller connected) looked like it worked: the splash advanced, then the title, then
the menu. Then Enter would not select `CONTINUE` — and the highlight **moved to `NEW GAME` on its
own** while only Enters were being sent (`run1-highlight-moved-to-NEW-GAME-by-itself.png`).

**Run 2** (Tefa unplugged the DualSense): **nothing advances at all.** Enter does not dismiss the
photosensitivity splash, with the game window's foreground state **verified before each send**
(`sendkey.ps1` now refuses rather than sending blind).

**So run 1's advances were the controller, not the keyboard** — and the stray `NEW GAME` highlight
was stick drift. Every apparent keyboard success on this game today is explained by the pad.
`[verified-live 2026-09-07, n=2 launches]`

## Why, and it matches Prince of Persia exactly

`EvilWithin.exe` imports **`DINPUT8.dll`** and **`XINPUT1_3.dll`**. DirectInput8 keyboard is
precisely the path that does not see injected Windows input — established on
`prince-of-persia-2008-vr` the same day, where a key held 22 seconds across four logged samples
never appeared in the game's own state buffer. `[verified-numerically 2026-09-07]`

⚠️ It also imports `GetAsyncKeyState`, `GetKeyboardState`, `GetKeyState`, `GetMessageA` and
`SetWindowsHookEx`, so it has more input surface than PoP did and the DirectInput path is a
**strong lead, not a proven cause** here. Nothing has yet shown *which* path the menu reads.

## The fix that is already written

The `GetDeviceState` injector built for PoP today is generic DirectInput8 — it writes into the
state buffer the game itself asks for, inside its own read path, where focus and injection blocking
do not apply. **TEW is 64-bit**, so it needs a 64-bit build, and this game already has a proxy
(`winmm.dll`, with MinHook vendored) — so the hook belongs **inside the existing proxy** rather
than as a second DLL.

Source to port: `staging/prince-of-persia-2008-vr/proxy-dinput8/src/input_inject.{c,h}` (36 host
checks) and its `tools/pop_input.py` harness.

## Tool fix made here

`sendkey.ps1` set the foreground window but never **verified** it, so a failed
`SetForegroundWindow` would send keys to whatever else owned the foreground and read as "the game
ignores input". It now retries up to 12 times and **refuses** rather than sending blind. The
capture script already did this; the send script did not.
