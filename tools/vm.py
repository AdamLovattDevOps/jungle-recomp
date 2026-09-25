#!/usr/bin/env python3
"""The 7th Level script VM — reference interpreter for FUN_1008_1bf2.

Scene scripts do not statically say what to draw: resource ids and coordinates
are computed into variables, and the draw layer reads them back. So the
expression bytecode has to actually run.

MACHINE MODEL

The decompiled source appears to use two stacks, at 0x70C and 0x70E. It does
not. Both are indexed by `sp * 2`, so the slot at `0x70E + sp*2` is the same
memory as `0x70C + (sp+1)*2`. There is ONE stack array; `0x70E`-relative simply
means "one slot higher".

With that, the discipline is consistent everywhere:

    top of stack      S[sp]
    push v            S[sp + 1] = v; sp += 1
    binary operator    sp -= 1; S[sp] = S[sp] OP S[sp + 1]

MEMORY

Variables are addressed, not named. An index resolves to a machine address:

    index <  0x13FE   addr = 0x151E + index * 2          script global
    index <  0x159F   addr = FRAME_PTR - index * 2        call-frame local/parameter
    otherwise         not an address at all — an immediate, value index + 0x7531

The middle region is a DOWNWARD-GROWING CALL STACK, not a register file: VM
opcode 13 pops arguments into a frame and moves the frame pointer down by the
frame size. That is why its addressing subtracts, and why the indices just above
0x13FE are referenced constantly — they are the current frame's first slots.

Modelling a flat address space rather than a variable dictionary is what makes
opcode 3 work: it computes `base + subscript * 2`, i.e. genuine array
subscripting, which a name-keyed model cannot express.

Opcode 4 pushes an address and opcode 2 pushes the value at it. That difference
is what gives the language lvalues: 4 yields something assignable, 2 its value.
"""
import struct

WIDE = {1, 2, 4, 37, 38}      # opcode + u16
WIDE4 = {12, 13}              # opcode + u32
VAR_BASE = 0x151E
VAR_SPLIT = 0x13FE
IMMEDIATE_BASE = 0x159F
IMMEDIATE_BIAS = 0x7531
HIGH_BASE = 0x27FC            # DAT_1020_10AC is added at runtime; 0 here
STACK = 128


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


class VMError(Exception):
    pass


