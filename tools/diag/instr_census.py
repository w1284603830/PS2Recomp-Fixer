"""Instruction census of the generated code: every '// 0xADDR: 0xWORD  mnemonic operands' comment in the runner's
recompiled .cpp files (deduplicated by address), with the C++ the recompiler emitted for it. Per mnemonic: how many
instructions, and the distinct emitted shapes (register numbers and immediates normalised), so each translation can be
checked against the reference (PCSX2's interpreter).

Usage: python -I instr_census.py <runner dir> <out.txt> [--shapes N]
"""
import os
import re
import sys
from collections import defaultdict

sys.stdout.reconfigure(encoding='utf-8')
src, out_path = sys.argv[1], sys.argv[2]
max_shapes = int(sys.argv[sys.argv.index('--shapes') + 1]) if '--shapes' in sys.argv else 4

head = re.compile(r'^\s*// 0x([0-9a-f]+): 0x([0-9a-f]+)\s+(\S+)\s*(.*)$')
seen = set()
count = defaultdict(int)
shapes = defaultdict(lambda: defaultdict(int))
sample = {}


def normalise(code):
    code = re.sub(r'ctx->pc = 0x[0-9a-f]+u;', '', code)
    code = re.sub(r'\b0x[0-9a-fA-F]+u?\b', 'K', code)
    code = re.sub(r'(?<![A-Za-z_])\d+(?![A-Za-z_])', 'N', code)
    return re.sub(r'\s+', ' ', code).strip()


files = [f for f in os.listdir(src) if f.endswith('.cpp')]
for n, fn in enumerate(files):
    with open(os.path.join(src, fn), encoding='utf-8', errors='replace') as f:
        lines = f.read().splitlines()
    cur = None
    body = []

    def flush():
        if cur is None:
            return
        addr, word, mnem, ops = cur
        if addr in seen:
            return
        seen.add(addr)
        count[mnem] += 1
        code = ' '.join(body)
        shp = normalise(code)
        shapes[mnem][shp] += 1
        if shp not in sample:
            sample[shp] = f'{addr:08X} {mnem} {ops} => {code.strip()[:400]}'

    for line in lines:
        m = head.match(line)
        if m:
            flush()
            cur = (int(m.group(1), 16), m.group(2), m.group(3), m.group(4))
            body = []
        elif cur is not None:
            s = line.strip()
            if s.startswith('case ') or s.startswith('label_') or s == '}' or s.startswith('//'):
                if s.startswith('label_') or s.startswith('//'):
                    continue
                flush()
                cur = None
                continue
            body.append(s)
    flush()
    if n % 5000 == 0:
        print(f'{n}/{len(files)} files, {len(seen)} instructions', file=sys.stderr)

with open(out_path, 'w', encoding='utf-8') as o:
    o.write(f'{len(seen)} instructions, {len(count)} mnemonics\n\n')
    for mnem in sorted(count, key=lambda k: -count[k]):
        sh = shapes[mnem]
        o.write(f'{mnem:10s} {count[mnem]:7d}  shapes {len(sh)}\n')
        for shp, c in sorted(sh.items(), key=lambda x: -x[1])[:max_shapes]:
            o.write(f'    [{c:6d}] {sample[shp]}\n')
print(f'{len(seen)} instructions, {len(count)} mnemonics -> {out_path}')
