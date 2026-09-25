#!/usr/bin/env python3
"""Decode "7L" type-1 bitmaps to PNG.

Bitmap header (20 bytes), per FUN_1000_02e8 / FUN_1000_0230 in JUNGR01.DLL:

  0x00  u16  headerSize    always 20
  0x02  u16  srcWidth
  0x04  u16  dstStride
  0x06  u16  height
  0x08  u16  flags         bit 0x8000 set => RLE compressed
  0x0A  i16  placement offset
  0x10  u32  data pointer  (runtime only; on disk the data starts at headerSize)

After the header come `height` u16 row offsets, then the RLE stream.

RLE, transcribed from FUN_1000_0230:

  b == 0x00           end of row; a row that begins with 0x00 ends the image
  0x01 <= b <= 0x7F   run: next byte repeated b times
  0x80 <= b <= 0xFE   literal: copy (0xFF - b) bytes verbatim
  b == 0xFF           literal: length is the next byte, then that many bytes

Palette, per RESLOADPALETTE: u32 fileOffset at header 0xEE, u32 byteLength at
0xF2, 4 bytes per entry.

Usage:  res_bitmap.py FILE.BIN [--limit N] [--all]
"""
import struct
import sys
import zlib
from pathlib import Path

from res_dir import entries
from res_header import parse as parse_header
import lz7l

ROOT = Path(__file__).resolve().parent.parent
BINDIR = ROOT / "orig" / "cd" / "JUNGLE"
OUT = ROOT / "assets_extracted"

OFF_PALETTE = 0xEE
HDR_LEN = 20
FLAG_RLE = 0x8000


def palette(data):
    off, length = struct.unpack_from("<II", data, OFF_PALETTE)
    if not length or off + length > len(data):
        return None
    raw = data[off:off + length]
    # 4 bytes per entry, stored R,G,B,flags — a Win16 PALETTEENTRY, not the
    # B,G,R,reserved order of an RGBQUAD.
    # 236 entries = 256 minus the 20 system colours Windows reserves, so the
    # stored palette maps to indices 10..245 and must be placed at that offset.
    ents = [(raw[i], raw[i + 1], raw[i + 2]) for i in range(0, len(raw) - 3, 4)]
    if len(ents) == 236:
        return [(0, 0, 0)] * 10 + ents + [(255, 255, 255)] * 10
    return ents


def unrle(src, width, height):
    """Expand one RLE stream into height rows of `width` bytes."""
    out = bytearray()
    p = 0
    row = bytearray()
    rows = 0
    n = len(src)
    while p < n and rows < height:
        b = src[p]
        p += 1
        if b == 0:
            row.extend(b"\x00" * max(0, width - len(row)))
            out.extend(row[:width])
            rows += 1
            row = bytearray()
            if p < n and src[p] == 0:
                break
            continue
        if b < 0x80:
            if p >= n:
                break
            row.extend(bytes([src[p]]) * b)
            p += 1
        else:
            ln = 0xFF - b
            if b == 0xFF:
                if p >= n:
                    break
                ln = src[p]
                p += 1
            row.extend(src[p:p + ln])
            p += ln
    while rows < height:
        out.extend(b"\x00" * width)
        rows += 1
    return bytes(out)


def decode(blob):
    hdr = struct.unpack_from("<6H", blob, 0)
    hdr_size, src_w, dst_w, height, flags, _ = hdr
    if hdr_size != HDR_LEN:
        return None
    width = dst_w or src_w
    body = blob[hdr_size:]
    if flags & FLAG_RLE:
        # skip the per-row offset table
        body = body[height * 2:]
        pix = unrle(body, width, height)
    else:
        # flags bit clear routes through FUN_1000_218c: a chunked LZW stream
        # that expands straight to pixels, with no row table.
        pix = lz7l.expand(blob, hdr_size, len(blob))
        pix = pix[:width * height].ljust(width * height, b"\x00")
    return width, height, pix


def write_png(path, width, height, pix, pal):
    # Stored bottom-up, like a Windows DIB; emit top-down for PNG.
    raw = bytearray()
    for y in range(height - 1, -1, -1):
        raw.append(0)  # filter: none
        raw.extend(pix[y * width:(y + 1) * width])

    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)
    plte = b"".join(bytes(c) for c in pal[:256])
    plte = plte.ljust(768, b"\x00")
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"PLTE", plte)
           + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))
    path.write_bytes(png)


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "JUNGOPTS.BIN"
    limit = 24
    if "--limit" in sys.argv:
        limit = int(sys.argv[sys.argv.index("--limit") + 1])
    if "--all" in sys.argv:
        limit = 10 ** 9

    path = Path(name) if Path(name).exists() else BINDIR / name
    data = path.read_bytes()
    pal = palette(data)
    if not pal:
        sys.exit("no palette found in %s" % path.name)
    print("%s: palette %d entries" % (path.name, len(pal)))

    _, ents = entries(data)
    t1 = [e for e in ents if e["type"] == 1]
    d = OUT / path.stem
    d.mkdir(parents=True, exist_ok=True)

    done = fail = 0
    for e in t1[:limit]:
        blob = data[e["offset"]:e["offset"] + e["size"]]
        try:
            r = decode(blob)
        except Exception:
            r = None
        if not r:
            fail += 1
            continue
        w, h, pix = r
        if not (0 < w <= 2048 and 0 < h <= 2048):
            fail += 1
            continue
        write_png(d / ("%05d_%dx%d.png" % (e["i"], w, h)), w, h, pix, pal)
        done += 1
    print("decoded %d, skipped %d, of %d type-1 resources -> %s"
          % (done, fail, len(t1), d))


if __name__ == "__main__":
    main()
