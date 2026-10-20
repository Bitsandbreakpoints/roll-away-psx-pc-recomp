#!/usr/bin/env python3
"""Runs the port with scripted controller input and makes a montage of
the pictures at the given frames (no global keyboard, no focus needed).

    python tests/play.py <name> <last frame> "<RA_INPUT script>" [frame ...]

e.g.  python tests/play.py menu 1500 "700:start,800:cross" 650 750 900
The pictures (native resolution) and montage.png land in build/test/<name>/.
The disc comes from build/settings.ini (choose it once in the launcher).
"""
import os
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, 'build', 'roll_away.exe')


def read_bmp(path):
    d = open(path, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0] // 8
    top = h < 0
    h = abs(h)
    stride = (w * bpp + 3) & ~3
    pixels = []
    for y in range(h):
        sy = y if top else h - 1 - y
        row = d[off + sy * stride: off + sy * stride + w * bpp]
        pixels.append([(row[bpp * x + 2], row[bpp * x + 1], row[bpp * x]) for x in range(w)])
    return w, h, pixels


def write_png(path, w, h, rows):
    raw = b''.join(b'\0' + bytes(c for p in row for c in p) for row in rows)

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def montage(images, columns, path):
    cell_w, cell_h = 320, 240
    rows_n = (len(images) + columns - 1) // columns
    canvas = [[(20, 20, 20)] * (cell_w * columns) for _ in range(cell_h * rows_n)]
    for k, (w, h, pixels) in enumerate(images):
        ox, oy = (k % columns) * cell_w, (k // columns) * cell_h
        for y in range(cell_h):
            sy = y * h // cell_h
            for x in range(cell_w):
                canvas[oy + y][ox + x] = pixels[sy][x * w // cell_w]
    write_png(path, cell_w * columns, cell_h * rows_n, canvas)


def main():
    name, last, script = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    frames = [int(f) for f in sys.argv[4:]]
    out = os.path.join(ROOT, 'build', 'test', name)
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        os.remove(os.path.join(out, f))
    env = dict(os.environ, RA_AUTOSTART='1', RA_QUIT='1', RA_LEAVE=str(last), RA_NATIVE='1', RA_FAST='1',
               RA_MUTE='1', RA_INPUT=script, RA_VRAM_DUMP=','.join(map(str, frames)))
    subprocess.run([EXE], cwd=out, env=env, timeout=last / 10 + 60)
    images = []
    for f in frames:
        shot = os.path.join(out, f'shot_{f}.bmp')
        if os.path.exists(shot):
            images.append(read_bmp(shot))
    if images:
        montage(images, 4, os.path.join(out, 'montage.png'))
    log = os.path.join(ROOT, 'build', 'roll_away.log')
    print(open(log, encoding='utf-8', errors='replace').read()[-1500:])
    print(f'{len(images)} pictures -> {os.path.join(out, "montage.png")}')


if __name__ == '__main__':
    main()
