#!/usr/bin/env python3
"""Human-readable listing of a scene script.

Combines the outer record walk with a disassembly of the embedded expression
bytecode, naming both instruction sets where the name is known. Unidentified
opcodes print as `opN` / `vmN` rather than an invented mnemonic.

Usage:  listing.py FILE.BIN [RESOURCE_INDEX]
        listing.py JUNGOPTS.BIN 0
"""
import struct
import sys
from pathlib import Path

import opnames
import script_dis as S
from res_dir import entries, fetch, resident_blob
from vm import WIDE, WIDE4, IMMEDIATE_BASE, IMMEDIATE_BIAS

ROOT = Path(__file__).resolve().parent.parent
BINDIR = ROOT / "orig" / "cd" / "JUNGLE"
EXPR_AT = {76: 4, 89: 4, 77: 6, 79: 6, 78: 10}


def operand_text(op, a):
    if a >= IMMEDIATE_BASE:
        return "#%d" % (a + IMMEDIATE_BIAS)
    if op in (2, 4):
        return "var%d" % a
    return str(a)


def disassemble_expression(code, indent="        "):
    out = []
    p = 0
    while p < len(code):
        op = code[p]
        if op == 0:
            out.append(indent + "end")
            break
        if op in WIDE4:
            a = struct.unpack_from("<I", code, p + 1)[0]
            out.append("%s%-13s %d" % (indent, opnames.vm(op), a))
            p += 5
        elif op in WIDE:
            a = struct.unpack_from("<H", code, p + 1)[0]
            out.append("%s%-13s %s" % (indent, opnames.vm(op), operand_text(op, a)))
            p += 3
        else:
            out.append(indent + opnames.vm(op))
            p += 1
    return out


def listing(data, entry, blob):
    b = fetch(data, entry, blob)
    lines = ["; resource %d — %d bytes" % (entry["i"], len(b))]
    for off, op, ln, operands in S.walk(b):
        if ln is None:
            lines.append("  %04x  ??? opcode %d  %s" % (off, op, operands.hex(" ")))
            break
        extra = ""
        if op == 37 and len(operands) >= 2:
            extra = "  -> %04x" % (off + struct.unpack_from("<h", operands, 0)[0])
        elif op in (77, 79) and len(operands) >= 4:
            f, t = struct.unpack_from("<HH", operands, 0)
            extra = "  false -> %04x   true -> %04x" % (off + f, off + t)
        lines.append("  %04x  %-16s len=%-4d%s" % (off, opnames.outer(op), ln, extra))
        if op in EXPR_AT and ln and ln > EXPR_AT[op]:
            lines += disassemble_expression(b[off + EXPR_AT[op]:off + ln])
    return lines


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    name = sys.argv[1]
    path = Path(name) if Path(name).exists() else BINDIR / name
    data = path.read_bytes()
    _, ents = entries(data)
    blob = resident_blob(data)
    t14 = [e for e in ents if e["type"] == 14]
    if len(sys.argv) > 2:
        want = int(sys.argv[2])
        t14 = [e for e in t14 if e["i"] == want] or t14[:1]
    else:
        t14 = t14[:3]
    for e in t14:
        print("\n".join(listing(data, e, blob)))
        print()


if __name__ == "__main__":
    main()
