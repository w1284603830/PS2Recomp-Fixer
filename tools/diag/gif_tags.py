"""GIF tags of a GS capture (DOD_GS_CAPTURE) per path, with the A+D writes of FRAME / ZBUF / TEX0 that look wrong
(a PSM the GS does not have, or a buffer width over 1024 pixels) - to find where the stream the GS gets stops being
GIF packets. Tag state is kept per path across records, as the GS does.
Usage: python -I gif_tags.py capture.bin <first field> [last field] [--all]"""
import struct
import sys

VALID_PSM = {0x00, 0x01, 0x02, 0x0A, 0x09, 0x13, 0x14, 0x1B, 0x24, 0x2C, 0x30, 0x31, 0x32, 0x3A}
NAMES = {0x4C: 'FRAME_1', 0x4D: 'FRAME_2', 0x4E: 'ZBUF_1', 0x4F: 'ZBUF_2', 0x06: 'TEX0_1', 0x07: 'TEX0_2'}


def suspicious(addr, v):
    if addr in (0x4C, 0x4D):
        return ((v >> 24) & 0x3F) not in VALID_PSM or ((v >> 16) & 0x3F) > 16
    if addr in (0x4E, 0x4F):
        return (((v >> 24) & 0xF) | 0x30) not in VALID_PSM
    if addr in (0x06, 0x07):
        return ((v >> 20) & 0x3F) not in VALID_PSM or ((v >> 14) & 0x3F) > 16
    return False


def main():
    path = sys.argv[1]
    lo_field = int(sys.argv[2])
    hi_field = int(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith('--') else lo_field
    show_all = '--all' in sys.argv
    data = open(path, 'rb').read()
    pos = 0
    field = None
    state = {}
    record = 0
    while pos + 8 <= len(data):
        p, n = struct.unpack_from('<II', data, pos)
        pos += 8
        if p == 0xFFFFFFFF:
            field = n
            continue
        rec = data[pos:pos + n]
        pos += n
        record += 1
        st = state.setdefault(p, {'left': 0})
        show = field is not None and lo_field <= field <= hi_field
        q = 0
        while q + 16 <= len(rec):
            lo, hi = struct.unpack_from('<QQ', rec, q)
            if st['left'] == 0:
                nloop = lo & 0x7FFF
                eop = (lo >> 15) & 1
                pre = (lo >> 46) & 1
                prim = (lo >> 47) & 0x7FF
                flg = (lo >> 58) & 3
                nreg = (lo >> 60) & 0xF or 16
                regs = [(hi >> (4 * i)) & 0xF for i in range(nreg)]
                st.update(flg=flg, nreg=nreg, regs=regs, idx=0)
                st['left'] = nloop * nreg if flg == 0 else (nloop * nreg + 1) // 2 if flg == 1 else nloop if flg == 2 else 0
                if show and (show_all or flg != 0 or nloop > 4096):
                    print(f'f{field} rec{record} path{p} q{q // 16:5d} TAG nloop {nloop} eop {eop} pre {pre} prim {prim:03X} '
                          f'{["PACKED", "REGLIST", "IMAGE", "DISABLE"][flg]} nreg {nreg} regs {"".join(f"{r:X}" for r in regs)}')
                q += 16
                continue
            if st['flg'] == 0:
                r = st['regs'][st['idx'] % st['nreg']]
                st['idx'] += 1
                if r == 0xE:
                    addr = hi & 0xFF
                    if show and (suspicious(addr, lo) or (show_all and addr in NAMES)):
                        print(f'f{field} rec{record} path{p} q{q // 16:5d}   A+D {NAMES.get(addr, hex(addr))} = {lo:016X}'
                              + ('  <-- suspicious' if suspicious(addr, lo) else ''))
            st['left'] -= 1
            q += 16


if __name__ == '__main__':
    main()
