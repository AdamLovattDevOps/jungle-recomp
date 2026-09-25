#!/usr/bin/env python3
"""The 7th Level ADPCM decoder, from FUN_1000_3f10 in JUNGA01.DLL.

Four-bit ADPCM, two samples per byte, low nibble first. It is IMA-derived but
not standard IMA, and a stock decoder produces wrong output.

What differs: the adaptation index is kept at 8x resolution. The index
adjustment table holds IMA's [-1,-1,-1,-1,2,4,6,8] multiplied by 8, the index
clamps to 0x2C0 = 704 = 88*8 (IMA's maximum index times 8), and the step table
is correspondingly interpolated to 711 entries rather than IMA's 89.

The magnitude then selects a step *relative to* the current index:

    delta = step[(nibble & 7) + index]

so the three magnitude bits walk up to seven entries along the interpolated
curve from wherever the index currently sits. Reconstruction is a plain
saturating add, with none of standard IMA's step/2 + step/4 + ... series.

Per sample:

    delta  = step[(nibble & 7) + index]
    if nibble & 8: delta = -delta
    pred   = clamp(pred + delta, -32768, 32767)
    index  = clamp(index + index_adj[nibble], 0, 704)

Tables are lifted from the binary rather than hardcoded, so they are whatever
the game actually shipped.
"""
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SEG = ROOT / "reference" / "JUNGA01_DLL" / "seg2.bin"

OFF_STEP = 0x1E0
OFF_INDEX = 0x770
INDEX_MAX = 0x2C0
N_INDEX = 16


def load_tables(path=SEG):
    d = path.read_bytes()
    n_step = (OFF_INDEX - OFF_STEP) // 2
    step = list(struct.unpack_from("<%dH" % n_step, d, OFF_STEP))
    index_adj = list(struct.unpack_from("<%dh" % N_INDEX, d, OFF_INDEX))
    return step, index_adj


def decode(data, step=None, index_adj=None):
    """Decode ADPCM bytes to signed 16-bit PCM."""
    if step is None:
        step, index_adj = load_tables()
    out = bytearray()
    pred = 0
    index = 0
    smax = len(step) - 1
    for byte in data:
        for nib in (byte & 0x0F, byte >> 4):
            delta = step[min((nib & 7) + index, smax)]
            if nib & 8:
                delta = -delta
            pred += delta
            if pred > 32767:
                pred = 32767
            elif pred < -32768:
                pred = -32768
            index += index_adj[nib]
            if index < 0:
                index = 0
            elif index > INDEX_MAX:
                index = INDEX_MAX
            out += struct.pack("<h", pred)
    return bytes(out)


def wav(pcm, channels=1, rate=22050, bits=16):
    align = channels * bits // 8
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * align, align, bits)
    body = (b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt
            + b"data" + struct.pack("<I", len(pcm)) + pcm)
    return b"RIFF" + struct.pack("<I", len(body)) + body


if __name__ == "__main__":
    step, idx = load_tables()
    print("step table : %d entries, first 8 %s, max %d"
          % (len(step), step[:8], max(step)))
    print("index adj  : %s" % idx[:8])
    print("index max  : %d  (= 88 * 8, IMA's max index at 8x resolution)" % INDEX_MAX)
