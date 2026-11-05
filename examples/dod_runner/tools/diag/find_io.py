"""List loads/stores to EE I/O registers in a range: lui rX,0x1000 then a memory op on rX with the given offsets.
Usage: find_io.py elf lo_offset hi_offset   (offsets relative to 0x10000000, hex)"""
import struct
import sys

LOAD_VADDR, LOAD_OFFSET = 0x100000, 0x100
CODE_END = 0x3FEC88
elf = open(sys.argv[1], 'rb').read()
lo_off, hi_off = int(sys.argv[2], 16), int(sys.argv[3], 16)
MEMOPS = {0x20: 'lb', 0x21: 'lh', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu', 0x27: 'lwu', 0x37: 'ld', 0x1E: 'lq',
          0x28: 'sb', 0x29: 'sh', 0x2B: 'sw', 0x3F: 'sd', 0x1F: 'sq'}


def word(a):
    return struct.unpack_from('<I', elf, a - LOAD_VADDR + LOAD_OFFSET)[0]


hits = {}
for a in range(LOAD_VADDR, CODE_END - 4, 4):
    w = word(a)
    if (w >> 26) != 0x0F or (w & 0xFFFF) not in (0x1000, 0x1001):
        continue
    rt = (w >> 16) & 31
    base = (w & 0xFFFF) << 16
    for k in range(1, 24):
        i = word(a + 4 * k)
        op = i >> 26
        if op in MEMOPS and ((i >> 21) & 31) == rt:
            off = i & 0xFFFF
            if off & 0x8000:
                off -= 0x10000
            target = base + off
            if lo_off <= target - 0x10000000 < hi_off:
                hits.setdefault(target, []).append((a + 4 * k, MEMOPS[op]))
        # stop if the register is overwritten
        if ((i >> 16) & 31) == rt and op in (0x0F, 0x09, 0x0D, 0x23, 0x37):
            break
for t in sorted(hits):
    print('%08X' % t, ' '.join('%08X:%s' % h for h in hits[t][:12]), '(%d)' % len(hits[t]))
