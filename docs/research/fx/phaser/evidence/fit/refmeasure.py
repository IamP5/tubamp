#!/usr/bin/env python3
"""refmeasure.py: the measures that compare a phaser's motion with Guitar Rig 6 (numpy only).

Shared by reftakes.py (which renders Guitar Rig 6 and stores the reference) and
tools/fxbench/check_phaser.py (which renders ours and compares):

  guitar(n, sr, seed, ...)   a guitar-like take: plucked notes of 12 partials (1/k roll-off with a
                             pick-position comb, slight inharmonicity, 4 ms attack, partial-dependent
                             decay), single notes with a three-note chord every fifth, off-grid
                             pitches. The round-2 verifier's generator (r2lib.guitar), unchanged.
  sine(f, n, sr, db)         a held note.
  cents(y, f0, sr)           instantaneous-frequency deviation of a held note in cents: an FFT
                             band-pass f0 +- 0.3 f0 with raised-cosine skirts, the analytic signal,
                             the derivative of its unwrapped phase; frames where the envelope is
                             below 0.35 of its median are dropped (a notch on the note), and 0.5 s
                             at either end. -> (p99 of |cents|, rms cents).
  null(ours, ref, sr, lat)   residual of ours against the reference, one least-squares gain, the
                             reference delayed by lat samples, the first 0.5 s skipped; dB re the
                             reference.
"""
import math

import numpy as np


def guitar(n, sr=48000, seed=7, peak_db=-6.0, notes=None, gap=0.45):
    rng = np.random.default_rng(seed); t = np.arange(n) / sr; y = np.zeros(n)
    base = notes or [82.41, 110.0, 146.83, 196.0, 246.94, 329.63, 440.0, 587.33, 123.47, 164.81]
    k = 0; t0 = 0.02
    while t0 < n / sr - 0.1:
        chord = (k % 5 == 4)
        fs_ = [base[k % len(base)]] if not chord else [base[(k + j * 2) % len(base)] for j in range(3)]
        for f0 in fs_:
            f0 = f0 * (1 + 0.003 * rng.standard_normal())
            tt = t - t0 - (0.004 * rng.random() if chord else 0); m = tt >= 0
            for h in range(1, 13):
                fh = f0 * h * np.sqrt(1 + 2e-4 * h * h)
                if fh > 0.45 * sr: break
                a = (1.0 / h) * abs(np.sin(np.pi * h * 0.21))
                env = (1 - np.exp(-tt[m] / 0.004)) * np.exp(-tt[m] / (1.2 / (1 + 0.25 * h)))
                y[m] += a * env * np.sin(2 * np.pi * fh * tt[m] + rng.random() * 6.28)
        t0 += gap * (1 + 0.2 * rng.random()); k += 1
    return (y / np.max(np.abs(y)) * 10 ** (peak_db / 20)).astype(np.float32).astype(np.float64)


def sine(f0, n, sr=48000, db=-12.0):
    return (10 ** (db / 20) * np.sin(2 * np.pi * f0 * np.arange(n) / sr)).astype(np.float32).astype(np.float64)


def cents(y, f0, sr=48000):
    y = np.asarray(y, float); n = len(y)
    f = np.fft.fftfreq(n, 1.0 / sr)
    bw = 0.3 * f0; skirt = 0.1 * f0
    a = np.abs(np.abs(f) - f0)
    gain = np.where(a <= bw, 1.0, np.where(a <= bw + skirt, 0.5 + 0.5 * np.cos(np.pi * (a - bw) / skirt), 0.0))
    gain = gain * (f > 0) * 2.0                       # one-sided: the analytic signal of the band
    z = np.fft.ifft(np.fft.fft(y) * gain)
    ph = np.unwrap(np.angle(z)); fi = np.gradient(ph) * sr / (2 * np.pi)
    c = 1200 * np.log2(np.maximum(fi, 1e-3) / f0); env = np.abs(z)
    s = slice(sr // 2, n - sr // 2)
    c, env = c[s], env[s]
    keep = env > 0.35 * np.median(env)
    return float(np.percentile(np.abs(c[keep]), 99)), float(np.sqrt(np.mean(c[keep] ** 2)))


def null(ours, ref, sr=48000, lat=16):
    n = min(len(ours), len(ref) - lat)
    a = np.asarray(ours[sr // 2:n - 64], float); b = np.asarray(ref[sr // 2 + lat:n - 64 + lat], float)
    k = np.dot(a, b) / np.dot(a, a)
    return 10 * math.log10(np.sum((k * a - b) ** 2) / np.sum(b * b)), 20 * math.log10(abs(k))
