# The branch merge is safe (tested, not argued), and the game ships its shaders on disk

**2026-09-03, dev PC, `/pd` (parallel development).**
**The game was not launched. Nothing here has been run against The Evil Within.**

Three things moved: the `stereo-6dof-core` merge fear turned out to be unfounded and is now
measured rather than feared; `dxbc_disasm.c` was built and proven for the first time; and a route to
the shader bytecode that does not need a launch was opened, though not finished.

---

## 1. The `stereo-6dof-core` merge: the 110/110 fear is disproved

The board carried this as a decision waiting on a judgement call, on the understanding that a plain
merge *"would read as 110 files deleted plus 110 added"* and lose per-file history, because the
branch is stranded on the pre-consolidation path layout (`proxy-winmm/…` against `main`'s
`mod/proxy-winmm/…`). The row also carried the right instinct as a `[hypothesis 2026-09-02]`: git
detects wholesale renames at merge time, so the 110/110 might be a diff-display artefact.

**It was, and better than that.** Tested in a throwaway clone — the working clone was never touched,
nothing was pushed. `[verified-numerically 2026-09-03, n=2 independent runs, identical result trees]`

```
git -c merge.directoryRenames=true merge --no-ff stereo-6dof-core
```

produces, against pre-merge `main` (`f342896`, 90 commits; the branch `0c43e34` adds 17):

| | |
|---|---|
| files added | **13**, all under `mod/proxy-winmm/` |
| files modified | **5**, all under `mod/proxy-winmm/` |
| files **deleted** | **0** |
| files left at the old `proxy-winmm/` path | **0** |
| conflicts | **1** — `README.md` |

Git's `ort` strategy detects the `proxy-winmm/ → mod/proxy-winmm/` directory rename by itself and
routes the branch's edits to the new paths. The 13 files the branch *adds* are the only ones needing
help: without the flag git raises them as "file location" conflicts and *tells you the right
destination*; with `merge.directoryRenames=true` it moves them itself. Both runs produced a
byte-identical result tree (`594df774…`).

**Per-file history is not lost — it grows.** `git log --follow mod/proxy-winmm/src/hooks.c` reaches
**4** commits on pre-merge `main` and **9** after the merge. The rename is followed in both
directions.

**The one conflict is trivial, and one-sided.** The branch's `README.md` is the obsolete
pre-consolidation "six repositories for The Evil Within VR" nav page (from `b144d84`, which added a
sixth repo to a table that the 2026-08-30 consolidation deleted outright). `main`'s side is the
current one. Resolution is `--ours`; nothing of value is on the branch side.

### The part that turns this from tidying into a real cost

**`main` is not the live source. The branch is.** Thirteen files exist only on `stereo-6dof-core`,
including `mvp_patch.c/h`, `mvptable.c/h`, `camera.c/h`, `shaderdump.c/h`, `seqdump.c/h`,
`framecapture.c/h` and `tools/dxbc_disasm.c`. `main`'s `mod/proxy-winmm/src/` has no MVP patch in it
at all — a build from `main` today is a Task-3-era proxy. Every day the branch stays unmerged, the
public-facing default branch misrepresents the project, and the tool §12 depends on is unreachable
from it.

**Recommendation:** merge it, with the flag above, resolving `README.md` with `--ours`. **Not done
here** — the board's own row says "do not MERGE", and repo topology is the user's call, so this is
left as a one-command action with a measured outcome rather than performed. Worth noting that the
2026-09-03 audit already established the working clone is clean and *behind* GitHub, so the original
reason for the caution — protecting uncommitted work — no longer applies.

---

## 2. `dxbc_disasm.c` had never been built. It works.

§12's plan for the 34 scattered-row shaders is "one launch with the shader-dump path, then
`dxbc_disasm.c` offline". The tool is 86 lines, is referenced by **no build script and no
documentation**, and there is no evidence it had ever been compiled.

