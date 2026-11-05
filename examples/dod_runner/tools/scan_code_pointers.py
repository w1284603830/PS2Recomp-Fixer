"""Find function starts Ghidra missed in SLPM_662.75: code addresses that are only reached through pointers.

Candidates come from (a) 32-bit words in the data area whose value points into the code area and (b) code that
builds a code address with lui + addiu/ori. A candidate is kept only if it is not already a function start and
it looks like one: the previous function ends right before it (jr $ra + delay slot, optionally nop padding),
or it opens with "addiu $sp, $sp, -N". The new starts are appended to the CSV as FUN_ records; run
fix_function_map.py afterwards so labels and functions are cut at the new starts.

Usage: python -I scan_code_pointers.py <elf> <in.csv> <out.csv>
"""
import csv
import struct
import sys

LOAD_VADDR, LOAD_OFFSET = 0x100000, 0x100
CODE_START, CODE_END = 0x100000, 0x3FEC88
DATA_START, DATA_END = 0x408DA0, 0x686100  # initialised data after the code area
JR_RA = 0x03E00008


def main():
    elf, src, dst = sys.argv[1:4]
    image = open(elf, "rb").read()

    def word(addr):
        return struct.unpack_from("<I", image, addr - LOAD_VADDR + LOAD_OFFSET)[0]

    with open(src, newline="") as f:
        rows = list(csv.DictReader(f))
    func_starts = {int(r["Start"], 16) for r in rows if not r["Name"].startswith("entry_")}

    candidates = {}
    for addr in range(DATA_START, DATA_END, 4):
        v = word(addr)
        if CODE_START <= v < CODE_END and v % 4 == 0:
            candidates.setdefault(v, "data@%08X" % addr)
    for addr in range(CODE_START, CODE_END - 4, 4):
        hi = word(addr)
        if (hi >> 26) != 0x0F:  # lui
            continue
        rt = (hi >> 16) & 31
        for k in range(1, 6):
            lo = word(addr + 4 * k)
            op = lo >> 26
            if op in (0x09, 0x0D) and ((lo >> 21) & 31) == rt:  # addiu / ori from the lui register
                imm = lo & 0xFFFF
                if op == 0x09 and imm & 0x8000:
                    imm -= 0x10000
                v = ((hi & 0xFFFF) << 16) + imm
                if CODE_START <= v < CODE_END and v % 4 == 0:
                    candidates.setdefault(v, "code@%08X" % addr)
                break

    def looks_like_start(t):
        w = word(t)
        if (w >> 16) == 0x27BD and (w & 0x8000):  # addiu sp, sp, -N
            return True
        prev = [word(t - 4 * i) for i in range(1, 5)]
        if prev[1] == JR_RA:
            return True
        if prev[0] == 0 and prev[2] == JR_RA:
            return True
        if prev[0] == 0 and prev[1] == 0 and prev[3] == JR_RA:
            return True
        return False

    # Direct call targets are function entries by definition (e.g. jal 0x247070, which Ghidra left inside
    # another function), so they need no shape check.
    jal_targets = set()
    for addr in range(CODE_START, CODE_END, 4):
        w = word(addr)
        if (w >> 26) == 0x03:
            t = (addr & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if CODE_START <= t < CODE_END:
                jal_targets.add(t)

    added = []
    for t in sorted(set(candidates) | jal_targets):
        if t in func_starts:
            continue
        if t not in jal_targets and not looks_like_start(t):
            continue
        added.append(t)
        candidates.setdefault(t, "jal")
    with open(dst, "w", newline="") as f:
        f.write("Name,Start,End,Size\n")
        for r in rows:
            f.write("%s,%s,%s,%s\n" % (r["Name"], r["Start"], r["End"], r["Size"]))
        for t in added:
            f.write("FUN_%08x,0x%08X,0x%08X,%d\n" % (t, t, CODE_END, CODE_END - t))
    print("candidates %d, already functions %d, new function starts %d"
          % (len(candidates), sum(1 for t in candidates if t in func_starts), len(added)))
    for t in added[:15]:
        print("  0x%08X from %s" % (t, candidates[t]))
    print("  0x0024B728 found:", 0x24B728 in added)


if __name__ == "__main__":
    main()
