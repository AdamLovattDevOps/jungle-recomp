#!/usr/bin/env python3
"""Extract resources from a "7L" container and identify what they are.

RESLOADRESOURCE seeks to the entry's offset and reads `size` bytes; only when
handle+0x116 is set does it route through a transform (FUN_1000_0984). So the
first question is simply whether the stored bytes are already recognisable.

Usage:  res_extract.py FILE.BIN [--write]
"""
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

from res_dir import entries

ROOT = Path(__file__).resolve().parent.parent
BINDIR = ROOT / "orig" / "cd" / "JUNGLE"
OUT = ROOT / "assets_extracted"

SIGS = [
    (b"RIFF", "RIFF/WAV"),
    (b"BM", "BMP"),
    (b"MThd", "MIDI"),
    (b"FORM", "IFF"),
    (b"\x89PNG", "PNG"),
    (b"\xff\xd8\xff", "JPEG"),
]


def sniff(b):
    for sig, name in SIGS:
        if b.startswith(sig):
            return name
    # A Windows BITMAPINFOHEADER starts with its own size, 40.
    if len(b) >= 4 and struct.unpack_from("<I", b, 0)[0] == 40:
        return "BITMAPINFOHEADER"
    if len(b) >= 4 and struct.unpack_from("<I", b, 0)[0] == 12:
        return "BITMAPCOREHEADER"
    return None


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "JUNGOPTS.BIN"
    write = "--write" in sys.argv
    path = Path(name) if Path(name).exists() else BINDIR / name
    data = path.read_bytes()
    h, ents = entries(data)

    by_type = defaultdict(list)
    for e in ents:
        by_type[e["type"]].append(e)

    print("%s: %d entries, %d types" % (path.name, len(ents), len(by_type)))
    print("%-6s %6s %10s %10s  %-18s %s"
          % ("type", "count", "medsize", "maxsize", "sniff", "first 12 bytes"))

    for t in sorted(by_type):
        es = sorted(by_type[t], key=lambda e: e["size"])
        med = es[len(es) // 2]
        big = es[-1]
        sizes = [e["size"] for e in es]
        # sniff across a sample, not one entry, so a fluke does not mislead
        hits = Counter()
        for e in es[:200]:
            blob = data[e["offset"]:e["offset"] + 16]
            hits[sniff(blob) or "-"] += 1
        common = hits.most_common(1)[0]
        head = data[med["offset"]:med["offset"] + 12]
        print("%-6d %6d %10d %10d  %-18s %s"
              % (t, len(es), med["size"], max(sizes),
                 "%s(%d/%d)" % (common[0], common[1], min(200, len(es))),
                 head.hex(" ", 1)))

        if write:
            d = OUT / path.stem / ("type%02d" % t)
            d.mkdir(parents=True, exist_ok=True)
            for e in es[:20]:
                (d / ("%05d.bin" % e["i"])).write_bytes(
                    data[e["offset"]:e["offset"] + e["size"]])

    if write:
        print("\nsamples written -> %s" % (OUT / path.stem))


if __name__ == "__main__":
    main()
