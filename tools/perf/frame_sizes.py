"""Stack-frame sizes of every function in a PE (dumpbin /disasm), for comparing builds.

  frame_sizes.py <exe> <out.json>

Counts `sub rsp,imm` prologues and the __chkstk idiom (`mov eax,imm` / `call` / `sub rsp,rax`) used for frames of
4 KiB or more. Reports the distribution, the largest frames with their addresses, and totals.
"""
import collections
import json
import os
import re
import subprocess
import sys

DUMPBIN = os.environ.get('DUMPBIN', 'dumpbin.exe')  # set DUMPBIN or run from a VS developer prompt
line_re = re.compile(r'^\s+([0-9A-F]{16}): (\S+)\s*(.*)$')


def main():
    proc = subprocess.Popen([DUMPBIN, '/nologo', '/disasm:nobytes', sys.argv[1]], stdout=subprocess.PIPE, text=True,
                            errors='replace')
    frames = []
    last_mov = None  # (addr, imm) of a recent `mov eax,imm`
    window = 0
    for line in proc.stdout:
        m = line_re.match(line)
        if not m:
            continue
        addr, mn, ops = m.groups()
        window += 1
        if mn == 'mov' and ops.startswith('eax,') and ops[4:].rstrip('h').isalnum():
            try:
                last_mov = (addr, int(ops[4:].rstrip('h'), 16))
                window = 0
            except ValueError:
                pass
        elif mn == 'sub' and ops.startswith('rsp,'):
            arg = ops[4:].strip()
            if arg == 'rax' and last_mov and window <= 3:
                frames.append((last_mov[1], last_mov[0], 'chkstk'))
            elif arg.endswith('h'):
                try:
                    frames.append((int(arg[:-1], 16), addr, 'sub'))
                except ValueError:
                    pass
    proc.wait()
    sizes = [f[0] for f in frames]
    buckets = collections.Counter()
    for s in sizes:
        buckets['<1K' if s < 1024 else '1-4K' if s < 4096 else '4-16K' if s < 16384 else '16-64K' if s < 65536
                else '>=64K'] += 1
    out = {'exe': sys.argv[1], 'functions': len(sizes), 'total_bytes': sum(sizes), 'buckets': buckets,
           'chkstk_frames': sum(1 for f in frames if f[2] == 'chkstk'),
           'largest': sorted(frames, reverse=True)[:40]}
    json.dump(out, open(sys.argv[2], 'w'), indent=1)
    print(json.dumps({k: out[k] for k in ('functions', 'total_bytes', 'buckets', 'chkstk_frames')}),
          out['largest'][:5])


if __name__ == '__main__':
    main()
