# Reading the shaders: the camera hunt is over

The previous note ended on a worrying cliff-hanger: no shared buffer moves the
world, so the engine must position geometry object by object — the hard case
for a VR mod. This session we stopped squinting at the screen and went to the
source of truth instead: the vertex shaders themselves.

## What we did

The proxy DLL grew a new trick (`TEWVR_SHADERDUMP=1`): it catches every vertex
shader the game creates and saves the compiled bytecode to disk, while counting
which shaders draw the most geometry. One thirty-second walk around the level
gave us 167 unique vertex shaders, ranked by how much of the world they draw.

Compiled D3D11 shaders keep a little table of the names of their constants, and
Microsoft ships a disassembler. So we disassembled the heavy hitters and simply
*read* how the on-screen position is calculated.

## What the shaders say

Every material shader carries a per-draw constant block whose members have
lovely, honest names: `mvpmatrixx`, `mvpmatrixy`, `mvpmatrixz`, `mvpmatrixw` —
the four rows of the combined model-view-projection matrix — plus the model
matrix, fog values, and so on. The final vertex position is exactly four dot
products against those four rows. That's id Tech 5's renderer parameter system
(the RAGE/Doom 3 BFG sources call it `rpMVPmatrixX`), alive and well inside
Tango's engine.

145 of the 167 shaders use it. The couple of dozen that don't are screen-space
work — post-processing, anti-aliasing, motion blur — that never touches world
geometry anyway. So the 06b conclusion is now proven, not suspected: **each
object arrives at the GPU with its own finished MVP matrix, composed on the
CPU.**

## The genuinely good news

That sounds like we'd have to understand every object to do stereo — we don't.
A bit of matrix algebra collapses the whole problem:

Each object's matrix is `projection x view x model`. What an eye needs is the
same thing with a small sideways head-shift inserted after the view. Factor it
out and the per-eye version is just

```
K_eye x (the matrix the game already made)
```

where `K_eye` is **one fixed matrix per eye** — the same for every single
object in the frame. Multiply every per-draw matrix by it as the constants fly
past our hooks (which already intercept exactly those buffer writes), and the
whole world renders from the shifted eye. No per-object understanding needed.

The "hard engine class" turned out to have a one-matrix master key.

## Next up

Work out precisely *when* the game fills each per-draw block relative to
binding the shader and issuing the draw, so we know the right moment to slip
the multiply in. Then: render the frame twice — once per eye — and we have
stereo.
