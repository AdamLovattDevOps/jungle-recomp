#!/usr/bin/env python3
"""Parse the "7L" resource-file header, per RESOPENFILE in JUNGR01.DLL.

Layout recovered by decompilation, not guesswork:

  0x00  u16  magic       0x4C37 ("7L")
  0x02  u16  headerSize  total header length in bytes (286 observed)
  0x54  u16  version     must be 2
  0xBE  6 x {u32 fileOffset, u32 byteLength}  the six resource tables

Validity: the 8-bit sum of all headerSize bytes must be zero, and table 1's
length must be non-zero. RESOPENFILE rejects the file otherwise, so any file
that passes these checks is parsed exactly as the engine parses it.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINDIR = ROOT / "orig" / "cd" / "JUNGLE"

MAGIC = 0x4C37
OFF_VERSION = 0x54
OFF_TABLES = 0xBE
N_TABLES = 6
# Entry stride for table 1 is 10 bytes (RESOPENFILE multiplies count by 10).
# The others are not yet recovered.
STRIDE = {0: 10}
TABLE_NAMES = ["table1", "table2", "table3", "table4", "table5", "table6"]


def parse(data):
    magic, hdr_size = struct.unpack_from("<HH", data, 0)
    if magic != MAGIC:
        raise ValueError("bad magic %#06x" % magic)
    version = struct.unpack_from("<H", data, OFF_VERSION)[0]
    checksum = sum(data[:hdr_size]) & 0xFF
    tables = []
    for i in range(N_TABLES):
        off, length = struct.unpack_from("<II", data, OFF_TABLES + i * 8)
        tables.append((off, length))
    return {
        "magic": magic,
        "hdr_size": hdr_size,
        "version": version,
        "checksum": checksum,
        "tables": tables,
    }


def main():
    files = sorted(BINDIR.glob("*.BIN"))
    print("%-13s %6s %4s %4s  %s" % ("file", "hdrsz", "ver", "cksum", "tables as length@offset"))
    ok = 0
    for f in files:
        data = f.read_bytes()
        try:
            h = parse(data)
        except ValueError as e:
            print("%-13s %s" % (f.name, e))
            continue
        good = h["checksum"] == 0 and h["version"] == 2 and h["tables"][0][1] != 0
        ok += good
        cells = " ".join("%d@%d" % (n, o) for o, n in h["tables"])
        print("%-13s %6d %4d %5d  %s %s"
              % (f.name, h["hdr_size"], h["version"], h["checksum"],
                 "OK " if good else "BAD", cells))

        # sanity: do the table offsets land inside the file, in order?
        offs = [o for o, n in h["tables"] if n]
        if offs and (offs != sorted(offs) or max(offs) >= len(data)):
            print("%-13s   WARNING: table offsets not ascending or out of range" % "")
    print("\n%d/%d files validate against the engine's own checks" % (ok, len(files)))


if __name__ == "__main__":
    main()
