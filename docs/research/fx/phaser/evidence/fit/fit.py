#!/usr/bin/env python3
"""fit.py: regenerate the fitted voicing constants from targets.tsv and trajectories.tsv (numpy only).

    python3 fit.py [--targets targets.tsv] [--traj trajectories.tsv] [--snapshots snapshots.tsv] [--skip-color] [--skip-range]

Phase 90 (block):
  1. beta (block feedback ratio) so that the closed-form block response peaks at the median
     measured resonance (peak_db of the p9_block rows; GR6 make-up already removed in targets.tsv).
  2. The sweep: JFET-leg law and LFO, fitted to the whole notch-1 trajectories of trajectories.tsv
     (six rates, 200 phase bins each), with the notch-1 sweep ends of targets.tsv as a penalty.
     Model = the prototype's own phase90_break_hz() (imported, constants patched) at a 4 kHz
     control rate:  LFO (RC-charge triangle, curvature kappa) -> one-pole gate smoothing gate_hz ->
         R(u) = (RS^m + (R1 exp(-gamma u))^m)^(1/m),  fc = (1/22k + 1/R(u)) / (2 pi 47n),
     and notch 1 = the analog block response's first minimum at that fc (table over fc, beta and the
     feedback lag as in the prototype). The prototype's discretisation puts its notches on the analog
     ones at every sample rate, so the analog notch is the rendered one. Free: RS, R1, gamma, m, kappa,
     gate_hz.
     Cost per rate: mean(e^2) + 30 mean(e^4) over the bins (e = ln model/measured, best common phase
     shift within +-0.03 cycle), plus 10 * ((ln end error)^4 / 1e-4) for each sweep end. Nelder-Mead from
     two starts (m = 1 and m = 3), the better is kept; both are printed.
     The impulse tracker reads smooth sweeps' trajectories to 0.05 % rms (selftest_traj.txt), so no
     tracker correction is applied.
  3. f_lag (one-pole lag in the feedback-resistor path) so that the median notch-1 depth over each
     measured sweep, computed in closed form (analog; the discretisation keeps the notch-1 depth
     analog) along the prototype's own break-frequency trajectory, matches depth_db on average over
     the p9_block rows (bisection). Without it the
     delay-free loop gives a frequency-independent -32.6 dB; Guitar Rig 6's notch fills in as it
     rises (-30.3 dB near 510 Hz to -27.2 dB near 1380 Hz at noon), the signature of a small
     delay in its feedback loop. Step 2's notch table uses the prototype's current f_lag (the lag
     moves notch 1 by < 0.2 %).
Small Stone, Color off, on the complex response (snapshots.tsv). Magnitudes alone cannot tell
  dry 0.75 + wet 0.50 from dry 0.50 + wet 0.75 (|a + b e^jt| = |b + a e^jt|), and the earlier fit to
  the median notch depth and peak picked the first; the phase tells them apart (round-2 verification).
  1. Structure from the snapshots of ss_off_r040 and ss_off_r050 (every third), model
         H = (dry + wet * s/(s + w_hp) * P^4) e^(-j w D),  P = (w_c - s)/(w_c + s)
     free: dry, wet, f_hp, the delay D (Guitar Rig's latency) and one break frequency per snapshot.
     Levenberg-Marquardt on the complex error, from the old weights (0.75 / 0.50) and from the swapped
     ones; the variant without the highpass is fitted too and printed for comparison.
  2. With the structure fixed, the break frequency of every snapshot of the three Color-off takes
     (golden section on the complex error).
  3. Sweep law against those break frequencies:
         ln fc = (ln f_lo + ln f_hi)/2 + ln(f_hi/f_lo)/2 * v + curve * (v^2 - 1)
     v the RC triangle at kappa = SS_KAPPA (shared with Color on), phase = phase0 + rate t, one phase0
     per take, t the snapshot's time plus D. Free: f_lo, f_hi, curve, the three phase0. Printed
     again with kappa free (it lands within its flat direction) and with curve = 0.
  4. Check: the Color-off trajectories of trajectories.tsv against notch 1 of the fitted analog
     response along the fitted law (the highpass moves notch 1 off tan(pi/8) fc near the bottom).
  Phase 90 block and Small Stone Color on are checked the same way (snapshots of p9_block_p010 and
  ss_on_r040, structure and constants fixed, dry and wet free): their weights and signs are printed
  against the prototype's.
Small Stone, Color on, in two steps.
  1. Structure constants from the six static frames (the `static` rows: frozen LFO, sweep
     turning points of three sweep windows). Model, the prototype's own analog equations in
     closed form:
         H = dry + wet * Ha * P^4 / (1 - c * F * P^4)
         Ha = (s + w_za)(s + w_zb) / ((s + w_p1)(s + w_p2)),  F = s (s + w_zf) / ((s + w_p1)(s + w_p2))
     Free: c, f_p1, f_p2, f_zf, f_za, f_zb, dry, wet, and one break frequency per frame (nuisance).
     Levenberg-Marquardt on the dB error at the 56 static_hz points of every frame, from three
     generic starts; the best is kept.
  2. Sweep range f_lo, f_hi from the complex snapshots (color_range_from_snapshots: the break
     frequency of every snapshot of ss_on_r040 and ss_on_r050 with the structure fixed, then the
     exponential law at kappa = SS_KAPPA). The previous step, kept behind --range-from-tracks:
     sweep range against the trajectory rows ss_on_r000 (quasi-static), r040, r050,
     r075. f_lo is the break frequency fitted to the frozen frame at the sweep bottom (stock
     window). The minima cannot set it: at 0.01 Hz both notches fade below the 3 dB prominence
     before the sweep bottom (notch 2 into a flat valley about 5 dB deep), and at 1.78 Hz notch 1
     fades where the fast sweep leaves it, so those readings move little with f_lo. f_hi starts at
     the break frequency of the frozen top frame and is moved (secant, three steps) until the
     notch-1 and notch-2 maximum errors over all four rows are centred on zero (minimax): at
     48 kHz the two notches cannot both match, see voicings.md. The prototype is rendered over the
     same probes and tracked like Guitar Rig 6; r000 in closed form frame by frame (a frozen LFO,
     which is what 0.01 Hz is). Start: the break frequencies of the stock-window frames at the
     sweep bottom and top. Needs W with the probes (measure.py probes, $W/pprobe.wav).
Prints the constants in the form used by prototype/phaser_ref.py.
"""
import argparse, importlib.util, math, os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
PROTO = os.path.join(HERE, "..", "..", "prototype", "phaser_ref.py")
sys.path.insert(0, HERE)


