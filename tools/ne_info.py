#!/usr/bin/env python3
"""Parse the NE (Win16) headers of the engine binaries.

Reports segment layout, imported modules, and the resident/non-resident name
tables — i.e. every exported entry point the engine defines. This is the
engine's own module API, already named by its authors, and it is the cheapest
possible map of the code before any disassembly.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
JD = ROOT / "orig" / "cd" / "JUNGLE"
NOTES = ROOT / "notes"
TARGETS = ["JUNGLE.EXE", "JUNGU01.DLL", "JUNGS01.DLL", "JUNGA01.DLL", "JUNGR01.DLL"]


def pstrings(data, off, limit):
    """Read a table of length-prefixed strings, each followed by a u16 ordinal."""
    out = []
    p = off
    while p < limit:
        n = data[p]
        if n == 0:
            break
        name = data[p + 1:p + 1 + n].decode("latin-1")
        ordv = struct.unpack_from("<H", data, p + 1 + n)[0]
        out.append((name, ordv))
        p += 1 + n + 2
    return out


def parse(path):
    data = path.read_bytes()
    ne_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[ne_off:ne_off + 2] != b"NE":
        return None
    h = ne_off
    info = {
        "file": path.name,
        "size": len(data),
        "ne_off": ne_off,
        "flags": struct.unpack_from("<H", data, h + 0x0C)[0],
        "cs_seg": struct.unpack_from("<H", data, h + 0x16)[0],
        "ip": struct.unpack_from("<H", data, h + 0x14)[0],
        "nsegs": struct.unpack_from("<H", data, h + 0x1C)[0],
        "nmods": struct.unpack_from("<H", data, h + 0x1E)[0],
        "seg_tab": h + struct.unpack_from("<H", data, h + 0x22)[0],
        "res_tab": h + struct.unpack_from("<H", data, h + 0x24)[0],
        "rname_tab": h + struct.unpack_from("<H", data, h + 0x26)[0],
        "mod_tab": h + struct.unpack_from("<H", data, h + 0x28)[0],
        "imp_tab": h + struct.unpack_from("<H", data, h + 0x2A)[0],
        "nrname_off": struct.unpack_from("<I", data, h + 0x2C)[0],
        "nrname_len": struct.unpack_from("<H", data, h + 0x30)[0],
        "align": struct.unpack_from("<H", data, h + 0x32)[0],
    }
    shift = info["align"] or 9

    segs = []
    for i in range(info["nsegs"]):
        o = info["seg_tab"] + i * 8
        sec, slen, flg, alloc = struct.unpack_from("<HHHH", data, o)
        segs.append({
            "n": i + 1,
            "file_off": sec << shift,
            "len": slen or 0x10000,
            "alloc": alloc or 0x10000,
            "code": not (flg & 1),
            "flags": flg,
        })
    info["segs"] = segs

    # imported module names, via the module-reference table into the imported-name table
    mods = []
    for i in range(info["nmods"]):
        noff = struct.unpack_from("<H", data, info["mod_tab"] + i * 2)[0]
        p = info["imp_tab"] + noff
        mods.append(data[p + 1:p + 1 + data[p]].decode("latin-1"))
    info["imports"] = mods

    # exports: resident names (first entry is the module's own name)
    info["resident"] = pstrings(data, info["res_tab"], info["mod_tab"])
    info["nonresident"] = pstrings(
        data, info["nrname_off"], info["nrname_off"] + info["nrname_len"])
    return info


def main():
    NOTES.mkdir(exist_ok=True)
    log = (NOTES / "ne-map.txt").open("w")
    for name in TARGETS:
        p = JD / name
        if not p.exists():
            continue
        i = parse(p)
        if not i:
            print("%-13s not NE" % name)
            continue
        code = sum(s["len"] for s in i["segs"] if s["code"])
        data_sz = sum(s["len"] for s in i["segs"] if not s["code"])
        exports = i["resident"][1:] + i["nonresident"][1:]
        print("%-13s %6dB  segs=%-3d code=%-6d data=%-6d exports=%-4d imports: %s"
              % (name, i["size"], i["nsegs"], code, data_sz, len(exports),
                 ",".join(i["imports"])))

        log.write("=== %s (%d bytes) ===\n" % (name, i["size"]))
        log.write("module name: %s   entry: seg %d:%04x\n"
                  % (i["resident"][0][0] if i["resident"] else "?", i["cs_seg"], i["ip"]))
        log.write("imports: %s\n" % ", ".join(i["imports"]))
        log.write("segments:\n")
        for s in i["segs"]:
            log.write("  seg %-3d %-4s file@%08x len=%-6d alloc=%-6d flags=%04x\n"
                      % (s["n"], "CODE" if s["code"] else "DATA", s["file_off"],
                         s["len"], s["alloc"], s["flags"]))
        log.write("exports (%d):\n" % len(exports))
        for nm, od in sorted(exports, key=lambda e: e[1]):
            log.write("  @%-4d %s\n" % (od, nm))
        log.write("\n")
    log.close()
    print("\nfull map -> notes/ne-map.txt")


if __name__ == "__main__":
    main()
