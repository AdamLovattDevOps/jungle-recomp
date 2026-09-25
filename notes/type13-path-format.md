# Type 13: JUNGS01 sprite motion/animation program

**Summary.** A type 13 resource is a small **bytecode program** that one sprite
runs. It is not a plain cel list and not a pure motion path. It is a flat stream
of variable-length records, each `u16 opcode` plus operands. The records cover:

- frame display: a cel range, or up to 4 cels composited at once, each held for N ms
- motion: a straight line, a 3-point parabolic arc, and absolute or relative jumps
- timers, flip flags, show/hide, and counted or infinite loops
- embedded scene-script statements that call back into the EXE

JUNGS01 copies the bytes into a private buffer per sprite, and the program
mutates its runtime fields in that copy.

Verified with `tools/type13.py`: **1262/1262 type 13 resources across all 13 .BIN
containers parse with every byte consumed (271,704 bytes, 22,819 records)**, plus
the structural checks listed at the end.

---

## 0. Naming caveat: `S_nnn` = ordinal nnn+1

Both decomps name JUNGS01 exports `S_nnn` with **nnn = NE ordinal − 1**:

- NE entry table: ordinal 1 = `1000:3e6e`, which is `WEP` in the DLL decomp; ordinal 2 = `1000:0024`, which is named `S_001`.
- EXE import relocations inside op 5's handler `FUN_1008_a312` (reloc chains walked from the raw NE): `0xa392 → JUNGS01 ord 26`, `0xa3a0 → ord 11`, `0xa3ff → ord 40`. Ghidra shows these three calls as `S_025`, `S_010` and `S_039`.

The two decomps use the same names, so `S_039` in the EXE is `S_039 @ 1000:4c8c` in
`notes/decomp/JUNGS01.DLL.c`. This is **ordinal 40**, not 39.

| name | ordinal | address | role |
|---|---|---|---|
| S_039 | 40 | 1000:4c8c | load a program (type 13 bytes) into a sprite |
| S_010 | 11 | 1000:4c12 | stop and discard the sprite's program |
| S_025 | 26 | 1000:3970 | hide the sprite (optionally erase it from screen) |

---

## 1. The three exports

The DLL's internal and exported functions are Pascal: arguments are pushed left to right and the callee cleans up (`ret 2`, `ret 0x10`). Ghidra lists the parameters in reverse. The signatures below are in source order.

### S_039(ctx, sprite, const void far *data, WORD len) → BOOL
```c
DAT_1008_0044 = param_5;                                  // ctx
if ((param_3 != 0 || param_2 != 0) && (param_1 != 0)) {   // far ptr non-null, len != 0
  iVar1 = LOCALALLOC(unaff_CS,param_1);                    // private copy, len bytes
  if (iVar1 != 0) {
    S_010(param_4,param_5);                                // drop any old program first
    FUN_1000_6fdc(iVar1,0x1008,param_2,param_3,param_1,...);   // far memcpy
    *(int *)(param_4 + 0x32) = iVar1;                      // program start
    *(int *)(param_4 + 0x36) = iVar1 + param_1;            // program end
    FUN_1000_45b2(param_4);                                // fetch + init first record
    return 1; } }
return 0;
```
- The EXE's op 5 passes exactly the 8-byte type-13 slot `{ptr_lo, ptr_hi, len}`: `S_039(.., puVar8[2], *puVar8, puVar8[1], iVar1)`.
- `S_007` (sprite clone) reuses S_039 to copy one sprite's program to the new sprite: `S_039(param_1[0x1b] - param_1[0x19], param_1[0x19], 0x1008, puVar3, ...)`, i.e. `len = end(+0x36) − start(+0x32)`.
- S_039 **does not start the sprite ticking**. The run flag is sprite `+0x5b`, set by `S_011` (the EXE calls `S_011(1)` after creating the sprite in `FUN_1008_5e9a`).

### S_010(ctx, sprite): stop program
Frees the buffer (`+0x32`). Zeroes the move timer and period (`+0x14`/`+0x1c`), the frame timer and period (`+0x18`/`+0x20`), `+0x32`, `+0x34` (PC), `+0x36`, `+0x58` (catch-up), `+0x54` and `+0x5a` (loop-break). The sprite stays on screen and keeps its current cel. The EXE calls it when op 5 is given no type 13 handle. The interpreter also calls it when the PC runs off the end of the program (`FUN_1000_45b2`: `if (iVar3 == 0) { S_010(param_1,DAT_1008_0044); return false; }`).

