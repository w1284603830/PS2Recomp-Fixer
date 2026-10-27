"""Tile 24-bit BMP/PNG-less GS snapshots into one PNG contact sheet (nearest-neighbour thumbnails, no PIL).
Usage: contact_sheet.py out.png thumb_width columns in1.bmp [in2.bmp ...]"""
import struct
import sys
import zlib


def read_bmp(path):
    b = open(path, 'rb').read()
    off = struct.unpack_from('<I', b, 10)[0]
    w, h = struct.unpack_from('<ii', b, 18)
    bpp = struct.unpack_from('<H', b, 28)[0]
    if bpp not in (24, 32):
        raise SystemExit(f'{path}: {bpp} bpp')
    return b, off, w, h, bpp // 8


def thumb(path, tw):
    b, off, w, h, px = read_bmp(path)
    ah = abs(h)
    th = max(1, ah * tw // w)
    stride = (w * px + 3) & ~3
    rows = []
    for ty in range(th):
        y = ty * ah // th
        sy = ah - 1 - y if h > 0 else y
        base = off + sy * stride
        row = bytearray(tw * 3)
        for tx in range(tw):
            x = tx * w // tw
            p = base + x * px
            row[tx * 3] = b[p + 2]
            row[tx * 3 + 1] = b[p + 1]
            row[tx * 3 + 2] = b[p]
        rows.append(bytes(row))
    return rows


def write_png(path, w, h, rows):
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b''.join(b'\x00' + r for r in rows)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    data += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(data)


def main():
    out, tw, cols = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    thumbs = [thumb(p, tw) for p in sys.argv[4:]]
    th = max(len(t) for t in thumbs)
    gap = 4
    rows_n = (len(thumbs) + cols - 1) // cols
    W = cols * tw + (cols - 1) * gap
    H = rows_n * th + (rows_n - 1) * gap
    canvas = [bytearray(b'\x40' * (W * 3)) for _ in range(H)]
    for i, t in enumerate(thumbs):
        cx = (i % cols) * (tw + gap)
        cy = (i // cols) * (th + gap)
        for y, r in enumerate(t):
            canvas[cy + y][cx * 3:cx * 3 + len(r)] = r
    write_png(out, W, H, [bytes(r) for r in canvas])


main()
