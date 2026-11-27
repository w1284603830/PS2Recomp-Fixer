"""Count AVX2/FMA/BMI and AVX (VEX) instructions in a PE via dumpbin /disasm (streamed).

  isa_scan.py <exe-or-dll> <out.json>

Counts are an upper bound: data decoded as code and CPU-dispatched paths (CRT, MSVC auto-vectoriser
`cmp [__isa_available],N`) are included; inspect the listed addresses before calling a hit unguarded.
"""
import os
import re, subprocess, sys, json, collections
DUMPBIN = os.environ.get('DUMPBIN', 'dumpbin.exe')  # set DUMPBIN or run from a VS developer prompt
AVX2 = re.compile(r'^(vpbroadcast|vbroadcasti128|vperm2i128|vpermq|vpermd|vpermps|vinserti128|vextracti128|vpgather|vgather|vpmaskmov|vpsllv|vpsrlv|vpsrav)')
FMA = re.compile(r'^vf(n?)m(add|sub)')
BMI = re.compile(r'^(andn|bextr|blsi|blsmsk|blsr|shlx|shrx|sarx|rorx|bzhi|pdep|pext|mulx)$')
line_re = re.compile(r'^\s+([0-9A-F]{16}): (?:[0-9A-F]{2} )+\s+(\S+)\s*(.*)$')
counts = collections.Counter(); first = {}
proc = subprocess.Popen([DUMPBIN, '/nologo', '/disasm:nobytes', sys.argv[1]], stdout=subprocess.PIPE, text=True, errors='replace')
line_re = re.compile(r'^\s+([0-9A-F]{16}): (\S+)\s*(.*)$')
for line in proc.stdout:
    m = line_re.match(line)
    if not m: continue
    addr, mn, ops = m.groups()
    kind = None
    if AVX2.match(mn) or (mn.startswith('vp') and 'ymm' in ops): kind = 'avx2'
    elif FMA.match(mn): kind = 'fma'
    elif BMI.match(mn): kind = 'bmi'
    elif mn.startswith('v') and mn not in ('verr','verw'): kind = 'avx'
    if kind:
        counts[kind] += 1
        first.setdefault(kind, []).append(addr) if len(first.get(kind, [])) < 2000 else None
proc.wait()
json.dump({'counts': counts, 'addresses': first}, open(sys.argv[2], 'w'))
print(sys.argv[1], dict(counts))
