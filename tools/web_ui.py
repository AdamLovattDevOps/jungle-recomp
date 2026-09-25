#!/usr/bin/env python3
"""Build the web page's art from your own disc: backdrop, characters, font.

The page (web/index.html) is dressed as one of the game's own screens: the
Score screen's bamboo frame and parchment as the backdrop, Zazu, the snake,
Timon and the pelican peeking round it, Pumbaa trotting along as the loading
bar, buttons in the PAUSE sign's wooden frame, and all text in the game's
HYENA font. Everything comes from the disc (upscaled art from build/hires/x4
when present, else the original bitmaps), so nothing here is committed.

Animations are sprite strips: every frame is placed by the bitmap's own
anchor (the engine's `left = x - ((w-1)/2 - hdr[0x0A])`), so they do not
wobble when played in CSS.

Usage:  tools/web_ui.py [--disc DIR] [--hires DIR] [--out DIR] [--scale 2]
Writes OUT/ui/* and OUT/ui/ui.json (frame sizes for the page).
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import res_dir  # noqa: E402

# name: (container, [bitmap indices], frame ms[, 'center'])
# Frames are placed by their anchors, except 'center' ones: Zazu's anchors
# carry him across the whole screen, so his flap is centred on its content.
ANIMS = {
    'zazu':    ('JUNGSCOR', list(range(140, 149)), 90, 'center'),
    'pumbaa':  ('JUNGINT2', list(range(94, 102)), 80),
    'timon':   ('JUNGOPTS', list(range(174, 180)), 140),
    'snake':   ('JUNGSCOR', [178, 179, 180, 181, 182, 181, 180, 179], 160),
    'pelican': ('JUNGSCOR', [200, 201, 202, 201], 220),
}
STILLS = {
    'backdrop': ('JUNGSCOR', 66, 'jpg'),     # bamboo frame + parchment, 800x600
    'frame':    ('JUNGOPTS', 52, 'png'),     # the PAUSE sign: its wooden border frames buttons
    'parch':    ('JUNGOPTS', 99, 'png'),     # hanging parchment panel
}


class Disc:
    def __init__(self, disc, hires, scale):
        self.disc, self.hires, self.scale = disc, hires, scale
        self.data, self.ents, self.tmp = {}, {}, tempfile.mkdtemp(prefix='jungle-ui-')

    def _load(self, cont):
        if cont not in self.data:
            d = open(os.path.join(self.disc, cont + '.BIN'), 'rb').read()
            self.data[cont] = d
            self.ents[cont] = res_dir.entries(d)[1] if isinstance(res_dir.entries(d), tuple) else res_dir.entries(d)
            out = os.path.join(self.tmp, cont)
            os.makedirs(out, exist_ok=True)
            subprocess.run([os.path.join(ROOT, 'jungle'), os.path.join(self.disc, cont + '.BIN'), '--export', out],
                           check=True, stdout=subprocess.DEVNULL)

    def header(self, cont, i):
        """(w, h, ox, oy) from the bitmap header."""
        self._load(cont)
        e = self.ents[cont][i]
        b = res_dir.fetch(self.data[cont], e) if hasattr(res_dir, 'fetch') else self.data[cont][e['offset']:e['offset'] + e['size']]
        w, h = struct.unpack_from('<H', b, 2)[0], struct.unpack_from('<H', b, 6)[0]
        ox, oy = struct.unpack_from('<hh', b, 0x0A)
        return w, h, ox, oy

    def image(self, cont, i):
        """The bitmap at SCALE, from the upscaled art when there is some."""
        self._load(cont)
        w, h, _, _ = self.header(cont, i)
        hi = os.path.join(self.hires, cont, '%05d.png' % i) if self.hires else None
        if hi and os.path.exists(hi):
            im = Image.open(hi).convert('RGBA')
        else:
            im = Image.open(os.path.join(self.tmp, cont, '%05d.png' % i)).convert('RGBA')
        return im.resize((w * self.scale, h * self.scale), Image.LANCZOS)


def strip(dsk, cont, frames, scale, center=False):
    """Frames aligned on their anchors (or centred), side by side. Returns (image, fw, fh)."""
    boxes = []
    for i in frames:
        w, h, ox, oy = dsk.header(cont, i)
        l, t = (-(w // 2), -(h // 2)) if center else (-((w - 1) // 2 - ox), -((h - 1) // 2 - oy))
        boxes.append((l, t, l + w, t + h))
    L = min(b[0] for b in boxes); T = min(b[1] for b in boxes)
    R = max(b[2] for b in boxes); B = max(b[3] for b in boxes)
    fw, fh = (R - L) * scale, (B - T) * scale
    out = Image.new('RGBA', (fw * len(frames), fh), (0, 0, 0, 0))
    for k, (i, b) in enumerate(zip(frames, boxes)):
        out.alpha_composite(dsk.image(cont, i), (k * fw + (b[0] - L) * scale, (b[1] - T) * scale))
    return out, fw, fh


def web_font(src, dst):
    """HYENA.TTF maps its glyphs at U+F020..U+F0FF (a symbol font). Add a plain
    Unicode map for U+0020..U+00FF so browsers can use it as an ordinary font."""
    from fontTools.ttLib import TTFont
    from fontTools.ttLib.tables._c_m_a_p import cmap_format_4
    f = TTFont(src)
    sym = {}
    for t in f['cmap'].tables:
        for code, g in t.cmap.items():
            if 0xF000 <= code <= 0xF0FF:
                sym[code - 0xF000] = g
    uni = cmap_format_4(4); uni.platformID, uni.platEncID, uni.language = 3, 1, 0
    uni.cmap = dict(sym)
    f['cmap'].tables = [t for t in f['cmap'].tables if not (t.platformID == 3 and t.platEncID == 0)] + [uni]
    f.save(dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--disc', default=os.path.join(ROOT, 'orig/cd/JUNGLE'))
    ap.add_argument('--hires', default=os.path.join(ROOT, 'build/hires/x4'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/web-ui'))
    ap.add_argument('--scale', type=int, default=2)
    a = ap.parse_args()
    ui = os.path.join(a.out, 'ui'); os.makedirs(ui, exist_ok=True)
    dsk = Disc(a.disc, a.hires if os.path.isdir(a.hires) else None, a.scale)
    meta = {'scale': a.scale, 'anims': {}, 'stills': {}}
    for name, (cont, i, ext) in STILLS.items():
        im = dsk.image(cont, i)
        path = os.path.join(ui, name + '.' + ext)
        if ext == 'jpg':
            im.convert('RGB').save(path, quality=86, optimize=True, progressive=True)
        else:
            im.save(path, optimize=True)
        meta['stills'][name] = {'file': name + '.' + ext, 'w': im.width, 'h': im.height}
    for name, spec in ANIMS.items():
        cont, frames, ms = spec[:3]
        im, fw, fh = strip(dsk, cont, frames, a.scale, center=len(spec) > 3 and spec[3] == 'center')
        im.save(os.path.join(ui, name + '.png'), optimize=True)
        meta['anims'][name] = {'file': name + '.png', 'w': fw, 'h': fh, 'frames': len(frames), 'ms': ms}
    web_font(os.path.join(a.disc, 'HYENA.TTF'), os.path.join(ui, 'hyena.ttf'))
    json.dump(meta, open(os.path.join(ui, 'ui.json'), 'w'), indent=1)
    total = sum(os.path.getsize(os.path.join(ui, f)) for f in os.listdir(ui))
    print('wrote %d files, %.1f MB, to %s' % (len(os.listdir(ui)), total / 1048576, ui))
    print(json.dumps(meta['anims']))


if __name__ == '__main__':
    main()
