#!/usr/bin/env python3
"""rate_limit.py: why the frozen magnitude response moves with the sample rate, and what closes it.

    python3 rate_limit.py > rate_limit.txt        (about 15 minutes, three processes)

Everything is closed form: a frozen voicing is linear and time-invariant, and on the unit circle a
TPT one-pole is its analog section with s = j tan(pi f / fs) and g its coefficient, so the rendered
response is the prototype's equations (spec_check.py) at the prototype's table rows. The metric is
check_phaser.py's "P9 rate: frozen response": |H| at 44.1 / 88.2 / 96 / 192 kHz against the same
voicing at 48 kHz, 20 Hz to 10 kHz on a 1 % grid, where the 48 kHz response is within 20 dB of its
peak, at sweep positions -1, -0.5, 0, 0.5, 1; worst dB. Sections:

  1  the metric for the prototype's tables (reproduces the bench row from the equations)
  2  the error of each rate against the analog response, split into the four stages (lag and
     Color network made analog) and the rest (stages made analog): which part causes it
  3  the wet path's phase error at 9.94 kHz, top of the sweep: sign and size
  4  the best the structure can do: the four stage coefficients (and the Phase 90 lag) free per
     sweep position and rate, fitted to the analog |H| (L8 norm on dB, floor-clipped), then the metric
  5  four stages refitted together with one second-order correction section C(z) on the wet output,
     outside every loop (DC gain 1, poles within radius 0.95), its gain above 10 kHz limited to
     L dB, for L = 0, 1, 3, 6: the metric and the wet path's boost above 10 kHz

Fits: Levenberg-Marquardt with a forward-difference Jacobian, L8 on dB error (residual
sign(e)|e|^4), dB clipped below 25 dB under the analog peak on both sides so the notch floors do
not dominate, |C| above 10 kHz as a hinge penalty. Fixed start points, no randomness.
"""
import importlib.util
import math
import os
import sys
from multiprocessing import Pool

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("phaser_ref_rl", os.path.join(HERE, "..", "..", "prototype", "phaser_ref.py"))
P = importlib.util.module_from_spec(spec)
spec.loader.exec_module(P)

SRS = (44100, 48000, 88200, 96000, 192000)
WS = (-1.0, -0.5, 0.0, 0.5, 1.0)
VOICINGS = ("script", "block", "off", "on")
NAMES = {"script": "Phase 90 script", "block": "Phase 90 block", "off": "Small Stone off", "on": "Small Stone on"}
F = 20.0 * 1.01 ** np.arange(int(math.log(10000.0 / 20.0) / math.log(1.01)) + 1)   # check_phaser.py's grid
DRY = {"script": 0.5, "block": 0.5, "off": P.SS_OFF["dry"], "on": P.SS_ON["dry"]}
FLOOR_DB = 25.0
R_MAX = 0.95


def fc_of(v, w):
    return float(P.phase90_fc(w)) if v in ("script", "block") else float(P.smallstone_fc(w, v == "on"))


def table_row(v, fs, w):
    G = P.coeff_table(v, fs)[int(round((w + 1) * P.TABLE_N / 2))]
    g = G / (1.0 - G)
    if v != "block":
        g[4] = 1.0     # unused
    return g


def response(v, f, fs, g, fc, stages_analog=False, rest_analog=False):
    """Complex H, no make-up. Stages and the rest (lag, Color network) each analog or TPT."""
    sd, sa = 1j * np.tan(np.pi * f / fs), 1j * f
    ss = sa if stages_analog else sd
    gs = (fc,) * 4 if stages_analog else tuple(g[:4])
    so = sa if rest_analog else sd
    ap = lambda s, k: (s - k) / (s + k)   # noqa: E731
    lp = lambda s, k: k / (k + s)         # noqa: E731
    p4 = ap(ss, gs[0]) * ap(ss, gs[1]) * ap(ss, gs[2]) * ap(ss, gs[3])
    if v == "script":
        return 0.5 + 0.5 * p4
    if v == "off":
        return P.SS_OFF["dry"] + P.SS_OFF["wet"] * p4
    if v == "block":
        b = P.P90_BETA
        gl = P.P90_FB_LAG_HZ if rest_analog else g[4]
        a34 = ap(ss, gs[2]) * ap(ss, gs[3])
        s2 = ((1 + b) * ss - gs[1]) / (ss + gs[1])
        return 0.5 + 0.5 * ap(ss, gs[0]) * s2 * a34 / (1 + b * lp(so, gl) * a34)
    p = P.SS_ON
    ka, kb, kf = 1 - p["f_za"] / p["f_p1"], 1 - p["f_zb"] / p["f_p2"], 1 - p["f_zf"] / p["f_p2"]
    g1, g2 = (p["f_p1"], p["f_p2"]) if rest_analog else (math.tan(math.pi * p["f_p1"] / fs), math.tan(math.pi * p["f_p2"] / fs))
    ha = (1 - ka * lp(so, g1)) * (1 - kb * lp(so, g2))
    fb = (1 - lp(so, g1)) * (1 - kf * lp(so, g2))
    return p["dry"] + p["wet"] * ha * p4 / (1 - p["c"] * fb * p4)


