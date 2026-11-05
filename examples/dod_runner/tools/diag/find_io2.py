"""List loads/stores whose address is lui rX,HI + signed offset, for absolute addresses in [lo, hi) (hex).
Follows each lui for 24 instructions (until rX is overwritten). Handles lui 0x1001 + negative offsets.
Usage: find_io2.py elf lo hi"""
import struct
import sys

LOAD_VADDR, LOAD_OFFSET = 0x100000, 0x100
CODE_END = 0x3FEC88
elf = open(sys.argv[1], 'rb').read()
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
MEMOPS = {0x20: 'lb', 0x21: 'lh', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu', 0x27: 'lwu', 0x37: 'ld', 0x1E: 'lq',
          0x28: 'sb', 0x29: 'sh', 0x2B: 'sw', 0x3F: 'sd', 0x1F: 'sq'}


def word(a):
    return struct.unpack_from('<I', elf, a - LOAD_VADDR + LOAD_OFFSET)[0]


def writes_reg(w, r):
    op = w >> 26
    if op == 0:  # SPECIAL: rd
        return ((w >> 11) & 31) == r and (w & 0x3F) not in (0x08, 0x09)  # jr/jalr excluded (jalr writes rd though)
    if op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x18, 0x19, 0x20, 0x21, 0x23, 0x24, 0x25, 0x27, 0x37,
              0x1E, 0x1A, 0x1B):
        return ((w >> 16) & 31) == r
    return False


hits = {}
for a in range(LOAD_VADDR, CODE_END - 4, 4):
    w = word(a)
    if (w >> 26) != 0x0F:
        continue
    r = (w >> 16) & 31
    base = (w & 0xFFFF) << 16
    for b in range(a + 4, min(a + 4 + 24 * 4, CODE_END), 4):
        x = word(b)
        op = x >> 26
        if op in MEMOPS and ((x >> 21) & 31) == r:
            imm = x & 0xFFFF
            if imm & 0x8000:
                imm -= 0x10000
            addr = (base + imm) & 0xFFFFFFFF
            if lo <= addr < hi:
                hits.setdefault(addr, []).append(f'{b:08X}:{MEMOPS[op]}')
        if writes_reg(x, r):
            break
for addr in sorted(hits):
    print(f'{addr:08X} ' + ' '.join(hits[addr]) + f' ({len(hits[addr])})')
