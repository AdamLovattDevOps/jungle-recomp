# Preserving the gaming experience

A port can decode every asset correctly and still feel wrong. This file records the behaviours that
decide whether it feels like the original, each tied to evidence in the binary rather than to taste.

Everything here is recovered from named imports (`notes/callmap-*.txt`, produced by
`tools/ne_callmap.py`) and the decompiled consumers.

## Timing

**The engine polls a free-running millisecond clock. It never uses Windows timers.**

`timeGetTime` appears at **13 call sites** in `JUNGLE.EXE`. `SetTimer`, `KillTimer` and `WM_TIMER`
appear **nowhere in any of the five modules**. So no part of the pacing depends on Windows message
delivery, message-queue latency, or timer coalescing.

That is the best possible news for a port: `SDL_GetTicks` is an exact substitute, and nothing about
the original's feel is tied to an OS behaviour that cannot be reproduced.

**Repeating timers accumulate. This is the detail that matters most.**

`FUN_1008_df36` services a table of active timers. When one is due and repeating, it rearms as:

```
deadline = deadline + interval        /* NOT now + interval */
```

The engine advances from the *previous deadline*, not from the current time. A frame that runs late
is caught up on the next pass rather than absorbed. Rearm from `now` instead and the game runs
progressively slower under load while every individual number still looks correct — the classic way
a port ends up feeling sluggish with no obvious bug.

`src/timing.c` implements this, and `tests/timing_test.c` pins it: a timer serviced 185 ms late
still lands on its original grid, and a 33 ms timer serviced every 37 ms for 100 passes stays
aligned to its start.

**One fire per service pass.** `df36` uses a single `if`, not a `while`. A very late service does
not replay every missed tick, it fires once and keeps its place. Replaying them would produce
animation lurches the original never had.

**Eight slots, keyed by id.** `FUN_1008_debe` scans the live entries for a matching id and
overwrites in place, appending only when the id is new, with a ceiling of 8. Re-arming an existing
timer does not consume a slot.

**Intervals are plain milliseconds.** Observed literal: `FUN_1008_debe(0, 0x4fae, 200, 0, id)` — a
200 ms one-shot.

## Palette

**Palette animation is real and must be preserved.** `ANIMATEPALETTE` (`GDI.367`) is called at
`seg1:5199`, alongside `CREATEPALETTE`, `SELECTPALETTE`, `REALIZEPALETTE` and
`GETSYSTEMPALETTEENTRIES`.

`AnimatePalette` rewrites palette entries *without redrawing anything*. In a 256-colour engine that
is how you get cycling water, flickering fire, pulsing highlights — effects that cost nothing and
are invisible to an asset extractor, because the bitmaps never change. A port that expands each
bitmap to RGBA at load time silently loses all of it.

So the port must keep the framebuffer **palette-indexed** and apply the palette at present time,
which is what `src/blit.c` and `--compose` already do. The palette is a live object, not a load-time
lookup table.

The container header also carries `maxFadeColors` (`0x10a`) and `maxTransColors` (`0x10c`), reached
by `RESGETMAXFADECOLORS` and `RESGETMAXTRANSCOLORS`, so the palette is partitioned: some entries are
reserved for fades and some for transparency effects. Fades are therefore also palette operations
rather than per-pixel blends.

## Presentation

`FUN_1000_3894` in `JUNGS01` is the single present path, and its calls are now named:

```
GDI.443  SETDIBITSTODEVICE     1:1
GDI.439  STRETCHDIBITS         scaled
```

The engine composites every sprite into one off-screen 8-bit DIB with its own software blitter, then
hands GDI the whole DIB once per frame. **There is no per-sprite GDI call anywhere on the path.**

Two consequences for the port:

- The SDL2 shape is exact — composite into the indexed surface, expand through the live palette into
  a streaming texture, present once per frame.
- **Scaling is native to the engine, not a retrofit.** `StretchDIBits` is already on the path, so
  resolution independence works with the original's grain rather than against it.

`JUNGS01`'s only other GDI use is clipping: `SAVEDC`, `EXCLUDECLIPRECT`, `RESTOREDC`, with
`OFFSETRECT` and `INTERSECTRECT` for rectangle maths. All trivially portable.

## Audio

