# 04 — The winmm proxy loader (Task 1)

The first piece of the mod: a proxy `winmm.dll` that loads our code into the
running game and proves it is there by writing a log file. This is the loader
that every later task builds on.

## What it does

- Placed next to `EvilWithin.exe`, the game loads it at startup (Windows
  searches the executable's own directory before system32 for `winmm.dll`,
  which is not a protected KnownDLL).
- It forwards every `winmm` call on to the real system DLL, so the game runs
  exactly as it would without us.
- On load it writes an attach banner to `%LOCALAPPDATA%\TEWVR\tewvr.log`,
  including the game's process id.

## The forwarding approach

The proxy forwards **all 180 named exports** of the real
`C:\Windows\System32\winmm.dll`, resolved at runtime by absolute path (we never
copy or redistribute the system DLL). The mechanism is a generated set of
assembly jump-thunks: for each export `NAME`, a global symbol `NAME` that does
`jmp *ptr_NAME(%rip)`, where `ptr_NAME` is a pointer slot. Every slot is
statically initialised to a safe logging stub, then upgraded at load to the real
function pointer via `GetProcAddress` (only if that succeeds). A jump-thunk
passes arguments and return values through untouched, so it works for all 180
functions without needing a single hand-written prototype.

Everything is generated from one list of names by
`tools/gen_winmm_forwarders.ps1`, which emits the `.h`, `.c`, `.s`, and `.def`
together, so the four artefacts can never drift apart.

## The lesson that cost us a first attempt

The first attempt forwarded only the handful of winmm functions that
`EvilWithin.exe` itself imports. It failed: the game threw
`Entry Point Not Found: waveOutPrepareHeader ... in bink2w64.dll`. The cause is
that **other modules the game loads also import winmm** — `bink2w64.dll` (the
Bink video library) pulls the `waveOut*` audio family from it. A winmm proxy
must satisfy *every* module in the process that imports winmm, not just the main
executable. Forwarding the complete export set is the robust answer, and it is
what a proxy of a system DLL should always do.

## Fail-safe behaviour

- Because each forwarder slot is statically initialised to a logging stub, the
  jump target is valid even in the brief window before the load-time
  initialiser runs — there is no moment where a forwarded call jumps to a null
  pointer.
- If `LoadLibraryW` of the real winmm fails, or a single `GetProcAddress`
  returns null, the affected exports stay on their logging stubs rather than
  crashing the game.

## Toolchain and result

- Built as a 64-bit `winmm.dll` with the llvm-mingw toolchain
  (`gcc -O2 -shared -static`); the static link means the game needs no extra
  runtime DLLs.
- Confirmed live: with the proxy in place, the game reaches its
  "The Evil Within" window and `tewvr.log` contains the attach banner with a
  matching process id.

## A note on launching

Launching the raw executable directly works when no debugger is attached: a
`steam_appid.txt` alongside the executable satisfies the Steamworks check, and
the CEG anti-tamper only objects to a launch-time *debugger* (see note 01). The
mod is a DLL, not a debugger, so it loads cleanly on a normal start.

No game files were read, copied, or modified. The only file we place in the game
folder is our own `winmm.dll`.
