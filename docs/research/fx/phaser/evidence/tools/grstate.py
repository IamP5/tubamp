#!/usr/bin/env python3
"""grstate.py <default.aupreset> <component.xml> <out.aupreset> [id=value ...]
Insert one rack component (XML block from a factory rack preset) into Guitar Rig 6's default
AU state and override parameter values by parameter id. The state is NI's chunk container:
'atad' chunks, zlib-compressed; chunk 'prst' holds ' LMX' + u32 1 + u32 len + XML."""
import plistlib, struct, sys, zlib, re
def parse(v):
    i = v.index(b'atad'); head = v[:i]; chunks = []
    while i < len(v):
        assert v[i:i+4] == b'atad'
        ver = struct.unpack_from('<I', v, i+4)[0]; tag = v[i+8:i+12]; comp = v[i+12:i+16]
        clen, ulen = struct.unpack_from('<II', v, i+16)
        chunks.append([ver, tag, comp, zlib.decompress(v[i+24:i+24+clen])]); i += 24+clen
    return head, chunks
src, compf, out = sys.argv[1:4]
p = plistlib.load(open(src,'rb'))
head, chunks = parse(bytes(p['vstdata']))
comp = open(compf).read()
for kv in sys.argv[4:]:
    k, v = kv.split('=')
    comp, n = re.subn(r'(<parameter id="%s" name="[^"]*" value=")[^"]*(")' % re.escape(k), r'\g<1>%s\g<2>' % v, comp)
    assert n == 1, (k, n)
for c in chunks:
    if c[1] == b'tsrp':
        raw = c[3]; assert raw[:4] == b' LMX'
        xml = raw[12:].decode('utf8')
        assert '<non-fix-components/>' in xml
        xml = xml.replace('<non-fix-components/>', '<non-fix-components>\n' + comp + '    </non-fix-components>')
        x = xml.encode('utf8')
        c[3] = raw[:8] + struct.pack('<I', len(x)) + x
blob = bytearray(head)
for ver, tag, cmp_, raw in chunks:
    body = zlib.compress(raw)
    blob += b'atad' + struct.pack('<I', ver) + tag + cmp_ + struct.pack('<II', len(body), len(raw)) + body
# header: u32 at len(head)-4 is total size field? keep, then patch if it equals old length
p['vstdata'] = bytes(blob); p['name'] = 'phaser-test'
plistlib.dump(p, open(out,'wb'))
