#!/usr/bin/env python3
"""bl_xref.py <thin-binary> <symbols.txt> 0xTARGET...: callers (bl/b) of an address, with containing symbol."""
import sys, subprocess, re, bisect
import numpy as np
path, symf = sys.argv[1], sys.argv[2]
targets = [int(a, 16) for a in sys.argv[3:]]
out = subprocess.run(["otool", "-l", path], capture_output=True, text=True).stdout
m = re.search(r"sectname __text\n\s+segname __TEXT\n\s+addr (0x[0-9a-f]+)\n\s+size (0x[0-9a-f]+)\n\s+offset (\d+)", out)
addr, size, off = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))
raw = np.fromfile(path, dtype="<u4", count=size // 4, offset=off).astype(np.int64)
pcs = addr + 4 * np.arange(raw.size, dtype=np.int64)
isb = (raw & 0x7c000000) == 0x14000000
imm = raw & 0x3ffffff
imm = np.where(imm & (1 << 25), imm - (1 << 26), imm)
tgt = pcs + (imm << 2)
syms = []
for line in open(symf):
    p = line.rstrip("\n").split(" ", 2)
    if len(p) == 3 and p[1] in "tT":
        try: syms.append((int(p[0], 16), p[2]))
        except ValueError: pass
syms.sort(); addrs = [s[0] for s in syms]
for t in targets:
    hits = pcs[isb & (tgt == t)]
    print(f"# {t:#x}: {hits.size} callers")
    for h in hits:
        i = bisect.bisect_right(addrs, int(h)) - 1
        print(hex(int(h)), syms[i][1][:160])
