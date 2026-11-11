#!/usr/bin/env python3
"""consts.py <disasm dump>: list float/double immediates built with mov/movk, per function."""
import re,struct,sys
txt=open(sys.argv[1]).read().split('\n')
f=lambda h: struct.unpack('<f',struct.pack('<I',h&0xffffffff))[0]
d=lambda h: struct.unpack('<d',struct.pack('<Q',h&0xffffffffffffffff))[0]
i=0
while i<len(txt):
    l=txt[i]
    if l.startswith('; ====='): print(l[8:110]); i+=1; continue
    m=re.search(r'^\s*([0-9a-f]+):\s+mov\s+([wx])(\d+), #(-?0x[0-9a-f]+)',l)
    if m:
        addr,rt,rn,v=m.group(1),m.group(2),m.group(3),int(m.group(4),16)
        bits=64 if rt=='x' else 32
        v&=(1<<bits)-1
        j=i+1
        while j<len(txt):
            m2=re.search(r'movk\s+%s%s, #(0x[0-9a-f]+), lsl #(\d+)'%(rt,rn),txt[j])
            if not m2: break
            sh=int(m2.group(2)); v=(v&~(0xffff<<sh))|(int(m2.group(1),16)<<sh); j+=1
        if rt=='w' and v>0x00800000: print('   %s f32 %#x %.7g'%(addr,v,f(v)))
        elif rt=='x' and v>0x0010000000000000: print('   %s f64 %#x %.10g'%(addr,v,d(v)))
        i=j; continue
    m=re.search(r'^\s*([0-9a-f]+):\s+fmov\s+([sd]\d+), #(-?[0-9.]+)',l)
    if m and '-a' in sys.argv: print('   %s imm %s'%(m.group(1),m.group(3)))
    i+=1
