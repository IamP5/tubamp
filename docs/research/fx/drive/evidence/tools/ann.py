#!/usr/bin/env python3
"""Annotated compact arm64 disassembly: resolves adrp/ldr literals and mov/movk/fmov float immediates.
   ann.py <binary> 0xADDR [0xADDR ...] | --match REGEX [--max N]
"""
import bisect, os, re, struct, subprocess, sys
from pathlib import Path
sys.path.insert(0, os.environ["RE"])
import disasm

def main():
    binary = Path(sys.argv[1]); args = sys.argv[2:]
    thin = disasm.thin_slice(binary)
    syms = disasm.symbol_index(binary); code = disasm.code_symbols(syms)
    addrs = [a for a, _ in code]; end = disasm.text_end(thin) or (code[-1][0] + 4)
    secs = disasm.sections(thin); blob = open(thin, "rb")
    def rd(addr, n):
        for _s, _c, vm, size, off in secs:
            if vm <= addr < vm + size and off:
                blob.seek(off + addr - vm); return blob.read(n)
        return None
    def name_at(a):
        i = bisect.bisect_right(addrs, a) - 1
        if i < 0: return hex(a)
        n = code[i][1]; d = a - code[i][0]
        n = re.sub(r'std::__1::', 'std::', n)
        if len(n) > 90: n = n[:87] + '...'
        return n + (f"+{d:#x}" if d else "")
    targets = []
    if args and args[0] == "--match":
        rx = re.compile(args[1], re.I); mx = int(args[3]) if len(args) > 3 and args[2] == "--max" else 40
        targets = [i for i, (_a, n) in enumerate(code) if rx.search(n)][:mx]
    else:
        for a in args:
            i = bisect.bisect_right(addrs, int(a, 16)) - 1; targets.append(i)
    def fl(b): return struct.unpack("<f", b)[0]
    def db(b): return struct.unpack("<d", b)[0]
    def fmtf(v): return f"{v:.9g}"
    for i in targets:
        start = code[i][0]; size = disasm.extent(code, i, end)
        print(f"\n;;; {code[i][1][:200]} @ {start:#x} ({size} bytes)")
        body = disasm.dump(thin, start, start + size)
        page = {}; imm = {}
        for line in body.splitlines():
            m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', line)
            if not m: continue
            a, op, rest = m.group(1), m.group(2), m.group(3)
            rest = re.sub(r'\s*;\s*=.*$', '', rest)
            note = ""
            tm = re.search(r'(0x[0-9a-f]+) <[^>]*>', rest)
            if op == "adrp":
                r = rest.split(",")[0].strip(); page[r] = int(tm.group(1), 16) if tm else None
                rest = f"{r}, {tm.group(1)}" if tm else rest
            elif op in ("bl", "b") and tm:
                t = int(tm.group(1), 16)
                rest = tm.group(1) + ("  ; " + name_at(t) if (op == "bl" or not (start <= t < start + size)) else "")
            elif tm:
                rest = re.sub(r' <[^>]*>', '', rest)
            mm = re.match(r'(x\d+), (x\d+), #(0x[0-9a-f]+)$', rest)
            if op == "add" and mm and page.get(mm.group(2)) is not None:
                v = page[mm.group(2)] + int(mm.group(3), 16); page[mm.group(1)] = v
                b = rd(v, 16)
                note = f"  ; ={v:#x}" + (f" f32[{', '.join(fmtf(x) for x in struct.unpack('<4f', b))}]" if b else "")
            mm = re.match(r'([sdqwx])(\d+), \[(x\d+), #(0x[0-9a-f]+)\]$', rest)
            if op in ("ldr", "ldrsw") and mm and page.get(mm.group(3)) is not None:
                v = page[mm.group(3)] + int(mm.group(4), 16); k = mm.group(1)
                n = {"s": 4, "d": 8, "q": 16, "w": 4, "x": 8}[k]; b = rd(v, n)
                if b:
                    if k == "s": note = f"  ; [{v:#x}] = {fmtf(fl(b))}"
                    elif k == "d": note = f"  ; [{v:#x}] = {fmtf(db(b))} (f64) | f32[{', '.join(fmtf(x) for x in struct.unpack('<2f', b))}]"
                    elif k == "q": note = f"  ; [{v:#x}] = f32[{', '.join(fmtf(x) for x in struct.unpack('<4f', b))}] | f64[{', '.join(fmtf(x) for x in struct.unpack('<2d', b))}]"
                    else: note = f"  ; [{v:#x}] = {int.from_bytes(b, 'little'):#x}"
                if k in "wx": page[k + mm.group(2)] = None
            mm = re.match(r'([wx])(\d+), #(-?0x[0-9a-f]+|-?\d+)(?:, lsl #(\d+))?$', rest)
            if op in ("mov", "movz") and mm:
                imm[mm.group(2)] = (int(mm.group(3), 0) << int(mm.group(4) or 0)) & 0xFFFFFFFFFFFFFFFF
                page.pop("x" + mm.group(2), None)
            elif op == "movk" and mm and mm.group(2) in imm:
                sh = int(mm.group(4) or 0); imm[mm.group(2)] = (imm[mm.group(2)] & ~(0xFFFF << sh)) | (int(mm.group(3), 0) << sh)
            mm = re.match(r'([sd])\d+, ([wx])(\d+)$', rest)
            if op == "fmov" and mm and mm.group(3) in imm:
                v = imm[mm.group(3)]
                note = f"  ; = {fmtf(fl(struct.pack('<I', v & 0xFFFFFFFF)))}" if mm.group(1) == "s" else f"  ; = {fmtf(db(struct.pack('<Q', v)))}"
            mm = re.match(r'[wx](\d+), \[', rest)
            if op in ("str", "stp") and mm and mm.group(1) in imm:
                v = imm[mm.group(1)]
                if v > 0xFFFF:
                    note = f"  ; imm {v:#x} f32={fmtf(fl(struct.pack('<I', v & 0xFFFFFFFF)))}" + (f" f64={fmtf(db(struct.pack('<Q', v)))}" if v > 0xFFFFFFFF else "")
            mm = re.match(r'v\d+, ([wx])(\d+)$', rest)
            if op.startswith("dup") and mm and mm.group(2) in imm:
                v = imm[mm.group(2)]; note = f"  ; dup imm f32={fmtf(fl(struct.pack('<I', v & 0xFFFFFFFF)))}"
            print(f"{a} {op} {rest}{note}")
main()