def db(h):
    return 20 * np.log10(np.maximum(np.abs(h), 1e-300))


def correction(z, c):
    """C(z) = k (1 + b1/z + b2/z^2) / (1 + a1/z + a2/z^2), k so that C(1) = 1; c = (b1, b2, a1, a2)."""
    b1, b2, a1, a2 = c
    zi = 1 / z
    return (1 + b1 * zi + b2 * zi * zi) / (1 + a1 * zi + a2 * zi * zi) * (1 + a1 + a2) / (1 + b1 + b2)


def p9_metric(resp):
    """resp[fs][i]: dB on F at WS[i] -> (worst, {fs: worst})."""
    per = {}
    for i in range(len(WS)):
        ref = resp[48000][i]
        mask = ref >= ref.max() - 20.0
        for fs in SRS:
            if fs != 48000:
                per[fs] = max(per.get(fs, 0.0), float(np.max(np.abs(resp[fs][i] - ref)[mask])))
    return max(per.values()), per


def lm(res, x, iters=300):
    x = np.array(x, float)
    r = res(x)
    lam = 1e-3
    for _ in range(iters):
        J = np.empty((len(r), len(x)))
        for j in range(len(x)):
            d = np.zeros(len(x))
            d[j] = 1e-7
            J[:, j] = (res(x + d) - r) / 1e-7
        A, gr = J.T @ J, J.T @ r
        while True:
            dx = np.linalg.solve(A + lam * np.diag(np.diag(A) + 1e-12), gr)
            rn = res(x - dx)
            if np.all(np.isfinite(rn)) and rn @ rn < r @ r:
                x, r = x - dx, rn
                lam = max(lam / 3, 1e-12)
                break
            lam *= 4
            if lam > 1e12:
                return x
    return x


def fit(v, fs, w, limit_db=None):
    """Section 4 (limit_db None: stage coefficients only) or 5 (with C, |C| <= limit_db above 10 kHz).
    -> (dB on F of the fitted response, worst |dB error| against analog over the clipped band,
        largest |C| in dB above 10 kHz)."""
    fc = fc_of(v, w)
    nst = 5 if v == "block" else 4
    g0 = table_row(v, fs, w)
    ha = db(response(v, F, fs, g0, fc, True, True))
    floor = ha.max() - FLOOR_DB
    hac = np.maximum(ha, floor)
    z = np.exp(2j * np.pi * F / fs)
    zc = np.exp(2j * np.pi * np.linspace(10000.0, 0.4995 * fs, 120) / fs)
    dry = DRY[v]

    def h_of(x):
        g = g0.copy()
        g[:nst] = g0[:nst] * np.exp(x[:nst])
        h = response(v, F, fs, g, fc)
        return h if limit_db is None else dry + (h - dry) * correction(z, x[nst:])

    def ok(x):
        return limit_db is None or np.all(np.abs(np.roots([1.0, x[nst + 2], x[nst + 3]])) <= R_MAX)

    def res(x):
        if not ok(x):
            return np.full(len(F) + (0 if limit_db is None else len(zc)), 1e3)
        e = np.maximum(db(h_of(x)), floor) - hac
        r = np.sign(e) * e ** 4
        if limit_db is None:
            return r
        pen = 3.0 * np.maximum(0.0, db(correction(zc, x[nst:])) - limit_db)
        return np.concatenate([r, pen ** 4])

    if limit_db is None:
        starts = [np.zeros(nst)]
    else:
        starts = [np.concatenate([np.zeros(nst), [-2 * rz * math.cos(th), rz * rz, -2 * rp * math.cos(th), rp * rp]])
                  for rp in (0.3, 0.7) for th in (2.2, 2.9) for rz in (0.2, 0.6)]
    best = None
    for x0 in starts:
        x = lm(res, x0)
        if not ok(x):
            continue
        hd = db(h_of(x))
        e = float(np.max(np.abs(np.maximum(hd, floor) - hac)))
        if best is None or e < best[1]:
            boost = 0.0 if limit_db is None else float(np.max(db(correction(zc, x[nst:]))))
            best = (hd, e, boost)
    return best


