#!/usr/bin/env python3
"""Numerical prototype of the proposed tubamp TS-style overdrive (type 1), to check it against the references.
Circuit values from Yeh/Abel/Smith DAFx-07 (R1 4.7k, Cz 47n, Cc 51p, R2 = 51k + D*500k; tone: Rs 1k, Cs 0.22u,
Ri 10k, Rf 1k, pot 20k, Rz 220, Cz 0.22u) and a Shockley diode pair (Is 2.52e-9 A, n*Vt 45.3 mV).
Steady state on periodic 1 s signals at 48 kHz; the clipper is static, run at 4x with first-order ADAA on a
piecewise-linear table (the scheme proposed in the report).
    ts_proto.py > ts_proto.tsv
"""
import math, sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from alias_schemes import stage, up2, down2, SR

R1, CZ, CC, IS, NVT = 4.7e3, 47e-9, 51e-12, 2.52e-9, 45.3e-3
ITERS = 4
V_REF = 1.0          # volts represented by digital full scale (0 dBFS peak = 1 V peak): our choice, see report
def r2(drive): return 51e3 + 500e3 * (drive ** 2)      # audio-ish taper: our choice

def clip_table(R2, n=1024, umax=40.0):
    """v(u) solving v + 2*Is*R2*sinh(v/nVt) = u on a uniform grid of u in [-umax, umax] volts (Newton)."""
    u = np.linspace(-umax, umax, n); k = 2 * IS * R2
    v = NVT * np.arcsinh(u / k)                       # start from the diode-only solution (always >= true |v|)
    v = np.sign(u) * np.minimum(np.abs(v), np.abs(u))
    for _ in range(60):
        f = v + k * np.sinh(v / NVT) - u; v = v - f / (1 + k / NVT * np.cosh(v / NVT))
    return u, v

def solve_v(u, k, iters):
    """v with v + k*sinh(v/nVt) = u. Start from an upper bound of |v| (min of |u| and the diode-only solution),
    so Newton on this convex function converges monotonically; `iters` fixed iterations."""
    v = np.sign(u) * np.minimum(np.abs(u), NVT * np.arcsinh(np.abs(u) / k))
    for _ in range(iters):
        v = v - (v + k * np.sinh(v / NVT) - u) / (1 + k / NVT * np.cosh(v / NVT))
    return v

def solver_adaa(x, R2, iters=4):
    """First-order ADAA without a table: with u(v) = v + k*sinh(v/a) explicit, integration by parts gives the exact
    antiderivative of the inverse, F(u) = u*v - v^2/2 - k*a*(cosh(v/a) - 1)."""
    k = 2 * IS * R2; v = solve_v(x, k, iters); F = x * v - 0.5 * v * v - k * NVT * (np.cosh(v / NVT) - 1)
    xp = np.roll(x, 1); Fp = np.roll(F, 1); d = x - xp; ok = np.abs(d) > 1e-6
    return np.where(ok, (F - Fp) / np.where(ok, d, 1), solve_v(0.5 * (x + xp), k, iters))

def table_adaa(x, u, v):
    """First-order ADAA of the piecewise-linear table (exact antiderivative: piecewise quadratic)."""
    du = u[1] - u[0]; slope = np.diff(v) / du
    Fk = np.concatenate([[0.0], np.cumsum(0.5 * (v[:-1] + v[1:]) * du)])
    def ev(z):
        zc = np.clip(z, u[0], u[-1] - 1e-9); i = np.floor((zc - u[0]) / du).astype(int); t = zc - u[i]
        f = v[i] + slope[i] * t; F = Fk[i] + v[i] * t + 0.5 * slope[i] * t * t
        return f, F
    f, F = ev(x); xp = np.roll(x, 1); fp, Fp = ev(xp); d = x - xp; ok = np.abs(d) > 1e-7
    fm, _ = ev(0.5 * (x + xp))
    return np.where(ok, (F - Fp) / np.where(ok, d, 1), fm)

def tone_H(f, T):
    s = 2j * np.pi * np.maximum(f, 1e-3); Rs, Cs, Ri, Rf, Rz, Cz2 = 1e3, 0.22e-6, 10e3, 1e3, 220.0, 0.22e-6
    Rl = max(T, 1e-3) * 20e3; Rr = max(1 - T, 1e-3) * 20e3; Yw = 1 / (Rz + 1 / (s * Cz2)); Gl, Gr = 1 / Rl, 1 / Rr
    k = Yw / (Gl + Gr + Yw)
    Va = (1 / Rs) / (1 / Rs + s * Cs + 1 / Ri + Gl * k)
    return Va * (1 + (Rf / Rr) * k)