- **Builds clean, zero warnings** (`gcc -O2 -Wall`, llvm-mingw). `[compile-verified 2026-09-03]`
- **Run end-to-end against a real SM5 DXBC vertex shader**, it loads `d3dcompiler_47.dll`, calls
  `D3DDisassemble`, and prints the input/output signatures and the full instruction stream.
  `[verified-numerically 2026-09-03, n=1]`
- Critically, its output contains the literal **`cb0[N]` row indices** the scattered-MVP work needs —
  on the test shader, `cb0[9]`, `cb0[12]`, `cb0[13]`, `cb0[20]`…`cb0[28]`. That is exactly the
  `(base, contiguous)` → four-explicit-row-offsets conversion §12 describes.

So the launch is now the **only** missing input, not the launch plus an unproven tool. (The test
vector was a DXBC blob taken from another game's shader bundle purely as a valid input; it is not
committed, and it says nothing about The Evil Within.)

---

## 3. The game ships its shaders on disk — route opened, not finished

Nothing in the dossier had recorded this, and the whole coverage-gap plan assumed a runtime dump is
the only source of vertex-shader bytecode. It may not be.

`base/common.tangoresource` contains **76 entries** named
`generated/renderprogs/shader_retail/pc/<name>.shaderbin2` (each paired with a `_ws` variant). The
exe corroborates the vocabulary — it contains `renderprogs` ×13, `shaderbin` ×2, `tangoresource` ×1.

**The container's table of contents is fully parsed** `[verified-numerically 2026-09-03]`:
magic `0x2394ABCD`, a big-endian entry count, then `count × { u32 len, name, u32 len, name, u32 hash }`.
The walk consumed **exactly 11,565 records**, matching the header's own count — which is what makes
this a parse rather than a guess.

**The payload is raw DEFLATE**, headerless. A raw inflate (`wbits=-15`) at `0x1902F0` produces
**916,986 bytes of coherent game data** — `release.cfg` text, then material declarations — before
losing sync. `[verified-numerically 2026-09-03]`

### What is NOT established

- Entry **offsets are not in the TOC**, so individual entries cannot yet be addressed. The decode
  above is one continuous run, not a per-entry extraction. A regular 10-byte-record index sits at the
  end of the file and is the obvious next target.
- **Whether a `.shaderbin2` contains DXBC at all is unknown.** It may be id Tech 5's own IR, in which
  case this route dies. The archive holds zero literal `DXBC` bytes uncompressed, and so does the
  exe, so if it is there it is behind the compression.

### The next step, and what each outcome means

Parse the trailing index, extract one `.shaderbin2`, look at its first four bytes.
**`DXBC`** ⇒ §12's shader-bytecode row can be retired with no launch at all, for every shader at
once rather than only those a session happens to exercise. **Anything else** ⇒ the archive holds an
intermediate form, the route dies, and the launch stays the only way. Either answer is worth having;
it is a `[PD]` task, not a contingent one.

### The methodology mistake, which is worth more than the finding

The first scan looked for **zlib-framed** streams (`0x78 0x9C`, `0x78 0xDA`), found 3,489 candidate
positions, successfully decompressed exactly one, and was on its way to being written up as "the
archive is not zlib-compressed". **That test could not have produced a positive result** — the data
is headerless raw deflate, which has no such signature. The retry with `wbits=-15` found it on the
first attempt.

This is the standing rule biting in a new place: *a negative result is only evidence if the test
could have found the thing.* Before recording "X is not present", state what a positive would have
looked like and confirm the test could have seen it.

---

## Files

- `dev-archive/recon/2026-09-03-tangoresource-and-branch-merge/` — the merge-test transcript, the
  TOC parser (`tangoresource_toc.py`, reusable against any `.tangoresource`), and the
  `dxbc_disasm` proof run.
- Dossier: new **§3a** (on-disk shader archive), §12 updated for the tool and the route, §12's
  Domain-Shader bullet updated from the drained `/gr` inbox drop.
