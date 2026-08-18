# Finding the camera matrix

This is the big one. To render the game in stereo, we have to take over the
camera's view matrix and nudge it left and right for the two eyes. So first we
have to *find* that matrix among the hundreds of things the game sends to the
GPU every frame. **We found it.**

## How

Every frame, the game uploads dozens of small "constant buffers" to the GPU —
matrices, colours, shader parameters, all sorts. We added temporary hooks that
dump every camera-sized buffer to a file, decoded as 4×4 matrices, then captured
about two and a half minutes of real gameplay (walking and looking around) —
roughly 40,000 buffer snapshots.

Then the trick: a view matrix is the one thing in there that is simultaneously
(a) a valid rotation — its 3×3 core is orthonormal — and (b) constantly
*changing* as you turn the camera, with a position that follows you as you walk.
A little Python sifted all 40,000 snapshots looking for exactly that signature.

## The result

Out of 40,000 snapshots, only **two** windows matched — and they turned out to
be two copies of the same buffer (the engine keeps two so it can write one while
the GPU reads the other). The camera view matrix is:

- in a **384-byte buffer** the game refreshes every frame,
- at a fixed spot **48 bytes into it**,
- stored as three rows of "rotation + position".

Its rotation swung exactly as the captured camera panned, and its position slid
as the player walked. There's no ambiguity — nothing else in the frame behaves
like that.

## What's left

We only captured the first 128 bytes of each buffer the first time, so the
*projection* matrix — and possibly a pre-combined "view × projection" matrix —
which live further into that 384-byte buffer, weren't recorded yet. That last
part matters: if the game's shaders use a pre-combined matrix, we'll need to
override *that* one, not the plain view matrix, to actually move the picture. So
one more short capture (now set up to grab the whole buffer) will settle it, and
then we can start bending the camera for each eye.

The single hardest search in the project — locating the camera in a live
commercial engine with no source code — is essentially done.
