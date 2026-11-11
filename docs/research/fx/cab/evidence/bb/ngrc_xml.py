#!/usr/bin/env python3
"""ngrc_xml.py <file.ngrc> <outprefix>: write every XML <component-preset> document embedded in a Guitar Rig
component preset file as <outprefix>_doc<N>.xml (doc0 is the INIT preset or the first factory preset)."""
import re, sys
b = open(sys.argv[1], 'rb').read()
for n, m in enumerate(re.finditer(rb'<\?xml.*?</component-preset>', b, re.S)):
    txt = m.group(0).decode('utf8', 'replace')
    open(f'{sys.argv[2]}_doc{n}.xml', 'w').write(txt + '\n')
    name = re.search(r'preset-name="([^"]*)"', txt)
    print(n, len(txt), name.group(1) if name else '(unnamed)')