def load_proto():
    spec = importlib.util.spec_from_file_location("phaser_ref_fit", PROTO)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m


def read_targets(path):
    rows = {}
    for line in open(path):
        if line.startswith("#") or line.startswith("tag\t") or line.startswith("static"): continue
        c = line.rstrip("\n").split("\t")
        rows[c[0]] = dict(n1_min=float(c[1]), n1_max=float(c[2]), period=float(c[5]), depth=float(c[7]), peak=float(c[8]))
    return rows


FGRID = np.exp(np.linspace(math.log(0.02), math.log(50), 100001))


def block_response(beta, f=FGRID):
    s = 1j * f; A = (s - 1) / (s + 1); S2 = ((1 + beta) * s - 1) / (s + 1)
    return 0.5 + 0.5 * A * S2 * A * A / (1 + beta * A * A)


def block_peak_db(beta):
    return 20 * math.log10(np.abs(block_response(beta)).max())


def notch1_ratio(beta):
    h = np.abs(block_response(beta)); k = int(np.argmin(np.where(FGRID < 1.0, h, 9)))
    return FGRID[k]


def bisect(fn, lo, hi, target, it=60):
    for _ in range(it):
        mid = 0.5 * (lo + hi)
        if (fn(mid) - target) * (fn(lo) - target) <= 0: hi = mid
        else: lo = mid
    return 0.5 * (lo + hi)


def nelder_mead(fun, x0, step, iters=400, tol=1e-10):
    n = len(x0); pts = [np.array(x0, float)]
    for i in range(n):
        p = np.array(x0, float); p[i] += step[i]; pts.append(p)
    vals = [fun(p) for p in pts]
    for _ in range(iters):
        o = np.argsort(vals); pts = [pts[i] for i in o]; vals = [vals[i] for i in o]
        if abs(vals[-1] - vals[0]) < tol: break
        c = np.mean(pts[:-1], axis=0); xr = c + (c - pts[-1]); fr = fun(xr)
        if fr < vals[0]:
            xe = c + 2 * (c - pts[-1]); fe = fun(xe)
            pts[-1], vals[-1] = (xe, fe) if fe < fr else (xr, fr)
        elif fr < vals[-2]: pts[-1], vals[-1] = xr, fr
        else:
            xc = c + 0.5 * (pts[-1] - c); fc = fun(xc)
            if fc < vals[-1]: pts[-1], vals[-1] = xc, fc
            else:
                pts = [pts[0]] + [pts[0] + 0.5 * (p - pts[0]) for p in pts[1:]]; vals = [vals[0]] + [fun(p) for p in pts[1:]]
    return pts[int(np.argmin(vals))], min(vals)


def p90_notch_depth(beta, f_lag, fc):
    """Notch-1 level (dB) of the analog block loop at break frequency fc, lag in the feedback path:
    wet = A S2 A^2 / (1 + beta L A^2), out = (x + wet) / 2."""
    f = fc * np.exp(np.linspace(math.log(0.3), math.log(0.65), 1500))
    sp = 1j * f; g = fc; gl = f_lag
    A = (sp - g) / (sp + g); S2 = ((1 + beta) * sp - g) / (sp + g); L = gl / (gl + sp)
    return float(20 * np.log10(np.abs(0.5 + 0.5 * A * S2 * A * A / (1 + beta * L * A * A)).min()))


def p90_notch_table(beta, f_lag):
    """Notch-1 frequency of the frozen analog block response against the break frequency fc."""
    fcg = np.exp(np.linspace(math.log(400.0), math.log(6000.0), 300)); out = []
    gl = f_lag
    for fc in fcg:
        f = fc * np.exp(np.linspace(math.log(0.35), math.log(0.6), 4001))
        sp = 1j * f; g = fc
        A = (sp - g) / (sp + g); S2 = ((1 + beta) * sp - g) / (sp + g); L = gl / (gl + sp)
        h = np.log(np.abs(0.5 + 0.5 * A * S2 * A * A / (1 + beta * L * A * A))); k = int(np.argmin(h))
        d = 0.5 * (h[k - 1] - h[k + 1]) / (h[k - 1] - 2 * h[k] + h[k + 1])
        out.append(f[k] * math.exp(d * math.log(f[1] / f[0])))
    return np.log(fcg), np.log(np.array(out))


