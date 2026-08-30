# We can move the camera

The last note ended with a plan: read each object's matrix as it's about to be drawn, nudge
it, hand back the adjusted copy. Simple in theory. It took three wrong turns and a very
satisfying screenshot to actually get there.

## Three ways to be wrong

**Wrong turn one:** read all six buffers once per frame and reuse those readings for every
draw that frame. Looked reasonable, until we did the maths — nearly two thousand objects
draw every frame, sharing only six buffers between them. Reusing one reading for three
hundred different objects means almost all of them get somebody else's position. Caught this
one just by checking the numbers, before ever running it.

**Wrong turn two, the expensive one:** grab a permanent, direct line into each buffer the
moment it's created, the same trick the engine itself uses. Tested it live, multiple times,
clean as anything — no crashes, no errors, buffers captured exactly as expected. Two whole
rounds of fixing and polishing went into this approach. It was still completely wrong: it
turns out the *real* world-position buffers are a special kind that flatly cannot be read
that way, full stop, no exceptions. Every buffer we were so successfully grabbing was a
decoy — a different set of buffers that happens to be exactly the same size, sitting there
doing something else entirely. The mechanism worked perfectly. It was reading the wrong
thing.

**Wrong turn three, quickly ruled out:** maybe our hook just wasn't ready in time and missed
the real buffers being created. We timed it precisely — our hook was ready in well under a
second, over two and a half seconds before anything happened. Not a timing problem.

## The actual answer

The real buffers get written a different way than we'd been watching for — not the common
"open it, write, close it" pattern, but a more direct "just overwrite these bytes" call.
Once we watched for *that* instead, everything clicked into place immediately: the true six
buffers appeared, all exactly the size we expected, and — critically — we could finally tell
them apart from the decoys, because only the real ones are actually bound to real world
draws. Size alone was never going to tell them apart; what the buffer is *used for* was the
only reliable test all along.

## Proof, not just hope

We built a way to launch the game, walk straight into a real level, and grab a picture of
the screen — all without anyone sitting at the keyboard. Three pictures told the story:

- Normal picture: Sebastian, a locker room, nothing unusual.
- Same setup, patch switched on but told to change nothing: identical, down to the film
  grain.
- Same setup, patch told to apply a rough 90-degree twist: the room is unrecognisable —
  mostly black, warped fragments of walls floating at odd angles.

That third picture looks like something broke. It didn't. The rough test twist we used isn't
a proper camera turn — it's a shortcut that proves control without looking pretty, and a
shortcut like that throws most of the scene off the edge of what the screen can show. The
real tell is a small light in the scene that our patch deliberately leaves untouched: it
sits in exactly the same spot in the "broken-looking" picture as in the normal one, while
everything else visibly jumped. If this were a crash or a corrupted mess instead of a real,
working camera change, that untouched light would have moved or vanished too. It didn't.
That's the proof.

## What's left before this becomes real stereo

The rough twist we used for the test isn't the actual camera maths needed for VR — that's the
next task. And we don't yet reach every single thing on screen — somewhere around a quarter
of the objects use a shader shape our patch doesn't cover yet, so those would render facing
the wrong way in a real stereo picture until that gap is closed. Both are known, written
down, and next in line — not surprises waiting to happen.

## Next

Build the real per-eye version of this: instead of a rough test twist, the actual "shift
this eye sideways and adjust the view" maths, applied twice per object — once per eye — into
the two halves of the screen. That's stereo.
