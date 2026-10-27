"""Convert 24-bit BMP files (PCSX2 GS snapshots) to PNG. Usage: bmp2png.py out_dir in1.bmp [in2.bmp ...]"""
import os
import struct
import sys
import zlib


def png(path, w, h, rows):
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b''.join(b'\x00' + r for r in rows)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    data += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(data)


out = sys.argv[1]
os.makedirs(out, exist_ok=True)
for name in sys.argv[2:]:
    b = open(name, 'rb').read()
    off, w, h, bpp = struct.unpack_from('<I', b, 10)[0], struct.unpack_from('<i', b, 18)[0], struct.unpack_from('<i', b, 22)[0], struct.unpack_from('<H', b, 28)[0]
    assert bpp == 24, name
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(abs(h)):
        sy = abs(h) - 1 - y if h > 0 else y
        r = b[off + sy * stride: off + sy * stride + w * 3]
        rows.append(bytes(r[i ^ 0 if False else i] for i in range(0)) or bytes(sum(([r[x * 3 + 2], r[x * 3 + 1], r[x * 3]] for x in range(w)), [])))
    dst = os.path.join(out, os.path.splitext(os.path.basename(name))[0] + '.png')
    png(dst, w, abs(h), rows)
    print(dst, w, abs(h))
