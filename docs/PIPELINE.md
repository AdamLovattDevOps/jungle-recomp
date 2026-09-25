# ISO → Ported Source: The Professional Decomp Pipeline

Target: *Timon & Pumbaa's Jungle Games* (7th Level Inc. / Disney Interactive, 1995).
Goal: a portable, resolution-independent source port with modern quality-of-life features.

Each line is one step. Do them in order. Do not skip Phase 0.

---

## Phase 0 — Preservation & provenance

- Dump the physical disc yourself if you own it; never work from a re-encoded copy.
- Hash the raw image (`shasum -a 256`) and commit the hash, never the image.
- Mount read-only (`hdiutil attach -readonly -nobrowse`) so you can never mutate the original.
- Copy every file off the disc, then hash each file individually into `orig/CHECKSUMS.sha256`.
- Record disc metadata (volume label, publisher, date) — it dates the toolchain used to build the game.
- Add `*.iso` and all extracted media to `.gitignore`; the repo holds *code and knowledge*, not copyrighted assets.
- Write down the legal frame you are working in: clean-room-ish reimplementation, no shipped assets, users supply their own disc.

## Phase 1 — Triage: what kind of program is this?

- Run `file` on every executable to get the container format (here: **NE** = Win16, plus one PE32 launcher).
- Note the bitness — NE/Win16 means 16-bit segmented x86, a different decomp problem to 32-bit PE.
- Measure the code surface: sum the EXE + DLL sizes. ~220 KB here; under ~1 MB is a realistically finishable decomp.
- `strings` the main EXE and read the copyright banner — it tells you if this is bespoke code or a licensed engine.
- **Finding:** banner says `Runtime Player 1.2 (C) Copyright 1995 7th Level, Inc.` — the EXE is an *engine*, the game is *data*.
- Check the data files' magic bytes (`xxd | head -1`) — here `37 4C` = ASCII `"7L"`, a 7th Level proprietary container.
- Look for sibling titles using the same engine (other 7th Level games) — prior art multiplies your leverage.
- Search for existing community work (decomp wikis, forums, GitHub) before writing a single line yourself.
- Decide the split: **Track A = reimplement the engine**, **Track B = parse the data format**. They proceed in parallel.

## Phase 2 — Static map of the binaries

- Install a disassembler with a real NE loader (Ghidra is free and handles NE/16-bit; IDA if you have it).
- Import the EXE and all four DLLs into one project so cross-references resolve between them.
- Dump the NE segment table — segment count and per-segment size is your work breakdown structure.
- Dump the import table: every `KERNEL`/`USER`/`GDI`/`MMSYSTEM`/`TOOLHELP` call is an OS boundary you must later shim.
- Dump the export table of each DLL — exported ordinals are the engine's internal module API, already named for you.
- Classify the DLLs by their imports: graphics (GDI-heavy), sound (MMSYSTEM), resource/loader, UI.
- Extract embedded PE/NE resources (icons, dialogs, string tables, bitmaps) with `wrestool` or Ghidra.
- Auto-analyse, then run a library-signature pass to identify compiler runtime functions (Microsoft C 7 / early MSVC CRT).
- Tag every identified CRT function as "do not decompile" — you will link a real CRT instead.
- Count the remaining unidentified functions. That number is your actual backlog.

## Phase 3 — Dynamic observation (cheaper than reading asm)

- Get the game running under something you can instrument. **Check your host first:** Wine's Win16 support needs a 32-bit Wine build, and macOS dropped 32-bit in Catalina, so Wine and CrossOver cannot run Win16 on a modern Mac at all. On Linux/x86 a 32-bit prefix still works. Otherwise it is a full machine emulator — DOSBox-X, 86Box/PCem, or QEMU with a real Win3.1/95 guest and your own licensed media.
- Turn on API tracing (`WINEDEBUG=+relay,+file,+gdi`) and capture a full boot-to-menu log.
- The relay log gives you, for free: file open order, blit calls, palette setup, timer rates, sound device init.
- Diff traces between two runs (main menu vs one minigame) to isolate per-scene code paths.
- Attach a debugger, break on `CreateWindow`/`SetTimer`/`BitBlt`, and walk back up the stack to name the engine's main loop.
- Record a reference video and frame timings — this is your ground truth for "does the port behave identically".
- **Budget for this phase properly.** Some facts are not recoverable by reading, at any effort. Anything the engine computes at runtime — callback addresses, script entry pointers, allocation layouts — is invisible to a disassembler and can only be watched. If you skip Phase 3 because static analysis is going well, you will hit a wall later that no amount of further reading can move, and you will have to come back and build this anyway. Stand the guest up early even if you do not need it yet.

## Phase 4 — Crack the data container (Track B)

