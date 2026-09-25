#!/usr/bin/env python3
"""Disassemble type-14 scene scripts using the recovered opcode length table.

Each record is `u16 opcode` followed by operands. The length comes from
opcode_table.STATIC where the interpreter assigns it a constant, and is
data-dependent for the opcodes in opcode_table.DYNAMIC — those stop the walk,
since guessing past one would silently desynchronise the whole stream.

Opcode 76 carries expression bytecode for FUN_1008_1bf2 (a stack VM) in its
payload rather than operands; 89 is the same with the result stored.

Usage:  script_dis.py [FILE.BIN] [--verbose]
"""
import struct
import sys
from collections import Counter
from pathlib import Path

from opcode_table import STATIC, DYNAMIC
from res_dir import entries, fetch, resident_blob

ROOT = Path(__file__).resolve().parent.parent
BINDIR = ROOT / "orig" / "cd" / "JUNGLE"
NOTES = ROOT / "notes"

# Opcodes whose length is stored in the second u16 rather than fixed.
LENGTH_IN_FIELD = {37, 76}

# Stack-VM opcodes that carry a u16 operand (3 bytes total); all others are 1
# byte. From FUN_1008_1bf2, where these reach `puVar9 = param_1 + 3`.
VM_WIDE = {1, 2, 4, 37, 38}    # opcode + u16
VM_WIDE4 = {12, 13}            # opcode + u32

# End-of-script markers: zero advance, no handler.
TERMINATORS = {0, 44}


def expr_len(b, p):
    """Length of the FUN_1008_1bf2 expression bytecode starting at p."""
    start = p
    while p < len(b):
        op = b[p]
        if op == 0:
            return p + 1 - start
        p += 5 if op in VM_WIDE4 else 3 if op in VM_WIDE else 1
    return len(b) - start


def record_len(b, p):
    """(length, branch_targets) for the record at p, or (None, []) if unknown."""
    import struct as _s
    op = _s.unpack_from("<H", b, p)[0]
    if op == 37:
        # JUMP, in both directions. The field is a DISTANCE, not a length: the
        # record is 4 bytes and the advance says where execution continues.
        # Counting the distance as a span marks every skipped byte as walked,
        # which inflates coverage without decoding anything in between.
        adv = _s.unpack_from("<h", b, p + 2)[0]
        return "jump", [adv]
    if op in LENGTH_IN_FIELD:
        return _s.unpack_from("<H", b, p + 2)[0], []
    if op in (77, 79):
        # conditional branch (FUN_1008_931a / FUN_1008_a1be): expression at +6,
        # advance u16[1] when it evaluates false, u16[2] when true
        a_false = _s.unpack_from("<H", b, p + 2)[0]
        a_true = _s.unpack_from("<H", b, p + 4)[0]
        own = 6 + expr_len(b, p + 6)
        return own, [a_false, a_true]
    if op == 78:
        # switch (FUN_1008_a098): case count at +8, table at p + u16[+4],
        # default advance at +6, selector expression at +10. Table entries are
        # 6 bytes: match value at +2, advance at +4, relative to the table base.
        n = b[p + 8]
        tbl = p + _s.unpack_from("<H", b, p + 4)[0]
        default = _s.unpack_from("<H", b, p + 6)[0]
        targets = [default]
        for i in range(n):
            q = tbl + i * 6
            if q + 6 <= len(b):
                targets.append(_s.unpack_from("<H", b, q + 4)[0])
        return 10 + expr_len(b, p + 10), targets
    if op == 43:
        # FUN_1008_9fee: same table geometry as opcode 78, but the selector is a
        # variable lookup rather than an expression. Irrelevant statically —
        # both arms/cases are followed either way.
        n = b[p + 8]
        tbl = p + _s.unpack_from("<h", b, p + 4)[0]
        targets = [_s.unpack_from("<H", b, p + 6)[0]]
        for i in range(n):
            q = tbl + i * 6
            if 0 <= q and q + 6 <= len(b):
                targets.append(_s.unpack_from("<H", b, q + 4)[0])
        return 10, targets
    if op == 38:
        # FUN_1008_927c: compares two variable lookups with an operator byte at
        # +8; advance is u16[+2] when the comparison is false, else 10.
        return 10, [_s.unpack_from("<H", b, p + 2)[0], 10]
    if op == 89:
        return 4 + expr_len(b, p + 4), []
    if op == 1:
        # local_8 = (byte[5] + 3) * 2
        return (b[p + 5] + 3) * 2 if p + 6 <= len(b) else None, []
    # Opcodes 44 and 0 advance by zero and invoke no handler. A zero advance
    # would loop forever, so the dispatcher's caller must stop on them: they
    # terminate the path rather than blocking it.
    if op in TERMINATORS:
        return 0, []
    # Resolved by reading the handlers directly rather than the generated table:
    #   59, 60  share a handler that falls through to `local_8 = 10`
    #   17      FUN_1008_919c writes 0xE (or 0 on one path) through an out-param
    # Opcodes 10 and 11 share one case body that falls through to
    # LAB_1008_c8c0 (local_8 = 4). The generated table missed it because the
    # shared `case 10: case 0xb:` pair leaves the first case with no body.
    if op in (10, 11):
        return 4, []
    if op in (59, 60):
        return 10, []
    if op == 17:
        return 14, []
    if op in STATIC and STATIC[op] >= 2:
        return STATIC[op], []
    return None, []


