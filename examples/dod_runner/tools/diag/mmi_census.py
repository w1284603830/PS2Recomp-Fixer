"""Count R5900 MMI instructions in the EE code of the ELF (by mnemonic), with a few sample addresses.
Usage: mmi_census.py elf"""
import struct
import sys
from collections import defaultdict

LOAD_VADDR, LOAD_OFFSET = 0x100000, 0x100
CODE_END = 0x3FEC88
elf = open(sys.argv[1], 'rb').read()

DIRECT = {0x00: 'madd', 0x01: 'maddu', 0x04: 'plzcw', 0x10: 'mfhi1', 0x11: 'mthi1', 0x12: 'mflo1', 0x13: 'mtlo1',
          0x18: 'mult1', 0x19: 'multu1', 0x1A: 'div1', 0x1B: 'divu1', 0x20: 'madd1', 0x21: 'maddu1', 0x30: 'pmfhl',
          0x31: 'pmthl', 0x34: 'psllh', 0x36: 'psrlh', 0x37: 'psrah', 0x3C: 'psllw', 0x3E: 'psrlw', 0x3F: 'psraw'}
MMI0 = {0x00: 'paddw', 0x01: 'psubw', 0x02: 'pcgtw', 0x03: 'pmaxw', 0x04: 'paddh', 0x05: 'psubh', 0x06: 'pcgth',
        0x07: 'pmaxh', 0x08: 'paddb', 0x09: 'psubb', 0x0A: 'pcgtb', 0x10: 'paddsw', 0x11: 'psubsw', 0x12: 'pextlw',
        0x13: 'ppacw', 0x14: 'paddsh', 0x15: 'psubsh', 0x16: 'pextlh', 0x17: 'ppach', 0x18: 'paddsb', 0x19: 'psubsb',
        0x1A: 'pextlb', 0x1B: 'ppacb', 0x1E: 'pext5', 0x1F: 'ppac5'}
MMI1 = {0x01: 'pabsw', 0x02: 'pceqw', 0x03: 'pminw', 0x04: 'padsbh', 0x05: 'pabsh', 0x06: 'pceqh', 0x07: 'pminh',
        0x0A: 'pceqb', 0x10: 'padduw', 0x11: 'psubuw', 0x12: 'pextuw', 0x14: 'padduh', 0x15: 'psubuh', 0x16: 'pextuh',
        0x18: 'paddub', 0x19: 'psubub', 0x1A: 'pextub', 0x1B: 'qfsrv'}
MMI2 = {0x00: 'pmaddw', 0x02: 'psllvw', 0x03: 'psrlvw', 0x04: 'pmsubw', 0x08: 'pmfhi', 0x09: 'pmflo', 0x0A: 'pinth',
        0x0C: 'pmultw', 0x0D: 'pdivw', 0x0E: 'pcpyld', 0x10: 'pmaddh', 0x11: 'phmadh', 0x12: 'pand', 0x13: 'pxor',
        0x14: 'pmsubh', 0x15: 'phmsbh', 0x1A: 'pexeh', 0x1B: 'prevh', 0x1C: 'pmulth', 0x1D: 'pdivbw', 0x1E: 'pexew',
        0x1F: 'prot3w'}
MMI3 = {0x00: 'pmadduw', 0x03: 'psravw', 0x08: 'pmthi', 0x09: 'pmtlo', 0x0A: 'pinteh', 0x0C: 'pmultuw', 0x0D: 'pdivuw',
        0x0E: 'pcpyud', 0x12: 'por', 0x13: 'pnor', 0x1A: 'pexch', 0x1B: 'pcpyh', 0x1E: 'pexcw'}

counts = defaultdict(list)
for a in range(LOAD_VADDR, CODE_END, 4):
    w = struct.unpack_from('<I', elf, a - LOAD_VADDR + LOAD_OFFSET)[0]
    if (w >> 26) != 0x1C:
        continue
    funct = w & 0x3F
    sub = (w >> 6) & 0x1F
    if funct == 0x08:
        name = MMI0.get(sub, f'mmi0_{sub:x}')
    elif funct == 0x28:
        name = MMI1.get(sub, f'mmi1_{sub:x}')
    elif funct == 0x09:
        name = MMI2.get(sub, f'mmi2_{sub:x}')
    elif funct == 0x29:
        name = MMI3.get(sub, f'mmi3_{sub:x}')
    else:
        name = DIRECT.get(funct, f'mmi_{funct:x}')
        if name == 'pmfhl':
            name += '.' + {0: 'lw', 1: 'uw', 2: 'slw', 3: 'lh', 4: 'sh'}.get(sub, str(sub))
    counts[name].append(a)
for name in sorted(counts, key=lambda n: -len(counts[n])):
    print(f'{name:10s} {len(counts[name]):6d}  ' + ' '.join(f'{x:06X}' for x in counts[name][:6]))
