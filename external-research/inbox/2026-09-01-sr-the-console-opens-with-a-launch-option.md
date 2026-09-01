# The developer console opens with a launch option, and `noclip` works — worth confirming before more static-only work

**From:** `/sr` cross-project sweep, 2026-09-01 (dev PC)
**For:** `/gr the-evil-within-vr` — research it properly and curate it into `external-research/`.
**Confidence:** `[reported]` — mainstream press and wiki sources, 2014-era, none tested by us and
none checked against the build installed here.

## What the sweep found

The Evil Within ships id Tech 5's developer console, and multiple independent mainstream sources
(PC Gamer, DSOGaming, Kotaku, GamesBeat, the game's wiki) give the same activation route:

- **Launch option `+com_allowconsole 1`** (Steam → Properties → Set Launch Options).
- **Open the console with `Insert`**, not tilde.
- Working commands reported include **`noclip`** (walk through walls), `God`, `g_infiniteammo`,
  `g_stoptime` (stops time), `giveunlock`, and `devmapjump <stage>`.

## Why this matters to this project specifically

This project is static-only so far — the shader-layout work of 2026-09-01 bounded the coverage gap
to ten shapes entirely off disk, which is excellent, and it also means nothing has been driven live.
A working console changes what a first live session can do:

- **`noclip` is a camera-decoupling primitive.** It is not a free camera, but a player who can pass
  through geometry is the cheapest possible test of whether this engine's culling follows the camera
  or the player — the question that has cost other projects in this estate weeks.
- **`g_stoptime` is a research instrument.** A frozen world with a movable view makes every
  before/after comparison trivially repeatable, which is exactly what the measurement discipline in
  the cross-engine library keeps asking for.
- **A console at all means a possible ground-truth readout.** Check for an id-lineage
  `getviewpos` / `where` equivalent: on the sibling engine that command turned the console into a
  permanent camera ground-truth instrument and made the eventual value-search hunt possible.

## What to check, and the specific trap to avoid

1. **Confirm `+com_allowconsole 1` still works on the currently installed build**, not the 2014 one
   the sources describe. Every source found is from launch year.
2. **Enumerate rather than assume.** If there is a `listCvars` / `listCmds` equivalent, capture the
   full list and count it. On the sibling engine the retail console registers a small fraction of the
   vocabulary while looking complete, and knowing the number is what made a later discovery legible.
3. **Look for a camera position printer first** — it is the highest-value single command for this
   project and costs one line.
4. **Do not trust a short-token string sweep.** This library has a recorded case of a `strings`
   default minimum length of four silently hiding three-character command names on this exact engine
   family and producing a confidently wrong published conclusion.

## Where this came from

Turned up while generalising a `doom-2016-vr` finding: on **id Tech 6** the retail console is gated
by production mode and a community tool re-adds the hidden interface. Checking whether the same shape
exists elsewhere in the estate found that **id Tech 5 simply hands you the console via a launch
option**. Same engine family, one generation earlier, far less locked down.

Sources: [PC Gamer](https://www.pcgamer.com/the-evil-within-debug-console-commands-detailed-god-mode-is-in/) ·
[DSOGaming](https://www.dsogaming.com/news/the-evil-within-console-commands-revealed-unlock-framerate-slow-down-time-enable-god-mode/) ·
[The Evil Within Wiki — Console commands](https://theevilwithin.fandom.com/wiki/Console_commands) ·
[Kotaku](https://kotaku.com/how-to-get-infinite-ammo-and-invincibility-in-the-evil-1646095871)
