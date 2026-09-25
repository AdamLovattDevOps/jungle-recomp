#!/usr/bin/env python3
"""End-to-end tests: the game played headless through the engine harness.

Each scenario runs `./jungle FILE.BIN --engine OUT.png MS EVENTS` with scripted
input (menu clicks, keys, typed names), then checks what a player would see:
the scene it ended in, the settings and high scores written to 7THLEVEL.INI,
the audio written to a WAV, and the final frame. Every run starts from its own
INI in a temporary directory, so results do not depend on earlier play.

Usage:  tests/e2e.py [DISC_DIR] [-k NAME]      (default disc: orig/cd/JUNGLE)
Needs the disc data, which is not part of the repository. Exit status 0 when
every scenario passes.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import wave
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'jungle')
RATE = 22050

# Main menu targets (canvas pixels).
MENU = {
    'JUNGHIPP': (490, 280), 'JUNGBURP': (130, 430), 'JUNGPINB': (150, 100),
    'JUNGSHOT': (570, 190), 'JUNGBUGD': (680, 320),
    'JUNGOPTS': (385, 72), 'JUNGSCOR': (385, 88), 'JUNGPRTY': (385, 103),
}
SECTION = "Timon and Pumbaa's Jungle Games"


class Run:
    def __init__(self, stdout, png, wav, ini):
        self.stdout, self.png, self.wav, self.ini = stdout, png, wav, ini
        m = re.findall(r'^(JUNG\w+)\.BIN: ', stdout, re.M)
        self.scene = m[-1] if m else None
        m = re.search(r'^unimplemented records:(.*)$', stdout, re.M)
        self.unimplemented = m.group(1).strip() if m else '?'
        m = re.search(r'^builtin calls:(.*)$', stdout, re.M)
        self.builtins = m.group(1).strip() if m else '?'

    def ini_value(self, key, section=None):
        text = open(self.ini).read() if os.path.exists(self.ini) else ''
        cur = None
        for line in text.splitlines():
            if line.startswith('['):
                cur = line.strip('[]')
            elif '=' in line and (section is None or cur == section):
                k, v = line.split('=', 1)
                if k == key:
                    return v
        return None

    def wav_samples(self):
        with wave.open(self.wav) as w:
            return w.getnframes(), w.readframes(w.getnframes())


def engine(tmp, name, file, ms, events='', ini_text=None, env=None):
    png, wav, ini = (os.path.join(tmp, name + ext) for ext in ('.png', '.wav', '.ini'))
    if ini_text is not None:
        open(ini, 'w').write(ini_text)
    e = dict(os.environ, ENGINE_WAV=wav, ENGINE_INI=ini)
    e.update(env or {})
    args = [BIN, os.path.join(DISC, file + '.BIN'), '--engine', png, str(ms)]
    if events:
        args.append(events)
    out = subprocess.run(args, env=e, capture_output=True, text=True, timeout=600, cwd=ROOT)
    return Run(out.stdout + out.stderr, png, wav, ini)


def png_pixels(path):
    """Decode the harness's PNG (8-bit RGB or palette, no interlace) to RGB rows."""
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    p, idat, pal = 8, b'', None
    while p < len(d):
        n, typ = struct.unpack('>I4s', d[p:p + 8])
        body = d[p + 8:p + 8 + n]
        if typ == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
        elif typ == b'PLTE':
            pal = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif typ == b'IDAT':
            idat += body
        p += 12 + n
    raw = zlib.decompress(idat)
    bpp = 3 if ctype == 2 else 1
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):                      # PNG filters
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line) if bpp == 3 else bytes(b for i in line for b in pal[i]))
        prev = line
    return w, h, rows


def brightness(path):
    w, h, rows = png_pixels(path)
    total = sum(sum(r[::9]) for r in rows[::4])      # sample every third pixel of every fourth row
    return total / (len(rows[::4]) * len(rows[0][::9]))


def rms(pcm):
    n = len(pcm) // 2
    if not n:
        return 0
    v = struct.unpack('<%dh' % n, pcm)
    return (sum(x * x for x in v) / n) ** 0.5


def hops(start, end, step=900):
    return ','.join('%d=26' % t for t in range(start, end, step))


SEASONED = '[%s]\nFreeSpace=256\n\n[%s.HippoHop]\nIntro=0\nEntries=20\n' % (SECTION, SECTION)

# ---- scenarios ---------------------------------------------------------------

def t_boot(tmp):
    """Unattended boot: logo, title and intro, then the main menu."""
    r = engine(tmp, 'boot', 'JUNGLE', 240000)
    yield r.scene == 'JUNGMAIN', 'ends in JUNGMAIN (got %s)' % r.scene
    yield r.unimplemented == '', 'no unimplemented records (%s)' % r.unimplemented
    yield brightness(r.png) > 40, 'menu frame is not blank'


