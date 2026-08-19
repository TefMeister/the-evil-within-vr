# The Evil Within VR — Status

Last updated: 2026-08-19 (session 2: TASK 4 STILL OPEN — the world-geometry
transform is not yet found, and the earlier "view matrix" was a false lead).
**Correction:** the 384-byte matrix reported on 2026-08-18 turned out to be a
per-object model matrix (a cloth mesh), not the camera — see
[notes/06b-render-pipeline-findings.md](06b-render-pipeline-findings.md). What we
established today by bind-counting and live perturbation: the heavily-shared
per-frame buffers are all **screen-space** — 96 bytes = lighting, 64 = colour
grading, 128 = lighting — and **no single shared buffer moves world geometry**.
The engine appears to transform geometry with **per-object CPU-computed
matrices** (id Tech 5 style), the harder engine class for VR. **Next:** shader-
level RE (x64dbg trace of a geometry draw, or vertex-shader disassembly) to find
exactly which matrix positions world vertices — not more visual probing. Tasks
1–3 remain complete (proxy loader, Present hook, device capture). The stereo 6DOF core is being implemented from its
[plan](../plans/2026-08-18-stereo-6dof-core-plan.md) (10 tasks). **Tasks 1–3 are
done.** Task 1: a proxy `winmm.dll` that forwards all 180 winmm exports and loads
our code into the game ([notes/04-proxy-loader.md](04-proxy-loader.md)). Task 2:
a MinHook trampoline on `IDXGISwapChain::Present`, obtained via a throwaway
device's vtable (index 8), firing every frame on the game's render thread. Task
3: capturing the game's real device, context, and back-buffer (1280×720
R8G8B8A8_UNORM) from inside the hook — both in
[notes/05-present-hook.md](05-present-hook.md). **Next: Task 4**, the
reverse-engineering hunt for the camera view and projection matrices (the heart
of the stereo work). Mod code lives in the `the-evil-within-vr-mod` repo
(push-gated; local commits only until release).

The approved design is at
[design/2026-08-18-stereo-6dof-core-design.md](../design/2026-08-18-stereo-6dof-core-design.md).
Approved decisions: proxy `winmm.dll` loader + MinHook injection (refined from
`dxgi.dll` during planning); true double-render stereo (not reprojection);
OpenVR/SteamVR as the eventual runtime; and a first milestone scoped to
**stereo correctness on the flat monitor** (head tracking and compositor
submission deferred to sub-project 2).

Earlier in session 1: FEASIBILITY SPIKE COMPLETE — verdict is **feasible**. We
confirmed, on the running game under a debugger, that The Evil Within uses a
Direct3D 11 renderer, that the id Tech developer console and cvar system
survived Tango's modifications intact, and that the game loads at a fixed base
address with no ASLR.

## Session 1 (2026-08-18): feasibility spike

Goal: answer a single question — *can this engine be brought into VR at all,
and by which route?* — cheaply, before committing to a plan.

What we found (full detail in the numbered notes):

- **Renderer is Direct3D 11 + DXGI.** Verified by watching `d3d11.dll` and
  `dxgi.dll` load into the live process, with a dedicated, named "Render
  Thread". This places a 6DOF conversion in the same well-understood category
  as modern D3D VR-injection mods, rather than the harder OpenGL id Tech 5 of
  RAGE and Wolfenstein.
- **The id Tech console and cvar system are intact.** `com_allowconsole 1` as a
  launch option enables the in-game console (opened with the Insert key). The
  binary contains a full developer cvar suite, including several that map
  directly onto VR needs: `g_fov`, the `pm_thirdPerson*` family (first-person
  is reached by collapsing the third-person camera distance), `g_showPlayerShadow`,
  `g_showHud`, `g_viewNodalX` / `g_viewNodalZ` (view-origin nodal offsets), and
  view-pitch clamps.
- **No ASLR.** The executable loads at the same base address (`0x7FF62BB20000`)
  across separate launches, so the addresses we find during development stay
  valid. This meaningfully speeds up reverse engineering.
- **Both input APIs are present.** `xinput1_3.dll` and `dinput8.dll` both load,
  so the input layer we will redirect to motion controllers is standard.
- **The player uses a fully-rigged model** (`pl/pl0001.md6`, with named spine,
  neck, and arm joints) driven by Morpheme (NaturalMotion) animation
  middleware. This is encouraging for the full-body, shadow, and weapon-gesture
  goals.
- **Verified live:** the `g_fov` cvar name string sits at a stable virtual
  address in the running process, in the exact table order the static scan
  predicted — confirming the cvar table is present and reachable in memory.

Known constraint discovered this session:

- **Steam DRM anti-debug at launch.** The game's executable is Steam CEG-wrapped
  and refuses to unwrap if the raw executable is launched directly under a
  debugger (it shows a "Steam Error" dialog). The workaround is to launch the
  game normally through Steam, let the DRM decrypt in memory, and then attach
  the debugger to the already-running process. The eventual VR injector will
  target the already-running process in the same way.

Spike verdict: **feasible.** The route is a D3D11 present/context hook for
stereo rendering (a known, solved category), combined with the engine's own
cvars for the "easy tier" of features (FOV, first-person, HUD, shadow, view
nodal offsets).

## Next

- Decompose the full mod into staged sub-projects: (1) stereo 6DOF core;
  (2) first-person and view polish; (3) motion controllers; (4) interaction
  gestures; (5) body and roomscale. Each gets its own design, plan, and build.
- Design sub-project 1 (the stereo 6DOF core) first — it is the foundation
  everything else rests on and the largest single risk.
