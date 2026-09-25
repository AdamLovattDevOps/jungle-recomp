# Findings

## Engine identity

`JUNGLE.EXE` is 7th Level's **Runtime Player 1.2 (1995)** — a generic engine, not game-specific code.
The game itself lives in the `.BIN` files as data. Anything learned here applies to every 7th Level title.

## Code surface (NE / Win16, 16-bit segmented x86)

| Binary | Code | Data | Segments | Exports | Role (inferred) |
|---|---:|---:|---:|---:|---|
| `JUNGLE.EXE` | 78,256 | 24,384 | 5 | 0 | Player entry point, main loop. Entry `seg 4:001a` |
| `JUNGU01.DLL` | 22,414 | 10,882 | 6 | 5 | Utility/runtime. Imports `WIN87EM` → x87 float emulation |
| `JUNGS01.DLL` | 28,870 | 32 | 2 | 2 | Graphics — only DLL importing `GDI` |
| `JUNGA01.DLL` | 17,239 | 1,957 | 2 | 1 | Animation/audio mid-layer |
| `JUNGR01.DLL` | 10,942 | 36 | 2 | 8 | **Resource + audio streaming — the `.BIN` reader** |

**Total code to match: 157,721 bytes.** Finishable.

Dependency order (leaf first): `JUNGU01` → `JUNGA01` → `JUNGR01` / `JUNGS01` → `JUNGLE.EXE`.

## Named symbols already recovered

From `JUNGR01.DLL`, C++ mangled (Microsoft) — these decode to real signatures:

```
?AUDIO_READ@@ZCJPAUTAGRESAUDIOINFO@@PIEJ@Z      AUDIO_READ(tagRESAUDIOINFO*, unsigned char*, long)
?AUDIO_REWIND@@ZCJPAUTAGRESAUDIOINFO@@@Z        AUDIO_REWIND(tagRESAUDIOINFO*)
?AUDIO_DONE@@ZCXPAUTAGRESAUDIOINFO@@@Z          AUDIO_DONE(tagRESAUDIOINFO*)
SWINMALLOC / SWINFREE / SWINREALLOC             engine allocator ("SWIN" = 7th Level SDK prefix)
LIBMAIN / WEP                                   standard Win16 DLL init/exit
```

`JUNGU01.DLL` exports `UTILGETTIME`, `ERROR_HANDLER`, `_SHI_INVOKEERRORHANDLER1`.

`tagRESAUDIOINFO` is a `RES`ource descriptor with a read/rewind/done interface — i.e. the container
is **streamed, not fully unpacked**. That shapes the port: the asset layer is a seekable stream, not a
load-all-into-memory archive.

## Container format — partial

- Magic `37 4C 1E 01` = `"7L"` + version `2` at `u32[1]`.
- Bytes `0x08`–`0x5F` are zero in all 13 files.
- `u32[24..27]` (`0x60`–`0x6F`) vary per file and ascend; 1–4 non-zero values. **Not** a full asset directory.
- Entropy is 7.0–7.9 bits/byte throughout → payload is compressed or encoded.
- Magic occurs exactly once per file → no per-chunk magic to scan for.
- The tail is lower entropy (4–6) but does not decode as an obvious directory either.

**Conclusion:** the directory is not findable by inspection. Stop guessing at hex and read
`JUNGR01.DLL` instead — 10,942 bytes of code that provably parses this format. That is the cheap path.

## Next actions

1. Install Ghidra (`brew install --cask ghidra`) — it has a working NE loader.
2. Import all five binaries into one project so inter-DLL calls resolve.
3. Start at `JUNGR01.DLL` `AUDIO_READ` / `AUDIO_REWIND` and recover `tagRESAUDIOINFO` field by field.
4. From that struct, the container layout falls out: offsets, sizes, and the decompressor.
5. Only then write the extractor.

---

# Round 2 — Ghidra headless, container format cracked

## Tooling

Ghidra 12.1.3 (`brew install ghidra`, formula not cask; needs `JAVA_HOME=/opt/homebrew/opt/openjdk@21`).
Its NE loader picks `x86:LE:16:Protected Mode` automatically and resolves the inter-DLL imports.

`tools/../ghidra_scripts/ExportDecomp.java` run headless exports decompiled C plus a function
inventory CSV per binary. **1,534 functions, 100% decompiled**, into `notes/decomp/`.

| Binary | Functions |
|---|---:|
| `JUNGLE.EXE` | 775 |
| `JUNGS01.DLL` | 219 |
| `JUNGA01.DLL` | 213 |
| `JUNGU01.DLL` | 204 |
| `JUNGR01.DLL` | 123 |

## The resource API is fully named in the binary

`JUNGR01.DLL` keeps its symbol names. The container API, for free:

```
RESOPENFILE   RESCLOSEFILE   RESREAD      RESREADDATA    RESSEEK
RESLOADRESOURCE   RESEXTRACTFILE   RESCOPYRESOURCE   RESDELETERESOURCE
RESLOADNAMETABLE  RESLOADPALETTE   RESLOADKEYRESOURCE   RESEXPANDBITMAP
RESGETTITLE   RESCREATEWAVEEVENT   RESCREATEMIDIEVENT
```

`RESEXPANDBITMAP` is the image decompressor. `RESLOADNAMETABLE` means resources have names.

## Header format — recovered from `RESOPENFILE`, not guessed

`RESOPENFILE` allocates a 436-byte handle, `mmioOpen`s the file, reads the header, then calls
**six table parsers in sequence**. The header is:

```
0x00  u16  magic        0x4C37  = "7L"
0x02  u16  headerSize   286 in every file on this disc
0x54  u16  version      must be 2
0xBE  6 x { u32 fileOffset, u32 byteLength }   the six resource tables
```

Validity checks the engine itself applies, and which `tools/res_header.py` reproduces:

- 8-bit sum of all `headerSize` bytes must be zero.
- Table 1 length must be non-zero.

**13/13 `.BIN` files validate.**

## Where the directory actually lives

The six tables sit **contiguously at the tail of the file**, which is exactly the low-entropy region
the entropy profile flagged in round 1 — the structure was always at the end, it just could not be
found by inspection. Example, `JUNGOPTS.BIN`:

```
table 1   offset 821594   length  2790     (279 entries x 10 bytes)
table 2   -                      empty
table 3   -                      empty
table 4   offset 824384   length     8
table 5   offset 824392   length    88
table 6   offset 824480   length 13769
```

Each table starts exactly where the previous one ends. Tables 2 and 3 are empty in all 13 files.
Table 1 has a 10-byte stride, confirmed by `RESOPENFILE` multiplying its entry count by 10.

The bulk of the file, everything before table 1, is the compressed payload.

## What this unlocks

The format is no longer a mystery, it is a reading exercise. Remaining work on Track B is
mechanical: decompile the five other table parsers the same way, name the 10-byte entry fields,
then `RESEXPANDBITMAP` for the decompressor.

## Next actions

1. Decompile the table-1 entry consumer to name its 10 bytes — almost certainly offset, length, type, id.
2. Do the same for tables 4, 5, 6 (small: 8, 88, 13769 bytes — likely palette, name table, and the scene script).
3. Recover `RESEXPANDBITMAP` and reimplement the decompressor in `tools/`.
4. Then write the extractor with a round-trip test.

---

# Round 3 — Directory decoded, resources classified

## Directory entry layout (table 1)

Recovered from `RESLOADRESOURCE`, which reads five u16s from the entry then seeks and reads:

```
0x00  u16  type
0x02  u32  fileOffset
0x06  u32  size          10 bytes total, matching RESOPENFILE's count * 10
```

`RESLOADRESOURCE` branches on type: **types 8–16 resolve from resident memory** blocks loaded at
open time, everything else is seeked to and streamed from the file.

Implemented in `tools/res_dir.py`. **23,276 resources across the 13 containers.**

The layout is proven, not assumed: header (286) + payload + the six tables tile each file exactly.
For `JUNGOPTS.BIN`, 286 + 821,308 = 821,594 = table 1's offset, to the byte.

## Resource types observed

| Type | Count | What it looks like |
|---:|---:|---|
| 1 | 11,506 | **Bitmaps** — confirmed, see below |
| 14 | 3,323 | small blobs, 26–907 bytes |
| 10 | 4,755 | almost always exactly 6 bytes |
| 15 | 1,304 | small, low-valued byte streams |
| 13 | 1,262 | 116–1,820 bytes |
| 7 | 628 | large, 27 KB–314 KB — likely audio streams |
| 9 | 163 | 40–216 bytes |
| 16 | 272 | resident |
| 11 | 51 | resident |
| 4 | 9 | — |
| 2 | 2 | — |
| 8 | 1 | — |

No standard file signature matches anywhere (no `RIFF`, `BM`, `MThd`). Everything is engine-native.

## Type 1 is a bitmap, and the art is 800x600

Every type-1 resource starts with a 20-byte header:

```
0x00  u16  headerSize   always 20
0x02  u16  stride       bytes per row
0x04  u16  width
0x06  u16  height
0x08  u16  flags        0x8000 set
0x0A  i16  signed offset (hotspot / placement)
```

`stride == width` throughout, so the pixel format is **8-bit paletted**, which matches
`RESLOADPALETTE` existing as a separate call.

Validation: 923/923 sampled type-1 entries have `headerSize == 20` and sane dimensions.

Dimensions seen include **800x600**, **640x480**, and many sprite-sized rects such as 424x176.
The game's source art is 800x600 — a genuinely useful result for a resolution-independent port,
since the assets are better than the 640x480 the original shipped at.

`width * height` versus stored size gives compression ratios of **2.3x to 17.3x**, so the pixel data
is compressed. The decompressor is `RESEXPANDBITMAP` -> `FUN_1000_02e8`.

## Read path

`RESLOADRESOURCE` picks between two readers:

- `FUN_1000_0a80` — plain `mmioRead`, unless the flag at `handle+0x116` is set, in which case it
  routes through `FUN_1000_0984`.
- `FUN_1000_07ae` — used when the u32 at `handle+4` is non-zero.

So the container payload itself is not encrypted in the general case; compression is per-resource.

## Next actions

1. Decompile `FUN_1000_02e8` (`RESEXPANDBITMAP`) and reimplement the bitmap decompressor.
2. Decompile `RESLOADPALETTE` to get the palette format, then write real PNGs.
3. That pair is the "see the game's art on screen" milestone and the proof the format work is correct.
4. Then tables 4/5/6 (palette, name table, scene script) and type 7 (audio).

---

# Round 4 — Bitmaps decoded. 11,479 PNGs extracted.

## The two codecs

`RESEXPANDBITMAP` -> `FUN_1000_02e8` branches on bit `0x80` of byte 9, i.e. the top bit of the
`u16` flags field at `0x08`:

- **bit set (`0x8000`)** -> `FUN_1000_0230`, a byte-oriented RLE. **Reimplemented.**
- **bit clear (`0x0000`)** -> `FUN_1000_218c`, a second codec. **Not yet reimplemented.**

## The RLE, transcribed from `FUN_1000_0230`

```
b == 0x00           end of row; a row that begins with 0x00 ends the image
0x01..0x7F          run: the next byte, repeated b times
0x80..0xFE          literal: copy (0xFF - b) bytes verbatim
b == 0xFF           literal: length is the next byte, then that many bytes
```

The original writes pairs at a time (`*(u16*)dst = value|value<<8`) purely as a 16-bit speed trick;
the odd-length handling makes the effective run length exactly `b`. That detail matters when
matching the assembly but not when reimplementing.

Layout of a compressed bitmap: 20-byte header, then `height` u16 row offsets, then the RLE stream.
The row table lets the engine seek directly to any scanline — it never has to decode from the top.
That is worth preserving in the port: it is how the original does partial redraws cheaply.

## Two corrections that mattered

Both were found by looking at the output, not by reading more code:

1. **Rows are stored bottom-up**, like a Windows DIB. Emitting them in stored order gives a
   vertically mirrored image that still looks structurally plausible — easy to miss.
2. **The palette is `PALETTEENTRY` (R,G,B,flags), not `RGBQUAD` (B,G,R,reserved).** Getting this
   wrong renders a brown wooden sign as a blue one: plausible, and completely wrong.

## Palette

Per `RESLOADPALETTE`: `u32 fileOffset` at header `0xEE`, `u32 byteLength` at `0xF2`, 4 bytes per
entry. This is table slot 7 of the array that starts at `0xBE`, so the "six tables" is really a
longer array.

**236 entries = 256 minus the 20 system colours Windows reserves.** The stored palette therefore
maps to indices 10..245, not 0..235. Index 0 appears to be the transparency key.

## Result

| | |
|---|---:|
| Type-1 bitmaps in the 13 containers | 11,506 |
| Decoded to PNG | **11,479 (99.8%)** |
| Skipped (the `FUN_1000_218c` codec) | 27 |

Roughly two per container are skipped, and they are the large full-screen images — so
`FUN_1000_218c` is very likely the background codec, used where RLE does nothing useful.

`tools/res_bitmap.py` does the whole chain: header -> directory -> palette -> RLE -> PNG, with no
third-party dependencies.

## Next actions

1. Reimplement `FUN_1000_218c` to recover the 27 full-screen backgrounds.
2. Confirm index 0 is the transparency key and emit `tRNS` so sprites composite correctly.
3. Type 7 (628 entries, 27 KB–314 KB) via `RESCREATEWAVEEVENT` / `tagRESAUDIOINFO` — the audio.
4. Tables 4/5/6 and `RESLOADNAMETABLE`, to get resource names instead of indices.
5. Then Track A: the engine's main loop in `JUNGLE.EXE`.

---

# Round 5 — Second codec cracked. 100% of bitmaps extracted.

## `FUN_1000_218c` is a container, not a codec

It walks a stream of chunks. Each chunk is a `u16` tag followed by `tag & 0x1FFF` bytes, with the
top three bits selecting the method:

| Tag bits | Method |
|---|---|
| `0x0000` | stored, copied verbatim |
| `0x2000` | `FUN_1000_26b4` — not used by any bitmap on this disc |
| `0x4000` | LZW, 10-bit codes |
| `0x6000` | LZW, 11-bit codes |
| `0x8000` | LZW, 12-bit codes |
| `0xE000` | end of stream |

A census across all 28 non-RLE bitmaps found **1,470 chunks, every one of them `0x6000`**, plus
exactly 28 end markers. Uniform data, and the chunk walk terminating cleanly on all 28 was the
first confirmation the framing was right.

Inside a chunk, `FUN_1000_263a` reads a run of sub-blocks, each a `u16` length then that many
bytes. Each sub-block is decoded independently — `FUN_1000_2470` clears the dictionary on entry,
so **the dictionary resets per sub-block**.

## `FUN_1000_2470` is LZW

The decompiled 16-bit output is hostile, but the structure is unmistakable once you see it:

- `next = 0x100` — the first free code sits immediately above the 256 literals.
- Three parallel arrays indexed by code: prefix (`int`), suffix (`byte`), length (`byte`).
- Emission walks the prefix chain backwards, then reverses — classic LZW string output.
- The `code >= next` branch emits the previous string plus the previous first character. That is
  the KwKwK case, and its presence alone rules out LZSS.
- Dictionary insert is `prefix[next] = prev`, `suffix[next] = firstChar`, `len[next] = len[prev]+1`.

`FUN_1000_22b8` supplies the parameters: code width N in 9..12 (default 11), dictionary `1<<N`,
code `(1<<N)-1` as end marker, `(1<<N)-2` as last usable entry. **N is fixed, never varied
mid-stream** — unusual for LZW and easy to get wrong by assuming GIF-style growing code width.

### The mask table gives away the bit packing

`FUN_1000_22b8` writes eight `u16` masks at `+0x20`. For N=11 they hold 0, 5, 2, 7, 4, 1, 6, 3
significant bits. That is exactly the leftover-bit count after each of the 8 codes that fit into 11
bytes — so the packing is **MSB-first**, and the table is just a precomputed rotation to mask off
already-consumed bits. Reading that table was faster than reverse-engineering the shift arithmetic.

## Verification

Size is the proof. A desynchronised LZW decoder drifts; it does not land on the right length.

**All 28 non-RLE bitmaps decode to exactly `width * height` bytes.** Not approximately — exactly,
in every case.

## A false alarm worth recording

The first decoded background looked structurally perfect — foliage, a carved mask, eyes — but
heavily speckled, which reads as a broken decoder. Run-length statistics said mean run 1.46 against
11.06 for a known-good RLE sprite.

That difference is real but it is not a bug. The RLE sprites are flat-shaded cartoon art; the
backgrounds are dithered photographic-style art, which is exactly why they compress badly under RLE
and were given to LZW instead. **The choice of codec per image is itself a signal about the image.**
The speckle is authentic 1995 dithering, and the exact-size check settles it.

## Result

| | |
|---|---:|
| Type-1 bitmaps | 11,506 |
| Decoded to PNG | **11,506 (100%)** |

Track B's bitmap path is complete. `tools/lz7l.py` holds the codec; it is content-agnostic and will
be reusable for any other resource type that turns out to be LZW-compressed.

## Next actions

1. Confirm index 0 is the transparency key; emit `tRNS` so sprites composite.
2. Type 7 (628 entries) — audio, via `RESCREATEWAVEEVENT` and `tagRESAUDIOINFO`.
3. `RESLOADNAMETABLE` and tables 4/5/6, for real resource names instead of indices.
4. Then Track A: `JUNGLE.EXE`'s main loop.

---

# Round 6 — Audio located and characterised, codec not yet cracked

## The audio resource header

`RESCREATEWAVEEVENT` reads 28 bytes off the front of a type-7 resource before handing the rest to
the streaming callbacks. Those 28 bytes are:

```
0x00  u32  dataSize    payload bytes = resourceSize - 28
0x04  u32  flags       always 1 on this disc
0x08  u16  param       0, 10 or 100
0x0A  u16  id
0x0C  WAVEFORMATEX:  tag=1 (PCM), channels=1, rate=22050, avg=44100, align=2, bits=16
```

