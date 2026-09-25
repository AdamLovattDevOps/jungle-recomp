#!/usr/bin/env python3
"""The 7th Level LZW codec, from FUN_1000_2470 / FUN_1000_22b8 in JUNGR01.DLL.

Framing, outermost first:

  FUN_1000_218c   stream of chunks. Each chunk is a u16 tag followed by
                  `tag & 0x1FFF` bytes. The top three bits select the method:

                     0x0000  stored, copied verbatim
                     0x2000  FUN_1000_26b4          (not seen in bitmap data)
                     0x4000  LZW with 10-bit codes
                     0x6000  LZW with 11-bit codes  (every bitmap chunk on this disc)
                     0x8000  LZW with 12-bit codes
                     0xE000  end of stream

  FUN_1000_263a   inside one chunk: a run of sub-blocks, each a u16 length
                  followed by that many bytes. Each sub-block is decoded
                  independently, so the dictionary resets per sub-block.

  FUN_1000_2470   the LZW decoder itself.

LZW specifics recovered from FUN_1000_22b8:

  code width N is fixed at 9..12, defaulting to 11; it is never varied mid-stream
  dictionary holds 1<<N entries; first free code is 0x100
  code (1<<N)-1 is the end marker, (1<<N)-2 the last usable entry
  codes are packed MSB-first

The mask table at +0x20 is the giveaway for the bit packing. For N=11 it holds
masks of 0,5,2,7,4,1,6,3 bits, which is exactly the leftover-bit count after
each of the 8 codes that fit in 11 bytes.
"""
import struct

METHODS = {
    0x0000: "stored",
    0x2000: "alt",
    0x4000: 10,
    0x6000: 11,
    0x8000: 12,
    0xE000: "end",
}


class Bits:
    """MSB-first bit reader."""

    def __init__(self, data):
        self.d = data
        self.pos = 0
        self.acc = 0
        self.n = 0

    def read(self, width):
        while self.n < width:
            if self.pos >= len(self.d):
                return None
            self.acc = (self.acc << 8) | self.d[self.pos]
            self.pos += 1
            self.n += 8
        self.n -= width
        v = (self.acc >> self.n) & ((1 << width) - 1)
        self.acc &= (1 << self.n) - 1
        return v


def lzw_block(data, width):
    """Decode one LZW sub-block. Dictionary is local to the block."""
    end_code = (1 << width) - 1
    max_code = (1 << width) - 2
    prefix = [0] * (1 << width)
    suffix = [0] * (1 << width)
    out = bytearray()

    bits = Bits(data)
    code = bits.read(width)
    if code is None or code == end_code:
        return bytes(out)
    out.append(code & 0xFF)
    prev = code
    first = code
    nxt = 0x100

    def emit(c):
        s = bytearray()
        while c >= 0x100:
            s.append(suffix[c])
            c = prefix[c]
        s.append(c)
        s.reverse()
        out.extend(s)
        return s[0]

    while True:
        code = bits.read(width)
        if code is None or code == end_code:
            break
        if code < nxt:
            first = emit(code)
        else:
            # KwKwK: the code is the one about to be added
            first = emit(prev)
            out.append(first)
        if nxt <= max_code:
            prefix[nxt] = prev
            suffix[nxt] = first
            nxt += 1
        prev = code
    return bytes(out)


def lzw_chunk(payload, width):
    """Decode the sub-block run inside one chunk (FUN_1000_263a)."""
    out = bytearray()
    p = 0
    while p + 2 <= len(payload):
        ln = struct.unpack_from("<H", payload, p)[0]
        p += 2
        if ln == 0 or p + ln > len(payload):
            break
        out.extend(lzw_block(payload[p:p + ln], width))
        p += ln
    return bytes(out)


def expand(data, offset, end):
    """Walk the chunk stream (FUN_1000_218c) and return the expanded bytes."""
    out = bytearray()
    p = offset
    while p + 2 <= end:
        tag = struct.unpack_from("<H", data, p)[0]
        method = METHODS.get(tag & 0xE000)
        ln = tag & 0x1FFF
        p += 2
        if method == "end":
            break
        if method == "stored":
            out.extend(data[p:p + ln])
        elif isinstance(method, int):
            out.extend(lzw_chunk(data[p:p + ln], method))
        else:
            break
        p += ln
    return bytes(out)
