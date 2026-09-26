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

## Table fixes (deliberate deviations)

Everywhere else the port follows the original instruction for instruction. Pinball is the one
place it knowingly plays differently, because the original physics can hold a ball still for good.
`JUNGLE_ORIGINAL=1` turns both changes off.

**How a ball gets stuck.** Each tick the movement script (721) aims the ball one step along its
heading and a mask script (1691 at the top of the table, 1692 at the bottom) halves the step back
until the point is off the table's pixel masks. It never deflects. Bouncing is left to the
collision scripts, which model posts and guides as line segments and bounce only a ball moving
*into* a line. Where a mask and its line disagree, the mask stops the ball while the line says it
is moving away. The ball then gets no move and no bounce, gravity keeps adding speed that goes
nowhere, and it stays put until the table is shaken.

Every routine on that path was checked against the original machine code: the expression VM
(all operators, truncating division), the trig tables and helpers, `GETANGLE` and its degrees
constant, `COLLIDE`, ray-to-box (op 50), the long-arithmetic builtins, sprite placement
(JUNGS01 `FUN_1000_0b30`) and the point-on-sprite test (JUNGS01 ordinal 81 and its pixel reader
`FUN_1000_304e`). All of them match.

1. **The rightmost GRUB lane post** (`table_fixes`). Its segment's top end (globals 2496 and 2501)
   is the stake's top-left corner, (146, −193). That is outside the stake's mask, while the other
   four posts' segments start inside theirs. A ball dropping onto the stake's top-right shoulder
   rests at (159, −191), inside the top-cap test (`y <= top + 2`) by one pixel, and is judged to be
   moving away from the cap. With the ball bot, that happened in about a fifth of games, after an
   ordinary 1–2.5 s launch. Moving the top end to (154, −185), inside the stake like the others,
   removes it (0 in 72 games).
2. **Any other wedge** (`pinball_unstick`). A ball in play that stays perfectly still for 2 s, not
   in the plunger lane and with no flipper held (cradling stays possible), is sent gently upward,
   30–60° off vertical, alternating sides. A ball at rest under gravity is always held from below,
   so up is the way out, and the table's own gravity brings it back down. In testing this fired
   about once in six games, always in the right inlane corner above the flipper pivot, and freed
   the ball within two pushes.

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
