# Stereo 6DOF Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render The Evil Within's scene twice per frame with correct per-eye view/projection matrices and present it side-by-side on the flat monitor, as the foundation for the VR mod.

**Architecture:** A `winmm.dll` proxy placed next to the game loads our code past the Steam DRM (which unwraps before our DLL loads). From there, MinHook installs trampoline hooks on the D3D11/DXGI methods we need — obtained by momentarily creating a throwaway device/swapchain to read their vtables. We intercept the camera view/projection constant-buffer upload, then drive a true double-render (real geometry, twice) into the left and right halves of the back-buffer.

**Tech Stack:** C (C11), MinHook (trampoline hooking, MIT-licensed, by Tsuda Kageyu), Direct3D 11 / DXGI. Compiler: **llvm-mingw** (clang/gcc, UCRT, x86_64) — already installed on this PC at `%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT_...\llvm-mingw-*-ucrt-x86_64\bin` and on PATH as `gcc`; it ships the DirectX headers (`d3d11.h`, `dxgi.h`). MSVC `cl.exe` is a valid alternative but the C++ workload is not currently installed. PowerShell build script. Output artifact: `winmm.dll` (64-bit).

**Spec:** `dev-archive/design/2026-08-18-stereo-6dof-core-design.md`

> **Spec refinement (loader proxy):** The spec names `dxgi.dll` as the proxy. This plan uses `winmm.dll` as the *loader* proxy instead, because DXGI's ordinal exports make a direct DXGI proxy fragile. The approach the spec approved — proxy DLL + MinHook vtable hooks + true double-render — is unchanged; only the proxied DLL differs. The spec should be updated to match once this is agreed.

## Global Constraints

- **Never commit or upload original game files.** Only files we create. The `.gitignore` blocks binaries/assets as a safety net.
- **The `the-evil-within-vr-mod` repo is push-gated.** All source in this plan is created in a local clone of that repo; **nothing is pushed to it without explicit user approval.** The dev-archive and modding-notes repos are updated (and pushed) freely.
- **Dev PC performance is non-diagnostic.** Low frame rate from double-rendering is expected and is never treated as a bug. Correctness is judged by eye; a frozen or black eye or inverted parallax is a real bug, a low frame rate is not.
- **The save file is disposable.** Testing may freely use `devmapjump`, new games, and console cheats to reach gameplay; no need to protect the save.
- **Fail-safe always.** Any hook or discovery failure must fall through to the game's normal mono rendering; the mod must never crash or hang the game.
- **British spelling** in any pushed prose (README, notes, commit messages), to match existing repos.
- **Update the ledger after every notable success or failure** (dev-archive + modding-notes), promptly.
- **Launch/attach procedure:** launch the game via `steam://rungameid/268050` (App ID 268050), never the raw exe under a debugger (Steam CEG DRM refuses a launch-time debugger). Attach after the window appears.
- **Console:** `+com_allowconsole 1` launch option; open with the Insert key in game.

## Repository file structure

All paths are relative to the `the-evil-within-vr-mod` repo root (local clone; push-gated).

```
proxy-winmm/
  src/
    dllmain.c            — DllMain; boots logging, real-winmm resolution, and the hook thread
    winmm_forward.c      — runtime forwarders to the real system winmm.dll
    winmm_forward.h
    log.c / log.h        — thread-safe file logger
    minhook_glue.c       — MinHook init/teardown helpers
    hooks.c / hooks.h    — dummy-device vtable capture; Present + constant-buffer hooks
    d3d_capture.c/.h     — captured ID3D11Device/Context/SwapChain and backbuffer info
    camera.c / camera.h  — matrix types, eye-offset math, view/proj override state
    stereo_core.c/.h     — RenderFrame(): per-eye loop and the IStereoSink seam
    backbuffer_sink.c/.h — IStereoSink impl: side-by-side into the game back-buffer
    config.c / config.h  — TEWVR_* environment-variable knobs
  third_party/
    minhook/             — vendored MinHook source (MIT); see CREDITS
  tools/
    gen_winmm_forwarders.ps1  — emits winmm_forward.c from the real winmm export names
  build.ps1              — MSVC build → winmm.dll
  USAGE.md               — install and usage, with the "what is / isn't confirmed" list
```

Testing note for this domain: this is native code that only exercises meaningfully against the live game, so classic unit tests do not apply. Each task's "test" is a concrete runtime verification against the running game with an explicit pre-state (what you should see before the code exists) and expected observation (log line or on-screen result). Treat those as the red/green gates.

---

### Task 1: winmm proxy that loads into the game and logs

Boot our DLL inside the game by masquerading as `winmm.dll`, forwarding every export the game imports to the real system winmm, and writing a log file to prove we are running in-process.

