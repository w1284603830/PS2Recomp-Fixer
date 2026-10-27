"""Compare two [callhist] reports (DOD_CALL_HIST with DOD_CALL_HIST_THREAD): rows present or much more frequent in one.
Usage: callhist_diff.py good_err.log bad_err.log"""
import re
import sys


def load(path):
    rows = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'\[callhist\]\s+(\d+)\s+([0-9A-F]{8}) <- ([0-9A-F]{8})', line)
        if m:
            rows[(m.group(2), m.group(3))] = int(m.group(1))
    return rows


good, bad = load(sys.argv[1]), load(sys.argv[2])
keys = set(good) | set(bad)
diffs = []
for k in keys:
    g, b = good.get(k, 0), bad.get(k, 0)
    if max(g, b) < 30:
        continue
    ratio = (g + 1) / (b + 1)
    diffs.append((ratio, k, g, b))
diffs.sort()
print('more in BAD (stall):')
for ratio, k, g, b in diffs[:25]:
    print(f'  target {k[0]} thread {int(k[1], 16)}: good {g} bad {b}')
print('more in GOOD:')
for ratio, k, g, b in diffs[::-1][:40]:
    print(f'  target {k[0]} thread {int(k[1], 16)}: good {g} bad {b}')
