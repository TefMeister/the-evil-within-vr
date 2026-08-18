# The winmm proxy loader

The mod's first working piece: a proxy `winmm.dll` that gets our code running
inside the game. Drop it next to `EvilWithin.exe`, and because Windows searches
the game's own folder before system32 for `winmm.dll`, the game loads ours. Ours
forwards every winmm call on to the real system DLL, so the game behaves
normally, and on load it writes a line to `%LOCALAPPDATA%\TEWVR\tewvr.log` to
prove we are in there.

## The one big lesson

We first tried to forward only the few winmm functions that `EvilWithin.exe`
itself imports. That failed immediately with
`Entry Point Not Found: waveOutPrepareHeader ... in bink2w64.dll`.

The reason is worth remembering for any DLL proxy: **it is not only the main
executable that imports from the DLL you are replacing.** Here, `bink2w64.dll`
(the Bink video library the game ships) pulls the `waveOut*` audio functions
from winmm. A proxy that only covers the exe's own imports leaves every other
module short. The fix — and the right default for proxying any system DLL — is
to forward the **entire** export set.

## How the full forwarding works

The real winmm has about 180 named exports. Rather than hand-write 180 C
wrappers (each needing the correct signature), we generate one assembly
jump-thunk per export: a tiny stub that jumps straight to a pointer slot filled
in at load time with the real function's address. A jump passes the arguments
and return value through untouched, so a single pattern covers all 180 functions
regardless of their signatures. One PowerShell generator emits the whole set
from a single list of names, so nothing can fall out of sync.

Two nice safety properties fell out of this:

- Each pointer slot starts pointing at a harmless logging stub, so even in the
  instant before the load-time setup runs, no call can jump into nowhere.
- If loading the real winmm fails, or one function can't be found, only those
  exports fall back to the stub — the game does not crash.

## Result

Built as a 64-bit DLL with the llvm-mingw toolchain we already had, the proxy
lets the game reach its title screen normally, with our log file written and the
game's process id recorded in it. The loader is done; the next task hooks
Direct3D 11 so we can start touching the rendering.
