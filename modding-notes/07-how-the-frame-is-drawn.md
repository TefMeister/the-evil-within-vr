# How the frame is actually drawn — and where the second eye comes from

The last note ended on a happy result: one master matrix per eye moves the whole world, no
per-object work needed. That told us *what* to change. This session answered the other half
— *how to make the game draw the scene a second time at all* — and the answer reshaped our
picture of the whole renderer.

## We taught the proxy to keep a diary

We extended the DLL to log the game's drawing commands in order, each stamped with which
thread and which Direct3D "context" issued it. A little trick lets us start recording from
outside the game once we're actually walking around in a level, instead of wasting the
recording on menus. Two play sessions and one live experiment did the job.

## Surprise: the game draws the frame on six threads at once

At the menu everything came from one thread. In gameplay, the draw commands pour out of
**six** worker threads at the same time, each recording into its own command list, while a
single seventh thread does all the setup and then "plays back" those lists. This is a
standard-but-advanced Direct3D technique called deferred contexts, and the game leans on it
hard — a couple of hundred playbacks every frame. It's why our first hooks saw the worker
threads' *draws* but not their *setup*: the workers use different internal plumbing, so we
had to reach in and hook each one the instant it appeared.

## The experiment that settled everything

We needed to know what those command lists actually contain. So instead of guessing, we
added a switch that lets us *skip* playing them back, flipped it on for about twelve seconds
while the user watched the screen, and flipped it back. (The game didn't even crash.)

With the command lists skipped, **the world went black.** Every bit of scenery and
Sebastian's own body disappeared. What survived: his **hair**, and the **windows and
lights**.

That's the whole answer in one image. The six worker threads record the **world** — all the
environment and the characters, the heavy majority of the frame. The main thread draws only
the leftovers directly: a separate hair pass, the glows and lights, and the on-screen
display.

## What it means for the mod

It draws a hard line under the strategy. To put the world in a headset, our per-eye camera
trick **has to reach the command lists** — the part that draws everything you actually walk
through. If we only patched the setup we can see directly, we'd shift the hair and the
lights into 3D and leave the entire world flat in one eye. Good to know *before* building it
rather than after.

So the plan for stereo is now concrete: take the command lists the game already recorded and
**play them back a second time, once per eye**, each time into its own half of the screen
with the matching eye's master matrix applied. We drive that from the exact same "playback"
hook the skip experiment used, so the seam we need is already in our hands and already
proven controllable.

## The one thing still to pin down

Command lists remember their settings from when they were recorded. The question the next
stage has to answer is whether playing one back a *second* time re-reads the current camera
numbers (in which case we simply change them between the two playbacks and get two eyes
almost for free) or reuses the recorded ones (in which case we record our own second pass
instead, which our existing tools already have the pieces for). Either way the *method* —
play twice, one eye each — is decided. That was this stage's job, and it's done.
