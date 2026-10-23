#!/usr/bin/env python3
"""Pictures of the game's window (what the player sees: the picture filter,
widescreen, the pause menu), e.g. for the README.

    python tests/game_shots.py <out_dir> <first second> <last second> [VAR=value ...] [ini:key=value ...]

The game starts at once (RA_AUTOSTART) with the environment given (RA_INPUT,
RA_DISC, RA_LEVEL ...); its window is captured every half second from the
first to the last second into <out_dir>/win_<seconds>.png.  ini:key=value
changes settings.ini for the run (put back afterwards).
"""
import os
import re
import shutil
import subprocess
import sys
import time

from testlib import EXE, close, find_window
from window_shot import grab
import images


def main():
    out = os.path.abspath(sys.argv[1])
    first, last = float(sys.argv[2]), float(sys.argv[3])
    env = dict(os.environ, RA_AUTOSTART='1', RA_MUTE='1')
    ini_changes = {}
    for arg in sys.argv[4:]:
        key, value = arg.split('=', 1)
        if key.startswith('ini:'):
            ini_changes[key[4:]] = value
        else:
            env[key] = value
    os.makedirs(out, exist_ok=True)
    ini = os.path.join(os.path.dirname(EXE), 'settings.ini')
    shutil.copy(ini, ini + '.shots')
    try:
        text = open(ini, encoding='utf-8').read()
        for key, value in ini_changes.items():
            text = re.sub(r'(?m)^%s = .*$' % re.escape(key), '%s = %s' % (key, value), text)
        open(ini, 'w', encoding='utf-8').write(text)
        proc = subprocess.Popen([EXE], cwd=out, env=env)
        start = time.time()
        try:
            t = first
            while t <= last:
                time.sleep(max(0, start + t - time.time()))
                hwnd = find_window(proc.pid)
                if hwnd:
                    w, h, rows = grab(hwnd)
                    images.write_png(os.path.join(out, 'win_%05.1f.png' % t), w, h, rows)
                t += 0.5
        finally:
            close(proc)
    finally:
        shutil.copy(ini + '.shots', ini)
        os.remove(ini + '.shots')
    print(out)


if __name__ == '__main__':
    main()
