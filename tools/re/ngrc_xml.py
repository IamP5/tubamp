#!/usr/bin/env python3
"""Cut the XML preset documents out of a Guitar Rig component preset file.

    ngrc_xml.py <file.ngrc> <outprefix>

Writes <outprefix>_doc<N>.xml for every embedded <component-preset> (doc0 is the INIT
or first factory preset) and prints index, size and preset name. Input for gr6state.py."""
import re, sys
b = open(sys.argv[1], 'rb').read()
for n, m in enumerate(re.finditer(rb'<\?xml.*?</component-preset>', b, re.S)):
    txt = m.group(0).decode('utf8', 'replace')
    open(f'{sys.argv[2]}_doc{n}.xml', 'w').write(txt + '\n')
    name = re.search(r'preset-name="([^"]*)"', txt)
    print(n, len(txt), name.group(1) if name else '(unnamed)')
