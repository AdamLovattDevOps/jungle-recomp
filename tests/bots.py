"""Bots that play each game through the --drive interface (see drive.py).

Each takes a Game already in its scene and plays until the game ends, then
returns the score the game recorded. They read the same things a player sees,
sprite positions, and press the game's own keys.
"""
import os
import time

VK_Z, VK_SLASH, VK_ENTER, VK_SPACE = 0x5A, 0xBF, 0x0D, 0x20



def ini_has(path, key):
    try:
        return any(l.startswith(key + '=') for l in open(path))
    except OSError:
        return False


def pinball(g, ini, limit_ms=900000, trace=None, y_lo=215, y_hi=280, x_span=160, hold=250, cool=150, shake=True, on_stuck=None):
    """Flip the flipper on the side the ball is falling towards, once it is
    over the flippers; launch whenever a ball waits in the plunger lane; shake
    the table (Space) when a ball comes to rest anywhere else. The table's own
    collision scripts can wedge a ball on a post, as a player would see, and
    a few shakes free it.

    The table keeps up to three balls: x in g1900..1902, y in g1903..1905,
    in play when g1917..1919 is 1. The lowest ball in play is the one to save."""
    held = {}                                          # vk -> release time
    ready = {VK_Z: 0, VK_SLASH: 0}                     # vk -> earliest next press
    prev = None; t = g.t(); t_end = t + limit_ms; launched_at = -10000
    flips = launches = shakes = 0
    still_since = t; last_shake = -10000; idle_since = t
    while t < t_end:
        g.run(16); t += 16
        for vk, until in list(held.items()):
            if t >= until:
                g.up(vk); del held[vk]; ready[vk] = t + cool
        if ini_has(ini, 'LASTSLOT'):
            break
        v = g.g(1900, 1919)
        balls = [(v[3 + i], v[i]) for i in range(3) if v[17 + i] == 1]   # (y, x) of balls in play
        if not balls:
            prev = None
            if t - idle_since > 4000 and t - launched_at > 3000:        # nothing in play for a while: launch
                g.key(VK_ENTER); g.run(1200); t += 1200; g.up(VK_ENTER)
                launched_at = t; launches += 1
            continue
        idle_since = t
        y, x = max(balls)
        vy = y - prev[1] if prev else 0
        if prev is None or (x, y) != prev:
            still_since = t
        prev = (x, y)
        if trace is not None:
            trace.append((t, x, y))
        if x > 300 and y > 100 and abs(vy) < 2:                         # resting on the plunger
            if t - launched_at > 1500:
                g.key(VK_ENTER); g.run(1200); t += 1200; g.up(VK_ENTER)
                launched_at = t; launches += 1
            continue
        if t - still_since > 800 and on_stuck and not (x > 300 and y > 100):
            if on_stuck(g, t, x, y) is False:
                return {'flips': flips, 'launches': launches, 'shakes': shakes, 't': t, 'stuck': (x, y)}
        if shake and t - still_since > 800 and t - last_shake > 900:
            g.key(VK_SPACE); g.run(60); t += 60; g.up(VK_SPACE)
            last_shake = t; shakes += 1
            continue
        if y_lo < y + 2 * vy < y_hi and vy >= 0 and abs(x) < x_span:
            vk = VK_Z if x < 0 else VK_SLASH
            if vk not in held and t >= ready[vk]:
                g.key(vk); held[vk] = t + hold; flips += 1
    return {'flips': flips, 'launches': launches, 'shakes': shakes, 't': t}

def mash(g, ini, keys, rng, limit_ms=900000, hold=(60, 300), gap=(50, 400), shake=None):
    """Press keys at random (a list of virtual keys) until the game records a
    result: the simplest player, for games whose scoring is not modelled."""
    t = g.t(); t_end = t + limit_ms
    while t < t_end:
        if ini_has(ini, 'LASTSLOT'):
            return {'t': t}
        vk = rng.choice(keys)
        h = rng.randint(*hold); gp = rng.randint(*gap)
        g.key(vk); g.run(h); g.up(vk); g.run(gp); t += h + gp
    return {'t': t}


