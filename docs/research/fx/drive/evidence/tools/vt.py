#!/usr/bin/env python3
"""vt.py <binary> <vtable vmaddr> [nslots]: decode vtable slots (chained-fixup aware) to symbol names."""
import bisect, os, struct, sys
from pathlib import Path
sys.path.insert(0, os.environ["RE"]); import disasm
binary = Path(sys.argv[1]); vt = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 48
thin = disasm.thin_slice(binary); code = disasm.code_symbols(disasm.symbol_index(binary)); addrs = [a for a, _ in code]
raw = disasm.read_vm(thin, vt, 8 * (n + 2))
for i in range(n + 2):
    v = struct.unpack_from("<Q", raw, 8 * i)[0]; t = v & 0xFFFFFFFFF
    j = bisect.bisect_right(addrs, t) - 1
    name = code[j][1] if j >= 0 and t - code[j][0] == 0 else "?"
    print(f"slot {8*(i-2):#06x}  {t:#x}  {name[:170]}")
