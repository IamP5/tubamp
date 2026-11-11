#!/usr/bin/env python3
"""Alias level of a tanh (and hard-clip) shaper under candidate oversampling / ADAA schemes, steady state.
Periodic 1 s test tones at 48 kHz (integer Hz), so every linear filter is applied exactly through its
frequency response on the FFT grid; the nonlinearity runs in the time domain at the oversampled rate.
Half-band filters are the JUCE polyphase-allpass designs (same formulas as juce_FilterDesign.cpp).
    alias_schemes.py > alias_schemes.tsv
"""
import math, sys
import numpy as np
SR = 48000

def juce_halfband(tw, db):
    wt = 2 * math.pi * tw; ds = 10 ** (db / 20)
    k = math.tan((math.pi - wt) / 4) ** 2; kp = math.sqrt(1 - k * k); e = (1 - math.sqrt(kp)) / (1 + math.sqrt(kp)) * 0.5
    q = e + 2 * e ** 5 + 15 * e ** 9 + 150 * e ** 13
    k1 = ds * ds / (1 - ds * ds); n = int(math.ceil(math.log(k1 * k1 / 16) / math.log(q)))
    n += (n % 2 == 0); n = max(n, 3); N = (n - 1) // 2; ai = []
    for i in range(1, N + 1):
        num = sum((-1) ** m * q ** (m * (m + 1)) * math.sin((2 * m + 1) * math.pi * i / n) for m in range(40)) * 2 * q ** 0.25
        den = 1 + 2 * sum((-1) ** m * q ** (m * m) * math.cos(m * 2 * math.pi * i / n) for m in range(1, 40))
        wi = num / den; api = math.sqrt((1 - wi * wi * k) * (1 - wi * wi / k)) / (1 + wi * wi); ai.append((1 - api) / (1 + api))
    return ai

def hb_response(ai, nbins):
    """H on an rfft grid of a signal of length 2*(nbins-1) at the doubled rate."""
    w = np.pi * np.arange(nbins) / (nbins - 1); z = np.exp(1j * w); a0 = np.ones_like(z); a1 = 1 / z
    for i, a in enumerate(ai):
        s = (a + z ** -2) / (1 + a * z ** -2)
        if i % 2 == 0: a0 = a0 * s
        else: a1 = a1 * s
    return 0.5 * (a0 + a1)

def up2(x, ai):
    y = np.zeros(2 * len(x)); y[::2] = 2 * x
    return np.fft.irfft(np.fft.rfft(y) * hb_response(ai, len(y) // 2 + 1), len(y))

def down2(x, ai):
    y = np.fft.irfft(np.fft.rfft(x) * hb_response(ai, len(x) // 2 + 1), len(x)); return y[::2]

# stage n design parameters exactly as juce::dsp::Oversampling (isMaximumQuality = true)
def stage(n): 
    f = 0.5 if n == 0 else 1.0
    return juce_halfband(0.10 * f, -90 + 10 * n), juce_halfband(0.12 * f, -75 + 10 * n)

def tanh_adaa(x):
    F = lambda v: np.abs(v) - math.log(2) + np.log1p(np.exp(-2 * np.abs(v)))
    xp = np.roll(x, 1); d = x - xp; ok = np.abs(d) > 1e-6
    return np.where(ok, (F(x) - F(xp)) / np.where(ok, d, 1), np.tanh(0.5 * (x + xp)))
def clip_adaa(x):
    F = lambda v: np.where(np.abs(v) <= 1, 0.5 * v * v, np.abs(v) - 0.5)
    xp = np.roll(x, 1); d = x - xp; ok = np.abs(d) > 1e-6
    return np.where(ok, (F(x) - F(xp)) / np.where(ok, d, 1), np.clip(0.5 * (x + xp), -1, 1))
SH = {"tanh": (np.tanh, tanh_adaa), "hardclip": (lambda v: np.clip(v, -1, 1), clip_adaa)}

def run(x, gain, stages, adaa, shaper):
    y = x * gain; des = [stage(n) for n in range(stages)]
    for up, _ in des: y = up2(y, up)
    y = SH[shaper][1 if adaa else 0](y)
    for _, dn in reversed(des): y = down2(y, dn)
    return y

def metrics(y, f0):
    w = np.hanning(len(y)); s = np.abs(np.fft.rfft(y * w)) / (w.sum() / 2)
    comp = lambda f: math.sqrt(float(np.sum(s[max(0, f - 2): f + 3] ** 2)) / 1.5)
    fund = comp(f0); mask = np.ones(len(s), bool); mask[:20] = False
    for k in range(1, SR // 2 // f0 + 1): mask[max(0, f0 * k - 6): f0 * k + 7] = False
    inh = np.where(mask, s, 0.0); below = inh[: f0 - 6]
    d = lambda v: 20 * math.log10(max(v, 1e-12))
    worst = int(np.argmax(below)) if f0 > 100 else 0
    return d(fund), worst, d(comp(worst)) - d(fund), d(math.sqrt(float(np.sum(inh[:20000] ** 2)) / 1.5)) - d(fund), d(math.sqrt(float(np.sum(inh[:20000] ** 2)) / 1.5))

if __name__ == "__main__":
    print("# alias_schemes.py: tanh / hard clip at the given pre-gain, 1 s periodic sines at 48 kHz, JUCE polyphase-IIR half-band stages (max quality), optional first-order ADAA at the oversampled rate")
    print("shaper\tfreq\tin_dbfs\tgain_db\tscheme\tfund_db\tworst_alias_hz\tworst_alias_rel_db\tinharm_to_20k_rel_db\tinharm_to_20k_dbfs")
    n = np.arange(SR)
    for shaper in ("tanh", "hardclip"):
        for f0, lvl in ((5000, -6), (5000, -18), (1244, -6), (3000, -6)):
            x = 10 ** (lvl / 20) * np.sin(2 * np.pi * f0 * n / SR)
            for g in (18, 36):
                for name, st, ad in (("1x", 0, False), ("1x+ADAA1", 0, True), ("2x (ships today)", 1, False), ("2x+ADAA1", 1, True), ("4x", 2, False), ("4x+ADAA1", 2, True), ("8x", 3, False), ("8x+ADAA1", 3, True), ("16x", 4, False)):
                    fu, wf, wr, tot, tabs = metrics(run(x, 10 ** (g / 20), st, ad, shaper), f0)
                    print(f"{shaper}\t{f0}\t{lvl}\t{g}\t{name}\t{fu:.2f}\t{wf}\t{wr:.1f}\t{tot:.1f}\t{tabs:.1f}")