### S_025(ctx, sprite, BOOL erase): hide
```c
if ((*(char *)(param_2 + 0x4e) != '\0') && (param_1 != 0)) FUN_1000_3498(0,param_2);  // repaint old rect
*(param_2 + 0x56) = 0;  *(param_2 + 0x4e) = 0;  *(param_2 + 0x57) = 0;               // not visible
```
`+0x4e` is "visible". The program is **not** touched. The EXE calls `S_025(1, sprite)` before installing a new program when the script's flag byte is set, so the old image is erased first. Program opcode 8 is also `S_025(1, …)`.

---

## 2. Execution model

- **Tick.** `S_001` (ordinal 2) walks the context's sprite list. A sprite runs if it has a program (`+0x34 != 0`), is running (`+0x5b`), and is not paused (`+0x5c == 0`). For each such sprite:
  ```c
  do { iVar3 = FUN_1000_4b16(spr);             // execute current record; 1 = finished
       iVar4 = 0; if (iVar3 != 0) iVar4 = FUN_1000_45b2(spr);  // advance + init next
  } while (iVar4 != 0);
  ```
  `FUN_1000_45b2` returns true (keep going in the same tick) only for the instant records 7, 12, 13, 14, 15 and 16. For these it has already run the record and recursed to the next one. Every other record costs at least one tick.
- **Record length.** The table is at DS:0BCE, indexed by opcode and filled in `FUN_1000_4554`. Opcode 20 is the exception: its length is its own `+2` word.
  `FUN_1000_4164`: `piVar2 = (int *)((int)piVar2 + *(int *)(*piVar2 * 2 + 0xbce));` and `if (*piVar2 != 0x14) ...; piVar2 = piVar2 + piVar2[1]`.

  | op | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | 22 |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
  | bytes | 10 | 14 | 2 | 2 | 30 | 8 | 10 | 2 | 22 | 8 | 6 | 8 | 4 | 4 | 8 | 6 | 4 | 4 | 2 | var | 14 | 12 |
- **Termination.** There is no end opcode and no header. `FUN_1000_4164`: `if (*(sprite+0x36) <= piVar2) return 0;`. The program ends when the PC reaches the end of the buffer, and S_010 is called then. Programs that should run forever end in an op 6 with count 0.
- **Clocks.** All times are **milliseconds from `timeGetTime()`**. Each sprite has two independent timers:
  - *frame timer*: due time at `+0x18`, period at `+0x20` (32-bit). Set by S_036, op 14, and the duration field of ops 1 and 2.
  - *move timer*: due time at `+0x14`, period at `+0x1c`. Set by S_041 and op 16.

  When a timer fires, `due = now + period`. If the catch-up flag `+0x58` is set (op 13), `due = previous due + period` instead, so the timer does not drift. Both periods are 0 after S_010, so movement steps then happen once per tick.
- **Value words.** Most u16 operands are resolved through the far table the EXE passes to `S_052`. That table is the script globals, `S_052(0x1018,0x151e,0x1020)` in `FUN_1008_bb7e`:
  ```c
  if (uVar6 < 0x159f) uVar6 = *(uint *)(uVar6 * 2 + *(int *)(DAT_1008_0044 + 0x44c));  // global var
  else                uVar6 = uVar6 + 0x7531;                                          // immediate
  ```
  So `raw < 0x159F` means script global #raw, and `raw ≥ 0x159F` means the immediate `raw − 0x8ACF` (mod 2¹⁶, signed; 0x8ACF = 0). This is the same encoding as script operands and resource handles.
  - Unlike the EXE, the DLL does **not** special-case 0x13FE..0x159E as frame locals. No type 13 operand falls in that range: 60,783 immediates and 15,572 global references were counted, with the highest global index 4531.
  - The fields marked **raw** below bypass this translation.

---

## 3. Sprite fields the program drives (JUNGS01 sprite object, 0x5F bytes)

