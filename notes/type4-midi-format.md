# Type 4 resources: 7th Level MIDI event lists

Type 4 resources hold the game music. Each one is a flat list of `midiOutShortMsg`
dwords with delay entries mixed in. It is not SMF, has no running status and uses no
meta or sysex events. Timing is in milliseconds, and each delay arms a one-shot
`timeSetEvent` timer.

Tool: `tools/type4_midi.py` (`--verify`, `FILE.BIN INDEX [-o out.mid]`, `--all [-o DIR]`).

All addresses below are `seg:offset`. JUNGA01 seg1 = `1000`, JUNGR01 seg1 = `1000`,
JUNGLE.EXE seg2 = `1008`. The disassembly comes from `tools/disasm.py` on `reference/*/seg*.bin`.

## 1. Loading path

EXE `FUN_1008_5cf6` (the type-4 case of `FUN_1008_669c`) calls JUNGR01 ordinal 28 =
`RESCREATEMIDIEVENT` (1:0132, per the JUNGR01 entry table). It caches the returned
handle in the resource slot.

`RESCREATEMIDIEVENT` (JUNGR01 1000:0132):

```c
lVar2 = RESSEEKRESOURCE(param_1,param_3);
if (((lVar2 != 0) && (iVar1 = FUN_1000_0a80(4,0,local_22,unaff_SS,param_3), iVar1 != 0)) &&   // read 4-byte header
   (local_24 = FUN_1000_0010(param_3,uVar3), local_24 != 0)) {                                  // stream-state {file, pos...}
  local_30 = 0x72;  ...  // AUDIO_READ   1000:0072
  local_2c = 0x106; ...  // AUDIO_REWIND 1000:0106
  local_28 = 0x5a;  ...  // AUDIO_DONE   1000:005a
  uVar3 = A_010(...);    // JUNGR01 import JUNGA01.10 = 1:1546 (verified via NE entry table)
```

The disassembly shows the push order `push [bp+8]` (ctx), `&callbacks`, `&header` → `A_010(ctx, lpCallbacks, lpHeader)`.
The callbacks struct is `{read far*, rewind far*, done far*, user}`.

JUNGA01 `A_010` → `FUN_1000_1304` allocates the per-track object with `LocalAlloc(0x40 LMEM_ZEROINIT, 0x40)`
(`push 0x40 / push 0x40` at 1000:1316). There are at most 20 objects (`cmp [bx+0x193],0x14`).
`FUN_1000_1414` then fills it in:

```
1000:1455  mov ax, es:[bx+2]      ; header word 1
1000:1459  mov [si+0x1e], ax      ;   -> obj+0x1e (ID, posted back on completion)
1000:1471  mov ax, es:[di]        ; header word 0 = byte length
1000:1476  shr ax, 2
1000:1479  mov [bx+6], ax         ;   -> obj+6 = event count
1000:147c  push 0 / push si / push 0 ; WINMALLOC(len)
1000:14a5  lcall [bx+0x10]        ; read callback: read `len` bytes into the buffer
1000:14a8  cmp ax, si / jne fail  ; must return exactly len
1000:14b3  call 0x14cc            ; immediately calls the DONE callback (stream closed)
```

**The whole event array is read into memory when the track is created.** Nothing is
streamed during playback. On all 9 resources on the disc, `byteLen + 4 == directory size`.
The data is stored raw. JUNGR01's `FUN_1000_0a80` has an alternate read path, taken when
the file flag `+0x116` is set, but the plain data proves it is not used here.

## 2. Byte format

```
+0  u16  byteLen     event bytes; event count = byteLen >> 2 (obj+6)
+2  u16  id          opaque; stored at obj+0x1e, sent as wParam of msg 0x52c at natural end
+4  u32  event[count]
```

Observed values of `id`: HIPP 859/860 = 0x8e2a/0x8e2b, BURP 655/656 = 0x8d5e/0x8d5f,
PINB 627/628 = 0x8d42/0x8d43, SHOT 2812/2813 = 0x95cb/0x95cc, BUGD 2783 = 0x95ae.
Each container's pair is consecutive. Its meaning beyond "echoed in the completion message" is not
established. It is **not** the local resource index. The value minus the EXE's 0x7531 literal bias
exceeds some containers' entry counts.

### Event dispatch (sequencer `FUN_1000_1b20`)

```
1000:1b29  mov ax,[si+6] / cmp [si+4],ax / jb     ; index < count ?
1000:1b31  push si / call 0x1aea                  ;   else end-of-list handling (loop/stop)
1000:1b3c  les di,[si]                            ; event buffer
1000:1b3e  mov bx,[si+4] / shl bx,2
1000:1b44  mov ax,es:[bx+di] / mov dx,es:[bx+di+2]; ax = low word, dx = high word
1000:1b51  inc word [si+4]
1000:1b54  test word [bp-2], 0xf000               ; high word & 0xF000 ?
1000:1b59  je 0x1b92                              ;   zero -> MIDI message
```

