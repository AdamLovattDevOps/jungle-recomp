#!/usr/bin/env python3
"""Dump the "7L" resource directory (table 1).

Entry layout recovered from RESLOADRESOURCE in JUNGR01.DLL:

  0x00  u16  type
  0x02  u32  fileOffset
  0x06  u32  size          -> 10 bytes, matching RESOPENFILE's count * 10

RESLOADRESOURCE branches on type: types 8..16 inclusive are resolved from
already-resident memory blocks, everything else is seeked to and read from
the file. That split is the first thing to verify against real data.

Usage:  res_dir.py [FILE.BIN ...]      (default: all of them, summary only)
        res_dir.py --entries FILE.BIN  (per-entry listing)
"""
import struct
import sys
from collections import Counter
from pathlib import Path

from res_header import parse as parse_header

ROOT = Path(__file__).resolve().parent.parent
BINDIR = ROOT / "orig" / "cd" / "JUNGLE"
NOTES = ROOT / "notes"
ENTRY = 10


def entries(data):
    h = parse_header(data)
    off, length = h["tables"][0]
    out = []
    for i in range(length // ENTRY):
        t, lo, hi, slo, shi = struct.unpack_from("<HHHHH", data, off + i * ENTRY)
        out.append({
            "i": i,
            "type": t,
            "offset": lo | (hi << 16),
            "size": slo | (shi << 16),
        })
    return h, out


def check(data, ents):
    """How many entries point at a sane region of the file?"""
    n = len(data)
    inrange = sum(1 for e in ents if 0 < e["offset"] < n and e["offset"] + e["size"] <= n)
    resident = sum(1 for e in ents if 8 <= e["type"] <= 16)
    return inrange, resident


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    want_entries = "--entries" in sys.argv
    files = [Path(a) for a in args] if args else sorted(BINDIR.glob("*.BIN"))

    NOTES.mkdir(exist_ok=True)
    log = (NOTES / "res-directory.txt").open("w")
    types_all = Counter()

    print("%-13s %6s %7s %9s  %s" % ("file", "ents", "inrange", "payload", "types (type:count)"))
    for f in files:
        data = f.read_bytes()
        h, ents = entries(data)
        inrange, resident = check(data, ents)
        tc = Counter(e["type"] for e in ents)
        types_all.update(tc)
        payload = sum(e["size"] for e in ents if not (8 <= e["type"] <= 16))
        top = " ".join("%d:%d" % (t, c) for t, c in sorted(tc.items()))
        print("%-13s %6d %7d %9d  %s" % (f.name, len(ents), inrange, payload, top))

        log.write("=== %s ===\n" % f.name)
        log.write("entries=%d in-range=%d resident-type(8..16)=%d\n" % (len(ents), inrange, resident))
        for e in ents:
            log.write("  [%4d] type=%-3d off=%9d size=%9d\n"
                      % (e["i"], e["type"], e["offset"], e["size"]))
        log.write("\n")
        if want_entries:
            for e in ents[:40]:
                print("  [%4d] type=%-3d off=%9d size=%9d"
                      % (e["i"], e["type"], e["offset"], e["size"]))
    log.close()
    print("\nall types seen: %s" % " ".join("%d:%d" % t for t in sorted(types_all.items())))
    print("full listing -> notes/res-directory.txt")


if __name__ == "__main__":
    main()


# --- resource fetch -------------------------------------------------------
# RESLOADRESOURCE addresses resources two different ways, chosen by type:
#
#   types 8..16  the `offset` field indexes the RESIDENT BLOB — table 6, loaded
#                whole at open time by FUN_1000_066a as `segments` contiguous
#                chunks (count at header 0xA0, cumulative sizes at header 0x60).
#   all others   the `offset` field is a plain file offset, seeked and streamed.
#
# Verified: every type 9/10/13/14/15 resource lies inside the blob, and no
# type 1 or 7 resource does.

RESIDENT_TYPES = range(8, 17)


def resident_blob(data):
    """Return table 6, the preloaded resource blob."""
    from res_header import parse
    off, length = parse(data)["tables"][5]
    return data[off:off + length]


def fetch(data, entry, blob=None):
    """Return the raw bytes of one resource, resident or streamed."""
    if entry["type"] in RESIDENT_TYPES:
        if blob is None:
            blob = resident_blob(data)
        return blob[entry["offset"]:entry["offset"] + entry["size"]]
    return data[entry["offset"]:entry["offset"] + entry["size"]]
