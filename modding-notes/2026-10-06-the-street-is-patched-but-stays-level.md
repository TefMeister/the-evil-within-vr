# 2026-10-06: the street is patched, but still stays level

Dev PC, `/lm`, two launches, one background helper. Run on Opus (Tefa's dev-PC rule).

## The question

With the 15° tilt test on, the Chapter 2 safe room tilts but the Chapter 1 street outside the asylum stays level,
with tilted "ghost" copies of fences and the police car. Why does the street escape?

## What was done

- The helper found that the old draw recorder could not answer this, and built a new one (`DRAWTRACE`): every
  draw call of a frame, which shaders and screen it uses, and **why** our tilt was or was not applied. It also
  found that five kinds of draw call (instanced and indirect ones) were never looked at by our code.
- I ran it on the street (Chapter 1 autosave, after the opening cutscene), with a screen grab of the same frame.

## What the recording says (frame 11880)

- 7,843 draws. **Every draw that matters was tilted by our code**: 93.2% of the work through the main path, 6.8%
  through the second path; only 94 tiny draws had no camera at all. `[verified-live 2026-10-06, n=1 frame]`
- **Ruled out for this frame:** instanced or indirect draws (none at all), tessellation (none), and the same
  object drawn once tilted and once not (no such object).
- The helper then read the 12 biggest shaders (91% of the work): in every one, the matrix we tilt **is** the one
  that places the geometry on screen. No previous-frame matrix exists. `[inferred-static 2026-10-06]`
- **Motion blur is already off** and the edge smoothing is MLAA, which only looks at the current frame, so a blend
  of old frames is unlikely. `[verified-live 2026-10-06, seen in Options]`
- Yet the captured frame shows the street level. So the tilt is applied to the right number, on the right draws,
  and the picture still does not show it.

## What is left

The picture we see is not the picture those draws make, or the tilt is undone afterwards. Candidates for the next
session: the recorded lists replayed in a later frame without our edit (two lists were recorded before the trace
began; needs a several-frame trace to tell), the street drawn through a cached copy (a "virtual texture" or
pre-rendered layer the street reuses), or our edit applied after the GPU already copied the buffer.

## Also learned

- **The virtual controller walks the character**: two seconds on the left stick moved the camera clearly forward
  along the street. `[verified-live 2026-10-06, n=1]` The game is fully drivable now.
- **Cutscenes:** Escape pauses them; Tefa says holding the right mouse button skips them.
- **Menus:** arrow keys need the "extended" flag here, and the mouse pointer resting over a menu moves the
  highlight, so park the pointer in a corner before choosing. The window's close button does not quit; use Exit.
- The Chapter 1 autosave starts just before the opening drive (about 2.5 minutes of cutscene).

Evidence: `dev-archive/recon/2026-10-06-street-drawtrace/`; recorder and analyser in
`staging/the-evil-within-vr/reader-seqdump-2026-10-06/`.
