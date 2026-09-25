#!/usr/bin/env python3
"""Probe the 7th Level "7L" container header.

Compares the first N bytes of every .BIN across the disc, field by field, and
reports which u16/u32 slots are constant (format constants) and which scale
with file size (offsets / counts). Full detail goes to notes/, only a short
summary is printed.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINDIR = ROOT / "orig" / "cd" / "JUNGLE"
NOTES = ROOT / "notes"
HDR = 128  # bytes of header to compare


def load():
    files = sorted(BINDIR.glob("*.BIN"))
    return [(f.name, f.stat().st_size, f.read_bytes()[:HDR]) for f in files]


def u32s(b):
    return list(struct.unpack_from("<%dI" % (len(b) // 4), b, 0))


def main():
    rows = load()
    if not rows:
        sys.exit("no .BIN files found in %s" % BINDIR)

    NOTES.mkdir(exist_ok=True)
    tab = {name: u32s(hdr) for name, _, hdr in rows}
    sizes = {name: sz for name, sz, _ in rows}
    nslots = HDR // 4

    constant, varying = [], []
    for i in range(nslots):
        vals = {tab[n][i] for n in tab}
        (constant if len(vals) == 1 else varying).append(i)

    # write full table to notes
    with (NOTES / "bin-header-table.txt").open("w") as fh:
        fh.write("slot(off)  " + "  ".join("%-10s" % n[:10] for n in tab) + "\n")
        for i in range(nslots):
            fh.write("u32[%02d]@%03x  " % (i, i * 4))
            fh.write("  ".join("%-10d" % tab[n][i] for n in tab) + "\n")
        fh.write("\nfilesize    " + "  ".join("%-10d" % sizes[n] for n in tab) + "\n")

    print("files: %d   header compared: %d bytes (%d u32 slots)" % (len(rows), HDR, nslots))
    print("magic: %r  version u32[1]=%d" % (rows[0][2][:2], tab[rows[0][0]][1]))
    print("constant slots : %s" % ", ".join("u32[%d]=%d" % (i, tab[rows[0][0]][i]) for i in constant[:12]))
    print("varying slots  : %s" % ", ".join("u32[%d]" % i for i in varying))
    print()
    # For each varying slot, show the smallest and largest file's value vs size
    small = min(rows, key=lambda r: r[1])[0]
    big = max(rows, key=lambda r: r[1])[0]
    print("%-10s %12s %12s   %s" % ("slot", small[:12], big[:12], "reads as"))
    print("%-10s %12d %12d   filesize" % ("size", sizes[small], sizes[big]))
    for i in varying:
        a, b = tab[small][i], tab[big][i]
        hint = ""
        if 0 < a < sizes[small] and 0 < b < sizes[big]:
            hint = "plausible offset"
        elif 0 < a < 10000 and 0 < b < 10000:
            hint = "plausible count"
        print("%-10s %12d %12d   %s" % ("u32[%d]" % i, a, b, hint))
    print("\nfull table -> notes/bin-header-table.txt")


if __name__ == "__main__":
    main()
