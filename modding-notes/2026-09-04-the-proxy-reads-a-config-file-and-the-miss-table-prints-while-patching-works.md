# 2026-09-04 (`/pd`, dev PC, static only) — the proxy reads `tewvr.ini`, and the bucketed pool-miss table now prints while patching works

**The game was not launched, and nothing here has been run.** Both `[PD]` rows on the board are
closed by code that builds clean and is deployed; what they promise is measured by the next launch,
not by this note.

---

## 1. Every `TEWVR_*` knob now comes from a file as well as the environment

**The problem.** The visible yaw test failed to arm three launches in a row (2026-09-03e, f, g),
every time for the same reason: the game was started from Steam, whose environment no script can
set, and every knob the proxy has was read with `GetEnvironmentVariableA` at process start and
nowhere else. `launch-yaw-test.bat` works — but starting a game from Steam is the natural way to
start it, and a mechanism that only works when the user remembers a special launcher is a mechanism
that fails 3 in 3.

**The change.** New `src/config.c` / `config.h` on branch `stereo-6dof-core` (commit `3475717`):

- `tewvr_getenv(name, buf, size)` is a drop-in for `GetEnvironmentVariableA` with the **same
  return contract** (0 = unset; `>= size` = the size needed; otherwise the length). All 11 read
  sites across `cbdump.c`, `framecapture.c`, `mvp_patch.c`, `seqdump.c`, `shaderdump.c` were
  switched by one `sed`; the callers' own `len == 0 || len >= sizeof(flag)` checks are untouched.
- **Precedence, first source that defines a key wins:** the process environment → `tewvr.ini`
  beside `EvilWithin.exe` → `%LOCALAPPDATA%\TEWVR\tewvr.ini`. So `launch-yaw-test.bat` and every
  existing script behave exactly as before, and can override a file.
- **Read once**, from `DllMain(DLL_PROCESS_ATTACH)` right after `log_init()`, using only kernel32
  file APIs (the logger already opens a file there, so the loader-lock posture is unchanged). After
  that every lookup is a read-only table scan, so it is thread-safe from the render threads.
- **Format:** `KEY = value` lines, `#`/`;` comments, CRLF tolerated, UTF-8 BOM skipped (Notepad
  adds one), keys case-insensitive, the `TEWVR_` prefix optional, `KEY=` (empty) counts as unset,
  unparseable lines are logged and skipped, never fatal. Documented in `proxy-winmm/tewvr.ini.example`.
- **The log names the source.** `tewvr.log` now carries `config: read <file>: N setting(s) taken`
  per file and `config: TEWVR_TEST_YAW=90 (from D:\...\tewvr.ini)` once per key that was found —
  so a run can never again be ambiguous about whether and how the yaw was armed. The identity line
  now reads `TEWVR_TEST_YAW unset/0 (neither the environment nor tewvr.ini set it)`.

**Verified offline** `[verified-numerically 2026-09-04, n=14 checks]`: `tools/config_test.c`
links the **shipped** `config.c` (not a transcription) against a stub logger, writes two scratch
`.ini` files and checks every rule above — comments, CRLF, BOM, both prefix forms, case, trimming,
empty value, first-wins within and across files, a junk line, an over-long key, the too-small-buffer
return, environment-over-file, and the exact read shape `mvp_patch.c` uses for the yaw (yields
90.0). 14 of 14 pass, zero warnings at `-Wall -Wextra`.

```
gcc -Wall -Wextra -O2 -o build\config_test.exe tools\config_test.c src\config.c && build\config_test.exe
```

**Deployed, and ARMED.** `TheEvilWithin\winmm.dll` is the new build (349,696 B); the previous one
(342,528 B, the 2026-09-03 13:58 scattered-rows build) is kept as
`winmm.dll.bak-2026-09-04-pre-config-file`, and the older `winmm.dll.bak-2026-09-03-pre-scattered-rows`
is still there too. Beside it is a **`tewvr.ini` containing `TEST_YAW = 90`** with a header
saying so. **The next launch of The Evil Within on this PC, however it is started, runs the visible
yaw test.** To play normally, set `TEST_YAW = 0` in that file or delete it.

**NOT established:** that the file is actually read in the game process. The parser is proven; the
call from `DllMain` is compile-verified only. The first log line to check after the next launch is
`config: read D:\...\TheEvilWithin\tewvr.ini: 1 setting(s) taken`. If it is absent, the proxy did
not find the file (wrong folder, or `GetModuleFileNameW(NULL)` returned something unexpected under
Steam) and the environment route still works as before.

## 2. The bucketed pool-miss table existed all along — it just never printed