The engine streams PCM itself through `waveOutOpen` / `waveOutPrepareHeader` / `waveOutWrite`, and
drives `waveOutPause`, `waveOutRestart`, `waveOutReset`, `waveOutGetVolume` and `waveOutSetVolume`.
Format is 22050 Hz, mono, 16-bit, ADPCM-compressed in the container and already decoded.

`RESCREATEWAVEEVENT` and `RESCREATEMIDIEVENT` mean sound is **scheduled as events against the same
clock as everything else**, not fired ad hoc. Music is MIDI, so it is synthesised, not streamed —
which is a fidelity decision the port has to make explicitly, because the original's sound depends
on whatever synth the machine had. Reproducing "the" sound is not well defined; the honest options
are a bundled soundfont for consistency or the host's synth for authenticity, and the choice should
be a user setting rather than silently baked in.

`JUNGR01` also exports three C++-mangled entry points naming a `TAGRESAUDIOINFO` struct —
`AUDIO_READ`, `AUDIO_REWIND`, `AUDIO_DONE` — so streaming audio is a small read/rewind/done
interface the mixer pulls from.

## Resolution

The art is **800x600**. The game shipped at 640x480. So the assets already carry more detail than
the original ever displayed, and running at 800x600 or integer multiples is closer to the artists'
material rather than an upscale.

Pillarbox for widescreen first, because it is always correct. Extending backgrounds is a per-scene
art judgement and should be opt-in.

## What not to "improve"

- Do not rearm timers from `now`. See above.
- Do not expand bitmaps to RGBA at load. It destroys palette animation.
- Do not replay missed ticks.
- Do not smooth or interpolate animation the engine steps discretely.
- Keep every change behind a flag so original behaviour stays reachable for parity testing.

## Pinball's stuck ball (a port bug, now fixed)

Balls used to wedge on the GRUB lane stakes and bounce forever in the mouth of the B lane. The
cause was the port, not the table: the movement script (721) sets the ball's cel from its height
every tick (`sprite command 15` with the range `(y + 239) / 60`, then `12` to move it), so the ball
is drawn smaller as it rolls up the table, and its pixel-overlap tests with the stakes and bumpers
use that cel. JUNGS01 takes a set range's first cel at the ball's next move (`FUN_1000_6364` sets
`+0x51`, `FUN_1000_37ca` steps it); the port never took it for a one-cel range, so the ball kept
its launch-size cel (38×39) over the whole table and could not fit the 25-pixel B lane. With the
cel step ported, bot games show no stuck ball at all (0 in 30, with and without the watchdog below).

An earlier workaround moved the rightmost stake's collision segment, taking the wedge for a defect
in the disc's data. It is gone. What remains is one deliberate deviation, `pinball_unstick`, kept
as a safety net that no longer fires in testing: a ball in play that stays perfectly still for 2 s,
not in the plunger lane and with no flipper held (cradling stays possible), is sent gently upward,
30–60° off vertical, alternating sides. `JUNGLE_ORIGINAL=1` turns it off.

The point-on-sprite test also reads RLE bitmaps the way `FUN_1000_304e` does. It walks the
compressed row with no end-of-row check, so a point past a row's last encoded pixel reads on into
the next row's bytes rather than returning transparent.

## Port bugs fixed

**Op 33's "hidden counts" flags were crossed.** A collision pair's record carries one flag per
sprite saying whether it still collides while hidden: +0E for the first sprite, +0F for the
second. `FUN_1008_31b8` keeps each with its sprite when it orders the pair, and `347e` passes
them to S_048 (JUNGS01 ordinal 49, `1000:09be`) as the first and second sprite's. The port had
them the other way round. Pinball's GRUB lane sensors (1533–1539, script 1496, set up by 1489)
are the only pairs registered in a bot run of every game whose two flags differ: the sensor is
hidden until its letter is lit and is the one marked to count while hidden. With the flags
crossed, a ball rolled through G, R, U and B without lighting them, so the GRUB bonus could never
be earned.

Found by the same field-by-field audit against the original machine code:

