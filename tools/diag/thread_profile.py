"""Per-thread CPU and leaf-function profile from tools/scripts/diag/thread_sampler.ps1.

Usage: python -I thread_profile.py <dist/logs/run> [--exe-map work/diag/dodNNN.map] [--gs-map work/diag/pcsx2gs_*.map]
       [--top 15] [--thread GameThread] [--waits] [--timeline] [--marker REGEX ...]

Reads samples.csv (B/S/E/L lines) and modules.csv, resolves each sampled RIP to module + symbol with the MSVC .map
files, and prints every thread's CPU share over the sampling window and its hottest functions. Generated guest code
(FUN_*, entry_*, caseD_* ...) is also summed as one bucket per thread.
--waits: for --thread, the callers (scanned from its stack) of the samples that sit in ntdll, i.e. what it waits in.
--timeline: per second, --thread's busy share (samples outside ntdll) next to the err.log lines written in that
second that match --marker (default: the game's disc-cache misses and the fps lines).
"""
import argparse
import bisect
import collections
import re
import sys
from pathlib import Path

GUEST_PREFIXES = ('FUN_', 'entry_', 'caseD_', 'switchD_', 'LAB_', 'thunk_')


def demangle(name):
    if not name.startswith('?'):
        return name.lstrip('_')
    body = name[1:]
    if body.startswith('?'):
        return name  # operators / special names: keep raw
    head = body.split('@@', 1)[0]
    parts = [p for p in head.split('@') if p]
    return '::'.join(reversed(parts))