**Files:**
- Create: `proxy-winmm/src/log.c`, `proxy-winmm/src/log.h`
- Create: `proxy-winmm/src/winmm_forward.c` (generated), `proxy-winmm/src/winmm_forward.h`
- Create: `proxy-winmm/src/dllmain.c`
- Create: `proxy-winmm/tools/gen_winmm_forwarders.ps1`
- Create: `proxy-winmm/build.ps1`

**Interfaces:**
- Produces: `void log_init(void)`, `void log_msg(const char *fmt, ...)`, `void log_shutdown(void)` — the logger used by every later task. Log file path: `%LOCALAPPDATA%\TEWVR\tewvr.log` (created if missing).
- Produces: a loaded `winmm.dll` whose `DllMain` calls `log_init()` and logs a start banner.

- [ ] **Step 1: Toolchain check**

Confirm the C toolchain is available:
```
gcc --version    # llvm-mingw clang, already installed and on PATH
```
Expected: prints the clang/llvm-mingw version. Also confirm the DirectX headers resolve:
```
echo '#include <d3d11.h>' | gcc -x c -fsyntax-only -   # expected: no error
```
If `gcc` is missing, either use the installed llvm-mingw `bin` directly, or install the MSVC "Desktop development with C++" workload and use `cl` instead (the build script supports both; MSVC path is a drop-in). Do not proceed until a compiler builds a trivial DLL.

- [ ] **Step 2: Enumerate the real winmm exports (for forwarding)**

We forward to the real system winmm without redistributing it, by resolving its exports at runtime. Generate the forwarder source from the system DLL's export names (we read the system DLL only to read names; we never copy or ship it):
```
proxy-winmm/tools/gen_winmm_forwarders.ps1
```
This script (write it in this step) lists `C:\Windows\System32\winmm.dll`'s named exports — using `llvm-nm --extern-only --defined-only` / `llvm-objdump -p`, or a self-contained PowerShell PE export-table parser (no `dumpbin` dependency, since MSVC is not installed) — and emits:
- `winmm_forward.h`: `void winmm_forward_init(void);` plus an `extern` function-pointer table.
- `winmm_forward.c`: for each named export `NAME`, a `__declspec(dllexport)` stub `NAME` that jumps to the resolved real pointer, and `winmm_forward_init()` which `LoadLibraryW(L"C:\\Windows\\System32\\winmm.dll")` and `GetProcAddress`-resolves every pointer.

Verify: running the script prints the count of exports and writes both files.

- [ ] **Step 3: Write the logger**

