#!/usr/bin/env python3
"""Extract NE segments to raw .bin files — the reference bytes to match against.

A matching decompilation is verified by comparing freshly compiled output
against the original segment bytes. That comparison needs the originals split
out as plain files, one per segment, which is what this produces.

Segment bytes are taken verbatim; NE relocation records are written alongside
as JSON but NOT applied. Relocations matter because a freshly linked object
will have different fixup values in the same positions, so those byte ranges
must be masked out of any diff rather than counted as mismatches.

Output: reference/<MODULE>/seg<N>.bin and seg<N>.relocs.json

Usage:  ne_extract.py [MODULE ...]      (default: all five engine binaries)
"""
import json
import struct
import sys
from pathlib import Path

from ne_info import parse

ROOT = Path(__file__).resolve().parent.parent
BINDIR = ROOT / "orig" / "cd" / "JUNGLE"
OUT = ROOT / "reference"
MODULES = ["JUNGLE.EXE", "JUNGU01.DLL", "JUNGS01.DLL", "JUNGA01.DLL", "JUNGR01.DLL"]

# NE segment flag bit 8 (0x100) marks a segment as having relocation records,
# which follow immediately after the segment data.
SEG_HAS_RELOCS = 0x100


def relocs(data, seg):
    """Read the relocation table that follows a segment, if it has one."""
    if not (seg["flags"] & SEG_HAS_RELOCS):
        return []
    p = seg["file_off"] + seg["len"]
    if p + 2 > len(data):
        return []
    count = struct.unpack_from("<H", data, p)[0]
    p += 2
    out = []
    for _ in range(count):
        if p + 8 > len(data):
            break
        src, flags, off, target = struct.unpack_from("<BBHH", data, p)
        out.append({"type": src, "flags": flags, "offset": off, "target": target})
        p += 8
    return out


def main():
    mods = sys.argv[1:] or MODULES
    OUT.mkdir(exist_ok=True)
    print("%-13s %-5s %-6s %9s %8s  %s" % ("module", "seg", "kind", "bytes", "relocs", "output"))
    for m in mods:
        path = BINDIR / m
        if not path.exists():
            print("%-13s missing" % m)
            continue
        data = path.read_bytes()
        info = parse(path)
        d = OUT / m.replace(".", "_")
        d.mkdir(parents=True, exist_ok=True)
        for seg in info["segs"]:
            body = data[seg["file_off"]:seg["file_off"] + seg["len"]]
            rel = relocs(data, seg)
            name = "seg%d" % seg["n"]
            (d / (name + ".bin")).write_bytes(body)
            (d / (name + ".relocs.json")).write_text(json.dumps(rel, indent=1))
            print("%-13s %-5d %-6s %9d %8d  %s"
                  % (m, seg["n"], "CODE" if seg["code"] else "DATA",
                     len(body), len(rel), (d / (name + ".bin")).relative_to(ROOT)))
    print("\nreference bytes -> %s" % OUT.relative_to(ROOT))


if __name__ == "__main__":
    main()