**All 628 type-7 resources across all 13 containers declare the identical format**: 22050 Hz, mono,
16-bit. `avg == rate * align` holds, so the structure is internally consistent and is definitely a
real `WAVEFORMATEX`, not a coincidence.

Total payload: **~24.7 MB** across the disc, the single largest asset class after bitmaps.

## The payload is not PCM, despite the header

Writing a RIFF wrapper around the payload produces files that open cleanly, report the right format,
and play static. Three independent checks:

| Check | Result | What PCM would give |
|---|---|---|
| Mean \|sample delta\| / RMS | 1.03 | 0.2–0.5 |
| Amplitude histogram | flat across all 16 buckets | sharply peaked near zero |
| DC offset | −5640 | near zero |

A flat amplitude histogram is the decisive one. No amount of loudness or distortion makes real audio
uniformly distributed across its dynamic range.

Ruled out by direct test: 8-bit unsigned, big-endian 16-bit, byte-plane split (low|high and
high|low), delta coding, and the LZW chunk stream from round 5. None of them produce correlated
samples.

**The `WAVEFORMATEX` describes the decoded output, not the stored bytes.**

## Where the codec actually lives

Two dead ends, both settled by reading rather than guessing:

- `AUDIO_READ` zeroes `handle+4` and `handle+6` before reading, which forces `RESLOADRESOURCE`'s
  plain-read path. So no transform is applied there.
- `FUN_1000_0984`, gated on the byte at header `0xAE`, is **not encryption** — it is a 4092-byte
  linked-block reader that follows a `u32` next-block pointer every 4092 bytes. It is disabled
  (`0xAE == 0`) in all 13 containers anyway.

`RESCREATEWAVEEVENT` passes `AUDIO_READ` as a callback into `A_011`, an export of **`JUNGA01.DLL`**.
So JUNGA01 is the audio engine: it pulls compressed bytes through the callback and decodes them
itself. `JUNGR01` only ever moves bytes.

**The audio codec is in `JUNGA01.DLL` — 17,239 bytes of code, 213 functions, already decompiled in
`notes/decomp/JUNGA01.DLL.c`.** That is the next thing to read.

## Tooling

`tools/res_audio.py` parses the headers and dumps the compressed payloads as `.pcmz`. It
deliberately does **not** emit WAVs: a RIFF header on compressed bytes produces a file that looks
valid and is not, which is worse than no file at all.

## Method note

This round repeated round 1's mistake before correcting it: several hypotheses were tested against
the bytes before anyone read `AUDIO_READ`. The read took one lookup and immediately ruled out the
entire JUNGR01 side of the search. Guessing at data costs more than reading code, every time.

## Next actions

1. Read `JUNGA01.DLL`'s callback consumer to find the audio codec.
2. Meanwhile: `RESLOADNAMETABLE` and tables 4/5/6, for resource names instead of indices.
3. Confirm index 0 is the bitmap transparency key and emit `tRNS`.

---

# Round 7 — Tables 4/5/6; scope for a native port

## Table 5 is the string table

Null-terminated strings, recovered directly. From `JUNGMAIN.BIN`:

```
SlowCpu   Timon and Pumbaa's Jungle Games   LoMem   FreeSpace
Hyena     MusicOff   SoundFXOff   Intro
```

These are configuration keys and scene names — `MusicOff` / `SoundFXOff` are settings the port will
need to honour, and `Intro` is a scene reference. This is what `RESLOADNAMETABLE` and `RESGETTITLE`
serve.

## Table 6 is NOT a flat TLV stream

The opening records look like clean `u16 type, u16 length` records — `type 76, length 12`, repeating
with an incrementing index. That shape does not hold.

A TLV walk over all 13 containers parsed **0 of 13** cleanly to the byte, desynchronising after ~124
records and then reporting impossible type values (19456, 63744, 65026). Table 6 is either nested,
variable-form, or contains a section the walk must skip.

Recording this as a negative result rather than quietly dropping it: the first-records-look-right
trap is exactly what made round 1 and round 6 expensive. The fix is the same as it was both times —
read the six table parsers in `JUNGR01.DLL` (`FUN_1000_050c`, `04b8`, `075a`, `061a`, `0720`,
`066a`, ~1,500 bytes, already decompiled) instead of pattern-matching the bytes.

## Table 4

8 bytes in `JUNGOPTS.BIN`: `52 06 c8 00 5b 06 ff ff` — u16s 1618, 200, 1627, 0xFFFF. The 0xFFFF
terminator suggests a short index list of key resources. Not confirmed.

## Scope

See `docs/PORT_PLAN.md`. Summary: the asset pipeline is complete, the game logic is not decoded at
all, and the engine interpreter is larger than everything done so far combined.

---

# Round 8 — Addressing model solved; scene script located

## Table 6 is the resident blob, not a script

`FUN_1000_066a` loads table 6 as `segments` contiguous chunks — count at header `0xA0`, cumulative
end offsets as u32s at header `0x60`. Those cumulative offsets are exactly the varying values found
blind in round 1 at file offset `0x60`, now explained.

## Resources are addressed two different ways

This was the missing piece:

| Types | `offset` field means |
|---|---|
| 8–16 | an offset into the **resident blob** (table 6), loaded whole at open |
| everything else (1, 7, …) | a plain **file offset**, seeked and streamed |

Verified across containers: **every** type 9/10/13/14/15 resource lies inside the blob, and **no**
type 1 or 7 resource does. With the split applied, all 279 resources in `JUNGOPTS.BIN` and all 2,595
in `JUNGMAIN.BIN` fetch at exactly their declared size, none truncated.

`tools/res_dir.py` now exposes `fetch(data, entry)` which resolves either kind.

## Type 14 is the scene script

Round 7 reported table 6 as "not a flat TLV stream". That was right about table 6 and wrong about
the data: the TLV records are real, they just live inside **individual type-14 resources**, not
spread across the blob. Walking the whole blob crossed resource boundaries, which is why it
desynchronised after ~124 records.

A type-14 resource opens:

```
4c 00  0c 00  04 01 00 01 00 00 08 00
4c 00  0c 00  04 02 00 01 00 00 08 00
```

`u16 type = 76`, `u16 length = 12`, 8 bytes of payload, with one field incrementing per record.

There are **3,323 type-14 resources** across the disc — 119 in `JUNGOPTS.BIN`, the vertical-slice
target.

## Other resident types, first bytes

```
type  9   40 bytes   05 00 90 8b 92 8b 00 00 05 00 90 8b 94 8b 00 00
type 10    6 bytes   4f 8b 62 00 6e ff
type 13   10 bytes   01 00 0a 00 0a 00 00 00 cf 8a
type 15   30 bytes   00 00 01 00 01 00 00 00 00 00 00 00 ff ff 00 00
```

Type 10 is always exactly 6 bytes and there are 4,755 of them — a small fixed record, most likely a
point or a frame reference. Type 13 at 10 bytes with a leading `01 00 0a 00 0a 00` looks like a
rect or a count-plus-dimensions.

## Method note

Three rounds in a row now, the same pattern: a byte-level guess looked right on the first few
records and then fell apart. Each time, reading the function that parses the data settled it
immediately. Round 8's unlock came from 181 bytes of decompiled C, after two rounds of failed
pattern-matching on megabytes of data.

## Next actions (vertical slice, `JUNGOPTS.BIN`)

1. Decode the type-14 record vocabulary — census record types across all 119 resources.
2. Identify types 10, 13, 15 from their consumers in `JUNGLE.EXE`.
3. Map one screen: background bitmap, button sprites, hit rectangles.
4. Minimal SDL3 host that draws it and responds to clicks.

---

# Round 9 — The interpreter found

## Type-14 record census (`JUNGOPTS.BIN`, 119 resources)

Treating each type-14 resource as a `u16 opcode, u16 length` stream: 47 of 119 parse exactly to the
end, 72 fail. The dominant record types are stable and small-valued:

| Record | Count | Lengths seen |
|---:|---:|---|
| 76 | 107 | 12, 16, 13, 20 |
| 77 | 62 | 18, 53, 26, 22 |
| 89 | 10 | 8, 9, 12 |
| 79 | 8 | 43, 42, 64, 53 |

## Why the generic walk cannot work

Two resources contradict each other under a fixed field order:

```
resource 0:  4c 00  0c 00  ...   records repeat every 12 bytes -> u16[1] is the length
resource 1:  0c 00  6b 00  ...   records repeat every 12 bytes -> u16[0] is the length
```

Both cannot be right, and they are not. `FUN_1008_c724` settles it — it switches on a `u16` opcode
and **each case computes its own advance**:

```c
switch (*param_2) {
case 1:
    iVar10  = FUN_1008_c63e(param_2, ...);
    local_8 = (*(byte *)((int)param_2 + 5) + 3) * 2;   // length derived, not read
    break;
```

**Record length is a property of the opcode, not a stored field.** Some opcodes carry an explicit
length, others are fixed-size, others compute it from a payload byte. No generic TLV walk can parse
this stream; the opcode table has to be transcribed.

That also explains the 47/72 split: the resources that "parsed cleanly" did so by luck, because
their particular opcodes happen to hold a length in the second `u16`.

## Two dispatchers

| Function | Size | Opcode range |
|---|---:|---|
| `FUN_1008_c724` | 1,976 B | 1–0x61 dense, plus 0x6A, 0x77 |
| `FUN_1008_0c52` | 3,442 B | 0x1B–0x8C, 99, 100 |

**Every record type found in the census — 76, 77, 79, 89, 12, 31, 16 — is a case in
`FUN_1008_c724`.** That is the scene-script interpreter.

`FUN_1008_0c52` covers a higher opcode space and picks up the 107/109 records seen in resource 1, so
type-14 resources are consumed by two different handlers. Some are scene script, some are something
else — most likely event or message handlers.

## What is now bounded

The remaining format work is no longer open-ended. It is: transcribe ~90 cases from
`FUN_1008_c724` and ~50 from `FUN_1008_0c52`, both already decompiled, totalling 5,418 bytes of
code. Each case gives an opcode's operand layout and its length rule at the same time.

## Next actions

1. Transcribe `FUN_1008_c724`'s cases into an opcode table: mnemonic, operand layout, length rule.
2. Start with the four opcodes that carry the scene: 76, 77, 79, 89.
3. Disassemble `JUNGOPTS.BIN`'s type-14 resources with that table.
4. Identify which opcode places a bitmap and which defines a hit rectangle — enough to draw one
   screen and respond to clicks.

---

# Round 10 — The scene script is a stack VM

## Opcode 76 is a wrapper, not a command

```c
case 0x4c:                                  // 76
    FUN_1008_1bf2((u16 *)param_2 + 2, seg); // payload starts at +4
    local_8 = ((u16 *)param_2)[1];          // length IS stored in u16[1]
    break;
case 0x59:                                  // 89 — same, result kept in a global
    DAT_1020_40ae = FUN_1008_1bf2((u16 *)param_2 + 2, seg);
    break;
case 0x4d:                                  // 77 — length computed by a helper
    local_8 = FUN_1008_931a(param_2, seg);
    break;
```

Opcode 76 does store its length in `u16[1]`, which is why 45 of its records measured 12 bytes and
matched. Opcode 77 does not, which is why the generic walk died on it.

## `FUN_1008_1bf2` is a bytecode interpreter

Not a scene-command list — a **stack machine**:

- Opcodes are **1 byte**, read as `(char)*param_1`.
- `DAT_1020_40ac` is the stack pointer.
- Two parallel stacks: values at `0x70E`, addresses at `0x70C`.
- Variables live in a table based at `0x151E`, with a second region above index `0x13FE` reached
  through `DAT_1020_10AC` — a split variable space.
- The stream terminates on opcode `0`, returning the top of the value stack.

That is why opcode 76 has a stored length and its contents still would not parse as records: the
payload is **expression bytecode**, not a record list.

## Instruction set — 30 opcodes recovered

| Opcode | Operand | Shape |
|---:|---|---|
| 1 | u16 | push immediate onto value stack |
| 2 | u16 | resolve variable index to address, push |
| 3 | — | pop, dereference through address stack |
| 4 | u16 | resolve variable index, push address |
| 6 | — | pop one, unary operation |
| 14, 15, 16 | — | store through the address stack — assignment forms |
| 17, 18, 19 | — | pop one |
| 20–29 | — | binary operators on the top two slots (`sp-1`, `sp`) |
| 30, 31, 32 | — | pop one |
| 33, 35, 36 | — | indirect through the address stack — call or pointer deref |
| 37, 38 | u16 | conditional branch, u16 target |

The exact arithmetic or comparison behind each of 20–29 is not yet assigned; only the stack shape is
confirmed. Opcodes 37 and 38 both test `addr_stack[sp] != 0` and take a u16 target, so they are
branch-if-false and branch-if-true.

## Why this is good news for the port

A 30-opcode stack VM with a flat variable table is **far easier to reimplement than a bespoke
command interpreter**. It is a contained, testable component: feed it bytecode, compare the value
stack against the original's behaviour. It also explains the engine's generality — this is the
machinery that let 7th Level ship several different games on the same runtime.

The port needs this VM, the resource layer (done), the blitter, and the outer dispatcher
`FUN_1008_c724`. That is a clear four-part architecture.

## Next actions

1. Assign the arithmetic and comparison semantics for opcodes 20–29 and 14–19.
2. Transcribe the remaining `FUN_1008_c724` cases — the outer scene commands, which is where bitmap
   placement and hit rectangles will be.
3. Write a bytecode disassembler in `tools/` and dump `JUNGOPTS.BIN`'s scripts.
4. Then the SDL3 host.

---

# Round 11 — Scene-script disassembler

## Opcode length table, generated not hand-copied

`tools/opcode_table.py` parses `FUN_1008_c724` out of the decompiled source and extracts each
case's length rule. Cases reach the assignment three ways — directly, via a shared `goto` label, or
from a helper — and only the first two give a static length.

**80 of 94 opcodes have a static length.** Generating the table from source rather than transcribing
it by hand means re-running it after any re-analysis reflects what the code actually says.

## Control flow: the script is a graph, not a list

Three opcodes turned out to be control flow, which is why a linear walk could never finish:

| Opcode | Helper | Behaviour |
|---:|---|---|
| 77 | `FUN_1008_931a` | conditional branch — evaluate expression at `+6`, advance `u16[1]` if false, `u16[2]` if true |
| 79 | `FUN_1008_a1be` | identical shape to 77 |
| 78 | `FUN_1008_a098` | **switch** — case count at `+8`, table at `p + u16[+4]`, default advance at `+6`, selector expression at `+10`; entries are 6 bytes with match value at `+2` and advance at `+4` |

Opcode 77 alone accounts for 96 records in the options screen. Since its two arms advance by
different amounts, the bytes after it belong to two different paths — a linear disassembler
desynchronises immediately and every byte after is garbage.

`tools/script_dis.py` therefore walks the script as a **graph**: a worklist seeded at offset 0,
following both arms of every branch and every switch case, recording visited offsets. No guessing
about which arm runs at runtime.

Also resolved: opcode 37 stores its length in `u16[1]` like 76 (the generated table misclassified it
as dynamic); opcode 1's length is `(byte[5] + 3) * 2`; opcode 89 is `4 + expression length`.

## Expression lengths

The stack-VM opcodes that carry a `u16` operand are **1, 2, 4, 37, 38** — three bytes each, the rest
one byte, terminating on opcode `0`. That makes expression length computable without evaluating
anything, which is what lets opcodes 77/78/79/89 be sized statically.

## Coverage

| Container | Type-14 resources | Fully disassembled |
|---|---:|---:|
| `JUNGOPTS.BIN` | 119 | **94 (79%)** |
| `JUNGMAIN.BIN` | 250 | **223 (89%)** |

Remaining blockers are few and named: opcodes 1, 17, 37, 43, 44, 59 in a handful of places. A few
impossible opcode values (1280, 7183, 9472) show the walk still lands mid-record somewhere, which is
the expected symptom of one length rule still being wrong.

## Next actions

1. Resolve the last blockers — 43 (`FUN_1008_9fee`), 44, 59, 17.
2. Identify which outer opcode places a bitmap and which defines a hit rectangle. Candidates are
   visible in the census: opcode 73 calls `SETCURSORPOS`, 13 and 21 call `POSTMESSAGE`, 53 calls into
   `JUNGA01` (audio), 119 calls `LSTRCPY`.
3. Assign the stack-VM arithmetic semantics for opcodes 20–29.
4. Then the SDL3 host for one screen.

---

# Round 12 — The rendering architecture

## A failed approach, recorded

Hypothesis: the opcode that places a bitmap would carry a resource index as an operand, so
correlating every opcode's `u16` operands against the set of known type-1 resource indices should
make it stand out.

It did not. The best hit rate was 14%, against a 44% base rate (123 bitmaps out of 279 resources) —
**below chance**. Those operands are not resource indices at all. Resources are referenced
indirectly, most likely through the stack VM's variable table rather than as literals in the script.

Recording the negative result because the technique was reasonable and the answer is useful: it
rules out literal resource references in the outer script.

## The renderer has exactly one presentation path

`JUNGS01.DLL` is the only DLL importing GDI, and across the whole DLL there are just:

- 2 calls to `STRETCHDIBITS`
- 2 calls to `SETDIBITSTODEVICE`
- 2 calls to `CREATEPALETTE`, 3 to `REALIZEPALETTE`

All of the blitting is in one function, `FUN_1000_3894`:

```c
if (flagA == 0 && flagB == 0)
    SETDIBITSTODEVICE(...)      // unscaled, 1:1
else
    STRETCHDIBITS(..., 0xCC /* SRCCOPY */, ..., scale * w, scale * h, ...)
```

**The engine composites everything in software into a single offscreen 8-bit DIB, then presents it
once per frame.** Nothing is drawn through GDI per sprite.

## Why this matters for the port

This is close to ideal. The port does not need to reimplement a GDI drawing model — it needs:

