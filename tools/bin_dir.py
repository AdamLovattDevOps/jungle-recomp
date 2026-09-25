#!/usr/bin/env python3
"""Walk the "7L" container offset table at 0x60 and identify what it points at.

Prints one compact line per file, then a hexless preview of the first chunk.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINDIR = ROOT / "orig" / "cd" / "JUNGLE"
NOTES = ROOT / "notes"
TABLE_OFF = 0x60


def offsets(data):
    """Read ascending u32s at 0x60 until one is zero or not ascending/in-range."""
    out = []
    off = TABLE_OFF
    prev = 0
    while off + 4 <= len(data):
        v = struct.unpack_from("<I", data, off)[0]
        if v == 0 or v <= prev or v >= len(data):
            break
        out.append((off, v))
        prev = v
        off += 4
    return out


def sniff(data, at, n=16):
    b = data[at:at + n]
    printable = "".join(chr(c) if 32 <= c < 127 else "." for c in b)
    return b.hex(" ", 1), printable


def main():
    NOTES.mkdir(exist_ok=True)
    log = (NOTES / "bin-directory.txt").open("w")
    files = sorted(BINDIR.glob("*.BIN"))
    print("%-13s %8s %5s  %s" % ("file", "size", "ents", "first entries (offset:tag)"))
    for f in files:
        data = f.read_bytes()
        ents = offsets(data)
        tags = []
        for _, v in ents[:4]:
            hexs, txt = sniff(data, v, 8)
            tags.append("%d:%s" % (v, txt))
        print("%-13s %8d %5d  %s" % (f.name, len(data), len(ents), "  ".join(tags)))
        log.write("=== %s (%d bytes, %d entries) ===\n" % (f.name, len(data), len(ents)))
        for slot, v in ents:
            hexs, txt = sniff(data, v, 24)
            log.write("  @%04x -> %9d  %s  |%s|\n" % (slot, v, hexs, txt))
        # what lies between end of table and first entry?
        if ents:
            log.write("  gap: table ends %#x, first entry %#x\n\n" % (TABLE_OFF + 4 * len(ents), ents[0][1]))
    log.close()
    print("\nfull walk -> notes/bin-directory.txt")


if __name__ == "__main__":
    main()
