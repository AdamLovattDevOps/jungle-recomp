#!/usr/bin/env python3
"""Compare a freshly built segment against the original — the matching metric.

Relocations are the subtlety. A freshly linked object holds different fixup
values at the same byte positions as the original, so those bytes would read as
mismatches even when the code is identical. They must be masked.

NE has two relocation forms, and only handling the first is wrong:

  ADDITIVE   (flags & 4)  the listed offset is the single site to fix up.
  CHAINED    (default)    the listed offset is the HEAD of a linked list. The
                          u16 stored at each site is the offset of the next
                          site, terminated by 0xFFFF. One relocation record can
                          therefore cover many byte positions across a segment.

Masking only the record offsets would leave every chained site counted as a
mismatch, understating a real match. This follows the chains.

Usage:  matchdiff.py REFERENCE.bin BUILT.bin [--relocs R.json] [--context N]
        matchdiff.py --selftest
"""
import json
import struct
import sys
from pathlib import Path

# Relocation address types -> bytes occupied at the fixup site.
RELOC_WIDTH = {0: 1, 2: 4, 3: 2, 5: 2, 11: 6, 13: 4}
DEFAULT_WIDTH = 2
FLAG_ADDITIVE = 4
CHAIN_END = 0xFFFF


def masked_positions(data, relocs):
    """Byte offsets whose value depends on link-time fixups."""
    mask = set()
    for r in relocs:
        width = RELOC_WIDTH.get(r["type"], DEFAULT_WIDTH)
        off = r["offset"]
        if r["flags"] & FLAG_ADDITIVE:
            mask.update(range(off, min(off + width, len(data))))
            continue
        # chained: walk the linked list stored in the segment itself
        seen = set()
        while off != CHAIN_END and 0 <= off + 1 < len(data) and off not in seen:
            seen.add(off)
            mask.update(range(off, min(off + width, len(data))))
            off = struct.unpack_from("<H", data, off)[0]
    return mask


def compare(ref, built, relocs):
    mask = masked_positions(ref, relocs)
    n = min(len(ref), len(built))
    same = diff = 0
    first = []
    for i in range(n):
        if i in mask:
            continue
        if ref[i] == built[i]:
            same += 1
        else:
            diff += 1
            if len(first) < 12:
                first.append((i, ref[i], built[i]))
    return {
        "ref_len": len(ref), "built_len": len(built),
        "compared": same + diff, "same": same, "diff": diff,
        "masked": len(mask), "first": first,
        "pct": 100.0 * same / (same + diff) if same + diff else 0.0,
    }


def selftest():
    """A differ that always reports 100% is worthless; prove it discriminates."""
    ref = bytes(range(256)) * 4
    relocs = [{"type": 3, "flags": FLAG_ADDITIVE, "offset": 10, "target": 0}]
    ok = compare(ref, ref, relocs)
    assert ok["pct"] == 100.0 and ok["diff"] == 0, ok
    bad = bytearray(ref)
    bad[500] ^= 0xFF
    r2 = compare(ref, bytes(bad), relocs)
    assert r2["diff"] == 1 and r2["pct"] < 100.0, r2
    # a byte inside a masked reloc must NOT count as a mismatch
    m = bytearray(ref)
    m[10] ^= 0xFF
    r3 = compare(ref, bytes(m), relocs)
    assert r3["diff"] == 0, r3
    print("selftest passed: identical=100%%, one-byte change detected, "
          "masked reloc byte ignored (masked=%d)" % ok["masked"])


def main():
    if "--selftest" in sys.argv:
        selftest()
        return
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    ref = Path(sys.argv[1]).read_bytes()
    built = Path(sys.argv[2]).read_bytes()
    relocs = []
    if "--relocs" in sys.argv:
        relocs = json.loads(Path(sys.argv[sys.argv.index("--relocs") + 1]).read_text())
    else:
        guess = Path(sys.argv[1]).with_suffix("").with_suffix(".relocs.json")
        alt = Path(str(Path(sys.argv[1])).replace(".bin", ".relocs.json"))
        if alt.exists():
            relocs = json.loads(alt.read_text())

    r = compare(ref, built, relocs)
    print("reference %d bytes, built %d bytes" % (r["ref_len"], r["built_len"]))
    print("masked by relocations: %d" % r["masked"])
    print("compared %d: %d match, %d differ  ->  %.2f%%"
          % (r["compared"], r["same"], r["diff"], r["pct"]))
    for off, a, b in r["first"]:
        print("  %#06x  ref %02x  built %02x" % (off, a, b))


if __name__ == "__main__":
    main()