`log.h`:
```c
#pragma once
void log_init(void);
void log_msg(const char *fmt, ...);
void log_shutdown(void);
```
`log.c`: open the log file under `%LOCALAPPDATA%\TEWVR\` (create the directory with `SHCreateDirectoryExW` or `CreateDirectoryW`), append mode, flush after every message, guarded by a `CRITICAL_SECTION`. Prefix each line with a millisecond timestamp (`GetTickCount64`) and the current thread id.

- [ ] **Step 4: Write `dllmain.c`**

```c
#include <windows.h>
#include "log.h"
#include "winmm_forward.h"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        winmm_forward_init();   /* resolve real winmm first, so forwards work immediately */
        log_init();
        log_msg("TEWVR winmm proxy attached (pid=%lu)", GetCurrentProcessId());
    } else if (reason == DLL_PROCESS_DETACH) {
        log_msg("TEWVR winmm proxy detaching");
        log_shutdown();
    }
    return TRUE;
}
```

- [ ] **Step 5: Write `build.ps1`**

Compile all `src/*.c` into a 64-bit `winmm.dll` with llvm-mingw (`gcc`). The winmm stubs are exported via `__declspec(dllexport)` (in `winmm_forward.c`). Statically link the runtime so the game has no extra dependency. Link `user32`, `shell32`, `d3d11`, `dxgi` (added in later tasks). Emit into `proxy-winmm/build/`.
```powershell
# build.ps1 (essentials — llvm-mingw)
$ErrorActionPreference = "Stop"
$out = "$PSScriptRoot\build"; New-Item -ItemType Directory -Force $out | Out-Null
$src = Get-ChildItem "$PSScriptRoot\src\*.c" | ForEach-Object { $_.FullName }
$inc = "-I`"$PSScriptRoot\third_party\minhook\include`""
# -shared builds the DLL; -static links libgcc/CRT; output name winmm.dll
gcc -O2 -shared -static -o "$out\winmm.dll" $inc $src `
    -luser32 -lshell32 -ld3d11 -ldxgi -lole32
if ($LASTEXITCODE -ne 0) { throw "build failed" }
```
(If using MSVC instead, the equivalent is `cl /LD /MT /O2 src\*.c /Fe:build\winmm.dll user32.lib shell32.lib d3d11.lib dxgi.lib` — same artifact.)

- [ ] **Step 6: Build**

Run: `powershell -File proxy-winmm/build.ps1`
Expected: `build/winmm.dll` is produced with no errors.

- [ ] **Step 7: Runtime test — pre-state**

With no `winmm.dll` in the game folder, confirm the log file does **not** exist:
```
Test-Path "$env:LOCALAPPDATA\TEWVR\tewvr.log"   # expected: False
```

- [ ] **Step 8: Runtime test — install and launch**

Copy `build/winmm.dll` next to `EvilWithin.exe`, then launch via Steam:
```
Copy-Item proxy-winmm/build/winmm.dll "D:\Program Files (x86)\Steam\steamapps\common\TheEvilWithin\winmm.dll"
Start-Process "steam://rungameid/268050"
```
Expected observations:
1. The game reaches its menu normally (proxy forwarding works; no missing-export crash).
2. `%LOCALAPPDATA%\TEWVR\tewvr.log` now exists and contains the "winmm proxy attached" banner with the game's PID.

If the game fails to start with a missing-export error, the forwarder list is incomplete — re-run Step 2 and confirm every winmm import of `EvilWithin.exe` (check its import table with `dumpbin /imports`) is exported by our DLL.

- [ ] **Step 9: Update the ledger**

Add a dev-archive note (`notes/04-proxy-loader.md`) and a modding-notes entry recording: winmm chosen as loader proxy, the forwarding approach, and confirmation the proxy loads and the game runs. Commit + push the two doc repos (British spelling; grammar-checked).

- [ ] **Step 10: Commit (mod repo, local only — do not push)**

```
git add proxy-winmm
git commit -m "Task 1: winmm proxy loader with runtime forwarders and logging"
```
Do **not** push `the-evil-within-vr-mod`.

---

### Task 2: MinHook + Present hook via a dummy swap-chain

Acquire the `IDXGISwapChain::Present` address by creating a throwaway device/swap-chain, read its vtable, and install a MinHook trampoline. Prove the hook fires once per frame.

**Files:**
- Create: `proxy-winmm/third_party/minhook/` (vendored MinHook source)
- Create: `proxy-winmm/src/minhook_glue.c`
- Create: `proxy-winmm/src/hooks.c`, `proxy-winmm/src/hooks.h`
- Modify: `proxy-winmm/src/dllmain.c` (spawn a bootstrap thread that installs hooks)
- Modify: `proxy-winmm/build.ps1` (compile MinHook + define its include path)

**Interfaces:**
- Consumes: `log_msg` (Task 1).
- Produces: `void hooks_install(void);` and `void hooks_remove(void);`.
- Produces: `typedef HRESULT (STDMETHODCALLTYPE *Present_t)(IDXGISwapChain*, UINT, UINT);` and a stored `Present_t g_present_orig;` — later tasks call through this.

- [ ] **Step 1: Vendor MinHook**

Add MinHook's `src/` and `include/` under `third_party/minhook/`. Add MinHook (Tsuda Kageyu, MIT) to `CREDITS.md` in both doc repos and the mod repo. Verify the license text is included under `third_party/minhook/`.

- [ ] **Step 2: Write the dummy-device vtable capture**

In `hooks.c`, `hooks_install()`:
```c
/* Create a hidden 1x1 swapchain to read the vtable, then release it. */
DXGI_SWAP_CHAIN_DESC scd = {0};
scd.BufferCount = 1;
scd.BufferDesc.Width = 1; scd.BufferDesc.Height = 1;
scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
scd.OutputWindow = GetDesktopWindow();   /* transient; never presented */
scd.SampleDesc.Count = 1;
scd.Windowed = TRUE;
ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; IDXGISwapChain *sc = NULL;
D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
    &fl, 1, D3D11_SDK_VERSION, &scd, &sc, &dev, NULL, &ctx);
