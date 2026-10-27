"""Convert P6 PPM files to PNG (optionally brightened, so faint pixels show). Usage: ppm2png.py out_dir boost in1.ppm [in2.ppm ...]"""
import os
import struct
import sys
import zlib


def read_ppm(path):
    data = open(path, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        c = struct.pack('>I', len(payload)) + tag + payload
        return c + struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


out_dir, boost = sys.argv[1], int(sys.argv[2])
for p in sys.argv[3:]:
    w, h, rgb = read_ppm(p)
    if boost > 1:
        rgb = bytes(min(255, b * boost) for b in rgb)
    out = os.path.join(out_dir, os.path.splitext(os.path.basename(p))[0] + '.png')
    write_png(out, w, h, rgb)
    print(out)
