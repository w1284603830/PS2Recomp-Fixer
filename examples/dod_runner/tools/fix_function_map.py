"""Clean the Ghidra function map (ExportPS2Functions.java CSV) of SLPM_662.75 before recompiling.

Two kinds of records make the recompiler emit hundreds of megabytes of useless C++:
  * label records that cover data: the ELF has one RWX segment, so Ghidra's "executable labels"
    after the last code function span the data area (0x408DA0..0x686100);
  * functions whose Ghidra body is not contiguous: the export uses the body's highest address as the
    end, so a 32-byte function with a far-away chunk becomes a 590 KB range.

Fixes: drop label records that start after the code area (except labels inside the small code island
Ghidra found at 0x6804C0..0x6807E0), and cut every record's end at the next function start.

Usage: python -I fix_function_map.py <in.csv> <out.csv>
"""
import csv
import sys

CODE_END = 0x00400000            # last Ghidra function ends at 0x003FEC88
ISLAND = (0x006804C0, 0x006807E0)  # small functions Ghidra found inside the data segment


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, newline="") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        r["start"] = int(r["Start"], 16)
        r["end"] = int(r["End"], 16)
        r["label"] = r["Name"].startswith("entry_")

    def keep(r):
        if r["start"] < CODE_END:
            return True
        if ISLAND[0] <= r["start"] < ISLAND[1]:
            return r["end"] <= ISLAND[1]
        return False

    kept = [r for r in rows if keep(r)]
    func_starts = sorted({r["start"] for r in kept if not r["label"]})

    import bisect
    trimmed = 0
    for r in kept:
        i = bisect.bisect_right(func_starts, r["start"])
        limit = func_starts[i] if i < len(func_starts) else None
        end = r["end"]
        if limit is not None and end > limit:
            end = limit
        if r["start"] < CODE_END and end > CODE_END:
            end = CODE_END
        if end != r["end"]:
            trimmed += 1
            r["end"] = end
    kept.sort(key=lambda r: r["start"])

    with open(dst, "w", newline="") as f:
        f.write("Name,Start,End,Size\n")
        for r in kept:
            f.write("%s,0x%08X,0x%08X,%d\n" % (r["Name"], r["start"], r["end"], r["end"] - r["start"]))
    print("records in %d, kept %d, dropped %d, trimmed %d" % (len(rows), len(kept), len(rows) - len(kept), trimmed))
    big = sorted(kept, key=lambda r: r["start"] - r["end"])[:5]
    for r in big:
        print("  largest: %s 0x%08X..0x%08X (%d bytes)" % (r["Name"], r["start"], r["end"], r["end"] - r["start"]))


if __name__ == "__main__":
    main()
