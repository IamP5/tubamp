#!/usr/bin/env python3
"""at5_params.py 0xOnSetParameter...: parameter-name strings each OnSetParameter compares against (read at run time)."""
import os, re, subprocess, sys
W = os.environ["W"]; AT5 = os.environ["AT5"]
for osp in sys.argv[1:]:
    out = subprocess.run(["python3", f"{W}/tools/ann.py", AT5, osp], capture_output=True, text=True).stdout
    cands = []
    for m in re.finditer(r"; =(0x[0-9a-f]+)", out):
        a = int(m.group(1), 16)
        for c in (a, a - 8):
            if c not in cands: cands.append(c)
    rec = f"{W}/wav/_gstr.rec"; open(rec, "w").write("".join(f"gstr {c:#x}\n" for c in cands))
    res = subprocess.run([f"{W}/tools/rigcall", AT5, rec], capture_output=True, text=True, errors="replace").stdout
    names = []
    for line in res.splitlines():
        m = re.match(r"gstr (0x[0-9a-f]+): ([A-Za-z][A-Za-z0-9 _\-\+/]{1,30})$", line)
        if m and int(m.group(1), 16) % 8 == 0: names.append((m.group(1), m.group(2)))
    title = out.splitlines()[1][:90] if out else osp
    print(osp, title); print("   ", ", ".join(f"{n}@{a}" for a, n in names))