P90_NAMES = ("P90_RS", "P90_R1", "P90_GAMMA", "P90_KNEE", "P90_KAPPA", "P90_GATE_HZ")


def p90_model(P, q, rate, table, fs_ctrl=4000.0):
    """Model notch-1 trajectory (folded like the measurement) and sweep ends for parameters q."""
    import trajectory as TJ
    for k, v in zip(P90_NAMES, q): setattr(P, k, float(v))
    per = 1.0 / rate; settle = max(1.0, 6.0 / (2 * math.pi * q[5])); n = int((settle + per) * fs_ctrl) + 2
    fc = P.phase90_break_hz(n, fs_ctrl, rate, 0.5)
    n1 = np.exp(np.interp(np.log(fc), *table))
    _, f1, _ = TJ.fold(np.arange(n) / fs_ctrl, n1, per, skip=settle)
    tail = n1[int(settle * fs_ctrl):]
    return f1, tail.min(), tail.max()


def p90_shape_cost(f1, ref):
    ph = (np.arange(len(ref)) + 0.5) / len(ref); best = None
    ph3 = np.concatenate([ph - 1, ph, ph + 1]); y3 = np.log(np.concatenate([f1, f1, f1]))
    for s in np.linspace(-0.03, 0.03, 31):
        d = np.interp((ph + s) % 1.0, ph3, y3) - np.log(ref); d = d[np.isfinite(d)]
        c = float(np.mean(d ** 2) + 30 * np.mean(d ** 4))
        if best is None or c < best: best = c
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--targets", default=os.path.join(HERE, "targets.tsv"))
    ap.add_argument("--traj", default=os.path.join(HERE, "trajectories.tsv"))
    ap.add_argument("--snapshots", default=os.path.join(HERE, "snapshots.tsv"))
    ap.add_argument("--skip-color", action="store_true", help="skip the Color-on fit")
    ap.add_argument("--skip-range", action="store_true", help="Color on: structure only")
    ap.add_argument("--range-from-tracks", action="store_true",
                    help="Color on: the previous range step (multitone tracks of the prototype, needs W)")
    a = ap.parse_args()
    T = read_targets(a.targets); P = load_proto()
    blk = [v for k, v in sorted(T.items()) if k.startswith("p9_block")]
    peak_t = float(np.median([v["peak"] for v in blk]))
    beta = bisect(block_peak_db, 0.1, 0.8, peak_t); ratio = notch1_ratio(beta)
    print(f"# Phase 90 block: target resonance {peak_t:.2f} dB -> beta {beta:.4f} (with a 10 kOhm op-amp feedback resistor, a fitted feedback-resistor equivalent of {10e3/beta/1e3:.2f} kOhm), analog notch-1/fc {ratio:.4f}")
    import trajectory as TJ
    TR = TJ.read_tsv(a.traj)
    rows = [(k, v, TR[k]) for k, v in sorted(T.items()) if k.startswith("p9_block") and k in TR]
    table = p90_notch_table(beta, P.P90_FB_LAG_HZ)
    print(f"# Phase 90 sweep: {len(rows)} trajectories ({', '.join(k for k, _, _ in rows)}), notch table with lag {P.P90_FB_LAG_HZ:.0f} Hz")

    def cost(q):
        if not (50 < q[0] < 5000 and 100 < q[1] < 20000 and 0.05 < q[2] < 5 and 0.3 < q[3] < 8 and abs(q[4]) < 3 and 0.5 < q[5] < 30):
            return 1e9
        e = 0.0
        with np.errstate(all="ignore"):
            for k, v, tr in rows:
                f1, lo, hi = p90_model(P, q, 1 / tr["period"], table)
                e += p90_shape_cost(f1, tr["f1"]) + 10 * (math.log(lo / v["n1_min"]) ** 4 + math.log(hi / v["n1_max"]) ** 4) / 1e-4
        return e if np.isfinite(e) else 1e9
    best, c = None, 1e9
    for m0 in (1.0, 3.0):                    # two starts: the series limit soft (m = 1) or sharp (m = 3)
        x0 = [600.0, 1500.0, 1.0, m0, 0.0, 4.0]
        q, cq = nelder_mead(cost, x0, [0.1 * abs(v) + 0.05 for v in x0], iters=600)
        q, cq = nelder_mead(cost, q, [0.03 * abs(v) + 0.01 for v in q], iters=600)
        print(f"#   start m={m0:.0f}: cost {cq:.6f}  " + "  ".join(f"{k}={v:.5g}" for k, v in zip(P90_NAMES, q)), flush=True)
        if cq < c: best, c = q, cq
    print("# Phase 90 sweep, best start: trajectory rms / largest error and sweep ends per rate")
    for k, v, tr in rows:
        f1, lo, hi = p90_model(P, best, 1 / tr["period"], table)
        r = TJ.traj_error(tr["f1"], f1)
        print(f"#   {k}: trajectory {r[0]:.2f} % rms, {r[1]:+.2f} % largest; ends {lo:.1f}-{hi:.1f} Hz, target "
              f"{v['n1_min']:.1f}-{v['n1_max']:.1f} Hz ({100*(lo/v['n1_min']-1):+.2f} %, {100*(hi/v['n1_max']-1):+.2f} %)")
    for k, v in zip(P90_NAMES, best): setattr(P, k, float(v))
    print(f"P90_RS = {best[0]:.1f}\nP90_R1 = {best[1]:.1f}\nP90_GAMMA = {best[2]:.4f}\nP90_KNEE = {best[3]:.3f}\n"
          f"P90_KAPPA = {best[4]:.3f}\nP90_GATE_HZ = {best[5]:.3f}\nP90_BETA = {beta:.4f}")

    # 3. feedback lag (notch depth): one-pole lowpass in the feedback-resistor path, fitted so that the median
    #    notch-1 depth over each measured sweep matches depth_db (mean over the p9_block rows).
    #    beta and the sweep law stay as fitted above (the lag moves notch 1 by < 0.2 %).
    def depth_median(f_lag, v):
        per = v["period"]; fs_c = 4000.0; settle = max(1.0, 6.0 / (2 * math.pi * P.P90_GATE_HZ))
        fc = P.phase90_break_hz(int((settle + per) * fs_c), fs_c, 1 / v["period"], 0.5)[int(settle * fs_c):]
        grid = np.exp(np.linspace(math.log(fc.min() * 0.98), math.log(fc.max() * 1.02), 60))
        dep = np.array([p90_notch_depth(beta, f_lag, f) for f in grid])
        return float(np.median(np.interp(np.log(fc), np.log(grid), dep)))
    target = float(np.mean([v["depth"] for v in blk]))
    lo_f, hi_f = 2000.0, 60000.0
    for _ in range(30):                  # deeper notch for a higher lag frequency
        mid = math.sqrt(lo_f * hi_f)
        m = float(np.mean([depth_median(mid, v) for v in blk]))
        lo_f, hi_f = (mid, hi_f) if m > target else (lo_f, mid)
    f_lag = math.sqrt(lo_f * hi_f)
    print(f"# Phase 90 block: measured notch-1 depth {target:.2f} dB (mean of medians) -> feedback lag {f_lag:.0f} Hz "
          f"(a {1e6/(2*math.pi*f_lag):.1f} us delay at low frequencies)")
    for k, v in sorted(T.items()):
        if k.startswith("p9_block"): print(f"#   {k}: model median depth {depth_median(f_lag, v):.2f} dB, measured {v['depth']:.2f} dB")
    print(f"P90_FB_LAG_HZ = {f_lag:.0f}")

    ss_off_complex_fit(P, a.snapshots, TJ.read_tsv(a.traj))
    weight_check(P, a.snapshots)

    if a.skip_color: return
    color_fit(a, T, P)


