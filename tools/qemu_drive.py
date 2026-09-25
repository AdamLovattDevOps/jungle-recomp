#!/usr/bin/env python3
"""Boot the reference guest and drive it: screenshots AND keystrokes.

qemu_capture.py can only watch. This can also type, which is what a period
installer or a game menu needs. QEMU's monitor exposes `sendkey`, so the guest
can be driven with no window, no host focus, and nobody present.

Script is a comma-separated list of steps, each `SECONDS:ACTION`:

    40:shot            screendump at t=40s
    45:key:ret         press Enter
    46:key:kp_enter    keypad Enter (some DOS installers only accept this)
    50:key:y           press Y
    55:keys:c,l,ret    a short sequence, 120ms apart

Times are absolute seconds from boot, and must be non-decreasing.

Usage:  qemu_drive.py OUTPREFIX "40:shot,45:key:ret,60:shot"
"""
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GUEST = os.path.join(os.path.dirname(HERE), "guest", "qemu")
SOCK = "/tmp/jungle-drive.sock"


def main():
    out = sys.argv[1]
    steps = []
    for part in sys.argv[2].split(","):
        t, _, action = part.partition(":")
        steps.append((float(t), action))

    if os.path.exists(SOCK):
        os.unlink(SOCK)

    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "32",
        "-fda", os.path.join(GUEST, "boot.img"),
        "-hda", os.path.join(GUEST, "hda.img"),
        "-boot", "a", "-vga", "std", "-display", "none",
        "-monitor", "unix:%s,server,nowait" % SOCK,
        "-rtc", "base=localtime",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, start_new_session=True)

    import atexit, signal
    @atexit.register
    def _reap():
        """timeout(1) kills this script but not the emulator it spawned."""
        try:
            os.killpg(os.getpgid(qemu.pid), signal.SIGKILL)
        except Exception:
            pass

    for _ in range(60):
        if os.path.exists(SOCK):
            break
        time.sleep(0.2)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.settimeout(4)
    time.sleep(0.5)
    try:
        s.recv(65536)
    except Exception:
        pass

    def mon(cmd):
        s.sendall((cmd + "\n").encode())
        time.sleep(0.35)
        try:
            s.recv(65536)
        except Exception:
            pass

    start = time.time()
    for t, action in steps:
        wait = t - (time.time() - start)
        if wait > 0:
            time.sleep(wait)
        if action == "shot":
            path = "%s_%03d.ppm" % (out, int(t))
            mon("screendump %s" % path)
            ok = os.path.exists(path)
            print("t=%-5.0f shot %s %s" % (t, path, "OK" if ok else "FAILED"), flush=True)
            if ok:
                subprocess.run([sys.executable, os.path.join(HERE, "ppm2png.py"),
                                path, path[:-4] + ".png"], check=False)
        elif action.startswith("key:"):
            mon("sendkey %s" % action[4:])
            print("t=%-5.0f key %s" % (t, action[4:]), flush=True)
        elif action.startswith("keys:"):
            for k in action[5:].split("|"):
                mon("sendkey %s" % k)
                time.sleep(0.12)
            print("t=%-5.0f keys %s" % (t, action[5:]), flush=True)

    mon("quit")
    time.sleep(1)
    qemu.terminate()


if __name__ == "__main__":
    main()
