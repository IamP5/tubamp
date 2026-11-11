#!/usr/bin/env python3
"""Prototype of the proposed tubamp compressor engine (our own design, for acceptance numbers).

    proto.py in.wav out.wav [--thr -20 --ratio 4 --knee 6 --att 5 --rel 120 --rel2 0 --w 1
                             --hold 0 --hpf 0 --fb 0 --makeup 0 --mix 1]

Feed-forward (or feedback with --fb 1), linked peak detector with optional hold and
sidechain high-pass, dB gain computer with quadratic knee, branching one-pole ballistics
on the gain reduction in dB (tau = time to 63 %), optional second release stage.
"""
import argparse, math, os, sys
import numpy as np
sys.path.insert(0, os.environ["RE"])
import sig
ap = argparse.ArgumentParser()
ap.add_argument("inp"); ap.add_argument("out")
for k, v in dict(thr=-20.0, ratio=4.0, knee=6.0, att=5.0, rel=120.0, rel2=0.0, w=1.0, hold=0.0,
                 hpf=0.0, fb=0.0, makeup=0.0, mix=1.0).items():
    ap.add_argument("--" + k, type=float, default=v)
a = ap.parse_args()
x, sr = sig.read_wav(a.inp)
n = x.shape[1]
coef = lambda ms: math.exp(-1.0 / (max(ms, 1e-3) * 1e-3 * sr))
aA, aR1, aR2 = coef(a.att), coef(a.rel), coef(a.rel2 if a.rel2 > 0 else a.rel)
hold_n = int(round(a.hold * 1e-3 * sr))
hp_a = math.exp(-2 * math.pi * a.hpf / sr) if a.hpf > 0 else 0.0
slope_ff = 1.0 / a.ratio - 1.0
slope_fb = 1.0 - a.ratio
W = max(a.knee, 1e-6)
y = np.zeros_like(x)
g1 = g2 = 0.0; held = 0.0; cnt = 0; hp_x = hp_y = 0.0; ylev = 0.0
xs = x.T.tolist()
mk = 10 ** (a.makeup / 20.0)
gtrace = np.zeros(n)
for i in range(n):
    fr = xs[i]
    if a.fb:
        d = ylev                                   # level of the previous output sample
    else:
        d = max(abs(v) for v in fr)                # linked: max over channels
    if hp_a:                                       # one-pole high-pass on the detector signal (mono: ch 0)
        s = fr[0] if not a.fb else d
        hp_y = hp_a * (hp_y + s - hp_x); hp_x = s; d = abs(hp_y)
    if hold_n:
        if d >= held: held = d; cnt = hold_n
        elif cnt > 0: cnt -= 1
        else: held = d
        d = held
    L = 20.0 * math.log10(max(d, 1e-6))
    over = L - a.thr
    sl = slope_fb if a.fb else slope_ff
    if over <= -W / 2: gc = 0.0
    elif over < W / 2: gc = sl * (over + W / 2) ** 2 / (2 * W)
    else: gc = sl * over
    if gc < g1: g1 = aA * g1 + (1 - aA) * gc
    else:       g1 = aR1 * g1 + (1 - aR1) * gc
    if gc < g2: g2 = aA * g2 + (1 - aA) * gc
    else:       g2 = aR2 * g2 + (1 - aR2) * gc
    g = a.w * g1 + (1 - a.w) * g2
    gl = 10 ** (g / 20.0)
    gtrace[i] = g
    ylev = max(abs(v) for v in fr) * gl
    for ch, v in enumerate(fr):
        y[ch, i] = (v * gl * mk) * a.mix + v * (1 - a.mix)
sig.write_wav(a.out, y, sr)
print(f"max_gr_db\t{-gtrace.min():.2f}", file=sys.stderr)