- Hex-dump the first 512 bytes of several `.BIN` files side by side; fields that stay constant are the header, fields that scale with file size are offsets/counts.
- Identify the directory: usually magic, version, entry count, then a table of (offset, length, type, name/id).
- Write a throwaway Python parser that dumps the directory to CSV; iterate until every offset lands inside the file and the entries tile the file with no gaps.
- Extract one entry and identify it by content (`file`, entropy, known image/audio headers) — expect DIB bitmaps, WAV, palettes, and scripts.
- If entries are compressed, entropy will be high and size fields will disagree; find the decompressor in the EXE by breakpointing on the read, then reimplement it.
- Look for an entry type that is neither image nor audio — that is the **scene script / bytecode** that drives the game.
- Disassemble the script bytecode by locating the engine's interpreter loop (a big `switch` on an opcode byte) in the EXE and transcribing the opcode table.
- **Read the consumer, do not guess the hex.** A hex dump gives you hypotheses; the engine code that reads the record gives you facts. Find the function that loads the type and note the fixed offsets it touches — that is the struct, for free and correct. Guessing produces layouts that are plausible, self-consistent and wrong.
- Build a proper extractor in `tools/` with a round-trip test: unpack → repack → byte-identical to the original.
- Version the *format spec* as Markdown in `docs/`; the spec is the durable asset, the script is disposable.

## Phase 5 — Decompile the engine (Track A)

- Pick the entry point (`WinMain`) and work outward depth-first; never decompile alphabetically.
- For each function: read asm → write C → compile with the *period* toolchain → diff the output asm against the original.
- This is **matching decompilation**: the goal is byte-identical object code, which proves your C is semantically exact.
- Automate the diff (`asm-differ` or equivalent) so "does it still match" is one command.
- Keep a `progress` script that reports percentage of bytes matched; it is your only honest measure of completion.
- Name and type aggressively as you go — every named struct field makes the next ten functions easier.
- Commit the symbol/type database alongside the code so knowledge is never trapped in one person's Ghidra project.
- Accept non-matching ("equivalent") decomp for CRT and compiler-artifact code; reserve matching effort for game logic.
- Stop at 100% of *game* code; you do not need to match the C runtime.

## Phase 6 — Lift to portable C

- Fork the matched source into a `port/` tree; the matched tree stays frozen as the reference.
- Replace every Win16 API call with a thin platform abstraction layer: `plat_blit`, `plat_audio_submit`, `plat_file_open`, `plat_ticks`.
- Back that layer with SDL. **Pick the version by your most constrained target, not your development machine** — SDL3 is the better API, but if a handheld is in scope you are probably on SDL2 (VitaSDK ships no SDL3). Choosing this late means rewriting the platform layer.
- Delete the segmentation: 16-bit `far`/`near` pointers become flat pointers; fix the pointer arithmetic that assumed 64 KB wrap.
- Fix endianness and struct packing: read file formats field-by-field, never `fread` a struct.
- Replace 8-bit palette blitting with a 32-bit RGBA surface plus a palette lookup, keeping the original palette animation semantics.
- Move fixed-rate game logic onto an explicit fixed timestep, decoupled from rendering, so the game speed stops depending on CPU speed.
- Verify parity against the reference video before adding a single feature.

## Phase 7 — Modern quality-of-life

- Decouple the render resolution from the 640×480 logical playfield: scale output, keep logic coordinates.
- Add integer scaling plus an optional CRT/nearest/linear filter; let the user pick.
- Add widescreen handling by pillarboxing first (always correct), then optionally extending backgrounds where art allows.
- Replace the fixed 60/70 Hz tick with a high-refresh-friendly loop: fixed logic tick, interpolated presentation.
- Add remappable input and full gamepad support through SDL's game controller layer.
- Add save-anywhere / state serialisation by snapshotting the engine's VM state, not the OS process.
- Add windowed/fullscreen toggle, audio mixer volumes, subtitle and language toggles from the resource string tables.
- Gate every change behind a flag so "original behaviour" remains reachable for parity testing.

## Phase 8 — Port to other platforms

- Keep the platform layer the only `#ifdef` zone; if platform code leaks into game code, fix it immediately.
- Add CMake presets per target rather than per-platform build scripts.
- Desktop first (macOS/Linux/Windows), then web via Emscripten, then consoles/handhelds via SDL ports.
- Handhelds constrain more than screen size: no JIT on iOS App Store builds or stock Vita, so any bytecode the game uses must stay interpreted. Check this before designing the VM, not after.
- For low-memory targets, stream assets from the extractor's repacked archive instead of loading whole scenes.
- Run the same automated parity test on every target: boot, play a scripted input sequence, hash the framebuffer.

## Phase 9 — Shipping responsibly

- Ship **code only**. The port requires the user's own disc or install.
- Ship the extractor as the on-boarding step: point it at the CD, it builds the asset pack locally.
- Document the exact original file hashes the extractor expects, and fail loudly on unknown revisions.
- Keep the format spec, the symbol database, and the pipeline doc in the repo — they outlive the code.

---

## This game specifically

| Thing | Value |
|---|---|
| Engine | 7th Level "Runtime Player 1.2" (1995) |
| Main binary | `JUNGLE.EXE`, 116 KB, NE / Win16 |
| Engine DLLs | `JUNGU01` (37 KB), `JUNGS01` (31 KB), `JUNGA01` (21 KB), `JUNGR01` (14 KB) |
| Launcher | `WIN95/LAUNCH.EXE`, PE32 — a thin shim, not the game |
| Data | 13 × `.BIN`, magic `"7L"`, ~60 MB total |
| Scenes | main, opts, pinb, bugd, burp, hipp, shot, prty, scor, cred, int1, int2 |
| Code to match | ≈ 220 KB — a finishable project |

Track A and Track B are independent. Track B (the `"7L"` container) gives visible results fastest and should go first.
