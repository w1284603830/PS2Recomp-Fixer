"""List likely function starts that the function map lacks: the address after `jr $ra` or `j` + delay slot (+ nop
padding) when no branch of the code before it (back to the previous function start) jumps past it.
Usage: find_missing_function_starts.py elf functions.csv [out.csv]"""
import csv
import struct
import sys

LOAD_VADDR, LOAD_OFFSET = 0x100000, 0x100
CODE_END = 0x3FEC88

elf = open(sys.argv[1], 'rb').read()
word = lambda a: struct.unpack_from('<I', elf, a - LOAD_VADDR + LOAD_OFFSET)[0]
starts = set()
for row in csv.DictReader(open(sys.argv[2], encoding='utf-8')):
    starts.add(int(row['Start'], 16))
ordered = sorted(starts)


def branch_target(addr, w):
    op = w >> 26
    if op in (4, 5, 6, 7, 20, 21, 22, 23) or (op == 1 and ((w >> 16) & 0x1F) in (0, 1, 2, 3, 16, 17, 18, 19)):
        off = w & 0xFFFF
        if off & 0x8000:
            off -= 0x10000
        return addr + 4 + (off << 2)
    if op == 2:  # j (not jal): a jump within / out of the function
        return ((addr + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
    return None


import bisect
found = []
addr = LOAD_VADDR
while addr < CODE_END - 8:
    w0 = word(addr)
    if w0 == 0x03E00008 or (w0 >> 26) == 2:  # jr $ra, or j (a tail call ends a function too)
        cand = addr + 8
        while cand < CODE_END and word(cand) == 0:
            cand += 4
        if cand < CODE_END and cand not in starts and word(cand) != 0:
            i = bisect.bisect_right(ordered, cand) - 1
            fstart = ordered[i] if i >= 0 else LOAD_VADDR
            continues = False
            for a in range(fstart, cand, 4):
                t = branch_target(a, word(a))
                if t is not None and cand <= t < cand + 0x10000 and (i + 1 >= len(ordered) or t < ordered[i + 1]):
                    continues = True
                    break
            if not continues:
                found.append(cand)
        addr = cand if cand > addr else addr + 4
    else:
        addr += 4
print(f'{len(found)} candidates')
for a in found[:40]:
    print(hex(a))
if len(sys.argv) > 3:
    with open(sys.argv[3], 'w', newline='') as f:
        for a in found:
            f.write(f'0x{a:08X}\n')
