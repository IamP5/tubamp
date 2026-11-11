import sys, struct
sys.path.insert(0, sys.argv[1])
import disasm
from pathlib import Path
binary = Path(sys.argv[2]); addr = int(sys.argv[3], 16); n = int(sys.argv[4]); per = int(sys.argv[5]) if len(sys.argv) > 5 else 16
thin = disasm.thin_slice(binary)
data = disasm.read_vm(thin, addr, n * 4)
vals = struct.unpack("<%di" % n, data)
for i in range(0, n, per):
    print(hex(addr + i * 4), " ".join(str(v) for v in vals[i:i + per]))
