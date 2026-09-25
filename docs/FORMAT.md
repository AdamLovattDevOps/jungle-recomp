# The 7th Level "7L" container format

Reverse-engineered from *Timon & Pumbaa's Jungle Games* (1995). The engine is 7th Level's **Runtime
Player 1.2**, so this format is shared with their other titles.

Verified three independent ways: the read path (`RESOPENFILE` and the table parsers), the write path
(`RESWRITEHEADER`, `RESWRITESCRIPTBUF`), and re-serialisation (39/39 structures byte-identical).

All values little-endian.

## File header — 286 bytes

| Offset | Type | Meaning |
|---|---|---|
| `0x00` | u16 | magic `0x4C37` = `"7L"` |
| `0x02` | u16 | header size, 286 on this disc |
| `0x54` | u16 | version, must be 2 |
| `0x60` | u32[] | cumulative end offsets of resident-blob segments |
| `0xA0` | u16 | resident-blob segment count |
| `0xA6` | u16 | extra resource slots added at load |
| `0xA8` | u16 | string count |
| `0xAA` | u16 | variable count |
| `0xBE` | — | table descriptor array, `{u32 offset, u32 length}` each |
| `0xEE` | — | palette descriptor (array slot 7) |

Validity, as the engine itself checks: the 8-bit sum of all header bytes must be zero, and table 0's
length must be non-zero.

## Table descriptors

| Slot | Loader | Purpose |
|---:|---|---|
| 0 | `FUN_1000_050c` | resource directory |
| 1 | `FUN_1000_04b8` | entries (`RESENUMENTRIES`) |
| 2 | `FUN_1000_075a` | unused in all 13 containers |
| 3 | `FUN_1000_061a` | variables (`RESENUMVARIABLES`) |
| 4 | `FUN_1000_0720` | constant strings (`RESGETCONSTSTR`) |
| 5 | `FUN_1000_066a` | resident blob, the "script buffer" |
| 7 | — | palette, 4-byte `PALETTEENTRY` records |

The tables sit contiguously at the tail of the file, each starting exactly where the previous ends.

## Resource directory — 10 bytes per entry

```
0x00  u16  type
0x02  u32  offset
0x06  u32  size
```

**Offset means two different things**, selected by type:

- types 8–16: an offset into the **resident blob** (table 5), loaded whole at open
- all other types: a plain **file offset**, seeked and streamed

## Sprites — type 15

```
0x02  u16  frame count
0x04  u16  frame count, copied from 0x02 when the record is faulted in
0x0C  i16  z-order, value-decoded (raw >= 0x159F is raw + 0x7531, else a global);
           typically 29000 or 30000; sprites paint in ascending order
0x10  u8   passed to the sprite-build call
0x12  u8   flag
0x13  u8   passed to the sprite library
0x14  u16[count]   frame handles
           followed by four zero u16s
```

So `size == 0x14 + count * 2 + 8`, which holds for every distinct record size observed.

Recovered from `FUN_1008_5e9a` (the type 15 loader) and `FUN_1008_5fdc`, which walks the handle
array, calls `RESGETTYPE` on each, and batches types 1, 10 and 16 to the sprite library while
remembering type 7 (audio) and type 4 separately.

Across the 13 containers this yields **1,294 sprite records and roughly 18,500 frame references**,
all in range.

## Positioned bitmaps — type 10

```
0x00  u16  bitmap handle
0x02  i16  origin X
0x04  i16  origin Y
```

Six bytes, always. Recovered from `FUN_1008_5a66`, which allocates a 20-byte control block, copies
the referenced **bitmap's** control block into it, and then overwrites two fields with the record's
own coordinates — so a type 10 is a shared bitmap placed somewhere else, and its coordinates
*replace* the bitmap's own origin rather than offsetting it.

Verified across all 13 containers: **4,755 of 4,755** records are exactly 6 bytes and every first
handle resolves to a type 1 bitmap. No exceptions.

Type 10 is not a minor case. In `JUNGHIPP.BIN` it outnumbers direct bitmap references in frame lists
by roughly two to one, and across the disc it accounts for a quarter of everything a sprite draws.

## Handles

A resource handle is not a directory index. The engine computes `handle + 0x7531` and identifies
resources as `0x10000 + index`, so:

```
index = handle + 0x7531 - 0x10000 = handle - 0x8ACF
```

Verified two independent ways: every frame handle in every sprite record resolves to an in-range
directory entry of a sensible type, and every bitmap's self-handle at `+0x0E` resolves to its own
index (11,506 of 11,506).

## Resource types

| Type | Contents |
|---:|---|
| 1 | bitmap |
| 7 | audio |
| 9, 10, 11, 13, 15, 16 | small resident records |
| 14 | scene script |

## Bitmaps — type 1

20-byte header, then `height` u16 row offsets, then pixel data.

```
0x00  u16  header size, always 20
0x02  u16  source width
0x04  u16  destination stride
0x06  u16  height
0x08  u16  flags; bit 0x8000 = RLE compressed
0x0A  i16  origin X offset
0x0C  i16  origin Y offset
0x0E  u16  the bitmap's own resource handle
```

