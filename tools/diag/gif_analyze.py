"""Summarise a GS capture (DOD_GS_CAPTURE): draws grouped by GS state, with vertex colour / fog / Q statistics.
Capture records: u32 path, u32 bytes, data[bytes]; path 0xFFFFFFFF marks a field (bytes = field number).
Usage: gif_analyze.py capture.bin [field_lo field_hi]"""
import struct
import sys
from collections import defaultdict

PACKED_NAMES = {0x0: 'PRIM', 0x1: 'RGBAQ', 0x2: 'ST', 0x3: 'UV', 0x4: 'XYZF2', 0x5: 'XYZ2', 0x6: 'TEX0_1', 0x7: 'TEX0_2',
                0x8: 'CLAMP_1', 0x9: 'CLAMP_2', 0xA: 'FOG', 0xC: 'XYZF3', 0xD: 'XYZ3', 0xE: 'A+D', 0xF: 'NOP'}
PRIM_TYPES = ['POINT', 'LINE', 'LSTRIP', 'TRI', 'TSTRIP', 'TFAN', 'SPRITE', 'BAD']


class Gs:
    def __init__(self):
        self.reg = defaultdict(int)
        self.reg[0x1A] = 1
        self.q = 1.0
        self.stats = {}
        self.order = []

    def write(self, addr, value):
        self.reg[addr] = value
        if addr in (0x04, 0x05, 0x0C, 0x0D):
            self.kick(addr, value)

    def kick(self, addr, value):
        # PRMODECONT.AC = 1 (reset value): attributes from PRIM; 0: from PRMODE (the type always from PRIM).
        prim = self.reg[0x00] if (self.reg[0x1A] & 1) else (self.reg[0x00] & 7) | (self.reg[0x1B] & ~7)
        ctxt = (prim >> 9) & 1
        tme, fge, abe = (prim >> 4) & 1, (prim >> 5) & 1, (prim >> 6) & 1
        tex0 = self.reg[0x06 + ctxt]
        frame = self.reg[0x4C + ctxt]
        zbuf = self.reg[0x4E + ctxt]
        test = self.reg[0x47 + ctxt]
        alpha = self.reg[0x42 + ctxt]
        key = (PRIM_TYPES[prim & 7], ctxt, tme, fge, abe,
               f'tbp{tex0 & 0x3FFF:X} psm{(tex0 >> 20) & 0x3F:X} tfx{(tex0 >> 35) & 3}' if tme else '-',
               f'fbp{frame & 0x1FF:X} fbmsk{frame >> 32:08X}', f'zbp{zbuf & 0x1FF:X} zmsk{(zbuf >> 32) & 1}',
               f'test{test:05X}', f'alpha{alpha:010X}' if abe else '-')
        s = self.stats.get(key)
        if s is None:
            s = self.stats[key] = {'n': 0, 'rgba_min': [255] * 4, 'rgba_max': [0] * 4, 'rgba_sum': [0] * 4,
                                   'fog_min': 255, 'fog_max': 0, 'z_min': 1 << 32, 'z_max': 0, 'x': [1 << 16, 0],
                                   'y': [1 << 16, 0]}
            self.order.append(key)
        rgbaq = self.reg[0x01]
        c = [(rgbaq >> (8 * i)) & 0xFF for i in range(4)]
        s['n'] += 1
        for i in range(4):
            s['rgba_min'][i] = min(s['rgba_min'][i], c[i])
            s['rgba_max'][i] = max(s['rgba_max'][i], c[i])
            s['rgba_sum'][i] += c[i]
        if addr in (0x04, 0x0C):
            fog = (value >> 56) & 0xFF
            z = (value >> 32) & 0xFFFFFF
        else:
            fog = (self.reg[0x0A] >> 56) & 0xFF
            z = value >> 32
        s['fog_min'] = min(s['fog_min'], fog)
        s['fog_max'] = max(s['fog_max'], fog)
        s['z_min'] = min(s['z_min'], z)
        s['z_max'] = max(s['z_max'], z)
        x, y = (value & 0xFFFF) / 16.0, ((value >> 16) & 0xFFFF) / 16.0
        s['x'] = [min(s['x'][0], x), max(s['x'][1], x)]
        s['y'] = [min(s['y'][0], y), max(s['y'][1], y)]