| off | meaning | evidence |
|---|---|---|
| +0,+2 | anchor x, y (screen px) | `FUN_1000_37ca`: `param_4 = param_4 + *param_5; ... *param_5 = param_4; param_5[1] = param_3;` |
| +4..+0xA | current bounding RECT (l,t,r,b) | `FUN_1000_3498`: `EXCLUDECLIPRECT(.., +10,+8,+6,+4)` |
| +0x28:+0x2A | far ptr to the current cel's bitmap header | `FUN_1000_63ee` |
| +0x30 / +0x40 | cel table `{u16 lo,u16 hi}[]` / count | S_002/S_003/S_009 |
| +0x38 | **z-order** (signed) | `FUN_1000_10c8`: `*(iVar1+0x38)=param_1;` sorted insert |
| +0x3A / +0x3B | flip-X / flip-Y bytes | `FUN_1000_0b30`, op 7 |
| +0x3C | owner tag (type 15 handle) | `S_034`; returned to the EXE with every event |
| +0x44/46/48/4A | current / next / first / last cel of the auto-cycle range | `FUN_1000_6364`, `FUN_1000_63ee` |
| +0x53 / +0x55 | range runs forward / range has >1 cel (auto-cycle on) | `FUN_1000_6364` |
| +0x4c | cel count in composite (op 2) mode, 0 = single-cel mode | `FUN_1000_0d3c` |
| +0x4e | visible | S_025, op 1 |
| +0x5a | "break loops" (op 6 falls through) | set by `S_004` (ord 5) |

**Cels.**
- The cel table comes from the type 15 record. `FUN_1008_5fdc` walks its handle list and passes **only entries of resource type 1, 10 or 16** to `S_003`. Types 8, 7 and 4 are skipped.
- So a cel index in a type 13 program is an index into *that filtered list*, not into the raw type-15 handle array.
- An entry with `hi == 0` is a handle that is loaded lazily: `FUN_1000_606e` → `FUN_1000_5f9e` → EXE callback `FUN_1008_69e4`, registered with `S_059`. That callback resolves type 1 directly. For type 10 it goes through `FUN_1008_5a66`, which copies the bitmap's 20-byte header and overwrites `+0x0A` with type-10 `x` and `+0x0C` with type-10 `y`.

**Placement.** Each cel's bitmap is centred on the anchor and then shifted by its header offsets (`FUN_1000_1278` / `FUN_1000_0b30`):
```c
left = x - ((w-1)/2 - hdr[+0x0A])        // flipX: left = (x - hdr[+0x0A]) - (w-1)/2
top  = y - ((h-1)/2 - hdr[+0x0C])        // flipY: top  = (y - hdr[+0x0C]) - (h-1)/2
```
**Conflict.** The JUNGS01 code uses bitmap `+0x0A` as the **X** offset and `+0x0C` as the **Y** offset, and type 10 (x, y) writes to `+0x0A`/`+0x0C` in that order. `docs/FORMAT.md` labels them the other way round (`0x0A origin Y, 0x0C origin X`). The DLL treats +0x0A as X: it is paired with `*param_1` (x) and the left/right edges, and it is the one negated by flip-X. **Check this against the renderer in src/ before trusting either label.**

**Z-order.** It comes from type 15 `+0x0C`, not from type 13. The EXE value-decodes it and creates the sprite with it: `iVar3 = S_009(unaff_CS,0,0,iVar3)`, then `FUN_1000_10c8(param_3)` stores it at `+0x38`. The sprite is inserted into the ascending list after the last sprite whose z ≤ its own (`if (*(int *)(*piVar3 + 0x38) <= param_1) break;`). The compositor `FUN_1000_3356` paints the list in index order, so **higher z is drawn on top**. Observed values include 29000 (421 sprites), 30000, 28000, −1000, −1, 0 and 10, which confirms a signed i16 layer value. `docs/FORMAT.md` currently calls type 15 `+0x0C` a "u32 handle". It is a value-encoded z.

---

## 4. Record reference

**Conventions.**
- `v` = value word (§2); `raw` = used untranslated; `u8` = byte; `rt` = runtime scratch, **zero in every file** (19,358 of 19,358 checked).
- "exec" is `FUN_1000_4b16` (runs every tick until it returns 1). "init" is the `FUN_1000_45b2` case, which runs once when the PC arrives.
- The duration fields of ops 1 and 2 hold the **raw value 0xFFFF** to mean "keep the current frame period", checked before translation (`if (*(int *)(iVar4 + 8) != -1)`).

