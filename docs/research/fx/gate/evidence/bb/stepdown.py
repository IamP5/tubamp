#!/usr/bin/env python3
"""stepdown.py gen <dir> <sigpy_dir> | an <in> <out> <sigpy_dir>
gen: 1 kHz, 0.5 s at -20 dBFS then 3 s at L2 for L2 in -45,-50,-55,-60,-65 (files step_<L2>.wav)
an : gain 0.2 s, 1.0 s and 2.9 s after the step"""
import sys, numpy as np
sys.path.insert(0, sys.argv[-1]); import sig
sr = 48000
if sys.argv[1] == 'gen':
    for l2 in (-45, -50, -55, -60, -65):
        lev = np.concatenate([np.full(sr//2, -20.0), np.full(3*sr, float(l2))])
        sig.write_wav(f'{sys.argv[2]}/step_{-l2}.wav', (10**(lev/20)*np.sin(2*np.pi*1000*np.arange(len(lev))/sr)).astype(np.float32), sr)
else:
    x, _ = sig.read_wav(sys.argv[2]); y, _ = sig.read_wav(sys.argv[3]); x = x[0]; y = y[0]
    r = []
    for dt in (0.2, 1.0, 2.9):
        a = int((0.5+dt)*sr); b = a+480
        r.append(f'{dt}s:{20*np.log10(np.sqrt(np.mean(y[a:b]**2))/np.sqrt(np.mean(x[a:b]**2))+1e-12):.1f}')
    print(' '.join(r))
