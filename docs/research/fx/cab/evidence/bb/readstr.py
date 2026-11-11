import sys
sys.path.insert(0, sys.argv[1])
import disasm
from pathlib import Path
thin = disasm.thin_slice(Path(sys.argv[2])); a=int(sys.argv[3],16); n=int(sys.argv[4])
d = disasm.read_vm(thin, a, n)
off=0
for s in d.split(b"\0"):
    if s: print(hex(a+off), s.decode('latin1'))
    off += len(s)+1
