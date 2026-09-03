# Offline test: `mvptable_reflect_rows()` against the game's own shaders

`reflect_rows_test.c` links the **shipped** `src/mvptable.c` and calls
`mvptable_reflect_rows()` directly — not a transcription of it — over every vertex shader the game
ships, and compares each result against an independent reflector.

**It launches nothing.** It is an ordinary console exe; the game is never started, and no debugger
is involved.

## Why this exists

`mvp_row_offsets_for_shader()` used to refuse any shader whose `mvpmatrixy/z/w` rows were not at
`x+16/+32/+48`, because the table kept only a contiguity boolean and the real offsets had been
thrown away. It now keeps and returns all four. That change is only safe if the offsets reflection
reports are actually right, so they are checked against a second, unrelated implementation:

| | reflector |
|---|---|
| the proxy (under test) | `D3DReflect` from the system `d3dcompiler_47.dll` |
| the expectation | `flat-to-vr-RE-toolkit/tools/dxbc-reflect.py`, a from-scratch RDEF walker |

Same bytes, two different parsers. Any disagreement is a real defect in one of them.

## Result on 2026-09-03

```
shaders reflected      : 2785
  no cb0 found         : 386
  all four rows found  : 1192  (contiguous 997, scattered 195)
  disagreements with the independent RDEF parser: 0

patchable under the OLD contiguous-only rule : 997
patchable under the NEW rule                 : 1192  (+195)
```

## Running it

The shader blob is **game content and is deliberately not committed.** Regenerate it first with the
extractor in the dev-archive, which unpacks `base/common.tangoresource`:

```bash
# 1. extract every DXBC container the archive holds (writes tew_all_shaders.bin)
python <repo>/dev-archive/recon/2026-09-03-tangoresource-and-branch-merge/tangoresource_extract.py

# 2. build and run
gcc -O2 -Wall -Wextra -Wno-unused-parameter -o build/reflect_rows_test.exe \
    test/reflect_rows_test.c src/mvptable.c -ld3dcompiler -ldxguid
./build/reflect_rows_test.exe tew_all_shaders.bin tew_expected.txt
```

The expectations file is five integers per line, one line per shader in file order:
`<cb0 size> <x> <y> <z> <w>`, with `-1` for an absent row and `0` for "no cb0". Produce it with
`dxbc-reflect.py`'s parser, selecting the cbuffer the same way `reflect_find_cb0()` does —
`constantBufferV` if present, otherwise whichever cbuffer binds at `b0`.

⚠️ The counts above use that selection rule, so they differ slightly from a plain
"count `constantBufferV`" census of the same file (1,208 there against 1,192 here): a handful of
shaders bind a differently-named buffer at `b0`. The figures here are the ones that describe what
the proxy will actually do at runtime.
