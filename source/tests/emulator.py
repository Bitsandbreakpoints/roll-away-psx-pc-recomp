#!/usr/bin/env python3
"""Runs the original game in DuckStation (with the BIOS in the project
folder) and takes screenshots at given times, pressing keys in between;
the reference for the port.  Nothing global: keys go to the emulator's
window with PostMessage.

    python tests/emulator.py <name> "<seconds>:<keys>|..." [disc image]

e.g.  python tests/emulator.py title "20:shot|21:enter|23:shot"
keys: enter (Start), x (Cross), c (Circle), z (Square), s (Triangle),
back (Select), up/down/left/right, waitN; "shot" takes a screenshot.
Pictures land in build/test/emu_<name>/<seconds>.png.

DuckStation is copied once from tools/emulator/duckstation to
build/test/duckstation with its own settings (software renderer, 1x, no
PGXP, keyboard pad, muted), so the emulator's own settings stay as they are.
"""
import glob
import os
import shutil
import subprocess
import sys
import time

from testlib import PORT, close, find_window, post_keys

EMU_SOURCE = os.path.join(PORT, 'tools', 'emulator', 'duckstation')
EMU_DIR = os.path.join(PORT, 'build', 'test', 'duckstation')
EMU_EXE = 'duckstation-qt-x64-ReleaseLTCG.exe'
DISC = os.path.join(PORT, "Roll Away (USA).chd")

EMU_SETTINGS = """[Main]
ConfirmPowerOff = false
PauseOnFocusLoss = false
SaveStateOnExit = false
SetupWizardIncomplete = false
StartFullscreen = false

[UI]
MainWindowWidth = 800
MainWindowHeight = 600

[AutoUpdater]
CheckAtStartup = false

[BIOS]
PatchFastBoot = true
PathNTSCJ = scph5502.bin
PathNTSCU = scph5502.bin
PathPAL = scph5502.bin
SearchDirectory = bios

[GPU]
Renderer = Software
ResolutionScale = 1
PGXPEnable = false

[Display]
ShowOSDMessages = false
ScreenshotMode = InternalResolution
ScreenshotFormat = PNG

[CDROM]
RegionCheck = false

[Audio]
Backend = Null
OutputMuted = true

[InternalPostProcessing]
Enabled = false

[PostProcessing]
Enabled = false

[Pad1]
Type = DigitalController
Up = Keyboard/Up
Down = Keyboard/Down
Left = Keyboard/Left
Right = Keyboard/Right
Cross = Keyboard/X
Circle = Keyboard/C
Square = Keyboard/Z
Triangle = Keyboard/S
Start = Keyboard/Return
Select = Keyboard/Backspace

[Hotkeys]
Screenshot = Keyboard/F10
"""


def setup():
    if not os.path.isdir(EMU_DIR):
        shutil.copytree(EMU_SOURCE, EMU_DIR)
    with open(os.path.join(EMU_DIR, 'settings.ini'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(EMU_SETTINGS)


def run(name, timeline, disc=DISC):
    out = os.path.join(PORT, 'build', 'test', 'emu_' + name)
    os.makedirs(out, exist_ok=True)
    setup()
    shots = os.path.join(EMU_DIR, 'screenshots')
    os.makedirs(shots, exist_ok=True)
    proc = subprocess.Popen([os.path.join(EMU_DIR, EMU_EXE), '-batch', '-nofullscreen', disc],
                            cwd=EMU_DIR)
    start = time.time()
    try:
        for step in timeline.split('|'):
            at, keys = step.split(':', 1)
            delay = start + float(at) - time.time()
            if delay > 0:
                time.sleep(delay)
            display = find_window(proc.pid, child=True)
            if keys == 'shot':
                before = set(glob.glob(os.path.join(shots, '**', '*.png'), recursive=True))
                post_keys(display, 'f10')
                time.sleep(1.0)
                new = set(glob.glob(os.path.join(shots, '**', '*.png'), recursive=True)) - before
                if new:
                    shutil.move(new.pop(), os.path.join(out, f'{at}.png'))
                else:
                    print(f'{at}: no screenshot')
            else:
                post_keys(display, keys)
    finally:
        close(proc)
    return out


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    print(run(sys.argv[1], sys.argv[2], *(sys.argv[3:4])))
