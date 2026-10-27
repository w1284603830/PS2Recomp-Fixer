"""Merge DOD_IO_CENSUS files (EE <name>.csv and IOP <name>.csv.iop.csv) and print every register the game touched,
named, with read / write counts (summed over the runs), the OR of the written values (bits the game sets), the AND
(bits always set) and in how many runs it appeared.

Usage: python -I io_census_report.py <dir or files...> [--ee] [--iop] [--spu2]
"""
import glob
import os
import sys

sys.stdout.reconfigure(encoding='utf-8')

EE = {
    0x10002000: 'IPU_CMD', 0x10002010: 'IPU_CTRL', 0x10002020: 'IPU_BP', 0x10002030: 'IPU_TOP',
    0x10003000: 'GIF_CTRL', 0x10003010: 'GIF_MODE', 0x10003020: 'GIF_STAT', 0x10003040: 'GIF_TAG0', 0x10003050: 'GIF_TAG1',
    0x10003060: 'GIF_TAG2', 0x10003070: 'GIF_TAG3', 0x10003080: 'GIF_CNT', 0x10003090: 'GIF_P3CNT', 0x100030A0: 'GIF_P3TAG',
    0x10004000: 'VIF0_FIFO', 0x10005000: 'VIF1_FIFO', 0x10006000: 'GIF_FIFO', 0x10007000: 'IPU_OUT_FIFO', 0x10007010: 'IPU_IN_FIFO',
    0x1000E000: 'D_CTRL', 0x1000E010: 'D_STAT', 0x1000E020: 'D_PCR', 0x1000E030: 'D_SQWC', 0x1000E040: 'D_RBSR',
    0x1000E050: 'D_RBOR', 0x1000E060: 'D_STADR', 0x1000F000: 'INTC_STAT', 0x1000F010: 'INTC_MASK', 0x1000F100: 'SIO_LCR',
    0x1000F110: 'SIO_LSR', 0x1000F120: 'SIO_IER', 0x1000F130: 'SIO_ISR', 0x1000F140: 'SIO_FCR', 0x1000F180: 'SIO_TXFIFO',
    0x1000F1C0: 'SIO_RXFIFO', 0x1000F200: 'SBUS_MSCOM', 0x1000F210: 'SBUS_SMCOM', 0x1000F220: 'SBUS_MSFLG', 0x1000F230: 'SBUS_SMFLG',
    0x1000F240: 'SBUS_F240', 0x1000F260: 'SBUS_F260', 0x1000F430: 'MCH_RICM', 0x1000F440: 'MCH_DRD', 0x1000F480: 'MCH_?',
    0x1000F520: 'D_ENABLER', 0x1000F590: 'D_ENABLEW',
}
for n, base in enumerate((0x10000000, 0x10000800, 0x10001000, 0x10001800)):
    for off, reg in ((0x0, 'COUNT'), (0x10, 'MODE'), (0x20, 'COMP'), (0x30, 'HOLD')):
        EE[base + off] = f'T{n}_{reg}'
for vif, base in (('VIF0', 0x10003800), ('VIF1', 0x10003C00)):
    for i, reg in enumerate(('STAT', 'FBRST', 'ERR', 'MARK', 'CYCLE', 'MODE', 'NUM', 'MASK', 'CODE', 'ITOPS', 'BASE', 'OFST',
                             'TOPS', 'ITOP', 'TOP', '?', 'R0', 'R1', 'R2', 'R3', 'C0', 'C1', 'C2', 'C3')):
        EE[base + i * 0x10] = f'{vif}_{reg}'
DMA = {0x10008000: 'D0_VIF0', 0x10009000: 'D1_VIF1', 0x1000A000: 'D2_GIF', 0x1000B000: 'D3_fromIPU', 0x1000B400: 'D4_toIPU',
       0x1000C000: 'D5_SIF0', 0x1000C400: 'D6_SIF1', 0x1000C800: 'D7_SIF2', 0x1000D000: 'D8_fromSPR', 0x1000D400: 'D9_toSPR'}
for base, name in DMA.items():
    for off, reg in ((0, 'CHCR'), (0x10, 'MADR'), (0x20, 'QWC'), (0x30, 'TADR'), (0x40, 'ASR0'), (0x50, 'ASR1'), (0x80, 'SADR')):
        EE[base + off] = f'{name}_{reg}'
