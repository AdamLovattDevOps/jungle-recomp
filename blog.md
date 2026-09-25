# Rebuilding a 1995 Disney Game Engine with an AI Pair

*September 2026*

TL;DR: Over three working days, Claude (Opus 5.5 with a 1M-token context, on medium effort, in
Claude Code) and I reverse engineered 7th Level's Windows 3.1 game engine and rebuilt it as
portable C. *Timon & Pumbaa's Jungle Games* now runs from its original disc files on a Mac, an
iPad, a phone browser, Windows, Linux and a PS Vita. The AI did most of the slog. My job was
direction, taste, and noticing when something was wrong.

## Why this game

It was my wife's favourite game as a child. I wanted her to be able to play it again on her
iPad, not in an emulator with a Windows 3.1 install, but as a proper app. I also wanted to
understand how it worked, which is the more honest reason for doing it the hard way.

The starting point was a single disc image and nothing else. There is no source code, no SDK
and, as far as I could find, no prior documentation of the file formats.

## The setup

- **Model:** Claude Opus 5.5 with the 1M-token context window, on medium reasoning effort,
  running in Claude Code on an M3 Pro MacBook.
- **Tools the AI installed and drove:** Ghidra (headless, for decompiling the 16-bit NE binaries),
  DOSBox-X and QEMU (for a Windows 3.1 guest), a 1990s Microsoft compiler, SDL2, Emscripten,
  VitaSDK, Xcode and Real-ESRGAN.
- **Tools it wrote:** about forty Python scripts. There are parsers for the container format,
  codec implementations, a disassembler for the game's script language, a matching harness,
  a script decompiler, a line-protocol driver for the engine, and game-playing bots.
- **Working style:** I set a goal and a standing loop ("keep going until the game is playable"),
  and it worked through it round by round, writing up each round in a findings log. It committed
  as it went: 153 commits, and a 3,000-line log that includes every wrong turn.

## Day one: the data, cracked in an afternoon

The game ships as a small Windows executable, four DLLs and thirteen `.BIN` containers. The
executable turned out not to be game code at all. It is 7th Level's **Runtime Player**, a
general engine that interprets the containers, so the real game lives in the data.

The first day went on the formats:

- **Containers.** Ghidra decompiled all 1,534 functions across the five binaries. From those, the
  AI worked out the container header, the resource directory and the resource types.
- **Bitmaps.** There were two codecs: an RLE, and an LZW with switchable code widths wrapped in a
  chunked stream. 11,506 bitmaps came out.
- **Sound.** 628 clips, all in 7th Level's own 4-bit ADPCM. The step tables it needed are read
  straight out of the game's DLL, so they never had to be transcribed.
- **Scripts.** Scenes are driven by a stack-based virtual machine. By the end of the day there was a
  disassembler, a Python VM and a C port of it, cross-validated against each other.

This is the kind of work that used to take a hobbyist months: reading decompiler output, forming a
hypothesis about a byte layout, writing a parser, and checking it against every file on the disc.
The model does that loop tirelessly, and it checks against the whole corpus by default, which is
what catches the wrong guesses.

## The detour: chasing a byte-identical decompilation

Early on I asked for the gold standard: C that recompiles to byte-identical machine code, like the
big console decompilation projects. That meant finding the original compiler.

This produced some of the best detective work of the project, and it was ultimately a detour.

- **Fingerprinting the compiler.** The AI fingerprinted the toolchain from the binaries' headers:
  every engine module was linked by LINK 5.50. From there it identified Visual C++ 1.0 and got it
  running under DOSBox-X.
- **Brute-forcing the flags.** It found the exact compiler flags by sweep, including one that an
  earlier, narrower sweep had wrongly ruled out.
- **A working harness.** It built a harness that compiles a function, links it, strips out
  relocations and diffs it against the original bytes. Seven of eight functions in the resource DLL
  came out byte-exact.

But byte-matching 158 KB of 16-bit code, function by function, was going to take a very long time,
and it would not produce a single playable frame. The code it would produce can only ever run on a
16-bit x86 machine. We changed course to a portable reimplementation that follows the original's
logic, checked against the assembly. The matched files stay in the repo as a reference.

In hindsight the pivot came a round or two later than it should have. Precise, measurable
progress is seductive, even when it is progress towards the wrong thing.

## Day two: from formats to a picture

Day two turned the formats into an actual picture on the screen:

- **Resources.** The resource-type dispatch, the sprite record layout and the frame lists.
- **A compositor.** Real disc art drawn through the recovered compositor.
- **A live guest.** A Windows 3.1 guest in QEMU, driven by scripted keystrokes, so we could compare
  against the real thing.

The honest part of this day was a rule I had set early: the AI had to say plainly what did
not work. The findings log has a round titled "Scoping the outer opcode work honestly". The viewer
it had built was an asset browser, not a port, and it said so instead of letting me believe
otherwise.

## Day three: the engine comes alive

