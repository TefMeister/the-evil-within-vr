# On a deferred context, `Map` hands back command-list scratch memory — which explains one branch of the re-test's own reading table, and puts a structural limit on Map-time stereo

**Status:** 🆕 new · **Priority:** medium — **most of what this session went looking for was already
known here**, and the topic says so up front. What survives is three things the record does not
carry, one of which bounds what the deployed dynamic path can ever do.

## ⚠️ First: what was already known, so this is not read as new

The commissioned research assumed less than this project already knows. Correcting that, from the
dossier and board:

- **"How many deferred contexts?"** — already measured: *"**Six** worker-thread deferred contexts
  record… the deferred command lists carry the bulk of the world (~7× the …)"*. The public record
  has **nothing** on id Tech 5 / STEM's D3D11 threading, and the research confirmed that gap is real
  rather than a search failure — but it does not matter, because this project measured it.
- **"Key the pairing on buffer identity, not thread"** — already done. The board's reading table for
  the pending re-test literally says *"no-map==unmaps ⇒ the **`(ctx,res)`** key is still wrong"*, and
  the note is titled *"the pairing was never a thread ring, it was a 32-entry table keyed on the
  wrong thing"*.
- **"Substitute the buffer at bind/draw time rather than editing at Map"** — already the main path:
  the dossier describes reading at each deferred draw into *"our own scratch buffer (a per-thread TLS
  ring), rebind slot 0 before the"* draw.

So the headline recommendations that came back are, here, **already implemented**. Three things are
not.

## ⭐ 1. Deferred `Map` does not touch the real resource — and that is a second reason the `unmaps==0` branch could fire

The board's reading table already anticipates one cause:

> "**maps>0 but unmaps==0 ⇒ our `Unmap` hook never sees these buffers** — deferred contexts use a
> different vtable flavour and `Map`/`Unmap` are hooked once on the immediate context and never
> late-hooked."

There is a **second, semantic** cause, and it is worth having in hand before the log is read
`[reported 2026-09-07]`:

On a deferred context, `Map` does not touch the real resource at all. The runtime/driver hands back a
**fresh scratch allocation owned by that context's command list**, and the write is committed when
the list is replayed. An implementation of the D3D11 contract shows how far that goes: in DXVK,
**`Unmap` on a deferred context is a no-op**, with the update committed *in `Map`*
(`WRITE_DISCARD` allocates a new slice; `WRITE_NO_OVERWRITE` looks up the existing entry and errors
if there was no initial discard, which is a documented requirement on deferred contexts).

**So `unmaps==0` would not necessarily mean the hook is blind.** It could mean the pairing being
counted is not a pairing the deferred path is obliged to make in a way our hook can observe. The two
causes are distinguishable by whether `Unmap` is reached *at all* on any context — worth separating
in the counter rather than reading `unmaps==0` as the vtable story by default.

⚠️ It also means **the same constant buffer can legitimately be mapped simultaneously on two
different deferred contexts**, which is why a resource-only key is unsafe and `(ctx,res)` is right.
This project got that right already; this is the *reason* it is right, which the record does not
state.

## ⭐⭐ 2. The structural limit: a per-eye value written at `Map` time is baked into the recorded list

This is the one worth a decision rather than a note. `[hypothesis 2026-09-07]` — it is a consequence
of the documented deferred model, not something observed in a tool or in this game.

Because a deferred `Map` writes into command-list-local memory that is **replayed later**, a value
stamped there is fixed at record time. If a recorded list is replayed once per frame, an edit made at
`Map` time can carry **one** matrix — so it cannot express a different value for the left and right
eyes from a single recorded upload.

**Why that matters here specifically:** the dossier already establishes that the deferred command
lists carry the bulk of the world (~7×), and that the main patch path works at **draw** time (read,
substitute a scratch buffer, rebind slot 0). Draw-time substitution is per-draw and *can* emit
different values per eye; Map-time editing on a deferred context structurally cannot.

That does **not** condemn the dynamic path — its job is the ~quarter of MVP-bearing draws whose cb0
is a per-shader `DYNAMIC` buffer the pool patch never intercepts, and for a *mono* correctness test
(`TEST_YAW=90`) Map-time editing is fine. But when stereo arrives, **the dynamic path may need to
move to draw-time substitution the way the pool path already has**, and it is cheaper to know that
before the design hardens than after.

Corroborating the direction: **3Dmigoto's canonical stereo mechanism is not CPU-side constant-buffer
editing at all** — it injects a small stereo-parameter resource and patches the shaders, applying the
eye offset in the shader. Its Map/Unmap interception exists mainly for resource hashing and
contamination tracking. `[reported]` A tool built specifically to do per-eye constant work chose not
to do it at Map time.

## ⚠️ 3. A named deadlock this hook shape is prone to

3Dmigoto issue #104, *"Lock ordering bug (Deadlock) between 3DMigoto and DirectX"* `[reported]`:

> "We take our critical section, then call into a DirectX routine … which takes their lock … DirectX
> takes their lock … then calls `Release()` … which calls our release tracker. Our release tracker
> attempts to take our critical section — Deadlock!"

