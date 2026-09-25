#!/usr/bin/env python3
"""Build the scene-script opcode length table from the decompiled interpreter.

FUN_1008_c724 switches on a u16 opcode and writes the record's length to its
out-parameter via the local `local_8`. Cases reach that assignment three ways:

  1. directly            local_8 = 0x1c;
  2. via a shared label  goto LAB_1008_c8c0;   ... LAB_1008_c8c0: local_8 = 4;
  3. from a helper       local_8 = FUN_1008_931a(...);

Only forms 1 and 2 give a static length. Form 3 is data-dependent and those
opcodes are reported as dynamic — a disassembler must stop at them until the
helper is transcribed.

Generating this from the decompiled source rather than hand-copying keeps it
honest: re-running it after any re-analysis reflects what the code actually says.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "notes" / "decomp" / "JUNGLE.EXE.c"
FUNC = "FUN_1008_c724"


def function_body(path, name):
    out, on = [], False
    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("/* ===== " + name + " "):
            on = True
        elif on and line == "}":
            break
        if on:
            out.append(line)
    return "\n".join(out)


def build():
    body = function_body(SRC, FUNC)

    # label -> length, for the shared assignment targets
    labels = {}
    for m in re.finditer(r"^(LAB_[0-9a-f_]+):\s*\n\s*local_8 = (0x[0-9a-f]+|\d+);",
                         body, re.M):
        labels[m.group(1)] = int(m.group(2), 0)

    table, dynamic = {}, {}
    chunks = re.split(r"\n  case ", body)
    for chunk in chunks[1:]:
        m = re.match(r"(0x[0-9a-f]+|\d+):", chunk)
        if not m:
            continue
        op = int(m.group(1), 0)
        seg = chunk[m.end():].split("\n  case ")[0]

        direct = re.search(r"local_8 = (0x[0-9a-f]+|\d+);", seg)
        helper = re.search(r"local_8 = (FUN_[0-9a-f_]+|\(\(undefined2)", seg)
        goto = re.search(r"goto (LAB_[0-9a-f_]+);", seg)

        if direct:
            table[op] = int(direct.group(1), 0)
        elif goto and goto.group(1) in labels:
            table[op] = labels[goto.group(1)]
        elif helper:
            dynamic[op] = helper.group(1)
        else:
            dynamic[op] = "unresolved"
    return table, dynamic


STATIC, DYNAMIC = build()


def main():
    print("static-length opcodes: %d" % len(STATIC))
    for op in sorted(STATIC):
        print("  %-4d len=%d" % (op, STATIC[op]))
    print("\ndynamic / unresolved: %d" % len(DYNAMIC))
    print("  " + " ".join(str(o) for o in sorted(DYNAMIC)))


if __name__ == "__main__":
    main()
