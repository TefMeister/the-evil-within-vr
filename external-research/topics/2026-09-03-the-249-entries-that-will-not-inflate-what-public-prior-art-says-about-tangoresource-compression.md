# The 249 `.tangoresource` entries that will not inflate — what public prior art says about the container's compression

**Date:** 2026-09-03 · **Status:** 🆕 new · **Priority:** low, by request — the modding side asked for
this only *"if nothing turns up quickly rather than digging"*, and it does not block anything (no
shader was lost to it). **Answers:** the one remaining target in
`inbox/2026-09-03b-mod-container-format-solved-do-not-research-it.md`: *249 of 9,001 entries (2.8 %)
fail to inflate as raw deflate; cause unknown — a second codec, an undecoded per-entry flag, or
encrypted entries.*

## What turned up quickly

1. **The container has been public knowledge since 2014**, as aluigi's QuickBMS script
   `the_evil_within.bms` (versions 0.1 → 0.1.1 for PS3/X360 → 0.1.2a "compression algorithm
   refinement") and the ZenHAX thread around it. The in-house parse this week agrees with that history
   rather than contradicting it; nothing there says the layout differs from what the modding side
   `[verified-numerically 2026-09-03]` — magic, big-endian count, paired name records with a trailing
   hash, and a 10-byte-per-entry index of BE offset / csize / usize / id. `[reported]`
2. **The deflate is described as "not a normal deflate".** The script's author moved it from
   `unzip_dynamic` (auto-detects zlib vs raw deflate) to a **`deflate_noerror`** mode — i.e. inflate
   and *tolerate the end-of-stream error* — and the thread's summary is that the data is
   **zlib 1.2.3 deflate written with `Z_FULL_FLUSH`**, "the same method used by RAGE". `[reported]`
   That is the profile of streams that end on a **sync/full-flush marker (`00 00 FF FF`) instead of a
   final block**: a strict raw-deflate decoder reports "unexpected end of stream" even though every
   byte of payload has already been produced.
3. **Some entries are stored uncompressed.** A participant in the thread asks about "3 files top
   without compression", and the id Tech 5 `.resources` family this container descends from stores an
   entry raw when its compressed size equals its uncompressed size. `[reported]` — one thread, thin.
4. **A maintained tool handles every entry**: **Laura** (DTZxPorter & id-daemon, v3.32, 2026-08-28)
   extracts `.tangoresource` / `.ptr` for both Evil Within games — textures, models, animations,
   worlds, sounds and *raw files as-is*. It is a tool, usable directly under the no-copy rule; its
   compression handling is not documented on its page. `[reported]`

## Two cheap static checks this suggests, in order

- **Are the 249 exactly the entries with `csize == usize`?** If yes: they are stored raw, and the
  "second codec" is *no* codec. One comparison over the index already parsed.
- **If not: do they inflate when the end-of-stream error is ignored, or when the stream is fed as
  deflate with a trailing `00 00 FF FF` accepted?** The `deflate_noerror` history says the public
  script needed exactly that tolerance. Check whether the bytes produced before the error equal
  `usize` — if so, the entry was fine all along and the decoder was the strict half of the pair.
- Only if both fail does "encrypted or a second codec" remain — and then Laura's behaviour on those
  249 ids (does it extract them, and to what size?) is the next observation, no game running.

## Why this is tagged the way it is

Everything above is `[reported]` from a 2014 forum thread and two tool pages; nothing here was run.
The modding side's own first compression test was a false negative from looking for zlib framing that
headerless deflate does not have — the same *shape* of mistake (a strict decoder reading a flush
marker as truncation) is the most likely explanation for the 249, and the cheapest to test.

## Sources

- https://www.zenhax.com/viewtopic.php@t=248.html — "The Evil Within (*.streamed, .tangoresource)", the `the_evil_within.bms` thread: `unzip_dynamic` → `deflate_noerror`, "not a 'normal' deflate", zlib 1.2.3 + `Z_FULL_FLUSH` "same as RAGE", uncompressed entries, PS3/X360 support
- https://dtzxporter.com/tools/laura — Laura asset extractor (DTZxPorter & id-daemon), v3.32 dated 2026-08-28: supported containers and export types
- aluigi's QuickBMS script archive (`the_evil_within.bms`) — named in the thread; the script itself was not fetched (read the thread, not the code)
