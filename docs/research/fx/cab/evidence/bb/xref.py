# xref.py <thin arm64 slice> 0xTARGET...: list every b/bl in __text that lands on a target (needs RE and RE_STATE)
import sys,struct,bisect
import os; sys.path.insert(0, os.environ['RE'])
import disasm
from pathlib import Path
T=Path(sys.argv[1]); targets=[int(x,16) for x in sys.argv[2:]]
for seg,sect,va,size,off in disasm.sections(T):
    if sect=='__text': break
data=open(T,'rb').read()[off:off+size]
syms=[(s[0],s[-1]) for s in disasm.symbol_index(T)]
addrs=[a for a,_ in syms]
import array
w=array.array('I'); w.frombytes(data[:len(data)//4*4])
tset=set(targets)
for i,ins in enumerate(w):
    if (ins>>26) in (0x25,0x05):  # bl / b
        imm=ins&0x3ffffff
        if imm&0x2000000: imm-=0x4000000
        pc=va+4*i; t=pc+imm*4
        if t in tset:
            j=bisect.bisect_right(addrs,pc)-1
            print(hex(t),hex(pc),'BL' if ins>>26==0x25 else 'B', syms[j][1][:150] if j>=0 else '?', hex(pc-syms[j][0]))
