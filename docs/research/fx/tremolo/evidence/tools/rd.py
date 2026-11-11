#!/usr/bin/env python3
"""rd.py <thin-binary> str|f32|f64|u8|u32|u64 0xADDR [count]: read data at a vm address."""
import sys, struct, subprocess, re
path, kind = sys.argv[1], sys.argv[2]
out = subprocess.run(["otool", "-l", path], capture_output=True, text=True).stdout
secs = [(int(m.group(1),16), int(m.group(2),16), int(m.group(3))) for m in re.finditer(r"sectname \S+\n\s+segname \S+\n\s+addr (0x[0-9a-f]+)\n\s+size (0x[0-9a-f]+)\n\s+offset (\d+)", out)]
def rd(addr, n):
    for a, s, o in secs:
        if a <= addr < a + s and o:
            with open(path, "rb") as f:
                f.seek(o + addr - a); return f.read(n)
    return None
args = sys.argv[3:]
if kind == "str":
    for a in args:
        b = rd(int(a,16), 200)
        print(a, repr(b.split(b"\0")[0].decode("utf8","replace")) if b else None)
else:
    fmt = {"f32":("<f",4),"f64":("<d",8),"u8":("<B",1),"u32":("<I",4),"u64":("<Q",8)}[kind]
    a = int(args[0],16); n = int(args[1]) if len(args)>1 else 1
    b = rd(a, n*fmt[1])
    if b is None: print("not file-backed")
    else:
        for i in range(n): print(hex(a+i*fmt[1]), struct.unpack_from(fmt[0], b, i*fmt[1])[0])
