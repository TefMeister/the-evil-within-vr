# `unmaps==0` has a SECOND cause: on a deferred context, `Map` returns command-list scratch memory

**From:** `/gr` (estate sweep, 2026-09-07) · **For:** the modding lane, before the pending ⭐ `[FLAT]`
dynamic-cb0 re-test

**One ask:** add the second candidate cause to the re-test's reading table, so a `unmaps==0` result is
not attributed to the vtable story by default.

**Full write-up:** [`external-research/topics/2026-09-07-deferred-map-hands-back-scratch-memory-so-a-per-eye-edit-is-baked-into-the-command-list.md`](../../external-research/topics/2026-09-07-deferred-map-hands-back-scratch-memory-so-a-per-eye-edit-is-baked-into-the-command-list.md)

## ⚠️ Most of what I went looking for, this project already knows

Stating that first so nothing below reads as new when it is not. Already in the dossier/board and
**not** rediscovered: **six worker-thread deferred contexts** (measured here; the public record has
nothing on id Tech 5 / STEM's D3D11 threading, and that gap is real rather than a search failure);
the key being **`(ctx,res)`** rather than a thread ring; and **draw-time buffer substitution** as the
pool path's mechanism. The commissioned research's headline recommendations are already implemented
here.

## The ask

The re-test's reading table says:

> "**maps>0 but unmaps==0 ⇒ our `Unmap` hook never sees these buffers** — deferred contexts use a
> different vtable flavour and `Map`/`Unmap` are hooked once on the immediate context and never
> late-hooked."

There is a **second, semantic cause** `[reported 2026-09-07]`. On a deferred context, `Map` does not
touch the real resource: the runtime hands back a **fresh scratch allocation owned by that context's
command list**, committed when the list is replayed. In DXVK's implementation of that contract,
**`Unmap` on a deferred context is a no-op — the update is committed in `Map`** (`WRITE_DISCARD`
allocates a new slice; `WRITE_NO_OVERWRITE` requires a prior discard on that list or errors).

So `unmaps==0` **need not mean the hook is blind.** Distinguishing the two is cheap: count whether
`Unmap` is reached on *any* context at all, not just on these buffers.

**Bonus, and it validates a choice already made:** the same constant buffer can legitimately be
mapped **simultaneously on two different deferred contexts**, which is exactly why a resource-only
key is unsafe and `(ctx,res)` is correct. The dossier has the right key; this is the reason, which it
does not state. (First-party vendor documentation also gives the cleanest statement of why the
original per-thread ring was on the wrong axis: *"only one thread can call a ID3D11DeviceContext at a
time"* — **threads are unbounded and transient; contexts are few and stable.**)

## Not the ask, but flagged because it would cost a launch

⚠️ **A named deadlock this hook shape is prone to.** 3Dmigoto issue #104, *"Lock ordering bug
(Deadlock) between 3DMigoto and DirectX"*: taking your own critical section, then calling into a
DirectX routine which takes its lock and later calls `Release()`, which re-enters your release
tracker, which tries to take your critical section again `[reported]`. **Rule: never hold a global
lock across a call back into D3D inside a `Map`/`Unmap` hook.** With six deferred contexts and a
shared table this project has the shape that bug lives in, and a deadlock would present as a hang.
One read of the deployed code before the launch.

A per-context state object needs **no lock on its inner table** at all, for the single-threadedness
reason above — only the outer context→state lookup needs one.

## Parked deliberately, not asked for now

There is a **structural limit on Map-time stereo** that follows from the same semantics — a value
written at `Map` time on a deferred context is baked into the recorded list, so it cannot express a
different value per eye from one recorded upload `[hypothesis]`. It does **not** affect the pending
mono re-test (`TEST_YAW=90`), and the pool path already works at draw time where this does not apply.
It is written up in the topic and should be revisited **only if the mono re-test passes and stereo is
next** — filing it as an action now would be premature.

## Credit

Microsoft (first-party D3D11 documentation on `Map`, multithreading, deferred rendering);
doitsujin and K0bin (DXVK, whose deferred-context implementation makes the behaviour legible);
bo3b and DarkStarSword (3Dmigoto, and issue #104); baldurk (RenderDoc); crosire (ReShade);
Kaldaien (Special K). Added to `external-research/CREDITS.md`. Read online only; nothing cloned,
downloaded or copied. ⚠️ Two forum threads returned 403, so a couple of corroborating quotes are
second-hand search snippets and are marked as such in the topic; the load-bearing claims are from
vendor documentation and source.
