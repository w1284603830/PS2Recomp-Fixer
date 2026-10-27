"""Offline inventory of the SDK functions the recompiler replaces with runtime HLE (the [general].stubs of a TOML):
for each, the guest call sites in the ELF (jal count), the runtime implementation (file, body lines) and a rough
shape: TODO (throws), RETURN (only sets a return value), SHORT (< 6 statements) or FULL. Stubs that the game calls
but that are TODO / RETURN / SHORT are the candidates for "an empty shell that does not report" (sceGsResetPath, row
102) and must be compared with the original function (diag/ee_dis.py).

Usage: python -I hle_inventory.py <toml> <elf> <PS2Recomp root> [--all]
"""
import os
import re
import struct
import sys

sys.stdout.reconfigure(encoding='utf-8')
toml_path, elf_path, root = sys.argv[1], sys.argv[2], sys.argv[3]
show_all = '--all' in sys.argv

text = open(toml_path, encoding='utf-8').read()
m = re.search(r'stubs\s*=\s*\[(.*?)\]', text, re.S)
stubs = re.findall(r'"([A-Za-z0-9_]+)@0x([0-9A-Fa-f]+)"', m.group(1))

elf = open(elf_path, 'rb').read()
LOAD_VADDR, LOAD_OFFSET, CODE_END = 0x100000, 0x100, 0x3FEC88
jal_count = {}
targets = {int(a, 16) for _, a in stubs}
for a in range(LOAD_VADDR, CODE_END, 4):
    w = struct.unpack_from('<I', elf, a - LOAD_VADDR + LOAD_OFFSET)[0]
    if (w >> 26) == 3:
        t = ((w & 0x3FFFFFF) << 2) | (a & 0xF0000000)
        if t in targets:
            jal_count[t] = jal_count.get(t, 0) + 1
# function pointers in data (address words equal to the stub address)
ptr_count = {}
for a in range(0, len(elf) - 4, 4):
    v = struct.unpack_from('<I', elf, a)[0]
    if v in targets:
        ptr_count[v] = ptr_count.get(v, 0) + 1

# runtime bodies
src_dirs = [os.path.join(root, 'ps2xRuntime', 'src', 'lib', 'Kernel')]
bodies = {}
sig = re.compile(r'^\s*void\s+([A-Za-z0-9_]+)\s*\(\s*uint8_t\s*\*\s*rdram\s*,\s*R5900Context\s*\*\s*ctx\s*,\s*PS2Runtime\s*\*\s*runtime\s*\)\s*$', re.M)
for d in src_dirs:
    for dirpath, _, files in os.walk(d):
        for fn in files:
            if not fn.endswith('.cpp'):
                continue
            p = os.path.join(dirpath, fn)
            s = open(p, encoding='utf-8', errors='replace').read()
            for mm in sig.finditer(s):
                name = mm.group(1)
                i = s.find('{', mm.end())
                depth, j = 0, i
                while j < len(s):
                    if s[j] == '{':
                        depth += 1
                    elif s[j] == '}':
                        depth -= 1
                        if depth == 0:
                            break
                    j += 1
                body = s[i + 1:j]
                code = re.sub(r'//[^\n]*', '', body)
                code = re.sub(r'/\*.*?\*/', '', code, flags=re.S)
                stmts = [x for x in code.split(';') if x.strip()]
                if 'TODO_NAMED' in code or re.search(r'\bTODO\s*\(', code):
                    shape = 'TODO'
                elif all(re.search(r'setReturn|return\b|\(void\)', x) for x in stmts) and len(stmts) <= 2:
                    shape = 'RETURN'
                elif len(stmts) < 6:
                    shape = 'SHORT'
                else:
                    shape = 'FULL'
                bodies[name] = (os.path.relpath(p, root).replace('\\', '/'), len(body.splitlines()), shape)

rows = []
for name, addr in stubs:
    a = int(addr, 16)
    calls, ptrs = jal_count.get(a, 0), ptr_count.get(a, 0)
    file, lines, shape = bodies.get(name, ('-', 0, 'MISSING'))
    rows.append((name, a, calls, ptrs, shape, lines, file))

order = {'MISSING': 0, 'TODO': 1, 'RETURN': 2, 'SHORT': 3, 'FULL': 4}
rows.sort(key=lambda r: (order[r[4]], -(r[2] + r[3]), r[0]))
print(f'{len(rows)} stubs in {os.path.basename(toml_path)}')
counts = {}
for r in rows:
    counts[r[4]] = counts.get(r[4], 0) + 1
print('shapes: ' + ', '.join(f'{k} {v}' for k, v in sorted(counts.items(), key=lambda x: order[x[0]])))
print(f'{"name":36s} {"addr":>8s} {"jal":>4s} {"ptr":>4s} {"shape":7s} {"lines":>5s} file')
for name, a, calls, ptrs, shape, lines, file in rows:
    if show_all or shape != 'FULL' or (calls + ptrs) == 0:
        print(f'{name:36s} {a:08X} {calls:4d} {ptrs:4d} {shape:7s} {lines:5d} {file}')