**MIDI message** (high word & 0xF000 == 0):

```
1000:1b92  mov al,[bp-4] / and al,0xf / cmp al,9 / jne   ; channel 10?
1000:1b9b  cmp word [0x7b2],0 / je
1000:1ba2  or byte [bp-4],0xf                            ;   RemapMidiDrums: ch10 -> ch16
1000:1bac  cmp byte [bx+si+0x30],0 / je send             ; channel muted?
1000:1bb5  and al,0xf0 / cmp al,0x90 / jne send          ;   muted: drop only note-on (0x9n)
1000:1bc2  push word [bx+0x18d]                          ; hMidiOut
1000:1bc6  push word [bp-2] / push word [bp-4]           ; dwMsg = the event dword as-is
1000:1bcc  lcall MIDIOUTSHORTMSG                         ; loop straight to next event
```

The dword is `status | data1<<8 | data2<<16 | 0<<24`. Every non-delay event has an explicit status
byte. The player does not check the status at all. It forwards anything that is not a delay.
Across the disc, only the status classes 0x8n, 0x9n, 0xBn (CC 1, 7, 10) and 0xCn are present. Note-offs are
always 0x8n. Note-on and note-off counts match exactly in every track.

**Delay** (high word & 0xF000 != 0). Bit 31 is the flag. The encoding is always `ms | 0x80000000`:

```
1000:1b5b  mov ax,[bp-4] / mov dx,[bp-2]
1000:1b61  and dh,0x7f                  ; delay = dword & 0x7FFFFFFF
1000:1b64  mov [bp-8],ax
1000:1b67  cmp word [si+0x24],0 / je 1b88
1000:1b6d  push 0/[si+0x24]/0/[si+0x22]/dx/ax
1000:1b7b  call 0x42ae                  ; lmul: delay * obj+0x22
1000:1b82  call 0x42e0                  ; uldiv: / obj+0x24     (only if obj+0x24 != 0)
1000:1b88  mov ax,dx / or ax,[bp-8] / je 0x1b29   ; zero delay -> next event immediately
1000:1bd4  or dx,dx / jne
1000:1bd8  cmp word [bp-8],3 / jae / mov word [bp-8],3   ; 1..2 ms -> 3 ms
1000:1be3  push word [bp-8]             ; uDelay   (LOW WORD ONLY)
1000:1be6  push 1                       ; uResolution = 1 ms
1000:1be8  push 0xffff / push 0x1c32    ; lpFunction = A_040 (1000:1c32, segment relocated)
1000:1bee  push 0 / push 0              ; dwUser = 0
1000:1bf2  push 0                       ; fuEvent = TIME_ONESHOT
1000:1bf4  lcall TIMESETEVENT
1000:1bfd  mov [bx+0x191], ax           ; timer id; return (resume in callback)
```

The timer callback `A_040` (1000:1c32, `retf 0x10` = the standard 16-byte `TIMECALLBACK` frame) clears the
timer id and calls `FUN_1000_1b20(current)` again.

### Timing / tempo

- **Units are milliseconds** (`timeSetEvent` uDelay). No tempo field exists in the data or the header.
- The default is unscaled. The object comes from `LMEM_ZEROINIT`, so `obj+0x24 == 0` and the scale is skipped. The only
  writer is `A_032` (see §4), which is reached only from script op 92 (0x5c, EXE `FUN_1008_982a`).
  **No script on the disc uses op 92**, so in practice delays are raw ms.
- Scheduling is a chain of one-shot timers. Each delay is timed from when the previous callback ran.
  No `timeGetTime` is involved and nothing corrects for drift, so the real playback can run slightly longer than the
  nominal sum. (`TIMEGETTIME` appears only in init, `FUN_1000_0c56`, as a seed.)
- **1–2 ms delays are raised to 3 ms.** Every track uses 2 ms delays heavily except PINB, whose
  minimum is 3. The disc has no delays of 0, and none above 777 ms, so the 16-bit truncation never triggers.
- A delay of 0 (none on the disc) would fall through without arming a timer.

### End / looping (`FUN_1000_1aea`)

```
1000:1af1  cmp word [si+0x26],0 / jne
1000:1af8  call 0x194c                  ; count==0: post completion (if requested)
1000:1b01  mov [bx+0x179],0 ; return 0  ;   current track = none; sequencer returns
1000:1b0a  cmp word [si+0x26],0 / jle   ; count<0: infinite
1000:1b10  dec word [si+0x26]           ; count>0: decrement
1000:1b13  mov word [si+4],0 ; return 1 ; rewind to event 0, continue at once
```