**The rule that falls out: never hold a global lock across a call back into D3D inside a `Map` /
`Unmap` hook.** With six deferred contexts and a shared table, this project has exactly the shape
that bug lives in. Worth one look at the deployed code before the re-test, because a deadlock here
would present as a hang and cost a launch to diagnose.

Two related design notes from the same corpus, both cheap:

- A **per-context** state object with the map table inside it needs **no lock on the inner table** —
  a `ID3D11DeviceContext` is single-threaded by API contract (*"only one thread can call a
  ID3D11DeviceContext at a time"*, first-party vendor documentation), so whichever worker is driving
  that context owns it exclusively. The lock is only needed for the outer context→state lookup. That
  is also the cleanest statement of *why* the original per-thread ring was on the wrong axis:
  **threads are unbounded and transient; contexts are few and stable.**
- **Cache `GetType()` per context** rather than querying repeatedly (Special K's pattern, which keeps
  paired context/bool arrays for exactly this).
- RenderDoc keys its open-map table on **resource *and* subresource**; 3Dmigoto keys on resource
  alone and **silently no-ops an unpaired `Unmap`**. For constant buffers the subresource is always
  0, so this is a correctness nicety here rather than a fix — but the "unpaired Unmap is a no-op, not
  an error" precedent is worth knowing, since this project counts them.

## Confidence, plainly

- Deferred `Map` returning command-list scratch memory, and `Unmap`-as-no-op in an implementation:
  **`[reported 2026-09-07]`**, from first-party vendor documentation plus DXVK's source.
- Context single-threadedness: **`[reported]`**, first-party vendor documentation, verbatim.
- The per-eye/command-list limit: **`[hypothesis]`** — a consequence of the documented model, not
  observed here.
- The deadlock shape: **`[reported]`**, a real filed issue against a tool of the same kind.
- **Nothing here has been run against this game.**

## The concrete next steps

1. **Before the re-test launch**, check the deployed hook for the lock-across-D3D-callback shape
   (§3). One read, and a deadlock avoided is a launch saved.
2. **When reading the log**, treat `unmaps==0` as having *two* candidate causes, not one — the vtable
   story the board names, and the deferred-semantics story above. Separating them may be as cheap as
   counting whether `Unmap` is reached on *any* context.
3. **Park, do not act on, the stereo limit** until the mono re-test has passed. If it passes and
   stereo is next, revisit whether the dynamic path should move to draw-time substitution like the
   pool path.

## Sources and credit

Read online only; nothing cloned, downloaded or copied.

- **Microsoft** — first-party Direct3D 11 documentation: `ID3D11DeviceContext::Map`, *Introduction to
  Multithreading in Direct3D 11*, *Immediate and Deferred Rendering*, *How to: Use dynamic
  resources*, `VSSetConstantBuffers1`.
- **doitsujin (Philip Rebohle)** and **K0bin** — DXVK, whose deferred-context implementation
  (`d3d11_context_def.h/.cpp`) is where the "Unmap is a no-op; committed in Map" behaviour is
  legible. <https://github.com/doitsujin/dxvk>
- **bo3b** and **DarkStarSword** — 3Dmigoto: the map-table design, the unpaired-`Unmap` no-op, the
  documented deferred-context caveats, the canonical shader-side stereo mechanism, and **issue #104**
  (the lock-ordering deadlock). <https://github.com/bo3b/3Dmigoto/issues/104>
- **baldurk (Baldur Karlsson)** — RenderDoc, for the `{resource, subresource}` open-map key and its
  per-context wrappers. <https://github.com/baldurk/renderdoc>
- **crosire** — ReShade, whose add-on event signatures separate buffer/texture mapping from
  command-list binding events.
- **Kaldaien** — Special K, for the cached per-context type/handle pattern and a public report of an
  engine spawning one deferred context per logical core (16 on an eight-core part).
- **J.M.P. van Waveren / id Software** — *"id Tech 5 Challenges: From Texture Virtualization to
  Massive Parallelization"*, SIGGRAPH 2009, for id Tech 5's job-system architecture.

## What came back empty, and whether it is real

- **id Tech 5 / STEM's D3D11 deferred-context count and constant-upload strategy: not found**, and
  assessed as a **genuine gap in public knowledge** rather than a search failure — id's own id Tech 5
  shipped on **OpenGL** (so id's publications would not describe a D3D11 design at all), and the
  D3D11 backend in this game is Tango's own addition with no public technical writing. Checked: the
  SIGGRAPH material, engine press, console-command documentation, PCGamingWiki, and hardware-forum
  threads. **It does not matter — this project measured it (six contexts) rather than reading it.**
- **The SIGGRAPH PDF could not be read** by the fetcher (returned as undecoded binary, 37 pp) — a
  tooling failure, not a negative. Its architectural claim above comes from secondary description.
- **Two forum threads returned HTTP 403** to automated fetch, so the deferred-`Map` quotes from them
  are **second-hand search snippets** and are weaker than the vendor-documentation and source claims.
  They are corroborating, not load-bearing.
- **Several source files truncated silently** mid-fetch, so it is **not** established whether
  3Dmigoto wraps or merely hooks deferred contexts. Recorded as unknown rather than guessed.
- **vorpX**: closed source, nothing beyond marketing-level description. **VK3DVision**: not found.