class VM:
    """Executes expression bytecode against a flat 16-bit address space."""

    def __init__(self, memory=None):
        self.mem = dict(memory or {})      # address -> 16-bit value
        self.writes = []                   # (address, value), in order

    # -- address space ----------------------------------------------------
    def address_of(self, index):
        if index < VAR_SPLIT:
            return VAR_BASE + index * 2
        return (HIGH_BASE - index * 2) & 0xFFFF

    def is_immediate(self, index):
        return index >= IMMEDIATE_BASE

    def load(self, addr):
        return s16(self.mem.get(addr & 0xFFFF, 0))

    def store(self, addr, value):
        addr &= 0xFFFF
        self.mem[addr] = s16(value)
        self.writes.append((addr, s16(value)))

    def variables(self):
        """Written addresses mapped back to script variable indices.

        Both regions must be inverted, and the high one descends: index 5119
        lands at address 0xFFFE, which a naive `(addr - VAR_BASE) / 2` decodes
        as index 30064. Anything that does not invert to a legal index is
        reported separately rather than silently mapped to a nonsense one.
        """
        low, frame, other = {}, {}, {}
        for addr, val in self.mem.items():
            idx_low = (addr - VAR_BASE) // 2
            idx_high = ((HIGH_BASE - addr) & 0xFFFF) // 2  # frame slot
            if (addr - VAR_BASE) % 2 == 0 and 0 <= idx_low < VAR_SPLIT:
                low[idx_low] = val
            elif VAR_SPLIT <= idx_high < IMMEDIATE_BASE:
                frame[idx_high] = val
            else:
                other[addr] = val
        return {"globals": low, "frame": frame, "unmapped": other}

    # -- execution --------------------------------------------------------
    def run(self, code, p=0, limit=200000):
        S = [0] * STACK
        sp = 0
        steps = 0
        while p < len(code):
            if steps > limit:
                raise VMError("step limit")
            steps += 1
            op = code[p]
            if op == 0:
                return s16(S[sp]), p + 1, steps

            arg = None
            if op in WIDE4:
                if p + 5 > len(code):
                    raise VMError("truncated u32 operand, op %d" % op)
                arg = struct.unpack_from("<I", code, p + 1)[0]
                p += 5
            elif op in WIDE:
                if p + 3 > len(code):
                    raise VMError("truncated u16 operand, op %d" % op)
                arg = struct.unpack_from("<H", code, p + 1)[0]
                p += 3
            else:
                p += 1

            if op == 1:                                  # push immediate
                sp += 1; S[sp] = s16(arg)
            elif op == 2:                                # push variable VALUE
                v = s16(arg + IMMEDIATE_BIAS) if self.is_immediate(arg) \
                    else self.load(self.address_of(arg))
                sp += 1; S[sp] = v
            elif op == 4:                                # push variable ADDRESS
                v = s16(arg + IMMEDIATE_BIAS) if self.is_immediate(arg) \
                    else self.address_of(arg)
                sp += 1; S[sp] = v
            elif op == 3:                                # indexed load: base + i*2
                sp -= 1
                S[sp] = self.load(S[sp + 1] * 2 + S[sp])
            elif op == 6:                                # address of base + i*2
                sp -= 1
                S[sp] = s16(S[sp + 1] * 2 + S[sp])
            elif op == 8:                                # store through address
                sp -= 1
                self.store(S[sp], S[sp + 1])
                S[sp] = S[sp + 1]
            elif op == 39:                               # load through address
                sp += 1; S[sp] = self.load(S[sp - 1])
            elif op == 34:                               # load then post-increment
                a = S[sp]
                S[sp] = self.load(a)
                self.store(a, self.load(a) + 1)
            elif op == 5:
                pass
            elif op in (12, 13):                         # external call
                pass
            elif op == 14: S[sp] = s16(-S[sp])
            elif op == 15: S[sp] = int(S[sp] == 0)
            elif op == 16: S[sp] = s16(~S[sp])
            elif op == 33: S[sp] = s16(S[sp] + 1)
            elif op in (35, 36): S[sp] = s16(S[sp] - 1)
            elif 17 <= op <= 32:
                sp -= 1
                x, y = s16(S[sp]), s16(S[sp + 1])
                if op == 17: r = x + y
                elif op == 18: r = x - y
                elif op == 19: r = x * y
                elif op == 20:
                    if y == 0: raise VMError("divide by zero (handler 0x6F)")
                    r = -(-x // y) if (x < 0) != (y < 0) else x // y
                elif op == 21:
                    if y == 0: raise VMError("modulo by zero (handler 0x6F)")
                    q = -(-x // y) if (x < 0) != (y < 0) else x // y
                    r = x - y * q
                elif op == 22: r = int(x == y)
                elif op == 23: r = int(x > y)
                elif op == 24: r = int(x >= y)
                elif op == 25: r = int(x < y)
                elif op == 26: r = int(x <= y)
                elif op == 27: r = int(x != y)
                elif op == 28: r = int(bool(x) and bool(y))
                elif op == 29: r = int(bool(x) or bool(y))
                elif op == 30: r = x & y
                elif op == 31: r = x | y
                else:          r = x ^ y
                S[sp] = s16(r)
            elif op in (37, 38):
                pass                                     # branches: control flow
            else:
                raise VMError("unknown opcode %d" % op)

            if sp < 0 or sp >= STACK - 1:
                raise VMError("stack pointer out of range: %d" % sp)
        return s16(S[sp]), p, steps
