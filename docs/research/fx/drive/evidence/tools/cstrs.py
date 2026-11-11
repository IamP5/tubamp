#!/usr/bin/env python3
"""cstrs.py <binary> 0xTABLE N: decode N pointers (chained fixups) at TABLE into C strings."""
import os, struct, sys
from pathlib import Path
sys.path.insert(0, os.environ["RE"]); import disasm
thin = disasm.thin_slice(Path(sys.argv[1])); t = int(sys.argv[2], 16)
for i in range(int(sys.argv[3])):
    v = struct.unpack("<Q", disasm.read_vm(thin, t + 8 * i, 8))[0] & 0xFFFFFFFFF
    s = disasm.read_vm(thin, v, 64).split(b"\0")[0].decode("utf8", "replace")
    print(f"{i}: {v:#x} {s!r}")