/* vtable[8] == Present */
void **vtbl = *(void***)sc;
Present_t present = (Present_t)vtbl[8];
```
Log the resolved `present` address. Release `sc`, `dev`, `ctx` immediately after reading the pointer (the vtable is shared by the game's real swap-chain, so the hook still applies).

- [ ] **Step 3: Write the Present hook**

```c
static Present_t g_present_orig = NULL;
static UINT64 g_frame = 0;
HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if ((g_frame++ % 120) == 0) log_msg("Present hook alive: frame %llu", g_frame);
    return g_present_orig(sc, sync, flags);
}
```
Install via MinHook: `MH_Initialize()`, `MH_CreateHook(present, &Hook_Present, (void**)&g_present_orig)`, `MH_EnableHook(present)`.

- [ ] **Step 4: Spawn the bootstrap thread from DllMain**

Hooking must not run inside `DllMain`. In `DLL_PROCESS_ATTACH`, `CreateThread` a bootstrap that waits briefly for D3D to be ready (poll for `d3d11.dll` module presence), then calls `hooks_install()`.

- [ ] **Step 5: Build**

Run: `powershell -File proxy-winmm/build.ps1`
Expected: builds clean, linking `d3d11.lib dxgi.lib` and MinHook.

- [ ] **Step 6: Runtime test**

Pre-state: current `tewvr.log` has no "Present hook alive" lines. Install the new `winmm.dll`, launch via Steam, reach the menu.
Expected: `tewvr.log` shows the resolved Present address once, then "Present hook alive: frame N" ticking roughly every 120 frames. The game renders and is interactive.

- [ ] **Step 7: Ledger + commit**

Dev-archive note `notes/05-present-hook.md` (Present address, that the dummy-device vtable technique worked). Commit mod repo locally (no push).

---

### Task 3: Capture the game's real device, context, and back-buffer

From inside the Present hook, capture the game's actual `IDXGISwapChain`, its `ID3D11Device` and immediate `ID3D11DeviceContext`, and the back-buffer description (resolution/format). These are the handles every later task renders with.

**Files:**
- Create: `proxy-winmm/src/d3d_capture.c`, `proxy-winmm/src/d3d_capture.h`
- Modify: `proxy-winmm/src/hooks.c` (populate capture on first Present)

**Interfaces:**
- Produces: a `struct D3DCapture { IDXGISwapChain *sc; ID3D11Device *dev; ID3D11DeviceContext *ctx; UINT width, height; DXGI_FORMAT format; }` and `extern struct D3DCapture g_d3d;` plus `bool d3d_capture_ready(void);`.

- [ ] **Step 1: Populate capture on first Present**

In `Hook_Present`, once only: store `sc`; `sc->GetDevice(IID_ID3D11Device, &g_d3d.dev)`; `dev->GetImmediateContext(&g_d3d.ctx)`; `sc->GetDesc(&scd)` to fill width/height/format. Log them.

- [ ] **Step 2: Build + runtime test**

Expected log (once): the game's real render resolution and format (e.g. `1920x1080 DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` or the game's actual back-buffer), matching the resolution you launched at. `d3d_capture_ready()` returns true thereafter.

- [ ] **Step 3: Ledger + commit**

Note the captured resolution/format in `notes/05-present-hook.md`. Commit locally.

---

### Task 4 (discovery): locate the camera view + projection constant buffer

This is a reverse-engineering discovery task. Its deliverable is documented knowledge — the constant-buffer slot, byte offset, and matrix layout of the camera view and projection matrices — plus a hook that reliably logs the live view matrix. There is no pre-written matrix address; the method and the done-criteria below are the gate.

**Files:**
- Modify: `proxy-winmm/src/hooks.c` (add temporary constant-buffer logging hooks)
- Create: dev-archive `notes/06-camera-matrix-discovery.md` (the deliverable)

**Interfaces:**
- Produces (documented, consumed by Task 6): which API uploads the camera matrices (`UpdateSubresource` vs `Map`/`Unmap` vs `VSSetConstantBuffers` slot), the byte offset of the view matrix and of the projection matrix within that buffer, and their layout (row- vs column-major; whether the stored matrix is view, view-projection, or inverse).

- [ ] **Step 1: Hook the three constant-buffer upload paths (temporary, logging only)**

Install MinHook trampolines on `ID3D11DeviceContext::UpdateSubresource`, `::Map`/`::Unmap`, and `::VSSetConstantBuffers` (vtable indices resolved from the dummy device the same way as Present). For buffers in the size range of a camera cbuffer (64–512 bytes), log the buffer pointer and a hexdump when a debug key (e.g. `TEWVR_DUMP` env flag) is set, throttled to a few per second.

- [ ] **Step 2: Identify the matrix by motion correlation**

Launch, reach gameplay (`devmapjump st01_main` or a new game; the save is disposable). Rotate the camera smoothly and watch the dumps: the view matrix is the 64-byte block whose upper-left 3×3 changes as a rotation while the bottom row / translation tracks the player position. Confirm a second block behaves like a projection matrix (constant while only rotating; its `[0][0]`/`[1][1]` scale with `g_fov`). Cross-check by changing `g_fov` in the console and watching the projection entries move.

- [ ] **Step 3: Corroborate in x64dbg**

Attach x64dbg (after Steam launch), set a memory breakpoint on the identified cbuffer source address, and confirm the writing code is the engine's camera/view setup (not an unrelated buffer). Record the writing instruction address (RVA; stable — no ASLR on this build).

- [ ] **Step 4: Document the deliverable**

Write `notes/06-camera-matrix-discovery.md`: the upload API used, buffer slot, byte offsets of view and projection, layout/handedness, the writer RVA, and a sample logged matrix with the camera pose it corresponded to. Push the doc repos.

- [ ] **Step 5: Done-criteria gate**

The task is done when the log reliably prints a 4×4 view matrix whose rotation tracks camera aim and whose translation tracks player position, across at least two gameplay locations, and the projection matrix responds to `g_fov`. If the matrix cannot be isolated this way, stop and escalate — do not guess.

- [ ] **Step 6: Commit**

Commit the temporary logging hooks in the mod repo locally (they will be narrowed in Task 6).

---

### Task 5 (discovery): choose and prove the double-render strategy

Decide how to produce the second eye. Preferred: re-invoke the engine's scene render with a second camera. Fallback (per the spec's risk section): re-issue the frame's draw calls with the second eye's matrices bound. The deliverable is a proven, documented decision.

**Files:**
- Create: dev-archive `notes/07-double-render-strategy.md` (the deliverable)
- Modify: `proxy-winmm/src/hooks.c` (a minimal spike of the chosen mechanism)

**Interfaces:**
- Produces (documented, consumed by Task 7): the exact hook point and mechanism for rendering the scene a second time within one Present interval, and whether the engine tolerates re-invocation.

- [ ] **Step 1: Locate the scene-render boundary**

Using x64dbg on the Render Thread, find the per-frame scene-render call that runs between frames (the function whose body issues the bulk of the draw calls, upstream of Present). Record its RVA.

- [ ] **Step 2: Re-entrancy probe**

Cheapest viable test of the preferred mechanism: from a hook just before the engine presents, attempt to invoke the scene-render a second time with the camera matrix overridden (using Task 4's knowledge), rendering into an off-screen or full-frame target, and observe stability over ~30 seconds of gameplay. Watch for corruption, crashes, or degenerate output (frozen/black), which indicate cached per-frame state.

- [ ] **Step 3: Decide**

If re-invocation is stable, adopt it. If not, adopt draw-call-level duplication: in Task 7, the Present-interval hook replays the recorded draw calls (or triggers a second engine pass) with the second eye's matrix bound. Document which, and why, in `notes/07-double-render-strategy.md`.

- [ ] **Step 4: Gate + commit + push docs**

Done when a second full render of the scene has been produced on screen at least once without destabilising the game. Commit the spike locally; push the doc note.

---

### Task 6: Per-draw MVP override — the keystone camera-ownership proof

> **Supersedes the original task text below the design line.** The original
> "one view/proj upload path" design is dead — ruled out by the ledger on
> 2026-08-20 (no shared view/proj buffer exists; every draw carries its own
> MVP; the world is drawn on deferred contexts). This rewrite reflects the
> decided design from the Task 6 discovery ledger entries
> (`.superpowers/sdd/2026-08-18-stereo-6dof-core-plan/progress.md`,
> 2026-08-20 entries). This IS the Playbook Phase 4 keystone proof — the
> feasibility go/no-go for the whole conversion.

**Design (decided):** patch the MVP of each deferred-recorded world draw at
record time. At each deferred `DrawIndexed`, read the currently-bound VS
slot-0 constant buffer's MVP rows at `mvp_offset_for_shader(current_vs)`
(Task 5's reflection table), left-multiply by a constant test matrix `K`,
write the patched rows into our own scratch constant buffer, rebind VS slot
0 to it, then call the original draw — so the substitution is what gets
recorded into the command list and replayed. Skip (call the original draw
unpatched) if the current VS has no known offset. `K` here is a pure test
rotation (`TEWVR_TEST_YAW`) — not yet a real per-eye matrix; that is Task 7/8,
which the ledger notes effectively merge with this patch mechanism (same
substitute-and-rebind point, `K_eye` instead of a test yaw, executed twice
per draw into each half-viewport).

**Files:**
- Create: `proxy-winmm/src/mvp_patch.c`, `proxy-winmm/src/mvp_patch.h`
- Modify: `proxy-winmm/src/camera.c`, `proxy-winmm/src/camera.h` (repurposed
  to generic 4x4 matrix helpers — drop the `camera_set_override`/view-proj
  design, it does not apply to this engine)
- Modify: `proxy-winmm/src/hooks.c` (install the Draw/DrawIndexed hooks
  alongside the existing late-hook machinery from Task 5 addendum 3)

**Interfaces:**
- Produces: `Mat4`, `Mat4 mat4_mul(Mat4 a, Mat4 b)`, `Mat4 mat4_rotation_y(float degrees)`,
  `Mat4 mat4_translation_local(const Mat4 *view, float dx)` (Task 8 reuses both).
- Produces: `void mvp_patch_install(void)`, `void mvp_patch_remove(void)`.
- Consumes: `mvp_offset_for_shader` (Task 5's `mvptable.c`), the address-keyed
  late-hook dispatch table (Task 5 addendum 3, `g_hooked_funcs`), `d3d_capture` (Task 3).

- [ ] **Step 0 (required first — resolve the read-mechanism risk before writing the patch loop):**

  The Task 6 discovery probe (`TEWVR_CBPEEK`) read draw-time cb0 contents via
  a GPU staging-copy readback. That was safe as a one-shot discovery capture,
  but calling a synchronous staging-copy readback on every one of ~1900
  deferred draws/frame is a different proposition: it is likely far too slow
  for real-time use, and a `CopyResource`+`Map` readback is normally an
  immediate-context/GPU-round-trip operation — issuing it from inside a
  deferred-context Draw hook (a worker thread) risks a severe stall or a
  threading violation. This was not resolved by the discovery work and must
  be settled before Step 1:
  (a) **First choice:** capture the buffer's CPU-writable pointer directly.
      The ~6 pool buffers were never seen being `Map`ped during gameplay
      capture, meaning the persistent map happened before our hooks were
      live — but check whether the pool is recreated on a level/scene
      transition (a fresh `Map` after our hooks are installed); if so, latch
      the returned pointer with a proper `AddRef` on the buffer (this also
      fixes the CBPEEK review's Important dangling-pointer finding) and read
      it directly with no GPU op.
  (b) **If no in-hook Map ever fires**, try hooking `ID3D11Device::CreateBuffer`
      (not yet hooked) to catch the pool buffers' creation and immediately
      issue our own `Map` to capture the pointer at creation time, then never
      `Unmap` (mirroring the engine's own pattern). Verify the engine
      tolerates a foreign `Map` on its own buffer without conflict — test
      carefully, the save is disposable if this destabilises the game.
  (c) **Fallback only if (a) and (b) both fail:** a staging-copy read, but
      batched once per frame (not per-draw) — copy each of the ~6 pool
      buffers on the immediate context right after `Present`, giving a CPU
      snapshot to patch from during the *next* frame's recording (one frame
      of staleness, acceptable for a rotation proof).
  Document the chosen mechanism and why in `notes/06-camera-matrix-discovery.md`.
  **Gate:** do not proceed to Step 1 until the chosen read path runs every
  frame without a measured stall (watch `Present` cadence in the log) and
  without corrupting engine state.

- [ ] **Step 1: Build the scratch constant-buffer pool**

  A ring of `ID3D11Buffer*` (DYNAMIC, `D3D11_MAP_WRITE_DISCARD`-able, sized
  to the largest observed cb0, e.g. 256 B). Size generously (start at 64;
  log + grow or wrap-with-a-warning if exhausted within a frame) — many
  recorded draws each need distinct patched contents within one frame.

- [ ] **Step 2: Install the Draw/DrawIndexed hooks**

  Reuse the address-keyed late-hook dispatch table from Task 5 addendum 3
  (register-between-create-and-enable discipline; fail loud + remove hook on
  table-full, per the addendum-3 review fixes — do not regress these). On
  each hooked `DrawIndexed`: using Step 0's read path, read the bound slot-0
  MVP rows at the shader's reflected offset; skip (call original unmodified)
  if the shader is unknown to `mvp_offset_for_shader`. Else compute
  `mvp' = K * mvp` (K = identity unless `TEWVR_TEST_YAW` is set), take the
  next scratch buffer from the ring, `Map(WRITE_DISCARD)` the full original
  cb0 bytes with only the MVP rows replaced, `Unmap`,
  `VSSetConstantBuffers(0, 1, &scratch)`, call the original draw, then rebind
  the engine's original buffer to slot 0 (so any later draw on the same
  context that reuses the binding is unaffected by our substitution).

