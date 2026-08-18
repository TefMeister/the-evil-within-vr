# Hooking Direct3D 11's Present

The first real hook into the renderer. Every frame, the game calls
`Present` on its Direct3D swap-chain to put the finished image on screen. If we
hook that one function, we get a dependable "end of frame" callback and a handle
to the swap-chain — which is exactly where the stereo rendering will eventually
live.

## The neat trick: borrow the address from the vtable

You might expect we'd have to hunt for `Present` in memory. We don't. Direct3D
objects are COM objects, and every instance of a class shares one virtual method
table. So we create a **throwaway** device and swap-chain of our own (a hidden
1×1 window that never actually shows anything), read entry number 8 of its
vtable — that's `Present` — and then throw the dummy objects away. The address
we read is the same function the game's real swap-chain uses, so hooking it
catches the game's own frames.

We resolved `Present` and immediately saw our hook firing on the game's render
thread — a different thread from the one that installed the hook — ticking every
single frame, thousands of frames deep, with the game still responsive. That's
the confirmation that we're on the true render path and not some side channel.

## Doing it safely

- **Not inside DllMain.** Installing a hook from `DllMain` can deadlock on the
  Windows loader lock, so instead `DllMain` starts a tiny helper thread that
  waits for Direct3D to be loaded and installs the hook from there.
- **Clean teardown.** On unload we disable the hook before the logger shuts
  down, so the render thread stops calling our code before anything it needs
  disappears.
- **Fail-safe.** If anything in the setup fails — the dummy device, MinHook,
  the hook install — we log it once and quietly do nothing, and the game runs
  normally. Better no stereo than a crash.

## The tool

We use MinHook (by Tsuda Kageyu) to place the actual trampoline. It's a small,
permissively licensed C library that builds cleanly with the toolchain we
already have. It's vendored with its licence and credited.

Next up: capturing the game's real device and back-buffer from inside the hook,
so we know the resolution and have the handles the stereo rendering needs.