def load_map(path):
    text = Path(path).read_text(encoding='latin-1', errors='replace')
    base = int(re.search(r'Preferred load address is ([0-9a-fA-F]+)', text).group(1), 16)
    syms = []
    for m in re.finditer(r'^ [0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s', text, re.M):
        addr = int(m.group(2), 16)
        if addr >= base:
            syms.append((addr - base, m.group(1)))
    syms.sort()
    return [s[0] for s in syms], [s[1] for s in syms]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--exe-map', default='work/diag/dod284.map')
    ap.add_argument('--gs-map', default='work/diag/pcsx2gs_20261019i.map')
    ap.add_argument('--top', type=int, default=15)
    ap.add_argument('--thread', default='GameThread')
    ap.add_argument('--waits', action='store_true')
    ap.add_argument('--timeline', action='store_true')
    ap.add_argument('--marker', action='append', default=None)
    a = ap.parse_args()
    run = Path(a.run)

    modules = []
    for line in (run / 'modules.csv').read_text().splitlines():
        name, base, size = line.rsplit(',', 2)
        modules.append((int(base, 16), int(size), name))
    modules.sort()
    mod_bases = [m[0] for m in modules]
    maps = {'dod.exe': load_map(a.exe_map), 'pcsx2gs.dll': load_map(a.gs_map)}

    def resolve(rip):
        i = bisect.bisect_right(mod_bases, rip) - 1
        if i < 0 or rip >= modules[i][0] + modules[i][1]:
            return '?', '?'
        base, _, name = modules[i]
        m = maps.get(name.lower())
        if not m:
            return name, name
        rva = rip - base
        j = bisect.bisect_right(m[0], rva) - 1
        return name, (demangle(m[1][j]) if j >= 0 else name)

    begin, end, desc = {}, {}, {}
    samples = collections.defaultdict(list)  # tid -> [(ms, rip, [callers])]
    log_sizes = []                           # (ms, err.log size)
    for line in (run / 'samples.csv').read_text(encoding='utf-8', errors='replace').splitlines():
        f = line.split(',')
        if f[0] == 'S':
            if len(f) == 3:  # old format: S,tid,rip
                samples[int(f[1])].append((None, int(f[2], 16), []))
            else:
                callers = [int(x, 16) for x in f[4].split(';') if x] if len(f) > 4 else []
                samples[int(f[1])].append((float(f[2]), int(f[3], 16), callers))
        elif f[0] == 'L':
            log_sizes.append((int(f[1]), int(f[2])))
        elif f[0] in ('B', 'E'):
            g = line.split(',', 4)
            tid, ms, cpu = int(g[1]), int(g[2]), int(g[3])
            (begin if g[0] == 'B' else end)[tid] = (ms, cpu)
            if len(g) > 4 and g[4]:
                desc[tid] = g[4]

    rows = []
    for tid in end:
        if tid not in begin or begin[tid][1] < 0 or end[tid][1] < 0:
            continue
        wall = (end[tid][0] - begin[tid][0]) / 1000.0
        cpu = (end[tid][1] - begin[tid][1]) / 1e7
        rows.append((cpu / wall * 100 if wall > 0 else 0.0, tid, cpu, wall))
    rows.sort(reverse=True)
    total = sum(r[2] for r in rows)
    wall = max((r[3] for r in rows), default=0)
    print(f'window {wall:.1f} s, process CPU {total:.1f} s = {total / wall * 100 if wall else 0:.0f}% of one core, '
          f'{len(rows)} threads')
    for pct, tid, cpu, _ in rows:
        if pct < 2.0:
            continue
        s = samples.get(tid, [])
        funcs = collections.Counter()
        mods = collections.Counter()
        guest = 0
        for _, rip, _ in s:
            mod, fn = resolve(rip)
            mods[mod] += 1
            funcs[(mod, fn)] += 1
            if mod.lower() == 'dod.exe' and fn.startswith(GUEST_PREFIXES):
                guest += 1
        n = len(s) or 1
        label = desc.get(tid, '')
        print(f'\n== thread {tid} {label}: CPU {pct:.0f}% of a core ({cpu:.1f} s), {len(s)} samples, '
              f'guest code {guest / n * 100:.0f}%')
        print('   modules: ' + ', '.join(f'{m} {c / n * 100:.0f}%' for m, c in mods.most_common(6)))
        for (mod, fn), c in funcs.most_common(a.top):
            print(f'   {c / n * 100:5.1f}%  {mod}!{fn[:110]}')

    target = [tid for tid, d in desc.items() if d == a.thread]
    if not target or not (a.waits or a.timeline):
        return 0
    seq = samples[target[0]]

    def in_ntdll(rip):
        return resolve(rip)[0].lower() == 'ntdll.dll'

    if a.waits:
        waits = [x for x in seq if in_ntdll(x[1])]
        first = collections.Counter()
        chain = collections.Counter()
        for _, _, callers in waits:
            names = [resolve(c)[1] for c in callers[:3]]
            first[names[0] if names else '(no caller found)'] += 1
            chain[' <- '.join(n[:60] for n in names) or '(no caller found)'] += 1
        n = len(waits) or 1
        print(f'\n== {a.thread} waits: {len(waits)} of {len(seq)} samples in ntdll; first caller on the stack:')
        for name, c in first.most_common(a.top):
            print(f'   {c / n * 100:5.1f}%  {name[:120]}')
        print('   first three callers:')
        for name, c in chain.most_common(a.top):
            print(f'   {c / n * 100:5.1f}%  {name[:200]}')

    if a.timeline and seq and seq[0][0] is not None:
        markers = [re.compile(m) for m in (a.marker or [r'DVCI: File cache was not hit', r'^\[fps\]'])]
        text = (run / 'err.log').read_bytes()
        events = []  # (byte offset, line)
        pos = 0
        for raw in text.split(b'\n'):
            line = raw.decode('utf-8', errors='replace').rstrip('\r')
            if any(m.search(line) for m in markers):
                events.append((pos, line))
            pos += len(raw) + 1
        sizes_ms = [m for m, _ in log_sizes]
        sizes = [s for _, s in log_sizes]
        per_second = collections.defaultdict(list)
        for off, line in events:
            k = bisect.bisect_left(sizes, off + 1)  # first sample at which the log had grown past this line
            if k < len(sizes):
                per_second[int(sizes_ms[k] // 1000)].append(line)
        busy = collections.defaultdict(lambda: [0, 0])
        for ms, rip, _ in seq:
            b = busy[int(ms // 1000)]
            b[0] += 0 if in_ntdll(rip) else 1
            b[1] += 1
        print(f'\n== {a.thread} timeline (busy % of samples per second; log lines that appeared in that second)')
        for sec in sorted(busy):
            b = busy[sec]
            lines = per_second.get(sec, [])
            disc = sum('DVCI' in l for l in lines)
            fps = [l for l in lines if l.startswith('[fps]')]
            extra = (f'  disc-cache misses {disc}' if disc else '') + (f'  {fps[0][:70]}' if fps else '')
            print(f'   {sec:3d}s  busy {b[0] * 100 // b[1]:3d}%{extra}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