def fit_job(args):
    v, fs, w, limit_db = args
    return args, fit(v, fs, w, limit_db)


def main():
    print("# rate_limit.py: the P9 frozen-response row from the prototype's equations (see the docstring).")
    print("# dB figures are worst |dB| differences; 'metric' = against the 48 kHz render, the bench row's measure.")

    print("\n## 1  the metric for the prototype's tables")
    for v in VOICINGS:
        resp = {fs: [db(response(v, F, fs, table_row(v, fs, w), fc_of(v, w))) for w in WS] for fs in SRS}
        worst, per = p9_metric(resp)
        print(f"{NAMES[v]}\tmetric {worst:.3f} dB\t" + "  ".join(f"{fs / 1000:g}k {d:.3f}" for fs, d in per.items()))

    print("\n## 2  against the analog response, in the P9 mask of the analog response: all / stages only / lag or Color only")
    print("voicing\tfs\t" + "\t".join(f"w {w:+.1f}" for w in WS))
    for v in VOICINGS:
        for fs in SRS:
            cells = []
            for w in WS:
                fc, g = fc_of(v, w), table_row(v, fs, w)
                ha = db(response(v, F, fs, g, fc, True, True))
                m = ha >= ha.max() - 20.0
                e = [float(np.max(np.abs(db(response(v, F, fs, g, fc, sa, ra)) - ha)[m]))
                     for sa, ra in ((False, False), (False, True), (True, False))]
                cells.append(f"{e[0]:.3f} / {e[1]:.3f} / {e[2]:.3f}")
            print(f"{NAMES[v]}\t{fs}\t" + "\t".join(cells))

    print("\n## 3  wet-path phase, digital minus analog, at 9.94 kHz, sweep top (negative: the digital chain lags)")
    f = np.array([F[-1]])
    for v in VOICINGS:
        fc = fc_of(v, 1.0)
        cells = []
        for fs in SRS:
            g = table_row(v, fs, 1.0)
            d = (response(v, f, fs, g, fc) - DRY[v]) / (response(v, f, fs, g, fc, True, True) - DRY[v])
            cells.append(f"{fs / 1000:g}k {math.degrees(float(np.angle(d[0]))):+.2f} deg")
        print(f"{NAMES[v]}\t" + "  ".join(cells))

    jobs = [(v, fs, w, None) for v in VOICINGS for fs in SRS for w in WS]
    limits = (0.0, 1.0, 3.0, 6.0)
    jobs += [(v, fs, w, L) for L in limits for v in VOICINGS for fs in SRS for w in WS]
    with Pool(3) as pool:
        out = dict(pool.map(fit_job, jobs))

    print("\n## 4  stage coefficients (and lag) free per position and rate, fitted to the analog |H|")
    for v in VOICINGS:
        resp = {fs: [out[(v, fs, w, None)][0] for w in WS] for fs in SRS}
        worst, per = p9_metric(resp)
        va = {fs: max(out[(v, fs, w, None)][1] for w in WS) for fs in SRS}
        print(f"{NAMES[v]}\tmetric {worst:.3f} dB\t" + "  ".join(f"{fs / 1000:g}k {d:.3f}" for fs, d in per.items())
              + "\tagainst analog: " + "  ".join(f"{fs / 1000:g}k {e:.3f}" for fs, e in va.items()))

    print("\n## 5  stages refitted with a correction section C(z) on the wet output, |C| <= L dB above 10 kHz")
    for L in limits:
        for v in VOICINGS:
            resp = {fs: [out[(v, fs, w, L)][0] for w in WS] for fs in SRS}
            worst, per = p9_metric(resp)
            boost = {fs: max(out[(v, fs, w, L)][2] for w in WS) for fs in SRS}
            print(f"L {L:g} dB\t{NAMES[v]}\tmetric {worst:.3f} dB\t" + "  ".join(f"{fs / 1000:g}k {d:.3f}" for fs, d in per.items())
                  + "\t|C| above 10 kHz up to: " + "  ".join(f"{fs / 1000:g}k {b:+.1f}" for fs, b in boost.items()))


if __name__ == "__main__":
    main()