Placement (JUNGS01 `FUN_1000_0b30`) centres the cel on the sprite anchor, then shifts it:
`left = x - ((w-1)/2 - hdr[0x0A])`, `top = y - ((h-1)/2 - hdr[0x0C])`, with `w = hdr[0x02]`,
`h = hdr[0x06]`. Flip negates the offset. An earlier version of this file had X and Y swapped.

The two origin fields matter: frames are cropped individually to their content, so a sprite
animation whose frames are drawn at a fixed point jitters. Drawing each frame at its own origin is
what makes it hold together. Across `JUNGMAIN.BIN` record 2460 the pair traces an arc -- X rising
steadily while Y falls and then rises -- which is a leap.

`+0x0E` holding the bitmap's own handle was checked against **all 11,506 bitmaps in all 13
containers with zero mismatches**, which also confirms the handle-to-index rule below from an
independent direction.

Pixels are 8-bit palette indices, stored **bottom-up** like a Windows DIB.

**RLE** (flag set), from `FUN_1000_0230`:

```
0x00        end of row; a row beginning with 0x00 ends the image
0x01..0x7F  run: the next byte repeated b times
0x80..0xFE  literal: copy (0xFF - b) bytes
0xFF        literal: length is the next byte
```

**LZW** (flag clear), from `FUN_1000_218c`: a chunk stream, each chunk a `u16` tag with the method in
the top three bits and the length in the low 13.

| Tag | Method |
|---|---|
| `0x0000` | stored |
| `0x4000` / `0x6000` / `0x8000` | LZW with 10 / 11 / 12-bit codes |
| `0xE000` | end |

Inside a chunk, sub-blocks each carry a `u16` length and are decoded independently — **the
dictionary resets per sub-block**. Codes are fixed-width MSB-first, dictionary `1<<N`, `(1<<N)-1` is
the end marker.

## Palette

4-byte records in `PALETTEENTRY` order (R, G, B, flags) — *not* `RGBQUAD`. 236 entries, mapping to
indices 10–245, because Windows reserves 20 system colours.

## Audio — type 7

28-byte header, then ADPCM data.

```
0x00  u32  payload size
0x04  u32  flags
0x08  u16  parameter
0x0A  u16  id
0x0C  WAVEFORMATEX   describes the DECODED output
```

Every clip on this disc is 22050 Hz, mono, 16-bit **after decoding**.

**The codec is 4-bit ADPCM**, IMA-derived but not IMA:

```
delta = step[(nibble & 7) + index]
if nibble & 8: delta = -delta
pred  = clamp(pred + delta, -32768, 32767)
index = clamp(index + index_adj[nibble], 0, 704)
```

The adaptation index runs at **8x resolution**: `index_adj` is IMA's `[-1,-1,-1,-1,2,4,6,8]` times
eight, the clamp is `88 * 8`, and the step table is interpolated to 712 entries. Both tables live in
`JUNGA01.DLL` segment 2 at `0x1E0` and `0x770`.

## Scene scripts — type 14

A record stream with real control flow. Each record is a `u16` opcode; **length is a property of the
opcode, not a stored field**. Some store it, some fix it, some compute it.

Control flow:

| Opcode | Behaviour |
|---:|---|
| 37 | jump; advance is **signed**, so negative means a backward jump (loops) |
| 77, 79 | conditional branch: advance `u16[1]` if false, `u16[2]` if true |
| 78, 43 | switch: case count at `+8`, table at `p + u16[+4]`, default at `+6`, 6-byte entries |
| 38 | compare two variables, operator byte at `+8` |
| 0, 44 | end of script |

Opcodes 76 and 89 carry **expression bytecode** for the stack VM.

## The stack VM

35 opcodes, 1 byte each; opcodes 1, 2, 4, 37, 38 carry a `u16` (3 bytes total) and 12, 13 carry a
`u32` (5 bytes). The stream ends at opcode 0, yielding the top of stack.

A complete C-like integer expression evaluator: `+ - * / %`, all six relational operators, logical
and bitwise AND/OR/XOR/NOT, negate, increment, decrement, pointer load and store. Division and
modulo by zero call error handler `0x6F`.

**Variable index space:**

| Range | Meaning |
|---|---|
| `< 0x13FE` | script variable |
| `0x13FE`–`0x159E` | engine registers |
| `>= 0x159F` | immediate constant, value `index + 0x7531` |

## The thing that matters most for a port

Resource ids and coordinates are **computed into variables at runtime**, not written literally in the
script. The draw layer resolves them by reading the same variable table. A static extractor therefore
cannot produce a scene layout — **the VM has to run**.

## VM memory model

The two stacks are not interchangeable. `0x70C` holds **addresses**, `0x70E` holds **operands**:

| Opcode | Behaviour |
|---:|---|
| 1 | push immediate onto the operand stack |
| 2 | resolve the index to an address, **dereference**, push the value |
| 4 | resolve the index to an address, push the **address** without dereferencing |
| 3 | pop, then indexed load: `base + index * 2` — array subscripting |
| 8 | pop, then **store**: write the operand to the address |
| 39 | load through the address |

