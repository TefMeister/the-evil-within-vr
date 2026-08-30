# Homework, without the game running

Task 6's code was finished and checked over, but the final proof — actually
watching the world spin on command — needs someone at the keyboard, and
nobody was free for a bit. Rather than sit idle, this session went back
through everything we'd already recorded from past play sessions, and picked
apart the game's own files on disk. No launching required. Turns out there
was plenty left to learn.

## The mystery buffer had a decoy

We'd built our fix around reading a very specific pile of data: six little
1920-byte lockboxes, one per drawing thread, holding every object's position
in the world. Last time round, we worried our new code might grab the wrong
box by mistake — because it was only picking boxes by *size*, and who's to
say there isn't some other 1920-byte box lying around?

There is. Going back through an old play-session recording, we found a
completely different pair of 1920-byte boxes — but these get *refilled* once
every single frame, holds the same fixed starting numbers each time, and
sits in a different pocket than the real ones. The real boxes never get
refilled at all (that was the whole trick that took so long to find last
time) and sit in a different pocket entirely. So: two look-alikes, easily
told apart once you know where to look. Good news — our new code checks the
right pocket, so it should walk straight past the decoy.

## We finally read the shaders properly

Every "recipe card" the graphics chip uses (the shader programs) gets saved
to disk during testing. There are 168 of them sitting in a folder, and until
now we'd only ever skimmed their ingredient lists. This time we actually
read the full recipes for the odd ones out — the ones that clearly *don't*
place an object in the world the normal way.

One of them turned out to be exactly the "smear the picture between frames"
shader used for smoothing jagged edges — we can now see precisely which
numbers it needs, for whenever we get to sorting that out (not urgent yet).

A more interesting find: a couple of shaders for detailed character
skin/face don't place points in the world *at all* — they hand off to a
separate, later step that does it instead (a "tessellation" step, which
adds extra detail to a mesh on the fly). Our current trick for owning the
camera only reaches the earlier step, so anything drawn this second way
won't turn with the rest of the world during our test. If a detailed close-up
of a face refuses to rotate while everything else does, that's *why* — not
a new bug, just a part of the picture we haven't reached yet.

## The game has nearly 2,000 hidden dials

We went hunting through the game's own file for every settings-name-shaped
string we could find, and turned up roughly two thousand tucked-away
options — miles more than the handful we already knew about. Most are
irrelevant to us, but a few stood out:

- A **free camera** command (`noclip`) — fly around without walking or
  bumping into things.
- The developers apparently built themselves an **automatic screenshot
  tool** for their own testing. We haven't touched it yet, but it might save
  us building our own from scratch later, when we get to teaching the game
  to test itself.
- A couple of **skip the boring bits** switches (skip the intro video, skip
  the "press any button" screen) — handy for the same future work.
- Confirmed: **no hidden 3D or VR mode** exists anywhere in the game. We're
  not missing a shortcut — building this from the ground up really is the
  only way in.

## Still on the list for whoever's next at the keyboard

- The actual "does the world spin on command" test — still needs a person.
- Whether the real position boxes actually turn up once a proper level is
  loaded (menus don't seem to need them).
- Watch for that one exception (detailed character faces) not turning with
  everything else — expected, not a bug.
