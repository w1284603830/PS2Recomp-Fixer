"""Turn the "[stack] tid=N rva rva ..." lines of a --trace log into function names using the MSVC linker map.

Prints each sampled stack innermost first, keeping recompiled guest functions (FUN_/entry_/sce.../named) and
host runtime functions; consecutive duplicate stacks are collapsed.

Usage: python -I symbolize_trace.py <ps2EntryRunner.map> <err.log> [max stacks]
"""
import bisect
import re
import sys

SYM = re.compile(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s")


def load_map(path):
    base = None
    syms = []
    for line in open(path, encoding="latin-1"):
        if base is None:
            m = re.search(r"Preferred load address is ([0-9a-fA-F]+)", line)
            if m:
                base = int(m.group(1), 16)
            continue
        m = SYM.match(line)
        if m:
            syms.append((int(m.group(2), 16) - base, m.group(1)))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def short(name):
    # ?FUN_00153460_0x153460@@YAXPEAE... -> FUN_00153460_0x153460
    if name.startswith("?"):
        name = name[1:].split("@", 1)[0]
    return name


GUEST = re.compile(r"^(FUN_|entry_|thunk_|caseD_|func_|sce|_sce|isce|mce)")


def main():
    addrs, names = load_map(sys.argv[1])
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    guest_only = len(sys.argv) > 4 and sys.argv[4] == "guest"
    counts = {}
    last = None
    shown = 0
    for line in open(sys.argv[2], encoding="utf-8", errors="replace"):
        if not line.startswith("[stack]"):
            continue
        parts = line.split()
        tid = parts[1]
        frames = []
        for rva in parts[2:]:
            try:
                value = int(rva, 16)
            except ValueError:  # another thread's output interleaved into this line
                break
            i = bisect.bisect_right(addrs, value) - 1
            frames.append(short(names[i]) if i >= 0 else rva)
        key = (tid, tuple(frames))
        if guest_only:
            if any(GUEST.match(f) for f in frames):
                counts[key] = counts.get(key, 0) + 1
            continue
        if key == last:
            continue
        last = key
        print(tid, " <- ".join(frames))
        shown += 1
        if shown >= limit:
            break
    for (tid, frames), n in sorted(counts.items(), key=lambda kv: -kv[1])[:limit]:
        print("%4d x" % n, " <- ".join(frames))


if __name__ == "__main__":
    main()
