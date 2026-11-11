#!/usr/bin/env python3
"""Compressor black-box helpers (numpy only). Complements tools/re/sig.py.

    cb.py gen-tb out.wav --freq 1000 --lo -40 --hi -10 --pre 1 --on 0.5 --post 3
    cb.py gen-fsteps out.wav --db -10 --freqs 40,60,100,200,500,1000,3000,8000 --hold 1.5
    cb.py timing in.wav out.wav --freq 1000 --pre 1 --on 0.5      attack/release metrics of the gain trace
    cb.py trace  in.wav out.wav --freq 1000 [--step-ms 5]         gain trace (dB) against time
    cb.py fgain  in.wav out.wav --freqs ... --hold 1.5            gain per stepped frequency (last third)
    cb.py ripple in.wav out.wav --freq 100                        gain ripple within a cycle (steady tone)

Gain trace: sliding RMS over exactly one carrier period of out and of in, ratio in dB,
after aligning out to in by the cross-correlation lag (plugin latency).
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.environ["RE"]))
import sig  # noqa: E402


def amp(d):
    return 10.0 ** (np.asarray(d, dtype=float) / 20.0)


def db(x):
    return 20.0 * np.log10(np.maximum(np.abs(x), 1e-12))


def gen_tb(a):
    sr = a.sr
    n = [int(a.pre * sr), int(a.on * sr), int(a.post * sr)]
    env = np.concatenate([np.full(n[0], amp(a.lo)), np.full(n[1], amp(a.hi)), np.full(n[2], amp(a.lo))])
    x = env * np.sin(2 * np.pi * a.freq * np.arange(len(env)) / sr)
    sig.write_wav(a.out, np.vstack([x, x]) if a.stereo else x, sr)


def gen_fsteps(a):
    sr = a.sr
    freqs = [float(f) for f in a.freqs.split(",")]
    n = int(a.hold * sr)
    parts = []
    for f in freqs:
        t = np.arange(n) / sr
        w = np.ones(n)
        k = int(0.01 * sr)
        w[:k] = np.linspace(0, 1, k)
        w[-k:] = np.linspace(1, 0, k)
        parts.append(amp(a.db) * w * np.sin(2 * np.pi * f * t))
    x = np.concatenate(parts)
    sig.write_wav(a.out, np.vstack([x, x]) if a.stereo else x, sr)


def load(a):
    x, sr = sig.read_wav(a.inp)
    y, sr2 = sig.read_wav(a.out)
    x = x[0]
    y = y[min(a.ch, y.shape[0] - 1)]
    n = min(len(x), len(y))
    x, y = x[:n], y[:n]
    lag = int(a.lag)
    if lag > 0:
        y = np.concatenate([y[lag:], np.zeros(lag)])
    return x, y, sr, lag


def lagcmd(a):
    """Latency in samples from a broadband (noise) pair: argmax of the cross-correlation."""
    x, sr = sig.read_wav(a.inp)
    y, _ = sig.read_wav(a.out)
    x, y = x[0], y[min(a.ch, y.shape[0] - 1)]
    n = min(len(x), len(y), sr * 2)
    x, y = x[:n], y[:n]
    f = np.fft.rfft(x, 2 * n); g = np.fft.rfft(y, 2 * n)
    c = np.fft.irfft(np.conj(f) * g)
    k = int(np.argmax(np.abs(c[:4096])))
    print(f"lag_smp\t{k}\tpolarity\t{'+' if c[k] > 0 else '-'}")


def gain_trace(x, y, sr, freq):
    p = max(2, int(round(sr / freq)))
    def srms(v):
        c = np.concatenate([[0.0], np.cumsum(v * v)])
        return np.sqrt(np.maximum((c[p:] - c[:-p]) / p, 1e-30))
    gx, gy = srms(x), srms(y)
    g = db(gy) - db(gx)
    # sample i of g covers [i, i+p); stamp it at its end
    t = (np.arange(len(g)) + p) / sr
    return t, g, db(gx) + 3.01, db(gy) + 3.01


def cross(t, g, t0, t1, target, rising):
    m = (t >= t0) & (t <= t1)
    tt, gg = t[m], g[m]
    idx = np.nonzero(gg >= target)[0] if rising else np.nonzero(gg <= target)[0]
    return (tt[idx[0]] - t0) if len(idx) else float("nan")


def timing(a):
    x, y, sr, lag = load(a)
    t, g, lx, ly = gain_trace(x, y, sr, a.freq)
    p = 1.0 / a.freq
    t_on, t_off, t_end = a.pre, a.pre + a.on, len(x) / sr
    def mean(t0, t1):
        m = (t >= t0) & (t <= t1)
        return float(np.mean(g[m]))
    g_pre = mean(t_on - 0.1, t_on - 0.001)
    g_on = mean(t_off - min(0.05, a.on / 4), t_off - 0.0005)
    g_post = mean(t_end - 0.1, t_end - 0.001)
    m = (t >= t_on + p) & (t <= t_off)
    g_min = float(np.min(g[m]))
    t_min = float(t[m][np.argmin(g[m])] - t_on)
    da = g_on - g_pre          # negative for compression
    out = {"lag_smp": lag, "g_pre_db": g_pre, "g_on_db": g_on, "g_min_db": g_min, "t_min_ms": 1e3 * t_min,
           "g_post_db": g_post}
    for frac in (0.5, 0.63, 0.9):
        out[f"att_t{int(frac*100)}_ms"] = 1e3 * cross(t, g, t_on, t_off, g_pre + frac * da, da > 0)
    # gain at fixed times into the burst
    for ms in (1, 3, 10, 30, 100):
        i = int(np.searchsorted(t, t_on + ms / 1e3))
        out[f"g_on+{ms}ms"] = float(g[min(i, len(g) - 1)])
    dr = g_post - g_on         # positive for recovery
    # the first full period after the offset is contaminated by the level step
    for frac in (0.1, 0.5, 0.63, 0.9):
        out[f"rel_t{int(frac*100)}_ms"] = 1e3 * cross(t, g, t_off + p, t_end, g_on + frac * dr, dr > 0) + 1e3 * p
    for ms in (10, 50, 200, 1000, 3000):
        if t_off + ms / 1e3 < t_end - 0.01:
            i = int(np.searchsorted(t, t_off + ms / 1e3))
            out[f"g_off+{ms}ms"] = float(g[min(i, len(g) - 1)])
    print("metric\tvalue")
    for k, v in out.items():
        print(f"{k}\t{v:.3f}" if isinstance(v, float) else f"{k}\t{v}")


def trace(a):
    x, y, sr, lag = load(a)
    t, g, lx, ly = gain_trace(x, y, sr, a.freq)
    step = max(1, int(sr * a.step_ms / 1000.0))
    print("time_s\tin_db\tout_db\tgain_db")
    lo, hi = int(a.t0 * sr), int(a.t1 * sr) if a.t1 > 0 else len(g)
    for i in range(lo, min(hi, len(g)), step):
        print(f"{t[i]:.5f}\t{lx[i]:.2f}\t{ly[i]:.2f}\t{g[i]:.3f}")


def fgain(a):
    x, y, sr, lag = load(a)
    freqs = [float(f) for f in a.freqs.split(",")]
    n = int(a.hold * sr)
    print("freq_hz\tin_db\tout_db\tgain_db")
    for k, f in enumerate(freqs):
        seg = slice(k * n + (2 * n) // 3, (k + 1) * n - n // 20)
        xi, yo = np.sqrt(np.mean(x[seg] ** 2)), np.sqrt(np.mean(y[seg] ** 2))
        print(f"{f:g}\t{db(xi)+3.01:.2f}\t{db(yo)+3.01:.2f}\t{db(yo)-db(xi):.2f}")


def ripple(a):
    """Peak-to-peak variation of the instantaneous gain |y|/|x| within a cycle of a steady tone."""
    x, y, sr, lag = load(a)
    n = len(x)
    seg = slice(n // 2, n - n // 8)
    xs, ys = x[seg], y[seg]
    m = np.abs(xs) > 0.5 * np.max(np.abs(xs))
    g = db(ys[m]) - db(xs[m])
    print("metric\tvalue")
    print(f"lag_smp\t{lag}")
    print(f"gain_median_db\t{np.median(g):.3f}")
    print(f"gain_p05_db\t{np.percentile(g, 5):.3f}")
    print(f"gain_p95_db\t{np.percentile(g, 95):.3f}")
    print(f"ripple_p95_p05_db\t{np.percentile(g, 95) - np.percentile(g, 5):.3f}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen-tb"); g.set_defaults(fn=gen_tb)
    g.add_argument("out"); g.add_argument("--sr", type=int, default=48000)
    g.add_argument("--freq", type=float, default=1000.0); g.add_argument("--lo", type=float, default=-40.0)
    g.add_argument("--hi", type=float, default=-10.0); g.add_argument("--pre", type=float, default=1.0)
    g.add_argument("--on", type=float, default=0.5); g.add_argument("--post", type=float, default=3.0)
    g.add_argument("--stereo", action="store_true")
    g = sub.add_parser("gen-fsteps"); g.set_defaults(fn=gen_fsteps)
    g.add_argument("out"); g.add_argument("--sr", type=int, default=48000)
    g.add_argument("--db", type=float, default=-10.0); g.add_argument("--freqs", default="40,60,100,200,500,1000,3000,8000")
    g.add_argument("--hold", type=float, default=1.5); g.add_argument("--stereo", action="store_true")
    for name, fn in (("timing", timing), ("trace", trace), ("fgain", fgain), ("ripple", ripple), ("lag", lagcmd)):
        p = sub.add_parser(name); p.set_defaults(fn=fn)
        p.add_argument("inp"); p.add_argument("out"); p.add_argument("--ch", type=int, default=0)
        p.add_argument("--freq", type=float, default=1000.0); p.add_argument("--lag", type=int, default=0)
        p.add_argument("--pre", type=float, default=1.0); p.add_argument("--on", type=float, default=0.5)
        p.add_argument("--step-ms", dest="step_ms", type=float, default=5.0)
        p.add_argument("--t0", type=float, default=0.0); p.add_argument("--t1", type=float, default=-1.0)
        p.add_argument("--freqs", default="40,60,100,200,500,1000,3000,8000"); p.add_argument("--hold", type=float, default=1.5)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
