#!/usr/bin/env python3
"""Sweep compiler flag combinations until the emitted bytes match the original.

Matching decompilation is as much about finding the build flags as writing the
C. The body of a function usually matches early; prologue, epilogue, register
choice and instruction scheduling are what the flags decide. This brute-forces
the flag space against one reference function and reports exact matches.

Usage:  flagsweep.py <srcdir> <cmd-template> <reference-hex> <anchor-hex>

  cmd-template   file containing {FLAGS} where the cl flags belong
  reference-hex  the original function's bytes
  anchor-hex     a distinctive run inside the function, used to locate the
                 built copy in the fresh segment
"""
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

FLAG_SETS = [
    "/ASw /Gw /GEd /Ox", "/ASw /Gw /GEf /Ox", "/ASw /Gw /GEa /Ox",
    "/ASw /Gw /GEe /Ox", "/ASw /Gw /GEm /Ox", "/ASw /Gw /GEr /Ox",
    "/ASw /Gw /GEs /Ox", "/ASw /GD /GEd /Ox", "/ASw /GD /GEf /Ox",
    "/ASw /GD /GEe /Ox", "/ASw /GD /GEr /Ox", "/ASw /GD /GEs /Ox",
    "/ASw /GW /GEd /Ox", "/ASw /GW /GEf /Ox", "/ASw /GEd /GEf /Ox",
    "/ASw /Gw /GEdf /Ox", "/ASw /GD /GEdf /Ox", "/ASw /Gw /GEfd /Ox",
    "/ASw /Gd /GEd /Ox", "/ASw /Gw /GEd", 
]


def seg1(dll):
    if not Path(dll).exists():
        return b""
    d = Path(dll).read_bytes()
    if len(d) < 0x40:
        return b""
    ne = struct.unpack_from("<H", d, 0x3C)[0]
    segtab = struct.unpack_from("<H", d, ne + 0x22)[0]
    align = struct.unpack_from("<H", d, ne + 0x32)[0] or 9
    sector, length, _f, _m = struct.unpack_from("<HHHH", d, ne + segtab)
    return d[sector << align:(sector << align) + length]


def main():
    srcdir, tmpl, refhex, anchorhex = sys.argv[1:5]
    ref = bytes.fromhex(refhex)
    anchor = bytes.fromhex(anchorhex)
    template = Path(tmpl).read_text()
    src = Path(srcdir)
    cmdfile = src / "sweep.cmd"
    best = None

    for flags in FLAG_SETS:
        cmdfile.write_text(template.replace("{FLAGS}", flags))
        for stale in ("JUNGR01.DLL", "COUNTS.OBJ"):
            (src / stale).unlink(missing_ok=True)
        subprocess.run([str(ROOT / "tools" / "dosbuild.sh"), str(src), str(cmdfile)],
                       check=False, capture_output=True)
        body = seg1(src / "JUNGR01.DLL")
        i = body.find(anchor)
        if i < 0:
            print("%-26s  build failed or anchor absent" % flags)
            continue
        lead = refhex.index(anchorhex.replace(" ", "")) // 2 if anchorhex in refhex else 9
        built = body[i - lead:i - lead + len(ref)]
        diff = [k for k in range(min(len(ref), len(built))) if ref[k] != built[k]]
        tag = "*** EXACT ***" if not diff else ("reloc-only" if diff in ([1], [2], [1, 2]) else "%d diff" % len(diff))
        print("%-26s  %-14s %s" % (flags, tag, built.hex(" ")))
        if best is None or len(diff) < best[1]:
            best = (flags, len(diff))

    if best:
        print("\nbest: %s  (%d differing bytes)" % best)


if __name__ == "__main__":
    main()
