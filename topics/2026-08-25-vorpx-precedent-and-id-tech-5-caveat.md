# vorpX works well for The Evil Within specifically — but a sibling id Tech 5 title (Wolfenstein: The New Order) can't do true 3D, so don't generalize from the engine family

**Status:** 🆕 new · **Priority:** medium — a feasibility signal specific to this exact game, with an
honest caveat about not over-generalizing from "id Tech 5" as a category.

## What was found

**vorpX has active, positive community use for The Evil Within** (and its sequel) — multiple current
vorpX forum threads ("The Evil Within 1 and 2 Game Pass," "I just need to say — Evil Within 2 in
Vorpx is amazing") and YouTube gameplay/tutorial videos confirm real, working use of vorpX against
this game family, with enthusiastic user feedback specifically for Evil Within 2. This research pass
could not confirm with certainty whether the *original* 2014 Evil Within (this project's actual
target) specifically achieves vorpX's higher-fidelity **Geometry 3D** mode versus the cheaper
Z-Buffer approximation — worth treating as "vorpX works here, mode unconfirmed" rather than assuming
the strongest possible case.

**A meaningful counter-example from the same engine, different studio**: **Wolfenstein: The New
Order** (2014, MachineGames) also runs on id Tech 5, but per a vorpX forum discussion, **it "can get
to run in 2D with vorpX, but 3D on them isn't doable"** — true stereoscopic 3D specifically fails for
that title, even though basic 2D/Cinema-mode display works. Depth3D (an alternative depth-based 3D
tool) is noted as having some success with older OpenGL-era Wolfenstein titles, but not clearly for
The New Order specifically.

## Why this matters — and why it's not a contradiction

This project's own `ENGINE-DOSSIER.md` already documents The Evil Within as running a **heavily
modified id Tech 5, internally called "STEM"** by Tango Gameworks — a materially different
customization of the base engine than MachineGames' own id Tech 5 fork used for Wolfenstein. The two
titles sharing an engine *family* name doesn't mean they share renderer-level implementation details
closely enough for a third-party tool's success/failure on one to predict the other — **The Evil
Within's own positive vorpX precedent should be trusted on its own merits, not doubted because of
Wolfenstein's weaker result, and vice versa: this project shouldn't assume its own id Tech 5 work will
be as friendly to third-party tools as Wolfenstein's, purely on shared engine-family branding.**
Treat this as confirmation that **this specific game** doesn't present unusual resistance to
third-party camera/stereo hooking — directly consistent with, and adding independent weight to, this
project's own already-successful native camera-override work (`10-we-can-move-the-camera.md`).

## Concrete next step

No action needed on the current roadmap — record as a positive, if imprecise, feasibility signal in
`ENGINE-DOSSIER.md`, and don't treat Wolfenstein's weaker id Tech 5 result as a reason for concern
about this specific game's own already-proven-working camera override.

## Sources

- https://www.vorpx.com/forums/topic/the-evil-within-1-and-2-game-pass/
- https://www.vorpx.com/forums/topic/i-just-need-to-say-evil-within-2-in-vorpx-is-amazing/
- https://www.vorpx.com/forums/topic/list-of-opengl-games-i-e-wolfenstein-new-order/
