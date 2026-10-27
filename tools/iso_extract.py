"""Read-only ISO9660 extractor: copy selected files out of a PS2 disc image.

Usage: python -I iso_extract.py <iso> <dest_dir> <max_bytes> [path_prefix ...]
Files are written under dest_dir with their ISO paths (";1" version suffix removed).
Only files no larger than max_bytes are copied; with no prefixes, every such file is copied.
"""
import os
import struct
import sys

SECTOR = 2048


def walk(f, lba, size, prefix, out):
    f.seek(lba * SECTOR)
    data = f.read(size)
    pos = 0
    while pos < len(data):
        rec_len = data[pos]
        if rec_len == 0:
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        rec = data[pos:pos + rec_len]
        pos += rec_len
        ext_lba = struct.unpack_from("<I", rec, 2)[0]
        ext_size = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        name = rec[33:33 + rec[32]]
        if name in (b"\x00", b"\x01"):
            continue
        name = name.decode("ascii").split(";")[0]
        path = prefix + name
        if flags & 0x02:
            walk(f, ext_lba, ext_size, path + "/", out)
        else:
            out.append((path, ext_lba, ext_size))


def main():
    iso, dest, max_bytes = sys.argv[1], sys.argv[2], int(sys.argv[3])
    prefixes = [p.upper() for p in sys.argv[4:]]
    with open(iso, "rb") as f:
        f.seek(16 * SECTOR)
        pvd = f.read(SECTOR)
        if pvd[1:6] != b"CD001":
            raise SystemExit("not an ISO9660 image")
        root = pvd[156:156 + 34]
        files = []
        walk(f, struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "/", files)
        for path, lba, size in files:
            if size > max_bytes:
                continue
            if prefixes and not any(path.upper().startswith(p) for p in prefixes):
                continue
            target = os.path.join(dest, *path.strip("/").split("/"))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            f.seek(lba * SECTOR)
            with open(target, "wb") as out:
                out.write(f.read(size))
            print(f"{path}\tlba={lba}\tsize={size}")


if __name__ == "__main__":
    main()