def t_av_clock(tmp):
    """Sound is mixed at exactly real time: MS of play gives MS * 22.05 samples."""
    for ms in (60000, 240000):
        r = engine(tmp, 'av%d' % ms, 'JUNGLE', ms)
        n, _ = r.wav_samples()
        yield n == ms * RATE // 1000, '%d ms of play gives %d samples (got %d, %+.2f%%)' % (
            ms, ms * RATE // 1000, n, 100.0 * (n - ms * RATE / 1000) / (ms * RATE / 1000))


def t_av_events(tmp):
    """Each clip starts in the mix at the engine time its script played it."""
    r = engine(tmp, 'avev', 'JUNGLE', 240000, env={'ENGINE_AVLOG': '1'})
    starts = [(int(a), int(b)) for a, b in re.findall(r'^av: clip \d+ at (\d+) ms sample (\d+)', r.stdout, re.M)]
    yield len(starts) > 5, '%d clip starts logged' % len(starts)
    worst = max((abs(s - t * RATE // 1000) for t, s in starts), default=0)
    yield worst <= RATE * 20 // 1000, 'worst clip start offset %.1f ms (limit 20 ms)' % (worst * 1000 / RATE)


def t_menu(tmp):
    """Every main-menu target opens its scene."""
    for scene, (x, y) in MENU.items():
        r = engine(tmp, 'menu_' + scene, 'JUNGMAIN', 70000, '45000=@%d:%d' % (x, y), SEASONED)
        yield r.scene == scene, '%s from the menu (got %s)' % (scene, r.scene)


def t_hippo_game(tmp):
    """Hippo Hop from the menu: hop, lose every life, enter a name, quit to the menu."""
    ev = '45000=@490:280,' + hops(62000, 91000) + ',97000=41,97500=44,98000=41,98500=4D,100000=0D'
    r = engine(tmp, 'hippo', 'JUNGMAIN', 103000, ev, SEASONED)
    score = r.ini_value('Score1', SECTION + '.HippoHop')
    yield score is not None and score.strip().isdigit() and int(score) > 0, 'a score is recorded (Score1=%r)' % score
    yield r.ini_value('LASTSLOT', SECTION + '.HippoHop') == '0', 'it takes the top slot'
    yield r.ini_value('Name1', SECTION + '.HippoHop') == 'adam', 'the typed name is saved (Name1=%r)' % r.ini_value('Name1')
    q = engine(tmp, 'hippo_quit', 'JUNGMAIN', 125000, ev + ',104000=@390:324', SEASONED)
    yield q.scene == 'JUNGMAIN', 'QUIT returns to the menu (got %s)' % q.scene
    n = engine(tmp, 'hippo_new', 'JUNGMAIN', 125000, ev + ',104000=@491:324', SEASONED)
    yield n.scene == 'JUNGHIPP', 'NEW GAME stays in Hippo Hop (got %s)' % n.scene
    s = engine(tmp, 'hippo_scores', 'JUNGSCOR', 10000, '', open(r.ini).read())
    yield s.scene == 'JUNGSCOR' and brightness(s.png) > 40, 'the Score screen opens with the new table'


def t_music(tmp):
    """Music plays in Hippo Hop, and is silent with ENGINE_MUSIC=0."""
    ev = '45000=@490:280'
    on = engine(tmp, 'mus_on', 'JUNGMAIN', 90000, ev, SEASONED)
    off = engine(tmp, 'mus_off', 'JUNGMAIN', 90000, ev, SEASONED, {'ENGINE_MUSIC': '0'})
    _, a = on.wav_samples()
    _, b = off.wav_samples()
    lo = RATE * 70 * 2                               # 70 s on: the game is running
    va = struct.unpack('<%dh' % ((len(a) - lo) // 2), a[lo:])
    vb = struct.unpack('<%dh' % ((len(b) - lo) // 2), b[lo:])
    level = (sum((x - y) ** 2 for x, y in zip(va, vb)) / max(1, len(va))) ** 0.5
    yield level > 300, 'music level %.0f RMS during play (sound effects excluded)' % level


def t_options(tmp):
    """Options: turn music off and exit; the choice is saved."""
    r = engine(tmp, 'opts', 'JUNGOPTS', 20000, '6000=@556:201,9000=@460:296', SEASONED)
    yield r.ini_value('MusicOff', SECTION) == '1', 'MusicOff=1 saved (got %r)' % r.ini_value('MusicOff', SECTION)


def t_containers(tmp):
    """Every scene runs 15 s without an unimplemented record or builtin."""
    for f in sorted(os.listdir(DISC)):
        if not f.upper().endswith('.BIN'):
            continue
        r = engine(tmp, 'c_' + f[:-4], f[:-4], 15000)
        yield r.unimplemented == '' and r.builtins == '', '%s clean (records: %s; builtins: %s)' % (
            f, r.unimplemented or '-', r.builtins or '-')


def t_deterministic(tmp):
    """The same input gives the same frame and the same sound, so runs can be compared."""
    ev = '45000=@490:280,' + hops(62000, 80000)
    a = engine(tmp, 'det_a', 'JUNGMAIN', 85000, ev, SEASONED)
    b = engine(tmp, 'det_b', 'JUNGMAIN', 85000, ev, SEASONED)
    yield open(a.png, 'rb').read() == open(b.png, 'rb').read(), 'identical final frames'
    yield open(a.wav, 'rb').read() == open(b.wav, 'rb').read(), 'identical audio'


def t_bots(tmp):
    """Every game played from the menu by its bot to a high score, beating an existing table."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import bots
    for game in bots.GAMES:
        table = [('ace', 150), ('bee', 120), ('cub', 100), ('doe', 90), ('elk', 80),
                 ('fox', 70), ('gnu', 60), ('hog', 50), ('ibis', 40), ('jay', 30)]
        if game == 'bugdrop':
            table = [(n, sc // 10) for n, sc in table]    # Bug Drop scores run lower
        d = bots.play(game, os.path.join(tmp, 'bot_%s.ini' % game), table=table,
                      shot=os.path.join(tmp, 'bot_%s.png' % game))
        sc = d.get('Score1', '').strip()
        yield (sc.isdigit() and d.get('Name1') == 'bot' and d.get('LASTSLOT') == '0'
               and d.get('Name2') == table[0][0]), \
            '%s: bot scores %s and tops the table (Name1=%r, Name2=%r)' % (game, sc or '-', d.get('Name1'), d.get('Name2'))


def bmp_rgb(path):
    d = open(path, 'rb').read()
    off, w, h, bpp = struct.unpack_from('<I', d, 10)[0], *struct.unpack_from('<iiH', d, 18)[0:2], struct.unpack_from('<H', d, 28)[0]
    rows = []
    stride = (w * bpp // 8 + 3) & ~3
    for y in range(abs(h)):
        r = d[off + y * stride: off + y * stride + w * bpp // 8]
        rows.append(bytes(b for i in range(0, len(r), bpp // 8) for b in (r[i + 2], r[i + 1], r[i])))
    return rows if h < 0 else rows[::-1]


def t_hires(tmp):
    """The GPU renderer (hires.c) paints the classic frame exactly when it has no art cache."""
    for scene in ('JUNGMAIN', 'JUNGPINB', 'JUNGSHOT'):
        shot = os.path.join(tmp, 'hr_%s.bmp' % scene)
        r = engine(tmp, 'hr_' + scene, scene, 30000, env={'ENGINE_HIRES_SHOT': shot, 'ENGINE_HIRES_SCALE': '1',
                                                        'JUNGLE_HIRES': os.path.join(tmp, 'no-cache')})
        a = png_pixels(r.png)[2]; b = bmp_rgb(shot)
        diff = sum(x != y for ra, rb in zip(a, b) for x, y in zip(ra, rb))
        yield diff == 0, '%s: %d of %d values differ' % (scene, diff, 800 * 600 * 3)


SCENARIOS = [t_boot, t_av_clock, t_av_events, t_menu, t_hippo_game, t_music, t_options, t_containers, t_deterministic, t_hires, t_bots]


def main():
    global DISC
    ap = argparse.ArgumentParser()
    ap.add_argument('disc', nargs='?', default=os.path.join(ROOT, 'orig/cd/JUNGLE'))
    ap.add_argument('-k', help='run scenarios whose name contains this')
    ap.add_argument('--keep', action='store_true', help='keep the frames, WAVs and INIs')
    a = ap.parse_args()
    DISC = a.disc
    if not os.path.exists(os.path.join(DISC, 'JUNGLE.BIN')):
        print('no disc data at %s' % DISC); return 2
    tmp = tempfile.mkdtemp(prefix='jungle-e2e-')
    fails = 0
    for sc in SCENARIOS:
        name = sc.__name__[2:]
        if a.k and a.k not in name:
            continue
        t0 = time.time()
        try:
            results = list(sc(tmp))
        except Exception as ex:                      # a crash or a harness failure is a failed scenario
            results = [(False, 'raised %s: %s' % (type(ex).__name__, ex))]
        ok = all(r for r, _ in results)
        fails += not ok
        print('%s %-14s %5.1fs  %s' % ('PASS' if ok else 'FAIL', name, time.time() - t0, sc.__doc__.strip().splitlines()[0]))
        for r, msg in results:
            print('       %s %s' % ('ok ' if r else 'BAD', msg))
    print('\n%s: %d scenario(s) failed' % ('FAIL' if fails else 'PASS', fails) if fails else '\nPASS: all scenarios')
    if a.keep:
        print('artifacts in', tmp)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
