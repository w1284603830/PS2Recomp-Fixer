"""How evenly the game's fields reached the screen, from pcsx2gs.dll's PS2X_PRESENT_STATS=<file> log.

The DLL writes, after every present, the swap chain's frame statistics: the last present the monitor showed and the
monitor refresh it was first shown on. Consecutive presents give how many refreshes each field stayed on screen; with
the vsync lock at 60 Hz that is 1 for every field, and anything else (a field shown for 2 refreshes, one never shown)
is what the eye sees as judder.

Usage: python -I present_stats.py <stats.csv> [skip first N presents, default 600]
"""
import collections
import csv
import sys


def main():
    path = sys.argv[1]
    skip = int(sys.argv[2]) if len(sys.argv) > 2 else 600
    shown = {}
    rows = 0
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            rows += 1
            present = int(row["shown_present"])
            refresh = int(row["shown_refresh"])
            if present and refresh and present not in shown:
                shown[present] = refresh
    presents = sorted(p for p in shown if p > skip)
    if len(presents) < 2:
        print(f"{rows} rows, no frame statistics (windowed DWM composition does not report them)")
        return
    durations = collections.Counter()
    never = 0
    for a, b in zip(presents, presents[1:]):
        if b == a + 1:
            durations[shown[b] - shown[a]] += 1
        else:
            never += b - a - 1
    total = sum(durations.values())
    print(f"{rows} presents logged, {len(presents)} with statistics after the first {skip}")
    for refreshes, count in sorted(durations.items()):
        print(f"  shown for {refreshes} refresh(es): {count:6d}  ({100.0 * count / total:5.1f} %)")
    print(f"  presents never shown: {never}")


if __name__ == "__main__":
    main()
