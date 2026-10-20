"""Small image helpers for the tests (no extra packages): read BMP/PNG,
write PNG, montages.  Pictures are (width, height, rows of (r, g, b))."""
import struct
import sys
import zlib


def read_bmp(path):
    d = open(path, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0] // 8
    top = h < 0
    h = abs(h)
    stride = (w * bpp + 3) & ~3
    rows = []
    for y in range(h):
        sy = y if top else h - 1 - y
        r = d[off + sy * stride: off + sy * stride + w * bpp]
        rows.append([(r[bpp * x + 2], r[bpp * x + 1], r[bpp * x]) for x in range(w)])
    return w, h, rows


def read_png(path):
    d = open(path, 'rb').read()
    pos, idat, w = 8, b'', 0
    while pos < len(d):
        n, kind = struct.unpack_from('>I4s', d, pos)
        data = d[pos + 8: pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, colour = struct.unpack_from('>IIBB', data)
        elif kind == b'IDAT':
            idat += data
        pos += 12 + n
    channels = {2: 3, 6: 4}[colour]
    raw = zlib.decompress(idat)
    stride = w * channels
    prev = bytearray(stride)
    rows, i = [], 0
    for _ in range(h):
        f = raw[i]
        line = bytearray(raw[i + 1: i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * channels: x * channels + 3]) for x in range(w)])
        prev = line
    return w, h, rows


def read(path):
    return read_png(path) if path.lower().endswith('.png') else read_bmp(path)


def write_png(path, w, h, rows):
    raw = b''.join(b'\0' + bytes(c for p in row for c in p) for row in rows)

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def montage(images, columns, path, cell=(320, 240)):
    cw, ch = cell
    n_rows = (len(images) + columns - 1) // columns
    canvas = [[(20, 20, 20)] * (cw * columns) for _ in range(ch * n_rows)]
    for k, (w, h, rows) in enumerate(images):
        ox, oy = (k % columns) * cw, (k // columns) * ch
        for y in range(ch):
            src = rows[y * h // ch]
            line = canvas[oy + y]
            for x in range(cw):
                line[ox + x] = src[x * w // cw]
    write_png(path, cw * columns, ch * n_rows, canvas)


if __name__ == '__main__':
    # python tests/images.py <out.png> <columns> <picture> ...
    montage([read(p) for p in sys.argv[3:]], int(sys.argv[2]), sys.argv[1])