The biggest single bug was found by the simplest method: rendering a scene to a PNG and
looking at it. The main menu came out as coloured noise. Every earlier round had checked the
LZW decoder by comparing the C against the Python, and both agreed, because both were wrong in the
same way.

The model then transcribed the decoder from the assembly, instruction by instruction, and found no
difference at all. The real bug was elsewhere: the palette was loaded at the wrong base index. The
LZW had always been right. The metric had fooled it for days.

After that, the engine came together quickly. It now runs about seventy script operations and fifty
built-in functions, with timers, collisions, sprites that run their own animation programs, input,
text in the game's own font, fades, save files and music. The bugs that stopped it were small and
instructive:

- **Comparisons.** Four of the six comparison operators in the script VM were inverted.
- **Jumps.** Every `goto` ended its script, because one opcode reported its length as −2 and the
  engine read that as "stop".
- **Coordinates.** Everything was drawn off-screen, because the original measures coordinates
  from the centre of the screen, set once with `SetViewportOrg`.
- **Scores.** No game ever reached the high-score table, because one function passed a string
  handle raw where the port had decoded it as a variable.

By the evening all five games played, including Pinball's full physics, which is script code over
a handful of native helpers ported directly from the assembly.

Music was a nice use of parallel work. The AI spun up a sub-agent to decode the MIDI format from
the disassembly while it carried on with the engine. The format turned out to be millisecond delays
interleaved with raw MIDI messages, and it now plays through a small General MIDI synth written for
the port.

## Where I earned my keep

The AI could not see or hear the game running live: it had no screen-recording permission on my
Mac, and no ears. Several of the most important bugs were ones I found by playing:

- **Sound was drifting.** I heard it before any test did. The mixer turned out to be running 6% fast
  because of a rounding error in how it stepped its clock. The fix now has a test that checks sound
  against game time sample for sample.
- **Pinball ball stuck in a corner.** That traced back to the table's own collision scripts, which
  can wedge the ball against a post, just as they presumably did in 1995. The game's built-in answer
  is the table-shake key, and the automated player uses it too.
- **Getting the goal wrong.** I asked for a hosted version for my wife behind a private link. The AI
  first built a "bring your own disc" page instead. I had to restate what I meant.

In short: I set the goals, made the judgement calls and did the real-world testing. The model did
the reading, writing, measuring and grinding, and it was candid about what it had not verified.

## Making it testable by machine

Once the game worked, the next question was how to keep it working. The answer was to let the AI
play it:

- **A drive mode.** A line-based protocol that steps the engine a tick at a time and reports sprite
  positions and script variables.
- **Bots.** One per game, written against that protocol. The Pinball bot tracks the balls, flips the
  right flipper, launches and shakes, and scores around 20,000.
- **An end-to-end suite.** It boots the game, opens every menu item, plays every game to game over,
  beats a pre-filled high-score table, types a name, and checks the saved file. The whole thing runs
  in about three seconds, because the engine runs headless far faster than real time.

## Making it look good on a modern screen

The original art is 800×600 in 256 colours, heavily dithered. The AI exported every bitmap from the
disc and upscaled them with Real-ESRGAN's anime model on the Mac's GPU. That took about eleven
minutes for 11,506 images.

The engine can now draw the frame as GPU textures at the display's native resolution. F9 cycles
between the original pixels and the upscaled art at 1×, 2×, 3× and 4×. The game logic is untouched:
hit tests and collisions still happen on the original 800×600 canvas.

## Shipping it

- **iPad.** A signed `.ipa`, sideloaded, running full screen. The iPad keyboard works as a PC
  keyboard; because the Magic Keyboard has no Esc key, `` ` `` stands in for it.
- **Web.** The same C compiled to WebAssembly with Emscripten, in a page dressed in the game's own
  art: Zazu flapping in the corner, and Pumbaa trotting along as the loading bar. On a phone it
  shows touch controls, and holding it sideways gives a handheld layout.
- **The rest.** Windows (cross-compiled), Linux, and a PS Vita package.

The public repository contains no game data. It is bring-your-own-disc: every tool that needs art
or code from the game reads it from your copy.

## What I took away

- **The slog is now cheap.** Reading decompiled 16-bit code, writing a parser, checking it against
  every file, and writing up what happened is exactly what the model is best at. It did it for three
  days without getting bored.
- **Judgement is still the bottleneck.** The expensive mistakes were choices about direction: the
  byte-matching detour, and trusting a metric that compared two wrong implementations with each
  other. Those were caught by stepping back, not by working harder.
- **Put real evidence in front of it.** Rendering a frame to an image, recording audio to a WAV, and
  bots that play to game over. Each time the AI could check its work against the actual output
  rather than its own expectations, it got faster and more accurate.
- **A findings log is worth the tokens.** Writing every round down, including the failures, meant
  that each new session and each sub-agent started from the truth rather than from a summary.

The game is on the iPad now, and on the web behind a private link for my wife. That was the point.

*The code, docs and the full investigation log are at
[github.com/AdamLovattDevOps/jungle-recomp](https://github.com/AdamLovattDevOps/jungle-recomp).*
