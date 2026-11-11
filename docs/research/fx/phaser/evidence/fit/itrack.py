#!/usr/bin/env python3
"""itrack.py: impulse-snapshot notch tracker (numpy only).

Copied unchanged in method from the independent verifier's tool
($RE_STATE/work/phaser-refit-verify/itrack.py, described in ../../verification-refit.md section 1);
only this header is new. Round 3 uses it for the whole-trajectory measure (trajectory.py, compare.py).

Principle (different from evidence/fit/track.py, which demodulates steady multitones with
8-cycle windows): the probe is a train of single-sample impulses, one every P samples. The
device's answer to each impulse is a complete impulse response taken at one instant of the
LFO. Its DTFT on a log frequency grid is |H(f)| at that instant; no analysis window sets the
frequency resolution, and the time resolution is the length of the response itself (a few
ms). M probe files with the train shifted by P/M samples are interleaved, so snapshots are
P/M samples apart.

Per snapshot: notches are local minima of |H| in dB inside [lo, hi] with at least `prom` dB
of prominence on both sides, refined by a golden-section search on the DTFT itself. Notch 1
and notch 2 are followed by continuity. Sweep extremes are not single frames: around each
turning point a two-sided quadratic with a common apex is fitted (it fits a smooth parabola
and a cusp alike) and the extreme of the fitted curve is reported; the median over cycles is the result. Where a
track ends before a turning point (the notch loses prominence) the reported extreme is the
last resolved value and is flagged `faded`.
"""
import math
import numpy as np

FS = 48000


