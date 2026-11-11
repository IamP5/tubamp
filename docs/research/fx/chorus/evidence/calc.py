#!/usr/bin/env python3
"""Closed-form numbers used in the chorus report (numpy only).  python3 calc.py ours | interp"""
import sys
import numpy as np

def ours():
    # juce::dsp::Chorus as tubamp configures it: delay_ms = max(1, 7 + 20 * (0.5 * depth) * sin(2 pi rate t))
    print("# rate_hz\tdepth\tdelay_min_ms\tdelay_max_ms\tpeak_cents_up\tpeak_cents_down\tclamped_at_1ms")
    for rate, depth in [(0.05, 0.4), (0.25, 0.4), (0.5, 0.4), (1, 0.4), (3, 0.4), (10, 0.4), (1, 0.1), (1, 1.0), (10, 1.0)]:
        D = 0.010 * depth
        dev = 2 * np.pi * rate * D
        print(f"{rate}\t{depth}\t{max(1.0, 7 - 10 * depth):.1f}\t{7 + 10 * depth:.1f}\t{1200 * np.log2(1 + dev):+.1f}\t"
              f"{1200 * np.log2(max(1 - dev, 1e-3)):+.1f}\t{'yes' if 7 - 10 * depth < 1 else 'no'}")

def interp():
    fs = 48000.0
    fr = np.linspace(0, 1, 101)
    print("# freq_hz\tlinear_worst_db\tlinear_spread_db\thermite4_worst_db\thermite4_spread_db\t(worst case and max-min over the fractional position, fs 48 kHz)")
    for f in (2000, 5000, 7000, 10000, 12000, 15000):
        w = 2 * np.pi * f / fs
        z = np.exp(-1j * w)
        lin = np.abs((1 - fr) + fr * z)
        # Catmull-Rom weights for samples at offsets -1, 0, 1, 2 around the read point
        hm1 = -0.5 * fr + fr**2 - 0.5 * fr**3
        h0 = 1 - 2.5 * fr**2 + 1.5 * fr**3
        h1 = 0.5 * fr + 2 * fr**2 - 1.5 * fr**3
        h2 = -0.5 * fr**2 + 0.5 * fr**3
        her = np.abs(hm1 * z**-1 + h0 + h1 * z + h2 * z**2)
        d = lambda v: 20 * np.log10(v)
        print(f"{f}\t{d(lin.min()):.2f}\t{d(lin.max()) - d(lin.min()):.2f}\t{d(her.min()):.2f}\t{d(her.max()) - d(her.min()):.2f}")

{"ours": ours, "interp": interp}[sys.argv[1]]()
