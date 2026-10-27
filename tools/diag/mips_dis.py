"""Tiny MIPS I disassembler for IOP IRX modules (enough to read control flow).
Usage: mips_dis.py file.irx load_base start_addr count
Addresses are IOP addresses (load_base + ELF vaddr); relocations are not applied (jal targets are module-relative)."""
import struct
import sys

R = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
     's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
SPECIAL = {0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav', 8: 'jr', 9: 'jalr', 12: 'syscall', 13: 'break',
           16: 'mfhi', 17: 'mthi', 18: 'mflo', 19: 'mtlo', 24: 'mult', 25: 'multu', 26: 'div', 27: 'divu', 32: 'add',
           33: 'addu', 34: 'sub', 35: 'subu', 36: 'and', 37: 'or', 38: 'xor', 39: 'nor', 42: 'slt', 43: 'sltu'}
IMM = {8: 'addi', 9: 'addiu', 10: 'slti', 11: 'sltiu', 12: 'andi', 13: 'ori', 14: 'xori', 15: 'lui', 32: 'lb', 33: 'lh',
       34: 'lwl', 35: 'lw', 36: 'lbu', 37: 'lhu', 38: 'lwr', 40: 'sb', 41: 'sh', 42: 'swl', 43: 'sw', 46: 'swr'}


def dis(w, pc):
    op = w >> 26
    rs, rt, rd, sa, fn = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if w == 0:
        return 'nop'
    if op == 0:
        n = SPECIAL.get(fn, f'special{fn}')
        if n in ('sll', 'srl', 'sra'):
            return f'{n} {R[rd]}, {R[rt]}, {sa}'
        if n == 'jr':
            return f'jr {R[rs]}'
        if n == 'jalr':
            return f'jalr {R[rd]}, {R[rs]}'
        if n in ('mfhi', 'mflo'):
            return f'{n} {R[rd]}'
        if n in ('mult', 'multu', 'div', 'divu'):
            return f'{n} {R[rs]}, {R[rt]}'
        if n in ('syscall', 'break'):
            return n
        return f'{n} {R[rd]}, {R[rs]}, {R[rt]}'
    if op == 1:
        return f'{"bgez" if rt & 1 else "bltz"}{"al" if rt & 16 else ""} {R[rs]}, {pc + 4 + simm * 4:#x}'
    if op in (2, 3):
        return f'{"j" if op == 2 else "jal"} {(w & 0x3FFFFFF) << 2:#x}'
    if op in (4, 5):
        return f'{"beq" if op == 4 else "bne"} {R[rs]}, {R[rt]}, {pc + 4 + simm * 4:#x}'
    if op in (6, 7):
        return f'{"blez" if op == 6 else "bgtz"} {R[rs]}, {pc + 4 + simm * 4:#x}'
    if op == 16:
        return f'cop0 {w:08x}'
    n = IMM.get(op, f'op{op}')
    if n == 'lui':
        return f'lui {R[rt]}, {imm:#x}'
    if n in ('andi', 'ori', 'xori'):
        return f'{n} {R[rt]}, {R[rs]}, {imm:#x}'
    if op >= 32:
        return f'{n} {R[rt]}, {simm}({R[rs]})'
    return f'{n} {R[rt]}, {R[rs]}, {simm}'


def main():
    path, base, start, count = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), int(sys.argv[4])
    elf = open(path, 'rb').read()
    phoff = struct.unpack_from('<I', elf, 0x1C)[0]
    phnum = struct.unpack_from('<H', elf, 0x2C)[0]
    segs = []
    for i in range(phnum):
        ptype, off, vaddr, paddr, filesz, memsz = struct.unpack_from('<6I', elf, phoff + 32 * i)
        if ptype == 1:
            segs.append((vaddr, off, filesz))
    for k in range(count):
        a = start + 4 * k
        v = a - base
        for vaddr, off, filesz in segs:
            if vaddr <= v < vaddr + filesz:
                w = struct.unpack_from('<I', elf, off + v - vaddr)[0]
                print(f'{a:08x}: {w:08x}  {dis(w, a)}')
                break
        else:
            print(f'{a:08x}: --')


main()
