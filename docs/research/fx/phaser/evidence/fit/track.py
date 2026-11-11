#!/usr/bin/env python3
"""track.py: notch-trajectory tracker for swept phasers (numpy only).

Method: interleaved sparse multitones. The tone grid (log spaced, ratio --step) is dealt
round-robin into --m probe files so that tones in one file are step**m apart. Each file is
rendered through the device. Every tone is demodulated from the output with a centred
Blackman-Harris window of q cycles (clipped to 256..12288 samples), computed in the
frequency domain (one FFT per file, then a small inverse FFT per tone), giving |H(f, t)|
on a common time grid (hop --hop samples). Per frame, notches are local minima of |H| with
>= --prom dB prominence on both sides, refined by a parabola through |H|^2 against log f.
The sweep minimum and maximum are taken per LFO cycle (parabola in time around the
extreme frame) and the median over cycles is reported, so a single outlier frame cannot
set them. The period is the mean spacing of upward crossings of the log-mid level.

Run:
  track.py gen   <dir> --secs 6 [--f0 60 --f1 12000 --step 1.02 --m 36 --db -36]
      writes <dir>/tone_00.wav ... (48 kHz mono float) and <dir>/grid.txt
  track.py ana   <indir> <outdir> [--q 8 --hop 16 --skip 1.0 --lo 60 --hi 12000 --prom 3
                  --gain-db 0 --ch 0 --dump track.tsv --label name]
      reads <indir>/tone_XX.wav and <outdir>/tone_XX.wav, prints one summary line:
      label n1_min n1_max n2_min n2_max period depth_db peak_db peak_max_db cycles frames
  track.py check <indir> <outdir>      list rendered files with NaN/Inf or short length
  track.py selftest [--m 36 --q 8]
      synthetic 4-stage TPT phaser with a known notch trajectory at several rates;
      prints tracked against true min/max/period.
--gain-db is added to |H| (use -5.40 for Guitar Rig 6 Phaser Nine's x1.862 make-up).
"""
import argparse, math, os, struct, sys
import numpy as np

FS = 48000


def read_wav(path):
    raw = open(path, "rb").read(); pos = 12; fmt = data = None
    while pos + 8 <= len(raw):
        tag, size = raw[pos:pos + 4], struct.unpack("<I", raw[pos + 4:pos + 8])[0]
        if tag == b"fmt ": fmt = struct.unpack("<HHIIHH", raw[pos + 8:pos + 24])
        elif tag == b"data": data = raw[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    code, ch, sr, _, _, bits = fmt
    if code == 0xFFFE: code = 3 if bits == 32 else 1
    if code == 3: s = np.frombuffer(data, "<f4" if bits == 32 else "<f8").astype(np.float64)
    elif bits == 16: s = np.frombuffer(data, "<i2") / 32768.0
    else: s = np.frombuffer(data, "<i4") / 2147483648.0
    return s.reshape(-1, ch).T, sr


def write_wav(path, x, sr=FS):
    x = np.atleast_2d(np.asarray(x, dtype=np.float32)); ch = x.shape[0]
    body = x.T.astype("<f4").tobytes()
    hdr = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 3, ch, sr, sr * 4 * ch, 4 * ch, 32)
    open(path, "wb").write(hdr + b"data" + struct.pack("<I", len(body)) + body)


def grid(f0, f1, step):
    n = int(math.floor(math.log(f1 / f0) / math.log(step))) + 1
    return f0 * step ** np.arange(n)


def make_tones(secs, f0=60.0, f1=12000.0, step=1.02, m=24, db=-36.0, sr=FS):
    f = grid(f0, f1, step); rng = np.random.default_rng(1234)
    ph = rng.uniform(0, 2 * np.pi, len(f)); n = np.arange(int(secs * sr))
    amp = 10 ** (db / 20); out = []
    for j in range(m):
        x = np.zeros(len(n))
        for k in range(j, len(f), m):
            x += amp * np.sin(2 * np.pi * f[k] * n / sr + ph[k])
        fade = 480; x[:fade] *= np.linspace(0, 1, fade)
        out.append(x)
    return f, out


