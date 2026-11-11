#!/usr/bin/env python3
"""Reference model of what tubamp ships: juce::dsp::Compressor<float> (juce_Compressor.cpp:99-121,
juce_BallisticsFilter.cpp:74,98-130), per channel, plus a static makeup gain.

    ours.py in.wav out.wav --thr -20 --ratio 4 --att 5 --rel 120 [--makeup 0]
"""
import argparse, math, os, sys
import numpy as np
sys.path.insert(0, os.environ["RE"])
import sig
ap = argparse.ArgumentParser()
ap.add_argument("inp"); ap.add_argument("out")
ap.add_argument("--thr", type=float, default=-20.0); ap.add_argument("--ratio", type=float, default=4.0)
ap.add_argument("--att", type=float, default=5.0); ap.add_argument("--rel", type=float, default=120.0)
ap.add_argument("--makeup", type=float, default=0.0)
a = ap.parse_args()
x, sr = sig.read_wav(a.inp)
exp_factor = -2.0 * math.pi * 1000.0 / sr                      # BallisticsFilter::prepare
cte = lambda ms: 0.0 if ms < 1e-3 else math.exp(exp_factor / ms)  # calculateLimitedCte
c_at, c_rl = cte(a.att), cte(a.rel)
thr = 10.0 ** (a.thr / 20.0); thr_inv = 1.0 / thr; rinv = 1.0 / a.ratio
mk = 10.0 ** (a.makeup / 20.0)
y = np.zeros_like(x)
for ch in range(x.shape[0]):
    yold = 0.0
    xin = x[ch]; out = y[ch]
    for i in range(len(xin)):
        v = abs(xin[i])
        c = c_at if v > yold else c_rl
        yold = v + c * (yold - v)
        g = 1.0 if yold < thr else (yold * thr_inv) ** (rinv - 1.0)
        out[i] = g * xin[i] * mk
sig.write_wav(a.out, y, sr)
