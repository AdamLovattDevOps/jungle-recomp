# Port — scope and plan

Goal: *Timon & Pumbaa's Jungle Games* running natively on **PS Vita, iOS, macOS, Windows and
Linux**, at arbitrary resolution, with modern quality-of-life.

The target list was macOS-only until 2026-09-24. Widening it changes two decisions, both recorded
below: the platform library drops from SDL3 to SDL2, and the matched tree is now explicitly a
reference artifact that never ships.

## Two trees, and why

The matching decompilation is 16-bit x86 Microsoft C output. It cannot run on ARM, so it cannot run
on a Vita or an iPhone. That is not a problem with the plan — it is the plan:

| Tree | Built with | Purpose |
|---|---|---|
| **Matched** | period MSC/VC++ under DOSBox | Proves the logic is right, by byte-identical diff against the original. Frozen once matched. |
| **Portable** | modern C99 + SDL2 | What ships. Every routine validated line-for-line against the matched tree. |

`docs/PROJECT_NOTE.md` already stated the port follows the matched tree. With Vita and iOS in scope
that sequencing stops being a preference and becomes structural.

## Platform constraints that bind the design

| Platform | Constraint | Consequence |
|---|---|---|
| PS Vita | VitaSDK ships **SDL2**, no SDL3 | SDL2 is the ceiling for the whole port |
| PS Vita | ARMv7, 512 MB, 960x544 native | 800x600 art needs scaling, not cropping |
| iOS | No JIT for App Store builds | Script VM stays a plain interpreter. It already is. |
| All | No dynamic code generation anywhere | Rules out any recompilation-based approach |

No target needs 64-bit, threads, or a GPU shader path. The engine is a palette-indexed software
compositor; a single streaming texture upload per frame covers every platform.

## Where the project actually stands

Corrected 2026-09-24. The previous version of this table was stale — it listed tables 4 and 6, the
audio codec and the resource directory as undecoded, all of which were finished in later rounds.
`README.md` and `docs/FORMAT.md` are authoritative.

| Piece | State |
|---|---|
| Container format (`"7L"`) | **Done** — verified three ways, 39/39 structures re-serialise byte-identical |
| Resource directory, all six table parsers | **Done** — 23,276 resources indexed |
| Bitmaps | **Done** — 11,506 / 11,506, both codecs (RLE and LZW) |
| Palette | **Done** |
| Audio | **Done** — 628 / 628, ADPCM codec reimplemented |
| Scene scripts | **89.1%** disassembled — 2,960 / 3,323 |
| Script VM | Instruction set complete, 19,840 expressions execute; memory model simplified |
| Compositor | **Uncompressed path transcribed** (`src/blit.c`), decode-during-blit path open |
| Script entry points | **Not resolved** — and it gates a scene running at all |
| Win16 guest | **Done** — Windows 3.1 installs unattended under DOSBox-X, `guest/` |
| Engine outer dispatch | 18 handlers located; transitive closure not bounded |
| Matching harness | **Done and matching** — Visual C++ 1.0, `/ASw /GD /GEf /Ox`, LINK 5.50. First two functions match to the relocated word |

## Critical path

Ranked by what blocks what, per `docs/FINDINGS.md`.

1. **Script entry points.** Scripts are entered at engine-supplied record pointers. Round 25
   established this, round 28 ruled out `RESSETCALLBACK` as the source. **Not statically resolvable
   from decompiled output** — this needs the engine observed live under a Win16 debugger, which
   means a DOSBox-X or 86Box guest. Everything downstream waits on it.
2. **Outer opcode handlers and their transitive closure.** Bounded, not small. The 18 first-layer
   handlers are ~2 KB of C, but they are entry points into shared engine machinery — `op5` alone
   reaches a primitive with 85 callers.
3. **Compositor, remainder.** The decode-during-blit path in `FUN_1000_2848`: RLE opcodes consumed
   against the row-offset table so clipped pixels are never expanded. Output-equivalent to what
   `src/blit.c` does now, so this is a performance item, not a correctness one.
4. **Message loop, input, timing.**
5. **Platform layer.** SDL2: window, palette-to-RGBA blit, input, fixed-timestep loop, audio out.
6. **Quality of life.** Resolution independence (art is 800x600, better than the 640x480 the game
   shipped at), integer scaling, gamepad, save-anywhere via engine-state snapshot.

Item 1 is the only one that cannot be moved forward by more reading.

## Why emulation is not a shortcut

Worth restating, because it is the usual answer and it does not apply:

- The game is **Win16 (NE)**. Running it needs 16-bit thunking.
- Wine's 16-bit support requires a 32-bit Wine build. macOS removed 32-bit support in Catalina, so
  Wine on modern macOS **cannot run Win16 at all**. CrossOver is the same.
- That leaves a full machine emulator — UTM/QEMU, 86Box, or DOSBox-X — running a real Windows 3.1
  or 95 guest, which needs your own licensed install media.

So emulation is possible but is a VM, not an app, and gets you nothing on a Vita or a phone. It is
still worth standing up for one reason: it is the debugger host that unblocks item 1.

## Realistic expectation

The container and asset work is genuinely complete. Items 3, 4 and 5 are comparable in size to what
is already done. **Item 2, the interpreter, is larger than everything done so far combined** — that
is where a port of this kind actually lives.

This is a weeks-to-months project. It is achievable: 157 KB of engine code is small by decomp
standards, the symbol names survived in one DLL, and every asset is already out. But no useful
purpose is served by implying a playable build is near.

---

## Toolchain acquisition

Matching requires the original compiler. Two routes were checked:

### Docker — not viable

Only Open Watcom images exist (`mmastrac/openwatcom`, `lapinlabs/watcom`, `ykode/watcom` and
similar). Open Watcom can target Win16 but is a **different compiler**: its register allocation and
instruction selection will never match a Microsoft-built binary, so it is useless for matching.

No MSVC 16-bit images exist, because Microsoft's compilers cannot be legally redistributed inside an
image. Docker is a dead end for this project.

### archive.org — viable

Relevant items are listed in the software collections, including Visual C++ 1.52c (1995) and
Windows 3.1 SDK copies. Microsoft has never re-released the 16-bit toolchains. Acquisition is the
user's decision.

### Which version to get

Do not assume 1.52c. The evidence points elsewhere:

- Engine binaries: **linker 5.50**
- `SETUP.EXE` / `UNINSTAL.EXE`: **linker 5.60**

The installer was built with a newer linker than the engine. If Visual C++ 1.52c ships LINK 5.60,
then the engine predates it and was built with Visual C++ 1.0 or 1.5. **Acquire several candidates**
and run the discrimination test — build a trivial Win16 DLL with each and compare the NE linker
stamp against 5.50.

### Hosting the toolchain on macOS

The 16-bit Microsoft toolchains are DOS-hosted, so they run under DOSBox or DOSBox-X, both of which
build ARM-native on Apple Silicon. Builds can be driven non-interactively by pointing DOSBox at a
config that mounts the source directory, runs a batch file, and exits — which is what the diff
harness will wrap.

No emulated Windows installation is required: `CL.EXE` and `LINK.EXE` are DOS programs even when
targeting Win16.
