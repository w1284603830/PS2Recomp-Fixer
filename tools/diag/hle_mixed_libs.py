"""SDK libraries that are only partly replaced by runtime HLE (RUNTIME_PATCHES row 113's class of bug): when some
functions of a library run as our C++ replacement and others as the game's own code, both sides touch the same
library state (packet builder structs, DMA tags, double-buffer settings ...) and must keep its conventions exactly.

Usage: python -I hle_mixed_libs.py <recomp toml with [general].stubs> <analyzer.toml (all identified SDK functions)>
Prints per library prefix: replaced / original functions, mixed libraries first.
"""
import re
import sys

sys.stdout.reconfigure(encoding='utf-8')


def names(path, section_key=None):
    text = open(path, encoding='utf-8', errors='replace').read()
    if section_key:
        m = re.search(section_key + r'\s*=\s*\[(.*?)\]', text, re.S)
        text = m.group(1) if m else ''
    return {(n, int(a, 16)) for n, a in re.findall(r'"([A-Za-z_][A-Za-z0-9_]*)@0x([0-9A-Fa-f]+)"', text)}


PREFIXES = ['sceVif1Pk', 'sceVif0Pk', 'sceGifPk', 'sceGsSetDef', 'sceGs', 'sceDma', 'sceSifCmd', 'sceSifRpc', 'sceSif',
            'sceCd', 'scePad', 'sceMc', 'sceIpu', 'sceMpeg', 'sceVu0', 'sceVpu', 'sceDbc', 'sceDev', 'sceTty', 'sceDeci2',
            'sceSd', 'sceFs', 'sceOpen', 'sceRead', 'sceClose', 'sceLseek', 'scePrintf', 'sce', '']


def lib_of(name):
    for p in PREFIXES:
        if name.startswith(p):
            return p or '(other)'
    return '(other)'


def main():
    replaced = names(sys.argv[1], 'stubs')
    known = names(sys.argv[2])
    libs = {}
    for n, a in known | replaced:
        libs.setdefault(lib_of(n), {'replaced': set(), 'original': set()})
        (libs[lib_of(n)]['replaced' if (n, a) in replaced or any(r[1] == a for r in replaced) else 'original']).add((a, n))
    rows = sorted(libs.items(), key=lambda kv: (not (kv[1]['replaced'] and kv[1]['original']), kv[0]))
    for lib, s in rows:
        mixed = s['replaced'] and s['original']
        print(f'{lib:12s} replaced {len(s["replaced"]):3d}  original {len(s["original"]):3d}{"  MIXED" if mixed else ""}')
        if mixed:
            print('    replaced: ' + ', '.join(f'{n}@{a:06X}' for a, n in sorted(s['replaced'])))
            print('    original: ' + ', '.join(f'{n}@{a:06X}' for a, n in sorted(s['original'])))


if __name__ == '__main__':
    main()
