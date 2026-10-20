#!/usr/bin/env python3
"""Starts the port without the game (the launcher) and saves a picture of
its window (PrintWindow, no focus needed), e.g. to check the launcher.

    python tests/window_shot.py <out.png> [seconds] [keys]
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

from testlib import EXE, close, find_window, post_keys
import images

user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
PW_RENDERFULLCONTENT = 2


def grab(hwnd):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right, r.bottom
    hdc = user32.GetDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    user32.PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT | 1)
    buf = ctypes.create_string_buffer(w * h * 4)
    bi = (ctypes.c_uint32 * 10)(40, w, (-h) & 0xFFFFFFFF, 1 | 32 << 16, 0, 0, 0, 0, 0, 0)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, bi, 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)
    raw = buf.raw
    rows = [[(raw[(y * w + x) * 4 + 2], raw[(y * w + x) * 4 + 1], raw[(y * w + x) * 4])
             for x in range(w)] for y in range(h)]
    return w, h, rows


def main():
    out = os.path.abspath(sys.argv[1])
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 3
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    try:
        time.sleep(seconds)
        hwnd = find_window(proc.pid)
        if len(sys.argv) > 3:
            post_keys(hwnd, sys.argv[3])
            time.sleep(1)
        w, h, rows = grab(hwnd)
        images.write_png(out, w, h, rows)
        print(out, w, h)
    finally:
        close(proc)


if __name__ == '__main__':
    main()