SR = 48000


def complex_levmar(model, H, q0):
    """Levenberg-Marquardt on the complex error model(q) - H (both arrays of snapshot spectra)."""
    def resid(q):
        r = (model(q) - H).ravel(); return np.concatenate([r.real, r.imag])
    q, _ = levmar(resid, np.array(q0, float))
    e = model(q) - H
    return q, 10 * math.log10(np.sum(np.abs(e) ** 2) / np.sum(np.abs(H) ** 2))


def stage4(s, fc): return ((fc - s) / (fc + s)) ** 4


def ss_off_complex_fit(P, path, TR):
    import snapshots, trajectory as TJ
    freqs, takes = snapshots.read(path); s = 1j * freqs
    delay = lambda D: np.exp(-2j * math.pi * freqs * D / SR)  # noqa: E731
    clamp = lambda lf: math.exp(min(max(lf, 3.0), 10.5))  # noqa: E731
    H = np.concatenate([takes[t][2][::3] for t in ("ss_off_r040", "ss_off_r050")])

    def fc_grid(Hk, resp):
        grid = np.exp(np.linspace(math.log(80.0), math.log(15000.0), 400))
        return math.log(grid[int(np.argmin([np.sum((np.log(np.abs(Hk)) - np.log(np.abs(resp(g)) + 1e-12)) ** 2) for g in grid]))])

    def variant(hp, d0, w0):
        k = 3 if hp else 2
        def model(q):
            g = s / (s + math.exp(q[2])) if hp else 1.0
            return np.array([(q[0] + q[1] * g * stage4(s, clamp(lf))) * delay(q[k]) for lf in q[k + 1:]])
        lfc = [fc_grid(Hk, lambda g: d0 + w0 * stage4(s, g)) for Hk in H]
        return complex_levmar(model, H, [d0, w0] + ([math.log(40.0)] if hp else []) + [16.0] + lfc)
    print(f"# Small Stone Color off, complex snapshots ({len(H)} of ss_off_r040/r050, {len(freqs)} frequencies "
          f"{freqs[0]:.0f}-{freqs[-1]:.0f} Hz); model (dry + wet [s/(s+w_hp)] P^4) e^(-jwD):")
    best = None
    for hp in (False, True):
        for d0, w0 in ((0.75, 0.50), (0.50, 0.75)):
            q, rel = variant(hp, d0, w0)
            txt = f"dry {q[0]:.4f}, wet {q[1]:.4f}" + (f", f_hp {math.exp(q[2]):.2f} Hz" if hp else ", no highpass") + f", D {q[3 if hp else 2]:.3f} samples"
            print(f"#   start dry {d0} wet {w0}: {txt}, residual {rel:.1f} dB re |H|")
            if hp and (best is None or rel < best[1]): best = (q, rel)
    q = best[0]; dry, wet, f_hp, D = q[0], q[1], math.exp(q[2]), q[3]
    hp = s / (s + f_hp)

    def fc_of(Hk):
        cost = lambda lf: float(np.sum(np.abs((dry + wet * hp * stage4(s, math.exp(lf))) * delay(D) - Hk) ** 2))  # noqa: E731
        grid = np.linspace(math.log(150.0), math.log(15000.0), 800); k = int(np.argmin([cost(g) for g in grid]))
        lo, hi = grid[max(k - 1, 0)], grid[min(k + 1, len(grid) - 1)]
        for _ in range(50):
            a, b = lo + 0.382 * (hi - lo), lo + 0.618 * (hi - lo)
            lo, hi = (lo, b) if cost(a) < cost(b) else (a, hi)
        return 0.5 * (lo + hi)
    tags = ("ss_off_r040", "ss_off_r050", "ss_off_r075")
    data = [(takes[t][0], takes[t][1] + D / SR, np.array([fc_of(Hk) for Hk in takes[t][2]])) for t in tags]

    def law(q, rate, t, kappa):
        v = P.rc_triangle((q[3] + rate * t) % 1.0, kappa)
        return 0.5 * (math.log(q[0]) + math.log(q[1])) + 0.5 * math.log(q[1] / q[0]) * v + q[2] * (v * v - 1.0)

    def sweep_fit(kappa_free, curve):
        def cost(x):
            kap = x[3] if kappa_free else P.SS_KAPPA
            if not (50 < x[0] < 2000 and 1000 < x[1] < 20000 and 0.01 < kap < 3): return 1e9
            c = x[2] if curve else 0.0
            return sum(float(np.sum((law([x[0], x[1], c, x[4 + j]], r, t, kap) - lf) ** 2)) for j, (r, t, lf) in enumerate(data))
        x0 = [P.SS_OFF["f_lo"], P.SS_OFF["f_hi"], 0.0, P.SS_KAPPA, 0.0, 0.0, 0.0]
        x, _ = nelder_mead(cost, x0, [20.0, 400.0, 0.005, 0.05, 0.005, 0.005, 0.005], iters=3000)
        x, _ = nelder_mead(cost, x, [2.0, 40.0, 0.001, 0.005, 0.001, 0.001, 0.001], iters=3000)
        kap = x[3] if kappa_free else P.SS_KAPPA
        errs = [law([x[0], x[1], x[2] if curve else 0.0, x[4 + j]], r, t, kap) - lf for j, (r, t, lf) in enumerate(data)]
        e = np.concatenate(errs)
        print(f"#   kappa {'free' if kappa_free else 'fixed'}, curve {'free' if curve else '0'}: f_lo {x[0]:.1f} Hz, f_hi {x[1]:.1f} Hz, "
              f"curve {x[2] if curve else 0.0:.4f}, kappa {kap:.3f}, phase0 " + "/".join(f"{(x[4 + j] + 0.5) % 1.0 - 0.5:+.4f}" for j in range(3))
              + f"; ln fc error {100 * np.sqrt(np.mean(e ** 2)):.3f} % rms, {100 * np.max(np.abs(e)):.2f} % largest")
        return x
    print(f"# Structure: dry {dry:.4f}, wet {wet:.4f}, f_hp {f_hp:.2f} Hz, D {D:.3f} samples. Break frequency per snapshot, "
          f"all {sum(len(d[2]) for d in data)} snapshots of " + ", ".join(tags) + ", then the sweep law:")
    sweep_fit(False, False)
    sweep_fit(True, True)
    x = sweep_fit(False, True)
    P.SS_OFF.update(f_lo=float(x[0]), f_hi=float(x[1]), curve=float(x[2]), dry=float(dry), wet=float(wet), f_hp=float(f_hp))

    # 4. notch-1 trajectories of trajectories.tsv against the analog response along the fitted law
    def n1_of(fc):
        f = fc * np.exp(np.linspace(math.log(0.25), math.log(0.8), 3000)); sv = 1j * f
        h = np.abs(dry + wet * sv / (sv + f_hp) * stage4(sv, fc)); k = int(np.argmin(h))
        return f[k]
    fcg = np.exp(np.linspace(math.log(P.SS_OFF["f_lo"] * 0.98), math.log(P.SS_OFF["f_hi"] * 1.02), 200))
    n1g = np.array([n1_of(f) for f in fcg])
    for k, v in sorted(TR.items()):
        if not k.startswith("ss_off"): continue
        per = v["period"]; n = 4000; t = np.arange(n) / n * per
        fc = P.smallstone_break_hz(n, n / per, 1 / per, 0.5, False)
        n1 = np.exp(np.interp(np.log(fc), np.log(fcg), np.log(n1g)))
        r = TJ.traj_error(v["f1"], TJ.fold(t, n1, per, skip=0.0)[1])
        print(f"#   {k}: notch-1 trajectory {r[0]:.2f} % rms ({r[1]:+.2f} % largest) against trajectories.tsv")
    print(f"SS_OFF = dict(f_lo={x[0]:.1f}, f_hi={x[1]:.1f}, curve={x[2]:.4f}, c=0.0, dry={dry:.4f}, wet={wet:.4f}, f_hp={f_hp:.1f})")