- [ ] **Step 3: Runtime test — the keystone proof**

  Pre-state (`TEWVR_TEST_YAW` unset): the game renders normally. With
  `TEWVR_TEST_YAW=20` set before launch: the **entire visible world** (not
  just hair/lights — confirm scenery and character bodies rotate, matching
  the SKIPCL finding of what the deferred lists carry) is rotated ~20° from
  the player's actual facing, stably, across at least 30 seconds of gameplay
  including camera movement, with no crash or corruption. Returns to normal
  immediately when the variable is unset. **This is the Phase 4 go/no-go
  proof for the whole conversion.**

- [ ] **Step 4: Fail-safe check**

  Force a failure path (e.g. temporarily make `mvp_offset_for_shader` always
  return "not found") and confirm the game still renders normally (every
  draw falls through unpatched), with a one-line log reason, no crash.

- [ ] **Step 5: Ledger + dossier + commit**

  Update `notes/06-camera-matrix-discovery.md` (keystone proof done, chosen
  Step 0 read mechanism) and `the-evil-within-vr-engine-research/ENGINE-DOSSIER.md`
  (status → Phase 4 complete / Phase 5 next; update open risks). Push doc
  repos + the engine-research repo. Commit the mod repo locally (no push —
  still push-gated).

---

### Task 7: StereoSink seam + BackbufferSink + double render (same matrix)

