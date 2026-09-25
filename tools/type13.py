#!/usr/bin/env python3
"""Parse type 13 resources: JUNGS01 sprite motion/animation programs.

A type 13 resource is a flat sequence of records, each `u16 opcode` followed by
operands. Record length is a per-opcode constant from the table JUNGS01 builds
at DS:0BCE in FUN_1000_4554, except opcode 20 whose length is its own u16 at +2
(FUN_1000_4164: `if (*piVar2 == 0x14) piVar2 = piVar2 + piVar2[1]`).
There is no header and no terminator: the program ends when the PC reaches the
end of the buffer (FUN_1000_4164 returns NULL, FUN_1000_45b2 then calls S_010).

Most u16 operands are "value words" in the script-VM encoding, resolved by the
DLL through the far pointer the EXE hands it with S_052 (= the script globals
at 1020:151E):

    raw <  0x159F   script global variable #raw
    raw >= 0x159F   immediate, value = (raw + 0x7531) & 0xFFFF  (i.e. raw - 0x8ACF)

Full field spec: notes/type13-path-format.md.

Usage:
    type13.py                     verify every type 13 in orig/cd/JUNGLE/*.BIN
    type13.py FILE.BIN INDEX      pretty-print one resource
"""
import glob
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import res_dir  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
BINDIR = ROOT / "orig" / "cd" / "JUNGLE"

# DS:0BCE + 2*op, from FUN_1000_4554. Opcode 20 is variable-length.
LENGTH = {1: 10, 2: 14, 3: 2, 4: 2, 5: 30, 6: 8, 7: 10, 8: 2, 9: 22, 10: 8,
          11: 6, 12: 8, 13: 4, 14: 4, 15: 8, 16: 6, 17: 4, 18: 4, 19: 2,
          20: None, 21: 14, 22: 12}

NAMES = {1: "PLAY_RANGE", 2: "SHOW_CELS", 3: "NOP3", 4: "STAMP", 5: "ARC",
         6: "LOOP", 7: "FLIP_OFFSET", 8: "HIDE", 9: "MOVE_TO", 10: "JUMP_POS",
         11: "WAIT", 12: "SET_POS", 13: "SET_CATCHUP", 14: "SET_FRAME_MS",
         15: "SET_RANGE", 16: "SET_MOVE_MS", 17: "SHOW_CEL", 18: "OP18",
         19: "EVENT19", 20: "SCRIPT", 21: "EVENT21", 22: "EXT_FRAMES"}

# (offset, kind, name). kind: v = value word, r = raw u16, b = raw u8,
# z = runtime field (must be zero in the file), i = raw i16.
FIELDS = {
    1: [(2, "v", "first"), (4, "v", "last"), (6, "z", "count"), (8, "v", "ms")],
    2: [(2, "v", "cel0"), (4, "v", "cel1"), (6, "v", "cel2"), (8, "v", "cel3"),
        (10, "v", "ms"), (12, "zb", "shown"), (13, "b", "ncels")],
    5: [(2, "v", "steps"), (4, "b", "rel"), (5, "b", "pad"), (6, "v", "midx"),
        (8, "v", "midy"), (10, "v", "endx"), (12, "v", "endy"),
        (14, "z", "x0"), (16, "z", "y0"), (18, "z", "MX"), (20, "z", "MY"),
        (22, "z", "EX"), (24, "z", "EY"), (26, "z", "i"), (28, "z", "n")],
    6: [(2, "r", "counter"), (4, "r", "count"), (6, "r", "target")],
    7: [(2, "v", "flipx"), (4, "v", "flipy"), (6, "v", "dx"), (8, "v", "dy")],
    9: [(2, "v", "steps_or_speed"), (4, "b", "speed"), (5, "b", "rel"),
        (6, "v", "x"), (8, "v", "y"), (10, "z", "x0"), (12, "z", "y0"),
        (14, "z", "X"), (16, "z", "Y"), (18, "z", "i"), (20, "z", "n")],
    10: [(2, "r", "rel"), (4, "v", "x"), (6, "v", "y")],
    11: [(2, "v", "ms"), (4, "z", "started")],
    12: [(2, "v", "x"), (4, "v", "y"), (6, "b", "rel"), (7, "b", "pad")],
    13: [(2, "b", "catchup"), (3, "b", "pad")],
    14: [(2, "v", "ms")],
    15: [(2, "v", "start"), (4, "v", "first"), (6, "v", "last")],
    16: [(2, "v", "move_ms"), (4, "v0", "frame_ms")],
    17: [(2, "r", "cel")],
    22: [(2, "v", "handle"), (4, "z", "h"), (6, "z", "ptr_lo"), (8, "z", "ptr_hi"),
          (10, "zb", "new"), (11, "b", "pad")],
}


def rec_len(b, p):
    op = struct.unpack_from("<H", b, p)[0]
    if op not in LENGTH:
        return op, None
    n = LENGTH[op]
    if n is None:
        n = struct.unpack_from("<H", b, p + 2)[0] if p + 4 <= len(b) else None
    return op, n