def weight_check(P, path):
    """Dry and wet of Phase 90 block and Small Stone Color on from their complex snapshots, the rest of
    each structure fixed at the prototype's constants."""
    import snapshots
    freqs, takes = snapshots.read(path); s = 1j * freqs
    p = P.SS_ON
    wets = {"p9_block_p010": ("Phase 90 block", 0.5 * 1.862, 0.5 * 1.862,
                              lambda fc: (P.h_block(s, (fc,) * 4, P.P90_FB_LAG_HZ) - 0.5) / 0.5),
            "ss_on_r040": ("Small Stone Color on", p["dry"], p["wet"],
                           lambda fc: (P.h_stone_on(s, (fc,) * 4, p["f_p1"], p["f_p2"]) - p["dry"]) / p["wet"])}
    for tag, (name, d_used, w_used, wet) in wets.items():
        H = takes[tag][2]
        best = None
        for d0, w0 in ((d_used, w_used), (w_used, d_used), (d_used, -w_used)):
            lfc = []
            for Hk in H:
                grid = np.exp(np.linspace(math.log(60.0), math.log(8000.0), 300))
                lfc.append(math.log(grid[int(np.argmin([np.sum((np.log(np.abs(Hk)) - np.log(np.abs(d0 + w0 * wet(g)) + 1e-12)) ** 2) for g in grid]))]))
            model = lambda q: np.array([(q[0] + q[1] * wet(math.exp(min(max(lf, 3.0), 10.5)))) * np.exp(-2j * math.pi * freqs * q[2] / SR) for lf in q[3:]])  # noqa: E731
            q, rel = complex_levmar(model, H, [d0, w0, 16.0] + lfc)
            if best is None or rel < best[1]: best = (q, rel, (d0, w0))
        q, rel, st = best
        print(f"# {name}, complex snapshots of {tag} ({len(H)}): dry {q[0]:.4f}, wet {q[1]:.4f} (dry/wet {q[0] / q[1]:.4f}; "
              f"prototype {d_used / w_used:.4f}), residual {rel:.1f} dB re |H|, best of three starts (from {st[0]:.3f}/{st[1]:.3f})")


