"""Find switch jump tables in SLPM_662.75 whose case targets have no record in the function map.

A case target that is neither a function start nor an entry label is not registered in the runtime, so the switch's
`jr` ends in "[guest-branch:missing-target]" and the guest thread stops (the disc-change state machine at 0x2AC760,
table 0x671B40, hung this way). Ghidra exported caseD_N functions only for some cases.

Pattern (GCC):  sltiu at, rI, N / beqz at, default / sll rO, rI, 2 / lui rT, hi / addiu rT, rT, lo / addu rP, rO, rT /
lw rX, off(rP) / jr rX  (any order of the lui/addiu/sll, the lui may sit in a delay slot).

Each missing target is appended as an entry label record (entry_XXXXXXXX) ending where the smallest record that holds
it ends, the way Ghidra's labels are exported; run the recompiler on the new CSV.

Usage: python -I scan_jump_tables.py <elf> <in.csv> [<out.csv>]
"""
import csv
import struct
import sys

LOAD_VADDR, LOAD_OFFSET = 0x100000, 0x100
CODE_START, CODE_END = 0x100000, 0x3FEC88
LOOKBACK = 16


def main():
    elf, src = sys.argv[1], sys.argv[2]
    dst = sys.argv[3] if len(sys.argv) > 3 else None
    image = open(elf, "rb").read()

    def word(addr):
        return struct.unpack_from("<I", image, addr - LOAD_VADDR + LOAD_OFFSET)[0]

    with open(src, newline="") as f:
        reader = csv.DictReader(f)
        fields = reader.fieldnames
        rows = list(reader)
    records = [(int(r["Start"], 16), int(r["End"], 16)) for r in rows]
    starts = {s for s, _ in records}

    def holder_end(addr):
        best = None
        for s, e in records:
            if s <= addr < e and (best is None or e - s < best[1] - best[0]):
                best = (s, e)
        return best[1] if best else None

    missing = {}
    tables = 0
    unbounded = []
    for j in range(CODE_START, CODE_END, 4):
        w = word(j)
        if w >> 26 != 0 or w & 0x3F != 8:
            continue
        rx = (w >> 21) & 31
        if rx == 31:
            continue
        # lw rX, off(rP)
        lw_at = None
        for a in range(j - 4, j - 4 * LOOKBACK, -4):
            y = word(a)
            if y >> 26 == 0x23 and (y >> 16) & 31 == rx:
                lw_at = a
                break
        if lw_at is None:
            continue
        y = word(lw_at)
        rp, off = (y >> 21) & 31, y & 0xFFFF
        off = off - 0x10000 if off & 0x8000 else off
        # addu rP, rA, rB
        sources = None
        for a in range(lw_at - 4, lw_at - 4 * LOOKBACK, -4):
            y = word(a)
            if y >> 26 == 0 and y & 0x3F in (0x21, 0x2D) and (y >> 11) & 31 == rp:
                sources = ((y >> 21) & 31, (y >> 16) & 31)
                break
        if sources is None:
            continue
        # lui rT, hi / addiu rT, rT, lo for one of the sources
        # (the other source is the scaled index; a lui of it further back is unrelated, so keep the source whose
        # lui/addiu gives an address in the initialised data)
        base = None
        for rt in sources:
            hi = lo = None
            for a in range(lw_at - 4, lw_at - 4 * LOOKBACK * 2, -4):
                y = word(a)
                if lo is None and y >> 26 == 9 and (y >> 16) & 31 == rt and (y >> 21) & 31 == rt:
                    lo = y & 0xFFFF
                    lo = lo - 0x10000 if lo & 0x8000 else lo
                elif y >> 26 == 15 and (y >> 16) & 31 == rt:
                    hi = y & 0xFFFF
                    break
            if hi is not None:
                candidate = (hi << 16) + (lo or 0) + off
                if CODE_END <= candidate < LOAD_VADDR - LOAD_OFFSET + len(image) - 4:
                    base = candidate
                    break
        if base is None:
            continue
        # bound: sltiu at, rI, N where sll index_reg, rI, 2
        count = None
        for a in range(j - 4, j - 4 * LOOKBACK * 2, -4):
            y = word(a)
            if y >> 26 == 11:  # sltiu
                count = y & 0xFFFF
                break
        if count is None or count == 0 or count > 512:
            unbounded.append((j, base))
            continue
        tables += 1
        for i in range(count):
            if base + 4 * i - LOAD_VADDR + LOAD_OFFSET + 4 > len(image):
                break
            t = word(base + 4 * i)
            if not (CODE_START <= t < CODE_END):
                break
            if t not in starts:
                missing.setdefault(t, set()).add((j, base))

    print(f"switch tables: {tables}, without a bound: {len(unbounded)}, case targets without a record: {len(missing)}")
    for j, base in unbounded[:20]:
        print(f"  no bound: jr at 0x{j:06X} table 0x{base:06X}")
    new_rows = []
    for t in sorted(missing):
        end = holder_end(t)
        users = ", ".join(f"jr 0x{j:06X} table 0x{b:06X}" for j, b in sorted(missing[t]))
        print(f"  0x{t:06X} (holder ends 0x{end:06X}) <- {users}" if end else f"  0x{t:06X} (no holder) <- {users}")
        if end:
            new_rows.append({"Name": f"entry_{t:08x}", "Start": f"0x{t:08X}", "End": f"0x{end:08X}", "Size": str(end - t)})
    if dst:
        # The existing rows keep their order; each new row goes before the first row that starts after it.
        out = []
        pending = sorted(new_rows, key=lambda r: int(r["Start"], 16))
        for r in rows:
            while pending and int(pending[0]["Start"], 16) < int(r["Start"], 16):
                out.append(pending.pop(0))
            out.append(r)
        out.extend(pending)
        with open(dst, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fields)
            writer.writeheader()
            for r in out:
                writer.writerow({k: r[k] for k in fields})
        print(f"wrote {dst}: {len(new_rows)} entry labels added")


if __name__ == "__main__":
    main()
