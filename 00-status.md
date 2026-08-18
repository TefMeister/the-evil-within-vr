# Status

**Last updated: 2026-08-18 — Tasks 1–3 complete; Task 4: the view matrix is found.**

The camera view matrix has been located — a 384-byte buffer the game refreshes
every frame, with the view matrix 48 bytes in, confirmed from a 40k-snapshot
gameplay capture ([06-camera-matrix.md](06-camera-matrix.md)). Still to do on
Task 4: one more short capture (now set up to grab the whole buffer) to find the
projection / combined view-projection matrix and confirm which one the shaders
use — then we can start bending the camera per eye.

---


The stereo 6DOF core is being built from a 10-task plan. Two tasks are done. Task
1: a proxy `winmm.dll` that forwards all 180 winmm exports gets our code running
inside the game ([04-proxy-loader.md](04-proxy-loader.md), including the "forward
everything, not just the exe's imports" lesson). Task 2: a MinHook hook on
Direct3D 11's `Present` — the end-of-frame call — obtained by borrowing the
address from a throwaway swap-chain's vtable, now firing every frame on the
game's render thread with the game responsive
([05-present-hook.md](05-present-hook.md)). Next up: capturing the game's real
device and back-buffer from inside the hook.

Approach for the sub-project: inject via a proxy `winmm.dll` with MinHook (refined
from `dxgi.dll` during planning), render the scene twice per frame with true
geometry (not depth reprojection), and — for the first milestone — present it
side-by-side on the flat monitor to prove stereo correctness before adding head
tracking and OpenVR compositor submission (those are sub-project 2). The full
design and plan live in the dev-archive repo.

---

**Earlier: feasibility spike complete, verdict: feasible.**

We spent one session answering a single question before committing to anything:
*can The Evil Within be brought into VR at all, and by which route?* The answer
is yes, and the engine turned out to be friendlier than "it needs complete
reverse engineering" first suggested.

The short version:

- The game renders with **Direct3D 11**, which is the best-supported target for
  VR injection. This is the single most important finding.
- The id Tech **developer console and cvar system are intact**, and several
  cvars map directly onto VR needs (FOV, first-person, HUD, player shadow,
  view-origin offsets). A lot of the "easy tier" may be console tweaks rather
  than assembly patches.
- The executable **does not use ASLR** — it loads at the same address every
  time — so addresses we find stay valid. That saves a great deal of RE effort.
- Both **XInput and DirectInput** are active, and the player is a **fully-rigged
  model driven by Morpheme** animation middleware, which is good news for the
  motion-control, full-body, and shadow goals.

No mod code exists yet. The next step is to break the mod into stages and design
the first one — the stereo 6DOF core, which is the foundation and the biggest
risk.

## What is confirmed vs. still aspirational

**Confirmed (this session):** D3D11 renderer; intact console/cvar system; no
ASLR; both input APIs present; fully-rigged player model; Morpheme animation
middleware; the cvar name table located at a stable address in live memory.

**Still aspirational (not yet attempted):** stereo rendering, head tracking,
motion controllers, and every gameplay-interaction feature. These are the real
engineering, staged for later sub-projects. Nothing here is claimed to work in
VR yet.

## Planned stages

1. **Stereo 6DOF core** — D3D11 hook, dual-eye rendering, VR runtime submission,
   head tracking. The make-or-break foundation.
2. **First-person and view polish** — collapse the third-person camera, fix the
   rough first-person weapon animations, HUD/shadow/FOV, 3D ammo counter.
3. **Motion controllers** — 6DOF aiming, aim-down-sights, two-handing, holsters.
4. **Interaction gestures** — manual reload, slide rack, pump action, motion
   melee, physical pickup, collision door-push, physical crouch.
5. **Body and roomscale** — full-body IK and roomscale mapping.