# ------------------------------------------------------------------ Small Stone, Color on
def read_static(path):
    hz, frames = None, []
    for line in open(path):
        c = line.rstrip("\n").split("\t")
        if c[0] == "static_hz": hz = np.array([float(x) for x in c[4:]])
        elif c[0] == "static": frames.append((c[1], np.array([float(x) for x in c[4:]])))
    return hz, frames


def color_static_model(hz):
    sp = 1j * hz                                                            # analog
    def g(f): return f
    def lp(f): return g(f) / (g(f) + sp)
    def resp(par, fc):
        c, f1, f2, fzf, fza, fzb, dry, wet = par
        F = (1 - lp(f1)) * (1 - (1 - fzf / f2) * lp(f2))
        Ha = (1 - (1 - fza / f1) * lp(f1)) * (1 - (1 - fzb / f2) * lp(f2))
        P4 = ((g(fc) - sp) / (g(fc) + sp)) ** 4
        return dry + wet * Ha * P4 / (1 - c * F * P4)
    return resp


def levmar(fun, q, it=300):
    lam = 1e-2; r = fun(q); cost = r @ r
    for _ in range(it):
        J = np.empty((len(r), len(q)))
        for j in range(len(q)):
            dq = np.zeros(len(q)); h = 1e-6 * max(1.0, abs(q[j])); dq[j] = h; J[:, j] = (fun(q + dq) - r) / h
        A = J.T @ J; gv = J.T @ r
        while True:
            step = np.linalg.solve(A + lam * np.diag(np.diag(A) + 1e-12), -gv); q2 = q + step; r2 = fun(q2); c2 = r2 @ r2
            if np.isfinite(c2) and c2 < cost: q, r, cost = q2, r2, c2; lam = max(lam / 3, 1e-9); break
            lam *= 4
            if lam > 1e8: return q, cost
        if np.max(np.abs(step)) < 1e-10: break
    return q, cost


def color_structure(hz, frames):
    resp = color_static_model(hz)
    def unpack(q): return (q[0], *np.exp(q[1:6]), q[6], q[7]), np.exp(q[8:])
    def resid(q):
        par, fcs = unpack(q)
        return np.concatenate([20 * np.log10(np.abs(resp(par, fc))) - v for fc, (_, v) in zip(fcs, frames)])
    best = None
    for start in [(0.6, 80, 200, 60, 30, 600, 0.2, 0.6), (0.7, 40, 300, 20, 20, 1000, 0.15, 0.5), (0.8, 100, 400, 50, 50, 500, 0.1, 0.4)]:
        par0 = start
        fc0 = []                                    # per-frame start: best break frequency on a grid
        for _, v in frames:
            grid = np.exp(np.linspace(math.log(40), math.log(6000), 400))
            fc0.append(grid[int(np.argmin([np.mean((20 * np.log10(np.abs(resp(par0, f))) - v) ** 2) for f in grid]))])
        q0 = np.array([par0[0], *np.log(par0[1:6]), par0[6], par0[7], *np.log(fc0)])
        q, cost = levmar(resid, q0)
        if best is None or cost < best[1]: best = (q, cost)
    par, fcs = unpack(best[0]); r = resid(best[0]).reshape(len(frames), -1)
    return par, dict(zip([n for n, _ in frames], fcs)), {n: (float(np.sqrt(np.mean(e ** 2))), float(np.abs(e).max())) for (n, _), e in zip(frames, r)}


