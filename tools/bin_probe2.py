#!/usr/bin/env python3
"""Second-pass structural probe of the "7L" container.

Answers three questions cheaply:
  1. Is the directory at the tail rather than the head?
  2. Does the magic repeat (per-chunk headers)?
  3. Where is the file compressed and where is it structured (entropy profile)?
"""
import math
import struct
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINDIR = ROOT / "orig" / "cd" / "JUNGLE"
NOTES = ROOT / "notes"
MAGIC = b"7L\x1e\x01"


def entropy(b):
    if not b:
        return 0.0
    c = Counter(b)
    n = len(b)
    return -sum((v / n) * math.log2(v / n) for v in c.values())


def find_all(data, pat):
    out, i = [], data.find(pat)
    while i != -1:
        out.append(i)
        i = data.find(pat, i + 1)
    return out


def main():
    NOTES.mkdir(exist_ok=True)
    log = (NOTES / "bin-probe2.txt").open("w")
    print("%-13s %9s %6s %6s  %s" % ("file", "size", "magics", "lowH", "entropy head/mid/tail"))
    for f in sorted(BINDIR.glob("*.BIN")):
        data = f.read_bytes()
        n = len(data)
        magics = find_all(data, MAGIC)
        # entropy in 64 KiB blocks; count blocks that look structured (< 7.0 bits)
        blocks = [entropy(data[i:i + 65536]) for i in range(0, n, 65536)]
        low = sum(1 for e in blocks if e < 7.0)
        head, mid, tail = blocks[0], blocks[len(blocks) // 2], blocks[-1]
        print("%-13s %9d %6d %6d  %.2f / %.2f / %.2f" % (f.name, n, len(magics), low, head, mid, tail))
        log.write("=== %s  %d bytes ===\n" % (f.name, n))
        log.write("magic at: %s\n" % (magics[:20],))
        log.write("last 64 bytes: %s\n" % data[-64:].hex(" ", 1))
        log.write("last 64 ascii: |%s|\n" % "".join(chr(c) if 32 <= c < 127 else "." for c in data[-64:]))
        log.write("u32 tail: %s\n" % list(struct.unpack_from("<16I", data, n - 64)))
        log.write("block entropy: %s\n\n" % " ".join("%.1f" % e for e in blocks))
    log.close()
    print("\ndetail -> notes/bin-probe2.txt")


if __name__ == "__main__":
    main()
