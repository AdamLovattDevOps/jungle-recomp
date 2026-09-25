# The period toolchain

The matching decompilation needs the compiler the game was actually built with. This directory
holds the harness; the compiler itself is not committed.

## Which one, and how we know

Every engine binary carries a linker version stamp in its NE header at offset `+0x02`:

| Binary | NE linker |
|---|---|
| `JUNGLE.EXE`, `JUNGR01`, `JUNGS01`, `JUNGU01`, `JUNGA01` | **5.50** |
| `SETUP.EXE`, `UNINSTAL.EXE` | 5.60 |

Two candidates were acquired and their linkers compared:

| Product | `LINK.EXE` banner |
|---|---|
| **Visual C++ 1.0 Professional** | `Microsoft (R) Segmented Executable Linker  Version 5.50` |
| Visual C++ 1.5 Professional | `Microsoft (R) Segmented Executable Linker  Version 5.60` |

So the engine was built with **Visual C++ 1.0**, and the installer with 1.5 or later — which is
exactly the split the two stamps implied. Do not use 1.52c.

Confirmed by building rather than by reading strings: `TEST.C` and `TEST.DEF` compile and link to a
Win16 DLL whose own NE header stamps **5.50**, matching the engine.

## Acquiring it

Not committed and not redistributable. Microsoft has never re-released the 16-bit toolchains; the
archive.org software collections carry them. Put the ISO at `guest/toolchain/VC100PRO.ISO`, or set
`VC100_ISO` to wherever you keep it.

## Running a build

```sh
tools/dosbuild.sh <srcdir> <command-file>
```

`srcdir` is mounted as `C:` and must be writable. `command-file` holds DOS commands, one per line.
`toolchain/stamp.cmd` is the smallest useful example — it rebuilds the stamp test:

```sh
tools/dosbuild.sh guest/build toolchain/stamp.cmd
```

## Two things that will waste your afternoon

**DOSBox-X needs `-silent`.** Without it, it blocks on its configuration-tool GUI, never reaches
`AUTOEXEC`, and sits at 0% CPU until the timeout. It looks like a hung emulator; it is a dialog you
cannot see. `-nogui -nomenu` alone are not enough.

**`TMP` must point at a writable drive.** `CL` and `LINK` run under the Phar Lap DOS extender, which
wants a swap file. If `TMP` is unset it tries the current drive, and if that is the CD you get
`Phar Lap err 58: Can't create VM swap file of size 0 in directory D:\` with no other explanation.
`dosbuild.sh` sets `TMP` and `TEMP` to `C:\` for this reason.

## Extracting Microsoft C/C++ 7.0 from floppy images

MSC 7.0 is the leading candidate for the *compiler* (the linker is settled; see above and
`docs/FINDINGS.md`). It ships as ten 1.44 MB floppy images, and its files are KWAJ-compressed with
a `$` as the last character of the extension.

There is no need to run its installer, which wants disk swapping:

1. Mount each image read-only and merge the trees — the directory layout across disks is already
   the final layout, so `ditto` of each mount into one directory reassembles it.
2. Expand with `EXPAND.EXE`, which ships on the Windows 3.1 install media and understands KWAJ.
   Only `BIN`, `INCLUDE`, `INCLUDE\SYS` and `LIB` are needed.
3. `EXPAND` decompresses but does **not** rename, so the output keeps the `$` names. Rename on the
   host afterwards: `.EX$`→`.EXE`, `.LI$`→`.LIB`, `.OB$`→`.OBJ`, `.DL$`→`.DLL`, `.H$`→`.H`, and so
   on. Check the size to confirm it really expanded — a compressed `CL.EX$` is 42 KB and the
   expanded one is 70 KB, under the same name.

## Open: MSC 7.0 needs a DPMI host

MSC 7.0's compiler passes (`C13216.EXE`, `C23216.EXE`, ...) are 32-bit DOS-extended and abort in
bare DOS with:

```
run-time error R6901
- DOSX32 : This is a protected-mode application that requires DPMI
```

Two hosts were tried and neither is working yet:

- **Windows 3.1 enhanced mode** does provide DPMI — the error advances to
  `R6914 - DOSX32 : can not initialize device : VPFD.386`. `VPFD.386`, `VMB.386` and `VMCPD.386`
  ship in MSC 7.0's `BIN` and belong in `WINDOWS\SYSTEM` with `device=` lines in `SYSTEM.INI`
  `[386Enh]`, which its own installer would normally add. With them added, the batch did not run.
- **CWSDPMI** (`guest/toolchain/dpmi/`, freely redistributable) loads, but `CL.EXE` then writes
  CWSDPMI's own string table to its output file rather than compiling. Not yet diagnosed.

This is the only thing standing between the matched tree and a compiler that can be tested against
the original prologue.

### What has been tried for the DPMI host

`DOSX32`, the extender bound into MSC 7.0's compiler passes, needs a **32-bit** DPMI host. The
errors are specific enough to triangulate with:

| Host | Result |
|---|---|
| none (bare DOS) | `R6901 - DOSX32 : This is a protected-mode application that requires DPMI` |
| **HDPMI16** (HX) | `R6902 - DOSX32 : DPMI host not 32 bit` — so the check is real and 16-bit hosts are out |
| **HDPMI32** (HX) | **No error.** The DPMI check passes, then `CL` hangs — spinning at ~92% CPU with `cycles=max`, idle at 0% with fixed cycles. |
| **CWSDPMI** | Loads, but `CL`'s output file receives CWSDPMI's own string table instead of compiler output. |
| **Windows 3.1 enhanced mode** | Supplies DPMI; advances to `R6914 - can not initialize device : VPFD.386`. Copying `VPFD.386`, `VMB.386` and `VMCPD.386` into `WINDOWS\SYSTEM` with `device=` lines in `SYSTEM.INI` `[386Enh]` clears that, but the batch then does not run. |

HDPMI32 is the right host — `R6902` proves the 32-bit requirement is being checked and satisfied.
The remaining hang is between `CL` and the child pass it spawns (`C13216.EXE`), not a missing host.
Worth trying next: invoking `C13216.EXE` directly with the command line `CL` would have built, which
removes the spawn from the picture, and running MSC 7.0's own `SETUP` so the toolchain is installed
as its installer intends rather than hand-expanded.

None of this blocks the port. It blocks only the *matching* metric, which cannot move until the
right compiler is running.

## Resolved: the exact build

```
cl   /c /ASw /GD /GEf /Ox <file>.c
link /NOD <objs>, <out>.dll,, SDLLCEW LIBW, <out>.def
```

Visual C++ 1.0 for both. `/GD` selects the DLL convention; `/GEf` selects the prologue the engine
uses — `DGROUP` into `AX` first, then the frame, no `inc bp` / `dec bp` export marker, `mov sp,bp`
epilogue. Verified on `RESCOUNTSTRINGS` and `RESCOUNTVARIABLES`, both matching to the relocated
`DGROUP` word only.

The MSC 7.0 material above is retained because the extraction recipe and the DPMI matrix are
reusable, but MSC 7.0 is **not** this game's compiler and is not needed to build the matched tree.
