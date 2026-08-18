# 05 — The Direct3D 11 Present hook (Task 2)

The first hook into the game's renderer. `IDXGISwapChain::Present` is the call
the game makes at the end of every frame to show the finished image; hooking it
gives us a reliable per-frame entry point and the swap-chain handle, which is
where the stereo work will live.

## How we get the Present address

We never have to find `Present` by scanning — we borrow it from Direct3D's own
vtable. `hooks_install()` creates a **throwaway** device and swap-chain with
`D3D11CreateDeviceAndSwapChain` (a hidden 1×1 window, `OutputWindow =
GetDesktopWindow()`, never actually presented), reads the swap-chain's virtual
method table, and takes entry **index 8**, which is `IDXGISwapChain::Present`.
The dummy device, context, and swap-chain are released immediately afterwards.

This works because a COM object's vtable is shared by every instance of that
class: the address we read from our throwaway swap-chain is the very same
function the game's real swap-chain uses. So a single MinHook trampoline placed
on it catches the game's own `Present` calls.

The code is compiled as C, using the C-style COM interface (`COBJMACROS`), so
all the `IDXGISwapChain`/`ID3D11Device` calls are plain `lpVtbl->` calls.

## What we observed live

- Resolved `Present` at `0x00007FF8922018C0` this session. (This is an address
  inside the system `d3d11.dll`; it will differ across machines, drivers, and
  Windows versions, but it was stable across repeated launches on this
  machine — a per-session constant, not something to hard-code.)
- The hook then fired on the game's **render thread** — a distinct OS thread
  from the bootstrap thread that installed the hook and from the main attach
  thread — ticking every frame, past 6,361 frames continuously, with the game
  responsive throughout. That confirms we are on the real render path.

## Where the hook is installed from

Hooking must not happen inside `DllMain` (the loader lock makes that unsafe). So
`DllMain` spawns a small **bootstrap thread** that waits for `d3d11.dll` to be
loaded (polling `GetModuleHandleW`, with a timeout) and only then calls
`hooks_install()`. On unload, `hooks_remove()` disables the hook before the
logger shuts down, so the render thread stops calling into our code before the
logger goes away.

A thin `minhook_glue` wrapper centralises the MinHook calls and turns MinHook
status codes into log messages, so later tasks can add hooks without repeating
the boilerplate.

## Fail-safe

Every failure point — the dummy device failing to create, MinHook failing to
initialise, or the hook failing to install — is logged once and then
`hooks_install()` simply returns with nothing hooked. The game keeps running
normally (mono) rather than crashing. This is the same safety posture the whole
mod follows.

## Tooling note

MinHook (Tsuda Kageyu) is vendored under `third_party/minhook/` with its
licence. It is plain C with no MSVC-only intrinsics, so it builds cleanly under
the llvm-mingw toolchain with no source changes. It is credited in CREDITS.

No game files were read, copied, or modified. The only file placed in the game
folder is our own `winmm.dll`.
