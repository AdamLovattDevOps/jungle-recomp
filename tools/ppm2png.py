#!/usr/bin/env python3
"""Convert QEMU screendump PPM frames to PNG, and report what is on them.

QEMU writes P6 binary PPM. This keeps the dependency surface at zero by writing
the PNG with zlib rather than pulling in an imaging library.

Usage:  ppm2png.py IN.ppm [OUT.png]
"""
import struct
import sys
import zlib
from pathlib import Path
from collections import Counter


def read_ppm(path):
    d = Path(path).read_bytes()
    if not d.startswith(b"P6"):
        raise SystemExit("%s: not a P6 PPM" % path)
    fields, p = [], 2
    while len(fields) < 3:
        while p < len(d) and d[p:p + 1].isspace():
            p += 1
        if d[p:p + 1] == b"#":
            while d[p:p + 1] not in (b"\n", b""):
                p += 1
            continue
        q = p
        while q < len(d) and not d[q:q + 1].isspace():
            q += 1
        fields.append(int(d[p:q])); p = q
    p += 1
    w, h, _maxv = fields
    return w, h, d[p:p + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    Path(path).write_bytes(png)


def main():
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit(".", 1)[0] + ".png"
    w, h, rgb = read_ppm(src)
    write_png(dst, w, h, rgb)
    px = [rgb[i:i + 3] for i in range(0, len(rgb), 3)]
    c = Counter(px)
    nonblack = sum(n for col, n in c.items() if col != b"\x00\x00\x00")
    print("%s -> %s  %dx%d  %d distinct colours, %.1f%% non-black"
          % (src, dst, w, h, len(c), 100.0 * nonblack / len(px)))
    for col, n in c.most_common(4):
        print("    #%02x%02x%02x  %5.1f%%" % (col[0], col[1], col[2], 100.0 * n / len(px)))


if __name__ == "__main__":
    main()
