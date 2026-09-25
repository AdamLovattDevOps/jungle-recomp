"""Drive the engine step by step (./jungle FILE.BIN --drive): the bots' interface.

    g = Game('JUNGMAIN', ini_text=...)
    g.run(45000); g.click(150, 100); g.run(20000)
    g.sprites()  -> {res: (x, y, z, cel, w, h)}
"""
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Game:
    def __init__(self, scene, disc=None, ini=None, ini_text=None, wav=None, env=None):
        disc = disc or os.path.join(ROOT, 'orig/cd/JUNGLE')
        e = dict(os.environ)
        if ini:
            if ini_text is not None:
                open(ini, 'w').write(ini_text)
            e['ENGINE_INI'] = ini
        if wav:
            e['ENGINE_WAV'] = wav
        e.update(env or {})
        self.p = subprocess.Popen([os.path.join(ROOT, 'jungle'), os.path.join(disc, scene + '.BIN'), '--drive'],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=e, cwd=ROOT)

    def _read(self):
        out = []
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError('engine exited')
            line = line.rstrip('\n')
            if line == '.':
                return out
            out.append(line)

    def cmd(self, s):
        self.p.stdin.write(s + '\n'); self.p.stdin.flush()
        return self._read()

    def run(self, ms): self.cmd('run %d' % ms)
    def key(self, vk): self.cmd('key %x' % vk)
    def up(self, vk): self.cmd('up %x' % vk)
    def tap(self, vk, hold=80): self.key(vk); self.run(hold); self.up(vk)
    def click(self, x, y, right=False): self.cmd('click %d %d%s' % (x, y, ' r' if right else ''))
    def move(self, x, y): self.cmd('move %d %d' % (x, y))
    def type(self, text):
        for ch in text:
            self.cmd('char %d' % ord(ch)); self.run(50)
    def scene(self): return self.cmd('scene')[0].split('.')[0]
    def t(self): return int(self.cmd('t')[0])
    def g(self, a, b=None): return [int(v) for v in self.cmd('g %d %d' % (a, b if b is not None else a))[0].split()]
    def shot(self, path): self.cmd('shot ' + path)

    def spr(self, res):
        r = self.cmd('spr %d' % res)[0]
        return None if r == 'none' else tuple(map(int, r.split()))

    def sprites(self):
        out = {}
        for line in self.cmd('sprites'):
            r, x, y, z, cel, w, h, l, t, rr, b = map(int, line.split())
            out[r] = (x, y, z, cel, w, h, l, t, rr, b)
        return out

    def close(self):
        try:
            self.cmd('quit')
        except Exception:
            pass
        self.p.wait(timeout=30)
