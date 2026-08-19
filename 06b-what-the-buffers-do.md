# What the buffers actually do (and a false lead corrected)

An honest correction to the earlier note: the "camera view matrix" we thought
we'd found (a 384-byte buffer) was **not** the camera. When we finally tested it
live — poking its values and watching the screen — only a single cloth mesh on
Sebastian moved. A matrix can rotate along with the camera and still just be an
object that's attached to the camera's view. Content patterns fooled us; only
actually perturbing the buffer and watching the game told the truth.

## How we figured out what's what

Two techniques together:

- **Counting** which constant buffer the vertex shader uses for the most draws
  (the shared, per-frame ones show up on top).
- **Wobbling** a buffer's contents live and watching what changes on screen —
  with one important trick: *uniformly* scaling a projection matrix does
  nothing visible (the maths cancels out when the GPU divides by depth), so the
  wobble has to be uneven to reveal anything.

## The results

Poking each of the busiest shared buffers, in real gameplay, showed they're all
**screen effects, not geometry**:

- **96-byte buffer** (the busiest, on every draw): **lighting** — lens flares,
  how bright things are, lighting that shifts with the camera angle.
- **64-byte buffer**: **colour grading** — trippy hue shifts, and it dragged the
  UI colours with it.
- **128-byte buffer**: **lighting** — lights flashing on and off.

And crucially: **no shared buffer ever moved the walls or floor.** The world
geometry just doesn't respond to any single buffer.

## What that means

The game (a heavily modified id Tech 5) almost certainly positions world
geometry with **per-object matrices worked out on the CPU**, not one shared
camera matrix we can grab and offset. The camera info exists, but it's used for
lighting; the geometry is placed object by object. That's the harder kind of
engine to bring into VR — you end up nudging every object's transform for each
eye, rather than one camera.

## What's next

Stop guessing by eye. The next step is proper reverse engineering with a
debugger: trace a single wall-or-floor draw and see exactly which matrix decides
where its pixels land on screen. That pinpoints what to change for stereo,
without needing to stare at wobbling screenshots.
