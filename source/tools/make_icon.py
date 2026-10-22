#!/usr/bin/env python3
"""Draws the program's icon (assets/icon/roll_away.ico): the striped ball
on a sandstone block, in the launcher's colours.  Each size is drawn on
its own (supersampled), the small ones as the ball alone so that it stays
readable.  Our own picture, made by this script (nothing from the game).

    python tools/make_icon.py [preview.png]
"""
import math
import os
import struct
import sys
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets', 'icon',
                   'roll_away.ico')
SIZES = [256, 128, 64, 48, 32, 24, 16]
BALL_ONLY = 32                      # this size and smaller: the ball alone

STRIPES = [(226, 36, 34), (248, 248, 246), (36, 92, 212), (252, 206, 32), (248, 248, 246),
           (40, 176, 72)]
LIGHT = (-0.45, 0.65, 0.65)
POLE = (-0.10, 0.55, 0.83)          # where the stripes meet (towards the viewer, a little up)

TOP, LEFT_SIDE, RIGHT_SIDE = (238, 180, 106), (206, 136, 70), (164, 96, 46)
GROOVE, EDGE = (176, 112, 58), (112, 60, 26)


def norm(v):
    length = math.sqrt(sum(c * c for c in v))
    return tuple(c / length for c in v)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


L = norm(LIGHT)
H = norm((L[0], L[1], L[2] + 1))
P = norm(POLE)
E1 = norm(cross(P, (0, 0, 1)))
E2 = cross(P, E1)


def ball(dx, dy, outline):
    """the ball's colour at (dx, dy) of the unit disc (y down), or None"""
    r2 = dx * dx + dy * dy
    if r2 > 1:
        return None
    n = (dx, -dy, math.sqrt(1 - r2))
    k = dot(n, P)
    if abs(k) > 0.93:
        c = STRIPES[1]
    else:
        angle = math.atan2(dot(n, E2), dot(n, E1))
        c = STRIPES[int((angle + math.pi) / (2 * math.pi) * 6 + 0.3) % 6]
    shade = 0.46 + 0.62 * max(0.0, dot(n, L))
    spec = max(0.0, dot(n, H)) ** 50 * 0.7
    if outline and r2 > 0.86:
        shade *= 0.55                   # (a dark rim: the small icons keep their shape)
    return tuple(min(255.0, ch * shade + 255 * spec) for ch in c)


def block(u, v):
    """the sandstone block (an isometric cube) at (u, v), or None"""
    cx, cy, hw, hh, side = 0.5, 0.66, 0.45, 0.2, 0.13
    s, t = (u - cx) / hw, (v - cy) / hh
    if abs(s) + abs(t) <= 1:
        a, b = t + s, t - s             # the top face as a square, -1..1
        m = max(abs(a), abs(b))
        if m > 0.965:
            return EDGE
        if 0.70 < m < 0.76 or (abs(a) < 0.5 and abs(b) < 0.5 and 0.40 < m < 0.44):
            return GROOVE               # carved frame and cartouche
        light = 1.04 - 0.10 * (t + 1) / 2
        return tuple(c * light for c in TOP)
    if abs(s) <= 1 and t > 0:
        # a side: below the near edges of the top face, `side` deep
        top_edge = cy + hh * (1 - abs(s))
        if top_edge <= v <= top_edge + side:
            depth = (v - top_edge) / side
            if depth > 0.93 or abs(s) > 0.985 or abs(s) < 0.006:
                return EDGE
            c = LEFT_SIDE if s < 0 else RIGHT_SIDE
            if 0.45 < depth < 0.53:
                c = tuple(x * 0.86 for x in c)
            return tuple(x * (1.0 - 0.18 * depth) for x in c)
    return None


def scene(u, v, small):
    if small:
        c = ball((u - 0.5) / 0.47, (v - 0.5) / 0.47, True)
        return (c, 1.0) if c else None
    c = ball((u - 0.5) / 0.29, (v - 0.40) / 0.29, False)
    if c:
        return (c, 1.0)
    c = block(u, v)
    if c is None:
        return None
    sx, sy = (u - 0.53) / 0.24, (v - 0.68) / 0.075       # the ball's shadow
    d = sx * sx + sy * sy
    if d < 1:
        c = tuple(x * (0.55 + 0.45 * d) for x in c)
    return (c, 1.0)


def render(size):
    small = size <= BALL_ONLY
    ss = 6 if size <= 32 else 4 if size <= 64 else 3
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            acc, alpha = [0.0, 0.0, 0.0], 0.0
            for j in range(ss):
                for i in range(ss):
                    hit = scene((x + (i + 0.5) / ss) / size, (y + (j + 0.5) / ss) / size, small)
                    if hit:
                        for k in range(3):
                            acc[k] += hit[0][k]
                        alpha += 1
            n = ss * ss
            if alpha:
                row.append(tuple(int(acc[k] / alpha + 0.5) for k in range(3))
                           + (int(255 * alpha / n + 0.5),))
            else:
                row.append((0, 0, 0, 0))
        rows.append(row)
    return rows


def png(rows):
    size = len(rows)
    raw = b''.join(b'\0' + bytes(c for px in row for c in px) for row in rows)

    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data
                + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def bmp(rows):
    """an icon image as a 32-bit DIB (BGRA, bottom up) with an empty AND mask"""
    size = len(rows)
    header = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = b''.join(bytes((px[2], px[1], px[0], px[3])) for row in reversed(rows) for px in row)
    mask = b'\0' * (((size + 31) // 32) * 4 * size)
    return header + pixels + mask


def main():
    images = []
    for size in SIZES:
        rows = render(size)
        images.append((size, png(rows) if size >= 256 else bmp(rows), rows))
    directory = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    data = b''
    for size, blob, _ in images:
        directory += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
        data += blob
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, 'wb').write(directory + data)
    print(os.path.normpath(OUT), ', '.join(str(s) for s in SIZES))
    if len(sys.argv) > 1:
        # (a preview: every size side by side on a grey and a dark ground)
        width = sum(SIZES) + 10 * len(SIZES) + 10
        sheet = [[(128, 128, 128, 255)] * width for _ in range(2 * 256 + 30)]
        for half, ground in ((0, (200, 200, 200)), (1, (32, 32, 40))):
            for y in range(half * 266 + 10, half * 266 + 266):
                sheet[y] = [ground + (255,)] * width
            x0 = 10
            for size, _, rows in images:
                for y, row in enumerate(rows):
                    for x, px in enumerate(row):
                        a = px[3] / 255
                        sheet[half * 266 + 10 + y][x0 + x] = tuple(
                            int(px[k] * a + ground[k] * (1 - a)) for k in range(3)) + (255,)
                x0 += size + 10
        open(sys.argv[1], 'wb').write(png_rect(sheet))


def png_rect(rows):
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\0' + bytes(c for px in row for c in px) for row in rows)

    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data
                + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    main()
