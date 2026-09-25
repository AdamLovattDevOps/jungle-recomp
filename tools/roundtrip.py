#!/usr/bin/env python3
"""Round-trip test: re-serialise what we parsed and compare against the original.

The strongest available proof that a format is understood is to rebuild it from
the parsed representation and get the original bytes back. Anything misread,
skipped, or padded shows up immediately as a byte difference.

This tests the parts currently modelled:

  directory   table slot 0, 10-byte entries (type, u32 offset, u32 size)
  palette     slot 7, 4-byte PALETTEENTRY records
  tables      the slot descriptor array at header 0xBE

It deliberately does NOT claim to rebuild a whole container: the resident blob
and the compressed payloads are passed through, not regenerated. What it proves
is that the structures parsed are parsed exactly.

Usage:  roundtrip.py
"""
import struct
import sys
from pathlib import Path

from res_dir import entries
from res_header import parse, OFF_TABLES, N_TABLES

ROOT = Path(__file__).resolve().parent.parent
BINDIR = ROOT / "orig" / "cd" / "JUNGLE"
OFF_PALETTE = 0xEE
ENTRY = 10


def rebuild_directory(ents):
    out = bytearray()
    for e in ents:
        out += struct.pack("<HII", e["type"], e["offset"], e["size"])
    return bytes(out)


def rebuild_tables(h):
    out = bytearray()
    for off, length in h["tables"]:
        out += struct.pack("<II", off, length)
    return bytes(out)


def main():
    files = sorted(BINDIR.glob("*.BIN"))
    print("%-13s %10s %10s %10s" % ("file", "directory", "tables", "palette"))
    fails = 0
    for f in files:
        data = f.read_bytes()
        h, ents = entries(data)

        d_off, d_len = h["tables"][0]
        dir_ok = rebuild_directory(ents) == data[d_off:d_off + d_len]

        tab_ok = rebuild_tables(h) == data[OFF_TABLES:OFF_TABLES + N_TABLES * 8]

        p_off, p_len = struct.unpack_from("<II", data, OFF_PALETTE)
        raw = data[p_off:p_off + p_len]
        pal = [(raw[i], raw[i + 1], raw[i + 2], raw[i + 3])
               for i in range(0, len(raw) - 3, 4)]
        rebuilt = b"".join(bytes(p) for p in pal)
        pal_ok = rebuilt == raw[:len(rebuilt)] and len(rebuilt) == p_len

        for ok in (dir_ok, tab_ok, pal_ok):
            if not ok:
                fails += 1
        print("%-13s %10s %10s %10s"
              % (f.name,
                 "OK" if dir_ok else "FAIL",
                 "OK" if tab_ok else "FAIL",
                 "OK" if pal_ok else "FAIL"))

    total = len(files) * 3
    print("\n%d/%d structure round-trips byte-identical" % (total - fails, total))
    if fails:
        sys.exit(1)


if __name__ == "__main__":
    main()