> **Note (ledger, 2026-08-20):** Task 6's per-draw patch-at-record mechanism
> and Task 7's double-render mechanism are now expected to merge: rather than
> re-executing a command list per eye (the original re-execute-vs-rerecord
> crux, now moot), the Task 6 Draw hook likely needs to inject the draw
> *twice* per original call — once per eye, each with its own patched cb0 and
> its own viewport (left/right half) — within the same recording pass. Revisit
> and fully re-scope this task's steps against Task 6's actual landed
> mechanism before writing code; the text below is the pre-Task-6 draft.

Introduce the `IStereoSink` seam and the `BackbufferSink`, and render the scene twice per frame into the left and right halves of the back-buffer — using the **same** camera matrix for both eyes first, to isolate the double-render/viewport mechanics from the eye-offset math.

**Files:**
- Create: `proxy-winmm/src/stereo_core.c`, `proxy-winmm/src/stereo_core.h`
- Create: `proxy-winmm/src/backbuffer_sink.c`, `proxy-winmm/src/backbuffer_sink.h`
- Modify: `proxy-winmm/src/hooks.c` (drive `stereo_render_frame()` from the render boundary chosen in Task 5)

**Interfaces:**
- Produces: `typedef struct IStereoSink { void (*begin_eye)(struct IStereoSink*, int eye, UINT w, UINT h); void (*end_eye)(struct IStereoSink*, int eye); } IStereoSink;` (eye 0 = left, 1 = right).
- Produces: `void stereo_render_frame(void);` and `IStereoSink *backbuffer_sink(void);`.
- Consumes: `d3d_capture` (Task 3), `camera_*` (Task 6), the render mechanism (Task 5).

