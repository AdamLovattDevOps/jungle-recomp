#!/usr/bin/env python3
"""Per-function targeting for the matching decompilation.

Ghidra reports addresses as `selector:offset`. For these NE modules the
selectors run 0x1000, 0x1008, 0x1010, ... — segment n is 0x1000 + 8*(n-1).
Selectors past the module's segment count address import thunks, not code in
this binary, and are excluded.

Every function is validated to lie wholly inside its segment; anything that
does not is reported rather than silently trusted.

Produces a work order for matching: leaf functions first (they call nothing, so
they can be matched without any other function existing), smallest first.

Usage:  funcs.py [MODULE]        e.g. funcs.py JUNGR01.DLL
        funcs.py --all
"""
import csv
import json
import sys
from pathlib import Path

from ne_info import parse

ROOT = Path(__file__).resolve().parent.parent
BINDIR = ROOT / "orig" / "cd" / "JUNGLE"
DECOMP = ROOT / "notes" / "decomp"
OUT = ROOT / "reference"
MODULES = ["JUNGR01.DLL", "JUNGA01.DLL", "JUNGS01.DLL", "JUNGU01.DLL", "JUNGLE.EXE"]


def seg_for(selector, nsegs):
    """Segment number for a Ghidra selector, or None if it is an import thunk."""
    if selector < 0x1000 or (selector - 0x1000) % 8:
        return None
    n = (selector - 0x1000) // 8 + 1
    return n if 1 <= n <= nsegs else None


def load(module):
    info = parse(BINDIR / module)
    segs = {s["n"]: s for s in info["segs"]}
    rows = list(csv.DictReader((DECOMP / (module + ".csv")).open()))

    funcs, external, oob = [], 0, []
    for r in rows:
        sel_s, _, off_s = r["addr"].partition(":")
        selector, off = int(sel_s, 16), int(off_s, 16)
        n = seg_for(selector, len(segs))
        if n is None:
            external += 1
            continue
        size = int(r["size"])
        if off + size > segs[n]["len"]:
            oob.append((r["name"], n, off, size, segs[n]["len"]))
            continue
        funcs.append({
            "name": r["name"], "seg": n, "offset": off, "size": size,
            "calls": int(r["calls"]), "called_by": int(r["called_by"]),
            "code": segs[n]["code"],
        })
    return info, funcs, external, oob


def work_order(funcs):
    """Leaf functions first, smallest first — matchable without dependencies."""
    return sorted(funcs, key=lambda f: (f["calls"] != 0, f["size"]))


def main():
    mods = MODULES if "--all" in sys.argv else [
        a for a in sys.argv[1:] if not a.startswith("--")] or ["JUNGR01.DLL"]
    for module in mods:
        info, funcs, external, oob = load(module)
        code = [f for f in funcs if f["code"]]
        total = sum(f["size"] for f in code)
        print("%s: %d functions in-segment (%d bytes of code), %d import thunks skipped"
              % (module, len(funcs), total, external))
        if oob:
            print("  WARNING: %d functions fall outside their segment:" % len(oob))
            for nm, n, off, sz, ln in oob[:5]:
                print("    %-28s seg%d off=%#x size=%d > seglen=%d" % (nm, n, off, sz, ln))

        d = OUT / module.replace(".", "_")
        d.mkdir(parents=True, exist_ok=True)
        (d / "functions.json").write_text(json.dumps(funcs, indent=1))

        if len(mods) == 1:
            order = work_order(code)
            leaves = [f for f in order if f["calls"] == 0]
            print("\n  leaf functions (call nothing): %d of %d — start here" % (len(leaves), len(code)))
            print("  %-34s %-5s %-8s %5s" % ("name", "seg", "offset", "size"))
            for f in order[:14]:
                print("  %-34s %-5d %-8s %5d"
                      % (f["name"][:34], f["seg"], hex(f["offset"]), f["size"]))
        print()


if __name__ == "__main__":
    main()