def parse_packet(gs, data, cont):
    """cont: pending tag state for PATH continuation (dict) -> returns updated state."""
    pos = 0
    n = len(data)
    while pos + 16 <= n:
        if cont.get('left', 0) == 0:
            lo, hi = struct.unpack_from('<QQ', data, pos)
            pos += 16
            nloop = lo & 0x7FFF
            pre = (lo >> 46) & 1
            prim = (lo >> 47) & 0x7FF
            flg = (lo >> 58) & 3
            nreg = (lo >> 60) & 0xF or 16
            regs = [(hi >> (4 * i)) & 0xF for i in range(nreg)]
            if pre and flg == 0:
                gs.write(0x00, prim)
            if flg == 2:
                cont.update(left=nloop, flg=2)
            elif flg == 0:
                cont.update(left=nloop * nreg, flg=0, regs=regs, nreg=nreg, idx=0)
            elif flg == 1:
                total = nloop * nreg
                cont.update(left=(total + 1) // 2, flg=1, regs=regs, nreg=nreg, idx=0, total=total)
            else:
                cont['left'] = 0
            continue
        if cont['flg'] == 2:
            pos += 16
            cont['left'] -= 1
            continue
        if cont['flg'] == 0:
            lo, hi = struct.unpack_from('<QQ', data, pos)
            pos += 16
            r = cont['regs'][cont['idx'] % cont['nreg']]
            cont['idx'] += 1
            cont['left'] -= 1
            if r == 0x0:
                gs.write(0x00, lo & 0x7FF)
            elif r == 0x1:
                col = (lo & 0xFF) | ((lo >> 32 & 0xFF) << 8) | ((hi & 0xFF) << 16) | ((hi >> 32 & 0xFF) << 24)
                gs.write(0x01, col | (gs.reg[0x01] & (0xFFFFFFFF << 32)))
            elif r == 0x2:
                gs.write(0x02, lo)
            elif r == 0x3:
                # PACKED UV: U in bits 0-13, V in bits 32-45; the UV register has V in bits 16-29
                gs.write(0x03, (lo & 0x3FFF) | (((lo >> 32) & 0x3FFF) << 16))
            elif r in (0x4, 0x5):
                x, y = lo & 0xFFFF, (lo >> 32) & 0xFFFF
                if r == 0x4:
                    z = (hi >> 4) & 0xFFFFFF
                    f = (hi >> 36) & 0xFF
                    v = x | (y << 16) | (z << 32) | (f << 56)
                else:
                    z = hi & 0xFFFFFFFF
                    v = x | (y << 16) | (z << 32)
                adc = (hi >> 47) & 1
                gs.write((0x04 if r == 0x4 else 0x05) if not adc else (0x0C if r == 0x4 else 0x0D), v)
            elif r in (0x6, 0x7, 0x8, 0x9):
                gs.write(r, lo)
            elif r == 0xA:
                gs.write(0x0A, ((hi >> 36) & 0xFF) << 56)
            elif r == 0xE:
                gs.write(hi & 0xFF, lo)
            continue
        # REGLIST: 64-bit words
        for half in range(2):
            if cont['idx'] >= cont['total']:
                break
            v = struct.unpack_from('<Q', data, pos + 8 * half)[0]
            r = cont['regs'][cont['idx'] % cont['nreg']]
            cont['idx'] += 1
            if r not in (0xE, 0xF):
                gs.write(r if r not in (0x4, 0x5) else r, v)
        pos += 16
        cont['left'] -= 1
    return cont


def main():
    path = sys.argv[1]
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 62
    data = open(path, 'rb').read()
    gs = Gs()
    conts = defaultdict(dict)
    pos = 0
    field = 0
    bytes_by_path = defaultdict(int)
    while pos + 8 <= len(data):
        p, size = struct.unpack_from('<II', data, pos)
        pos += 8
        if p == 0xFFFFFFFF:
            field = size
            continue
        chunk = data[pos:pos + size]
        pos += size
        if not (lo <= field <= hi):
            continue
        bytes_by_path[p] += size
        conts[p] = parse_packet(gs, chunk, conts[p])
    print('bytes per path', dict(bytes_by_path))
    rows = sorted(gs.order, key=lambda k: -gs.stats[k]['n'])
    for key in rows[:60]:
        s = gs.stats[key]
        avg = [round(v / s['n']) for v in s['rgba_sum']]
        print(f"{s['n']:7d} {key[0]:6s} c{key[1]} tme{key[2]} fge{key[3]} abe{key[4]} {key[5]:28s} {key[6]:22s} {key[7]:16s} "
              f"{key[8]} {key[9]} rgba min{s['rgba_min']} max{s['rgba_max']} avg{avg} fog {s['fog_min']}-{s['fog_max']} "
              f"z {s['z_min']:X}-{s['z_max']:X} x {s['x'][0]:.0f}-{s['x'][1]:.0f} y {s['y'][0]:.0f}-{s['y'][1]:.0f}")


if __name__ == '__main__':
    main()
