#!/usr/bin/env python3
"""adrp_xref.py <thin-binary> 0xPAGEADDR [...]: list addresses of ADRP instructions targeting the page(s)."""
import sys, struct, subprocess, re
import numpy as np
path = sys.argv[1]
targets = [int(a, 16) & ~0xfff for a in sys.argv[2:]]
out = subprocess.run(["otool", "-l", path], capture_output=True, text=True).stdout
# find __text section in __TEXT
m = re.search(r"sectname __text\n\s+segname __TEXT\n\s+addr (0x[0-9a-f]+)\n\s+size (0x[0-9a-f]+)\n\s+offset (\d+)", out)
addr, size, off = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))
raw = np.fromfile(path, dtype="<u4", count=size // 4, offset=off).astype(np.int64)
pcs = addr + 4 * np.arange(raw.size, dtype=np.int64)
is_adrp = (raw & 0x9f000000) == 0x90000000
immlo = (raw >> 29) & 3
immhi = (raw >> 5) & 0x7ffff
imm = (immhi << 2) | immlo
imm = np.where(imm & (1 << 20), imm - (1 << 21), imm)
tgt = (pcs & ~0xfff) + (imm << 12)
for t in targets:
    hits = pcs[is_adrp & (tgt == t)]
    print(f"# page {t:#x}: {hits.size} adrp")
    for h in hits:
        print(hex(int(h)))
