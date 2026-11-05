"""Find code in SLPM_662.75 that builds a 32-bit constant with lui + ori/addiu (same register). Usage: find_const.py elf hexvalue..."""
import struct
import sys

LOAD_VADDR, LOAD_OFFSET = 0x100000, 0x100
CODE_END = 0x3FEC88

elf = open(sys.argv[1], 'rb').read()


def word(a):
    return struct.unpack_from('<I', elf, a - LOAD_VADDR + LOAD_OFFSET)[0]


for text in sys.argv[2:]:
    value = int(text, 16)
    hits = []
    for a in range(LOAD_VADDR, CODE_END - 4, 4):
        w = word(a)
        if (w >> 26) != 0x0F:
            continue
        rt = (w >> 16) & 31
        for k in range(1, 8):
            lo = word(a + 4 * k)
            op = lo >> 26
            if op in (0x09, 0x0D) and ((lo >> 21) & 31) == rt:
                imm = lo & 0xFFFF
                if op == 0x09 and imm & 0x8000:
                    imm -= 0x10000
                if (((w & 0xFFFF) << 16) + imm) & 0xFFFFFFFF == value:
                    hits.append(a)
                break
        if (w & 0xFFFF) << 16 == value and not hits[-1:] == [a]:
            pass
    print('%08X:' % value, ' '.join('%08X' % h for h in hits[:30]), '(%d)' % len(hits))
