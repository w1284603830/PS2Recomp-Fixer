"""Game frame rate over the key-script time of a pcsx2_compare.py run (RUNTIME_PATCHES row 124).

Usage: python -I pcsx2_compare_report.py <reference run dir> [--from 0 --to 65 --step 1]

samples.csv has t (seconds from the new game, when the key script starts), and the game's frame counter
0x817730 and field counter 0x817734 (both found with find_frame_counter.py). Prints frames/fields per window, and
the game's DVCI disc-cache misses from emulog.txt placed on the same t axis (emulog seconds minus the new-game
moment in keys.log, both from the PCSX2 start; about +-0.3 s).
"""
import re
import sys
from pathlib import Path


def main():
    run = Path(sys.argv[1])
    t_from = float(sys.argv[sys.argv.index('--from') + 1]) if '--from' in sys.argv else 0.0
    t_to = float(sys.argv[sys.argv.index('--to') + 1]) if '--to' in sys.argv else 65.0
    step = float(sys.argv[sys.argv.index('--step') + 1]) if '--step' in sys.argv else 1.0
    rows = []
    lines = (run / 'samples.csv').read_text().splitlines()
    head = lines[0].split(',')
    fi, vi = head.index('0x817730'), head.index('0x817734')
    for line in lines[1:]:
        f = line.split(',')
        t = float(f[0])
        if t > -100:
            rows.append((t, int(f[fi]), int(f[vi])))
    new_game = None
    for line in (run / 'keys.log').read_text().splitlines():
        m = re.match(r'([0-9.]+) new game', line)
        if m:
            new_game = float(m.group(1))
    misses = []
    for line in (run / 'emulog.txt').read_text(errors='replace').splitlines():
        m = re.match(r'\[\s*([0-9.]+)\]\s.*DVCI: File cache was not hit', line)
        if m and new_game is not None:
            misses.append(float(m.group(1)) - new_game)

    def value_at(t):
        best = min(rows, key=lambda r: abs(r[0] - t))
        return best

    print(f'disc-cache misses at t = ' + ', '.join(f'{m:.1f}' for m in misses))
    t = t_from
    while t + step <= t_to + 1e-9:
        a, b = value_at(t), value_at(t + step)
        span = b[0] - a[0]
        frames, fields = b[1] - a[1], b[2] - a[2]
        mark = ' *' if any(t <= m < t + step for m in misses) else ''
        print(f'  t {t:5.1f}-{t + step:5.1f}: {frames:4d} frames / {fields:4d} fields = '
              f'{frames / span if span else 0:5.1f} game fps{mark}')
        t += step
    return 0


if __name__ == '__main__':
    sys.exit(main())
