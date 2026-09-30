# mvp_patch.c split into six files (move-only), ready to merge

From: the /lm static reader helper, 2026-09-30. Nothing was launched, pushed or copied into the game folder.

## What it is

`proxy-winmm/src/mvp_patch.c` on `stereo-6dof-core` (62f04a9) was 2,464 lines, past the 1,500-line hard limit. It is now split by job, with no behaviour change `[compile-verified 2026-09-30]`:

| File | Lines | Holds |
| --- | --- | --- |
| `mvp_patch.c` | 499 | module overview comment, `mvp_patch_prepare()`, Draw/DrawIndexed/CreateBuffer detours, `g_K` |
| `mvp_diag.c` | 411 | skip counters, rate limiter, CreateBuffer coverage record, miss sampler, periodic DIAG report |
| `mvp_shadow.c` | 637 | shadow storage + seqlock (shared by both pools), DEFAULT pool, UpdateSubresource source and late-hooking |
| `mvp_dynamic.c` | 279 | DYNAMIC pool, Map/Unmap source, the (context, resource) map table |
| `mvp_scratch.c` | 92 | per-thread scratch rings and lazy creation |
| `mvp_install.c` | 315 | `hook_one()`, `mvp_patch_install()`, `mvp_patch_remove()` |
| `mvp_patch_internal.h` | 363 | shared constants, typedefs, static asserts, `struct DirectPoolEntry`, extern declarations |

Every file is under 800 lines. `code-shape-scan.py`: before, `mvp_patch.c` OVER-HARD 2464; after, no mvp file is flagged (only `seqdump.c`, 1,858, remains OVER-HARD) `[verified-numerically 2026-09-30]`.

## Where it is

- Staging clone: `staging/the-evil-within-vr/mvp-split-2026-09-30/repo`, branch `split-mvp-patch-2026-09-30`, commit 54869ab on top of 62f04a9. Local tag `pre-split-2026-09-30` = 62f04a9. **Not pushed.**
- Patch: `staging/the-evil-within-vr/mvp-split-2026-09-30/patches/0001-proxy-winmm-split-mvp_patch.c-into-six-files-move-on.patch` (`git am` it onto `stereo-6dof-core`).
- The generator (`split.py`), the line check (`linecheck.py`) and the check script (`check.sh`) are in the same staging folder, so the split can be re-run if 62f04a9 moves on before it is merged.

## How it was proved

- **Move-only, mechanically:** the files are generated from line ranges of the original. A line-multiset check shows the only lines that left the original are 93 `static ...` definition lines, and every added line is one of: the same line without `static`, an extern declaration, a banner comment, or include/guard lines `[verified-numerically 2026-09-30]`.
- **The only semantic edits:** `static` dropped from 76 functions/variables another file now uses (declared in `mvp_patch_internal.h`); the old tentative-definition block became extern declarations; its four variables that had no initialised definition (`g_dyn_pool_count`, `g_diag_dyn_patched`, `g_diag_dyn_empty`, `g_dyn_shadow_writes`) are now defined, zero-initialised as before, in `mvp_dynamic.c`. Comments stay with their code, word for word; a few now say "above/below in this file" about code that is in a sibling file.
- **Build** with `build.ps1` (llvm-mingw clang 22.1.8), both before and after: success.
- **Exports:** the same 180 names and ordinals `[verified-numerically 2026-09-30]`.
- **Warnings:** `-Wall -Wextra` over every `src/*.c`: 0 before, 0 after.
- **Unit tests:** `map_pairing_test` 17/17 pass, `test_k_test` 12/12 pass, before and after.
- **Log text:** all 48 mvp/DIAG log strings in the DLL are identical.
- **DLL comparison: NOT byte-identical.** 355,328 bytes before, 358,400 after (+3,072). The split build is deterministic (two builds, same hash). `.text` grew 240 bytes, `.rdata` 552, `.pdata` 48, `.reloc` 140. Two causes, both expected when static functions move between files without LTO:
  1. Eight small helpers that the compiler used to inline into their one caller are now real out-of-line calls: `mvp_alloc_scratch_index`, `mvp_diag_maybe_report`, `mvp_direct_pool_find`, `mvp_dyn_pool_find`, `mvp_rate_limit_should_fire`, `mvp_seen_cb_record`, `mvp_shadow_note_writer`, `mvp_shadow_read`.
  2. mingw reaches extern variables through `.refptr.*` pointer stubs (60 new ones), which adds one pointer load per access.

## Unsure / worth knowing

- The draw hot path now makes about five extra plain function calls per draw, and some counter reads go through an extra pointer load. At ~110k draws/s that is far too small to see on the frame rate `[hypothesis]`, and the Interlocked and seqlock ordering is unchanged, since a function call is a stronger compiler barrier, not a weaker one. If it ever matters, `-flto` or `static inline` helpers in the header would bring the inlining back.
- Not tested in the game. It needs one menu-to-gameplay smoke run showing the same `mvp_patch: installed` and DIAG lines as a pre-split build, before anyone trusts it live.
