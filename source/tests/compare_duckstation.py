#!/usr/bin/env python3
"""Compares the port with the original game in DuckStation: the same
screens in both, a difference picture and numbers per screen
(build/test/compare/montage.png).

    python tests/compare_duckstation.py

The emulator is driven by time (tests/emulator.py), the port by frame
numbers (RA_INPUT, RA_NATIVE=1: original resolution); the screens are
chosen where the timing matters least: the title menu (after the
Psygnosis movie) and the 1 Player menu (Cross).  The 3D background
moves, so only the menus are expected to match.
"""
import os
import subprocess
import sys

import emulator
from testlib import PORT, PS, run_port

OUT = os.path.join(PORT, 'build', 'test', 'compare')
# (screen, emulator time of its screenshot, port frame)
SCREENS = [('title', 35, 1150), ('1player', 39, 1300)]
EMU_TIMELINE = '35:shot|36:x|39:shot'
PORT_INPUT = '1200:cross'


def main():
    os.makedirs(OUT, exist_ok=True)
    emu_dir = emulator.run('compare', EMU_TIMELINE)
    for line in run_port(os.path.join(OUT, 'port'), PORT_INPUT, [f for _, _, f in SCREENS],
                         max(f for _, _, f in SCREENS) // 60 + 12, native=True):
        print(line)
    pairs = ';'.join(f'{name}|{os.path.join(emu_dir, f"{t}.png")}|'
                     f'{os.path.join(OUT, "port", f"shot_{frame}.bmp")}'
                     for name, t, frame in SCREENS)
    subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                    os.path.join(PS, 'compare.ps1'), '-pairs', pairs, '-out',
                    os.path.join(OUT, 'montage.png')], check=False)
    print(os.path.join(OUT, 'montage.png'))


if __name__ == '__main__':
    sys.exit(main())