The address form is `table_base + index * 2` for `index < 0x13FE`, and
`DAT_1020_10AC - index * 2 + 0x27FC` above it — note the **descending** second region, which is why
the engine-register indices sit at the boundary and grow downward in memory.

Opcodes 2 and 4 differing only by a dereference is what gives the scripting language lvalues: `4`
produces something assignable, `2` produces its value.

### Implementation status

`tools/vm.py` evaluates all 35 opcodes and executes every expression on the disc, but models memory
in a simplified way — it does not yet maintain a real variable table across the two stacks. That is
sufficient for verifying the instruction decoder and the arithmetic, and **not** sufficient for
recovering scene state.

Completing it requires implementing the address/operand split exactly as above. Until then the VM
should not be used to answer "what does this scene draw", because a simplified memory model will
produce plausible and wrong answers rather than obvious failures.

## Outer opcode semantics

Tracing each dispatcher case through its handler to the APIs and DLL exports it reaches names the
opcodes without transcribing them:

| Opcode | Reaches | Function |
|---:|---|---|
| 7 | `GETCURSORPOS`, `SCREENTOCLIENT` | mouse position |
| 8, 9, 14, 15 | `A_029`, `A_027`, `A_037`, `A_039` | audio (JUNGA01 exports) |
| 13, 18 | `A_008`, `POSTMESSAGE` | audio event, posted to the message queue |
| 19 | `TIMEGETTIME` | timing |
| 47 | `GETKEYSTATE` | keyboard |
| 49, 50, 66 | `GETANGLE`, `NORMALIZEANGLE`, `COSINE` | trigonometry |
| 62 | `GETINTERSECT` | intersection / collision test |
| 52 | `GETWINDOWSDIRECTORY`, `MAKEFULLPATH` | path resolution |
| 56 | `WVSPRINTF`, `LSTRCPYN`, `WINMALLOC` | string formatting |
| 58 | `LSTRCMP`, `LSTRCMPI` | string comparison |
| 67 | `WRITEPRIVATEPROFILESTRING` | persist a setting to an INI file |
| 5, 25, 26, 28, 36, 39, 40, 42, 55, 63 | `S_xxx` | graphics (JUNGS01 exports) |
| 2, 32 | `Ordinal_8` | the most-called import, 29 sites |

Several things follow from this.

The scripting language has **trigonometry and collision detection as primitives** (`GETANGLE`,
`NORMALIZEANGLE`, `COSINE`, `GETINTERSECT`). Those are gameplay operations, not presentation ones —
further evidence this is a general-purpose game scripting language rather than a display list.

Settings persist through `WRITEPRIVATEPROFILESTRING`, so the options screen writes an INI file. That
matches the constant strings recovered earlier: `MusicOff`, `SoundFXOff`, `SlowCpu`, `LoMem`.

Audio is driven by **posting Windows messages** rather than called synchronously, so sound is
asynchronous with respect to script execution — worth preserving in a port, since a synchronous
implementation would change timing.

## The graphics export surface

Of the `S_xxx` exports the scene opcodes call, **only one reaches GDI**:

| Export | Size | Reaches | Role |
|---|---:|---|---|
| `S_027` | 148 | `SETDIBITSTODEVICE`, `STRETCHDIBITS` | **present / flip** |
| `S_039` | 107 | `LOCALALLOC` | create surface |
| `S_010` | 85 | `LOCALFREE` | destroy surface |
| `S_004`, `S_025`, `S_032`, `S_038`, `S_051`, `S_053`, `S_054`, `S_056`, `S_060`, `S_066` | 25–268 | internal only | software raster operations |

Every drawing export except the presenter is pure software working on an offscreen 8-bit surface.
That is a very small platform boundary: a port needs to reimplement exactly **three** host-dependent
operations — allocate a surface, free it, and present it — and can port the rest as ordinary C with
no graphics API at all.

Opcode 39 calls `S_027`, so it is the script-level present. Opcodes 5 and 26 call the
allocate/free pair.

### The three host operations, in detail

```c
S_039(size, width, height, ctx, env)   // create surface
```
Allocates `size` bytes, releases any existing surface first via `S_010`, initialises the raster
state, then stores the buffer at `ctx+0x32` and its end at `ctx+0x36`. The surface is a **flat byte
buffer with begin and end pointers** — not a structured bitmap object.

```c
S_010(ctx, env)                        // destroy surface
```
Frees the buffer and zeros the field run at `ctx+0x14` through `ctx+0x22`, which is the clip
rectangle and dimensions.

```c
S_027(mode, flag, ctx)                 // present
```
Presents through `FUN_1000_3894`, passing **negated scroll offsets** from `ctx+0x884` and
`ctx+0x886`. So presentation takes a pan offset: the engine supports a viewport that moves over a
surface larger than the window, which matters for any minigame that scrolls.

For a port, that is the whole platform boundary:

1. allocate a flat byte buffer
2. free it
3. present a rectangle of it, at a scroll offset, either 1:1 or scaled

Everything else — clipping, sprite compositing, RLE decode during blit, palette handling — is
already host-independent C once the segmented pointers are flattened.
