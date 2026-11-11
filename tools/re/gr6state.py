#!/usr/bin/env python3
"""Author a Guitar Rig 6 AU state without the editor, for headless measurement.

    gr6state.py <default.aupreset> <component-doc.xml> <name> <out.aupreset> [id=value ...] [Rack.id=value ...]

Takes the default state written by `auhost state aufx 'Ni$R' -NI- default.aupreset`,
wraps the parameter block of a factory component preset into a rack component, inserts
it as the only one, overrides parameter values by id, and rebuilds the NI chunk
container (zlib 'atad' chunks; the rack is XML). `Rack.<id>=<v>` overrides a parameter
of the fixed Rack component (Rack.G=0 turns its gate off).

<component-doc.xml> comes from ngrc_xml.py, which cuts the XML documents out of
"/Library/Application Support/Native Instruments/Guitar Rig 6/Component Presets/<Name>/<Name> Factory.ngrc".
A document with no component id needs GR6_COMPONENT_ID in the environment.

Then: auhost render aufx 'Ni$R' -NI- in.wav out.wav --state out.aupreset

Found by the tremolo, phaser and cab studies (docs/research/fx/*/evidence)."""
import base64, os, plistlib, re, struct, sys, zlib

def parse(v):
    i = v.index(b'atad'); head = v[:i]; chunks = []
    while i < len(v):
        assert v[i:i+4] == b'atad'
        ver = struct.unpack_from('<I', v, i+4)[0]; tag = v[i+8:i+12]; comp = v[i+12:i+16]
        clen, ulen = struct.unpack_from('<II', v, i+16)
        chunks.append([ver, tag, comp, zlib.decompress(v[i+24:i+24+clen])]); i += 24 + clen
    return head, chunks

src, doc, name, out = sys.argv[1:5]
p = plistlib.load(open(src, 'rb'))
head, chunks = parse(bytes(p['vstdata']))
txt = open(doc).read()
m = re.search(r'component-id="(\d+)"', txt) or re.search(r'<component id="(\d+)"', txt)
cid = m.group(1) if m else os.environ["GR6_COMPONENT_ID"]
params = re.search(r'<parameters[^>]*num-parameters="(\d+)"[^>]*>(.*?)</parameters>', txt, re.S)
n, body = params.group(1), params.group(2)
rack = {}
for kv in sys.argv[5:]:
    k, v = kv.split('=')
    if k.startswith('Rack.'):
        rack[k[5:]] = v; continue
    body, cnt = re.subn(r'(<parameter id="%s" name="[^"]*" value=")[^"]*(")' % re.escape(k), r'\g<1>%s\g<2>' % v, body)
    assert cnt == 1, (k, cnt)
uuid = base64.b64encode(bytes(range(16, 32))).decode()
comp = ('      <component id="%s" name="%s" uuid="%s">\n        <component-gui>\n'
        '          <view component-template-bank="GR4" component-template-name="INIT" stored-view-mode="4" view-mode="0" visible="true"/>\n'
        '        </component-gui>\n        <component-audio version="2">\n'
        '          <parameters enable-automation="1" num-parameters="%s" static-automation="0">%s</parameters>\n'
        '        </component-audio>\n      </component>\n') % (cid, name, uuid, n, body)
for c in chunks:
    if c[1] == b'tsrp':
        raw = c[3]; assert raw[:4] == b' LMX'
        xml = raw[12:].decode('utf8')
        assert '<non-fix-components/>' in xml
        xml = xml.replace('<non-fix-components/>', '<non-fix-components>\n' + comp + '    </non-fix-components>')
        for k, v in rack.items():
            i0 = xml.index('<component id="100" name="Rack"'); i1 = xml.index('</component>', i0)
            seg, cnt = re.subn(r'(<parameter id="%s" name="[^"]*" value=")[^"]*(")' % re.escape(k), r'\g<1>%s\g<2>' % v, xml[i0:i1])
            assert cnt == 1, (k, cnt)
            xml = xml[:i0] + seg + xml[i1:]
        x = xml.encode('utf8')
        c[3] = raw[:8] + struct.pack('<I', len(x)) + x
blob = bytearray(head)
for ver, tag, cmp_, raw in chunks:
    z = zlib.compress(raw)
    blob += b'atad' + struct.pack('<I', ver) + tag + cmp_ + struct.pack('<II', len(z), len(raw)) + z
p['vstdata'] = bytes(blob); p['name'] = os.path.basename(out)
plistlib.dump(p, open(out, 'wb'))