def ini_score(ini):
    for l in open(ini):
        if l.startswith('LASTSCORE='):
            v = l.split('=', 1)[1].strip()
            return int(v) if v.isdigit() else 0
    return None


def shooter(g, ini, limit_ms=900000, every=150):
    """Sling Shooter: shoot (click) whatever has popped up in the field since
    the round began, keeping clear of the HUD and of Timon at the bottom."""
    base = set(g.sprites())
    t = g.t(); t_end = t + limit_ms; shots = 0; aimed = {}
    while t < t_end:
        if ini_has(ini, 'LASTSLOT'):
            break
        for r, v in g.sprites().items():
            l, tp, rr, b = v[6:]
            cx, cy = (l + rr) // 2, (tp + b) // 2
            if r in base or not (20 < rr - l < 220) or not (60 < cy < 470) or not (0 < cx < 800):
                continue
            if t - aimed.get(r, -10000) < 600:
                continue
            g.click(cx, cy); shots += 1; t += 112; aimed[r] = t
            break
        g.run(every); t += every
    return {'t': t, 'shots': shots}


SECTION = "Timon and Pumbaa's Jungle Games"
GAMES = {                                      # name: (INI section, menu target, scene)
    'hippo': ('HippoHop', (490, 280), 'JUNGHIPP'),
    'burper': ('Burper', (130, 430), 'JUNGBURP'),
    'pinball': ('Pinball', (150, 100), 'JUNGPINB'),
    'bugdrop': ('BugDrop', (680, 320), 'JUNGBUGD'),
    'sling': ('SlingShooter', (570, 190), 'JUNGSHOT'),
}
SEASONED = '[%s]\nFreeSpace=256\n\n' % SECTION + ''.join(
    '[%s.%s]\nIntro=0\nEntries=20\n\n' % (SECTION, sec) for sec, _, _ in GAMES.values())


def ini_section(ini, section):
    out, cur = {}, None
    for line in open(ini):
        line = line.rstrip('\n')
        if line.startswith('['):
            cur = line.strip('[]')
        elif '=' in line and cur == section:
            k, v = line.split('=', 1); out[k] = v
    return out


def play(game, ini, name='bot', seed=1, shot=None, table=None):
    """From the main menu: open GAME, play it to game over with its bot,
    type NAME into the high-score panel. Returns the game's INI section."""
    import random
    from drive import Game
    sec, (mx, my), scene = GAMES[game]
    rng = random.Random(seed)
    text = SEASONED
    if table:                                        # an existing high-score table, best first
        head = '[%s.%s]\n' % (SECTION, sec)
        rows = ''.join('Name%d=%s\nScore%d=%9s\n' % (i + 1, n, i + 1, sc) for i, (n, sc) in enumerate(table))
        text = text.replace(head, head + rows)
    g = Game('JUNGMAIN', ini=ini, ini_text=text)
    try:
        g.run(45000); g.click(mx, my); g.run(12000)
        assert g.scene() == scene, 'menu opened %s, not %s' % (g.scene(), scene)
        if game == 'hippo':
            mash(g, ini, [0x26, 0x26, 0x26, 0x25, 0x27], rng)
        elif game == 'burper':
            mash(g, ini, [0x58, 0x58, 0x25, 0x27], rng)
        elif game == 'pinball':
            pinball(g, ini, y_lo=215, y_hi=280, hold=250, x_span=160)
        elif game == 'bugdrop':
            for _ in range(15):                            # "Player choose your log": the left one
                g.click(190, 330); g.run(2000)
            mash(g, ini, [0x25, 0x27, 0xDE, 0xBA, 0x28], rng)
        elif game == 'sling':
            g.tap(0x20); g.run(3000)
            shooter(g, ini)
        g.run(5000)                                 # the high-score panel opens
        g.type(name); g.key(0x0D); g.run(200); g.up(0x0D); g.run(3000)
        if shot:
            g.shot(shot)
    finally:
        g.close()
    return ini_section(ini, '%s.%s' % (SECTION, sec))
