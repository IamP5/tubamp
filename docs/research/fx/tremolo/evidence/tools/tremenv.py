#!/usr/bin/env python3
"""tremenv.py <out.wav> --freq F [--ref ref.wav | --refdb DB] [--ch N] [--skip S] [--table N] [--lp HZ]

Gain envelope of a tremolo acting on a steady sine of frequency F.
Envelope = |lowpassed complex demodulation at F| (Gaussian lowpass, default 0.35*F), so harmonics
added by an amp do not leak in. Gain = envelope / reference amplitude (--refdb: sine peak level in
dBFS; --ref: unprocessed render, median envelope). Prints one summary row and, with --table N, one
modulation cycle resampled to N points (cycle starts at the gain minimum).
Columns: rate_hz  gmax  gmin  depth_db  duty_hi (fraction of cycle above the mid gain)
         rise_ms / fall_ms (10-90 % of the swing)  mean_gain_db
"""
import argparse, sys
import numpy as np
sys.path.insert(0, __import__('os').environ['RE'])
import sig

def envelope(x, sr, f, lp):
    n = np.arange(len(x))
    z = x * np.exp(-2j * np.pi * f * n / sr)
    X = np.fft.fft(z)
    fr = np.fft.fftfreq(len(x), 1 / sr)
    X *= np.exp(-0.5 * (fr / (lp / 1.177)) ** 2)   # gaussian, -3 dB at lp
    return 2 * np.abs(np.fft.ifft(X))

ap = argparse.ArgumentParser()
ap.add_argument('out'); ap.add_argument('--freq', type=float, required=True)
ap.add_argument('--ref'); ap.add_argument('--refdb', type=float)
ap.add_argument('--ch', type=int, default=0); ap.add_argument('--skip', type=float, default=1.0)
ap.add_argument('--table', type=int, default=0); ap.add_argument('--lp', type=float, default=0)
ap.add_argument('--label', default='')
a = ap.parse_args()
x, sr = sig.read_wav(a.out); x = x[a.ch]
lp = a.lp or 0.35 * a.freq
e = envelope(x, sr, a.freq, lp)
if a.ref:
    r, _ = sig.read_wav(a.ref); ref = np.median(envelope(r[0], sr, a.freq, lp))
elif a.refdb is not None:
    ref = 10 ** (a.refdb / 20)
else:
    ref = e.max()
s0 = int(a.skip * sr); s1 = len(e) - int(0.25 * sr)
g = e[s0:s1] / ref
gmax, gmin = np.percentile(g, 99.9), np.percentile(g, 0.1)
mid = 0.5 * (gmax + gmin)
# period from upward mid crossings
up = np.nonzero((g[:-1] < mid) & (g[1:] >= mid))[0]
rate = float('nan'); duty = float('nan'); rise = fall = float('nan'); cyc = None
if len(up) >= 3 and (gmax - gmin) > 1e-3 * max(gmax, 1e-9):
    per = np.median(np.diff(up)); rate = sr / per
    duty = float(np.mean(g[up[0]:up[-1]] > mid))
    lo, hi = gmin + 0.1 * (gmax - gmin), gmin + 0.9 * (gmax - gmin)
    rs, fs = [], []
    for k in range(len(up) - 1):
        c = g[up[k]:up[k + 1]]
        i_min = int(np.argmin(c)); c2 = np.concatenate([c[i_min:], c[:i_min]])   # start at minimum
        i_max = int(np.argmax(c2))
        seg_r, seg_f = c2[:i_max + 1], c2[i_max:]
        a_lo = np.nonzero(seg_r >= lo)[0]; a_hi = np.nonzero(seg_r >= hi)[0]
        if len(a_lo) and len(a_hi): rs.append((a_hi[0] - a_lo[0]) / sr * 1e3)
        b_hi = np.nonzero(seg_f <= hi)[0]; b_lo = np.nonzero(seg_f <= lo)[0]
        if len(b_hi) and len(b_lo): fs.append((b_lo[0] - b_hi[0]) / sr * 1e3)
        if cyc is None and k >= 1: cyc = c2
    rise, fall = float(np.median(rs)) if rs else rise, float(np.median(fs)) if fs else fall
db = lambda v: 20 * np.log10(max(v, 1e-9))
print('label\trate_hz\tgmax\tgmin\tdepth_db\tduty_hi\trise_ms\tfall_ms\tmean_gain_db')
print(f'{a.label or a.out}\t{rate:.4f}\t{gmax:.4f}\t{gmin:.4f}\t{db(gmax) - db(gmin):.2f}\t{duty:.3f}\t{rise:.2f}\t{fall:.2f}\t{db(np.sqrt(np.mean(g ** 2))):.2f}')
if a.table and cyc is not None:
    xi = np.linspace(0, len(cyc) - 1, a.table)
    t = np.interp(xi, np.arange(len(cyc)), cyc)
    print('phase\tgain'); [print(f'{i / a.table:.3f}\t{v:.4f}') for i, v in enumerate(t)]
