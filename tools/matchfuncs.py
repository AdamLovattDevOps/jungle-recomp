#!/usr/bin/env python3
"""Compare individual matched functions against the original, by name.

progress.py measures whole segments, which only becomes meaningful once a module
is largely transcribed. Early on the useful question is per function: does this
one match yet, and if not, where does it diverge.

Reference bytes come from reference/<MODULE>/seg<N>.bin at the offset and size
in functions.json. The built copy is located in the fresh DLL by its exported
name, via the entry table, so nothing depends on functions landing at the same
offsets as the original -- they will not until every preceding function matches.

Relocated words are masked: a fixup holds a different value in a fresh link, so
comparing them raw would report a mismatch on identical code.

Usage:  matchfuncs.py BUILT.DLL [MODULE]
"""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def ne_offsets(path):
    d = Path(path).read_bytes()
    ne = struct.unpack_from("<H", d, 0x3C)[0]
    return d, ne


def seg_body(d, ne, index=1):
    segtab = struct.unpack_from("<H", d, ne + 0x22)[0]
    align = struct.unpack_from("<H", d, ne + 0x32)[0] or 9
    sector, length, _f, _m = struct.unpack_from("<HHHH", d, ne + segtab + (index - 1) * 8)
    return d[sector << align:(sector << align) + length]


def exports(path):
    """name -> (segment, offset) from the entry table, resolved via name tables."""
    d, ne = ne_offsets(path)
    enttab = struct.unpack_from("<H", d, ne + 0x04)[0]
    # ordinal -> (seg, off)
    ents, p, ordinal = {}, ne + enttab, 1
    while p < len(d):
        cnt = d[p]
        if cnt == 0:
            break
        kind = d[p + 1]
        p += 2
        for _ in range(cnt):
            if kind == 0:
                continue
            if kind == 0xFF:
                seg, off = d[p + 3], struct.unpack_from("<H", d, p + 4)[0]
                p += 6
            else:
                seg, off = kind, struct.unpack_from("<H", d, p + 1)[0]
                p += 3
            ents[ordinal] = (seg, off)
            ordinal += 1
    # names
    out = {}
    cbnres = struct.unpack_from("<H", d, ne + 0x20)[0]
    restab = struct.unpack_from("<H", d, ne + 0x26)[0]
    nrestab = struct.unpack_from("<I", d, ne + 0x2C)[0]
    for base, limit, resident in ((ne + restab, None, True), (nrestab, cbnres, False)):
        q, end, first = base, base + (limit if limit else 4096), True
        while q < end:
            n = d[q]
            if n == 0:
                break
            nm = d[q + 1:q + 1 + n].decode("ascii", "replace")
            q += 1 + n
            o = struct.unpack_from("<H", d, q)[0]
            q += 2
            if (not first or not resident) and o in ents:
                out[nm.upper()] = ents[o]
            first = False
    return out


def reloc_mask(module, seg_index, length):
    """Byte positions covered by a relocation, walking the chains."""
    import sys as _s
    _s.path.insert(0, str(ROOT / "tools"))
    from ne_imports import parse
    disc = ROOT / "orig" / "cd" / "JUNGLE" / (module.replace("_DLL", ".DLL").replace("_EXE", ".EXE"))
    mask = bytearray(length)
    if not disc.exists():
        return mask
    img = parse(str(disc))
    data = disc.read_bytes()
    for s in img["segs"]:
        if s["i"] != seg_index:
            continue
        body = data[s["off"]:s["off"] + s["len"]]
        for r in s["relocs"]:
            off, seen = r["offset"], set()
            while off != 0xFFFF and off + 2 <= len(body) and off not in seen:
                seen.add(off)
                for k in range(off, min(off + 4, length)):
                    mask[k] = 1
                off = struct.unpack_from("<H", body, off)[0]
    return mask


def main():
    built = sys.argv[1]
    module = sys.argv[2] if len(sys.argv) > 2 else "JUNGR01_DLL"
    fns = json.load(open(ROOT / "reference" / module / "functions.json"))
    ref_seg1 = (ROOT / "reference" / module / "seg1.bin").read_bytes()
    mask = reloc_mask(module, 1, len(ref_seg1))

    d, ne = ne_offsets(built)
    body = seg_body(d, ne, 1)
    exp = exports(built)

    rows, matched, total = [], 0, 0
    for f in fns:
        nm = f["name"].upper()
        if nm not in exp or f.get("seg") != 1 or not f.get("code"):
            continue
        _seg, off = exp[nm]
        ref = ref_seg1[f["offset"]:f["offset"] + f["size"]]
        got = body[off:off + f["size"]]
        if len(got) < len(ref):
            rows.append((nm, f["size"], "short", 0)); continue
        diff = [k for k in range(len(ref))
                if ref[k] != got[k] and not mask[f["offset"] + k]]
        total += f["size"]
        if not diff:
            matched += f["size"]
        rows.append((nm, f["size"], "MATCH" if not diff else "%d diff" % len(diff), len(diff)))

    print("%-24s %6s  %s" % ("function", "bytes", "result"))
    for nm, sz, res, _ in sorted(rows, key=lambda r: (r[3] != 0, r[0])):
        print("%-24s %6d  %s" % (nm, sz, res))
    print("\n%d of %d attempted bytes match (%.1f%%)"
          % (matched, total, 100.0 * matched / total if total else 0))


if __name__ == "__main__":
    main()
