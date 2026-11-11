#!/usr/bin/env python3
"""Headless black-box of Guitar Rig 6 EQ components (EQ orchestrator). Uses gr6state.py / ngrc_xml.py copied from
docs/research/fx/tremolo/evidence/tools/. Works in $RE_STATE/work/eq/gr6bb.
  setup once:  $RE_STATE/bin/auhost state aufx 'Ni$R' -NI- gr6_default.aupreset
               python3 -I ngrc_xml.py "/Library/Application Support/Native Instruments/Guitar Rig 6/Component Presets/EQ Parametric/EQ Parametric Factory.ngrc" ngrc_EQParametric   (same for 'EQ Graphic')
  one row:     python3 gr6state.py gr6_default.aupreset ngrc_EQParametric_doc0.xml "EQ Parametric" st.aupreset F1=<v> G1=<v> Q1=<v> G2=0.5 Rack.G=0
               $RE_STATE/bin/auhost render aufx 'Ni$R' -NI- n<sr>.wav o.wav --state st.aupreset --warmup 0.5
  curve = Welch |H| of a -24 dBFS noise pair, relative to the same component with all gains centred.
  python3 gr6eq.py"""
import os, subprocess, sys
import numpy as np
RE = os.environ["RE"]; RS = os.environ["RE_STATE"]; sys.path.insert(0, RE)
import sig
W = os.path.join(RS, "work/eq/gr6bb"); os.chdir(W)
def noise(sr):
    p = f"n{sr}.wav"
    if not os.path.exists(p):
        rng = np.random.default_rng(3); sig.write_wav(p, (rng.standard_normal(sr * 8) * sig.amp(-24)).astype(np.float32), sr)
    return p
def tf(doc, name, kv, sr):
    subprocess.run([sys.executable, "gr6state.py", "gr6_default.aupreset", doc, name, "st.aupreset", *[f"{k}={v}" for k, v in kv.items()], "Rack.G=0"], check=True, capture_output=True)
    r = subprocess.run([os.path.join(RS, "bin/auhost"), "render", "aufx", "Ni$R", "-NI-", noise(sr), "o.wav", "--state", "st.aupreset", "--warmup", "0.5"], capture_output=True, text=True, timeout=300)
    if r.returncode: raise RuntimeError(r.stderr[-300:])
    x, _ = sig.read_wav(noise(sr)); y, _ = sig.read_wav("o.wav"); n = min(x.shape[1], y.shape[1])
    f, h, _ = sig.welch_tf(x[0][sr:n], y[0][sr:n], sr, 8192 if sr < 60000 else 16384)
    return f, 20 * np.log10(np.abs(h) + 1e-30)
def peak(f, d, lo=30, hi=1e9):
    k = (f > lo) & (f < hi); i = int(np.argmax(np.abs(d[k]))); ff, dd = f[k], d[k]; g = dd[i]; s = np.sign(g)
    a = i
    while a > 0 and s * dd[a] > s * g / 2: a -= 1
    b = i
    while b < len(dd) - 1 and s * dd[b] > s * g / 2: b += 1
    return g, ff[i], ff[i] / max(ff[b] - ff[a], 1e-9)
P = ("ngrc_EQParametric_doc0.xml", "EQ Parametric"); flat = {"G1": 0.5, "G2": 0.5}
print("# EQ Parametric, band 1: peak gain, peak frequency, half-gain Q (search limited to f < 0.42 fs). Expected from the code (no pre-warp): f_peak = (fi/pi) atan(pi f / fi), f = 8.176*2^((119.589 v + 15.487)/12)")
print("# fi = internal rate. At 44.1 and 48 kHz the plugin reports 16 samples of latency and rolls off above 0.45 fs (it runs the rack at twice the host rate); at 96 kHz it reports 0: fi = 2 fs below 50 kHz, else fs.")
print("fs\tF1\tG1\tQ1\tpeak_dB\tpeak_Hz\tQ_halfgain\tnominal_Hz_from_code\tpredicted_peak_Hz_no_prewarp")
for sr in (44100, 48000, 96000):
    f, base = tf(*P, flat, sr)
    for F1, G1, Q1 in ((0.0, 1.0, 0.5), (0.5, 1.0, 0.5), (0.8, 1.0, 0.5), (0.9, 1.0, 0.5), (1.0, 1.0, 0.5), (0.5, 0.0, 0.5), (0.5, 0.75, 0.5), (0.5, 1.0, 0.0), (0.5, 1.0, 1.0)):
        _, d = tf(*P, {**flat, "F1": F1, "G1": G1, "Q1": Q1}, sr); g, fp, q = peak(f, d - base, 30, 0.42 * sr)
        nom = 8.1758 * 2 ** ((119.589 * F1 + 15.487) / 12); fi = sr * 2 if sr < 50000 else sr; pred = fi / np.pi * np.arctan(np.pi * nom / fi)
        print(f"{sr}\t{F1}\t{G1}\t{Q1}\t{g:.2f}\t{fp:.0f}\t{q:.2f}\t{nom:.0f}\t{pred:.0f}")
Gq = ("ngrc_EQGraphic_doc0.xml", "EQ Graphic")
print("# EQ Graphic: defaults (Min Freq 0.5, Max Freq 0.5, Gain Range 0.5); one band at maximum: peak; then all eight at maximum: min..max between the outer centres")
print("fs\tsetting\tpeak_dB\tpeak_Hz\tQ_halfgain")
sr = 48000; f, base = tf(*Gq, {}, sr); cs = []
for b in range(1, 9):
    _, d = tf(*Gq, {f"b{b}": 1.0}, sr); g, fp, q = peak(f, d - base, 30, 0.42 * sr); cs.append(fp); print(f"{sr}\tb{b}=1\t{g:.2f}\t{fp:.0f}\t{q:.2f}")
_, d = tf(*Gq, {f"b{b}": 1.0 for b in range(1, 9)}, sr); k = (f >= cs[0]) & (f <= cs[-1]); e = (d - base)[k]
print(f"{sr}\tall=1\tmax {e.max():.2f}\tmin {e.min():.2f}\tripple {e.max() - e.min():.2f}")
