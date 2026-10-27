"""Hex dump of records of a GS capture (DOD_GS_CAPTURE): path, size and the first qwords of each record in a range.
Usage: python -I gif_record.py capture.bin <first record> [last record] [qwords]"""
import struct
import sys


def main():
    path = sys.argv[1]
    first = int(sys.argv[2])
    last = int(sys.argv[3]) if len(sys.argv) > 3 else first
    qwords = int(sys.argv[4]) if len(sys.argv) > 4 else 16
    data = open(path, 'rb').read()
    pos = 0
    field = None
    record = 0
    while pos + 8 <= len(data):
        p, n = struct.unpack_from('<II', data, pos)
        pos += 8
        if p == 0xFFFFFFFF:
            field = n
            continue
        record += 1
        if first <= record <= last:
            print(f'rec{record} field {field} path{p} bytes {n} ({n // 16} qwords)')
            for q in range(min(qwords, n // 16)):
                lo, hi = struct.unpack_from('<QQ', data, pos + q * 16)
                w = struct.unpack_from('<4I', data, pos + q * 16)
                print(f'  q{q:4d} {w[3]:08X} {w[2]:08X} {w[1]:08X} {w[0]:08X}')
        pos += n
        if record > last:
            break


if __name__ == '__main__':
    main()
