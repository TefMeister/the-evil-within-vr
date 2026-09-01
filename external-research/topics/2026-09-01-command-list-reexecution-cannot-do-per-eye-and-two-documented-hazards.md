# Re-executing a command list per eye cannot work here — and `ExecuteCommandList` has two documented hazards worth knowing before Task 7

**Status:** 🆕 new · **Priority:** high — this targets `ENGINE-DOSSIER.md` §12's first live open risk:
*"Double-render on a deferred-context engine: re-execute command lists per eye vs. record a second
pass — mechanism chosen but unproven at runtime."*

Sourced from Microsoft's own Direct3D 11 documentation, so this is primary-source API semantics
rather than community lore.

## 1. The rejected alternative is not merely slower — it cannot express what stereo needs

The dossier records that the mechanism has been chosen: **patch-and-draw-twice per original call at
record time, once per eye**, rather than re-executing a recorded command list per eye. Two facts
from the documentation say that choice was right, and that the alternative should stay closed.

**A command list is immutable.** Microsoft's wording is direct: *"A command list is immutable and is
designed to be recorded and played back during a single execution of an application."* Playing one
back twice therefore replays **the same recorded commands**, including any constant-buffer updates
that were recorded into it. A second execution does not re-evaluate anything — so on its own it
produces the same view twice.

**The obvious escape does not fit this engine.** In principle you could update a matrix on the
immediate context between the two `ExecuteCommandList` calls and get two different results, since
commands execute in submission order. That works only if the per-eye value lives in **one** buffer
that the command list itself never rewrites. This project has already measured that it does not:
§11 records **~1900 draws per frame sharing ~6 buffer identities**, with contents rewritten per
draw — which is exactly why the once-per-frame batched staging-copy design was found
data-insufficient and abandoned. One outer update before each execution cannot express ~1900
per-draw matrices, for the same reason one snapshot could not capture them.

**So the two failures have a single root cause, and it is already in the dossier.** The per-draw
constant-buffer pooling that killed the batched snapshot also kills per-eye command-list re-execution.
Recording that link is worth more than either fact alone: it means the chosen record-time mechanism
is not a preference, it is the only one of the two that can work on this engine.

## 2. 🪤 `RestoreContextState=FALSE` leaves the context in its **default** state

Straight from the API reference: the flag *"determines whether the target context state is saved
prior to and restored after the execution of a command list. Use **FALSE** to indicate that no state
shall be saved or restored, **which causes the target context to return to its default state after
the command list executes**."* And the guidance points that way: *"Applications should typically use
**FALSE**"*, for performance.

**Why it matters here:** it is very likely the game passes `FALSE`. Any hook that runs *between* two
executions, or that adds an execution of its own, must not assume the immediate context still holds
the bindings the previous list left — render targets, constant buffers, shaders and everything else
are back at defaults. A per-eye pass that quietly inherits nothing will fail in a way that looks like
a patching bug rather than a state-management one.

This bites the record-time design too, not just the rejected one: if the second per-eye draw is
issued anywhere other than immediately alongside the first, inside the same recorded stream, the
state it inherits is not obviously the state you reasoned about.

## 3. 🚨 `ExecuteCommandList` can **silently decline to execute** — a documented silent no-op

The most valuable line on the page, and the least likely to be found by accident:

> *"This method performs some runtime validation related to queries. Queries that are begun in a
> device context cannot be manipulated indirectly by executing a command list… If such a condition
> occurs, **the ExecuteCommandList method does not execute the command list.** However, the state of
> the device context is still maintained, as would be expected (`ClearState` is performed, unless the
> application indicates to preserve the device context state)."*

`ExecuteCommandList` returns `void`. There is **no return code to check**. So a command list can be
submitted, silently discarded, *and still clear the context state on the way past* — leaving a frame
that renders wrongly with no error anywhere.

**Why this project should care specifically:** a hook that injects or re-orders command-list
execution around a game that uses D3D11 queries (occlusion, timestamps, predication — all normal in
a renderer of this vintage) can trip this condition. The symptom would be a missing or wrongly-stated
pass, and the natural diagnosis would be "my patch is wrong" rather than "my command list was never
run". This is precisely the failure class the cross-engine library now files under **silent no-ops:
verification that cannot see the failure**, and it deserves a debug-layer check rather than trust.

**The cheap guard:** run with the **D3D11 debug layer** enabled during bring-up. The runtime
validation above is exactly what the debug layer reports, and it turns an invisible discard into a
message. Worth doing before, not after, chasing a patching bug.

## What is NOT established

- All of the above is **API semantics**, `[reported]` from published first-party Direct3D
  documentation (read 2026-09-01) — it says what Direct3D guarantees, not what this game does. **Whether The Evil
  Within uses queries at all, and whether it passes `TRUE` or `FALSE` for `RestoreContextState`, is
  unmeasured** and both are answerable statically or with one instrumented run.
- The per-draw density figures (~1900 draws, ~6 buffer identities) are this project's own prior
  measurements, quoted here rather than re-derived.
- Nothing here has been run. The dossier's §12 risk stays open — this narrows *which* risk it is.

## Concrete next steps

1. **Log `RestoreContextState` at every `ExecuteCommandList`** in the proxy. One boolean, and it
   decides how much state the per-eye pass must re-establish.
2. **Log whether the game creates or uses `ID3D11Query`** objects. If it does, the silent-discard
   condition above is live and worth designing around.
3. **Enable the D3D11 debug layer for the first stereo bring-up run**, so a declined command list
   announces itself instead of presenting as a patching failure.
4. Fold the §11 link into the dossier: the per-draw pooling that killed the batched snapshot is the
   same thing that makes command-list re-execution unusable — one root cause, two closed doors.

## Sources

- [Command List — Direct3D 11, Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-command-list)
  (immutability, record-once/play-back, deferred-record/immediate-playback rule)
- [`ID3D11DeviceContext::ExecuteCommandList` — Microsoft Learn](https://learn.microsoft.com/en-us/windows/desktop/api/D3D11/nf-d3d11-id3d11devicecontext-executecommandlist)
  (`RestoreContextState` semantics and the default-state consequence; the query-validation silent
  discard and its `ClearState` side effect)
- [Immediate and Deferred Rendering — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-render)
- Our own prior measurements, quoted: `ENGINE-DOSSIER.md` §7 and §11 (the per-draw buffer pool and
  the data-insufficient batched-snapshot design)
