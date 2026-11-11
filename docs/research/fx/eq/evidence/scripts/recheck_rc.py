#!/usr/bin/env python3
"""Independent recheck of helper measurements (EQ orchestrator). Noise pair (-30 dBFS, 6 s), Welch |H|, relative to a flat render of the same unit and rate.
   python3 rc.py   -> prints the table; each row's auhost --set list is in the label."""
import os, subprocess, sys
import numpy as np
RE = os.environ["RE"]; RS = os.environ["RE_STATE"]; sys.path.insert(0, RE)
import sig
W = os.path.join(RS, "work/eq/recheck")
def noise(sr):
    p = os.path.join(W, f"n{sr}.wav")
    if not os.path.exists(p):
        rng = np.random.default_rng(7); sig.write_wav(p, (rng.standard_normal(sr * 6) * sig.amp(-30)).astype(np.float32), sr)
    return p
def tf(au, sets, sr):
    out = os.path.join(W, "o.wav"); cmd = [os.path.join(RS, "bin/auhost"), "render", *au, noise(sr), out, "--warmup", "0.5"]
    for k, v in sets.items(): cmd += ["--set", f"{k}={v}"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if r.returncode: raise RuntimeError(r.stderr[-300:])
    x, _ = sig.read_wav(noise(sr)); y, _ = sig.read_wav(out); n = min(x.shape[1], y.shape[1])
    f, h, _ = sig.welch_tf(x[0][:n], y[0][:n], sr, 16384 if sr < 60000 else 32768)
    return f, 20 * np.log10(np.abs(h) + 1e-30)
def rel(au, base, sets, sr, grid):
    f, a = tf(au, base, sr); _, b = tf(au, {**base, **sets}, sr); d = b - a
    return [float(np.mean(d[(f > q * 0.97) & (f < q * 1.03)])) for q in grid]
G = [5000, 8000, 10000, 12000, 14000, 16000, 18000, 20000]
print("case\tsr\t" + "\t".join(map(str, G)))
BX = ("aumf", "bcfs", "Brwx"); bxbase = {4: 0, 5: 0, 11: 0, 12: 0, 25: 0, 33: 0, 44: 0}
for sr in (44100, 96000):
    print(f"bx_console HMF x3, f max, Q min, gain max (60=1,59=1,58=0,61=1)\t{sr}\t" + "\t".join(f"{v:.2f}" for v in rel(BX, bxbase, {60: 1, 59: 1, 58: 0, 61: 1}, sr, G)))
EQP = ("aufx", "U3AN", "UADx")
for sr in (44100, 96000):
    print(f"UADx EQP-1A HF 16k boost 10 sharp (51=6,52=1,53=0)\t{sr}\t" + "\t".join(f"{v:.2f}" for v in rel(EQP, {}, {51: 6, 52: 1, 53: 0}, sr, G)))
FRG = ("aufx", "SPFE", "SPL1")
for sr in (44100, 96000):
    print(f"SPL Free Ranger 16k band max (5=1)\t{sr}\t" + "\t".join(f"{v:.2f}" for v in rel(FRG, {}, {5: 1}, sr, G)))
L = [20, 30, 40, 60, 80, 100, 150, 200, 300, 500, 800, 1000, 2000]
print("case\tsr\t" + "\t".join(map(str, L)))
for lab, s in (("EQP-1A LF 60, boost 10 (48=2,49=1)", {48: 2, 49: 1}), ("EQP-1A LF 60, atten 10 (48=2,50=1)", {48: 2, 50: 1}), ("EQP-1A LF 60, boost 10 + atten 10 (48=2,49=1,50=1)", {48: 2, 49: 1, 50: 1})):
    print(f"{lab}\t48000\t" + "\t".join(f"{v:.2f}" for v in rel(EQP, {}, s, 48000, L)))
