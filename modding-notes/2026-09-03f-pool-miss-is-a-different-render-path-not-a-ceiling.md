# 2026-09-03f (`/lm`, dev PC) — `pool_miss` is a DIFFERENT RENDER PATH, not a ceiling; and the yaw run did not happen

Short session. The relaunch intended to arm the test rotation did not arm it, so the visual keystone
is still open — but reading the diagnostics properly **corrected a claim this project made only an
hour earlier**, in note `2026-09-03e` and on the board.

---

## 1. ⚠️ CORRECTION: `pool_miss` is not "the new ceiling"

`2026-09-03e` recorded `pool_miss = 24,574` against `patched = 88,421` and called it *"now the
dominant limit on draw coverage"* and *"the next ceiling"*. **That framing was wrong**, and the
proxy's own diagnostic — which was in the log at the time and which I did not read closely enough —
says so:

> `DIAG first pool-miss: … That buffer's real desc: ByteWidth=64 Usage=2 BindFlags=0x4
> CPUAccessFlags=0x10000 … to be registered it must be CONSTANT_BUFFER, Usage=DEFAULT(0),
> CPUAccessFlags=0 and 1856-1984 bytes … **A miss here is expected and harmless for the small
> per-shader cb0s (64/96/128/160/176/224/272B) this mechanism does not cover**`

The missed buffer is **64 bytes, `USAGE_DYNAMIC`, `CPU_ACCESS_WRITE`** — a per-object constant
buffer the game fills through `Map`/`Unmap`. The pool deliberately registers only the large shared
world constant buffer (`DEFAULT`, no CPU access, 1856–1984 B) whose contents are shadowed on
`UpdateSubresource`. **A dynamic per-object buffer cannot be shadowed by that mechanism at all**, so
missing it is the design, not a failure.

### The structural fact underneath, which is worth keeping

`[measured 2026-09-03, n=167 shaders]` Across every known/patchable shader in the log, the declared
`cb0` size runs **0 → 352 bytes**, and **not one is in the 1856–1984 pool window**:

| cb0 size | shaders | | cb0 size | shaders |
|---|---|---|---|---|
| 0 | 10 | | 160 | 18 |
| 16–96 | 28 | | 176–208 | 14 |
| 112 | 16 | | 224 | 33 |
| 128 | 19 | | 240–352 | 21 |
| 144 | 8 | | **1856–1984** | **0** |

That is not a contradiction: **D3D11 permits binding a buffer larger than the shader declares.** So
the game binds one big shared world constant buffer at slot 0 for the draws we patch, while each
shader declares only the window it reads. `patched` are the draws where that shared buffer is bound;
`pool_miss` are the draws where a small dynamic per-object buffer is bound instead. **Two different
render paths, not one path failing 19 % of the time.**

### What is genuinely open — stated as a question, not a defect

**Do the `pool_miss` draws carry world geometry that must move in stereo?** If they are effects, UI,
or per-object passes that already inherit a patched view, the misses cost nothing. If they are world
geometry on a dynamic-buffer path, they would stay mono and that *would* be a real ceiling.

**Nothing in this session answers that**, and the proxy currently cannot: it reports only the *first*
pool-miss, so the 24,574 are unbucketed. **The concrete next step is `[PD]`:** bucket the pool-miss
counter by buffer size and usage, so "expected small dynamic cb0" is separated from "a large
DEFAULT buffer we should have registered and did not". Until then, treating `pool_miss` as either a
ceiling *or* as harmless is a guess.

## 2. The yaw run did not happen — launched from Steam, so the env var was never set

`[verified-live 2026-09-03]` The log's newest line reads
`TEWVR_TEST_YAW unset/0 -> K = identity`. The cause, from the process table:

```
parent process : steam.exe
cmdline        : "…\TheEvilWithin\evilwithin.exe" +com_allowconsole
```

**The game was started by Steam, not by `launch-yaw-test.bat`**, so the environment variable the
launcher exists to set was absent. That is a perfectly natural way to start a game; the fault is a
launcher that only works when invoked in one specific way.

> ### ⚠️ `TEWVR_*` variables cannot be armed except by the launching process
> They are read at process start, and there is **no runtime channel**. The proxy's file-based
> triggers were checked directly: `capture.txt`, `seqarm.txt` and `skipcl.txt` exist, and **none of
> them arms the yaw**. So "just set it now" is not available — a fresh launch *from the launcher* is
> the only route, and asking for a relaunch must say that explicitly rather than saying "relaunch".

## 3. Two smaller live findings

- **Steam launch options carry `+com_allowconsole`** on this title. Pressing tilde produced **no
  console** — the game had dropped to its attract screen, which is what the frame delta was
  `[verified-live 2026-09-03, n=1]`. Not investigated further; the project drives this game through
  its own proxy, so a console is not needed.
- **⚠️ The `patched=88421` figures in note `2026-09-03e` came from the run before this one.** Log
  timestamps make it plain (`761…` for that session, `762…` for this one), and this launch only ever
  rendered menus and the attract screen. The measurements stand — they were taken during the session
  that was actually driven — but they are not from the launch that followed.

## 4. What is NOT established

- **The visible camera override.** Still open, still one armed launch away.
- **Whether `pool_miss` matters.** §1. The bucketing work is what decides it.
- **Whether a console exists on this build.** One tilde press against an attract screen is not a test.
- No VR, no headset, no comfort judgement anywhere in this session.
