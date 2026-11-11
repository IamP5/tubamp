#!/usr/bin/env python3
"""xref.py <calls.txt> <all_syms.txt> 0xTARGET...: callers (containing symbol) of each target address."""
import bisect, sys
syms = []
for line in open(sys.argv[2]):
    p = line.rstrip("\n").split("\t")
    if len(p) == 3:
        try: syms.append((int(p[0], 16), p[2]))
        except ValueError: pass
syms.sort(); addrs = [a for a, _ in syms]
want = {t.lower() for t in sys.argv[3:]}
for line in open(sys.argv[1]):
    a, op, t = line.split()[:3]
    if t in want:
        ai = int(a, 16); i = bisect.bisect_right(addrs, ai) - 1
        print(t, op, hex(ai), f"{syms[i][0]:#x}", syms[i][1][:160])
