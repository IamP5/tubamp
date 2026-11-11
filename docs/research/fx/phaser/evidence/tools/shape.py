#!/usr/bin/env python3
"""shape.py track.tsv period [n=2] [k=0]: fold the k-th notch track over one LFO period, print normalised log-f shape."""
import sys, numpy as np
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import ptrack
T, F, D, PK, M = ptrack.load_track(sys.argv[1], int(sys.argv[3]) if len(sys.argv) > 3 else 2)
per = float(sys.argv[2]); k = int(sys.argv[4]) if len(sys.argv) > 4 else 0
lf = np.log(F[:, k]); lo, hi = lf.min(), lf.max()
t0 = T[np.argmin(lf)]; ph = ((T - t0)/per) % 1.0
bins = 24; out = []
for b in range(bins):
    m = (ph >= b/bins) & (ph < (b+1)/bins)
    if m.any(): out.append(((b+0.5)/bins, float(np.median((lf[m]-lo)/(hi-lo))), float(np.exp(np.median(lf[m])))))
print('# phase\tnorm_logf\thz')
for p, v, hz in out: print(f'{p:.3f}\t{v:.3f}\t{hz:.0f}')