def walk(b):
    """Disassemble as a graph, following both arms of every conditional branch.

    A linear walk cannot work: opcode 77 advances by one of two amounts
    depending on a runtime expression, so the bytes after it belong to two
    different paths. Following both and recording visited offsets covers the
    whole resource without guessing which arm runs.
    """
    seen = set()
    work = [0]
    while work:
        p = work.pop()
        while p + 2 <= len(b) and p not in seen:
            seen.add(p)
            op = struct.unpack_from("<H", b, p)[0]
            ln, targets = record_len(b, p)
            if ln == "jump":
                # 4-byte record, then follow the edge. The linear path ends
                # here either way.
                yield (p, op, 4, b[p + 2:p + 4])
                q = p + targets[0]
                if 0 <= q < len(b) and q not in seen:
                    work.append(q)
                break
            if ln == 0 and op in TERMINATORS:
                yield (p, op, 0, b"")
                break
            if ln is None or ln < 2 or p + ln > len(b):
                yield (p, op, None, b[p:p + 8])
                break
            yield (p, op, ln, b[p + 2:p + ln])
            # Only seed a branch target if a known opcode sits there. A wrong
            # target lands mid-record, and every byte after it decodes as
            # garbage — one bad seed poisons a whole resource. Opcode 37 was
            # showing up as the top "blocker" purely as a symptom of this.
            for t in targets:
                q = p + t
                if 2 <= t and q + 2 <= len(b) and q not in seen:
                    nxt = struct.unpack_from("<H", b, q)[0]
                    if nxt in STATIC or nxt in LENGTH_IN_FIELD or nxt in TERMINATORS \
                       or nxt in (77, 78, 79, 89, 17, 59, 60):
                        work.append(q)
            p += ln


def main():
    name = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else "JUNGOPTS.BIN"
    verbose = "--verbose" in sys.argv
    path = Path(name) if Path(name).exists() else BINDIR / name
    data = path.read_bytes()
    _, ents = entries(data)
    blob = resident_blob(data)
    t14 = [e for e in ents if e["type"] == 14]

    NOTES.mkdir(exist_ok=True)
    log = (NOTES / ("script-%s.txt" % path.stem)).open("w")

    full = partial = 0
    ops = Counter()
    stoppers = Counter()
    for e in t14:
        b = fetch(data, e, blob)
        log.write("=== resource %d (%d bytes) ===\n" % (e["i"], e["size"]))
        # Coverage must be measured as bytes reached, not as "the last linear
        # step landed on the end". A graph walk legitimately finishes inside the
        # resource whenever the final record is entered via a jump, so the old
        # test reported complete resources as partial.
        covered = set()
        stopped = None
        for off, op, ln, operands in walk(b):
            if ln is None:
                stopped = op
                log.write("  %04x  STOP opcode %d  %s\n" % (off, op, operands.hex(" ")))
                break
            ops[op] += 1
            covered.update(range(off, min(off + max(ln, 2), len(b))))
            log.write("  %04x  op %-4d len %-3d %s\n" % (off, op, ln, operands.hex(" ")))
        if stopped is None and len(covered) == len(b):
            full += 1
        else:
            partial += 1
            if stopped is not None:
                stoppers[stopped] += 1
        log.write("\n")
    log.close()

    print("%s: %d type-14 resources" % (path.name, len(t14)))
    print("  fully disassembled : %d" % full)
    print("  stopped early      : %d" % partial)
    print("\nopcodes decoded (op:count): %s"
          % " ".join("%d:%d" % t for t in ops.most_common(14)))
    if stoppers:
        print("blocking opcodes (op:count): %s"
              % " ".join("%d:%d" % t for t in stoppers.most_common(10)))
        print("of those, known-dynamic: %s"
              % sorted(o for o in stoppers if o in DYNAMIC))
    print("\nfull listing -> notes/script-%s.txt" % path.stem)


if __name__ == "__main__":
    main()
