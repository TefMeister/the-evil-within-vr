# The command-list topic carries an invented confidence tag, so no tool can see it

Filed by: `/gs`, 2026-09-01
For: `/gr` (curator of `external-research/`)

## What

`topics/2026-09-01-command-list-reexecution-cannot-do-per-eye-and-two-documented-hazards.md`
line 81 tags its central limitation with:

```
`[verified from published first-party documentation, 2026-09-01]`
```

That is not in the vocabulary. `CONVENTIONS.md` -> "Claim hygiene" defines six:
`[verified-live YYYY-MM-DD, n=K]` · `[measured …]` · `[inferred-static]` · `[reported]` ·
`[hypothesis]` · `[disproved …]`.

`[verified 2026-09-01]` — grepped the file for every vocabulary tag: **zero matches.** The
mechanical `/gs` check reads the whole document as **untagged**, i.e. `[hypothesis]`.

## Why it is worth fixing even though the writing is careful

The prose here is genuinely well disciplined — the tag sits under a **"What is NOT established"**
heading, it is dated, and the surrounding text is scrupulous ("it says what Direct3D guarantees,
not what this game does"; "Nothing here has been run"). A human reader is not misled at all.

The problem is purely that **an off-vocabulary tag is invisible to tooling**. It reads as the
strongest word available ("verified") to a human skimming, while every automated check counts the
document as carrying no confidence information whatsoever. That is the failure mode the tagging
convention exists to prevent, arrived at from the opposite direction.

## Suggested fix

API semantics taken from vendor documentation is `[reported]` — that is exactly what the tag
means: true as published, not confirmed in this binary. Keep the useful detail in the prose:

```
- All of the above is **API semantics**, `[reported]` from published first-party documentation
  (read 2026-09-01) — it says what Direct3D guarantees, not what this game does.
```

## Related, and NOT yours to fix

The same `/gs` run found the checker's own tag list is out of date with `commands/pd.md`, which
uses `[verified-numerically …]` and `[compile-verified …]` — two tags neither `CONVENTIONS.md`
nor `gs-scan.sh` knows. That is a separate modding-lane issue, reported to the user; it is why
`far-cry-2-vr`'s correctly-tagged dossier also shows as untagged. Mentioned so you do not chase it.
