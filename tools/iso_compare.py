"""Read-only comparison of PS2 disc images (ISO9660), for the merged-disc plan (docs/PLAN_MERGED_DISC.md).

Usage:
  python -I iso_compare.py info <iso>
      volume size vs image size, second-layer volume descriptors (DVD9), SYSTEM.CNF, file count, highest file end
  python -I iso_compare.py list <iso>
      every file: path, LBA, size
  python -I iso_compare.py diff <isoX> <isoY> [--hash] [--all]
      files only in X / only in Y / with another size / (with --hash) with other contents, by path
  python -I iso_compare.py origin <merged> <discA> <discB> [--hash] [--all]
      for each file of the merged image: does disc A or B have the same file (same size, and same SHA-1 with --hash),
      under the same path or another one; per top-level directory totals tell a boot-menu merge (two trees) from a
      one-tree merge with a patched ELF
Nothing is written: images are only opened for reading.
"""
import hashlib
import os
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from iso_extract import SECTOR, walk  # noqa: E402

SHOW = 25  # examples printed per group without --all


def open_iso(path):
    f = open(path, 'rb')
    f.seek(16 * SECTOR)
    pvd = f.read(SECTOR)
    if pvd[1:6] != b'CD001':
        raise SystemExit(f'{path}: not an ISO9660 image')
    return f, pvd


class _Shifted:
    """File view whose sector 0 is `base` (a DVD9 layer 1 tree: its LBAs count from the layer start)."""

    def __init__(self, f, base):
        self.f, self.base = f, base

    def seek(self, pos):
        self.f.seek(pos + self.base * SECTOR)

    def read(self, n):
        return self.f.read(n)


def layer1_start(f, pvd, path):
    """Sector where a PS2 DVD9's layer 1 starts when it has its own volume descriptor (16 sectors in), else None."""
    vol = struct.unpack_from('<I', pvd, 80)[0]
    blocks = os.path.getsize(path) // SECTOR
    for start in (vol - 16, vol):
        if 0 < start and start + 16 < blocks:
            f.seek((start + 16) * SECTOR)
            d = f.read(SECTOR)
            if d[0] == 1 and d[1:6] == b'CD001':
                return start
    return None


def files_of(path):
    """Every file of the image as (path, absolute LBA, size); a DVD9 layer 1 tree is included with its LBAs made
    absolute (layer start + LBA), as a PS2's sceCdLayerSearchFile reports them."""
    f, pvd = open_iso(path)
    root = pvd[156:156 + 34]
    out = []
    walk(f, struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], '/', out)
    l1 = layer1_start(f, pvd, path)
    if l1 is not None:
        f.seek((l1 + 16) * SECTOR)
        root1 = f.read(SECTOR)[156:156 + 34]
        more = []
        walk(_Shifted(f, l1), struct.unpack_from('<I', root1, 2)[0], struct.unpack_from('<I', root1, 10)[0], '/', more)
        out += [(p, lba + l1, s) for p, lba, s in more]
    return f, pvd, out


def sha1(f, lba, size):
    h = hashlib.sha1()
    f.seek(lba * SECTOR)
    left = size
    while left:
        chunk = f.read(min(left, 1 << 20))
        if not chunk:
            break
        h.update(chunk)
        left -= len(chunk)
    return h.hexdigest()