### op 1: PLAY_RANGE (10 bytes, 8,543 records)
`+2 v first, +4 v last, +6 rt count, +8 v ms`
- init: `count = FUN_1000_6364(last, first, start=first)`, which sets `+0x48=first, +0x4a=last, +0x46=first`, direction forward if first ≤ last, and returns `|last−first|+1`, or 0 if either index ≥ the cel count. The decomp reads `+2` twice (`local_6 = +2; local_a = +2; local_8 = +4`), and the disassembly at 1000:4616/464f/467d confirms the second read is real, so start = first.
- exec (`FUN_1000_35cc`): wait until the frame timer is due. Then: if `count != 0`, set the period from `+8`, re-arm the timer, set `+0x51` (advance cel) and make the sprite visible (`+0x4e=1`), redraw (`FUN_1000_36a2` → `FUN_1000_63ee`, which steps cur ← next and next ± 1 with wrap to first), and decrement `count`.
- The op finishes when count reaches 0 **and** the timer is next due, so the last cel is held for its full duration too. `ms == 0` finishes without waiting.
- Effect: shows cels first..last (or last..first, reversed) in order, **each for `ms` milliseconds**. The common idiom is `first == last`, meaning "show cel N for ms" (e.g. `JUNGBURP[781]`: cels 3..15 at 83/125/42 ms).

### op 2: SHOW_CELS, a composite frame (14 bytes, 9,707 records)
`+2 v cel0, +4 v cel1, +6 v cel2, +8 v cel3, +0xA v ms, +0xC rt shown(u8), +0xD u8 ncels`
- exec (`FUN_1000_0d3c`): once the frame timer is due and the frame has not yet been shown, set the period from `+0xA` and re-arm the timer. For each of the first `ncels` slots, value-decode the index: `0xFFFF` means an empty slot (`if (uVar6 != 0xffff)`), and an index ≥ the cel count aborts the op. Resolve each cel through the +0x30 table into a 4-entry cache at `+0x2e` (12 bytes each: far ptr + rect). Set `+0x4c = number of cels`, then compute each cel's rect with the placement rule in §3 (`FUN_1000_0b30`). If the same cels are already up, only the changed ones are redrawn. Finally set `shown=1`.
- It returns 0 (keep waiting) while `ms != 0`, and 1 on the next due tick. So the frame is held for `ms`.
- All cels share the sprite anchor, and each is offset by its own bitmap/type-10 origin. `ncels` is 1..4 in all 9,707 records. It is not range-checked in code; the local array holds 4.
- Example: `JUNGMAIN[72]` is 30 frames of 3 layered cels (0,1,2 / 3,4,5 / 6,7,8 …) at 83 ms, then `LOOP forever → 0`.

### op 5: ARC, a quadratic path through 3 points (30 bytes, 16 records)
`+2 v steps, +4 u8 relative, +5 u8 (0), +6 v midX, +8 v midY, +0xA v endX, +0xC v endY, +0xE..+0x1D rt {x0,y0,midX,midY,endX,endY,i,n}`
- init: `x0,y0 = sprite pos` (`*(iVar3+0xe) = *param_1; *(iVar3+0x10) = param_1[1];`). Decode mid/end, and add the sprite position to them if `relative` is set. `i=0`, `n=steps`.
- exec (`FUN_1000_1868`): on each move-timer period, `FUN_1000_1810` does `++i` and `pos = FUN_1000_1754(...)`, then moves the sprite to it absolutely (`FUN_1000_37ca(1,0,y,x)`). It finishes when `i ≥ n`. If the cel range has more than one cel (`+0x55`), cels also advance on the frame timer while the sprite moves.
- Curve, from the disassembly at 1000:1754: `t = (i*1024 + n/2) / n`, and
  `x = x0 + ((t*(1024−t))*(4*(midX−x0) − (endX−x0)) + t²*(endX−x0)) >> 20`, with the same formula for y.

  This is a quadratic Bézier that passes through `mid` at t=½ and ends exactly at `end` when i=n. It is used for leaps.

### op 6: LOOP (8 bytes, 424 records)
`+2 raw counter (rt, file = count), +4 raw count, +6 raw target` (byte offset from program start)
- Handled inside the PC-advance routine `FUN_1000_4164`, so it never executes as a record:
  ```c
  if ((*(char *)(param_1 + 0x5a) != '\0') || (piVar2[1] == 1)) { piVar2[1] = piVar2[2]; /* fall through */ }
  else { if (piVar2[1] != 0) piVar2[1]--;  piVar2 = start + piVar2[3]; }   // jump
  ```
- `count = N` runs the body **N times** in total (N−1 jumps), then resets the counter so the loop can be entered again. `count = 0` loops forever. The loop-break flag `+0x5a` (S_004) forces a fall-through.
- On disk `counter == count` in 424/424 records, and the target is a record boundary at or before the op in 424/424.