def gen(a):
    os.makedirs(a.dir, exist_ok=True)
    f, xs = make_tones(a.secs, a.f0, a.f1, a.step, a.m, a.db)
    for j, x in enumerate(xs): write_wav(os.path.join(a.dir, f"tone_{j:02d}.wav"), x)
    open(os.path.join(a.dir, "grid.txt"), "w").write(f"{a.f0} {a.f1} {a.step} {a.m} {a.secs}\n")
    print(f"{len(f)} tones in {a.m} files, peak {20*np.log10(max(np.abs(x).max() for x in xs)):.1f} dBFS")


def read_grid(d):
    f0, f1, step, m, secs = open(os.path.join(d, "grid.txt")).read().split()
    return float(f0), float(f1), float(step), int(m), float(secs)


def bh_centered_spectrum(theta, L):
    """DTFT of a centred Blackman-Harris window of length L (real, even)."""
    a = (0.35875, 0.48829, 0.14128, 0.01168); d = 2 * np.pi / (L - 1)
    def D(t):
        s = np.sin(t / 2); small = np.abs(s) < 1e-12
        return np.where(small, L, np.sin(t * L / 2) / np.where(small, 1, s))
    W = a[0] * D(theta)
    for k in (1, 2, 3):
        W = W + 0.5 * a[k] * (D(theta - k * d) + D(theta + k * d))
    return W


def envelopes(sig, sr, freqs, q, hop, nfft):
    """|windowed demodulation| of sig at each freq, on the time grid hop*i (window centre)."""
    S = np.fft.fft(sig, nfft); M = nfft // hop; out = np.empty((len(freqs), M))
    for i, f in enumerate(freqs):
        L = int(np.clip(round(q * sr / f), 256, 12288)); half = int(math.ceil(6.0 * nfft / L))
        k0 = f * nfft / sr; kc = int(round(k0))
        bins = np.arange(kc - half, kc + half + 1)
        W = bh_centered_spectrum(2 * np.pi * (bins - k0) / nfft, L)
        band = np.zeros(M, complex)
        band[(bins - kc) % M] = S[bins % nfft] * W
        e = np.fft.ifft(band) * M / nfft
        out[i] = np.abs(e)
    return out


def H_from_dirs(indir, outdir, q, hop, ch=0, gain_db=0.0):
    f0, f1, step, m, secs = read_grid(indir); f = grid(f0, f1, step)
    H = None
    for j in range(m):
        x, sr = read_wav(os.path.join(indir, f"tone_{j:02d}.wav"))
        y, sr2 = read_wav(os.path.join(outdir, f"tone_{j:02d}.wav")); assert sr == sr2
        n = min(x.shape[1], y.shape[1]); nfft = 1 << int(math.ceil(math.log2(n + 12288)))
        ks = np.arange(j, len(f), m)
        X = envelopes(x[0][:n], sr, f[ks], q, hop, nfft); Y = envelopes(y[min(ch, y.shape[0]-1)][:n], sr, f[ks], q, hop, nfft)
        if H is None:
            nt = (n - 6144) // hop; H = np.zeros((nt, len(f))); t = np.arange(nt) * hop / sr  # drop the tail a long window would overrun
        # input tones are constant: use the median input envelope as reference
        ref = np.median(X[:, int(0.2 * nt):int(0.8 * nt)], axis=1)
        H[:, ks] = (Y[:, :nt] / ref[:, None]).T
    return f, step, t, H * 10 ** (gain_db / 20), secs


def frame_notches(f, step, d, P, lo, hi, prom):
    """Notches in one frame: list of (freq, depth_db)."""
    nn = []; sel = np.nonzero((f >= lo) & (f <= hi))[0]
    a, b = sel[0], sel[-1]
    for k in range(a + 1, b):
        if not (d[k] <= d[k - 1] and d[k] < d[k + 1]): continue
        l = k
        while l > a and d[l - 1] >= d[l]: l -= 1
        r = k
        while r < b and d[r + 1] >= d[r]: r += 1
        if min(d[l], d[r]) - d[k] < prom: continue
        y0, y1, y2 = P[k - 1], P[k], P[k + 1]; den = y0 - 2 * y1 + y2
        off = float(np.clip(0.5 * (y0 - y2) / den, -0.5, 0.5)) if den > 0 else 0.0
        ymin = max(y1 - (y0 - y2) ** 2 / (8 * den) if den > 0 else y1, 1e-14)
        nn.append((f[k] * step ** off, 10 * math.log10(ymin)))
    return nn