GS = {0x00: 'PMODE', 0x10: 'SMODE1', 0x20: 'SMODE2', 0x30: 'SRFSH', 0x40: 'SYNCH1', 0x50: 'SYNCH2', 0x60: 'SYNCV',
      0x70: 'DISPFB1', 0x80: 'DISPLAY1', 0x90: 'DISPFB2', 0xA0: 'DISPLAY2', 0xB0: 'EXTBUF', 0xC0: 'EXTDATA', 0xD0: 'EXTWRITE',
      0xE0: 'BGCOLOR', 0x1000: 'CSR', 0x1010: 'IMR', 0x1040: 'BUSDIR', 0x1080: 'SIGLBLID'}
for off, name in GS.items():
    EE[0x12000000 + off] = f'GS_{name}'
    EE[0x12000000 + off + 4] = f'GS_{name}.hi'

IOP = {0x1F801070: 'I_STAT', 0x1F801074: 'I_MASK', 0x1F801078: 'I_CTRL', 0x1F8010F0: 'DPCR', 0x1F8010F4: 'DICR',
       0x1F801570: 'DPCR2', 0x1F801574: 'DICR2', 0x1F801578: 'DMACEN', 0x1F80157C: 'DMACINTEN',
       0x1F801450: 'IOP_CONFIG?', 0x1F801460: 'SBUS?', 0x1F801010: 'BIOS_ROM_DELAY', 0x1F801014: 'SPU_DELAY',
       0x1F801400: 'SPU2_DELAY?', 0x1F801404: 'SPU2_DELAY2?', 0x1F8014C0: 'RTC?', 0x1F802070: 'POST'}
for ch, base in ((0, 0x1F801080), (1, 0x1F801090), (2, 0x1F8010A0), (3, 0x1F8010B0), (4, 0x1F8010C0), (5, 0x1F8010D0),
                 (6, 0x1F8010E0), (7, 0x1F801500), (8, 0x1F801510), (9, 0x1F801520), (10, 0x1F801530), (11, 0x1F801540),
                 (12, 0x1F801550)):
    for off, reg in ((0, 'MADR'), (4, 'BCR'), (8, 'CHCR'), (0xC, 'TADR')):
        IOP[base + off] = f'DMA{ch}_{reg}'
for t, base in enumerate((0x1F801100, 0x1F801110, 0x1F801120, 0x1F801480, 0x1F801490, 0x1F8014A0)):
    for off, reg in ((0, 'COUNT'), (4, 'MODE'), (8, 'TARGET')):
        IOP[base + off] = f'TMR{t}_{reg}'

VOICE_REGS = ['VOLL', 'VOLR', 'PITCH', 'ADSR1', 'ADSR2', 'ENVX', 'VOLXL', 'VOLXR']
VADDR_REGS = ['SSA.hi', 'SSA.lo', 'LSAX.hi', 'LSAX.lo', 'NAX.hi', 'NAX.lo']
CORE_REGS = {0x180: 'PMON.lo', 0x182: 'PMON.hi', 0x184: 'NON.lo', 0x186: 'NON.hi', 0x188: 'VMIXL.lo', 0x18A: 'VMIXL.hi',
             0x18C: 'VMIXEL.lo', 0x18E: 'VMIXEL.hi', 0x190: 'VMIXR.lo', 0x192: 'VMIXR.hi', 0x194: 'VMIXER.lo',
             0x196: 'VMIXER.hi', 0x198: 'MMIX', 0x19A: 'ATTR', 0x19C: 'IRQA.hi', 0x19E: 'IRQA.lo', 0x1A0: 'KON.lo',
             0x1A2: 'KON.hi', 0x1A4: 'KOFF.lo', 0x1A6: 'KOFF.hi', 0x1A8: 'TSA.hi', 0x1AA: 'TSA.lo', 0x1AC: 'DATA',
             0x1AE: 'CTRL', 0x1B0: 'ADMAS', 0x2E0: 'ESA.hi', 0x2E2: 'ESA.lo', 0x33C: 'EEA.hi', 0x340: 'ENDX.lo',
             0x342: 'ENDX.hi', 0x344: 'STATX'}
REVERB = ['APF1_SIZE', 'APF2_SIZE', 'SAME_L_DST', 'SAME_R_DST', 'COMB1_L_SRC', 'COMB1_R_SRC', 'COMB2_L_SRC',
          'COMB2_R_SRC', 'SAME_L_SRC', 'SAME_R_SRC', 'DIFF_L_DST', 'DIFF_R_DST', 'COMB3_L_SRC', 'COMB3_R_SRC',
          'COMB4_L_SRC', 'COMB4_R_SRC', 'DIFF_L_SRC', 'DIFF_R_SRC', 'APF1_L_DST', 'APF1_R_DST', 'APF2_L_DST', 'APF2_R_DST']