def process(x, drive, tone, level_db=0.0, os_stages=2):
    N = len(x); f = np.fft.rfftfreq(N, 1 / SR); s = 2j * np.pi * f; R2 = r2(drive)
    X = np.fft.rfft(x * V_REF)
    hp_in = s / (s + 2 * np.pi * 15.9)
    lin = (R2 / R1) * (s * R1 * CZ / (1 + s * R1 * CZ)) / (1 + s * R2 * CC)     # current through the 4.7k/47n leg times R2||Cc
    xin = np.fft.irfft(X * hp_in, N); u = np.fft.irfft(X * hp_in * lin, N)
    des = [stage(n) for n in range(os_stages)]
    for up, _ in des: u = up2(u, up)
    v = solver_adaa(u, R2, ITERS)
    for _, dn in reversed(des): v = down2(v, dn)
    y = xin + v                                            # non-inverting stage: output = input + clipped term
    Y = np.fft.rfft(y) * tone_H(f, tone)
    return np.fft.irfft(Y, N) * 10 ** (level_db / 20) / V_REF

def spec(y):
    w = np.hanning(len(y)); return np.abs(np.fft.rfft(y * w)) / (w.sum() / 2)
def comp(s, f): return math.sqrt(float(np.sum(s[max(0, f - 2): f + 3] ** 2)) / 1.5)
def dB(v): return 20 * math.log10(max(v, 1e-12))

if __name__ == "__main__":
    n = np.arange(SR); sine = lambda f0, lvl: 10 ** (lvl / 20) * np.sin(2 * np.pi * f0 * n / SR)
    print("# ts_proto.py: proposed TS-style type, numerical prototype (static diode-feedback clipper, 4x + ADAA1)")
    print("## small-signal gain (dB) at -70 dBFS, tone 0.5")
    pts = [30, 50, 80, 110, 160, 220, 320, 450, 640, 720, 900, 1280, 1800, 2500, 3600, 5000, 7000, 10000, 14000, 18000]
    print("drive\t" + "\t".join(str(p) for p in pts))
    for d in (0.1, 0.5, 1.0):
        print(f"{d}\t" + "\t".join(f"{dB(comp(spec(process(sine(p, -70), d, 0.5)), p)) + 70:.1f}" for p in pts))
    print("## tone sweep, drive 0.5: gain at 720 Hz / 5 kHz"); 
    for T in (0.0, 0.5, 1.0):
        print(f"tone {T}\t" + "\t".join(f"{p}:{dB(comp(spec(process(sine(p, -70), 0.5, T)), p)) + 70:.1f}" for p in (110, 720, 2500, 5000, 10000)))
    print("## pre-clip emphasis: input level (dBFS) for 1 dB compression of the fundamental, drive 0.5")
    out = []
    for p in (60, 80, 110, 160, 220, 320, 450, 640, 900, 1280, 1800, 2500, 3600, 5000, 7000):
        g0 = dB(comp(spec(process(sine(p, -70), 0.5, 0.5)), p)) + 70; lo, hi = -70.0, 0.0
        for _ in range(11):
            m = 0.5 * (lo + hi); g = dB(comp(spec(process(sine(p, m), 0.5, 0.5)), p)) - m
            if g > g0 - 1: lo = m
            else: hi = m
        out.append((p, 0.5 * (lo + hi)))
    ref = min(v for _, v in out); print("\t".join(f"{p}:{ref - v:.1f}" for p, v in out))
    print("## harmonics 1 kHz and 110 Hz (rel dB to fundamental), tone 0.5")
    print("freq\tin_dbfs\tdrive\tfund_db\th2\th3\th5\tthd_pct")
    for f0 in (1000, 110):
        for lvl in (-30, -18, -6):
            for d in (0.1, 0.5, 1.0):
                s = spec(process(sine(f0, lvl), d, 0.5)); h = [comp(s, f0 * k) for k in range(1, 10)]
                print(f"{f0}\t{lvl}\t{d}\t{dB(h[0]):.2f}\t{dB(h[1]) - dB(h[0]):.1f}\t{dB(h[2]) - dB(h[0]):.1f}\t{dB(h[4]) - dB(h[0]):.1f}\t{100 * math.sqrt(sum(v * v for v in h[1:])) / h[0]:.2f}")
    print("## alias: 5 kHz sine -6 dBFS, drive 1.0, tone 0.5: worst inharmonic below 5 kHz rel fundamental, by scheme")
    for name, st in (("2x+ADAA1", 1), ("4x+ADAA1", 2), ("8x+ADAA1", 3)):
        s = spec(process(sine(5000, -6), 1.0, 0.5, os_stages=st)); fund = comp(s, 5000); mask = np.ones(len(s), bool); mask[:20] = False
        for k in range(1, 5): mask[5000 * k - 6: 5000 * k + 7] = False
        inh = np.where(mask, s, 0); w = int(np.argmax(inh[:4994]))
        print(f"{name}\tfund {dB(fund):.1f}\tworst {w} Hz {dB(comp(s, w)) - dB(fund):.1f} dB\tsum<20k {dB(math.sqrt(float(np.sum(inh[:20000] ** 2)) / 1.5)) - dB(fund):.1f} dB")