def crossings(T, lg):
    """Upward crossings of the log-mid level, with hysteresis (10 % of the log range)."""
    mid = 0.5 * (lg.max() + lg.min()); h = 0.1 * (lg.max() - lg.min()); up = []; armed = lg[0] < mid - h
    for i in range(1, len(T)):
        if lg[i] < mid - h: armed = True
        if armed and lg[i - 1] < mid <= lg[i] and T[i] - T[i - 1] < 0.05:
            up.append(T[i - 1] + (mid - lg[i - 1]) / (lg[i] - lg[i - 1]) * (T[i] - T[i - 1])); armed = False
    return np.array(up)


def para_ext(T, y, i, sign):
    """Parabola through frames i-2..i+2 (time) around an extreme; returns refined extreme value."""
    lo, hi = max(i - 3, 0), min(i + 4, len(y))
    if hi - lo < 4: return y[i]
    c = np.polyfit(T[lo:hi] - T[i], y[lo:hi], 2)
    if c[0] * sign >= 0: return y[i]
    v = c[2] - c[1] ** 2 / (4 * c[0])
    return v if abs(v - y[i]) < 0.05 else y[i]


def summarise(t, f, step, H, lo, hi, prom, skip, nmax=2):
    keep = t >= skip; t = t[keep]; H = H[keep]
    db = 20 * np.log10(np.maximum(H, 1e-9)); P = H ** 2
    band = (f >= lo) & (f <= hi)
    rows = [frame_notches(f, step, db[i], P[i], lo, hi, prom) for i in range(len(t))]
    pk = db[:, band].max(axis=1)
    res = {"peak_db": float(np.median(pk)), "peak_max_db": float(pk.max())}
    # notch identity by continuity: with two or more notches, the lowest two are n1, n2;
    # a lone notch goes to whichever of the last n1/n2 it is nearer (log distance);
    # a value more than 10 % away from its own previous value within 5 ms is dropped.
    tr = [[None] * len(t) for _ in range(nmax)]; last = [None] * nmax; lastt = [-1.0] * nmax
    for i in range(len(t)):
        nn = sorted(rows[i])
        if len(nn) >= nmax: cand = {k: nn[k] for k in range(nmax)}
        elif len(nn) == 1 and all(v is not None for v in last):
            d = [abs(math.log(nn[0][0] / v[0])) for v in last]; cand = {int(np.argmin(d)): nn[0]}
        elif len(nn) >= 1 and last[0] is None: cand = {0: nn[0]}
        else: cand = {}
        for k, v in cand.items():
            if last[k] is not None and t[i] - lastt[k] < 0.005 and abs(math.log(v[0] / last[k][0])) > 0.1:
                continue
            tr[k][i] = v; last[k] = v; lastt[k] = t[i]
    for idx in range(nmax):
        sel = [i for i in range(len(t)) if tr[idx][i] is not None]
        if len(sel) < 10:
            res[f"n{idx+1}"] = None; continue
        T = t[sel]; F = np.array([tr[idx][i][0] for i in sel]); D = np.array([tr[idx][i][1] for i in sel])
        lg = np.log(F); up = crossings(T, lg)
        per = float(np.mean(np.diff(up))) if len(up) > 1 else float("nan")
        mins, maxs = [], []
        if len(up) > 1:
            for c0, c1 in zip(up[:-1], up[1:]):
                s = np.nonzero((T >= c0) & (T < c1))[0]
                if len(s) < 8: continue
                i = s[np.argmax(lg[s])]; maxs.append(para_ext(T, lg, i, -1))
                i = s[np.argmin(lg[s])]; mins.append(para_ext(T, lg, i, +1))
        if not mins:
            mins, maxs = [lg.min()], [lg.max()]
        res[f"n{idx+1}"] = dict(min=math.exp(np.median(mins)), max=math.exp(np.median(maxs)), period=per,
                                depth=float(np.median(D)), cycles=len(mins), frames=len(T), T=T, F=F)
    res["nframes"] = len(t)
    return res


