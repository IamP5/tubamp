import re, sys, zlib
raw = open(sys.argv[1], 'rb').read()
out = []
for m in re.finditer(rb'stream\r?\n(.*?)\r?\nendstream', raw, re.S):
    try: d = zlib.decompress(m.group(1))
    except Exception: continue
    if b'BT' not in d: continue
    for t in re.finditer(rb'\[(.*?)\]\s*TJ|\((.*?)\)\s*Tj', d, re.S):
        s = t.group(1) or t.group(2)
        if t.group(1) is not None:
            parts = re.findall(rb'\(((?:\\.|[^\\)])*)\)|(-?\d+\.?\d*)', s)
            txt = b''
            for a, b in parts:
                if a: txt += a
                elif b and float(b) < -200: txt += b' '
            s = txt
        out.append(s.decode('latin1'))
    out.append('\n')
print(' '.join(out))