def info(path):
    f, pvd, files = files_of(path)
    vol_blocks = struct.unpack_from('<I', pvd, 80)[0]
    img_blocks = os.path.getsize(path) // SECTOR
    print(f'image      {path}')
    print(f'volume id  {pvd[40:72].decode("ascii", "replace").strip()}')
    print(f'size       {os.path.getsize(path)} bytes = {img_blocks} sectors; ISO9660 volume = {vol_blocks} sectors')
    # DVD9: a second layer stored after the first may start with its own volume descriptors (PTP, or OTP dumps that
    # keep layer 1's descriptors); report every "CD001" found at the usual places past layer 0.
    if img_blocks > vol_blocks:
        print(f'           image is {img_blocks - vol_blocks} sectors larger than the volume: possible second layer')
        for base in (vol_blocks, vol_blocks - 16):
            for at in (base + 16,):
                if 0 <= at < img_blocks:
                    f.seek(at * SECTOR)
                    d = f.read(SECTOR)
                    if d[1:6] == b'CD001':
                        print(f'           descriptor at sector {at}: type {d[0]}, volume {struct.unpack_from("<I", d, 80)[0]} sectors'
                              f' -> layer 1 would start at {at - 16}')
    end = max((lba + (size + SECTOR - 1) // SECTOR for _, lba, size in files), default=0)
    print(f'files      {len(files)}, total {sum(s for _, _, s in files)} bytes, highest file end sector {end}'
          f' ({end * SECTOR / 2**30:.2f} GiB)')
    over = [p for p, lba, s in files if (lba + (s + SECTOR - 1) // SECTOR) * SECTOR > 2**32]
    if over:
        print(f'           {len(over)} files end past 4 GiB (e.g. {over[0]})')
    tops = defaultdict(lambda: [0, 0])
    for p, _, s in files:
        top = p.split('/')[1] if p.count('/') > 1 else '(root)'
        tops[top][0] += 1
        tops[top][1] += s
    print('top level  ' + ', '.join(f'{k}: {n} files {b / 2**20:.0f} MiB' for k, (n, b) in sorted(tops.items())))
    cnf = [(lba, s) for p, lba, s in files if p.upper() == '/SYSTEM.CNF']
    if cnf:
        f.seek(cnf[0][0] * SECTOR)
        print('SYSTEM.CNF ' + f.read(cnf[0][1]).decode('ascii', 'replace').strip().replace('\r\n', ' | ').replace('\n', ' | '))
    else:
        print('SYSTEM.CNF (none in the root)')


def listing(path):
    _, _, files = files_of(path)
    for p, lba, s in files:
        print(f'{p}\t{lba}\t{s}')


def diff(px, py, use_hash, show_all):
    fx, _, ax = files_of(px)
    fy, _, ay = files_of(py)
    mx = {p.upper(): (p, lba, s) for p, lba, s in ax}
    my = {p.upper(): (p, lba, s) for p, lba, s in ay}
    only_x = sorted(set(mx) - set(my))
    only_y = sorted(set(my) - set(mx))
    both = sorted(set(mx) & set(my))
    size_diff = [k for k in both if mx[k][2] != my[k][2]]
    same_size = [k for k in both if mx[k][2] == my[k][2]]
    content_diff = []
    if use_hash:
        for k in same_size:
            if sha1(fx, mx[k][1], mx[k][2]) != sha1(fy, my[k][1], my[k][2]):
                content_diff.append(k)
    groups = [(f'only in {os.path.basename(px)}', [mx[k] for k in only_x]),
              (f'only in {os.path.basename(py)}', [my[k] for k in only_y]),
              ('size differs', [(mx[k][0], mx[k][2], my[k][2]) for k in size_diff])]
    if use_hash:
        groups.append(('same size, contents differ', [mx[k] for k in content_diff]))
    print(f'{len(both)} paths in both, {len(same_size)} of them with the same size'
          + (f', {len(same_size) - len(content_diff)} identical' if use_hash else ''))
    for title, items in groups:
        print(f'{title}: {len(items)}')
        for it in (items if show_all else items[:SHOW]):
            print('   ' + '\t'.join(str(v) for v in it))
        if not show_all and len(items) > SHOW:
            print(f'   ... {len(items) - SHOW} more (--all)')


def origin(pm, pa, pb, use_hash, show_all):
    fm, _, am = files_of(pm)
    sources = []
    index = defaultdict(list)  # size (and hash) -> [(disc, path)]
    by_path = {}
    for disc, p in (('A', pa), ('B', pb)):
        f, _, files = files_of(p)
        sources.append(f)
        for path, lba, size in files:
            key = (size, sha1(f, lba, size)) if use_hash else (size,)
            index[key].append((disc, path.upper()))
            by_path.setdefault(path.upper(), []).append((disc, key))
    totals = defaultdict(lambda: defaultdict(int))
    examples = defaultdict(list)
    for path, lba, size in am:
        key = (size, sha1(fm, lba, size)) if use_hash else (size,)
        same_path = {d for d, k in by_path.get(path.upper(), []) if k == key}
        elsewhere = {(d, q) for d, q in index.get(key, []) if q != path.upper()}
        if same_path:
            kind = 'same path on ' + '+'.join(sorted(same_path))
        elif elsewhere:
            kind = 'moved from ' + '+'.join(sorted({d for d, _ in elsewhere}))
            examples[kind].append(f'{path} <- {sorted(elsewhere)[0][0]}:{sorted(elsewhere)[0][1]}')
        else:
            kind = 'new or changed'
            examples[kind].append(f'{path}\t{size}')
        top = path.split('/')[1] if path.count('/') > 1 else '(root)'
        totals[top][kind] += 1
        totals['(all)'][kind] += 1
    note = 'size + SHA-1' if use_hash else 'size only (add --hash to compare contents)'
    print(f'matching by {note}')
    for top in sorted(totals):
        print(f'{top}: ' + ', '.join(f'{k} {n}' for k, n in sorted(totals[top].items())))
    for kind, items in examples.items():
        print(f'{kind}:')
        for it in (items if show_all else items[:SHOW]):
            print('   ' + it)
        if not show_all and len(items) > SHOW:
            print(f'   ... {len(items) - SHOW} more (--all)')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    use_hash = '--hash' in sys.argv
    show_all = '--all' in sys.argv
    if len(args) == 2 and args[0] == 'info':
        info(args[1])
    elif len(args) == 2 and args[0] == 'list':
        listing(args[1])
    elif len(args) == 3 and args[0] == 'diff':
        diff(args[1], args[2], use_hash, show_all)
    elif len(args) == 4 and args[0] == 'origin':
        origin(args[1], args[2], args[3], use_hash, show_all)
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