- There is **no end marker**. The end is `index == byteLen/4`.
- Loop count L (`obj+0x26`): L = 0 plays once. L = N > 0 plays N+1 times. L < 0 (script uses 0xFFFF) loops forever.
- The loop restart adds no gap. The loop period is the sum of the delays. Every track on the disc ends
  with a MIDI message, not a delay, so the last event and event 0 fire in the same callback.
- Natural end sends no all-notes-off. The data leaves no hanging notes (checked: 0 in every track).

### Completion message (`FUN_1000_194c`)

```
1000:1953  cmp word [si+0x2c],0 / je    ; notify requested?
1000:195d  cmp word [bx],0 / je         ; ctx+0 = hwnd
1000:1962  mov word [si+0x2c],0         ; one-shot
1000:196b  push word [bx]               ; hWnd   = ctx+0
1000:196d  push 0x52c                   ; msg    = 0x52C
1000:1970  push word [si+0x1e]          ; wParam = header id
1000:1973  push 0 / push word [si+0x20] ; lParam = notify value from A_028 params
1000:1978  lcall POSTMESSAGE
```

This is only reached on a natural end. `FUN_1000_1982` (stop) clears `obj+0x2c` **before** calling `194c`,
so an explicit stop, or starting another track, never posts. The EXE window procedure
(`FUN_1008_f1c8`, `param_4 == 0x52c`) handles the message. With a zero value it calls
`FUN_1008_64b8(type, id)`, a per-type release. Otherwise it queues a script event through `FUN_1008_ea92`.
Which Ghidra parameter maps to which of wParam/lParam there is **not verified** (the order is reversed).

## 3. Script op 80 (0x50) → A_028, op 81 (0x51) → A_038

The EXE import at `1008:95ef` is `JUNGA01.28` = 1:19ce, and the one at `1008:9e90` is `JUNGA01.38` = 1:1ab6. Both were checked with
`ne_imports.py` and the JUNGA01 entry table, so the names are correct (not off by one).

Op record layout (`RT_017` builds it as `{0x50, res, p[0], p[1], p[2]}`). The script dumps show bytes from +2:

| off | field |
|---|---|
| +2 | track: script var/literal → resource slot → JUNGA01 handle |
| +4 | loop count (0 = once, 0xFFFF = forever, N = N extra passes) |
| +6 | low byte: post completion message? |
| +8 | completion value (var/literal, de-biased by 0x7531) → lParam |

`FUN_1008_952a`: `669c(4,res)` makes sure the track is created. It copies +4..+9 into a local 6-byte struct and
replaces word 3 with the resolved value of +8. Then it pushes `[0xe1c]` (ctx; it is left on the stack by the
`666a` call sequence), the handle and `&struct` → `A_028(ctx, handle, lpParams)`.

`A_028` (1000:19ce): `lpParams == NULL` selects the zero default at DS:0a1e (BSS: once, no notify).
Then `FUN_1000_18aa`:

```
1000:18b8  mov ax,es:[di] / mov [si+0x28],ax / mov [si+0x26],ax   ; loop count
1000:18c1  cmp byte es:[di+2],1 / sbb / inc / mov [si+0x2c],ax    ; notify = byte!=0
1000:18d3  mov ax,es:[di+4] -> [si+0x20]                          ; value (0 if !notify)
1000:18df  mov word [si+4],0                                      ; rewind
1000:18ef  push [bx+0x179] / call 0x1982                          ; stop whatever is playing
1000:18f6  call 0x16fe                                            ; open MIDI out if needed, notes off + reset, MidiVolume
1000:1901  mov [bx+0x179],si                                      ; this is now the current track
1000:1909  cmp word [bx+4],0 / jne / push si / call 0x1b20        ; start now unless ctx paused
```

- Only one type-4 track plays at a time (`ctx+0x179`). Play always starts from event 0.
- There is no volume or fade parameter. Volume comes from the track's own CC7 messages. Device volume is
  set with `midiOutSetVolume` from `7thlevel.ini [audio] MidiVolume` (0 = leave alone), and set to 0
  while sound is off (`A_023` → `FUN_1000_16c8` → `A_033`).
- Script usage seen: `op 80 fe13 0000 0100 ff13` (once, notify with var 0x13ff), `op 80 fe13 0000 0000 0000`
  (once, silent), `op 80 fe13/b80d ffff 0000 0000` (loop forever).