def color_track(P, rows, f_lo, f_hi, quasi):
    """Prototype notch extremes for each trajectory row, tracked like the target."""
    import track, measure, static
    P.SS_ON["f_lo"], P.SS_ON["f_hi"] = f_lo, f_hi
    take = {t[0]: t for t in measure.TAKES}; out = {}
    for tag, v in rows.items():
        if tag == quasi[0]:      # 0.01 Hz: closed-form response at each frame centre (frozen LFO)
            pr = static.bins() * static.FS / static.N; t = np.arange(1.0, 102.0, 2048 / static.FS)
            fc = P.smallstone_break_hz(len(t), 1.0 / (t[1] - t[0]), 1.0 / v["period"], 0.5, True, phase0=t[0] / v["period"])
            resp = color_static_model(pr); par = tuple(P.SS_ON[k] for k in ("c", "f_p1", "f_p2", "f_zf", "f_za", "f_zb", "dry", "wet"))
            H = np.array([np.abs(resp(par, f)) for f in fc]); r = static.summary(pr, t, H)
        else:
            secs = take[tag][3]; ind = f"{measure.W}/probe{secs}"
            xs = np.stack([track.read_wav(f"{ind}/tone_{j:02d}.wav")[0][0] for j in range(track.read_grid(ind)[3])], axis=1)
            y = P.render(xs, track.FS, rate=1 / v["period"], voicing="smallstone", color=True, makeup=False)
            import tempfile, shutil
            d = tempfile.mkdtemp(dir=measure.W)
            for j in range(y.shape[1]): track.write_wav(f"{d}/tone_{j:02d}.wav", y[:, j])
            f, step, tt, H, _ = track.H_from_dirs(ind, d, 8, 16, 0, 0.0); shutil.rmtree(d)
            r = track.summarise(tt, f, step, H, 60, 12000, 3.0, 1.0)
        out[tag] = (r["n1"]["min"], r["n1"]["max"], r["n2"]["min"], r["n2"]["max"])
    return out


def color_range_from_snapshots(P, path):
    """Color on sweep range against the complex snapshots of ss_on_r040 and ss_on_r050: the structure
    fixed at the static-frame fit, the break frequency of every snapshot by golden section on the
    complex error (delay fitted once on the first take), then f_lo, f_hi and one phase0 per take with
    kappa = SS_KAPPA (Nelder-Mead on ln fc). The curvature term of Color off is fitted too and printed:
    it is not resolved here, so Color on keeps the plain exponential law."""
    import snapshots
    freqs, takes = snapshots.read(path); s = 1j * freqs
    p = P.SS_ON
    resp = lambda fc: P.h_stone_on(s, (fc,) * 4, p["f_p1"], p["f_p2"])  # noqa: E731
    H0 = takes["ss_on_r040"][2]
    lfc = []
    for Hk in H0[::4]:
        grid = np.exp(np.linspace(math.log(60.0), math.log(5000.0), 300))
        lfc.append(math.log(grid[int(np.argmin([np.sum((np.log(np.abs(Hk)) - np.log(np.abs(resp(g)))) ** 2) for g in grid]))]))
    model = lambda q: np.array([resp(math.exp(min(max(lf, 3.0), 10.5))) * np.exp(-2j * math.pi * freqs * q[0] / SR) for lf in q[1:]])  # noqa: E731
    q, rel = complex_levmar(model, H0[::4], [16.0] + lfc)
    D = q[0]

    def fc_of(Hk):
        cost = lambda lf: float(np.sum(np.abs(resp(math.exp(lf)) * np.exp(-2j * math.pi * freqs * D / SR) - Hk) ** 2))  # noqa: E731
        grid = np.linspace(math.log(60.0), math.log(5000.0), 600); k = int(np.argmin([cost(g) for g in grid]))
        lo, hi = grid[max(k - 1, 0)], grid[min(k + 1, len(grid) - 1)]
        for _ in range(50):
            x1, x2 = lo + 0.382 * (hi - lo), lo + 0.618 * (hi - lo)
            lo, hi = (lo, x2) if cost(x1) < cost(x2) else (x1, hi)
        return 0.5 * (lo + hi)
    tags = ("ss_on_r040", "ss_on_r050")
    data = [(takes[t][0], takes[t][1] + D / SR, np.array([fc_of(Hk) for Hk in takes[t][2]])) for t in tags]

    def law(x, rate, t, curve):
        v = P.rc_triangle((x[3] + rate * t) % 1.0, P.SS_KAPPA)
        return 0.5 * (math.log(x[0]) + math.log(x[1])) + 0.5 * math.log(x[1] / x[0]) * v + curve * (v * v - 1.0)

    def fit(curve_free):
        def cost(x):
            if not (20 < x[0] < 1000 and 500 < x[1] < 20000): return 1e9
            return sum(float(np.sum((law([x[0], x[1], 0, x[3 + j]], r, t, x[2] if curve_free else 0.0) - lf) ** 2))
                       for j, (r, t, lf) in enumerate(data))
        x, _ = nelder_mead(cost, [p["f_lo"], p["f_hi"], 0.0, 0.0, 0.0], [5.0, 100.0, 0.005, 0.005, 0.005], iters=3000)
        x, _ = nelder_mead(cost, x, [0.5, 10.0, 0.001, 0.001, 0.001], iters=3000)
        e = np.concatenate([law([x[0], x[1], 0, x[3 + j]], r, t, x[2] if curve_free else 0.0) - lf for j, (r, t, lf) in enumerate(data)])
        print(f"#   curve {'free' if curve_free else '0'}: f_lo {x[0]:.1f} Hz, f_hi {x[1]:.1f} Hz, curve {x[2] if curve_free else 0.0:.4f}, "
              f"phase0 {(x[3] + 0.5) % 1 - 0.5:+.4f}/{(x[4] + 0.5) % 1 - 0.5:+.4f}; ln fc error {100 * np.sqrt(np.mean(e ** 2)):.3f} % rms, "
              f"{100 * np.max(np.abs(e)):.2f} % largest")
        return x
    old = [law([p["f_lo"], p["f_hi"], 0, 0.0], r, t, 0.0) - lf for r, t, lf in data]
    print(f"# Small Stone Color on, sweep range from the complex snapshots of {', '.join(tags)} (delay {D:.3f} samples, "
          f"residual {rel:.1f} dB re |H| on every fourth snapshot of the first); the previous range ({p['f_lo']:.1f}-{p['f_hi']:.1f} Hz, "
          f"phase0 0) reads {100 * np.sqrt(np.mean(np.concatenate(old) ** 2)):.3f} % rms:")
    fit(True)
    x = fit(False)
    return float(x[0]), float(x[1])