### op 7: FLIP_OFFSET (10 bytes, 76), instant
`+2 v flipX, +4 v flipY, +6 v dx, +8 v dy`
- `FUN_1000_41d6`: `+0x3a = CONCAT11(flipY!=0, flipX!=0)`. If dx or dy is non-zero, the sprite moves **relatively** (`FUN_1000_37ca(0,1,dy,dx)`). The sprite is redrawn if anything changed.
- The flip bytes mirror the cel origin (§3) and are passed to the blitters (`FUN_1000_2848(*(+0x3a), …)`).

### op 8: HIDE (2 bytes, 377)
`S_025(1, sprite)`: erase and mark invisible. The program **continues**, and the next op 1/2 makes the sprite visible again (e.g. `JUNGBUGD[242]` blinks: show 333 ms ×4, show 1000 ms, HIDE, WAIT 125, …).

### op 9: MOVE_TO, a straight line (22 bytes, 97)
`+2 v stepsOrSpeed, +4 u8 speedMode, +5 u8 relative, +6 v x, +8 v y, +0xA..+0x15 rt {x0,y0,X,Y,i,n}`
- init: `x0,y0 = sprite pos`, `X,Y = x,y` (plus the sprite position if `relative`), `n = stepsOrSpeed`. If `speedMode` is set, `FUN_1000_44e8` replaces n with `ceil(max(|X−x0|,|Y−y0|) / stepsOrSpeed)`, so the value becomes **pixels per step**.
- exec (`FUN_1000_3f34` → `FUN_1000_3ee2`): one step per move-timer period, `pos = start + round(delta*i/n)` (`FUN_1000_3e82`, rounding half away from zero). It finishes at `i ≥ n`, and cels auto-cycle as in op 5.

### op 10: JUMP_POS (8 bytes, 10)
`+2 raw relative, +4 v x, +6 v y`: `FUN_1000_4022` waits for the next move-timer period, then does `FUN_1000_37ca(1, relative, y, x)` and finishes. It auto-cycles cels while it waits.

### op 11: WAIT (6 bytes, 394)
`+2 v ms, +4 rt started`: `FUN_1000_62c6` arms the frame timer for `ms` on the first exec and finishes when it is due. On disk the values are 125, 1000, 0 and so on, so the unit is plainly ms.

### op 12: SET_POS (8 bytes, 387), instant
`+2 v x, +4 v y, +6 u8 relative, +7 pad`: `FUN_1000_4324`: `FUN_1000_37ca(0, *(u8*)(rec+6), y, x)`.

### op 13: SET_CATCHUP (4 bytes, 221), instant
`+2 u8 flag`: `*(sprite+0x58) = rec[2]`, and both timers are zeroed. With the flag set, frame and move timers advance from their previous due time rather than from now.

### op 14: SET_FRAME_MS (4 bytes, 23), instant
`+2 v ms`: `S_036(ms, 0, sprite)`, which sets the frame period and re-arms both timers from now.

### op 15: SET_RANGE (8 bytes, 63), instant
`+2 v start, +4 v first, +6 v last`: `FUN_1000_6364(last, first, start)`. This sets the auto-cycle range that ops 5, 9 and 10 animate through while moving.

### op 16: SET_MOVE_MS (6 bytes, 83), instant
`+2 v moveMs, +4 v frameMs (raw 0 = leave unchanged)`: `S_041(moveMs, 0)`; then `if (*(int *)(rec+4) != 0)` the raw value is decoded and passed to `S_036(frameMs)`.

### op 17: SHOW_CEL (4 bytes, 58)
`+2 raw cel`: `S_043(1, cel, sprite)`, which sets the next cel to `cel` if it is < the count and redraws. It also marks the sprite visible. On disk the value is always `0xFFFF`, meaning "no index change, just make the current cel visible".

### op 20: SCRIPT (variable length, 2,339 records)
`+2 raw len (whole record), +4.. one type-14 scene-script statement`
- exec: `FUN_1000_43be(len, sprite)` copies the record into the context buffer and calls the EXE callback registered by `S_035`, which is `FUN_1008_c59e`. That callback stores the sprite's owner tag in script global 0x1399 (`*(int *)0x3c50 = …`) and, for record type 0x14, runs `FUN_1008_c724(…, rec+4, …)`, the **type-14 statement interpreter**.
- **2339/2339** payloads are exactly one statement by `script_dis.record_len`. The mix is outer opcode 1 ×1331 (`FUN_1008_c63e`, a call with args), 5 ×316, 16 ×254, 8 ×190, 24 ×78, 3 ×72, and others.
- These are used for sound cues and game logic synchronised to animation frames.
- The callback is skipped entirely while script global 0 at `DAT_1020_151c` is non-zero.

