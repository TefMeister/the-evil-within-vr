# Finding the world's matrix — the last mystery, solved

The previous note left us with a genuine head-scratcher. We knew the world is drawn from
recorded command lists, and we knew each object needs its own position matrix — but when we
watched the buffer that feeds those matrices to the shaders, it was *never written*. Bound to
every world draw, yet nothing ever seemed to fill it. A buffer that draws the world out of
thin air.

## Chasing it down

First guess: maybe a background thread fills it and we simply weren't watching that thread.
So we widened our net to catch any hidden helper. We caught nothing new — the mystery buffer
still had no visible writer.

So we stopped trying to catch the *writing* and just *read the buffer's contents* directly,
at the moment each object is drawn. That did it. Two objects in a row showed two completely
different matrices in that buffer. It isn't one static buffer at all — it's a small handful
of them (six, matching the six worker threads), each holding a fresh matrix per object.

And the reason we never saw them written: the game opens these buffers *once* and then keeps
a permanent door into them, poking new numbers straight into memory without ever going
through the normal "write to a buffer" call we were listening for. Perfectly standard for a
fast engine — just invisible to the kind of watching we'd been doing.

## Why this is the moment it all comes together

This was the single unknown blocking the whole stereo plan. Now it's answered, and answered
in the best possible way: **at the exact instant each piece of the world is drawn, we can
read the matrix it's about to use.** If we can read it, we can change it.

So the plan for stereo is now fully concrete. For each eye, at each world draw: read the
object's matrix, nudge it sideways by that eye's fixed offset (the "one master matrix" from
two notes ago), hand the object our adjusted copy, and let it draw. Do that for both eyes into
the two halves of the screen and the whole world renders in stereo — no guesswork left about
where the numbers live.

There's real engineering ahead to make that fast and safe (our probe that read the buffers
actually crashed the game once, which is fine — it was a deliberately rough tool and it had
already told us what we needed). But the discovery phase is over. We know exactly what the
engine does and exactly where to step in.

## Next

Build the real thing: intercept each world draw, apply the eye offset to its matrix, and — as
the first proof — inject a deliberate turn so the whole world visibly rotates on command.
Once that works, the second eye is the same trick with a sideways shift, and we have stereo.
