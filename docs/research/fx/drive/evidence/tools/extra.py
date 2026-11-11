#!/usr/bin/env python3
"""extra.py: latency (impulse peak position), idle noise and render cost per model. Writes $W/meas/extra.tsv."""
import os, sys, time
import numpy as np
sys.path.insert(0, os.environ["RE"]); sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import meas
from models import MODELS
W = os.environ["W"]; SR = 48000
models = sys.argv[1:] or open(os.path.join(W, "meas_all.txt")).read().split()
imp = np.zeros(SR); imp[4800] = 0.01; sil = np.zeros(2 * SR)
rng = np.random.default_rng(3); long = 0.05 * rng.standard_normal(30 * SR)
rows = []
for m in models:
    dk = MODELS[m]["drive"]; dv = MODELS[m]["drives"][1]
    y, cmd = meas.render(m, {dk: dv}, meas.wav_in("imp.wav", imp), "x"); pk = int(np.argmax(np.abs(y))) - 4800
    # centre of energy of the response (samples after the impulse)
    e = y[4700:9600] ** 2; cen = float(np.sum(np.arange(len(e)) * e) / max(np.sum(e), 1e-30)) - 100
    z, _ = meas.render(m, {dk: MODELS[m]["drives"][-1]}, meas.wav_in("sil.wav", sil), "x"); noise = meas.db(float(np.sqrt(np.mean(z[SR:] ** 2))))
    xin = meas.wav_in("long.wav", long); t0 = time.time(); meas.render(m, {dk: dv}, xin, "x"); t1 = time.time() - t0
    t0 = time.time(); meas.render(m, {dk: dv}, meas.wav_in("short.wav", long[:SR // 10]), "x"); t2 = time.time() - t0
    rows.append([m, pk, f"{cen:.1f}", f"{noise:.1f}", f"{100 * (t1 - t2) / 30:.3f}"])
with open(os.path.join(W, "meas", "extra.tsv"), "w") as fh:
    fh.write("# extra.py: impulse 0.01 at sample 4800 (48 kHz) -> peak offset and energy centroid in samples; silence in -> output RMS at max drive;\n# cost = (wall time for 30 s of noise - wall time for 0.1 s) / 30 s, in percent of real time, single core, includes file I/O and host overhead\n")
    fh.write("model\timpulse_peak_offset_samples\tenergy_centroid_samples\tidle_noise_dbfs\tcost_pct_realtime\n")
    for r in rows: fh.write("\t".join(str(c) for c in r) + "\n")
for r in rows: print("\t".join(str(c) for c in r))
