"""Name the module+offset lines of a DOD_PROF report with the linker maps.
Usage: prof_symbolize.py err.log dod_map pcsx2gs_map [max_lines_per_section]"""
import bisect
import re
import sys


def load_map(path):
    lines = open(path, encoding='latin-1').read().splitlines()
    pref = 0
    for line in lines:
        m = re.search(r'Preferred load address is ([0-9a-fA-F]+)', line)
        if m:
            pref = int(m.group(1), 16)
    syms = []
    for line in lines:
        m = re.match(r'\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s', line)
        if m:
            syms.append((int(m.group(2), 16) - pref, m.group(1)))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def name(maps, module, off):
    if module not in maps:
        return f'{module}+{off:x}'
    keys, names = maps[module]
    i = bisect.bisect_right(keys, off) - 1
    if i < 0:
        return f'{module}+{off:x}'
    n = names[i]
    m = re.match(r'\?(\w+)@', n)
    return (m.group(1) if m else n)[:90] + f'+{off - keys[i]:x}'


def main():
    log, dod_map, gs_map = sys.argv[1:4]
    maps = {'dod.exe': load_map(dod_map), 'pcsx2gs.dll': load_map(gs_map)}
    for line in open(log, encoding='utf-8', errors='replace'):
        if not line.startswith('[prof]'):
            continue
        m = re.search(r'(\S+\.(?:exe|dll))\+([0-9a-f]+)', line)
        if m:
            line = line[:m.start()] + name(maps, m.group(1), int(m.group(2), 16)) + line[m.end():]
        print(line.rstrip())


main()