### op 22: EXT_FRAMES (12 bytes, 1 record: `JUNGLE.BIN[87]`)
`+2 v handle (→ JUNGLE.BIN[52], type 8), +4 rt handle, +6/+8 rt far ptr, +0xA rt new-frame flag`
- `FUN_1000_4d86` → `FUN_1000_4d24` calls the EXE callback `S_050` = `FUN_1008_807c`. That callback runs its own timer and writes a bitmap pointer to `+6/+8` and a "changed" flag to `+0xA`. The DLL then uses that pointer as the sprite's cel, placed by the same centring rule.
- **Uncertain**: it looks like an externally generated frame source (type 8 is not otherwise decoded), and its completion condition was not traced.

### Opcodes that have a length but never occur on disk
| op | behaviour |
|---|---|
| 3 | no handler (no-op) |
| 4 | `FUN_1000_042a`: stamp the current image into the background surface, then hide without erase and `S_010` |
| 18 | no handler in either switch (no-op) |
| 19 | EXE callback posts window message `0x4C8` |
| 21 | EXE `FUN_1008_c4a6` |

The EXE callback also receives a synthetic `{0x12, tag}` event from `FUN_1000_4482` whenever a sprite with `+0x50` set changes image (→ `FUN_1008_e6e6(tag)`).

---

## 5. Relation to types 1 / 10 / 15

- **Frames.** Type 13 never contains resource handles for cels, only **indices** into the sprite's cel table. That table is built from the type-15 handle list, keeping only entries of type 1, 10 and 16. Op 22's `+2` is the only resource handle in the format; op 20 statements carry their own script handles.
- **Position.** The sprite anchor is set by ops 5, 7, 9, 10 and 12, and by the EXE (`S_051` translates the sprite and shifts the targets of an op 5 or op 9 that is still in flight). Each cel is drawn centred on the anchor, plus the bitmap's `+0x0A`/`+0x0C` offsets, which a type 10 overrides.
- **Z.** It comes from type 15 `+0x0C`, fixed when the sprite is created.
- **Timing.** Every duration is in ms (`timeGetTime`). Values like 42/83/125/333/1000 fit 12/24 fps multiples.

## 6. Verification (`python3 tools/type13.py`)
```
type 13 resources parsed cleanly: 1262/1262  (271704 bytes, every byte consumed)
  JUNGBUGD 113/113  JUNGBURP 60/60  JUNGCRED 2/2  JUNGHIPP 349/349  JUNGINT1 7/7
  JUNGINT2 8/8  JUNGLE 3/3  JUNGMAIN 168/168  JUNGOPTS 14/14  JUNGPINB 159/159
  JUNGPRTY 141/141  JUNGSCOR 39/39  JUNGSHOT 199/199
records: 1:8543 2:9707 5:16 6:424 7:76 8:377 9:97 10:10 11:394 12:387 13:221
         14:23 15:63 16:83 17:58 20:2339 22:1
checks: runtime fields zero 19358/19358; loop target on record boundary 424/424;
        loop target backwards 424/424; loop counter==count 424/424;
        op2 ncels in 1..4 9707/9707; op20 payload == one type-14 statement 2339/2339
```
`python3 tools/type13.py FILE.BIN INDEX` pretty-prints one program with its values decoded (`gN` = script global N).

## 7. Open / uncertain
- The bitmap `+0x0A`/`+0x0C` X/Y labelling conflicts with docs/FORMAT.md (§3). The DLL evidence says +0x0A = X.
- Op 22 semantics and its type 8 source are only partly traced.
- Ops 3, 4, 18, 19 and 21 are unused on disc and were described from code only.
- The DLL's value decoding reads `table[raw]` for every raw < 0x159F, including 0x13FE..0x159E, which the EXE treats as frame locals. This is harmless on the shipped data, where no operand falls in that range.
- Which program runs is chosen by the EXE (script op 5 → `FUN_1008_a312`). S_039 alone does not set `+0x5b`, so the sprite must already be running.