def value(raw):
    """Decode a value word: ('imm', signed int) or ('var', index)."""
    if raw < 0x159F:
        return ("var", raw)
    v = (raw + 0x7531) & 0xFFFF
    return ("imm", v - 0x10000 if v & 0x8000 else v)


def fmt_value(raw):
    k, v = value(raw)
    return "g%d" % v if k == "var" else str(v)


def records(b):
    """Yield (offset, op, length) or raise ValueError on desync."""
    p = 0
    while p < len(b):
        if p + 2 > len(b):
            raise ValueError("trailing byte at %d" % p)
        op, n = rec_len(b, p)
        if n is None or n < 2 or p + n > len(b):
            raise ValueError("bad record op=%d len=%s at %d" % (op, n, p))
        yield p, op, n
        p += n


def decode(b, p, op, n):
    out = []
    for off, kind, name in FIELDS.get(op, []):
        if kind in ("b", "zb"):
            val = b[p + off]
            out.append("%s=%d" % (name, val))
        else:
            raw = struct.unpack_from("<H", b, p + off)[0]
            if kind == "v0":
                # op 16: raw 0 means "leave the frame period alone"
                out.append("%s=%s" % (name, "unchanged" if raw == 0 else fmt_value(raw)))
            elif kind == "v":
                s = fmt_value(raw)
                if raw == 0xFFFF:
                    s = "raw-1"
                out.append("%s=%s" % (name, s))
            elif kind == "z":
                if raw:
                    out.append("%s=!%04x" % (name, raw))
            else:
                out.append("%s=%d" % (name, raw))
    if op == 20:
        out.append("stmt=%s" % b[p + 4:p + n].hex(" "))
    return " ".join(out)


def iter_type13():
    for f in sorted(glob.glob(str(BINDIR / "*.BIN"))):
        data = open(f, "rb").read()
        try:
            _, ents = res_dir.entries(data)
        except Exception:
            continue
        for i, e in enumerate(ents):
            if e["type"] != 13:
                continue
            try:
                b = res_dir.fetch(data, e)
            except Exception:
                continue
            yield Path(f).name, i, b


def verify():
    try:
        from script_dis import record_len as stmt_len
    except Exception:
        stmt_len = None
    total = clean = 0
    nbytes = 0
    ops = Counter()
    per_file = Counter()
    per_file_ok = Counter()
    checks = Counter()
    problems = []
    for fname, idx, b in iter_type13():
        total += 1
        per_file[fname] += 1
        try:
            recs = list(records(b))
        except ValueError as ex:
            problems.append((fname, idx, str(ex)))
            continue
        clean += 1
        per_file_ok[fname] += 1
        nbytes += len(b)
        starts = {p for p, _, _ in recs}
        for p, op, n in recs:
            ops[op] += 1
            # runtime fields are zero on disk
            for off, kind, name in FIELDS.get(op, []):
                if kind == "z":
                    ok = struct.unpack_from("<H", b, p + off)[0] == 0
                    checks["runtime-zero " + ("ok" if ok else "BAD")] += 1
                elif kind == "zb":
                    checks["runtime-zero " + ("ok" if b[p + off] == 0 else "BAD")] += 1
            if op == 6:
                counter, count, tgt = struct.unpack_from("<HHH", b, p + 2)
                checks["loop target on record boundary " +
                       ("ok" if tgt in starts else "BAD")] += 1
                checks["loop target backwards " + ("ok" if tgt <= p else "BAD")] += 1
                checks["loop counter==count " +
                       ("ok" if counter == count else "BAD")] += 1
            if op == 2:
                checks["show-cels ncels<=4 " + ("ok" if 1 <= b[p + 13] <= 4 else "BAD")] += 1
            if op == 20 and stmt_len is not None:
                ln, _ = stmt_len(b[p + 4:p + n], 0)
                checks["script payload == one type-14 statement " +
                       ("ok" if ln == n - 4 else "BAD")] += 1
    print("type 13 resources parsed cleanly: %d/%d  (%d bytes, every byte consumed)"
          % (clean, total, nbytes))
    for f in sorted(per_file):
        print("  %-14s %4d/%d" % (f, per_file_ok[f], per_file[f]))
    print("records by opcode:")
    for op, c in sorted(ops.items()):
        print("  %2d %-12s %6d" % (op, NAMES.get(op, "?"), c))
    print("consistency checks:")
    for k, c in sorted(checks.items()):
        print("  %-55s %6d" % (k, c))
    for pr in problems[:20]:
        print("PROBLEM", pr)
    return clean == total


def dump(path, index):
    data = open(path, "rb").read()
    _, ents = res_dir.entries(data)
    e = ents[index]
    b = res_dir.fetch(data, e)
    print("%s[%d] type %d, %d bytes" % (Path(path).name, index, e["type"], len(b)))
    for p, op, n in records(b):
        print("  %04x  %2d %-12s %s" % (p, op, NAMES.get(op, "?"), decode(b, p, op, n)))


if __name__ == "__main__":
    if len(sys.argv) >= 3:
        dump(sys.argv[1], int(sys.argv[2]))
    else:
        sys.exit(0 if verify() else 1)
