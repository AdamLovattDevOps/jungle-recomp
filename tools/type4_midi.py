#!/usr/bin/env python3
"""Type 4 ("MIDI event") resources: parse, verify, and convert to SMF.

Format (derived from JUNGR01 RESCREATEMIDIEVENT and the JUNGA01 sequencer
FUN_1000_1b20; see notes/type4-midi-format.md for the quoted evidence):

  +0  u16  byteLen   length of the event array in bytes (event count = byteLen>>2)
  +2  u16  id        posted back as wParam of message 0x52c when play finishes
  +4  u32  events[byteLen/4], little-endian

Each event dword `w`:
  (w >> 16) & 0xF000 != 0   -> DELAY:  ms = w & 0x7FFFFFFF (bit 31 is the flag).
                               timeSetEvent gets only the low 16 bits; a delay
                               of 1..2 ms is raised to 3 ms; 0 means "no wait".
  otherwise                 -> midiOutShortMsg(hmo, w): byte0 status, byte1
                               data1, byte2 data2, byte3 unused (0).
There is no end marker: playback stops (or loops back to event 0) when the
index reaches the event count.

Usage:
  type4_midi.py --verify                         check every type 4 on the disc
  type4_midi.py FILE.BIN INDEX [-o OUT.mid]      convert one resource
  type4_midi.py --all [-o DIR]                   convert every type 4 resource
Options for conversion:
  --loops N        write N passes back to back (default 1)
  --no-clamp       keep 1..2 ms delays as-is (the player raises them to 3 ms)
  --remap-drums    move channel 10 to channel 16 (7thlevel.ini RemapMidiDrums=1)
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import res_dir  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
BINDIR = ROOT / "orig" / "cd" / "JUNGLE"
MIN_DELAY_MS = 3            # 1000:1bd8  cmp [bp-8],3 / jae / mov [bp-8],3
PPQ = 500                   # 500 ticks per quarter at 500000 us/quarter
TEMPO_US = 500000           #   => 1 tick == 1 ms exactly

# data bytes carried by each status class (upper nibble)
DATA_LEN = {0x8: 2, 0x9: 2, 0xA: 2, 0xB: 2, 0xC: 1, 0xD: 1, 0xE: 2}


def parse(raw):
    """Return (header, events, problems). events: ('delay', ms) | ('msg', bytes)."""
    probs = []
    if len(raw) < 4:
        return None, [], ["shorter than header"]
    blen, rid = struct.unpack_from("<HH", raw, 0)
    if blen + 4 != len(raw):
        probs.append("byteLen+4=%d != resource size %d" % (blen + 4, len(raw)))
    if blen & 3:
        probs.append("byteLen %d not a multiple of 4 (player drops %d bytes)" % (blen, blen & 3))
    n = blen >> 2
    ev = []
    for i in range(n):
        if 4 + i * 4 + 4 > len(raw):
            probs.append("event %d past end of resource" % i)
            break
        (w,) = struct.unpack_from("<I", raw, 4 + i * 4)
        hi = w >> 16
        if hi & 0xF000:
            ms = w & 0x7FFFFFFF
            if hi & 0x7000 or ms > 0xFFFF:
                probs.append("event %d: delay %#x has bits the player truncates (timeSetEvent takes 16 bits)" % (i, w))
            ev.append(("delay", ms))
        else:
            st, d1, d2, b3 = w & 0xFF, (w >> 8) & 0xFF, (w >> 16) & 0xFF, w >> 24
            if st < 0x80 or st >= 0xF0:
                probs.append("event %d: status %#04x is not a channel message" % (i, st))
            dl = DATA_LEN.get(st >> 4, 2)
            if d1 > 0x7F or (dl == 2 and d2 > 0x7F):
                probs.append("event %d: data byte >0x7f in %08x" % (i, w))
            if b3 or (dl == 1 and d2):
                probs.append("event %d: nonzero unused byte in %08x" % (i, w))
            ev.append(("msg", bytes([st, d1, d2][:1 + dl])))
    return {"byteLen": blen, "id": rid, "count": n}, ev, probs


def eff_delay(ms, clamp=True):
    """Delay the player actually waits (unscaled path, [+0x24]==0)."""
    ms &= 0xFFFF                     # only the low word reaches timeSetEvent
    if clamp and 0 < ms < MIN_DELAY_MS:
        return MIN_DELAY_MS
    return ms


def analyse(ev, clamp=True):
    total = 0
    chans, progs = set(), {}
    held = {}
    clamped = 0
    for kind, v in ev:
        if kind == "delay":
            d = eff_delay(v, clamp)
            clamped += d != v
            total += d
            continue
        st = v[0]
        ch = st & 0xF
        chans.add(ch)
        hi = st >> 4
        if hi == 0xC:
            progs.setdefault(ch, [])
            if v[1] not in progs[ch]:
                progs[ch].append(v[1])
        elif hi == 0x9 and v[2]:
            held[(ch, v[1])] = held.get((ch, v[1]), 0) + 1
        elif hi == 0x8 or (hi == 0x9 and not v[2]):
            if held.get((ch, v[1])):
                held[(ch, v[1])] -= 1
    hanging = sum(1 for k in held.values() if k > 0)
    return {"ms": total, "chans": sorted(chans), "progs": progs,
            "hanging": hanging, "clamped": clamped}


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def to_smf(ev, name="", loops=1, clamp=True, remap_drums=False):
    trk = bytearray()
    trk += b"\x00\xff\x51\x03" + TEMPO_US.to_bytes(3, "big")
    if name:
        nb = name.encode("ascii", "replace")
        trk += b"\x00\xff\x03" + vlq(len(nb)) + nb
    pending = 0
    for p in range(loops):
        if loops > 1:
            mk = b"loop %d" % p
            trk += vlq(pending) + b"\xff\x06" + vlq(len(mk)) + mk
            pending = 0
        for kind, v in ev:
            if kind == "delay":
                pending += eff_delay(v, clamp)
                continue
            m = bytearray(v)
            if remap_drums and (m[0] & 0xF) == 9:
                m[0] |= 0xF
            trk += vlq(pending) + bytes(m)   # always explicit status (no running status)
            pending = 0
    trk += vlq(pending) + b"\xff\x2f\x00"    # keep trailing delay: it is part of the loop period
    hdr = b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQ)
    return hdr + b"MTrk" + struct.pack(">I", len(trk)) + bytes(trk)


def all_type4():
    for f in sorted(BINDIR.glob("*.BIN")):
        data = f.read_bytes()
        _, ents = res_dir.entries(data)
        for e in ents:
            if e["type"] == 4:
                yield f.name, e["i"], res_dir.fetch(data, e)


def fmt_progs(progs):
    return " ".join("ch%d:%s" % (c + 1, ",".join(str(p) for p in ps)) for c, ps in sorted(progs.items()))


def verify():
    ok = n = 0
    for fname, idx, raw in all_type4():
        n += 1
        h, ev, probs = parse(raw)
        a = analyse(ev)
        nd = sum(1 for k, _ in ev if k == "delay")
        good = not probs
        ok += good
        print("%-12s %5d  %s  bytes=%6d id=%#06x events=%5d (msg %5d, delay %5d)  %7.2fs  clamp1-2ms=%d hanging=%d  last=%s"
              % (fname, idx, "OK  " if good else "FAIL", len(raw), h["id"], h["count"],
                 h["count"] - nd, nd, a["ms"] / 1000.0, a["clamped"], a["hanging"],
                 "delay" if ev and ev[-1][0] == "delay" else "msg"))
        print("%18s channels=%s  programs=%s" % ("", ",".join(str(c + 1) for c in a["chans"]), fmt_progs(a["progs"])))
        for p in probs[:10]:
            print("      ! " + p)
    print("\n%d/%d type 4 resources consume their bytes exactly and parse cleanly" % (ok, n))
    return ok == n


def main():
    argv = sys.argv[1:]
    opt = {"loops": 1, "clamp": True, "remap": False, "out": None}
    pos = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--loops":
            opt["loops"] = int(argv[i + 1]); i += 1
        elif a == "--no-clamp":
            opt["clamp"] = False
        elif a == "--remap-drums":
            opt["remap"] = True
        elif a == "-o":
            opt["out"] = argv[i + 1]; i += 1
        else:
            pos.append(a)
        i += 1
    kw = dict(loops=opt["loops"], clamp=opt["clamp"], remap_drums=opt["remap"])
    if "--verify" in pos:
        sys.exit(0 if verify() else 1)
    if "--all" in pos:
        outdir = Path(opt["out"] or "build/midi")
        outdir.mkdir(parents=True, exist_ok=True)
        for fname, idx, raw in all_type4():
            _, ev, _ = parse(raw)
            name = "%s_%d" % (Path(fname).stem, idx)
            (outdir / (name + ".mid")).write_bytes(to_smf(ev, name, **kw))
            print("wrote", outdir / (name + ".mid"))
        return
    if len(pos) != 2:
        print(__doc__)
        sys.exit(2)
    f = Path(pos[0])
    if not f.exists():
        f = BINDIR / pos[0]
    data = f.read_bytes()
    _, ents = res_dir.entries(data)
    e = ents[int(pos[1], 0)]
    if e["type"] != 4:
        sys.exit("resource %s is type %d, not 4" % (pos[1], e["type"]))
    h, ev, probs = parse(res_dir.fetch(data, e))
    for p in probs:
        print("warning:", p, file=sys.stderr)
    name = "%s_%d" % (f.stem, e["i"])
    out = Path(opt["out"] or name + ".mid")
    out.write_bytes(to_smf(ev, name, **kw))
    a = analyse(ev, opt["clamp"])
    print("%s: %d events, %.2fs/pass -> %s" % (name, h["count"], a["ms"] / 1000.0, out))


if __name__ == "__main__":
    main()