**The finding.** The `[PD]` row asked to "bucket the `pool_miss` counter by buffer size and usage".
`mvp_patch.c` has done exactly that since fix round 4: `mvp_miss_sample()` takes 1 in 256 misses,
calls `GetDesc` on the bound buffer, and tallies distinct
`(ByteWidth, Usage, BindFlags, CPUAccessFlags)` combos, each split by whether our `CreateBuffer`
hook ever saw the buffer created. **But `mvp_diag_report_misses()` was called only under
`pool_miss > 0 && patched == 0`** — "only while the round-4 mystery is happening" — so from the
moment patching started working, the table was never printed again. This is the **same gate that
hid the shadow-concurrency counters until 2026-09-01**; that fix moved *those* counters out from
behind it and left the miss table where it was. Every "we cannot tell what the missed draws are"
statement since (2026-09-03e/f/g) was true only because of that `if`.

**The change** (same commit):

- The table now also prints **every 6th 5-second window while misses continue** (~30 s cadence),
  and **once more at shutdown** from `mvp_patch_remove()`, so the session's cumulative picture is
  in the log however the cadence lined up with the quit. The failing-case behaviour is unchanged.
- **Each combo now also records what the missed DRAWS look like**, from arguments the draw hooks
  already had: `mvp_patch_prepare()` takes the draw's `IndexCount`/`VertexCount` and an
  indexed flag. Per combo: `geom` = sampled misses with count ≥ 300 (≥ 100 triangles — a mesh,
  not a sprite), `tiny` = count ≤ 6 (a full-screen pass or a billboard), `non-indexed`,
  `max_count`, and **up to 8 distinct vertex-shader hashes with hit counts**. The hash is the
  FNV-1a64 the proxy already keys shaders by, which matches the on-disk archive 167/168
  (2026-09-03c), so a missed shader can be **named** offline from
  `dev-archive/recon/2026-09-03-tangoresource-and-branch-merge/`.

**How to read it after the next launch.** Two lines per combo, e.g.

```
mvp_patch: DIAG miss-combo[0]: ByteWidth=176 Usage=2 BindFlags=0x4 CPUAccess=0x10000 | sampled misses: created-through-our-hook=812 NEVER-seen-by-our-hook=0
mvp_patch: DIAG miss-combo[0] draws: sampled=812 geom(count>=300)=3 tiny(count<=6)=790 non-indexed=790 max_count=1200 | vertex shaders: 3F2A... x790 9C11... x22
```

- `Usage=2` (`DYNAMIC`) with `CPUAccess=0x10000` (write) and a small `ByteWidth` ⇒ the **expected
  small per-shader dynamic cb0** of 2026-09-03f. If those combos are also mostly `tiny` /
  non-indexed and their shaders are not ones that appear in `mvp_offsets.log` with an MVP, they are
  post/UI passes and **the residual is harmless**.
- `Usage=0` (`DEFAULT`), `CPUAccess=0`, `ByteWidth` **outside** 1856–1984, with `geom` high and
  shaders that *do* carry an MVP ⇒ **a second world buffer size the pool filter refuses** — that is
  the case where widening `MVP_POOL_BUF_MIN/MAX_BYTES` (§7 of the dossier) is the fix.
- `NEVER-seen-by-our-hook` non-zero on a `DEFAULT` combo ⇒ the buffer was created on a device
  vtable we do not hook — the old (B) case from fix round 4, a different fix entirely.
- Multiply `sampled` by ~256 for absolute draw counts; the ratios are what matter.

**NOT established:** any of the above — the columns exist, they have not been filled. The thresholds
300/6 are conventions, not measurements; if a combo lands mostly between them, judge it by its
shaders, not by the counts.

## 3. Housekeeping done alongside

- **The branch merge was re-measured** in a throwaway clone after this commit: **19 added, 5
  modified, 0 deleted, one `README.md` conflict, nothing at the old `proxy-winmm/` path**
  `[verified-numerically 2026-09-04, n=1]`. The four new files all land in directories that already
  exist on both sides (`src/`, `tools/`, the `proxy-winmm/` root), so the merge is still one command.
  ⚠️ 19 files now exist only on the branch.
- `launch-yaw-test.bat` (staging) gained a note that the file route exists and that the environment
  it sets still wins.

## 4. What the next launch answers, and what each outcome means

One Steam launch, play a minute in any chapter, quit through the menus, read `tewvr.log`:

| Line | Meaning |
| --- | --- |
| `config: read ...\TheEvilWithin\tewvr.ini: 1 setting(s) taken` then `config: TEWVR_TEST_YAW=90 (from ...)` | the file route works; from here on a Steam launch can arm anything |
| `mvp_patch: TEWVR_TEST_YAW=90.000 -> test rotation K ACTIVE` and the world is visibly rotated | **the visible override is finally witnessed** — the 2026-09-03 `[FLAT]` row's question, "do the 34 scattered shaders rotate WITH the world?", is answerable from the screen |
| the `config:` lines are missing | the file was not found; check the folder, and fall back to `launch-yaw-test.bat` — the env route is unchanged |
| `DIAG miss-combo[...] draws:` lines | read them with §2's table; that decides whether the pool-miss residual is a problem or noise |
| `DIAG final bucketed pool-miss table at shutdown` | the cumulative version — the one to copy into the next note |
