# Static recon — reading the executable before running it

Before launching anything, we scanned `EvilWithin.exe` for tell-tale strings.
This is the cheapest possible recon and it answered most of the big questions.

## The renderer is Direct3D 11

The binary imports `d3d11.dll`, calls `D3D11CreateDevice`, and ships
`d3dcompiler`. There is no sign of OpenGL (`opengl32.dll`, `wglCreateContext`)
or of the older Direct3D 9. This matters more than anything else: unlike the
OpenGL id Tech 5 of RAGE and Wolfenstein, this game sits squarely in the
category that modern VR-injection mods already know how to handle.

## The console and cvars survived Tango's fork

The id Tech console machinery is all still there: `com_allowconsole`, `idCVar`,
`listcmds`, and the `idStudio` developer mode. More usefully, dumping the region
of the cvar name table around `g_fov` revealed a rich developer suite, and
several of those cvars line up neatly with VR needs:

- **Camera and view:** `g_fov`, `g_viewNodalX`, `g_viewNodalZ` (view-origin
  nodal offsets — conceptually an IPD/eye-offset knob the engine already has),
  `view_damageBlur`, `view_doubleVision`, `g_skipViewEffects`.
- **Player and movement:** the whole `pm_thirdPerson*` family, plus
  `pm_crouchviewheight`, `pm_normalviewheight`, and the view-pitch clamps
  `pm_minviewpitch` / `pm_maxviewpitch`.
- **Display:** `g_showPlayerShadow`, `g_showHud`.

The lesson: a good part of the "easy tier" — first person, HUD, shadow, FOV,
eye offsets — may be reachable through the engine's own cvars.

## A first-person nuance

Some online guides mention a `pl_FPS 1` command. That exact string is **not** in
this build. What is present is the `pm_thirdPerson*` family, which suggests
first-person here is reached by collapsing the third-person camera distance
rather than through a dedicated toggle. This lines up with the common community
report that first-person weapon animations look buggy — the path exists but was
never finished. That rough edge is exactly what the mod is for.

## The player model and animation system

The player is a fully-rigged model (`pl/pl0001.md6`) with named spine, neck,
and arm joints, animated by **Morpheme** (NaturalMotion). Morpheme drives the
weapon and character animation, so it is the system we would work with for
two-handing, manual reload, and pump-action gestures. A full rig also makes the
full-body and correct-shadow goals realistic.

Everything in this note is derived from the executable's own strings and
structure, which are interoperability facts. No game assets were extracted.
