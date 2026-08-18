# 02 — Static reconnaissance

Findings from scanning `EvilWithin.exe` for strings, before running it. All
offsets below are **raw file offsets** into the executable, not runtime virtual
addresses; the mapping to memory is covered in note 03.

## Graphics API

Present in the binary:

- `d3d11.dll`, `D3D11CreateDevice`, `d3dcompiler` — Direct3D 11.
- `XINPUT` — XInput controller support.

Not present (ruled out): `d3d9.dll`, `opengl32.dll`, `wglCreateContext`,
`glTexImage2D`, `CreateDXGIFactory` as an import string. The engine is D3D11,
not the OpenGL used by other id Tech 5 titles.

## Console and cvar system

The id Tech console and cvar machinery survived Tango's engine modifications:

- `com_allowconsole`, `idCVar`, `listcmds`, `idStudio` (developer/cheat mode).
- `g_fov`, `pm_thirdPerson`, `firstperson`, `thirdperson`, `cameraOffset`,
  `g_stopTime`, `g_debugPlayer`, `flashlight`.

A dump of the cvar name table region (around the `g_fov` string) revealed a
large, contiguous developer suite, including entries directly relevant to VR:

- Camera and view: `g_fov`, `g_viewNodalX`, `g_viewNodalZ`, `view_damageBlur`,
  `view_doubleVision`, `g_skipViewEffects`, `g_testPostProcess`.
- Player and movement: the full `pm_thirdPerson*` family
  (`pm_thirdPersonRange`, `pm_thirdPersonHeight`, `pm_thirdPersonAngle`,
  `pm_thirdPersonSimple`, `pm_thirdPersonSlide`, `pm_thirdPersonSmoothness`,
  `pm_thirdPersonClip`, `pm_thirdPersonFocusJoint`), plus `pm_crouchviewheight`,
  `pm_normalviewheight`, `pm_minviewpitch`, `pm_maxviewpitch`, `pm_noBob`.
- Display: `g_showPlayerShadow`, `g_showHud`, `g_showAllPlayerInfo`.
- A full developer and editor toolset: `g_debug*` (anim, move, damage, weapon,
  script, triggers) and `g_edit*` entity-editing commands.

The takeaway: first-person, HUD toggling, shadow control, FOV, and per-eye
view-origin offsets may be reachable through the engine's own cvars rather than
through assembly patches. This is the "easy tier" of the mod.

## First-person nuance

The community command `pl_FPS 1` cited in some online guides is **not** present
in this build's binary. Instead the binary has the `pm_thirdPerson*` family and
a `firstperson` string. First-person in this build is most likely reached by
collapsing the third-person camera distance (for example, setting
`pm_thirdPersonRange` to zero) rather than through a dedicated toggle. This
matches the widely reported community observation that weapon animations look
buggy in first-person: the path exists but was never polished. That rough edge
is exactly the kind of thing the mod will own.

## Player model and animation

- Player model referenced as `pl/pl0001.md6`, with named skeleton joints
  (`head`, `neck_01`, `spine_00`, `spine_01`, `r_upperarm`, `lantern`).
- Animation middleware is **Morpheme** (NaturalMotion): the binary contains
  `morphemeNetwork` and related load/version strings. Morpheme is the system
  that drives weapon and character animation, and therefore the system we would
  retarget for two-handing, manual reload, and pump-action gestures.

## Engine build strings

- `buildgame -preBuildGameOnly -x64` and related build-pipeline strings confirm
  a 64-bit id Tech content build.

No game assets were extracted. This note records only facts about the
executable's structure and its human-readable strings, which are
interoperability information.
