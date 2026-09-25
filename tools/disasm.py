#!/usr/bin/env python3
"""Disassemble a range of 16-bit x86 from a reference segment.

Apple's LLVM objdump cannot disassemble raw binary as i8086, so this drives
radare2, which decodes 16-bit correctly. Output is plain text for side-by-side
comparison of original against freshly built code.

Usage:  disasm.py reference/JUNGR01_DLL/seg1.bin [OFFSET] [LENGTH]
"""
import subprocess
import sys
from pathlib import Path


def disasm(path, offset=0, length=128):
    cmd = ["r2", "-q", "-a", "x86", "-b", "16", "-e", "scr.color=0",
           "-c", "s %d; pD %d" % (offset, length), str(path)]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    return out.stdout or out.stderr


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = Path(sys.argv[1])
    off = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    ln = int(sys.argv[3], 0) if len(sys.argv) > 3 else 128
    print(disasm(path, off, ln))


if __name__ == "__main__":
    main()