def best_period(r):
    c = [r[k] for k in ("n1", "n2") if r.get(k) and r[k]["cycles"] >= 1 and np.isfinite(r[k]["period"])]
    return max(c, key=lambda d: (d["cycles"], d["frames"]))["period"] if c else float("nan")


def fmt_summary(label, r):
    n1, n2 = r.get("n1"), r.get("n2")
    g = lambda d, k, p: (f"{d[k]:.{p}f}" if d else "nan")
    return "\t".join([label, g(n1, "min", 1), g(n1, "max", 1), g(n2, "min", 0), g(n2, "max", 0),
                      f"{best_period(r):.4f}", g(n1, "depth", 2), f"{r['peak_db']:.2f}", f"{r['peak_max_db']:.2f}",
                      str(n1["cycles"] if n1 else 0), f"{n1['frames'] if n1 else 0}/{r['nframes']}"])


HEADER = "label\tn1_min_hz\tn1_max_hz\tn2_min_hz\tn2_max_hz\tperiod_s\tdepth_db\tpeak_db\tpeak_max_db\tcycles\tframes"


def ana(a):
    f, step, t, H, _ = H_from_dirs(a.indir, a.outdir, a.q, a.hop, a.ch, a.gain_db)
    r = summarise(t, f, step, H, a.lo, a.hi, a.prom, a.skip)
    if a.dump and r.get("n1"):
        with open(a.dump, "w") as fh:
            for tt, ff in zip(r["n1"]["T"], r["n1"]["F"]): fh.write(f"{tt:.5f}\t{ff:.2f}\n")
    print(fmt_summary(a.label or os.path.basename(a.outdir.rstrip('/')), r))
    return r


# ------------------------------------------------------------------ self-test
def synth_phaser(xs, fc):
    """4-stage TPT allpass, out = 0.5 x + 0.5 A^4 x, all stages share fc (per sample)."""
    X = np.stack(xs, axis=1); G = np.tan(np.pi * fc / FS); G = G / (1 + G)
    s = np.zeros((4, X.shape[1])); Y = np.empty_like(X)
    for n in range(X.shape[0]):
        u = X[n]; g = G[n]
        for k in range(4):
            v = (u - s[k]) * g; lp = v + s[k]; s[k] = lp + v; u = 2 * lp - u
        Y[n] = 0.5 * X[n] + 0.5 * u
    return [Y[:, j] for j in range(Y.shape[1])]