def probe(nsamp, P, M, amp=0.25, start=4800):
    """M impulse trains, shape (M, nsamp); train j has impulses at start + j*P/M + k*P."""
    x = np.zeros((M, nsamp)); pos = []
    for j in range(M):
        idx = np.arange(start + (j * P) // M, nsamp - P, P)
        x[j, idx] = amp; pos.append(idx)
    return x, pos


def fgrid(f0=40.0, f1=16000.0, ratio=1.0035):
    n = int(math.log(f1 / f0) / math.log(ratio)) + 1
    return f0 * ratio ** np.arange(n)


def snapshots(y, pos, P, amp, latency=0, pre=8, f=None, fs=FS, taper=0.125):
    """y: list of M output arrays; pos: impulse positions per file. Returns t (s), f, H (frames x F) linear,
    and tail: energy of the last eighth of each segment over the whole segment (truncation check)."""
    f = fgrid() if f is None else f
    L = P
    w = np.ones(L); nt = int(taper * L)
    w[L - nt:] = 0.5 * (1 + np.cos(np.pi * (np.arange(nt) + 0.5) / nt))
    segs, ts = [], []
    for yj, pj in zip(y, pos):
        for n0 in pj:
            a = n0 + latency - pre
            if a < 0 or a + L > len(yj): continue
            segs.append(yj[a:a + L]); ts.append(n0 / fs)
    segs = np.array(segs); ts = np.array(ts)
    o = np.argsort(ts); segs = segs[o]; ts = ts[o]
    tail = np.sum(segs[:, L - L // 8:] ** 2, axis=1) / np.maximum(np.sum(segs ** 2, axis=1), 1e-300)
    E = np.exp(-2j * np.pi * np.outer(np.arange(L), f) / fs)
    H = np.abs((segs * w) @ E) / amp
    return ts, f, H, tail, segs * w


def dtft_mag(seg, f, fs=FS):
    return abs(np.dot(seg, np.exp(-2j * np.pi * f * np.arange(len(seg)) / fs)))


def golden_min(fun, a, b, it=40):
    g = (math.sqrt(5) - 1) / 2
    c, d = b - g * (b - a), a + g * (b - a); fc, fd = fun(c), fun(d)
    for _ in range(it):
        if fc < fd: b, d, fd = d, c, fc; c = b - g * (b - a); fc = fun(c)
        else: a, c, fc = c, d, fd; d = a + g * (b - a); fd = fun(d)
    x = 0.5 * (a + b); return x, fun(x)


def frame_notches(f, db, lo, hi, prom):
    """Indices (into f) of local minima inside [lo, hi] with >= prom dB prominence on both sides."""
    k0, k1 = np.searchsorted(f, lo), np.searchsorted(f, hi)
    d = db[k0:k1]; out = []
    for i in range(1, len(d) - 1):
        if not (d[i] <= d[i - 1] and d[i] < d[i + 1]): continue
        j = i - 1; left = d[i]
        while j >= 0 and d[j] >= d[i]: left = max(left, d[j]); j -= 1
        j = i + 1; right = d[i]
        while j < len(d) and d[j] >= d[i]: right = max(right, d[j]); j += 1
        if left - d[i] >= prom and right - d[i] >= prom: out.append(k0 + i)
    return out


def track(ts, f, H, segs, amp, lo, hi, prom=3.0, fs=FS, gain_db=0.0):
    """Per frame: refined notch list [(hz, db)], peak level in band. Returns dict."""
    db = 20 * np.log10(np.maximum(H, 1e-12)) + gain_db
    k0, k1 = np.searchsorted(f, lo), np.searchsorted(f, hi)
    notches, peak = [], []
    for i in range(len(ts)):
        lst = []
        for k in frame_notches(f, db[i], lo, hi, prom):
            x, v = golden_min(lambda lf: dtft_mag(segs[i], math.exp(lf), fs), math.log(f[k - 1]), math.log(f[k + 1]))
            lst.append((math.exp(x), 20 * math.log10(max(v / amp, 1e-12)) + gain_db))
        notches.append(lst); peak.append(db[i, k0:k1].max())
    return dict(t=ts, notches=notches, peak=np.array(peak), db=db, f=f)


def assign(tr, max_jump=0.30, rlo=3.2, rhi=8.0):
    """Follow notch 1 and notch 2. Seeds: frames with exactly two notches whose ratio is in [rlo, rhi]
    (lower = notch 1). Other frames: each notch goes to the track whose most recent value (in the
    sweep direction, forward then backward pass) is nearest in ln f, if within max_jump."""
    n = len(tr["t"]); n1 = np.full(n, np.nan); n2 = np.full(n, np.nan); d1 = np.full(n, np.nan); d2 = np.full(n, np.nan)
    seeded = np.zeros(n, bool)
    for i, lst in enumerate(tr["notches"]):
        if len(lst) == 2 and rlo <= lst[1][0] / lst[0][0] <= rhi:
            n1[i], d1[i] = lst[0]; n2[i], d2[i] = lst[1]; seeded[i] = True
    idx = np.arange(n)
    for sweep in (idx, idx[::-1]):
        last1 = last2 = None
        for i in sweep:
            if not seeded[i]:
                for hz, dbv in tr["notches"][i]:
                    c1 = abs(math.log(hz / last1)) if last1 else 9.0
                    c2 = abs(math.log(hz / last2)) if last2 else 9.0
                    if min(c1, c2) > max_jump: continue
                    if c1 <= c2:
                        if np.isnan(n1[i]): n1[i], d1[i] = hz, dbv
                    elif np.isnan(n2[i]): n2[i], d2[i] = hz, dbv
            if not np.isnan(n1[i]): last1 = n1[i]
            if not np.isnan(n2[i]): last2 = n2[i]
    return n1, d1, n2, d2


def period_from_track(t, v, levels=np.linspace(0.25, 0.75, 11)):
    """Period from level crossings of ln f at several levels, both directions; least squares per
    level, median over levels. Returns (period, n_crossings_used)."""
    ok = ~np.isnan(v); t = t[ok]; y = np.log(v[ok])
    lo, hi = np.percentile(y, 2), np.percentile(y, 98); est = []
    for lv in levels:
        c = lo + lv * (hi - lo)
        for sgn in (1, -1):
            s = sgn * (y - c); k = np.where((s[:-1] < 0) & (s[1:] >= 0) & (np.diff(t) < 4 * np.median(np.diff(t))))[0]
            if len(k) < 2: continue
            tc = t[k] + (t[k + 1] - t[k]) * (-s[k]) / (s[k + 1] - s[k])
            # drop double triggers closer than 0.4 of the median spacing
            keep = [0]
            for j in range(1, len(tc)):
                if tc[j] - tc[keep[-1]] > 0.4 * np.median(np.diff(tc)): keep.append(j)
            tc = tc[keep]
            if len(tc) < 2: continue
            j = np.round((tc - tc[0]) / np.median(np.diff(tc)))
            est.append(np.polyfit(j, tc, 1)[0])
    return (float(np.median(est)), len(est)) if est else (float("nan"), 0)


def apex_fit(t, y, sign, t_guess, half):
    """Two-sided quadratic with a common apex, fitted to (t, y) within t_guess +- half.
    sign +1 for a maximum, -1 for a minimum. Returns (apex value, apex time, rms residual, n)."""
    m = (t > t_guess - half) & (t < t_guess + half) & ~np.isnan(y)
    tt, yy = t[m], y[m]
    if len(tt) < 8: return None
    best = None
    for ts in np.linspace(t_guess - 0.5 * half, t_guess + 0.5 * half, 201):
        d = tt - ts; L = d < 0
        A = np.column_stack([np.ones(len(d)), np.where(L, d, 0), np.where(L, d * d, 0), np.where(L, 0, d), np.where(L, 0, d * d)])
        if L.sum() < 3 or (~L).sum() < 3: continue
        co, *_ = np.linalg.lstsq(A, yy, rcond=None); r = yy - A @ co; e = float(np.sqrt(np.mean(r * r)))
        if best is None or e < best[2]: best = (co, float(ts), e, len(tt))
    if best is None: return None
    # the reported value is the extreme of the fitted curve inside the window (the junction for a cusp,
    # the vertex for a smooth turn), not the value at the junction
    co, ts, e, n = best; g = np.linspace(tt[0], tt[-1], 2001) - ts; L = g < 0
    c = co[0] + np.where(L, co[1] * g + co[2] * g * g, co[3] * g + co[4] * g * g)
    k = int(np.argmax(sign * c))
    return (float(c[k]), float(ts + g[k]), e, n)


def extremes(t, v, period, sign, skip=1.0, half_frames=12):
    """Sweep extreme of one notch track (Hz). sign +1 maximum, -1 minimum.
    value = median over cycles of the fitted apex; if the track is not resolved through the turning
    point (kind 'faded'), the median of the last resolved values on both sides of each gap.
    raw = median over cycles of the extreme of a 3-frame running median (no fit)."""
    ok = (t >= skip); t = t[ok]; y = np.log(v[ok]); dt = np.median(np.diff(t))
    good = ~np.isnan(y); half_frac = half_frames * dt / period
    if good.sum() < 20: return None
    ys = y.copy()
    for i in range(1, len(y) - 1):
        w = y[i - 1:i + 2]; ys[i] = np.nanmedian(w) if np.isfinite(w).sum() >= 2 else np.nan
    tg = t[good]; t0 = t[0]
    ph = ((t - t0) / period) % 1.0
    nb = 80; prof = np.full(nb, np.nan)
    for b in range(nb):
        m = (ph >= b / nb) & (ph < (b + 1) / nb) & ~np.isnan(ys)
        if m.sum() >= 1: prof[b] = np.median(ys[m])
    filled = ~np.isnan(prof)
    b_ext = int(np.nanargmax(prof)) if sign > 0 else int(np.nanargmin(prof))
    gap_bins = [b for b in range(nb) if not filled[b]]
    faded = bool(gap_bins) and min(min((b - b_ext) % nb, (b_ext - b) % nb) for b in gap_bins) <= 2
    vals, raws = [], []
    if faded:
        k = np.where(good)[0]; g = np.where(np.diff(t[k]) > 3 * dt)[0]
        ext = prof[b_ext]
        for gi in g:
            for kk in (k[gi], k[gi + 1]):
                if abs(y[kk] - ext) < 0.25: vals.append(y[kk])
    else:
        ncyc = int(math.ceil((t[-1] - t0) / period)) + 1
        for c in range(-1, ncyc + 1):
            tc = t0 + (c + (b_ext + 0.5) / nb) * period
            if tc < t[0] + half_frac * period or tc > t[-1] - half_frac * period: continue
            # the folded profile only locates the turning point to one bin: centre on the actual extreme
            w = (t > tc - 1.5 * period / nb) & (t < tc + 1.5 * period / nb) & ~np.isnan(ys)
            if w.sum() < 5: continue
            te = t[w][int(np.argmax(sign * ys[w]))]
            if te < t[0] + half_frac * period or te > t[-1] - half_frac * period: continue
            r = apex_fit(t, y, sign, te, half_frac * period)
            if r is None: continue
            vals.append(r[0]); raws.append(sign * np.max(sign * ys[w]))
    if not vals: return None
    vals = np.array(vals)
    return dict(value=float(np.exp(np.median(vals))), kind="faded" if faded else "apex", n=len(vals),
                spread=float(100 * (np.exp(vals.max() - vals.min()) - 1)) if len(vals) > 1 else float("nan"),
                raw=float(np.exp(np.median(raws))) if raws else float("nan"))


def summarise(tr, lo, hi, period_hint=None, skip=1.0):
    n1, d1, n2, d2 = assign(tr); t = tr["t"]; m = t >= skip
    per, ncross = period_from_track(t[m], n1[m])
    if not np.isfinite(per):
        per, ncross = period_from_track(t[m], n2[m])
    if not np.isfinite(per) and period_hint: per = period_hint
    out = dict(period=per, ncross=ncross, n1=n1, n2=n2, d1=d1, d2=d2,
               depth=float(np.nanmedian(d1[m])), peak=float(np.median(tr["peak"][m])), peak_max=float(np.max(tr["peak"][m])),
               resolved1=float(np.mean(~np.isnan(n1[m]))), resolved2=float(np.mean(~np.isnan(n2[m]))))
    P = per if np.isfinite(per) else period_hint
    for name, v in (("n1", n1), ("n2", n2)):
        for sgn, lab in ((-1, "min"), (1, "max")):
            out[f"{name}_{lab}"] = extremes(t, v, P, sgn, skip) if P else None
    return out


def fmt(label, r):
    def e(x):
        if x is None: return "-"
        return f"{x['value']:.1f}{'*' if x['kind'] == 'faded' else ''}"
    return (f"{label}\tn1 {e(r['n1_min'])}-{e(r['n1_max'])}\tn2 {e(r['n2_min'])}-{e(r['n2_max'])}\tperiod {r['period']:.5f}"
            f"\tdepth {r['depth']:.2f}\tpeak {r['peak']:.2f}\tres1 {r['resolved1']:.2f} res2 {r['resolved2']:.2f}")
