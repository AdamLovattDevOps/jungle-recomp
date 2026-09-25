#!/usr/bin/env python3
"""Mnemonics for both instruction sets, derived from what the handlers reach.

Outer opcode names come from the API each dispatcher case ultimately calls
(round 24's trace). VM opcode names come from reading FUN_1008_1bf2 directly.
Anything not positively identified is left unnamed rather than guessed — an
invented mnemonic is worse than a bare number, because it reads as knowledge.
"""

OUTER = {
    0:  "END",
    5:  "SURFACE.ALLOC",
    7:  "MOUSE.GETPOS",
    8:  "SND.8",
    9:  "SND.9",
    10: "OP10",
    11: "GFX.11",
    13: "SND.POST",
    14: "SND.14",
    15: "SND.15",
    18: "SND.POST2",
    19: "TIME.GET",
    25: "GFX.25",
    26: "SURFACE.FREE",
    28: "GFX.28",
    32: "IMPORT8",
    36: "GFX.36",
    37: "JUMP",
    38: "CMP.VARS",
    39: "PRESENT",
    40: "GFX.40",
    42: "GFX.42",
    43: "SWITCH.VAR",
    44: "END2",
    47: "KEY.GETSTATE",
    49: "MATH.GETANGLE",
    50: "MATH.NORMANGLE",
    52: "PATH.RESOLVE",
    55: "GFX.55",
    56: "STR.FORMAT",
    58: "STR.CMP",
    59: "OP59",
    60: "OP60",
    62: "GEOM.INTERSECT",
    63: "GFX.63",
    66: "MATH.COSINE",
    67: "INI.WRITE",
    76: "EXPR",
    77: "BRANCH",
    78: "SWITCH",
    79: "BRANCH2",
    89: "EXPR.STORE",
}

VM = {
    0:  "end",      1:  "push.imm",  2:  "push.val",  3:  "index.load",
    4:  "push.addr", 5: "nop",       6:  "index.addr", 8: "store",
    12: "call.12",  13: "call.13",  14: "neg",       15: "not.l",
    16: "not.b",    17: "add",       18: "sub",       19: "mul",
    20: "div",      21: "mod",       22: "eq",        23: "gt",
    24: "ge",       25: "lt",        26: "le",        27: "ne",
    28: "and.l",    29: "or.l",      30: "and.b",     31: "or.b",
    32: "xor.b",    33: "inc",       34: "load.postinc", 35: "dec.load",
    36: "dec",      37: "br.false",  38: "br.true",   39: "load",
}


def outer(op):
    return OUTER.get(op, "op%d" % op)


def vm(op):
    return VM.get(op, "vm%d" % op)
