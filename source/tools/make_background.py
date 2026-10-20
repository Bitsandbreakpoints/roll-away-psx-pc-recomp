#!/usr/bin/env python3
"""Draws the launcher's background (assets/launcher/background.png): a
striped ball rolling on a stone track that floats above desert dunes
under a blue sky, in the port's own style.  Our own picture, made by
this script (no photo, nothing from the game).

    python tools/make_background.py
"""
import math
import os
import random
import struct
import zlib

W, H = 1600, 960
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets', 'launcher',
                   'background.png')

HORIZON = 600
SKY_TOP, SKY_LOW = (18, 52, 128), (150, 206, 240)
SAND_FAR, SAND_NEAR = (236, 170, 92), (168, 86, 38)

# the track: a straight stone path towards a vanishing point above the
# horizon, ending a little before it (it floats)
VANISH = (1110, 430)
TRACK_NEAR_Y, TRACK_FAR_Y = H, 560
TRACK_NEAR = (700, 1560)            # left and right x at the near end
STONE_A, STONE_B = (214, 160, 102), (188, 132, 80)

# the ball
BALL = (1150, 742, 112)             # centre x, y, radius
STRIPES = [(222, 44, 44), (246, 244, 236), (40, 92, 222), (246, 244, 236), (40, 172, 74),
           (246, 244, 236)]
LIGHT = (-0.45, -0.6, 0.66)


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def norm(v):
    n = math.sqrt(sum(c * c for c in v))
    return tuple(c / n for c in v)


def clouds():
    rnd = random.Random(724)
    blobs = []
    for _ in range(9):
        cx, cy = rnd.uniform(300, 1650), rnd.uniform(60, 420)
        for _ in range(7):
            blobs.append((cx + rnd.uniform(-160, 160), cy + rnd.uniform(-25, 25),
                          rnd.uniform(40, 95), rnd.uniform(0.25, 0.5)))
    return blobs


def sky(x, y, blobs):
    c = mix(SKY_TOP, SKY_LOW, (y / HORIZON) ** 1.4)
    cover = 0.0
    for bx, by, r, a in blobs:
        d2 = ((x - bx) ** 2 + ((y - by) * 2.2) ** 2) / (r * r)
        if d2 < 4:
            cover += a * math.exp(-d2)
    return mix(c, (250, 252, 255), min(cover, 0.9))


def ground(x, y):
    depth = (y - HORIZON) / (H - HORIZON)
    c = mix(SAND_FAR, SAND_NEAR, depth ** 0.8)
    dune = math.sin(x * 0.006 + 3 * math.sin(y * 0.011)) * math.sin(y * 0.045 / (0.3 + depth))
    c = mix(c, (255, 214, 150), max(0.0, dune) * 0.25)
    c = mix(c, (110, 50, 20), max(0.0, -dune) * 0.18)
    haze = math.exp(-(y - HORIZON) / 26.0)
    return mix(c, SKY_LOW, haze * 0.8)


def track_edges(y):
    """Left and right x of the track on line y, and its depth (1 near, larger far)."""
    if not TRACK_FAR_Y <= y <= TRACK_NEAR_Y:
        return None
    k = (y - VANISH[1]) / (TRACK_NEAR_Y - VANISH[1])     # straight edges to the vanishing point
    left = VANISH[0] + (TRACK_NEAR[0] - VANISH[0]) * k
    right = VANISH[0] + (TRACK_NEAR[1] - VANISH[0]) * k
    return left, right, 1 / k


def track(x, y):
    e = track_edges(y)
    if not e or not e[0] <= x <= e[1]:
        return None
    left, right, z = e
    u = (x - left) / (right - left)
    v = z * 2.2
    tile = (int(u * 4) + int(v)) & 1
    c = STONE_A if tile else STONE_B
    edge = min(u, 1 - u)
    if edge < 0.03 or (u * 4) % 1 < 0.02 or v % 1 < 0.02 * z:
        c = mix(c, (96, 58, 30), 0.6)   # grooves between the stones
    c = mix(c, (255, 226, 170), max(0.0, 0.3 - u) * 0.6)
    return mix(c, SKY_LOW, min(1.0, (z - 1) / 5) * 0.5)


def ball(x, y):
    bx, by, r = BALL
    dx, dy = (x - bx) / r, (y - by) / r
    d2 = dx * dx + dy * dy
    if d2 > 1:
        return None, 0
    nz = math.sqrt(1 - d2)
    n = (dx, dy, nz)
    # the ball's axis, tilted: stripes run from pole to pole
    ax = norm((0.55, -0.75, 0.35))
    lon = math.atan2(n[0] * ax[2] - n[2] * ax[0],
                     n[1] - ax[1] * (n[0] * ax[0] + n[1] * ax[1] + n[2] * ax[2]))
    lat = n[0] * ax[0] + n[1] * ax[1] + n[2] * ax[2]
    if abs(lat) > 0.93:
        c = (250, 214, 64)              # the caps
    else:
        c = STRIPES[int((lon + math.pi) / (2 * math.pi) * len(STRIPES) + 0.3) % len(STRIPES)]
    lv = norm(LIGHT)
    diffuse = max(0.0, n[0] * lv[0] + n[1] * lv[1] + n[2] * lv[2])
    h = norm((lv[0], lv[1], lv[2] + 1))
    spec = max(0.0, n[0] * h[0] + n[1] * h[1] + n[2] * h[2]) ** 40
    c = tuple(ch * (0.28 + 0.8 * diffuse) for ch in c)
    c = mix(c, (255, 255, 255), spec * 0.9)
    rim = smooth_edge(math.sqrt(d2))
    return c, rim


def smooth_edge(d):
    return max(0.0, min(1.0, (1 - d) * 60))


def shadow(x, y):
    bx, by, r = BALL
    d = ((x - bx - 26) / (r * 1.05)) ** 2 + ((y - by - r * 0.95) / (r * 0.22)) ** 2
    return 0.55 * math.exp(-d * 2) if d < 3 else 0.0


def main():
    blobs = clouds()
    rows = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            c = sky(x, y, blobs) if y < HORIZON else ground(x, y)
            t = track(x, y)
            if t:
                c = mix(t, (40, 20, 10), shadow(x, y))
            b, a = ball(x, y)
            if b:
                c = mix(c, b, a)
            row += bytes(max(0, min(255, int(v))) for v in c)
        rows.append(bytes(row))
    raw = b''.join(b'\0' + r for r in rows)

    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data
                + struct.pack('>I', zlib.crc32(kind + data)))
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, 'wb').write(png)
    print(OUT)


if __name__ == '__main__':
    main()
