#!/usr/bin/env python3
"""Build the high-resolution art cache from your own disc.

Every bitmap in every container is exported (./jungle FILE.BIN --export),
upscaled 4x with Real-ESRGAN's anime model (realesrgan-ncnn-vulkan, which runs
on the GPU through Vulkan/MoltenVK), then resampled to the target scale with
Lanczos, keeping the alpha channel. The engine draws these in place of the
original 8-bit bitmaps when --hires is given (see docs/PLATFORMS.md).

Usage:  tools/upscale.py [--disc DIR] [--out DIR] [--scale 3] [--esrgan PATH]

Output goes to OUT/x<SCALE>/<CONTAINER>/<NNNNN>.png. Scale 4 is the model's own
output, kept as is; other scales are resampled from it. The game makes 1x and
2x from x4 when it loads them, so x3 and x4 cover every level up to 4x.

Defaults: disc orig/cd/JUNGLE, out build/hires, scale 3, and the ncnn build
unpacked at build/deps/esrgan. Nothing here is committed: the art is the disc's.
Runs that are interrupted resume; finished containers are skipped.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ProcessPoolExecutor

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def resample(args):
    src, dst, w, h = args
    im = Image.open(src).convert('RGBA')
    im = im.resize((w, h), Image.LANCZOS)
    im.save(dst, optimize=False, compress_level=3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--disc', default=os.path.join(ROOT, 'orig/cd/JUNGLE'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/hires'))
    ap.add_argument('--scale', type=int, default=3)
    ap.add_argument('--esrgan', default=os.path.join(ROOT, 'build/deps/esrgan'))
    ap.add_argument('--only', help='one container, e.g. JUNGMAIN')
    a = ap.parse_args()
    exe = os.path.join(a.esrgan, 'realesrgan-ncnn-vulkan')
    if not os.path.exists(exe):
        sys.exit('realesrgan-ncnn-vulkan not found in %s (see docs/PLATFORMS.md)' % a.esrgan)
    bins = sorted(f[:-4] for f in os.listdir(a.disc) if f.upper().endswith('.BIN'))
    if a.only:
        bins = [a.only]
    for name in bins:
        dst_dir = os.path.join(a.out, 'x%d' % a.scale, name)
        done_mark = os.path.join(dst_dir, '.done-x%d' % a.scale)
        if os.path.exists(done_mark):
            print('%-9s done already' % name); continue
        t0 = time.time()
        with tempfile.TemporaryDirectory(prefix='jungle-hires-') as tmp:
            src, up = os.path.join(tmp, 'src'), os.path.join(tmp, 'up')
            os.makedirs(src); os.makedirs(up)
            subprocess.run([os.path.join(ROOT, 'jungle'), os.path.join(a.disc, name + '.BIN'), '--export', src],
                           check=True, stdout=subprocess.DEVNULL)
            subprocess.run([exe, '-i', src, '-o', up, '-n', 'realesrgan-x4plus-anime', '-s', '4', '-f', 'png',
                            '-m', os.path.join(a.esrgan, 'models')], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            os.makedirs(dst_dir, exist_ok=True)
            if a.scale == 4:                         # the model's own output
                n = 0
                for f in sorted(os.listdir(up)):
                    shutil.move(os.path.join(up, f), os.path.join(dst_dir, f)); n += 1
                open(done_mark, 'w').write('%d\n' % n)
                print('%-9s %5d bitmaps in %5.0f s' % (name, n, time.time() - t0), flush=True)
                continue
            jobs = []
            for f in sorted(os.listdir(src)):
                if not os.path.exists(os.path.join(up, f)):
                    continue
                w, h = Image.open(os.path.join(src, f)).size
                jobs.append((os.path.join(up, f), os.path.join(dst_dir, f), w * a.scale, h * a.scale))
            with ProcessPoolExecutor() as ex:
                list(ex.map(resample, jobs, chunksize=16))
        open(done_mark, 'w').write('%d\n' % len(jobs))
        print('%-9s %5d bitmaps in %5.0f s' % (name, len(jobs), time.time() - t0), flush=True)


if __name__ == '__main__':
    main()
