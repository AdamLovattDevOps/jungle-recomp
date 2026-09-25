#!/usr/bin/env python3
"""Report matching-decompilation progress: percent of bytes matched per segment.

Compares anything present in build/<MODULE>/seg<N>.bin against the extracted
reference, masking link-time fixups. Segments with no build output yet count as
0% rather than being skipped, so the total reflects the real state of the
project rather than only the parts already attempted.

This is the project's only honest progress metric.

Usage:  progress.py
"""
import json
from pathlib import Path

from matchdiff import compare

ROOT = Path(__file__).resolve().parent.parent
REF = ROOT / "reference"
BUILD = ROOT / "build"


def main():
    if not REF.exists():
        raise SystemExit("no reference/ — run ne_extract.py first")
    total = matched = 0
    print("%-16s %-6s %9s %9s %8s" % ("module", "seg", "bytes", "matched", "pct"))
    for mod in sorted(REF.iterdir()):
        if not mod.is_dir():
            continue
        for ref in sorted(mod.glob("seg*.bin")):
            rb = ref.read_bytes()
            rel_path = Path(str(ref).replace(".bin", ".relocs.json"))
            relocs = json.loads(rel_path.read_text()) if rel_path.exists() else []
            built = BUILD / mod.name / ref.name
            if built.exists():
                r = compare(rb, built.read_bytes(), relocs)
                total += r["compared"]
                matched += r["same"]
                pct = r["pct"]
                got = r["same"]
            else:
                from matchdiff import masked_positions
                n = len(rb) - len(masked_positions(rb, relocs))
                total += n
                pct, got = 0.0, 0
            print("%-16s %-6s %9d %9d %7.2f%%"
                  % (mod.name, ref.stem, len(rb), got, pct))
    print("\nTOTAL  %d of %d comparable bytes matched  ->  %.3f%%"
          % (matched, total, 100.0 * matched / total if total else 0.0))
    if matched == 0:
        print("\nNo build output yet. Populate build/<MODULE>/seg<N>.bin once the")
        print("period toolchain is available; this number is the goal to drive up.")


if __name__ == "__main__":
    main()