| What | Original | What the port did |
|---|---|---|
| Builtin 0x6C (S_072) | `(sprite, on)`, EXE seg2:11ee; JUNGS01 `60f0` no-ops an unchanged state and moves the frame timer on by the time held | Read `(on, sprite)`: Pause froze nothing, and Pinball's progression bonus (1498) froze the whole table |
| Op 18 "back" (+0x11) | `8c00`: the scene we came from (DS:0x150e, set on leaving), or WM_CLOSE from the first scene | Copied an empty name: Esc and OK in Options did nothing |
| Op 8 completion | `88d4`: the done script is decoded when the clip starts, 0 = none; the tag is the clip's handle | Decoded at completion in whatever frame was live; `script 33(clip, 0)` then ran script 0, re-initialising Pinball mid-game (dead balls, games that never ended) |
| Op 80 completion | `9561`: decoded, then re-encoded | Stored decoded and decoded again |
| Pause | seg2 main loop skips the whole idle pass while DAT_5a5d is set; the mouse paths and key-up test it too | Timers, collisions, the queue and animation all ran behind the PAUSE sign |
| Stopping a program (S_010) | `4c12` frees it and zeroes both timers, both periods, catch-up and the loop break; S_039 and op 5 with no program call it | Kept the buffer and PC, so commands appended later ran after the old program's remains |
| S_067, commands 1/2/5/9 | `5c90`: the first step runs inside the call (`4b16`), timers kept, ms kept plain | Deferred to the next tick: a sprite shown and then re-placed in the same script stayed hidden (Pinball's hole kick-out) |
| COLLIDE (0x7C) | JUNGU01 `0f12..0f98`: each velocity component is a truncated long | Kept fractions |
| Hotspot clicks, drag release, key-up | `2c7e` passes no tag to a hotspot; `2d9e` keeps a vetoed drag; `304e` gives a player's key-up to the player alone and never reads +0x0E | Passed the sprite id; dropped the drag; ran the up binding |

## Port audit: behaviour the port had silently dropped

Found by comparing the port against the original's dispatch tables and by a census of what the
disc's data actually uses (`jungle FILE.BIN --audit`, and `JUNGLE_BUILTIN_CENSUS=1 jungle FILE.BIN
--script`). Each item returned success without doing anything, so the e2e "no unimplemented
opcode" check could not see it. All are now ported from the original code.

| What | Original | Effect when missing |
|---|---|---|
| Sprite command 22, movies | JUNGS01 `FUN_1000_4d86`; frames in a type 2 resource, indexed by a second type 2, named by a type 8 | The animated **Disney Interactive logo** in the opening card's banner never played |
| Op 35 and the Pause key | `FUN_1008_2776`, called by `FUN_1008_2f72` for VK_PAUSE | **Pause** did nothing; every game has a PAUSE sign and handler |
| Op 4, every hotspot at once | `FUN_1008_25a8` (on/off), `228e` (one click script, or none) | Clicks were taken where a scene had switched them all off |
| Op 83, device bindings | `FUN_1008_454c`: the old device off, held input released (`3e42`), the kinds in use | A player's previous device stayed armed; enable/disable and release did nothing |
| Mouse as a joystick | `FUN_1008_73fe` / `72e4`: dead zone 20, box 40, GETANGLE → GETQUADRANT → DS:0x78 | Choosing the mouse as a player's device gave no control at all |
| Builtin 0x70 | JUNGA01 ordinal 25: hold or resume sound and music | Called three times in every scene |
| Builtin 0x78 | JUNGS01 ordinal 77: `ScrollDC` by (dx, dy), by −2(dx, dy), back | Pinball's and Bug Drop's screen jolts never showed |
| Builtin 0x89 | `FUN_1008_488e`: a player's device back, optionally the other's off | Called once in each game |
| Builtin 0x6E | sets DAT_5a57, how joysticks are read (`FUN_1008_4a00`) | Stored; the port has no joystick path (a pad arrives as keys) |
| Game keys | `FUN_1008_4e6e` returns 0 once a player takes the key, and `2f72` stops | A player's key could also fire a script binding |
| Shift / Ctrl bindings | `2f72` tests Shift first, then Ctrl, with no fall-back | Shift+key ran the plain binding where the original ran nothing |

Still not ported, on purpose: sprite commands 3, 4, 18 and 19 (not used anywhere on this disc),
builtins 0x1B, 0x5F, 0x64, 0x68, 0x7D and 0x82 (no script calls them), and the Esc handling of
the rubber-band rectangle in segment 3 (`DAT_5a66`), which no game script starts. The "&" and
"PRESENT" sprites on the opening card (JUNGLE.BIN 82 and 84) are art the original never shows.