- [ ] **Step 1: Implement BackbufferSink**

`begin_eye` sets an `ID3D11` viewport to the left or right half of the captured back-buffer (`x = eye * width/2`, `width/2 × height`) and binds the back-buffer RTV. `end_eye` is a no-op for now. Save/restore prior viewport and RTV around the pair so the game's own state is untouched.

- [ ] **Step 2: Implement stereo_render_frame**

```
save engine render state (RTV, viewport, camera override)
for eye in {0,1}:
    sink->begin_eye(sink, eye, width, height)
    camera_set_override(&engine_view, &engine_proj)   /* same matrix both eyes for now */
    <invoke the second render per Task 5's mechanism, or let the native pass fill eye 0 and re-render eye 1>
    sink->end_eye(sink, eye)
camera_clear_override()
restore engine render state
```
(If Task 5 chose native-pass-plus-one-extra, eye 0 is the engine's own pass constrained to the left viewport and eye 1 is the extra render; if full re-invocation, both eyes are explicit renders. Follow Task 5's documented decision.)

- [ ] **Step 3: Runtime test**

Launch, reach gameplay. Expected on the monitor: the scene appears **twice, side by side**, each half filling its viewport, both halves live and updating as you move (neither frozen nor black). Depth is not yet correct (same matrix) — that is expected at this step.

- [ ] **Step 4: Ledger + commit**

Dev-archive `notes/08-first-double-render.md` with a description of the side-by-side result. Push docs. Commit mod repo locally.

---

### Task 8: Per-eye matrices (IPD offset + per-eye projection)

Give each eye its own viewpoint: offset the view matrix by ±IPD/2 along the camera's right axis and use a per-eye projection. This produces correct stereo parallax.

**Files:**
- Modify: `proxy-winmm/src/stereo_core.c` (compute per-eye matrices)
- Modify: `proxy-winmm/src/camera.c` (eye-offset helpers, if not already present)

**Interfaces:**
- Consumes: `mat4_translation_local`, `camera_get_last_view` (Task 6).
- Produces: internal `Mat4 eye_view(int eye, float ipd)` and `Mat4 eye_proj(int eye)`.

- [ ] **Step 1: Compute per-eye view**

