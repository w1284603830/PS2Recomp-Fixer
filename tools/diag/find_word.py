import glob, os, struct, sys

value = int(sys.argv[1], 16)
for f in sorted(glob.glob(os.path.join(sys.argv[2], '*.IRX'))):
    d = open(f, 'rb').read()
    hits = []
    i = d.find(struct.pack('<I', value))
    while i >= 0 and len(hits) < 8:
        hits.append(hex(i))
        i = d.find(struct.pack('<I', value), i + 1)
    # lui reg,hi ; ori/addiu reg,reg,lo within 6 instructions
    hi, lo = value >> 16, value & 0xFFFF
    code = []
    for p in range(0, len(d) - 4, 4):
        w = struct.unpack_from('<I', d, p)[0]
        if (w >> 26) == 0xF and (w & 0xFFFF) == hi:
            for q in range(p + 4, min(p + 28, len(d) - 4), 4):
                w2 = struct.unpack_from('<I', d, q)[0]
                if (w2 >> 26) in (0xD, 0x9) and (w2 & 0xFFFF) == lo:
                    code.append(hex(p))
                    break
    print(os.path.basename(f), 'data', hits, 'code', code[:6])
