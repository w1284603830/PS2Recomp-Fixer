"""Small R5900 (EE) disassembler for the game ELF: enough to read a library function's control flow and its loads and
stores (MIPS I/II/III plus lq/sq, ld/sd, the 64-bit ALU ops; anything else is printed as a raw word).
Usage: python -I ee_dis.py <elf> <start vaddr> <count>"""
import struct
import sys

R = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
     's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
SPECIAL = {0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav', 8: 'jr', 9: 'jalr', 10: 'movz', 11: 'movn',
           12: 'syscall', 13: 'break', 15: 'sync', 16: 'mfhi', 17: 'mthi', 18: 'mflo', 19: 'mtlo', 20: 'dsllv',
           22: 'dsrlv', 23: 'dsrav', 24: 'mult', 25: 'multu', 26: 'div', 27: 'divu', 32: 'add', 33: 'addu', 34: 'sub',
           35: 'subu', 36: 'and', 37: 'or', 38: 'xor', 39: 'nor', 42: 'slt', 43: 'sltu', 44: 'dadd', 45: 'daddu',
           46: 'dsub', 47: 'dsubu', 56: 'dsll', 58: 'dsrl', 59: 'dsra', 60: 'dsll32', 62: 'dsrl32', 63: 'dsra32'}
IMM = {8: 'addi', 9: 'addiu', 10: 'slti', 11: 'sltiu', 12: 'andi', 13: 'ori', 14: 'xori', 15: 'lui', 24: 'daddi',
       25: 'daddiu', 26: 'ldl', 27: 'ldr', 30: 'lq', 31: 'sq', 32: 'lb', 33: 'lh', 34: 'lwl', 35: 'lw', 36: 'lbu',
       37: 'lhu', 38: 'lwr', 39: 'lwu', 40: 'sb', 41: 'sh', 42: 'swl', 43: 'sw', 44: 'sdl', 45: 'sdr', 46: 'swr',
       55: 'ld', 63: 'sd', 49: 'lwc1', 57: 'swc1'}
BRANCH = {4: 'beq', 5: 'bne', 6: 'blez', 7: 'bgtz', 20: 'beql', 21: 'bnel', 22: 'blezl', 23: 'bgtzl'}


def load(path):
    elf = open(path, 'rb').read()
    phoff = struct.unpack_from('<I', elf, 0x1C)[0]
    phnum = struct.unpack_from('<H', elf, 0x2C)[0]
    segs = []
    for i in range(phnum):
        t, off, va, pa, fs = struct.unpack_from('<5I', elf, phoff + i * 32)
        if t == 1:
            segs.append((va, off, fs))
    return elf, segs


def word(elf, segs, va):
    for sva, off, fs in segs:
        if sva <= va < sva + fs:
            return struct.unpack_from('<I', elf, off + va - sva)[0]
    return None


def dis(w, pc):
    op = w >> 26
    rs, rt, rd, sa, fn = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if w == 0:
        return 'nop'
    if op == 0:
        name = SPECIAL.get(fn)
        if name in ('sll', 'srl', 'sra', 'dsll', 'dsrl', 'dsra', 'dsll32', 'dsrl32', 'dsra32'):
            return f'{name} {R[rd]}, {R[rt]}, {sa}'
        if name == 'jr':
            return f'jr {R[rs]}'
        if name == 'jalr':
            return f'jalr {R[rd]}, {R[rs]}'
        if name in ('mfhi', 'mflo'):
            return f'{name} {R[rd]}'
        if name in ('syscall', 'break', 'sync'):
            return name
        if name:
            return f'{name} {R[rd]}, {R[rs]}, {R[rt]}'
    if op == 1:
        kind = {0: 'bltz', 1: 'bgez', 2: 'bltzl', 3: 'bgezl', 16: 'bltzal', 17: 'bgezal'}.get(rt, f'regimm{rt}')
        return f'{kind} {R[rs]}, 0x{pc + 4 + simm * 4:X}'
    if op in (2, 3):
        return f'{"j" if op == 2 else "jal"} 0x{((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2):X}'
    if op in BRANCH:
        if op in (6, 7, 22, 23):
            return f'{BRANCH[op]} {R[rs]}, 0x{pc + 4 + simm * 4:X}'
        return f'{BRANCH[op]} {R[rs]}, {R[rt]}, 0x{pc + 4 + simm * 4:X}'
    if op in IMM:
        name = IMM[op]
        if name == 'lui':
            return f'lui {R[rt]}, 0x{imm:X}'
        if name in ('andi', 'ori', 'xori'):
            return f'{name} {R[rt]}, {R[rs]}, 0x{imm:X}'
        if name in ('addi', 'addiu', 'slti', 'sltiu', 'daddi', 'daddiu'):
            return f'{name} {R[rt]}, {R[rs]}, {simm}'
        reg = f'$f{rt}' if name in ('lwc1', 'swc1') else R[rt]
        return f'{name} {reg}, {simm}({R[rs]})'
    return f'.word 0x{w:08X}'


def main():
    elf, segs = load(sys.argv[1])
    start = int(sys.argv[2], 0)
    count = int(sys.argv[3])
    for i in range(count):
        pc = start + i * 4
        w = word(elf, segs, pc)
        if w is None:
            break
        print(f'{pc:08X}: {w:08X}  {dis(w, pc)}')


if __name__ == '__main__':
    main()