`A_038(ctx, handle)` (1000:1ab6): handle 0 means the current track. `FUN_1000_1982`: clear notify, and if it is the current track:
`FUN_1000_1c08` (current = 0, `timeKillEvent`), then `FUN_1000_1796`, which sends `B0+ch 7B 00` (All Notes Off) on all 16
channels and calls `midiOutReset`. Op 81 in the scripts is always `op 81 0000` (stop current).

## 4. Other controls (not reached by script data on this disc)

- `A_032(ctx, handle, lp)` (op 92): the struct is `{u8 setMute, u8 setTempo, u16 chMask, u16 muteBits, u16 den, u16 num}`.
  It sets `obj+0x24 = den` and `obj+0x22 = num` (delay' = delay*num/den). For each channel set in chMask it sets
  `obj+0x30+ch` from muteBits and sends All Notes Off when a channel becomes muted.
- `A_006(ctx, handle)`: sets the loop count to 0, so the track finishes the current pass and then stops.
- `A_025(flag, ctx)`: pause/resume (`ctx+4`). Pause kills the timer and silences. Resume calls `1b20` at the
  current index, which drops the rest of the pending delay.
- `A_005(active, ctx)`: when the app deactivates it closes the MIDI device. When it reactivates it **restarts the current
  track from event 0** (`mov [obj+4],0` then `1b20`).
- `RemapMidiDrums` (`7thlevel.ini [audio]`, default 0) moves channel 10 → 16 (`FUN_1000_0c78`: `DAT_07b2 =
  GetPrivateProfileInt("audio","RemapMidiDrums",0,"7thlevel.ini")`).

## 5. Disc survey (`tools/type4_midi.py --verify`): 9/9 clean

| res | events (msg/delay) | nominal s (3 ms clamp) | channels | programs (1-based ch: GM 0-based prog) |
|---|---|---|---|---|
| JUNGHIPP 859 | 11793 (7849/3944) | 120.68 | 1,2,3,4,10 | 1:32,35 2:7 3:68,74 4:65 |
| JUNGHIPP 860 | 10836 (7212/3624) | 109.70 | 1,2,3,4,10 | same as 859 |
| JUNGBURP 655 | 11829 (7065/4764) | 165.04 | 1,3,5-10 | 1:32 3:84 5:108 6:28 7:75 8:6 9:84 |
| JUNGBURP 656 | 4486 (2683/1803) | 60.07 | 1,3-10 | 1:32 3:84 4:29 5:108 6:28 7:75 8:6 9:76 |
| JUNGPINB 627 | 9536 (6920/2616) | 89.59 | 1,2,3,4,10 | 1:32,35 2:0,7,80 3:108 4:72 |
| JUNGPINB 628 | 10008 (7400/2608) | 96.38 | 1,2,3,4,10 | same as 627 |
| JUNGSHOT 2812 | 8418 (4657/3761) | 98.95 | 1-5,7-10 | 1:34 2:108 3:115 4:6 5:12 7:28 8:75 9:54 |
| JUNGSHOT 2813 | 9545 (5367/4178) | 126.89 | 1-6,10 | 1:34 2:108 3:115 4:6 5:75 6:54 |
| JUNGBUGD 2783 | 7222 (4373/2849) | 103.16 | 1,6,8,9,10 | 1:32 6:6 8:13 9:12 |

"Clean" means: `byteLen+4 == size`, `byteLen % 4 == 0`, every non-delay has status 0x80–0xEF with
7-bit data and a zero 4th byte, and every delay fits 16 bits. Without the 3 ms clamp the totals are
0–0.7 s shorter (for example SHOT 2812 is 98.39 s raw).

## 6. SMF conversion

`type4_midi.py` writes format 0 with PPQ 500 and tempo 500000 µs, so 1 tick = 1 ms exactly. It applies the
player's 1–2 → 3 ms clamp by default (`--no-clamp` turns it off), keeps any trailing delay before End-of-Track, and has `--loops N`
(unrolled, with marker meta per pass) and `--remap-drums`.

## Uncertainties

- The meaning of header word 1 (`id`). It is echoed as the 0x52c wParam; the EXE's use of it is only partly traced.
- Which of wParam and lParam the EXE 0x52c handler treats as the "zero → release resource" test (Ghidra reverses the parameter order there).
- Real-hardware timing: the chained one-shot timers add per-event latency (callback overhead plus
  timer granularity). The nominal ms sums above are a lower bound on wall-clock length.
- Whether the per-game DLLs call the EXE export `RT_017`/`RT_018` (the op 80/81 wrappers) with other params
  was not checked.
- RemapMidiDrums is assumed to be 0 (default). If a user's 7thlevel.ini sets it, drums go to channel 16.