for i, n in enumerate(REVERB):
    CORE_REGS[0x2E4 + i * 4] = n + '.hi'
    CORE_REGS[0x2E6 + i * 4] = n + '.lo'
MASTER = ['MVOLL', 'MVOLR', 'EVOLL', 'EVOLR', 'AVOLL', 'AVOLR', 'BVOLL', 'BVOLR', 'MVOLXL', 'MVOLXR', 'IIR_VOL', 'COMB1_VOL',
          'COMB2_VOL', 'COMB3_VOL', 'COMB4_VOL', 'WALL_VOL', 'APF1_VOL', 'APF2_VOL', 'IN_COEF_L', 'IN_COEF_R']


def spu2_name(off):
    if off >= 0x760 and off < 0x760 + 2 * 0x28:
        c, r = divmod(off - 0x760, 0x28)
        return f'C{c}_{MASTER[r // 2] if r // 2 < len(MASTER) else hex(r)}'
    if off == 0x7C0:
        return 'SPDIF_OUT'
    if off == 0x7C2:
        return 'SPDIF_IRQINFO'
    if off == 0x7C6:
        return 'SPDIF_MODE'
    if off == 0x7C8:
        return 'SPDIF_MEDIA'
    if off == 0x7CA:
        return 'SPDIF_COPY'
    c, local = divmod(off, 0x400)
    if c > 1:
        return f'?{off:03X}'
    if local < 0x180:
        return f'C{c}_V*_{VOICE_REGS[(local & 0xF) >> 1]}'
    if 0x1C0 <= local < 0x1C0 + 24 * 0xC:
        return f'C{c}_V*_{VADDR_REGS[((local - 0x1C0) % 0xC) >> 1]}'
    return f'C{c}_{CORE_REGS.get(local, hex(local))}'


def load(paths):
    ee, iop = {}, {}
    for p in paths:
        target = iop if p.endswith('.iop.csv') else ee
        for line in open(p, encoding='utf-8').read().splitlines()[1:]:
            f = line.split(',')
            addr = int(f[0], 16)
            reads, writes, orb, andb = int(f[1]), int(f[2]), int(f[3], 16), int(f[4], 16)
            e = target.setdefault(addr, [0, 0, 0, ~0 & ((1 << 64) - 1), 0, set()])
            e[0] += reads
            e[1] += writes
            if writes:
                e[2] |= orb
                e[3] &= andb
            e[4] = int(f[5], 16)
            e[5].add(os.path.basename(p).split('.csv')[0])
    return ee, iop


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    paths = []
    for a in args:
        paths += glob.glob(os.path.join(a, '*.csv')) if os.path.isdir(a) else [a]
    ee, iop = load(paths)
    runs = len({p.split('.csv')[0] for p in paths})
    want = {x for x in ('--ee', '--iop', '--spu2') if x in sys.argv} or {'--ee', '--iop', '--spu2'}
    if '--ee' in want:
        print(f'== EE I/O and GS privileged registers ({len(ee)}, {runs} runs)')
        for a in sorted(ee):
            r, w, orb, andb, last, rs = ee[a]
            print(f'  {a:08X} {EE.get(a, "?"):16s} reads {r:9d} writes {w:8d} or {orb:016X} and {andb if w else 0:016X} last {last:016X} runs {len(rs)}')
    if '--iop' in want:
        hw = {a: v for a, v in iop.items() if not (0x1F900000 <= a < 0x1F900800)}
        print(f'== IOP hardware registers ({len(hw)})')
        for a in sorted(hw):
            r, w, orb, andb, last, rs = hw[a]
            print(f'  {a:08X} {IOP.get(a, "?"):16s} reads {r:9d} writes {w:8d} or {orb:08X} and {andb if w else 0:08X} last {last:08X} runs {len(rs)}')
    if '--spu2' in want:
        sp = {a: v for a, v in iop.items() if 0x1F900000 <= a < 0x1F900800}
        agg = {}
        for a, v in sp.items():
            name = spu2_name(a - 0x1F900000)
            e = agg.setdefault(name, [0, 0, 0, (1 << 64) - 1, 0, 0])
            e[0] += v[0]
            e[1] += v[1]
            if v[1]:
                e[2] |= v[2]
                e[3] &= v[3]
            e[5] += 1
        print(f'== SPU2 registers ({len(sp)} addresses, {len(agg)} names; voice registers summed over voices)')
        for name in sorted(agg):
            r, w, orb, andb, _, n = agg[name]
            print(f'  {name:22s} reads {r:9d} writes {w:8d} or {orb & 0xFFFF:04X} and {(andb if w else 0) & 0xFFFF:04X} addrs {n}')


if __name__ == '__main__':
    main()
