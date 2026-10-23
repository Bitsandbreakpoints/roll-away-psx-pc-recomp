#!/usr/bin/env python3
"""Starts the level editor (build/roll_away_editor.exe), optionally with a
level, and saves a picture of its window (PrintWindow, no focus needed).

    python tests/editor_shot.py <out.png> [level] [seconds] [keys]
"""
import os
import subprocess
import sys
import time

from testlib import close, find_window, post_keys
from window_shot import grab
import images

EDITOR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build',
                      'roll_away_editor.exe')


def main():
    out = os.path.abspath(sys.argv[1])
    level = [os.path.abspath(sys.argv[2])] if len(sys.argv) > 2 and sys.argv[2] else []
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 3
    proc = subprocess.Popen([EDITOR] + level, cwd=os.path.dirname(EDITOR))
    try:
        time.sleep(seconds)
        hwnd = find_window(proc.pid)
        if len(sys.argv) > 4:
            post_keys(hwnd, sys.argv[4])
            time.sleep(1)
        w, h, rows = grab(hwnd)
        images.write_png(out, w, h, rows)
        print(out, w, h)
    finally:
        close(proc)


if __name__ == '__main__':
    main()