def color_fit(a, T, P):
    hz, frames = read_static(a.targets)
    par, fcs, err = color_structure(hz, frames)
    names = ("c", "f_p1", "f_p2", "f_zf", "f_za", "f_zb", "dry", "wet")
    print("# Small Stone Color on, structure from %d static frames (rms / max dB error per frame):" % len(frames))
    for n, (e, m) in err.items(): print(f"#   {n}: break frequency {fcs[n]:.1f} Hz, {e:.2f} / {m:.2f} dB")
    for k, v in zip(names, par): P.SS_ON[k] = float(v)
    if a.skip_range:
        print("SS_ON " + " ".join(f"{k}={v:.4g}" for k, v in zip(names, par))); return
    if not a.range_from_tracks:
        lo, hi = color_range_from_snapshots(P, a.snapshots)
        print("SS_ON = dict(f_lo=%.1f, f_hi=%.1f, c=%.4f, dry=%.4f, wet=%.4f,\n             f_p1=%.1f, f_p2=%.1f, f_zf=%.1f, f_za=%.1f, f_zb=%.1f)"
              % (lo, hi, par[0], par[6], par[7], par[1], par[2], par[3], par[4], par[5]))
        return
    rows = {}
    for line in open(a.targets):
        c = line.rstrip("\n").split("\t")
        if c[0].startswith("ss_on_r"): rows[c[0]] = dict(n1_min=float(c[1]), n1_max=float(c[2]), n2_min=float(c[3]), n2_max=float(c[4]), period=float(c[5]))
    import measure
    lo, hi = fcs["ss_on_full_bot"], fcs["ss_on_full_top"]
    def errs(o):
        return {t: [math.log(o[t][i] / rows[t][k]) for i, k in enumerate(("n1_min", "n1_max", "n2_min", "n2_max"))] for t in rows}
    def balance(h):                     # centre of the notch-1/notch-2 maximum errors over all rows
        e = errs(color_track(P, rows, lo, h, measure.QUASI)); mx = [x for t in e for x in (e[t][1], e[t][3])]
        print(f"#   f_hi {h:.1f}: maximum errors {100*min(mx):+.2f} % to {100*max(mx):+.2f} %", flush=True)
        return 0.5 * (max(mx) + min(mx))
    x0 = math.log(hi); b0 = balance(hi); x1 = x0 - b0; b1 = balance(math.exp(x1))
    for _ in range(2):                  # secant on the balance
        if abs(b1 - b0) < 1e-6: break
        x0, x1, b0 = x1, x1 - b1 * (x1 - x0) / (b1 - b0), b1; b1 = balance(math.exp(x1))
    hi = math.exp(x1)
    o = color_track(P, rows, lo, hi, measure.QUASI); e = errs(o)
    for t in sorted(rows):
        print(f"#   {t}: n1 {o[t][0]:.1f}-{o[t][1]:.1f} ({100*e[t][0]:+.2f} %, {100*e[t][1]:+.2f} %), n2 {o[t][2]:.0f}-{o[t][3]:.0f} ({100*e[t][2]:+.2f} %, {100*e[t][3]:+.2f} %)")
    print("SS_ON = dict(f_lo=%.1f, f_hi=%.1f, c=%.4f, dry=%.4f, wet=%.4f,\n             f_p1=%.1f, f_p2=%.1f, f_zf=%.1f, f_za=%.1f, f_zb=%.1f)"
          % (lo, hi, par[0], par[6], par[7], par[1], par[2], par[3], par[4], par[5]))


if __name__ == "__main__":
    main()
