#!/usr/bin/env python3
"""Drives the level editor with window messages (no focus needed) and
saves pictures of it: a script of steps separated by ';'

    move X Y | click X Y | rclick X Y | drag X1 Y1 X2 Y2 | rdrag X1 Y1 X2 Y2 |
    wheel N | key NAME [ctrl] | wait MS | shot FILE.png

    python tests/editor_drive.py [level] "click 1280 700; shot build/test/a.png"
X, Y are client pixels.
"""
import ctypes
import os
import subprocess
import sys
import time

from testlib import KEYS, close, find_window
from window_shot import grab
from editor_shot import EDITOR
import images

user32 = ctypes.windll.user32
WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP = 0x200, 0x201, 0x202
WM_RBUTTONDOWN, WM_RBUTTONUP, WM_MOUSEWHEEL = 0x204, 0x205, 0x20A
WM_KEYDOWN, WM_KEYUP = 0x100, 0x101
MK_LBUTTON, MK_RBUTTON = 1, 2


def lp(x, y):
    return (y & 0xFFFF) << 16 | (x & 0xFFFF)


def mouse(hwnd, msg, x, y, flags=0):
    user32.PostMessageW(hwnd, msg, flags, lp(x, y))
    time.sleep(0.05)


def drag(hwnd, x1, y1, x2, y2, down, up, flag):
    mouse(hwnd, WM_MOUSEMOVE, x1, y1)
    time.sleep(0.1)
    mouse(hwnd, down, x1, y1, flag)
    for i in range(1, 21):
        mouse(hwnd, WM_MOUSEMOVE, x1 + (x2 - x1) * i // 20, y1 + (y2 - y1) * i // 20, flag)
    mouse(hwnd, up, x2, y2)


def key(hwnd, name, ctrl=False):
    special = {'f5': 0x74, 'del': 0x2E, 'pgup': 0x21, 'pgdn': 0x22}
    code = KEYS.get(name) or special.get(name) or ord(name.upper())
    codes = ([0x11] if ctrl else []) + [code]
    for c in codes:
        user32.PostMessageW(hwnd, WM_KEYDOWN, c, 1)
        time.sleep(0.05)
    time.sleep(0.1)
    for c in reversed(codes):
        user32.PostMessageW(hwnd, WM_KEYUP, c, 0xC0000001)
        time.sleep(0.05)


def main():
    level = sys.argv[1] if len(sys.argv) > 2 else ''
    script = sys.argv[-1]
    proc = subprocess.Popen([EDITOR] + ([os.path.abspath(level)] if level else []),
                            cwd=os.path.dirname(EDITOR), env=dict(os.environ, RA_EDITOR_TEST='1'))
    try:
        time.sleep(3)
        hwnd = find_window(proc.pid)
        for step in script.split(';'):
            a = step.split()
            if not a:
                continue
            n = [int(v) for v in a[1:] if v.lstrip('-').isdigit()]
            if a[0] == 'move':
                mouse(hwnd, WM_MOUSEMOVE, *n)
            elif a[0] == 'click':
                mouse(hwnd, WM_MOUSEMOVE, *n)
                time.sleep(0.15)
                mouse(hwnd, WM_LBUTTONDOWN, n[0], n[1], MK_LBUTTON)
                time.sleep(0.1)
                mouse(hwnd, WM_LBUTTONUP, n[0], n[1])
            elif a[0] == 'drag':
                drag(hwnd, *n, WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON)
            elif a[0] == 'rdrag':
                drag(hwnd, *n, WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON)
            elif a[0] == 'wheel':
                user32.PostMessageW(hwnd, WM_MOUSEWHEEL, (n[0] * 120 & 0xFFFF) << 16, lp(1280, 700))
            elif a[0] == 'key':
                key(hwnd, a[1], len(a) > 2 and a[2] == 'ctrl')
            elif a[0] == 'wait':
                time.sleep(n[0] / 1000)
            elif a[0] == 'shot':
                time.sleep(0.4)
                w, h, rows = grab(hwnd)
                images.write_png(os.path.abspath(a[1]), w, h, rows)
                print(a[1])
            time.sleep(0.15)
    finally:
        close(proc)


if __name__ == '__main__':
    main()