For each eye, `sign = (eye == 0) ? -1.f : +1.f;` `view_eye = mat4_translation_local(&engine_view, sign * ipd * 0.5f);` where `ipd` comes from config (Task 9; hardcode a neutral default like `0.064f` metres-equivalent for now, scaled to the engine's world units — determine the world-unit scale empirically and record it).

- [ ] **Step 2: Per-eye projection**

For this milestone use the engine's projection unchanged per eye (symmetric), optionally scaled by `TEWVR_FOV_SCALE`. Off-axis/asymmetric frusta are deferred to the OpenVR stage where real HMD tangents exist.

- [ ] **Step 3: Set per-eye override in the loop**

In `stereo_render_frame`, replace the same-matrix override with `camera_set_override(&eye_view(eye,ipd), &eye_proj(eye))`.

- [ ] **Step 4: Runtime test — correctness**

Launch, reach gameplay. Expected: the two halves now differ by a horizontal parallax — near objects shift more between the halves than far objects, and the shift direction is correct (free-viewing/cross-eye fusion yields a solid 3D image, or verify parallax direction against a near object like the held weapon vs. a distant wall). A wrong-direction parallax means the eye sign or handedness is inverted — fix by flipping `sign` or the world-unit scale, not by guessing.

- [ ] **Step 5: Determine the world-unit scale**

Tune the IPD constant until near/far parallax reads as natural depth by eye, and record the world-unit-per-metre scale in `notes/08-first-double-render.md` for the OpenVR stage to reuse.

- [ ] **Step 6: Ledger + commit**

Record the working stereo milestone. Push docs. Commit mod repo locally.

---

### Task 9: Configuration knobs (TEWVR_*)

Expose the runtime knobs from the spec as environment variables with correctness-first defaults.

**Files:**
- Create: `proxy-winmm/src/config.c`, `proxy-winmm/src/config.h`
- Modify: `proxy-winmm/src/stereo_core.c`, `proxy-winmm/src/dllmain.c` (read config at load)

**Interfaces:**
- Produces: `struct Config { bool enable; float ipd; float fov_scale; float convergence; bool swap_eyes; }` and `const struct Config *config_get(void);`, loaded once from the environment at DLL attach.

- [ ] **Step 1: Implement config load**

Read `TEWVR_ENABLE` (default 1), `TEWVR_IPD` (default the value tuned in Task 8), `TEWVR_FOV_SCALE` (default 1.0), `TEWVR_CONVERGENCE` (default neutral), `TEWVR_SWAP_EYES` (default 0). Log the resolved config at startup.

- [ ] **Step 2: Wire the knobs**

`enable=0` → skip stereo entirely, let the game render mono (fall through in `stereo_render_frame`). `ipd`, `fov_scale` feed the eye math. `swap_eyes` swaps which viewport each eye renders to.

- [ ] **Step 3: Runtime test**

Verify each: `TEWVR_ENABLE=0` → normal mono game (single full-screen image). `TEWVR_SWAP_EYES=1` → left/right images exchanged. `TEWVR_IPD` larger → wider parallax. Each set via the environment before launch.

- [ ] **Step 4: Ledger + commit**

Document the knobs in `USAGE.md`. Commit mod repo locally.

---

### Task 10: Fail-safe fallback, logging polish, and USAGE

Guarantee the game is never worse off with the mod present than without it, and document installation.

**Files:**
- Modify: `proxy-winmm/src/hooks.c`, `proxy-winmm/src/stereo_core.c` (guard every entry with capability checks)
- Create: `proxy-winmm/USAGE.md`

- [ ] **Step 1: Guard every path**

If MinHook init fails, if the dummy device cannot be created, if `d3d_capture` is not ready, or if the camera matrix was never found, log the reason once and **disable the stereo path** — the Present hook then just calls the original, and the game renders mono. No code path may crash or return an error into the game.

- [ ] **Step 2: Fault-injection test**

Temporarily force each failure (e.g. skip the camera hook install) and confirm the game still runs and plays normally in mono, with a clear one-line reason in the log.

- [ ] **Step 3: Write USAGE.md**

Document: requirements (owning the game; the winmm.dll placement), install/uninstall (copy/delete `winmm.dll` next to `EvilWithin.exe`), the `TEWVR_*` knobs, what is confirmed working (monitor-side stereo) and what is **not** in this milestone (no headset output, no head tracking), and known issues. British spelling; grammar-checked.

- [ ] **Step 4: Regression test**

With `winmm.dll` removed, confirm the game is byte-for-byte stock behaviour. With it present and `TEWVR_ENABLE=1`, confirm working side-by-side stereo. This closes the milestone.

- [ ] **Step 5: Final ledger + commit**

Update `notes/00-status.md` in both doc repos to mark the stereo core milestone complete, with a link to the results note. Push docs. Commit the mod repo locally.

- [ ] **Step 6: Push gate for the mod repo**

The mod source is complete for this milestone but **remains local**. Ask the user for explicit approval before pushing `the-evil-within-vr-mod`. Do not push without it.

---

## Milestone completion criteria

- A `winmm.dll` that, placed next to `EvilWithin.exe`, renders the game in correct side-by-side stereo on the monitor (parallax correct, both eyes live), and that falls back cleanly to stock mono on any failure or with `TEWVR_ENABLE=0`.
- The reverse-engineering findings (camera matrix layout, render boundary, double-render strategy, world-unit scale) documented in the dev-archive.
- Doc repos current; the mod repo committed locally and awaiting the user's push decision.
- Head tracking and OpenVR compositor submission are intentionally not included — they are sub-project 2, which reuses the `IStereoSink` seam and the documented findings.
