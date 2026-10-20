"""Shared helpers for the PC port's tests (Windows only).

The tests never use the global keyboard or move the focus: keys go to the
test window with PostMessage, the port is driven by RA_INPUT and pictures
come from RA_VRAM_DUMP (port) or the emulator's own screenshots, so the PC
stays usable while they run.
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
ROOT = PORT
EXE = os.path.join(PORT, 'build', 'roll_away.exe')
LOG = os.path.join(PORT, 'build', 'roll_away.log')
PS = os.path.join(HERE, 'ps')

user32 = ctypes.windll.user32
WM_CLOSE = 0x0010
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
KEYS = {'enter': 0x0D, 'esc': 0x1B, 'up': 0x26, 'down': 0x28, 'left': 0x25, 'right': 0x27,
        'x': 0x58, 'c': 0x43, 'z': 0x5A, 's': 0x53, 'back': 0x08, 'f1': 0x70, 'f10': 0x79}
ENUM_PROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def _area(hwnd):
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return (r.right - r.left) * (r.bottom - r.top)


def find_window(pid, child=False):
    """The largest visible top-level window of `pid` (or its largest child)."""
    best = [None, -1]

    def visit(hwnd, _):
        if user32.IsWindowVisible(hwnd) and _area(hwnd) >= best[1]:
            best[0], best[1] = hwnd, _area(hwnd)
        return True

    def top(hwnd, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            if child:
                user32.EnumChildWindows(hwnd, ENUM_PROC(visit), 0)
            else:
                visit(hwnd, 0)
        return True

    user32.EnumWindows(ENUM_PROC(top), 0)
    return best[0]


def post_keys(hwnd, keys):
    """'enter wait500 x': key messages to one window (held ~120 ms each)."""
    for key in keys.split():
        if key.startswith('wait'):
            time.sleep(int(key[4:]) / 1000)
            continue
        code = KEYS[key]
        scan = user32.MapVirtualKeyW(code, 0)
        ext = 0x01000000 if 0x25 <= code <= 0x28 else 0
        user32.PostMessageW(hwnd, WM_KEYDOWN, code, 1 | scan << 16 | ext)
        time.sleep(0.12)
        user32.PostMessageW(hwnd, WM_KEYUP, code, 0xC0000001 | scan << 16 | ext)
        time.sleep(0.25)


def close(proc, timeout=5):
    """Closes the program's window (so files are finished), else kills it."""
    hwnd = find_window(proc.pid)
    if hwnd:
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    try:
        proc.wait(timeout)
    except subprocess.TimeoutExpired:
        proc.kill()


def powershell(script, *args):
    subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                    os.path.join(PS, script), *args], check=False)


def run_port(out_dir, script, frames, seconds, native=False):
    """Runs the port's game (disc from settings.ini) with RA_INPUT=`script`,
    pictures at `frames` (shot_<frame>.bmp in out_dir), silent; returns
    the log's summary lines."""
    os.makedirs(out_dir, exist_ok=True)
    env = dict(os.environ, RA_AUTOSTART='1', RA_MUTE='1', RA_INPUT=script,
               RA_VRAM_DUMP=','.join(str(f) for f in frames))
    if native:
        env['RA_NATIVE'] = '1'
    proc = subprocess.Popen([EXE], cwd=out_dir, env=env)
    time.sleep(seconds)
    close(proc)
    with open(LOG, encoding='utf-8', errors='replace') as f:
        return [line.strip() for line in f if 'CRASH' in line or 'frames in' in line]


def montage(out_dir, frames, out):
    powershell('montage.ps1', '-dir', out_dir, '-frames', ','.join(str(f) for f in frames),
               '-out', out)
