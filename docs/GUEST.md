# The reference guest

A headless Windows 3.1 guest that runs the original game under QEMU and dumps frames on demand.

Its purpose is **ground truth**. Everything the port claims about pixels is otherwise only internal
consistency — the format agreeing with itself. A captured frame from the real game is the only thing
that turns "this looks right" into a number.

## Why QEMU and not DOSBox-X

DOSBox-X is the better DOS environment and is still used for the period toolchain (see
`toolchain/README.md`). It is useless for this job:

- It has **no headless screenshot**. Capture is bound to a mapper key, and `-silent` means no window
  and no keys.
- Launched windowed from a background shell it **blocks at startup** on its configuration-tool
  dialog, at 0% CPU, with no window ever appearing.
- Driving it through a GUI session with AppleScript hits a macOS Automation permission prompt that
  cannot be granted non-interactively.

QEMU runs with `-display none` and still answers `screendump` on its monitor socket, so frames can
be captured unattended and forever.

## What the guest is made of

| Piece | Source |
|---|---|
| Boot floppy | **FreeDOS 1.3**, Floppy Edition `x86BOOT.img`. Freely redistributable, so nothing here depends on unlicensed media. |
| Hard disk | 300 MB raw image, one FAT16 partition, built entirely from the host with `mpartition` and `mcopy` |
| Windows 3.1 | installed by the unattended `SETUP.SHH` route under DOSBox-X, then copied in file by file |
| The game | copied from the disc to `C:\JUNGLE` |

Nothing in `guest/` is committed: it holds the user's own Windows and game files.

## Building it

```sh
qemu-img create -f raw hda.img 300M
mpartition -I -c -t 609 -h 16 -s 63 c:
mformat -F c:                       # -F forces FAT16
mcopy -s -o <windows tree>/* c:/WINDOWS/
mcopy -o <disc>/JUNGLE/* c:/JUNGLE/
```

with an `mtoolsrc` naming the images:

```
drive a: file="<path>/boot.img"
drive c: file="<path>/hda.img" partition=1
```

The boot floppy's `FDCONFIG.SYS` and `FDAUTO.BAT` are replaced so it boots straight into the game
rather than the FreeDOS installer.

## Capturing

```sh
tools/qemu_capture.py /tmp/shot 55,110,165
```

Boots the guest, waits, and issues `screendump` at each time in seconds. `tools/ppm2png.py` converts
QEMU's P6 output and reports the colour histogram, which is usually enough to tell a text-mode error
screen from a real game frame without looking.

## Boot problems, in the order they appeared

Each was diagnosed from a captured frame, which is the rig proving its own worth.

| Symptom | Cause and fix |
|---|---|
| `CONFIG.SYS error in line 3` | `HIMEM.EXE` is not on the FreeDOS floppy. Use `DEVICE=C:\WINDOWS\HIMEM.SYS` from the Windows install instead. |
| `R6950 — DOSX32: VMCPD.386 has been superceded` | Left over from the MSC 7.0 compiler experiment, which added `VPFD.386`, `VMB.386` and `VMCPD.386` to `SYSTEM.INI`. Removed. |
| `ERROR: Unsupported MS-DOS version` | FreeDOS reports 7.10 and Windows 3.1 refuses it. Needs the reported version forced down, via the kernel's `VERSION=` directive and `SETVER.SYS`. |

## The guest runs the game. It cannot yet render it.

The guest boots FreeDOS, loads Windows 3.1, and launches `JUNGLE.EXE`, which gets far enough to put
up **its own dialog**:

> Timon and Pumbaa's Jungle Games — This program requires 256 colors or more to run.

That is the game's code, drawn through Windows. Everything up to the renderer works: DOS, Windows,
the loader, the DLL imports, and the game's own start-up.

Two things had to be solved to get there, and both are worth keeping:

- **Windows will not start under FreeDOS.** `WIN.COM` rejects the reported DOS version, FreeDOS 1.3
  reports 7.10, the kernel has no `VERSION=` directive and only a stub `SETVER.SYS` with no
  `SETVER.EXE`. The version check lives in `WIN.COM`, which is only a loader — so invoking
  `SYSTEM\DOSX.EXE` directly starts standard mode with no version gate at all.
- **`shell=` in `SYSTEM.INI`** launches the game instead of Program Manager, with `C:\JUNGLE` on the
  `PATH` so its DLLs resolve, while the working directory stays `C:\WINDOWS` so Windows can find
  `DISPLAY.DRV`. Getting that the wrong way round produces `Error loading DISPLAY.DRV`.

### The open blocker: 256 colours

QEMU emulates a standard VGA or a Cirrus GD5446. **Windows 3.1 shipped no 8-bit driver for either.**

| Attempt | Outcome |
|---|---|
| `SVGA256.DRV` from the Windows media | `Error loading svga256.drv`. It supports Video 7, Tseng, Paradise, Trident and ATI — none of which QEMU emulates. |
| Period Cirrus 5430 drivers (archive.org) | Sealed in Cirrus `ARCV` containers. MS `EXPAND` passes them through unchanged and the self-extractor yields the same `.$00` files. |
| Driving the Cirrus `INSTALL.EXE` with `sendkey` | Reaches its UI, then refuses: *"This Dell Computer Software Support diskette does not include support for this system."* It checks for Dell hardware. |
| Generic VGA-6000 driver floppy | Packed the same way, same vendor-installer requirement. |

This is not a bug to fix; it is a missing driver. The realistic routes are:

1. **An unbranded Cirrus GD54xx Windows 3.1 driver** whose files can be copied straight into
   `WINDOWS\SYSTEM` and referenced from `SYSTEM.INI`, skipping vendor hardware detection. Run QEMU
   with `-vga cirrus` to match.
2. **86Box or PCem** instead of QEMU, emulating a chipset Windows 3.1 drives natively at 8bpp. Both
   are scriptable, but neither is installed and both need a BIOS set.
3. **Unpacking `ARCV`.** The header is legible — magic `ARCV`, a version word, then per file a
   length-prefixed name (`5430.drv`), uncompressed size and compressed size — but the compression
   itself is undocumented.

Until one lands there is still **no reference frame of the game**, and every pixel claim in this
project remains internal consistency only.
