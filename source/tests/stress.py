#!/usr/bin/env python3
"""Plays the port with random controller input for a long time and
reports whether it stopped with an error (the log's FATAL lines).

    python tests/stress.py [frames] [seed]
"""
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, 'build', 'roll_away.exe')
BUTTONS = ['up', 'up', 'up', 'down', 'left', 'right', 'cross', 'cross', 'circle', 'start',
           'triangle', 'square']


def main():
    frames = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
    rnd = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
    steps, f = [], 1150
    while f < frames:
        hold = rnd.choice([1, 1, 20, 60, 180])
        steps.append(f'{f}-{f + hold}:{rnd.choice(BUTTONS)}' if hold > 1 else f'{f}:{rnd.choice(BUTTONS)}')
        f += hold + rnd.randint(5, 40)
    out = os.path.join(ROOT, 'build', 'test', 'stress')
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ, RA_AUTOSTART='1', RA_QUIT='1', RA_LEAVE=str(frames), RA_FAST='1',
               RA_MUTE='1', RA_INPUT=','.join(steps))
    subprocess.run([EXE], cwd=out, env=env, timeout=frames / 5 + 120)
    log = open(os.path.join(ROOT, 'build', 'roll_away.log'), encoding='utf-8', errors='replace').read()
    fatal = [l for l in log.splitlines() if 'FATAL' in l or 'unmapped' in l]
    print('\n'.join(log.splitlines()[-3:]))
    print('problems:', len(fatal))
    print('\n'.join(fatal[:10]))
    return 1 if fatal else 0


if __name__ == '__main__':
    sys.exit(main())
