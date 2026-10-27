"""Draw passes of a GS capture (DOD_GS_CAPTURE) in order: consecutive draws with the same GS state are one line
(count, primitive, context, texture, frame / Z buffer, test, alpha, x / y range), with the local-to-local and
host-to-local transfers (BITBLTBUF / TRXPOS / TRXREG / TRXDIR) between them. For comparing a room that draws wrong
with one that draws right.
Usage: python -I gif_passes.py capture.bin [field]"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gif_analyze as ga  # noqa: E402


class Passes(ga.Gs):
    def __init__(self):
        super().__init__()
        self.seq = []

    def write(self, addr, value):
        super().write(addr, value)
        if addr == 0x53:  # TRXDIR
            b = self.reg[0x50]
            pos = self.reg[0x51]
            reg = self.reg[0x52]
            d = value & 3
            kind = ['host->local', 'local->host', 'local->local', 'off'][d]
            self.seq.append([1, f'TRX {kind} src sbp{b & 0x3FFF:X} sbw{(b >> 16) & 0x3F} spsm{(b >> 24) & 0x3F:X} '
                                f'-> dbp{(b >> 32) & 0x3FFF:X} dbw{(b >> 48) & 0x3F} dpsm{(b >> 56) & 0x3F:X} '
                                f'src {pos & 0x7FF},{(pos >> 16) & 0x7FF} dst {(pos >> 32) & 0x7FF},{(pos >> 48) & 0x7FF} '
                                f'size {reg & 0xFFF}x{(reg >> 32) & 0xFFF}', None])

    def kick(self, addr, value):
        before = len(self.order)
        super().kick(addr, value)
        key = self.order[-1] if len(self.order) > before else None
        if key is None:
            # an existing key: find it from the last state written
            prim = self.reg[0x00] if (self.reg[0x1A] & 1) else (self.reg[0x00] & 7) | (self.reg[0x1B] & ~7)
            ctxt = (prim >> 9) & 1
            tme, fge, abe = (prim >> 4) & 1, (prim >> 5) & 1, (prim >> 6) & 1
            tex0 = self.reg[0x06 + ctxt]
            frame = self.reg[0x4C + ctxt]
            zbuf = self.reg[0x4E + ctxt]
            test = self.reg[0x47 + ctxt]
            alpha = self.reg[0x42 + ctxt]
            key = (ga.PRIM_TYPES[prim & 7], ctxt, tme, fge, abe,
                   f'tbp{tex0 & 0x3FFF:X} psm{(tex0 >> 20) & 0x3F:X} tfx{(tex0 >> 35) & 3}' if tme else '-',
                   f'fbp{frame & 0x1FF:X} fbmsk{frame >> 32:08X}', f'zbp{zbuf & 0x1FF:X} zmsk{(zbuf >> 32) & 1}',
                   f'test{test:05X}', f'alpha{alpha:010X}' if abe else '-')
        x, y = (value & 0xFFFF) / 16.0, ((value >> 16) & 0xFFFF) / 16.0
        if self.seq and self.seq[-1][2] == key:
            e = self.seq[-1]
            e[0] += 1
            e[3] = [min(e[3][0], x), max(e[3][1], x), min(e[3][2], y), max(e[3][3], y)]
        else:
            tex0 = self.reg[0x06 + key[1]]
            extra = f' tw{(tex0 >> 26) & 0xF} th{(tex0 >> 30) & 0xF} tbw{(tex0 >> 14) & 0x3F}' if key[2] else ''
            frame = self.reg[0x4C + key[1]]
            extra += f' fbw{(frame >> 16) & 0x3F} fpsm{(frame >> 24) & 0x3F:X}'
            ofs = self.reg[0x18 + key[1]]
            extra += f' ofs{(ofs & 0xFFFF) / 16:.1f},{((ofs >> 32) & 0xFFFF) / 16:.1f}'
            sc = self.reg[0x40 + key[1]]
            extra += f' sc{sc & 0x7FF}-{(sc >> 16) & 0x7FF},{(sc >> 32) & 0x7FF}-{(sc >> 48) & 0x7FF}'
            self.seq.append([1, ' '.join(str(k) for k in key) + extra, key, [x, x, y, y]])


def main():
    path = sys.argv[1]
    want = int(sys.argv[2]) if len(sys.argv) > 2 else None
    data = open(path, 'rb').read()
    pos = 0
    field = None
    gs = None
    conts = {}
    while pos + 8 <= len(data):
        p, n = struct.unpack_from('<II', data, pos)
        pos += 8
        if p == 0xFFFFFFFF:
            if gs is not None and (want is None or field == want):
                dump(field, gs)
            field = n
            gs = Passes()
            conts = {}
            continue
        if gs is not None:
            ga.parse_packet(gs, data[pos:pos + n], conts.setdefault(p, {}))
        pos += n
    if gs is not None and (want is None or field == want):
        dump(field, gs)


def dump(field, gs):
    print(f'== field {field}: {len(gs.seq)} passes')
    for e in gs.seq:
        if e[2] is None:
            print(f'       {e[1]}')
        else:
            r = e[3]
            print(f'{e[0]:6d} {e[1]} x {r[0]:.0f}-{r[1]:.0f} y {r[2]:.0f}-{r[3]:.0f}')


if __name__ == '__main__':
    main()
