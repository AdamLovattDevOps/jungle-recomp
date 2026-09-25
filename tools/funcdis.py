#!/usr/bin/env python3
"""Print one function's original bytes and disassembly, ready to match against.

Looks the function up in reference/<MODULE>/functions.json, slices it out of
the extracted segment, and disassembles it as 16-bit x86. Relocated byte
positions are flagged, because those are the ones a fresh link will legitimately
fill differently — they must not be treated as code to reproduce literally.

Usage:  funcdis.py MODULE FUNCTION
        funcdis.py JUNGR01.DLL RESCOUNTSTRINGS
"""
import json
import subprocess
import sys
from pathlib import Path

from matchdiff import masked_positions

ROOT = Path(__file__).resolve().parent.parent
REF = ROOT / "reference"


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    module, want = sys.argv[1], sys.argv[2]
    d = REF / module.replace(".", "_")
    funcs = json.loads((d / "functions.json").read_text())
    match = [f for f in funcs if f["name"] == want or f["name"].startswith(want)]
    if not match:
        sys.exit("no function %r in %s" % (want, module))
    f = match[0]

    seg = d / ("seg%d.bin" % f["seg"])
    body = seg.read_bytes()
    rel_path = Path(str(seg).replace(".bin", ".relocs.json"))
    relocs = json.loads(rel_path.read_text()) if rel_path.exists() else []
    masked = masked_positions(body, relocs)

    lo, hi = f["offset"], f["offset"] + f["size"]
    in_func = sorted(p for p in masked if lo <= p < hi)

    print("%s  %s  seg%d:%04x  %d bytes  (calls %d, called by %d)"
          % (module, f["name"], f["seg"], f["offset"], f["size"],
             f["calls"], f["called_by"]))
    if in_func:
        print("relocated byte offsets within this function: %s"
              % " ".join(hex(p - lo) for p in in_func[:24]))
    else:
        print("no relocations inside this function — fully self-contained")
    print()
    out = subprocess.run(
        ["r2", "-q", "-a", "x86", "-b", "16", "-e", "scr.color=0",
         "-c", "s %d; pD %d" % (f["offset"], f["size"]), str(seg)],
        capture_output=True, text=True, timeout=60)
    print(out.stdout or out.stderr)


if __name__ == "__main__":
    main()
