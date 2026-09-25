#!/usr/bin/env python3
"""Boot the guest headless under QEMU and capture frames through the monitor.

This exists because DOSBox-X cannot be screenshotted without a window, and a
window cannot be opened from a background shell on this machine. QEMU can run
with -display none and still dump the framebuffer on demand, so reference frames
can be captured unattended -- which is what makes any pixel comparison against
the original possible at all.
"""
import os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
SOCK = "/tmp/jungle-qmp.sock"
OUT  = sys.argv[1] if len(sys.argv) > 1 else "/tmp/shot"
SHOTS = [int(x) for x in (sys.argv[2].split(",") if len(sys.argv) > 2 else [40, 70, 100, 130])]

if os.path.exists(SOCK):
    os.unlink(SOCK)

qemu = subprocess.Popen([
    "qemu-system-i386",
    "-m", "32",
    "-fda", os.path.join(HERE, "boot.img"),
    "-hda", os.path.join(HERE, "hda.img"),
    "-boot", "a",
    "-vga", "std",
    "-display", "none",
    "-monitor", "unix:%s,server,nowait" % SOCK,
    "-rtc", "base=localtime",
], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
   start_new_session=True)

import atexit, signal
@atexit.register
def _reap():
    """timeout(1) kills this script but not the emulator it spawned, which
    leaves an orphan QEMU that later polls mistake for a live run."""
    try:
        os.killpg(os.getpgid(qemu.pid), signal.SIGKILL)
    except Exception:
        pass

for _ in range(50):
    if os.path.exists(SOCK):
        break
    time.sleep(0.2)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(SOCK)
s.settimeout(5)          # never block the run on a silent monitor
time.sleep(0.5)
try:
    s.recv(65536)
except Exception:
    pass

def mon(cmd):
    s.sendall((cmd + "\n").encode())
    time.sleep(1.0)
    try:
        return s.recv(65536).decode(errors="replace")
    except Exception:
        return ""       # screendump is fire-and-forget; the file is the result

last = 0
for t in SHOTS:
    time.sleep(max(0, t - last)); last = t
    path = "%s_%03d.ppm" % (OUT, t)
    mon("screendump %s" % path)
    ok = os.path.exists(path) and os.path.getsize(path) > 0
    print("t=%-4ds %s %s" % (t, path, "OK %d bytes" % os.path.getsize(path) if ok else "FAILED"), flush=True)

# Convert what we captured so the caller sees images, not PPMs.
conv = os.path.join(os.path.dirname(HERE), "..", "tools", "ppm2png.py")
for t in SHOTS:
    ppm = "%s_%03d.ppm" % (OUT, t)
    if os.path.exists(ppm):
        subprocess.run([sys.executable, conv, ppm, ppm[:-4] + ".png"], check=False)

mon("quit")
time.sleep(1)
qemu.terminate()
try:
    qemu.wait(timeout=5)
except Exception:
    qemu.kill()