def selftest(a):
    import tempfile
    tmp = tempfile.mkdtemp(prefix="trk_", dir=os.environ.get("TRK_TMP", None))
    t8 = math.tan(math.pi / 8)
    cases = [  # (name, rate Hz, fc lo, fc hi, shape)
        ("p90-like", 0.25, 1100, 3700, "smooth"), ("p90-like", 1.07, 1100, 3700, "smooth"),
        ("p90-like", 3.7, 1100, 3700, "smooth"), ("p90-like", 9.76, 1800, 2700, "smooth"),
        ("ss-like", 0.32, 360, 8300, "rc"), ("ss-like", 1.78, 360, 8300, "rc")]
    print("case\trate\ttrue_min\ttrk_min\terr_min%\ttrue_max\ttrk_max\terr_max%\ttrue_per\ttrk_per\terr_per%")
    worst = 0.0
    for name, rate, lo, hi, shape in cases:
        secs = 1.0 + max(3.2 / rate, 2.0); d_in = os.path.join(tmp, "in"); d_out = os.path.join(tmp, "out")
        os.makedirs(d_in, exist_ok=True); os.makedirs(d_out, exist_ok=True)
        f, xs = make_tones(secs, 60, 12000, 1.02, a.m, -36)
        for j, x in enumerate(xs): write_wav(os.path.join(d_in, f"tone_{j:02d}.wav"), x)
        open(os.path.join(d_in, "grid.txt"), "w").write(f"60 12000 1.02 {a.m} {secs}\n")
        n = len(xs[0]); ph = (rate * np.arange(n) / FS + 0.13) % 1.0
        if shape == "smooth":
            v = np.sin(2 * np.pi * ph) + 0.15 * np.sin(6 * np.pi * ph)
            v /= np.abs(v).max()
        else:
            up = ph < 0.5; u = np.where(up, 2 * ph, 2 * ph - 1); kap = 0.7
            s = (1 - np.exp(-kap * u)) / (1 - math.exp(-kap)); v = np.where(up, 2 * s - 1, 1 - 2 * s)
        fc = math.sqrt(lo * hi) * (hi / lo) ** (0.5 * v)
        ys = synth_phaser(xs, fc)
        for j, y in enumerate(ys): write_wav(os.path.join(d_out, f"tone_{j:02d}.wav"), y)
        fq, step, t, H, _ = H_from_dirs(d_in, d_out, a.q, a.hop)
        r = summarise(t, fq, step, H, 60, 12000, 3.0, 1.0)
        notch = FS / np.pi * np.arctan(np.tan(np.pi * fc / FS) * t8)
        nn = notch[FS:]; tmin, tmax = nn.min(), nn.max()
        e = [100 * (r["n1"]["min"] / tmin - 1), 100 * (r["n1"]["max"] / tmax - 1), 100 * (r["n1"]["period"] * rate - 1)]
        worst = max(worst, max(abs(x) for x in e))
        print(f"{name}\t{rate}\t{tmin:.1f}\t{r['n1']['min']:.1f}\t{e[0]:+.2f}\t{tmax:.1f}\t{r['n1']['max']:.1f}\t{e[1]:+.2f}\t"
              f"{1/rate:.4f}\t{r['n1']['period']:.4f}\t{e[2]:+.2f}\tdepth {r['n1']['depth']:.1f} peak {r['peak_db']:.2f}")
    print(f"worst |error| {worst:.2f} %")
    import shutil; shutil.rmtree(tmp)


def check(a):
    """Print names of output files that are missing, short, or contain NaN/Inf."""
    m = read_grid(a.indir)[3]
    for j in range(m):
        b = f"tone_{j:02d}.wav"; po = os.path.join(a.outdir, b)
        try:
            x, _ = read_wav(os.path.join(a.indir, b)); y, _ = read_wav(po)
            if y.shape[1] < x.shape[1] or not np.all(np.isfinite(y)): print(b)
        except Exception:
            print(b)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen"); g.add_argument("dir"); g.add_argument("--secs", type=float, default=6)
    g.add_argument("--f0", type=float, default=60); g.add_argument("--f1", type=float, default=12000)
    g.add_argument("--step", type=float, default=1.02); g.add_argument("--m", type=int, default=36)
    g.add_argument("--db", type=float, default=-36)
    s = sub.add_parser("ana"); s.add_argument("indir"); s.add_argument("outdir")
    st = sub.add_parser("selftest"); st.add_argument("--m", type=int, default=36)
    for p in (s, st):
        p.add_argument("--q", type=float, default=8); p.add_argument("--hop", type=int, default=16)
    s.add_argument("--skip", type=float, default=1.0); s.add_argument("--lo", type=float, default=60)
    s.add_argument("--hi", type=float, default=12000); s.add_argument("--prom", type=float, default=3)
    s.add_argument("--gain-db", type=float, default=0.0); s.add_argument("--ch", type=int, default=0)
    s.add_argument("--dump"); s.add_argument("--label")
    c = sub.add_parser("check"); c.add_argument("indir"); c.add_argument("outdir")
    a = ap.parse_args()
    {"gen": gen, "ana": ana, "selftest": selftest, "check": check}[a.cmd](a)


if __name__ == "__main__":
    main()
