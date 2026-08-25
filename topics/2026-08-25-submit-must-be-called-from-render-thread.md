# `IVRCompositor::Submit` must be called from the same thread you render on — directly relevant given this game's unusual 6-thread recording architecture

**Status:** 🆕 new · **Priority:** high — a specific, easy-to-get-wrong threading requirement,
directly relevant to this project's already-documented unusual frame architecture (6 worker threads
recording command lists, replayed by a 7th).

## What was found

OpenVR's own documentation is explicit and specific: **`IVRCompositor::Submit()` "should only be
called from the same thread you are rendering on"** — i.e., whichever thread owns the D3D11
immediate context that actually issues the final rendering/`Present` call, not an arbitrary worker
thread. This is a real constraint, not a suggestion — mixing this up is exactly the kind of thing that
produces intermittent, hard-to-diagnose corruption or crashes rather than an obvious immediate error.

## Why this is directly relevant to this project specifically

This project's own recon already found a genuinely unusual frame structure: the game records draw
commands on **six worker threads simultaneously** using deferred contexts, and a **seventh thread
plays those command lists back** into the immediate context (`07-how-the-frame-is-drawn.md`). The
existing `Present` hook — the mechanism already proven working in this project (Task 2, landed and
firing every frame) — necessarily runs on **the thread that owns the immediate context**, since
`Present` is an immediate-context/swap-chain operation, not something deferred contexts can call
directly. **That means the already-proven Present-hook thread is, by construction, the correct thread
to also call `IVRCompositor::Submit()` from** — the two naturally line up, since both need "the thread
that owns the game's real D3D11 device/immediate context," and this project already found and hooked
exactly that thread for Task 2. Worth stating this explicitly as a design constraint before
sub-project 2 implementation starts, rather than discovering it as a bug after the fact: **do the
Submit call inside (or immediately following) the existing Present hook, on that same thread — do not
dispatch it to a different thread for convenience (e.g. a dedicated "VR thread"), even though that
might seem like a natural way to decouple VR-host logic from the game's own render hook.**

## Concrete next step

When implementing OpenVR compositor submission (sub-project 2), call `WaitGetPoses` and
`IVRCompositor::Submit` from inside the existing `Present` hook's thread context, not from a separate
thread — consistent with both OpenVR's documented requirement and this project's own already-mapped
frame architecture.

## Sources

- https://github.com/ValveSoftware/openvr/wiki/IVRCompositor_Overview (via search-engine summary of the official threading requirement)
