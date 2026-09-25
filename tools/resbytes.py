#!/usr/bin/env python3
"""Print or hash one resource's raw bytes.

Usage:  resbytes.py FILE.BIN INDEX [--md5]
"""
import hashlib
import sys

import res_dir


def get(path, index):
    data = open(path, "rb").read()
    _, ents = res_dir.entries(data)
    return ents[index], res_dir.fetch(data, ents[index])


if __name__ == "__main__":
    e, b = get(sys.argv[1], int(sys.argv[2]))
    if "--md5" in sys.argv:
        print(hashlib.md5(b).hexdigest(), e["type"], len(b))
    else:
        print(f"type {e['type']}  {len(b)} bytes")
        for i in range(0, len(b), 16):
            print(f"{i:04x}  " + b[i:i + 16].hex(" "))