1. One 8-bit indexed surface the size of the playfield.
2. A software sprite compositor writing into it (the engine's own, from `JUNGS01`).
3. One present per frame: palette-expand the surface to RGBA and upload as a single SDL texture.

Step 3 is where resolution independence comes for free — the original already chooses between a 1:1
blit and a scaled one, so scaling is a concept the engine understands rather than something the port
has to bolt on.

The compositor candidates in `JUNGS01` are `FUN_1000_2848` (2,033 bytes) and `FUN_1000_45b2`
(1,330 bytes, 10 callers — the heavily-used one, so most likely the per-sprite blit).

## Next actions

1. Read `FUN_1000_45b2` — the likely sprite blitter — for the clipping and transparency rules.
2. Find how the script names a resource: trace the stack VM's variable table to a resource handle.
3. Then the SDL3 host: one indexed surface, palette expand, one texture upload.

---

# Round 13 — Toolchain fingerprinting for matching decompilation

The project target is a **byte-accurate matching decompilation** (see `docs/PROJECT_NOTE.md`), which
means the original 1995 compiler must be identified and obtained. Modern compilers cannot reproduce
period register allocation and instruction selection.

## Fingerprint 1 — linker version, from the NE header

Bytes at NE header `+0x02`/`+0x03` carry the linker version that produced the file:

| File | Linker | DOS stub |
|---|---|---:|
| `JUNGLE.EXE` | **5.50** | 960 B |
| `JUNGU01.DLL` | **5.50** | 960 B |
| `JUNGR01.DLL` | **5.50** | 960 B |
| `JUNGS01.DLL` | **5.50** | 960 B |
| `JUNGA01.DLL` | **5.50** | 960 B |
| `UNINSTAL.EXE` | 5.60 | 960 B |
| `SETUP.EXE` | 5.60 | 960 B |

**All five engine binaries were built with the same linker, version 5.50.** The installer and
uninstaller use 5.60 — a different build, almost certainly a third-party installer product, and not
part of the decompilation target.

That the whole engine is one consistent toolchain is the useful result: one compiler to identify,
not five.

## Fingerprint 2 — CRT startup code

`JUNGLE.EXE` entry point (segment 4:001a):

```
33 ed          xor  bp, bp
55             push bp
9a ff ff 00 00 call far <unrelocated>
0b c0          or   ax, ax
74 ec          jz   ...
```

`JUNGR01.DLL` entry (segment 1:1504):

```
8c d8 90 45 55 8b ec 1e 8e d8 57 1e 51 06 56 e3 0e ...
```

The `xor bp,bp / push bp / call far` prologue and the `9A FF FF 00 00` unrelocated far-call pattern
are the standard Microsoft C Win16 startup shape. Combined with the `WIN87EM` import (Microsoft's
x87 emulator) and MSVC-style C++ mangling in `JUNGR01` (`?AUDIO_READ@@ZCJPAUTAGRESAUDIOINFO@@PIEJ@Z`),
the compiler is Microsoft C family with high confidence.

## What is NOT yet established

**Which Microsoft product shipped LINK 5.50.** Candidates are Microsoft C/C++ 7.0, Visual C++ 1.0
and Visual C++ 1.5 (all 16-bit, 1992–1994). Asserting a mapping from memory would be a guess, and
guessing here is expensive — the wrong compiler means nothing ever matches and the failure looks
like bad C rather than a bad toolchain.

**The test is empirical, not bibliographic:**

1. Obtain each candidate toolchain.
2. Build a trivial Win16 DLL with each.
3. Compare the NE linker-version stamp and the CRT startup bytes against the table above.

The linker stamp alone discriminates between candidates, and the CRT startup bytes confirm the
compiler independently. This is a cheap, decisive experiment once the toolchains are in hand.

## Next actions

1. Acquire candidate 16-bit Microsoft toolchains and run the discrimination test above.
2. Stand up DOSBox with the winning toolchain and confirm it builds a Win16 DLL end to end.
3. Build the diff harness: compile one `.c`, extract the produced segment bytes, diff against the
   original NE segment, report bytes matched.
4. First matching target: `JUNGR01.DLL` — smallest at 10,942 bytes, 123 functions, and the only
   module whose symbol names survived.

---

# Round 14 — The matching harness

Everything except the compiler itself is toolchain-independent, so the harness was built now rather
than waiting on acquisition. It runs on macOS today.

## `tools/ne_extract.py` — reference bytes

Splits each NE binary into per-segment `.bin` files under `reference/`, with the relocation table
for each written alongside as JSON. **17 segments extracted** across the five engine modules.

## `tools/matchdiff.py` — the comparison

The subtlety is relocations. A freshly linked object holds different fixup values at the same byte
positions as the original, so those bytes read as mismatches even when the code is identical, and
must be masked.

NE has two relocation forms, and handling only the first is wrong:

- **Additive** (`flags & 4`) — the listed offset is the single fixup site.
- **Chained** (the default) — the listed offset is the **head of a linked list**. The `u16` stored at
  each site is the offset of the next site, terminated by `0xFFFF`. One record can cover many byte
  positions across the segment.

Measured on `JUNGR01.DLL` segment 1: **31 relocation records expand to 702 masked bytes.** Masking
only the record offsets would have masked about 62, leaving ~640 bytes — 6% of the segment — counted
as permanent false mismatches. A decomp would have looked stuck at 94% with no bug to find.

## `tools/progress.py` — the metric

Percent of comparable bytes matched, per segment and overall. Segments with no build output count as
**0% rather than being skipped**, so the total reflects the whole project rather than only the parts
already attempted.

## The number to drive up

```
TOTAL  0 of 190,084 comparable bytes matched  ->  0.000%
```

190,084 is the real size of the task: all five modules, code and data segments, minus bytes masked
as link-time fixups. (Code alone is 157,721 bytes; the remainder is initialised data.)

A differ that always reports success is worthless, so `matchdiff.py --selftest` proves it
discriminates: identical input gives 100%, a single flipped byte is caught and located, and a byte
inside a masked relocation is correctly ignored.

## Also

`tools/disasm.py` wraps radare2 for 16-bit disassembly. Apple's LLVM `objdump` cannot disassemble
raw binary as i8086 — it rejects `-b binary -m i8086` — so radare2 (`-a x86 -b 16`) does the job,
verified against the known CRT prologue.

## Status

The harness is complete and tested. The only missing piece is the compiler: populate
`build/<MODULE>/seg<N>.bin` and the number moves. Everything downstream — per-function targeting,
side-by-side disassembly, progress tracking — is ready.

---

# Round 15 — Per-function targeting

## `tools/funcs.py`

Ghidra reports addresses as `selector:offset`; for these modules segment *n* is selector
`0x1000 + 8*(n-1)`, and selectors past the segment count address import thunks rather than code in
this binary. Every function is validated to lie wholly inside its segment.

`JUNGR01.DLL`: **94 functions in-segment, 10,750 bytes of code**, 29 import thunks excluded.

The tool emits a work order — **leaf functions first** (they call nothing, so each can be matched
without any other function existing yet), smallest first. `JUNGR01` has **25 leaves**, the smallest
19 bytes.

That ordering matters: starting on a large function that calls ten others means nothing matches
until all eleven are right, and there is no way to tell which one is wrong.

## `tools/funcdis.py`

Prints one function's disassembly with its relocated byte positions flagged — those are the bytes a
fresh link legitimately fills differently, so they must not be reproduced literally.

`RESCOUNTSTRINGS`, 23 bytes, entirely readable:

```
mov ax, 0x15b9        ; relocated — DGROUP selector
push bp / mov bp,sp
push ds / mov ds,ax   ; __loadds
mov bx, [bp+6]        ; the one argument
mov ax, [bx+0x110]    ; return a field of the file handle
pop ds / mov sp,bp / pop bp
retf 2                ; __far __pascal, 2 bytes of arguments
```

The calling convention is legible straight off the metal: `__loadds __far __pascal`, one 16-bit
argument. That is exactly the kind of detail matching decompilation needs and guesswork gets wrong.

## Header fields recovered from accessors

Reading the tiny accessor functions is a cheap way to name header fields exactly:

| Accessor | Handle field | Header offset |
|---|---|---|
| `RESCOUNTSTRINGS` | `+0x110` | `0xA8` |
| `RESCOUNTVARIABLES` | `+0x112` | `0xAA` |
| `RESCOUNTRESOURCES` | `+0x192` (minus `+0x10E`) | runtime totals |

`RESCOUNTVARIABLES` confirms the stack VM's variable table is a first-class part of the format —
`JUNGBUGD.BIN` declares **310 variables**.

## A correction: table 5 is not simply "the string table"

Round 7 called table 5 the string table on the strength of its readable contents. The accessors say
that was too simple. `RESCOUNTSTRINGS` returns 15 for `JUNGBUGD.BIN`, but table 5 contains 75
null-terminated entries. Tested across all 13 containers: **0 of 13 match.**

So table 5 holds more than strings — most likely variable names alongside string values, consistent
with `RESLOADNAMETABLE` being a separate call from `RESCOUNTSTRINGS`. The readable contents were
real; the interpretation was not. Recorded rather than quietly dropped.

## Status

The harness is now complete end to end and needs no compiler to be useful:

- reference bytes extracted per segment, with relocation tables
- relocation-aware differ, self-tested
- per-function targeting with a dependency-aware work order
- per-function disassembly with relocation sites flagged
- progress metric over 190,084 comparable bytes

First matching target: `JUNGR01.DLL`, leaf functions, smallest first.

---

# Round 16 — The RESFILE handle, from the accessors

Sweeping all 42 small named functions in `JUNGR01.DLL` for handle-field accesses names fields
exactly, with no guesswork. 25 accessors yielded offsets.

## Field map

| Accessor | Handle offset | Meaning |
|---|---|---|
| `RESGETMAXFADECOLORS` | `0x10A` | max fade colours |
| `RESGETMAXTRANSCOLORS` | `0x10C` | max transparent colours |
| `RESCOUNTRESOURCES` | `0x192` − `0x10E` | resource totals |
| `RESCOUNTSTRINGS` | `0x110` | string count |
| `RESCOUNTVARIABLES` | `0x112` | variable count |
| `RESGETBUILDTYPE` | `0x114` | build type |
| `RESSETLIBRARY` | `0x118` | library flag |
| `RESGETNEXTREC` | `0x116`, `0x1A6`–`0x1AC` | record iterator state |
| `RESSETCALLBACK` | `0x186`–`0x18C` | callback far pointer pair |
| `RESREADDATA` | `0x18E` | streaming read cursor |

`RESGETMAXTRANSCOLORS` settles an open question from round 4: the format **does** carry
transparency, as a count of transparent palette entries rather than a single key index.

## Table slots identified by their loaders

Each table parser stores its far pointer at a distinct handle offset, and the accessors that read
those same offsets name the table:

| Slot | Parser | Pointer | Named by | Purpose |
|---:|---|---|---|---|
| 0 | `FUN_1000_050c` | `0x08` | — | resource directory |
| 1 | `FUN_1000_04b8` | `0x64` | `RESENUMENTRIES` | entries |
| 2 | `FUN_1000_075a` | `0x54` | — | (empty in all 13 containers) |
| 3 | `FUN_1000_061a` | `0x58` | `RESENUMVARIABLES` | **variables** |
| 4 | `FUN_1000_0720` | `0x60` | `RESGETCONSTSTR` | **constant strings** |
| 5 | `FUN_1000_066a` | `0x14` | — | resident blob |

So the table this project has been calling "table 5" is the **constant string** table, read by
`RESGETCONSTSTR` — the readable contents were right, the name now comes from the code rather than
from inspection.

## The runtime contains the format writer

Previously unseen symbols: **`RESWRITEHEADER`, `RESWRITEPALETTE`, `RESWRITERECDATA`,
`RESWRITESCRIPTBUF`**, alongside `RESSETSTRINGCOUNT`, `RESSETVARIABLECOUNT`, `RESSETBUILDTYPE`,
`RESSETLIBRARY`.

`JUNGR01.DLL` can **write** containers, not just read them. The shipped runtime includes 7th Level's
authoring path.

This is unusually valuable:

- The writer documents the format from the producing side, which is normally the missing half.
- `RESWRITEHEADER` accesses `0x68` — the header copy — so it serialises the same structure the
  reader parses, giving an independent check on every field.
- It makes **round-trip testing** possible: unpack a container and repack it, then compare bytes.
  That is the strongest available proof that the format is fully understood, and it does not need
  the game to run.

## Next actions

1. Read `RESWRITEHEADER` and `RESWRITESCRIPTBUF` — the writer will name remaining header fields.
2. Use the writer's field order to finish the header map.
3. Build the round-trip test once the writer is transcribed.

## The writer confirms the reader's model

Reading the write path is a free check on everything inferred from the read path. Three
confirmations, each independent of how the format was originally worked out:

**Header size.** `RESWRITEHEADER` copies `0x8F` words — **286 bytes** — into `handle+0x68`. That is
exactly the `headerSize` value found in round 2 by parsing `RESOPENFILE`, arrived at from the
opposite direction.

**The resident blob model.** `RESWRITESCRIPTBUF` stores a segment count at `handle+0x108`, then
copies that many far-pointer pairs to `handle+0xC8`, and finally calls `RESWRITEKEYRESOURCE(...,
slot 5, ...)`. Round 8 deduced all three of those — count at `0x108`, cumulative array at `0xC8`,
resident blob in slot 5 — by reading `FUN_1000_066a` and testing against the data. The writer agrees
on every one.

**Records follow the header.** `RESGETNEXTREC` computes a record position by adding the constant
`0x11E` = 286 = the header size, so records begin immediately after the header.

The name `RESWRITESCRIPTBUF` is itself informative: the resident blob is, in 7th Level's own
terminology, the **script buffer**. That matches round 8's finding that type-14 scene scripts live
inside it.

## Status at end of session

The format side is now largely self-confirming: reader and writer independently agree on the header
size, the resident-blob layout, and the table slots. The matching harness is complete and tested,
and per-function targeting is in place with a dependency-ordered work list.

The one blocker remains the period toolchain, which is an acquisition step rather than a technical
one.

---

# Round 17 — Round-trip verification

`tools/roundtrip.py` rebuilds each parsed structure from its parsed representation and compares
against the original bytes. Anything misread, skipped, or silently padded shows up immediately.

Tested per container: the resource directory (10-byte entries), the table descriptor array at
header `0xBE`, and the palette (4-byte records).

```
39/39 structure round-trips byte-identical
```

This matters because every earlier claim about these structures rested on the parser agreeing with
itself. Re-serialisation is an independent check: a parser that skipped a field or mis-sized an
entry would still "work" on read and would fail here.

The test deliberately does not claim to rebuild whole containers — the resident blob and compressed
payloads are passed through rather than regenerated. What it proves is that the structures modelled
are modelled exactly.

Combined with the writer-side confirmations, the container format is now verified three independent
ways: read path, write path, and re-serialisation.

---

# Round 18 — The audio codec is ADPCM

Round 6 established that type-7 payloads are not PCM and that the codec lives in `JUNGA01.DLL`.
Located now.

## Path

`RESCREATEWAVEEVENT` hands `AUDIO_READ` to `A_011` (JUNGA01 ordinal export) -> `FUN_1000_2696`
(stream setup) -> `FUN_1000_3f10`, a 677-byte leaf called from exactly one place: the inner decode
loop.

## It is 4-bit ADPCM

The arithmetic is unmistakable:

```c
uVar2 = step_table[(nibble & 7) + state->index];      // magnitude -> step size
if (nibble & 8) { ... }                               // sign bit
index += index_table[(nibble & 0xf) * 2];             // adapt the step index
nibble = byte >> 4;                                   // second sample in the same byte
```

Two 4-bit samples per byte, magnitude in bits 0–2, sign in bit 3, a step-size table indexed by
magnitude plus a running index, and an index-adjustment table. That is the IMA/DVI ADPCM family.

This explains every observation from round 6: the payload is packed nibble codes, which is why the
amplitude histogram was flat and consecutive bytes uncorrelated, and why the `WAVEFORMATEX` declares
16-bit PCM — it describes the **decoded output**. 4-bit in, 16-bit out is a 4:1 ratio.

## The tables are IMA-derived but not standard

| Table | Address | First entries |
|---|---|---|
| index adjustment | `0x770` | `-8, -8, -8, -8, 16, 32, 48, 64` (repeating for the sign half) |
| step | `0x1E0` | `0, 1, 3, 4, 7, 8, 10, 11, …` |

The index table is exactly the standard IMA index table `[-1,-1,-1,-1,2,4,6,8]` **multiplied by 8**,
so the adaptation index is held in 1/8 fixed-point rather than whole steps. The sign half of the
table repeats the magnitude half, confirming bit 3 is not used for adaptation.

The step table is **not** the standard 89-entry IMA table — it is a smaller, differently-scaled
variant indexed by `(magnitude + index)`.

So a stock IMA decoder will not reproduce this correctly. The tables must be lifted from the
binary's data segment, which is straightforward: both live in `JUNGA01.DLL` segment 2, already
extracted to `reference/JUNGA01_DLL/seg2.bin`.

## Next actions

1. Transcribe `FUN_1000_3f10` fully, including the exact clamping and saturation behaviour.
2. Lift both tables from `seg2.bin` at their known offsets.
3. Decode one clip and check the statistics that failed in round 6 — correlated samples, peaked
   amplitude histogram, near-zero DC offset.

---

# Round 19 — ADPCM decoded. All 628 clips extracted.

`FUN_1000_3f10` transcribed into `tools/adpcm.py`.

## The algorithm

Four-bit ADPCM, two samples per byte, low nibble first:

```
delta = step[(nibble & 7) + index]
if nibble & 8: delta = -delta
pred  = clamp(pred + delta, -32768, 32767)
index = clamp(index + index_adj[nibble], 0, 704)
```

## Why a stock IMA decoder fails

The index is kept at **8x resolution**:

- `index_adj` is IMA's `[-1,-1,-1,-1,2,4,6,8]` multiplied by 8.
- The index clamps to `0x2C0` = 704 = **88 * 8**, IMA's maximum index times eight.
- The step table is correspondingly interpolated: **712 entries**, not IMA's 89.

The three magnitude bits then select a step *relative to* the current index — `step[(nibble & 7) +
index]` — so they walk up to seven entries along the interpolated curve from wherever the index sits.
Reconstruction is a plain saturating add, with none of standard IMA's `step/2 + step/4 + ...` series.

Both tables are lifted from `JUNGA01.DLL` segment 2 at runtime rather than hardcoded, so the decoder
uses exactly what shipped.

## Verification

The round-6 statistics that proved the payload was not PCM now all pass:

| Check | Raw payload | Decoded | Real PCM |
|---|---|---|---|
| mean \|delta\| / RMS | 1.03 | **0.045–0.565**, median 0.262 | 0.2–0.5 |
| DC offset | −5640 | **max \|DC\| 393** | ~0 |
| amplitude histogram | flat | **53411, 6310, 2158, 800, 303, 69, 17, 0…** | peaked at zero |

Sampling 60 of the 628 decoded clips: **0 failures**. Durations run 0.14s to 90.9s, the long ones
being music rather than effects. Pure noise would score ~1.13 on the first metric; nothing comes
close.

## Result

**All 628 audio resources decode to playable WAV.** With the bitmaps already at 11,506 of 11,506,
every substantial asset class on the disc is now extracted.

| Asset class | Count | State |
|---|---:|---|
| Bitmaps (type 1) | 11,506 | **100%** |
| Audio (type 7) | 628 | **100%** |
| Scene scripts (type 14) | 3,323 | disassembler at 79–89% |

---

# Round 20 — Disassembler coverage

Three more opcodes resolved by reading their handlers rather than trusting the generated table:

| Opcode | Rule | Source |
|---:|---|---|
| 59, 60 | length 10 | shared handler falling through to `local_8 = 10` |
| 17 | length 14 | `FUN_1008_919c` writes `0xE` through an out-parameter |
| 43 | data-dependent | `FUN_1008_9fee` walks a match table, like opcode 78 |

Opcode 17 is a reminder that the generated table cannot catch everything: the length is written
through a pointer argument, so no `local_8 = <constant>` appears in the case at all.

## Coverage across all 13 containers

```
JUNGPRTY  151/155  97%     JUNGMAIN  226/250  90%     JUNGSCOR  134/172  78%
JUNGINT1   63/67   94%     JUNGCRED   46/50   92%     JUNGBURP  266/369  72%
JUNGINT2   53/57   93%     JUNGOPTS   96/119  81%     JUNGSHOT  208/312  67%
JUNGLE     48/52   92%     JUNGHIPP  728/936  78%     JUNGPINB  280/460  61%
                                                      JUNGBUGD  189/324  58%

TOTAL 2488/3323 type-14 resources fully disassembled -> 74.9%
```

The pattern is informative: menu and cutscene containers sit at 90–97%, while the minigame
containers (`JUNGPINB` 61%, `JUNGBUGD` 58%, `JUNGSHOT` 67%) trail. The gameplay scripts use a wider
slice of the opcode set, which is where the unresolved dynamic-length opcodes concentrate. Those are
exactly the opcodes that matter most for the port.

## Session state

| Asset class | Count | State |
|---|---:|---|
| Bitmaps (type 1) | 11,506 | **100%** |
| Audio (type 7) | 628 | **100%** |
| Scene scripts (type 14) | 3,323 | **74.9%** disassembled |

Container format verified three independent ways (read path, write path, re-serialisation). Matching
harness complete and self-tested over 190,084 comparable bytes. Blocked only on the period toolchain.

## Terminators, and a symptom mistaken for a cause

**Opcodes 0 and 44 are end-of-script markers.** Both advance by zero and invoke no handler; a zero
advance would loop forever, so the dispatcher's caller must stop on them. Treating them as
terminators rather than failures took coverage from 74.9% to **76.7%**.

**Opcode 37 was never the problem.** It appeared as the top blocker with 153 occurrences across the
minigame containers, but its rule is simply `local_8 = u16[1]` — identical to opcode 76, and already
implemented correctly. It was blocking only because the walk had already landed mid-record, so the
"length" it read was garbage. One bad branch seed poisons every byte after it, and the damage
surfaces at whatever opcode happens to be sitting there.

Adding a guard — only follow a branch target if a known opcode sits at it — fixed that class of
error. Per-container it helped (`JUNGPINB` 61→65%, `JUNGHIPP` 78→80%), but **the overall total did
not move**: 76.7% before and after. So bad seeds were real in some scripts and are not what caps the
rest.

The remaining ~23% needs the genuinely data-dependent opcodes transcribed — 43 (`FUN_1008_9fee`, a
match-table dispatch), 1, and 38 (`FUN_1008_927c`). No shortcut available; they have to be read.

```
TOTAL 2549/3323 type-14 resources fully disassembled -> 76.7%
```

## Opcodes 43 and 38 transcribed

**Opcode 43** (`FUN_1008_9fee`) is the same table dispatch as opcode 78 — count at `+8`, table at
`base + i16[+4]`, default advance at `+6`, 6-byte entries with the match value at `+2` and the
advance at `+4`. The only difference is that its selector comes from a **variable lookup** rather
than an evaluated expression. That distinction is irrelevant to static disassembly, since every arm
is followed regardless.

**Opcode 38** (`FUN_1008_927c`) compares two variable lookups using an operator byte at `+8`. The
advance is `u16[+2]` when the comparison is false and `10` when true — another two-armed branch.

Both share the same variable-resolution shape seen throughout the engine:

```c
if (idx < 0x13FE)      var = *(int *)(idx * 2 + 0x151E);
else if (idx < 0x159F) var = *(int *)(DAT_1020_10AC - idx * 2 + 0x27FC);
else                   var = idx + 0x7531;     // immediate, not a variable
```

The third branch is worth noting: indices at or above `0x159F` are not variables at all but
immediate constants offset by `0x7531`. That single rule explains how the scripts encode literals
and variables in the same field.

## Coverage

```
JUNGPRTY  151/155  97%     JUNGMAIN  230/250  92%     JUNGSCOR  138/172  80%
JUNGINT1   63/67   94%     JUNGCRED   46/50   92%     JUNGBURP  275/369  75%
JUNGINT2   53/57   93%     JUNGLE     48/52   92%     JUNGSHOT  214/312  69%
JUNGHIPP  769/936  82%     JUNGOPTS   98/119  82%     JUNGPINB  300/460  65%
                                                      JUNGBUGD  196/324  60%

TOTAL 2581/3323 type-14 resources fully disassembled -> 77.7%
```

Gains are diminishing: three opcodes transcribed this round moved the total by 1 point. Menu and
cutscene containers are now 92–97%, while the two heaviest minigames (`JUNGBUGD` 60%, `JUNGPINB`
65%) still trail — they use opcodes the others never touch, so each remaining opcode benefits fewer
resources than the last. Finishing them is a grind through `FUN_1008_c724`'s remaining handlers
rather than any single insight.

---

# Round 21 — The advance is signed

## Diagnosing before grinding

A census of what actually blocked the remaining 442 incomplete resources showed opcode 37 alone
accounting for **255 of them — 58%**. Everything else was a long tail. Worth understanding rather
than moving on to the next opcode.

## Opcode 37 is a backward jump

Its rule is `local_8 = u16[1]`, which was implemented and looked correct. The failing records:

```
at 51   claimed 65497      bytes: 25 00 d9 ff ...
at 216  claimed 65347      bytes: 25 00 43 ff ...
at 72   claimed 65476      bytes: 25 00 c4 ff ...
```

`0xFFD9` is not a 65,497-byte record. It is **−39**. The advance is **signed**, and a negative
advance is a backward jump — the loop construct. Read as signed, every failing case lands on a small
positive offset:

| at | claimed | signed | target |
|---:|---:|---:|---:|
| 51 | 65497 | −39 | 12 |
| 216 | 65347 | −189 | 27 |
| 72 | 65476 | −60 | 12 |

Those targets are loop heads. Of course the scripts have loops; nothing else in the opcode set
provided one.

Handling it: emit the jump, seed its target, and end the linear path. When the target is already
visited the loop is closed, which is the ordinary case.

## Result

```
JUNGPRTY  151/155  97%     JUNGMAIN  230/250  92%     JUNGSCOR  151/172  88%
JUNGINT1   63/67   94%     JUNGCRED   46/50   92%     JUNGHIPP  810/936  87%
JUNGINT2   53/57   93%     JUNGLE     48/52   92%     JUNGBURP  300/369  81%
JUNGOPTS  106/119  89%     JUNGSHOT  243/312  78%     JUNGPINB  338/460  73%
                                                      JUNGBUGD  231/324  71%

TOTAL 2770/3323 -> 83.4%
```

**77.7% to 83.4% from one sign bit.** The minigame containers gained most — `JUNGBUGD` 60→71%,
`JUNGSHOT` 69→78%, `JUNGSCOR` 80→88% — which fits: gameplay scripts loop, menus mostly do not. That
also explains why the minigames had been trailing since round 11.

## Method note

The previous three rounds transcribed one opcode at a time for about a point each. Censusing what
actually blocked, then diagnosing the single biggest blocker, was worth more than all of them
combined. Grinding felt like progress; measuring first was progress.

## A wrong fix, reverted

After the jump fix, 203 resources remained incomplete. **117 of them failed at an odd offset**, and
the record immediately before was consistently opcode 89 with length 15 or opcode 77 with length 17
— both odd. Since every statically-known length in `FUN_1008_c724` is even, the inference was that
records are word-aligned and the expression-derived lengths should round up.

Rounding computed lengths up to even **dropped coverage from 83.4% to 76.3%**, so it was reverted.

The odd lengths are therefore real: expression bytecode genuinely produces odd-length records, and
records are not word-aligned despite every fixed length happening to be even. The odd-offset failures
are a symptom of something else still unresolved, not of missing alignment.

Worth recording because the evidence for the hypothesis was strong — a clean correlation across 117
cases — and it was still wrong. The measurement is what settled it, not the reasoning.

## Opcode 1: formula confirmed by measurement, not inspection

Opcode 1 became the top remaining blocker at 46 resources. Inspecting the failing records suggested
the length rule might be `byte[5] + 3` rather than `(byte[5] + 3) * 2` — in two of the cases the
unmultiplied value landed exactly on the end of the resource (19+3=22 remaining, 58+3=61 remaining),
which looked convincing.

Testing both:

| Formula | Coverage |
|---|---|
| `byte[5] + 3` | 76.7% |
| `(byte[5] + 3) * 2` | **83.4%** |

The decompiler's reading was right and the two clean-looking cases were coincidence. Opcode 1's
remaining failures are mid-record symptoms like the rest, not a wrong formula.

Second time this round that a well-evidenced hypothesis was wrong and only the measurement caught
it. On this codebase, a plausible pattern across a handful of records is not evidence — running it
across all 3,323 is.

---

# Round 22 — The stack VM's instruction set, completed

Round 10 recovered the VM's shape but left opcodes 14–36 as "binary operators, exact operation not
yet assigned". Reading the cases assigns all of them.

## Complete operator set

| Opcode | Operation | Evidence |
|---:|---|---|
| 14 | negate | `*p = -*p` |
| 15 | logical NOT | `result = (x == 0)` |
| 16 | bitwise NOT | `*p = ~*p` |
| 17 | add | pop, `*p = *p + rhs` |
| 18 | subtract | pop, `*p = *p - rhs` |
| 19 | multiply | `a * b` |
| 20 | divide | `a / b`, guarded by a zero check calling error handler `0x6F` |
| 21 | modulo | `a % b`, same zero guard |
| 22 | equal | true when `a == b` |
| 23 | greater than | branches false when `a <= b` |
| 24 | greater or equal | branches false when `a < b` |
| 25 | less than | branches false when `b <= a` |
| 26 | less or equal | branches false when `b < a` |
| 27 | not equal | yields 0 when `a == b` |
| 28 | logical AND | both operands non-zero |
| 29 | logical OR | either operand non-zero |
| 30 | bitwise AND | `*p &= rhs` |
| 31 | bitwise OR | `*p \|= rhs` |
| 32 | bitwise XOR | `*p ^= rhs` |
| 33 | increment | `*p = *p + 1` |
| 35 | decrement and dereference | `*p = *p - 1` then load through it |
| 36 | decrement | `*p = *p - 1` |

## What this tells us

This is a **complete C-like expression evaluator**: full integer arithmetic, all six relational
operators, both logical and bitwise boolean sets, increment/decrement, and division guarded against
divide-by-zero with a named error handler.

That is not a hand-rolled command language — it is the backend of a **compiled scripting language**.
7th Level shipped a compiler; the `.BIN` files contain its output. It explains the engine's
generality, and it explains why the outer opcode set has real control flow (branches, switches,
backward jumps for loops) rather than a flat display list.

## Consequences for the port

The VM is the most tractable component to reimplement exactly:

- 30-odd operators, all simple integer operations on two 16-bit stacks.
- No floating point anywhere in the evaluator.
- Behaviour is fully testable in isolation — feed it bytecode, compare the resulting stack.
- Divide-by-zero has defined behaviour (error handler `0x6F`) that must be preserved rather than
  left to the host language.

Combined with the resource layer, the software compositor, and the outer dispatcher, this is the
fourth and last of the engine's major subsystems to be identified.

## The draw path reads VM variables — which explains round 12

`FUN_1000_45b2` in `JUNGS01.DLL` (1,330 bytes, 10 callers — the heavily-used draw entry) switches on
a type field and resolves its parameters through **the same variable-table lookup used by the script
VM**:

```c
if (index < 0x159F)  value = *(uint *)(index * 2 + table_base);
else                 value = index + 0x7531;    // immediate
```

Round 12 tested whether any outer opcode carried a bitmap resource index as a literal operand, and
found a 14% hit rate against a 44% base rate — below chance. That negative result now has an
explanation rather than just a shrug: **resource identifiers and coordinates are held in VM
variables, not embedded as literals in the script stream.** The outer opcodes manipulate variables;
the draw layer reads them.

So the data flow is: script bytecode writes the variable table, outer opcodes invoke draw
operations, and the draw layer resolves what to draw by reading those same variables. Nothing about
which bitmap appears where is statically visible in the script — it is computed.

That matters for the port: a static extractor cannot produce a scene layout. The VM has to run.

---

# Round 23 — The VM runs

Round 22 concluded the VM has to run, because resource ids and coordinates are computed into
variables rather than written literally. So it was built: `tools/vm.py`, a reference interpreter for
`FUN_1008_1bf2`.

## A counting error, caught by running it

Synthetic tests passed immediately (`7 + 5`, `20 % 6`, `3 < 9` all correct), but executing real
script bytecode failed on **75.4%** of expressions. The failures were concentrated on five "unknown"
opcodes: 8, 13, 12, 34, 39.

Those are `'\b'`, `'\r'`, `'\f'`, `'"'` and `'\''`. The case-extraction regex in round 22 matched
single-character C literals and silently skipped **every case written as an escape sequence**.

The VM has **35 opcodes, not 30**. Round 22's table was incomplete and is corrected here:

| Opcode | Operation |
|---:|---|
| 8 | store through pointer |
| 12, 13 | external call, **u32 operand** |
| 34 | load through pointer, then increment |
| 39 | load through pointer |

## A third operand width

Opcodes 12 and 13 advance by **5 bytes** — opcode plus a 32-bit operand. Every earlier length
calculation assumed only 1-byte and 3-byte forms, so any expression containing one was mis-measured.

Correcting it moved scene-script coverage **83.4% to 87.9%**, and it also explains the odd-length
records that prompted the wrong word-alignment hypothesis two rounds ago: those lengths were not odd
because of alignment, they were wrong because two opcodes were being counted as 1 byte instead of 5.

## Result

```
expressions executed : 19,840 of 19,845  (100.0%)
mean instructions    : 5.3
results              : 773 distinct values, range -49 to 30000
remaining failures   : 5, all divide- or modulo-by-zero
```

**Every expression in every scene script on the disc executes.** The five failures run with an empty
variable table, so any division by a variable is a division by zero — an artefact of running without
engine state, not a decoding fault. The original defines that case anyway, calling error handler
`0x6F`, which the interpreter reproduces by raising rather than silently returning a host-language
result.

## Where this leaves the port

The script VM is the first engine subsystem that is **complete and executable**. Feed it bytecode and
it produces the same values the original would. It is also the component a native port can verify
most rigorously, since correctness is checkable without any graphics, audio or timing.

Subsystem status:

| Subsystem | State |
|---|---|
| Resource layer | complete, round-trip verified |
| Bitmap + audio codecs | complete, 100% of assets extracted |
| Script VM | **complete and executing** |
| Outer dispatcher | 87.9% disassembled |
| Renderer | architecture mapped, compositor not transcribed |

## The variable space has three regions

Censusing every variable reference in the scene scripts:

```
JUNGOPTS.BIN: 654 refs over 72 distinct indices, 0 immediates, range 1..5127
              most used: 5118, 5119, 5120, 1627, 217, 1017, 5124, 17
JUNGMAIN.BIN: 252 refs over 52 distinct indices, 0 immediates, range 1..5119
              most used: 36, 5118, 35, 29, 5119, 33, 46, 1
```

The resolution rule seen throughout the engine splits the index space at two constants:

| Index range | Resolved as |
|---|---|
| `< 0x13FE` (5118) | script variable, table based at `0x151E` |
| `0x13FE`–`0x159E` | second region, reached via `DAT_1020_10AC` |
| `>= 0x159F` (5535) | **immediate constant**, value `index + 0x7531` |

The three most-referenced indices in `JUNGOPTS` are **5118, 5119, 5120** — precisely at and just
above the `0x13FE` boundary. So the upper region is not more script variables but the engine's own
registers: the scripts hammer the first few because they are where results and engine state live.
`JUNGMAIN` shows the same signature.

Two further observations:

- **No immediates at all.** Not one reference reaches `0x159F`, so scripts never encode literal
  constants through the variable path — literals go through opcode 1's `push immediate` instead.
- **Only 2–4% of variable indices coincide with bitmap resource indices**, which is chance. This
  independently reconfirms rounds 12 and 22: nothing about which resource a scene uses is visible
  statically.

## The compositor blits from compressed source

`FUN_1000_2848` in `JUNGS01.DLL` (2,033 bytes) is the sprite blitter. It takes eight parameters —
destination context, position, width, height, and a source bitmap pointer — and immediately branches
on:

```c
if ((src[9] & 0x80) == 0) { ... }
```

That is the **same flag bit** identified in round 4: byte 9 is the high byte of the bitmap's `u16`
flags field at `0x08`, and `0x80` marks RLE compression.

So the compositor decodes **during** the blit rather than expanding to a temporary buffer first.
That is what the per-row offset table is for: with a row table, the blitter can seek straight to the
first visible scanline of a clipped sprite and decode only the rows it needs.

It also tests `width & 3`, so there is a word/dword-aligned fast path alongside a byte path.

For the port this is a design decision already made by the original, and worth keeping: decoding at
blit time with per-row seek is why the engine can composite hundreds of sprites on a 1995 machine.
Expanding every sprite to a full buffer first would be simpler and materially slower.

## Session close

| Subsystem | State |
|---|---|
| Resource layer | complete, round-trip verified |
| Bitmap + audio codecs | complete, 100% of assets extracted |
| Script VM | complete, all 19,840 expressions execute |
| Outer dispatcher | 87.9% disassembled |
| Renderer | compositor and present path both identified |
| Matching harness | complete, blocked only on the period toolchain |

---

# Round 24 — The metric was wrong

A census of what still blocked the disassembler returned a surprise: **2 resources of 3,323** hit an
unknown opcode. Not 403, which is what "87.9% fully disassembled" implied.

## The bug was in the measurement, not the disassembler

Coverage was tested as `consumed == len(b)`, where `consumed` tracked the end of the last record
decoded in linear order. That test is valid for a linear walk and wrong for a graph walk: whenever a
resource's final record is entered via a jump, the last linear step finishes somewhere in the middle
and the resource is scored as incomplete even though every byte was decoded.

Measuring bytes actually reached instead:

```
JUNGPRTY  155/155  100.0%    JUNGPINB  458/460   99.6%    JUNGHIPP  927/936   99.0%
JUNGINT1   67/67   100.0%    JUNGBURP  367/369   99.5%    JUNGSHOT  308/312   98.7%
JUNGINT2   57/57   100.0%    JUNGSCOR  171/172   99.4%    JUNGLE     51/52    98.1%
JUNGBUGD  323/324   99.7%    JUNGOPTS  118/119   99.2%    JUNGCRED   49/50    98.0%
JUNGMAIN  249/250   99.6%

TOTAL 3300/3323 -> 99.31%
```

## What this says about the preceding rounds

Rounds 20–23 reported 74.9%, 76.7%, 77.7%, 83.4%, 87.9%. Those numbers were all understated, and
several rounds of transcribing one opcode at a time "for about a point each" were partly chasing a
broken measurement rather than a broken parser.

The fixes themselves were real — the signed-advance discovery and the 5-byte operand class corrected
genuine decoding faults, and both moved the honest number too. But the sense that progress was
grinding was manufactured by the metric.

This is the third time this session that a confident number turned out to be measuring the wrong
thing. The pattern is consistent: the disassembler was fine, the thermometer was broken. Worth more
scepticism toward a metric that moves slowly than toward the code it measures.

## Actual remaining work

**Two resources** fail on opcode 10, whose length rule is still unresolved. Everything else on the
disc decodes.

## Opcodes 10 and 11, and the remaining tail

Opcodes 10 and 11 share a single case body falling through to `LAB_1008_c8c0` (`local_8 = 4`). The
generated table missed it because the shared `case 10: case 0xb:` pair leaves the first case with an
empty body. Adding it fixed the last two resources that hit an unknown opcode.

```
TOTAL 3302/3323 -> 99.37%
```

**21 resources remain, and none of them fail.** They decode cleanly but leave bytes unreached —
2,388 bytes in total. Those bytes consistently begin with `25 00`, which is opcode 37: a jump record
that no path from offset 0 arrives at.

The likely explanation is that a type-14 resource can have **more than one entry point** — separate
event handlers within one script — so walking from offset 0 alone cannot reach all of it. That fits
everything else known about the format: the outer dispatcher is driven by events, and
`RESSETCALLBACK` exists to register handlers.

This is a limitation of how the disassembler is seeded, not a gap in the opcode table. Confirming it
means finding where the engine obtains entry offsets, which is a different thread from opcode work.

---

# Round 25 — Scripts have multiple entry points

Round 24 left 21 resources decoding cleanly but with bytes unreachable from offset 0, and suggested
those scripts have more than one entry point. Confirmed structurally.

## How scripts are entered

`FUN_1008_c724`, the dispatcher, has five callers. Two of them are red herrings:

```c
void RT_015(u16 arg) { rec[0] = 0x15; rec[1] = arg; FUN_1008_c724(0, 0, rec, ss); }
void RT_016(void)    { rec[0] = 0x0D;               FUN_1008_c724(0, 0, rec, ss); }
```

These synthesise a **single opcode record on the stack** and execute it — runtime API wrappers, not
script entry points. Opcode 0x15 and 0x0D both reach `POSTMESSAGE`, so they are the engine's way of
raising an event from C code.

The real driver is `FUN_1008_c59e(index, record_ptr, env)`:

```c
if (index < 0x159F)  value = *(int *)(variable_address_for(index));
else                 value = index + 0x7531;
*(int *)0x3C50 = value;          // stash the event argument
switch (*record_ptr) { case 0x12: ... }
```

It resolves a variable using the standard three-region rule, stashes it at a fixed location, and
then dispatches **from a caller-supplied record pointer**.

## What follows

Scripts are entered at arbitrary record offsets chosen by the engine, not only at offset 0. A type-14
resource is therefore a **collection of handlers** sharing one byte range, not a single linear
program — which is exactly why 2,388 bytes across 21 resources are unreachable from a walk seeded
only at zero.

That also explains `RESSETCALLBACK` holding a far pointer pair at handle `0x186`–`0x18C`, and it
fits the engine being message-driven: `POSTMESSAGE` appears throughout the opcode set.

## Consequence for the disassembler

The seeds are the limitation, not the opcode table. Complete coverage requires knowing where entry
offsets come from — a different thread from opcode work, and one that likely runs through the
resident record types (9, 10, 13, 15) rather than the scripts themselves.

An earlier attempt to find those offsets by matching them against `u16` fields in the resident types
was **inconclusive**: the unreachable offsets are small values (16, 17, 26) that occur by chance in
any field, so 147 matches across 3,149 type-10 records is noise. A sharper test needs the rarer
offsets, or the engine code that computes them.

## The callback path is not statically resolvable

Following the entry-point question further hit a wall worth documenting.

`FUN_1008_c59e` — the function that dispatches from a caller-supplied record pointer — has **no
callers inside `JUNGLE.EXE`**. It is `__stdcall16far`, so it is invoked from outside: a callback the
engine registers and something else calls.

`RESSETCALLBACK` looked like the obvious registrar, and it does store a far pointer pair at handle
`0x186`–`0x18C`. But **nothing in `JUNGR01.DLL` reads those fields back**. The indirect calls that do
exist in that DLL go through parameter structures, not the handle.

So the chain is: something registers a callback, something invokes it with a record pointer, and
neither end is visible in the decompiled output of the module that owns the field. Resolving it
needs either the remaining DLLs traced for the read site, or dynamic observation — the engine running
under a debugger with a breakpoint on the dispatcher.

Recorded rather than guessed. The plausible story (`RESSETCALLBACK` registers the script dispatcher)
fits the shape of the evidence and is **not** supported by it, and this session has already produced
three confident inferences that measurement later contradicted.

### RESSETCALLBACK appears to be authoring-side

Searching all five modules for reads of the callback fields at handle `0x186`–`0x18C` returns
nothing. The single apparent hit in `JUNGLE.EXE` is `0x1860`, a memory-size constant in a
`GLOBALCOMPACT` / `GETFREESPACE` routine — a false positive.

So across the whole shipped engine those fields are **written and never read**.

That fits `RESSETCALLBACK` belonging to the same authoring-side API surface as `RESWRITEHEADER`,
`RESWRITEPALETTE`, `RESWRITERECDATA` and `RESWRITESCRIPTBUF`: 7th Level shipped their tool's resource
library inside the runtime, and parts of it are simply unused by the player.

This kills the tidy hypothesis that `RESSETCALLBACK` registers the script dispatcher. The dispatcher
is still entered from outside `JUNGLE.EXE` by some path, but it is not this one, and identifying it
needs dynamic observation rather than more reading.

---

# Round 26 — The VM has memory

Round 23 built an interpreter that decoded every opcode but modelled memory in a simplified way,
and that limit was documented rather than papered over. Now corrected.

## There is one stack, not two

The decompiled source appears to use two stacks, at `0x70C` and `0x70E`. It does not.

Both are indexed by `sp * 2`, so the slot at `0x70E + sp*2` is **the same memory** as
`0x70C + (sp+1)*2`. There is a single stack array, and a `0x70E`-relative reference simply means
"one slot higher". Every operation becomes consistent once that is seen:

```
top of stack      S[sp]
push v            S[sp+1] = v; sp += 1
binary operator   sp -= 1; S[sp] = S[sp] OP S[sp+1]
```

Reading them as two independent stacks is what made the earlier memory model unimplementable.

## A flat address space, not a variable dictionary

Variables are addressed:

```
index <  0x13FE   addr = 0x151E + index * 2
index <  0x159F   addr = HIGH_BASE - index * 2      (descending)
otherwise         an immediate, value index + 0x7531
```

Modelling this as a flat address space rather than a name-keyed dictionary is what makes opcode 3
work: it computes `base + subscript * 2`, genuine array subscripting, which a dictionary cannot
express.

Opcode 4 pushes an **address** and opcode 2 pushes the **value** at it. That difference is what gives
the scripting language lvalues — 4 yields something assignable, 2 yields its value — and opcode 8
stores through the address.

## Verification

```
var[5] = 7 + 3      ->  10, variables {5: 10}
var[5] * 2          ->  20        (read back from stored state)
```

Across the whole disc:

```
expressions executed : 19,841 of 19,846  (99.97%)
resources writing variables : 1,516
script variables written    : 1,560 distinct
engine registers written    : 22 distinct  (5119, 5120, 5121, 5122, 5123, 5243 ...)
unmapped addresses          : 50
```

The five failures are divide- and modulo-by-zero, which are defined behaviour in the original
(handler `0x6F`) and expected when running without engine state.

## An error caught on the way

The first run reported variables at indices like 30064 and 30063 — impossible, since the space tops
out near 5535. The VM was right; the helper mapping addresses back to indices was wrong. The high
region **descends**, so index 5119 lands at address `0xFFFE`, which a naive `(addr - base) / 2`
decodes as 30064.

Inverting both regions separately, and reporting anything that fails to invert as unmapped rather
than forcing it, gives the clean split above. Worth noting that the bogus numbers looked like data —
they were consistent and repeatable — and only failed on a range check.

---

# Round 27 — Readable listings

`tools/listing.py` combines the outer record walk with disassembly of the embedded expression
bytecode, naming both instruction sets. `tools/opnames.py` holds the mnemonics: outer names come
from the API each handler reaches, VM names from reading `FUN_1008_1bf2`. Anything not positively
identified prints as `opN` / `vmN` — an invented mnemonic reads as knowledge and is worse than a
bare number.

The scripts are now legible. A resource opens with plain initialisation:

```
0000  EXPR    len=12        0048  EXPR    len=17
      push.addr  var1             push.addr  var12
      push.imm   0                push.imm   140
      store                       call.12    0
      end                         store
```

And control flow reads as source:

```
0083  BRANCH  len=21   false -> 00b0   true -> 0098
      push.val   var5031
      br.false   9
      push.val   var12
      push.imm   53
      gt
      or.l
```

That is `if (var5031 || var12 > 53)`. The `br.false` is the **short-circuit**: if the first operand
is already true, skip evaluating the second. A hand-written bytecode language would rarely bother;
a compiler emits it as a matter of course.

This is the clearest confirmation yet that the `.BIN` files hold **compiler output**, which was
inferred in round 22 from the completeness of the operator set and is now visible in the codegen
itself.

## Note on a test that could not answer its question

With the VM holding memory, the obvious move was to ask round 12's question again: do computed
values correspond to resource indices? The test is worthless as constructed — the computed values
all fall inside the resource-index range by construction, so the base rate is 100% and a match means
nothing.

Only 22–47 values emerge per container anyway, because expressions run in isolation. Scene state is
driven by the **outer** opcodes, which are not yet executed, so the embedded expressions alone
cannot answer it. Recorded as unanswerable with current tooling rather than reported as a weak
positive.

---

# Round 28 — The FFI, and a correction about the variable space

Reading a legible listing immediately paid off. The options script contains:

```
push.addr var6 ; push.val var5002 ; br.false 9 ; push.val var5003
push.imm 800 ; ge ; or.l ; store        =>  var6 = (var5002 || var5003 >= 800)
```

An 800-pixel width test, matching the native art resolution. And repeatedly:

```
push.imm 140 ; call.12 0
push.imm 100 ; call.13 0
push.imm 101 ; call.13 0
```

Those two call opcodes are the script's interface to the engine.

## Opcode 12 is the engine service call

`FUN_1008_1ad4` takes a count from the operand, reads a service number off the stack, and:

```c
if (service < 0x8D) FUN_1008_0c52(count, service);
else                FUN_1008_1b10(count, service);
```

`0x8D` is 141, and `FUN_1008_0c52`'s cases run `0x1B`–`0x8C` — **exactly** 27 to 140.

Round 9 described `FUN_1008_0c52` as "a second dispatcher covering a higher opcode space, possibly
event handlers". That was wrong. It is the **engine service table**, invoked from inside expression
bytecode, not a record dispatcher at all. The two functions are not peers: `FUN_1008_c724` walks
scene records, `FUN_1008_0c52` implements ~50 callable services.

## Opcode 13 is a script function call

`FUN_1008_1b5e` pops N arguments into a frame based at `DAT_1020_10AC`, then moves that pointer
**downward** by the frame size.

That is stack-frame allocation, which reinterprets the variable space:

| Range | Round 24 called it | Actually |
|---|---|---|
| `< 0x13FE` | script variable | script global |
| `0x13FE`–`0x159E` | "engine registers" | **stack frame: locals and parameters** |
| `>= 0x159F` | immediate | immediate |

The region descends because it is a **downward-growing call stack**, not a fixed register file. That
explains everything odd about it: why the most-referenced indices sit right at the `0x13FE` boundary
(they are the current frame's first slots), why the address arithmetic is `base - index * 2`, and
why those indices are written constantly.

So the scripting language has **functions with local variables and parameters**, called via opcode
13, alongside engine services called via opcode 12. Combined with round 22's complete operator set
and round 27's compiler-emitted short-circuits, there is no remaining doubt that the `.BIN` files
contain the output of a real compiler for a real language.

## Correction log

Two claims from earlier rounds are now superseded:

- `FUN_1008_0c52` is the engine service table, not a second script dispatcher (round 9).
- Variable indices `0x13FE`–`0x159E` are a call-stack frame, not engine registers (round 24, and
  `tools/vm.py`'s `variables()` reports them under the wrong name).

## The engine service table

With `FUN_1008_0c52` correctly identified as the service table, tracing each case to the API it
reaches names 36 of its 51 services:

| Service | Reaches | Category |
|---:|---|---|
| 124 | `COLLIDE` | **collision** |
| 125 | `INTERSECTRECT`, `S_019`, `S_077` | rectangle intersection |
| 126 | `DISTANCE` | distance between points |
| 127 | `REFLECTANGLE` | **angle of reflection** |
| 128 | `SQUAREROOT` | square root |
| 133 | `GETQUADRANT` | quadrant of a vector |
| 49, 65 | `GETANGLE`, `COSINE` | trigonometry |
| 47, 111 | `GETKEYSTATE`, `GETCURSORPOS`, `SCREENTOCLIENT` | input |
| 109 | `TIMEGETTIME` | timing |
| 27, 57, 58, 99, 100, 116, 118 | `WVSPRINTF`, `LSTRCMP`, `LSTRCMPI`, `LSTRLEN`, `LSTRCPY` | strings |
| 112 | `A_025` | audio |
| 68, 90, 91, 104, 108, 114, 115, 120, 131, 132, 134, 138 | `S_0xx` | graphics |

**The engine provides a 2D geometry and physics library to scripts.** `COLLIDE`, `DISTANCE`,
`REFLECTANGLE`, `GETQUADRANT` and `SQUAREROOT` are not presentation helpers — they are what you need
to write a ball bouncing off a surface. `REFLECTANGLE` in particular is bounce physics, and the disc
contains a pinball minigame.

That is a significant result for the port. These services are **named**, they are implemented in
`JUNGLE.EXE` rather than hidden behind an ordinal, and their behaviour has to be reproduced exactly:
a subtly different `REFLECTANGLE` or `DISTANCE` changes gameplay while everything still appears to
work. They are also individually testable, since they are pure functions of their arguments.

Combined with the round-27 and round-28 findings, the architecture is now fully characterised:

```
scene records          FUN_1008_c724   walks type-14 resources
  |- expression bytecode  FUN_1008_1bf2   35-opcode stack VM
       |- opcode 12         FUN_1008_0c52   ~51 engine services
       |- opcode 13         FUN_1008_1b5e   script function call, new frame
```

### Correction: the math library lives in JUNGU01, not JUNGLE.EXE

The geometry services were described above as "implemented in `JUNGLE.EXE` rather than hidden behind
an ordinal". Wrong — every one of them is 4 bytes at segment `1110`, which is the **import thunk
segment**. They are imported, not implemented there.

They all live in `JUNGU01.DLL`, the utility module:

| Function | Size |
|---|---:|
| `COLLIDE` | 444 |
| `GETINTERSECT` | 441 |
| `COSINE` | 94 |
| `DISTANCE` | 83 |
| `GETQUADRANT` | 61 |
| `GETANGLE` | 53 |
| `NORMALIZEANGLE` | 38 |
| `SQUAREROOT` | 28 |
| `REFLECTANGLE` | 28 |
| **total** | **1,270** |

The entire 2D geometry and physics library the scripts rely on is **1,270 bytes**.

That makes it the ideal first target for the matching decompilation, ahead of anything else:

- Pure functions of their arguments, with no engine state to set up.
- Individually testable — feed both implementations the same inputs and compare outputs.
- Already **named**, so their intent is not in question.
- Small: `SQUAREROOT` and `REFLECTANGLE` are 28 bytes each.
- Behaviourally critical. A subtly wrong `REFLECTANGLE` or `DISTANCE` changes how the game plays
  while everything continues to look correct, which is the hardest class of bug to find later.

`JUNGU01` also holds the memory and string utilities (`WINMALLOC`, `COPYHUGEBYTES`, `SEARCHSTR`,
`LINEARSEARCH`), so it is the natural first module overall — more of it is named than any module
except `JUNGR01`.

### The first two targets, read

`REFLECTANGLE` — 28 bytes, and the whole algorithm is visible:

```
mov ax, [bp+6]     ; surface angle
shl ax, 1          ; * 2
sub ax, [bp+8]     ; - incoming angle
push ax
call 0xDEC         ; normalise
retf 4             ; __far __pascal, two 16-bit arguments
```

`reflect(surface, incoming) = normalize(2 * surface - incoming)` — the standard reflection identity,
in integer angle units. Nothing about it is ambiguous, which is exactly what a first matching target
should look like.

`SQUAREROOT` — 28 bytes, and more interesting than expected:

```
wait
fild word [bp+6]   ; integer -> x87 stack
call 0x13D4        ; square root
call 0x1406        ; x87 -> integer
retf 2
```

It uses the **x87 floating-point unit**, which is why `JUNGU01` imports `WIN87EM`, Microsoft's
floating-point emulator: on a machine without a coprocessor these instructions are trapped and
emulated.

That is a real hazard for the port. The x87 computes with 80-bit intermediate precision, and the
emulator had to match it. A port using a C `sqrt()` on 64-bit doubles will occasionally round
differently, and since this feeds collision and reflection maths, a one-unit difference can change
where a ball goes. Reproducing it needs the rounding behaviour checked against the original, not
assumed.

Both functions are `__loadds __far __pascal`, take 16-bit integer arguments, and touch no engine
state.

### A tooling note

`funcs.py` reports one function in `JUNGU01` falling outside its segment (`FUN_1010_0a72`, offset
`0xA72` + 198 bytes against a 2,871-byte segment 3). The bounds check exists precisely to surface
this rather than trust the address blindly; it needs investigating before that function is worked on,
but it does not affect the others.

---

# Round 29 — Scoping the outer opcode work honestly

The viewer built in the native round is an **asset browser and codec harness, not a port**. It never
runs the game. Recording that plainly because it was described at one point as "a native build to
test", which oversold it.

## What a running scene actually needs

Censusing which outer opcodes a scene executes (`--opcensus` on `JUNGOPTS.BIN`, 718 records):

```
76  308  42.9%     12   21  78.0%     4    8  86.8%     44   4  93.3%
77  118  59.3%     18   13  79.8%     1    8  87.9%     83   4  93.9%
37   78  70.2%     78   11  81.3%     2    8  89.0%     60   4  94.4%
89   35  75.1%     79   11  82.9%     45   8  90.1%     16   4  95.0%
                    5   11  84.4%     64   8  91.2%     19   3  95.4%
                   31    9  85.7%     93   6  92.1%
```

**Opcodes 76, 77, 37 and 89 are 75% of all records, and they are expression evaluation and control
flow — already implemented and cross-validated.** The work is the remaining 25%, about 18 opcodes.

## First-layer handler sizes

```
op 83  677 bytes    op 18  186 bytes    op 31   46 bytes
op 12  315          op 16  172          op 45  inline
op  5  251          op 93  167          op 64  inline
op 19  232
```

Roughly 2 KB of decompiled C — which looked encouraging until reading two of them.

## Why 2 KB is the first layer, not the total

`op5` (`FUN_1008_a312`) immediately calls `FUN_1008_669c` and `FUN_1008_6782`. `FUN_1008_669c` has
**85 callers** — it is a core engine primitive, not a leaf. `op31` (`FUN_1008_e902`) sets a global
mode and calls into a notification chain.

So the handlers are entry points into the engine's shared machinery. Transcribing the 18 pulls in
their transitive closure, and the honest estimate is not 2 KB.

## Remaining blockers, ranked

1. **Entry points.** Round 25 established scripts are entered at engine-supplied record pointers,
   and round 28 ruled out `RESSETCALLBACK` as the source. Not statically resolvable from the
   decompiled output — likely needs the engine observed under a debugger.
2. **Outer handlers plus transitive closure.** Bounded but not small.
3. **Compositor** `FUN_1000_2848`, 2 KB, and the three host operations it sits behind.
4. **Message loop, input, timing.**

Item 1 is the one that cannot be resolved by more reading, and it gates a scene running at all.

---

# Round 30 — the resource type dispatch, and the type 15 layout

## The type to accessor map

`FUN_1008_6782` in `JUNGLE.EXE` (1008:6782, 191 bytes) resolves a handle through the documented
variable-address form and then switches on a type code. The same switch shape appears at three
other sites. It gives the per-type accessor directly:

| Type | Accessor |
|---:|---|
| 1 | `FUN_1008_59d2` |
| 4 | `FUN_1008_5cf6` |
| 7 | `FUN_1008_621e` |
| 8 | `FUN_1008_5da4` |
| 9, 12, 13, 14 | `FUN_1008_5bb2` — one shared handler for all four |
| 10 | `FUN_1008_5a66` |
| 11 | `FUN_1008_5c4e` |
| 15 | `FUN_1008_5e9a` |
| 16 | `FUN_1008_6192` |

The same function's truthiness switch also constrains the record widths: types 1, 8, 10 and the
9/12/13/14 group are tested on words 0 and 1, type 15 on word 2, type 16 on word 3. So a type 15
control block is at least three words wide and type 16 at least four.

Note that types 9, 12, 13 and 14 share **one** accessor. Type 14 is the scene script. That places
the other three in the same family as scripts rather than alongside sprites, which is not what the
record sizes suggested.

## `FUN_1008_5bb2` is a lazy loader

The 9/12/13/14 handler is small and does one thing: resolve the handle, check whether the control
block's first two words are still zero, and if so call `FUN_1008_6866` to fault the resource in,
storing the returned far pointer across words 0 and 1 and two more results into words 2 and 3.
Already-resident resources return immediately. So the first two words are a cached far pointer,
not data.

## Type 15 record layout, from its consumer

`FUN_1008_5e9a` (1008:5e9a, 320 bytes) is the type 15 loader. It reads the record at fixed offsets,
which recovers the layout without guessing:

```
+0x02  u16   initial value
+0x04  u16   current value  -- explicitly copied from +0x02 on load
+0x0C  u32   handle, resolved through the variable-address form, passed to S_009
+0x0F  u8    argument to S_058
+0x10  u8    argument to FUN_1008_5fdc
+0x12  u8    flag; when non-zero calls FUN_1008_5e38 and latches DAT_1020_0e24
+0x13  u8    argument to S_058
+0x14  ...   inline payload, passed as a pointer to FUN_1008_5fdc
```

Checked against the 18 type 15 records in `JUNGOPTS.BIN`: every one has `+0x02 == +0x04 == 1`,
consistent with the copy being an initialisation of a live value from a stored one, and every one
carries a distinct sequential id at `+0x14` (`0x8b03`, `0x8b32`, `0x8b3d`, `0x8b3f`, ...).

`S_009`, `S_034`, `S_058` and `S_011` are imported ordinals, all called with the constant `0x1110`.
Identifying which DLL exports them is the next step and is mechanical — the import table is already
dumped.

## Method note

The hex for types 9, 10, 13 and 15 suggested plausible layouts — type 13 looked like 14-byte
records with a trailing argument count, type 15 like fixed 30-byte instances. Reading the consumers
confirmed the type 15 reading and showed the type 13 grouping was wrong: it shares a handler with
the scene scripts. Pattern-matching a hex dump produces hypotheses, not facts. The consumer is the
evidence.

## The imported ordinals resolve to the sprite library — and the names are stripped

`tools/ne_imports.py` parses the NE relocation records properly. The existing dumps in
`reference/*/seg*.relocs.json` keep the address type, relocation type and first target word but
drop the **second** target word, which for an `IMPORTORDINAL` fixup is the ordinal itself. Without
it no imported call can be named.

NE relocations are also **chained**: one record heads a linked list threaded through the segment
body, each site holding the offset of the next and `0xFFFF` ending it. Walking the chains turns
245 relocation records in `JUNGLE.EXE` segment 2 into **549 attributed call sites**.

The four calls in the type 15 loader resolve to:

| Call in `FUN_1008_5e9a` | Site | Import |
|---|---|---|
| `S_009` | seg2:5f50 | `JUNGS01.10` |
| `S_034` | seg2:5f67 | `JUNGS01.35` |
| `S_058` | seg2:5f7c | `JUNGS01.59` |
| `S_011` | seg2:5fcd | `JUNGS01.12` |

All four are **`JUNGS01`, the Sprite Library**. That settles what type 15 is: a sprite instance.
`FUN_1008_5e9a` faults in the record, copies the stored initial value at `+0x02` to the live value
at `+0x04`, resolves the handle at `+0x0C`, creates the sprite, and caches the resulting handle in
word 2 of the control block — which is exactly the word `FUN_1008_6782` tests for type 15.

**The sprite library's symbol names do not survive.** `S_009` is not a placeholder invented by the
disassembler: it is the literal string in `JUNGS01.DLL`'s non-resident name table. 7th Level shipped
that DLL with its exports named `S_001` through `S_082`, and `JUNGA01` and `JUNGU01` the same way.
Only `JUNGR01` kept meaningful names (`RESLOADRESOURCE`, `RESEXPANDBITMAP`, ...), which is why the
container fell so quickly and why the sprite and audio layers have not.

So the import map is now complete, but it buys addresses rather than meanings. Naming the sprite
API requires reading the 83 function bodies in `JUNGS01`. That is the honest size of the next step,
and it is the same shape of work as the compositor, which is one of those 83.

## The linker is identified. The compiler is not.

Matching `RESCOUNTSTRINGS` (23 bytes, the smallest useful accessor) got to seven differing bytes
and then stopped moving, and the seven are informative.

The original:

```
b8 b9 15        mov ax, DGROUP      <- relocated
55              push bp
8b ec           mov bp, sp
1e              push ds
8e d8           mov ds, ax
8b 5e 06        mov bx, [bp+6]
8b 87 10 01     mov ax, [bx+0x110]
1f              pop ds
8b e5           mov sp, bp
5d              pop bp
ca 02 00        retf 2
```

Every build produced here:

```
55 8b ec 1e     push bp / mov bp,sp / push ds
b8 33 00        mov ax, DGROUP
8e d8           mov ds, ax
... identical from here ...
```

**The body matches exactly.** `8b 5e 06 8b 87 10 01 1f 8b e5 5d ca 02 00` is byte-identical, so the
C is right, the calling convention is right (`__far __pascal`, `retf 2`), the parameter really is a
near pointer, and the field offset is right. Only the order of the prologue differs: the original
loads `DGROUP` into `AX` *before* setting up the frame, every build here does it after.

Forty flag combinations were swept across both compilers -- memory models `/AS`, `/ASw`, `/ASu`,
`/ASnw`, `/AM`, `/AL`, `/AC`; code generation `/GD`, `/Gw`, `/GW`, `/GA`, `/GEf`, `/GEd`, `/Gc`,
`/Gx`, `/Gy`, `/Gr`; optimisation `/Ox`, `/O1`, `/O2`, `/Os`, `/Ot`, `/Oa`, `/Og`, `/Oi`, `/Ob1`,
none. **Not one reorders that prologue.** Visual C++ 1.0 and 1.5 emit identical code here, so this
is not a flag that was missed; it is a different code generator.

So the earlier conclusion was half right and is corrected here:

| | |
|---|---|
| **Linker** | **Visual C++ 1.0's LINK 5.50.** Confirmed by building: the emitted NE header stamps 5.50, matching all five engine binaries. MSC 7.0 ships LINK 5.30 and VC++ 1.5 ships 5.60, so neither produced these files. |
| **Compiler** | **Not VC++ 1.0 or 1.5.** Unresolved. |

A mixed toolchain was completely ordinary in 1992-95: LINK 5.50 was redistributed with the Windows
SDK and was commonly paired with an older compiler. The leading candidate is **Microsoft C/C++
7.0**, whose code generator predates both, and whose `__loadds` prologue is documented as loading
`DGROUP` first.

Testing it is blocked on a mechanical problem rather than an analytical one: MSC 7.0's compiler
passes are 32-bit DOS-extended (`C13216.EXE`, `C23216.EXE`) and refuse to run without a DPMI host --

```
run-time error R6901
- DOSX32 : This is a protected-mode application that requires DPMI
```

Bare DOS under DOSBox-X provides none. Windows 3.1 enhanced mode does, and a guest is already
installed, so the compiler can be driven from inside it; a free DPMI host such as CWSDPMI would
also serve.

**Method note.** Seven bytes of prologue is a better result than it looks. A matching decompilation
is mostly the search for the build environment, not the search for the source: the body matched on
the second attempt, and everything since has been about the compiler rather than the C.

## The resource-load path, named

`tools/ne_imports.py` resolves the loader's calls, and because `JUNGR01` is the one module whose
export names survived, they come back meaningful rather than as ordinals.

`FUN_1008_6866` is the fault-in path behind every resource accessor:

| Site | Import | |
|---|---|---|
| seg2:68a9 | `JUNGR01.8` | `RESGETTYPE` |
| seg2:68c7 | `JUNGR01.11` | `RESLOADRESOURCE` |
| seg2:68da | `JUNGA01.37` | (audio, name stripped) |
| seg2:68e9 | `JUNGS01.62` | (sprite, name stripped) |
| seg2:6907 | `JUNGR01.16` | `RESSETCALLBACK` |
| seg2:6937 | `JUNGR01.11` | `RESLOADRESOURCE` |
| seg2:6966 | `JUNGR01.16` | `RESSETCALLBACK` |

The control flow reads directly off this. It calls `RESGETTYPE`, then branches on
`(type < 8) || (type > 0x10)`:

- **Types 8–16** — the resident ones — go straight to `RESLOADRESOURCE`. No callback, no
  notification. They are already in the blob table 5 loaded at open, so nothing can block.
- **Everything else** is streamed from the file, and the engine wraps the load: it installs a
  callback with `RESSETCALLBACK`, pokes the audio and sprite libraries, loads, then restores the
  previous callback and undoes the pokes.

That is exactly the resident/streamed split `docs/FORMAT.md` recorded from the directory format,
now confirmed from the consumer side.

It also supports round 28's conclusion rather than reopening it. `RESSETCALLBACK` does appear on the
load path, but symmetrically — saved, set, restored around a streaming read. That is a progress or
abort hook for slow media, not the source of script entry points.

`FUN_1008_5a66`, the type 10 accessor, allocates a 20-byte control block through `JUNGU01.8`
(`WINMALLOC`) and caches it, so type 10 is an object with its own heap-allocated state rather than a
plain record.

### The full JUNGR01 API

All 58 exports, names intact:

```
RESOPENFILE RESCLOSEFILE RESCREATEFILE RESREAD RESWRITE RESSEEK RESTELL
RESGETHEADER RESWRITEHEADER RESGETTYPE RESGETSIZE RESGETINDEX RESGETTITLE
RESSETTITLE RESCOUNTRESOURCES RESENUMRESOURCES RESSEEKRESOURCE RESLOADRESOURCE
RESCOPYRESOURCE RESDELETERESOURCE RESLOADKEYRESOURCE RESWRITEKEYRESOURCE
RESLOADPALETTE RESWRITEPALETTE RESEXPANDBITMAP RESLOADNAMETABLE RESGETCONSTSTR
RESCOUNTSTRINGS RESSETSTRINGCOUNT RESCOUNTVARIABLES RESSETVARIABLECOUNT
RESENUMVARIABLES RESENUMENTRIES RESGETNEXTREC RESFLUSHREC RESWRITERECDATA
RESWRITESCRIPTBUF RESWRITEINDEXBUFS RESREADDATA RESEXTRACTFILE RESSETCALLBACK
RESSETNOTIFY RESGETBUILDTYPE RESSETBUILDTYPE RESGETMAXFADECOLORS
RESGETMAXTRANSCOLORS RESSETLIBRARY RESGETLIBRARYNAME RESCREATEWAVEEVENT
RESCREATEMIDIEVENT SWINMALLOC SWINREALLOC SWINFREE LIBMAIN WEP
?AUDIO_READ@@... ?AUDIO_REWIND@@... ?AUDIO_DONE@@...
```

Three are C++ mangled, naming a struct `TAGRESAUDIOINFO`, so parts of the library were built as C++
and the audio streaming interface is a small class-like triple of read/rewind/done.

## Naming the Windows calls: 24% -> 74%

The engine calls `KERNEL`, `USER`, `GDI` and `MMSYSTEM` entirely by ordinal, so a raw call map shows
`GDI.443` and stops there. The ordinal-to-name mapping is carried in the period import libraries as
OMF `IMPDEF` records, which makes recovering it authoritative rather than a table typed from memory.

`tools/libw_names.py` extracts them from `LIBW.LIB` and `MMSYSTEM.LIB`: **880 ordinals** across
`KERNEL` (165), `USER` (328), `GDI` (226), `MMSYSTEM` (133), `KEYBOARD` (12) and `SOUND` (16).

One trap: a sequential OMF record walk does not survive an OMF *library*, because modules are padded
to page boundaries and the record stream is not contiguous. Scanning for the record signature and
validating each hit is what works.

Feeding that into `tools/ne_callmap.py` names every imported call site:

| Module | Call sites | Named |
|---|---:|---:|
| `JUNGLE.EXE` | 745 | 555 (74%) |
| `JUNGR01.DLL` | 113 | 111 (98%) |
| `JUNGS01.DLL` | 88 | 88 (100%) |
| `JUNGU01.DLL` | 57 | 55 (96%) |
| `JUNGA01.DLL` | 79 | 79 (100%) |

Maps are in `notes/callmap-*.txt`.

## The present path, named

Round 21 identified `FUN_1000_3894` in `JUNGS01` as the single presentation path and inferred from
its shape that the engine had both an unscaled and a scaled blit. The call map now names them:

```
seg1:386e  USER.77   OFFSETRECT
seg1:38fe  GDI.443   SETDIBITSTODEVICE
seg1:3964  GDI.439   STRETCHDIBITS
```

So the whole renderer resolves to: composite into an off-screen 8-bit DIB with the engine's own
software blitter, then hand the DIB to GDI once per frame — `SetDIBitsToDevice` at 1:1,
`StretchDIBits` when scaled. There is no per-sprite GDI call anywhere on the path.

That is the best possible shape for the port. The SDL2 equivalent is exact and cheap: keep the
palette-indexed surface `src/blit.c` already composites into, expand through the palette into a
streaming texture, and present once. Scaling is already a concept the engine has, so resolution
independence does not have to be retrofitted against its grain.

`JUNGS01`'s only other GDI use is clipping — `SAVEDC`, `EXCLUDECLIPRECT`, `RESTOREDC` around the
composite, with `OFFSETRECT` and `INTERSECTRECT` for rectangle maths. All trivially portable.

## Toolchain resolved: Visual C++ 1.0, `/ASw /GD /GEf /Ox`, LINK 5.50

The previous section concluded the compiler was not Visual C++ because no flag combination
reordered the prologue. **That was wrong, and the reason is worth recording: the flag space was
swept too narrowly.** `/GEf` had been tried, but only alongside `/Gw`. Paired with `/GD` it is exact.

```
reference : b8 b9 15 55 8b ec 1e 8e d8 8b 5e 06 8b 87 10 01 1f 8b e5 5d ca 02 00
built     : b8 3d 00 55 8b ec 1e 8e d8 8b 5e 06 8b 87 10 01 1f 8b e5 5d ca 02 00
                 ^^^^^ relocated DGROUP word, fixed at link time and masked
```

Both functions attempted so far match:

| Function | Result |
|---|---|
| `RESCOUNTSTRINGS` | match, relocated word only |
| `RESCOUNTVARIABLES` | match, relocated word only |

`/GD` selects the DLL convention and `/GEf` the prologue form: `DGROUP` loaded into `AX` *first*,
then the frame, with no `inc bp` / `dec bp` export marker and a `mov sp,bp` epilogue. `/Gw` forces
the MakeProcInstance-style frame instead, which is why every `/Gw` combination stayed 14 bytes out.

So the full build is:

```
cl   /c /ASw /GD /GEf /Ox <file>.c
link /NOD <objs>, <out>.dll,, SDLLCEW LIBW, <out>.def
```

**MSC 7.0 was a red herring.** It was pursued because its code generator predates VC++ and the
prologue looked older, and a great deal of effort went into extracting it and finding a DPMI host
for it. That work is not wasted -- the extraction recipe and the DPMI matrix are recorded in
`toolchain/README.md` and both are reusable -- but it was not needed. The evidence that sent us
there, "no VC++ flag does this", was a statement about the sweep, not about the compiler.

**Method note, and the honest lesson.** A negative result from a brute-force sweep is only as strong
as the sweep's coverage, and it is very easy to state it as though it were a property of the tool.
The correct phrasing throughout should have been "no combination *tried* reorders it". The tell was
there: `/GEf` changed nothing under `/Gw`, which should have prompted trying it under every other
prologue mode rather than concluding the option was inert.

The matching metric can now move for the first time.

# Round 31 — what a frame list actually contains

## A frame list is a timeline, not a film strip

Decoding type 10 (see `docs/FORMAT.md`) took the share of frame-list entries that can be drawn from
65.4% to **91.3%**. The interesting part is what the remaining 8.7% turned out to be:

| Type | Entries | Share | What it is |
|---:|---:|---:|---|
| 1 | 12,097 | 65.4% | bitmap |
| 10 | 4,774 | 25.8% | repositioned bitmap |
| 13 | 1,212 | 6.6% | **undecoded** |
| 16 | 272 | 1.5% | 18 bytes, binds a type 14 scene script |
| 7 | 125 | 0.7% | audio |
| 14 | 5 | — | scene script |
| 8 | 1 | — | — |

**Nothing left is a picture.** So 91.3% is not a staging post on the way to 100%; it is effectively
the ceiling for drawables, and the right conclusion is that a type 15 "frame list" is a **timeline**
— draws, sounds, and commands interleaved — rather than a strip of animation frames.

That reframes what is missing. The port can already draw everything a sprite draws. What it cannot
do is *sequence* it, and sequencing is what types 13 and 16 carry.

## Type 16 — partial

272 records, **always 18 bytes**. The fourth word is a handle to a **type 14 scene script**:

```
0x00  u16  0x00d8 in every sample examined
0x02  u16  handle in the 0x80xx space (see below)
0x04  u16  handle in the 0x80xx space
0x06  u16  handle -> type 14, a scene script
0x08  u16  0
0x0A  u16  4
```

Its accessor `FUN_1008_6192` faults the resource in and passes it to `FUN_1008_d662`. So type 16 is
a script binding, not a drawable, even though `FUN_1008_5fdc` batches it to the sprite library
alongside types 1 and 10.

## Type 13 — two hypotheses tested and rejected

Type 13 is the largest undecoded piece of a timeline (6.6% of entries, 1,262 records). It is
recorded here as **not decoded**, with the failed attempts, because both looked convincing on a
single record and neither survives contact with the corpus.

**Rejected: a list of 14-byte entries.** One record in `JUNGOPTS` decomposes beautifully into
`{u16 tag, u16 slot[4], u16 target, u8 pad, u8 used}` where `used` equals the number of non-zero
slots. Across the disc that relation holds for **13.8%** of entries, and record sizes are not
multiples of 14 (the remainder mod 14 is spread evenly across 0, 2, 4, 6, 8, 10 and 12). The clean
parse was a coincidence.

**Rejected: the first word is a repeat count.** Sizes do not follow from it. `(size - 2) % tag == 0`
holds for 676 records and fails for 586 — indistinguishable from chance. There are **293 distinct
record sizes** across 1,262 records.

Type 13 is genuinely variable-structured and needs its consumer read. That consumer is not in
`JUNGLE.EXE`: `FUN_1008_5fdc` explicitly ignores type 13 while batching 1, 10 and 16, and the
per-type dispatch sends 9, 12, 13 and 14 to one shared lazy loader that only faults the bytes in.
The interpretation therefore lives in **`JUNGS01`, whose exports are stripped to `S_0nn`** — which
is the same wall the sprite API hit. That is the honest next frontier and it is a large one.

## A second handle space

Type 16's second and third words, and some values inside type 13, sit around `0x80xx`–`0x91xx` and
do **not** resolve as local resources. The engine's own arithmetic explains why: it computes
`handle + 0x7531`, and resource ids are `0x10000 + index`. A handle of `0x806f` yields `0xF5A0`,
which is below `0x10000`, so it is not a resource at all. A handle of `0x9152` yields `0x10683`,
which *is* a resource id — but index 1667, far beyond `JUNGOPTS`'s 279 entries even allowing for the
150 extra slots its header reserves.

So there is at least one further id space below `0x10000`, and resource ids appear to be allocated
across more than one open container. `JUNGLE.BIN` is the obvious candidate for a shared container
opened alongside the scene, which would also explain the reserved extra slots. Not yet confirmed.

# Round 32 — the entry points are statically derivable, and the opcode job is small

## Type 16 binds a script to an actor

Round 25 concluded that scripts are entered at engine-supplied record pointers and round 28 ruled
out `RESSETCALLBACK` as the source, leaving "entry points" as the blocker that gated everything.
Those rounds did not have type 16 decoded. It changes the picture.

A type 16 record's fourth word is a handle to a **type 14 scene script**, and this holds for
**272 of 272 records across the disc, with no exceptions**:

| Container | Actors | Distinct scripts |
|---|---:|---:|
| `JUNGBUGD` | 45 | 3 |
| `JUNGBURP` | 49 | 3 |
| `JUNGHIPP` | 58 | 5 |
| `JUNGPINB` | 45 | 3 |
| `JUNGSCOR` | 26 | 1 |
| `JUNGSHOT` | 49 | 3 |

Many actors share one script — 45 bugs running the same behaviour — which is an actor/behaviour
model, not a per-object program.

`FUN_1008_6192` faults a type 16 record in and hands it to `FUN_1008_d662`, which allocates a
**180-byte runtime actor**, copies the record's nine words into it at `+0x12`, and calls
`FUN_1008_d2d6`. That reads the actor's word at `+0x18` — the record's fourth word, the script
handle — resolves it and stores the result at `+0x02`. `FUN_1008_d612` attaches children into an
eight-slot array at `+0xA4`.

So an actor is created from data, with its script named in that data. **Entry points can be derived
statically after all.** They are not engine-supplied pointers; they are a field.

## What the scripts actually say

Disassembly makes two opcodes obvious immediately:

```
=== resource 25 (18 bytes) ===
  0000  op 18   len 18  6a 75 6e 67 63 72 65 64 2e 62 69 6e 00 00 01 00     "jungcred.bin"
=== resource 20 (8 bytes) ===
  0000  op 1    len 8   ef 8a 00 01 d0 8a
```

- **op 18 carries a container filename** — `jungopts.bin`, `jungcred.bin`, `jungint1.bin`,
  `jungint2.bin`. That is a **scene transition**.
- **op 1** takes two handles and a word, and the tiny 8-byte scripts differ only in the second
  handle (`0x8ad0`, `0x8ad1`, `0x8ad2`, ...). These are **menu actions**, one per button.

So type 16 actors are interactive objects bound to short scripts, and the shell of the game is
scene transitions driven by them.

## The opcode job is 16 opcodes, not the whole engine

Running the census over every container:

```
total: 27,185 executions, 51 distinct opcodes
  op 76  14,364  52.8%
  op 77   3,589  13.2%
  op 37   3,230  11.9%
  op 5      956   3.5%
  op 89     611   2.2%
  ...
  ==> 16 opcodes cover 95% of all script execution
```

**Three opcodes are 78% of everything the game does. Sixteen cover 95%.**

This reframes the estimate in `docs/PORT_PLAN.md`, which called the interpreter "larger than
everything done so far combined". That estimate was about *transcribing* the engine: the 18
first-layer handlers pull in their transitive closure, `op5` alone reaching a primitive with 85
callers, and that is genuinely enormous.

But a port does not have to transcribe the engine. It has to **reimplement what each opcode means**
against its own renderer — and the renderer, compositor, palette, timing and asset layers already
exist here and are tested. The transitive closure matters for a matching decompilation. It does not
bind a reimplementation.

That is the difference between a months-long decompilation and a bounded piece of work, and it is
the first time this project has had a number for it.

## The scene graph, executed

Implementing six opcodes took the main scripts from ten unimplemented actions to five, and produced
the first behaviour that is recognisably *the game*.

| Opcode | Engine arm | Meaning |
|---:|---|---|
| 89 | `case 0x59` | evaluate an expression and latch it in a result global (`DAT_1020_40ae`) |
| 21 | `case 0x15` | store the record's word 1 in a global |
| 29 | `case 0x1d` | store the record's word 1 in a different global |
| 31 | `case 0x1f` | pass a byte to a mode setter |
| 18 | — | **scene transition**, container named inline |
| 56 | `case 0x38` | scene transition, name after a length word |

Running every actor script on the disc and collecting the transitions gives the game's navigation
graph, derived by execution rather than assumed:

```
JUNGPINB res 25 -> "jungcred.bin"
JUNGPINB res 26 -> "jungint1.bin"
JUNGPINB res 27 -> "jungint2.bin"
JUNGPINB res 30 -> "jungopts.bin"
```

Every container reaches the same seven targets — credits, both intros, main, options, party and
score — which is a **persistent menu bar present in every scene**, exactly as the game behaves.
**91 transitions across 13 containers.**

The remaining unimplemented actions in the main scripts are five: **59, 60, 47, 28, 50**. Ops 59 and
60 share the handler `FUN_1008_aa68`, which resolves two operands through `FUN_1008_e9c2` — the
**string** resolver — and dispatches on which of the two opcodes it is.

### The second id space is strings

`FUN_1008_e9c2` is what resolves the `0x80xx` handles that never made sense as resources. The actor
spawn path uses it on the type 16 record's word 1, and ops 59/60 use it on two of their operands.
So the handle space below `0x10000` after the `+ 0x7531` bias is a **string table**, not a second
resource directory. That closes the question left open in round 31.

## Input: op 12 is the keyboard map

`FUN_1008_29c4` picks a slot from the flag bytes at `+6..+10` and stores the word at `+4` into it.
The word at `+2` turns out to be a **Windows virtual key code**, which makes the whole record a key
binding. `JUNGPINB` resource 1 is nothing but sixteen of them:

```
vk 0x6b NUM+  -> res 4 (script)     vk 0x1b ESC   -> res 3  (script)
vk 0x6d NUM-  -> res 5 (script)     vk 0x1b ESC   -> res 28 (script)
vk 0x6a NUM*  -> res 10 (script)    vk 0x4d 'M'   -> res 28 (script)
vk 0x20 SPACE -> res 15 (script)    vk 0x42 'B'   -> res 20 (script)
                                    vk 0x44 'D'   -> res 21 (script)
```

**Every target is a type 14 script.** So the engine's input model is: a key binds to a script, and
pressing it runs that script.

Following the chain end to end, entirely by execution:

```
press M -> script 28 -> op 18 LOAD SCENE "jungmain.bin"
press B -> script 20 -> op 1 (res 32, res 1)
press D -> script 21 -> op 1 (res 32, res 2)
```

`M` returns to the main menu. That is the real game behaving as the real game, driven by its own
data through a reimplementation.

Op 1 takes two script handles rather than a filename, so it is an in-scene call or handler
installation rather than a transition.

## Op 47 is the other half of input

`FUN_1008_934a` resolves the record's word 1 as a **variable** and stores `GetKeyState()` of a
virtual key into it. So scripts both bind keys to scripts (op 12, edge-triggered) and poll key state
into variables (op 47, level-triggered) — which is exactly what a game needs for menus and for held
movement keys respectively.

## Where the opcodes stand

Running all 119 type 14 scripts in `JUNGOPTS`, every one executes. Across the whole script set the
remaining unimplemented actions are led by ops 2, 1, 4, 93, 45, 64, 5 and 16 — the ones the main
scripts never reached. The main scripts themselves are down to 59, 60, 47(done), 28(done), 50.

# Round 33 — the LZW backgrounds have always decoded to garbage

Rendering a scene headlessly and looking at it found a bug that every previous round missed.

`--scene` composes what `--game` would draw and writes a PNG, so the result can be inspected with
no display and no person. The first one rendered was `JUNGMAIN`, whose background is resource 69,
800x600. It came out as coloured noise — recognisable shapes under heavy speckle.

**The Python reference decoder produces the identical garble.** So the C is faithful to the
reference, and both are wrong.

## Why this was never caught

The project has reported **"11,506 / 11,506 bitmaps decoded, 0 failed"** since the asset work
finished, and `README.md` called bitmaps complete. That number only ever meant *the decoder returned
without error*. It was never compared against a correct image, because there has never been a
reference frame.

The cross-validation was self-referential too: the C decoder was checked byte-for-byte against the
Python one, and they agree — on the same wrong output.

## The blast radius is small but it is the backgrounds

Only **28 of 11,506 bitmaps on the disc use LZW**; everything else is RLE and is fine. But those 28
are the large full-screen backgrounds — `JUNGMAIN` res 69 at 800x600 and res 70 at 640x480 are both
LZW. So the single most visible asset class in the game is the one that is broken, and the sprite
work that looked correct in `--compose` and `--anim` was all RLE.

That is also why it survived: the LZW path is exercised by 0.24% of the corpus.

## What is and is not the cause

Ruled out by measurement rather than guessed:

- **Not the chunk stream offset.** LZW images have no per-row offset table, and `bitmap_decode`
  already starts at the 20-byte header for them. Starting after a row table instead makes only
  4 of 28 images parse; starting at the header makes **28 of 28** parse with valid methods and a
  proper terminator.
- **Not an unhandled chunk method.** Resource 69 is 61 chunks of method `0x6000` and a terminator,
  and `0x6000` is implemented. (Method `0x2000` *is* unhandled and appears 60 times disc-wide, so
  that is a second, separate bug.)
- **Not bit order or dictionary base.** The engine's `FUN_1000_2470` sets its next code to `0x100`
  and shifts right by `bits - width`, which is MSB-first — both match this implementation.

So the fault is inside the LZW inner loop. `FUN_1000_2470` carries something this implementation
has no equivalent for: a rotating eight-entry table at `+0x20`, indexed by a counter advanced as
`(n + 2) & 0xE`, whose high byte is used to **mask** each assembled code. That is not ordinary LZW,
and it is the most likely place the two diverge.

## The lesson

"Decoded without error" is not "decoded correctly", and two implementations agreeing proves only
that they share an author's assumptions. The check that found this was rendering the result and
looking at it — which cost one new mode and one glance.

## Narrowing the LZW bug, and a metric to score any fix

**A control settles that it is really broken.** Horizontal adjacency — the fraction of neighbouring
pixels with the same index — is **0.850** on a known-good RLE bitmap from the same game and
**0.317** on the LZW background. Heavy dithering was the obvious innocent explanation for the
speckle, and this rules it out: the art the engine draws correctly is not dithered anything like
that hard. Any candidate fix can be scored against 0.85 automatically, with no eyeballing.

**The decompression is structurally correct.** Every LZW bitmap decodes to *exactly* `stride ×
height` bytes — **15 of 15** checked across six containers, 480,000 and 307,200 on the nose. A
decoder that had lost sync would not land on the exact size fifteen times.

Eliminated by measurement:

| Hypothesis | Result |
|---|---|
| Chunk stream starts after a row table | No. 28/28 parse from the header, 4/28 with a row table, and the C already starts at the header. |
| An unhandled chunk method | No, not for this image. Resource 69 is 61 chunks of `0x6000` plus a terminator. (`0x2000` *is* unhandled, 60 chunks disc-wide — a separate bug.) |
| LSB-first bit order | No. Renders as pure noise, far worse than MSB. |
| Wrong dictionary base or end code | No. `FUN_1000_22b8` sets end `(1<<w)-1`, max `(1<<w)-2`, and `FUN_1000_2470` starts next at `0x100` — all matching. |
| A row or plane filter on the output | No. Cumulative-add per row 0.000, XOR previous row 0.009, add previous row 0.006, four-plane de-interleave 0.298 — every one worse than the 0.317 raw. |
| Heavy dithering, ie not a bug at all | No. See the control above. |

**A false lead worth recording.** An automated search over bit-order and dictionary-base variants
scored LSB-first highest, and it was wrong: the score was inflated by a large flat region of
*undecoded* buffer that the variant left unfilled. A smoothness metric rewards a decoder that gives
up early. Any future use of this metric has to require the full `stride × height` first.

So the fault is inside the inner loop, producing plausible-length output from a diverging
dictionary. The prime suspect remains `FUN_1000_2470`'s handling around its rotating mask table,
which is an optimisation of the bit extraction whose phase must be reset in a way this
implementation does not reproduce.

## Everything except the LZW codec is working

`JUNGLE.BIN` is the one container whose background is RLE rather than LZW, and `--scene` renders it
correctly: a bamboo frame around a hanging sign, a moon and a starfield, right colours, right
placement. So the container layer, the palette, the compositor, the scene executor and the scene
composition are all sound. The single defect is the LZW codec.

That defect is not cosmetic. **Every 800x600 background on the disc is LZW** — twelve of the
thirteen containers — so it is the backdrop of nearly every screen in the game.

### The control had to be redone

The first control was a 424x176 sprite, which is unfair: sprites are mostly flat transparent
background and score high for free. Redone against `JUNGLE.BIN`'s **full-screen RLE background**,
which is known correct because it renders as recognisable art:

| Image | Adjacency |
|---|---|
| RLE background, renders correctly | **0.855** |
| RLE sprite | 0.850 |
| LZW background | **0.317** |

The fair control lands in the same place as the unfair one, so 0.85 stands as the target.

### More eliminations

| Hypothesis | Result |
|---|---|
| Dictionary carried across sub-blocks | No. Produces 582,000 bytes for a 480,000-byte image; resetting per sub-block lands on 480,000 exactly. |
| Growth not stopped at `maxc` | No effect either way. |

Resetting the dictionary per sub-block is therefore confirmed correct, and the decoder hits the
exact output size on 15 of 15 images while still producing noise. A decoder that had lost bit sync
would not repeatedly land on the exact byte count, so the divergence is in what the dictionary
*contains*, not in how much it emits.

### Further eliminations, from the assembly rather than the decompiler

For a bit-twiddling routine the decompiled C is unreliable, so `FUN_1000_2470` was read as
disassembly. The bit reader is:

```asm
mov si, [bx+0x1e]        ; phase index, reset to 0 on entry
mov cx, [bx+si+0x20]     ; mask for this phase
add si, 2 / and si, 0xe  ; advance phase, 8 entries
mov ah, [bx+0x30]        ; carried byte into the HIGH half
and ax, cx               ; mask off the bits already consumed
mov al, fs:[esi]         ; next byte into the low half
cmp cx, [bx+0x12] / jge  ; one more byte if 8 bits is not enough
shr eax, cl              ; cl = bits - width
mov [bx+0x30], al        ; carry is the last BYTE, not the leftover bits
```

That is a byte-carry-plus-phase-mask formulation of exactly the MSB-first reader this project
already has — it stores the last byte and masks the consumed high bits, where ours stores the
leftover bits directly. Equivalent, and ruled out as the cause.

Also ruled out this round:

| Hypothesis | Result |
|---|---|
| The engine's 1-byte length table and signed walk loop | No. Implemented faithfully, including `length[next] = length[prev] + 1` as a byte and the `cVar4 != 0 && cVar5 > 0` termination — byte-identical output, adjacency unchanged at 0.317. |
| Wrong row width in the decoded buffer | No. Adjacency scanned across widths 200…2400 stays flat at 0.317–0.319, with no peak. Vertical adjacency at width 800 is 0.371, also flat. |
| Same art available as RLE somewhere for ground truth | Not found. 29 large RLE bitmaps exist and duplicates occur across containers (`JUNGBUGD` res 110 and `JUNGBURP` res 108 are both 456x393 at exactly 14,227 bytes), but no background appears in both codecs. |

The decoded bytes are noise under every interpretation tried, while landing on the exact output
size every time. That combination — perfect length, wrong content — remains the whole shape of the
bug.

### The encoder's band structure, and two final eliminations

Instrumenting the decode per sub-block shows the encoder's layout plainly. Resource 69 is **30
pairs** of sub-blocks, alternating **8191** and **7809** output bytes — 16,000 per pair, which is
exactly 20 rows of 800, and 30 pairs is exactly the 480,000-byte image. `8191` is `0x1FFF`, the
13-bit maximum of the chunk length field, so the encoder caps a sub-block's output there and puts
the remainder in a second. Every sub-block terminates cleanly on its end code.

So the framing is completely understood, and it is right.

| Hypothesis | Result |
|---|---|
| Dictionary resets when full rather than freezing | No. Produces 420,522 bytes instead of 480,000; freezing lands exactly. |
| The control was unfair (flat art flatters RLE) | No, the opposite. A *detailed* RLE image, `JUNGBUGD` res 110 at 456x393, scores **0.966** — higher than the night-sky background's 0.855. The gap to 0.317 is wider than first measured, not narrower. |

One number is worth keeping in view: a single sub-block consumes **3,907 codes** while width 11
allows only **1,790** dictionary entries. So more than half the codes are emitted against a frozen
dictionary. That is legal LZW and the decoder stays in sync — the exact output length proves it —
but it means the encoder leans hard on late-dictionary state, where any small divergence early
would corrupt everything after without changing the byte count.

### Next approach

Every attempt so far has argued that some part of this implementation is *equivalent* to the
engine's, and every one of those arguments has held up while the output stayed wrong. The remaining
move is to stop reasoning about equivalence and **transcribe `FUN_1000_2470` from its disassembly
instruction by instruction**, including the register-level details the decompiler smooths over. The
function is 458 bytes, fully self-contained, with no relocations — so it can be transcribed exactly.

## Round 34 — there was no LZW bug; it was the palette base

Transcribing `FUN_1000_2470` instruction by instruction, together with its setup `FUN_1000_22b8`
and its sub-block driver `FUN_1000_263a`, turned up no difference from the C at all:

- **Bit reader:** MSB-first with a fixed width. `[bx+0x12]` is never written inside the decode
  loop. The eight-entry mask table that `22b8` builds for widths 9–12 is exactly `(1 << leftover) - 1`
  in the high byte, which is what a standard reader computes.
- **Codes:** end code `(1<<w)-1`, freeze at `(1<<w)-2`, `next` starts at `0x100`. There is no
  clear code.
- **String output:** strings are written backwards using a byte-wide length table. KwKwK is taken
  when `code >= next`.
- **Sub-blocks:** each carries a u16 length and starts with fresh state. The driver stops early
  only if the output would overflow.

The C already did all of this. So the codec was never the fault.

**What was wrong:** looking at the decoded pixels settled it. Resource 69 had perfect geometry (the
main-menu hub: deck chair, sign board, pond, drums) in psychedelic colours. The indices were right
and the palette was not.

`container_open` placed the palette at index 10 only when the table held exactly 236 entries, and at
0 otherwise. Only JUNGOPTS, JUNGLE, JUNGHIPP, JUNGINT1/2, JUNGPINB and JUNGSHOT have 236 entries.
JUNGMAIN (232), JUNGBUGD (229), JUNGBURP (228), JUNGPRTY (233), JUNGSCOR (231) and JUNGCRED (139)
carry a trimmed table. JUNGMAIN's first bytes are identical to JUNGOPTS's, so it is the same
10-based palette, just shorter.

**Fix:** the base is always 10. All 18 full-screen backgrounds across all 13 containers now render
correctly.

**Why the metric misled:** horizontal adjacency counts neighbours with *identical* indices.
Smoothly shaded painted art has many near-equal but distinct indices, so it scores low whatever
palette is loaded, and the RLE sprite art used as the control is flat-shaded cel art. The metric
measured art style, not decoder correctness. The fifteen exact `stride × height` lengths were the
real evidence, and they said the decoder was right.

**Lesson:** render the result and look at it before building a theory, and look again after
every elimination that fails to change the picture. This time the indices could have been
eyeballed on day one. A falsified palette looks like noise at thumbnail size but not at full size.

# Round 35 — the runtime engine: the game boots, navigates and starts playing

`src/engine.c` reimplements the scene runtime from `JUNGLE.EXE` and `JUNGS01.DLL`. Each part was
taken from the original function named beside it in the source. The game now **boots on its own**:
JUNGLE → JUNGINT1 → JUNGINT2 → JUNGMAIN, with the 7th Level logo, the title, and the Timon and
Pumbaa intro. The main menu then **launches a game from a click**, by way of the menu's own
character state machine. In Hippo Hop, Timon gives his introduction, the hippos swim, and the
keyboard reaches the player's input script.

## How a scene runs

- **Entry.** Loading a container (`FUN_1008_bd72`) runs resource 0. `DAT_1020_14ea` is
  uninitialised data, so the entry is always index 0. Resource 0 is structurally identical in
  every container; only two constants differ, the scene's own setup scripts, which it calls
  through expression op 13.
- **Deferred work.** Op 21 schedules a one-shot post-load script (`14e0`, run from
  `WM_USER+0xC8`). Op 29 sets a focus script. Op 18 queues the scene change for the next
  message-loop pass (`WM_USER+0xC9`). Op 30, clicks and input all go through a 40-entry
  deferred-call queue (`FUN_1008_ea92`).
- **Calls have frames.** Expression op 13 and record op 1 both build a call frame below
  `DAT_1020_10ac`. Arguments become locals `0x13FE…`, and the callee's op 89 return value comes
  back through `DAT_1020_40ae`.
- **Globals are per scene.** Globals 0–5000 (`0x151E`, `0x2712` bytes) are saved when a container
  is left and restored on return. A first visit starts from zero plus key resource 3, a list of
  `{index, value}` pairs. Globals from 5001 up are shared by every scene: system facts, settings
  such as `NumPlayers`, and the mouse position.

## Sprites

- **Creation and placement.** Op 5 faults in a type 15 and creates its sprite. The z-order comes
  from `+0x0C`, and the cel table is the handle list filtered to types 1, 10 and 16. The sprite
  is then given a type 13 program. New sprites are anchored at the canvas centre (399, 299).
- **Chaining.** Characters chain animations through builtin `0x5B`, which queues event `0x15`
  (a call) for when the program ends. The main menu's Timon runs a 1,472-byte state machine
  (script 559) this way. A clicked sign records the request in `g36`/`g33`, and the action fires
  when the requested animation starts.
- **Other ops.** Op 24 swaps one sprite for another in place. Op 36 moves a sprite relatively,
  shifting any path it is following. Builtin `0x65` clones a type 15 to spawn copies (the hippos).

## Input

- **Clicks** follow `FUN_1008_2b04`. The topmost opaque sprite's type 15 `+0` click script
  (which op 4 can rewrite) is tried first, then press/release objects (`+6`/`+8`), then
  rectangle hotspots. A right-click on a menu sign runs its action immediately.
- **Game controls** are a device layer.
  - Op 83 binds a device to a player, and op 84 sets the player's input script.
  - Keyboard layouts are six `{vk, code}` pairs, with defaults in the EXE's data segment at
    `DS:0x3C`. Games override them from variables: for Hippo Hop, arrows plus X and Z.
  - Direction presses combine into 8-way codes through the table at `DS:0x54`.
  - Each event queues `input_script(player, code)`.

## Bugs found on the way

| Bug | Effect |
|---|---|
| Decoded bitmaps are bottom-up DIBs; sprites were composed top-down | every sprite's vertical position mirrored; foreground tiles showed at the wrong edge |
| Relational ops 23–26 inverted (the old VM followed a misread decompile) | four of six comparisons wrong in every script |
| Record op 37 (jump) reported by `record_len` as −2, read as "stop" | every `goto` ended its script; Hippo Hop never set its player count |
| Switch tables matched entry `+0`; the value is at `+2` | every switch took its default |
| A program replaced by its own op 20 had its new PC advanced by the old record | characters froze mid-program |
| Sprites capped at 64 cels; type 15 lists run to 1,750 | long animations finished instantly |
| Bitmap origin fields swapped in FORMAT.md | `+0x0A` is X, `+0x0C` is Y (`FUN_1000_0b30`) |

## What is not done

- **Records:** ops 16 (arithmetic fold), 28 (`S_066`), 31, 33 (collision setup), 41, 56/59/60
  (string formatting and INI access). Sound plays silently: completion events are timed from each
  clip's length.
- **Builtins:** `0x5A`, `0x7B`, `0x83`, `0x84` and the trig functions.
- **Hippo Hop:** Timon walks the bank but does not yet hop, and only one lane fills.

These are sprite-library queries whose exact semantics have to come from JUNGS01.

# Round 36 — all five games play

- **Coordinates are centre-relative.** JUNGS01 calls `SetViewportOrg(cx/2, cy/2)`, so sprite
  anchors, hotspots and the mouse are all measured from the screen centre. New sprites start at
  (0, 0). Getting this wrong had left every scripted position off-screen: Hippo Hop's lanes, and
  Timon in the main menu.
- **Sprite commands** (builtins `0x5A`/`0x5B`, `FUN_1000_57d6`) are type 13 opcodes issued by
  scripts. `0x5B` appends a raw-encoded record to the program, and event `0x15` becomes a record
  that calls a script. `0x5A` applies the command at once. Both fault the sprite in first; Pinball
  relies on this to create its 40 fixtures.
- **Input model**, completed:
  - The mouse filter runs on every release and move (code 2, code 4).
  - Hover enter/leave calls a hover script (`0x6F`). The keyboard filter is set with `0x69`.
  - Press/release objects drag.
  - Each key has one binding slot with plain, Shift, Ctrl and key-up variants (op 12).
- **Resources:**
  - Op 2 creates a type 15's sprite, and op 3 destroys it.
  - Resource 3 of each container holds `{index, value}` pairs that initialise its globals.
    Globals 0–5000 are saved per container, and higher ones are shared.
- **Pinball physics** is script code over a few native primitives, all ported from the
  assembly:
  - ray to box (op 50, `FUN_1008_6aec`, using JUNGU01's tangent table)
  - elastic collision (`COLLIDE`)
  - point-on-sprite (`S_080`) and sprite-overlap (`S_048`) tests
  - line and arc points (`S_074`/`S_075`)
  - sine, cosine, tangent, angle, distance and reflect, with the tables read from the user's
    JUNGU01.DLL
- **Small bugs that blocked whole games:**
  - The op 20 owner tag must be the sprite's index, not its handle; Bug Drop's switch fell
    through.
  - A 64-cel cap, a 128-sprite cap, and a test harness that clicked faster than the queue could
    install the release filter.
- **Tools:** `tools/script_decomp.py` renders a script as readable pseudo-code, with operands as
  `gN`/`LN`/`#N` and expressions as infix.

**Not done:** MIDI music (ops 80/81), string formatting and INI persistence (ops 56/59/60), the
type 16 text objects (op 41) that high-score names use, and op 61 hover regions.

# Round 37 — music, fades, a full game to the high-score table, four platforms

- **Music.** Type 4 tracks are dword streams for JUNGA01's sequencer (`FUN_1000_1b20`): a
  MIDI short message, or with any top-nibble bit set, a delay of `w & 0x7FFFFFFF` ms (1–2 ms
  raised to 3). Ops 80/81 play and stop; one track at a time, looping by count. `src/synth.c`
  plays them. The format is in `notes/type4-midi-format.md`. Hippo Hop's music is gated by the
  game itself: while the INI play count `Entries` (g5041) is 5 or less, script 570 shows help first.
- **Fades.** Ops 10/11 run `FUN_1008_3bb2`. On a palette display (the 256-colour case) they
  scale the palette from index 10 over `speed` steps and hold the engine until done; the
  faded flag starts set, so the first op 10 fades in. A true-colour display gets a box wipe
  instead (`FUN_1008_37d6/386a`), which the port does not use.
- **sprintf string arguments are raw handles.** `FUN_1008_9b20` hands a string argument's word
  to `FUN_1008_e9c2` undecoded. Decoding it as a variable made every score compare as blank, so
  no game ever reached the high-score table.
- **Text entry** (op 54, `FUN_1008_dbdc`, keys in `FUN_1008_dc3c`): a type 16 object becomes an
  edit field with a `_` cursor; Enter keeps the text, Escape restores it, and either copies it to
  a string variable and queues a done script. Key bindings are skipped while a field is open.
- **Smaller ops:** 68 (sprite visible query), 71 (repaint now, a no-op here), 73
  (SetCursorPos from the viewport origin).
- **Checked end to end:** Hippo Hop from the menu through five timed rounds to GAME OVER, the
  score into slot 1, a typed name saved to `7THLEVEL.INI`, QUIT to the menu and NEW GAME, and
  the menu's Score screen showing it. The Options screen writes MusicOff/SoundFXOff/Intro.
- **Platforms:** macOS; iOS (runs in the 18.1 simulator); PS Vita (`jungle.vpk` builds);
  Windows (`jungle.exe` cross-compiles). See `docs/PLATFORMS.md`.
- **Harness:** untimed events are spread over the run, so runs of different length start games
  at different times; give events explicit `TIME=` prefixes when comparing runs. The event list
  had a silent 32-entry cap (now 4096). `ENGINE_WATCH=N` logs changes to global N.
