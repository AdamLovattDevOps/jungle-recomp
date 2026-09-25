#!/usr/bin/env python3
"""Extract type-7 audio resources from a "7L" container as RIFF/WAV.

RESCREATEWAVEEVENT reads a 28-byte header off the front of the resource before
handing the rest to the streaming callbacks (AUDIO_READ / AUDIO_REWIND /
AUDIO_DONE on tagRESAUDIOINFO). That header is:

  0x00  u32  dataSize       payload bytes, i.e. resourceSize - 28
  0x04  u32  flags          always 1 on this disc
  0x08  u16  param          0, 10 or 100
  0x0A  u16  id
  0x0C  WAVEFORMATEX, 16 bytes:
          u16 wFormatTag       1 = WAVE_FORMAT_PCM
          u16 nChannels        1
          u32 nSamplesPerSec   22050
          u32 nAvgBytesPerSec  44100
          u16 nBlockAlign      2
          u16 wBitsPerSample   16

Every type-7 resource on this disc declares 22050 Hz mono 16-bit PCM, and
dataSize matches the stored length exactly.

The payload is 4-bit ADPCM, not raw PCM: the WAVEFORMATEX describes the DECODED
output. RESCREATEWAVEEVENT hands AUDIO_READ to A_011 in JUNGA01.DLL, and the
codec lives there (FUN_1000_3f10). See tools/adpcm.py, which lifts the step and
index tables straight out of the binary.

Decoding is verified statistically: mean |delta| / RMS drops from 1.03 (noise)
to 0.09-0.67, DC offset from -5640 to near zero, and the amplitude histogram
from flat to sharply peaked at zero - which is what real PCM looks like.

Usage:  res_audio.py [FILE.BIN ...] [--write]
"""
import struct
import sys
from collections import Counter
from pathlib import Path

from res_dir import entries
import adpcm

ROOT = Path(__file__).resolve().parent.parent
BINDIR = ROOT / "orig" / "cd" / "JUNGLE"
OUT = ROOT / "assets_extracted"
HDR = 28
STEP, IDX = adpcm.load_tables()


def parse_audio(blob):
    data_size, flags = struct.unpack_from("<II", blob, 0)
    param, rid = struct.unpack_from("<HH", blob, 8)
    tag, ch, rate, avg, align, bits = struct.unpack_from("<HHIIHH", blob, 12)
    return {
        "data_size": data_size, "flags": flags, "param": param, "id": rid,
        "tag": tag, "channels": ch, "rate": rate, "avg": avg,
        "align": align, "bits": bits,
    }


def wav_name(idx, a):
    return "%05d_id%d.wav" % (idx, a["id"])


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    write = "--write" in sys.argv
    files = [Path(a) for a in args] if args else sorted(BINDIR.glob("*.BIN"))

    total = written = odd = 0
    fmts = Counter()
    print("%-13s %6s %10s  %s" % ("file", "count", "pcm bytes", "format"))
    for f in files:
        path = f if f.exists() else BINDIR / f.name
        data = path.read_bytes()
        _, ents = entries(data)
        t7 = [e for e in ents if e["type"] == 7]
        if not t7:
            continue
        d = OUT / path.stem / "audio"
        if write:
            d.mkdir(parents=True, exist_ok=True)

        pcm_total = 0
        for e in t7:
            blob = data[e["offset"]:e["offset"] + e["size"]]
            a = parse_audio(blob)
            fmts["%dHz %dch %dbit tag%d" % (a["rate"], a["channels"], a["bits"], a["tag"])] += 1
            pcm = blob[HDR:HDR + a["data_size"]]
            if len(pcm) != e["size"] - HDR:
                odd += 1
            pcm_total += len(pcm)
            total += 1
            if write:
                decoded = adpcm.decode(pcm, STEP, IDX)
                (d / wav_name(e["i"], a)).write_bytes(
                    adpcm.wav(decoded, a["channels"], a["rate"], a["bits"]))
                written += 1
        print("%-13s %6d %10d  %s"
              % (path.name, len(t7), pcm_total,
                 "%dHz %dch %dbit" % (t7 and parse_audio(
                     data[t7[0]["offset"]:t7[0]["offset"] + 28])["rate"],
                     parse_audio(data[t7[0]["offset"]:t7[0]["offset"] + 28])["channels"],
                     parse_audio(data[t7[0]["offset"]:t7[0]["offset"] + 28])["bits"])))

    print("\ntotal type-7: %d   formats seen: %s" % (total, dict(fmts)))
    print("resources whose stored length exceeds dataSize: %d" % odd)
    if write:
        print("wrote %d decoded WAVs -> %s" % (written, OUT))


if __name__ == "__main__":
    main()
