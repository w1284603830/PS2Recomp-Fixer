"""Count COP2 (VU0 macro mode) instructions in the EE code of the ELF, with sample addresses.
Usage: cop2_census.py elf"""
import struct
import sys
from collections import defaultdict

LOAD_VADDR, LOAD_OFFSET = 0x100000, 0x100
CODE_END = 0x3FEC88
elf = open(sys.argv[1], 'rb').read()

SP1 = {0x00: 'vaddx', 0x01: 'vaddy', 0x02: 'vaddz', 0x03: 'vaddw', 0x04: 'vsubx', 0x05: 'vsuby', 0x06: 'vsubz',
       0x07: 'vsubw', 0x08: 'vmaddx', 0x09: 'vmaddy', 0x0A: 'vmaddz', 0x0B: 'vmaddw', 0x0C: 'vmsubx', 0x0D: 'vmsuby',
       0x0E: 'vmsubz', 0x0F: 'vmsubw', 0x10: 'vmaxx', 0x11: 'vmaxy', 0x12: 'vmaxz', 0x13: 'vmaxw', 0x14: 'vminix',
       0x15: 'vminiy', 0x16: 'vminiz', 0x17: 'vminiw', 0x18: 'vmulx', 0x19: 'vmuly', 0x1A: 'vmulz', 0x1B: 'vmulw',
       0x1C: 'vmulq', 0x1D: 'vmaxi', 0x1E: 'vmuli', 0x1F: 'vminii', 0x20: 'vaddq', 0x21: 'vmaddq', 0x22: 'vaddi',
       0x23: 'vmaddi', 0x24: 'vsubq', 0x25: 'vmsubq', 0x26: 'vsubi', 0x27: 'vmsubi', 0x28: 'vadd', 0x29: 'vmadd',
       0x2A: 'vmul', 0x2B: 'vmax', 0x2C: 'vsub', 0x2D: 'vmsub', 0x2E: 'vopmsub', 0x2F: 'vmini', 0x30: 'viadd',
       0x31: 'visub', 0x32: 'viaddi', 0x34: 'viand', 0x35: 'vior', 0x38: 'vcallms', 0x39: 'vcallmsr'}
SP2 = {0x00: 'vaddax', 0x01: 'vadday', 0x02: 'vaddaz', 0x03: 'vaddaw', 0x04: 'vsubax', 0x05: 'vsubay', 0x06: 'vsubaz',
       0x07: 'vsubaw', 0x08: 'vmaddax', 0x09: 'vmadday', 0x0A: 'vmaddaz', 0x0B: 'vmaddaw', 0x0C: 'vmsubax',
       0x0D: 'vmsubay', 0x0E: 'vmsubaz', 0x0F: 'vmsubaw', 0x10: 'vitof0', 0x11: 'vitof4', 0x12: 'vitof12',
       0x13: 'vitof15', 0x14: 'vftoi0', 0x15: 'vftoi4', 0x16: 'vftoi12', 0x17: 'vftoi15', 0x18: 'vmulax', 0x19: 'vmulay',
       0x1A: 'vmulaz', 0x1B: 'vmulaw', 0x1C: 'vmulaq', 0x1D: 'vabs', 0x1E: 'vmulai', 0x1F: 'vclipw', 0x20: 'vaddaq',
       0x21: 'vmaddaq', 0x22: 'vaddai', 0x23: 'vmaddai', 0x24: 'vsubaq', 0x25: 'vmsubaq', 0x26: 'vsubai', 0x27: 'vmsubai',
       0x28: 'vadda', 0x29: 'vmadda', 0x2A: 'vmula', 0x2C: 'vsuba', 0x2D: 'vmsuba', 0x2E: 'vopmula', 0x2F: 'vnop',
       0x30: 'vmove', 0x31: 'vmr32', 0x34: 'vlqi', 0x35: 'vsqi', 0x36: 'vlqd', 0x37: 'vsqd', 0x38: 'vdiv', 0x39: 'vsqrt',
       0x3A: 'vrsqrt', 0x3B: 'vwaitq', 0x3C: 'vmtir', 0x3D: 'vmfir', 0x3E: 'vilwr', 0x3F: 'viswr', 0x40: 'vrnext',
       0x41: 'vrget', 0x42: 'vrinit', 0x43: 'vrxor'}
RS = {0x01: 'qmfc2', 0x02: 'cfc2', 0x05: 'qmtc2', 0x06: 'ctc2', 0x08: 'bc2'}

counts = defaultdict(list)
for a in range(LOAD_VADDR, CODE_END, 4):
    w = struct.unpack_from('<I', elf, a - LOAD_VADDR + LOAD_OFFSET)[0]
    op = w >> 26
    if op == 0x36:
        counts['lqc2'].append(a)
        continue
    if op == 0x3E:
        counts['sqc2'].append(a)
        continue
    if op != 0x12:
        continue
    rs = (w >> 21) & 0x1F
    if rs < 0x10:
        counts[RS.get(rs, f'cop2rs_{rs:x}')].append(a)
        continue
    funct = w & 0x3F
    if funct < 0x3C:
        counts[SP1.get(funct, f'sp1_{funct:x}')].append(a)
    else:
        code = ((w >> 4) & 0x7C) | (w & 3)
        counts[SP2.get(code, f'sp2_{code:x}')].append(a)
for name in sorted(counts, key=lambda n: -len(counts[n])):
    print(f'{name:9s} {len(counts[name]):6d}  ' + ' '.join(f'{x:06X}' for x in counts[name][:5]))
