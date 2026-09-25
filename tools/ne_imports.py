#!/usr/bin/env python3
"""Resolve NE imported-ordinal relocations to MODULE.ordinal.

The existing reference dumps in reference/*/seg*.relocs.json record a
relocation's address type, relocation type and first target word, but drop the
*second* target word -- which for an IMPORTORDINAL fixup is the ordinal itself.
Without it an imported call cannot be named, so Ghidra shows S_009 / S_034 and
the call site stays anonymous.

NE relocation record, 8 bytes:

  u8   address type   0 LOBYTE, 2 SEGMENT, 3 FAR PTR, 5 OFFSET, 11 PTR48
  u8   relocation type  0 INTERNALREF, 1 IMPORTORDINAL, 2 IMPORTNAME,
                        3 OSFIXUP; bit 2 (0x04) = ADDITIVE
  u16  offset within the segment
  u16  target 1        module reference index, for the import types
  u16  target 2        ORDINAL for IMPORTORDINAL, name-table offset for
                       IMPORTNAME

Usage:  ne_imports.py FILE.NE [--seg N] [--at SEG:OFF ...]
"""
import struct
import sys
from pathlib import Path

ADDR_TYPE = {0: "LOBYTE", 2: "SEGMENT", 3: "FARPTR", 5: "OFFSET", 11: "PTR48", 13: "OFF32"}
REL_TYPE = {0: "INTERNAL", 1: "ORDINAL", 2: "NAME", 3: "OSFIXUP"}


def pstr(data, off):
    """Length-prefixed Pascal string."""
    n = data[off]
    return data[off + 1:off + 1 + n].decode("ascii", "replace")


def parse(path):
    data = Path(path).read_bytes()
    ne = struct.unpack_from("<H", data, 0x3C)[0]
    if data[ne:ne + 2] != b"NE":
        raise SystemExit("%s: not an NE image" % path)

    cseg, cmod = struct.unpack_from("<HH", data, ne + 0x1C)
    segtab, _rsrc, restab, modtab, imptab = struct.unpack_from("<HHHHH", data, ne + 0x22)
    align = struct.unpack_from("<H", data, ne + 0x32)[0] or 9

    # Module reference table: cmod u16 offsets into the imported names table.
    modules = []
    for i in range(cmod):
        noff = struct.unpack_from("<H", data, ne + modtab + i * 2)[0]
        modules.append(pstr(data, ne + imptab + noff))

    segs = []
    for i in range(cseg):
        sector, length, flags, minalloc = struct.unpack_from("<HHHH", data, ne + segtab + i * 8)
        segs.append({
            "i": i + 1,
            "off": sector << align,
            "len": length,
            "flags": flags,
            "reloc": bool(flags & 0x0100),
        })

    for s in segs:
        s["relocs"] = []
        if not s["reloc"] or not s["len"]:
            continue
        p = s["off"] + s["len"]
        count = struct.unpack_from("<H", data, p)[0]
        p += 2
        for _ in range(count):
            at, rt, off, t1, t2 = struct.unpack_from("<BBHHH", data, p)
            p += 8
            rec = {
                "addr_type": ADDR_TYPE.get(at, hex(at)),
                "rel_type": REL_TYPE.get(rt & 3, hex(rt)),
                "additive": bool(rt & 4),
                "offset": off,
                "t1": t1,
                "t2": t2,
            }
            if (rt & 3) == 1:
                rec["module"] = modules[t1 - 1] if 0 < t1 <= len(modules) else "?%d" % t1
                rec["ordinal"] = t2
                rec["symbol"] = "%s.%d" % (rec["module"], t2)
            elif (rt & 3) == 2:
                rec["module"] = modules[t1 - 1] if 0 < t1 <= len(modules) else "?%d" % t1
                rec["symbol"] = "%s.%s" % (rec["module"], pstr(data, ne + imptab + t2))
            s["relocs"].append(rec)
    return {"modules": modules, "segs": segs, "name": Path(path).name}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    want = [a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--at=")]
    only = [a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--seg=")]
    if not args:
        raise SystemExit(__doc__)

    for path in args:
        img = parse(path)
        print("=== %s   modules: %s" % (img["name"], ", ".join(img["modules"])))
        for s in img["segs"]:
            if only and str(s["i"]) not in only:
                continue
            imports = [r for r in s["relocs"] if r["rel_type"] in ("ORDINAL", "NAME")]
            if not imports:
                continue
            print("  seg %d  %d relocs, %d imported" % (s["i"], len(s["relocs"]), len(imports)))
            if want:
                for w in want:
                    sg, _, off = w.partition(":")
                    if sg and str(s["i"]) != sg:
                        continue
                    o = int(off, 0)
                    hit = [r for r in imports if r["offset"] == o]
                    for r in hit:
                        print("    %04x  %-10s %s" % (r["offset"], r["addr_type"], r["symbol"]))
                continue
            tally = {}
            for r in imports:
                tally[r["symbol"]] = tally.get(r["symbol"], 0) + 1
            for sym, n in sorted(tally.items(), key=lambda kv: -kv[1]):
                print("    %4d  %s" % (n, sym))


if __name__ == "__main__":
    main()
