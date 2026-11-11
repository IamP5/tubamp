#!/usr/bin/env python3
"""Chorus black-box analyses (numpy only). Lives in $RE_STATE/work/chorus/tools.

  cx.py gen clicks out.wav --period 2400 --secs 8 --db -6 [--stereo]   impulse train
  cx.py taps out.wav --period 2400 [--ch 0] [--thr -18] [--skip 0]     delay taps per click frame
  cx.py fm out.wav --freq 1000 [--ch 0]                                 instantaneous-frequency trace of a sine
  cx.py width out.wav                                                   L/R correlation, mid/side energy
  cx.py spec in.wav out.wav [--ch 0] [--points 40]                      long-term average magnitude ratio
"""
import argparse, os, sys
import numpy as np
sys.path.insert(0, os.environ["RE"])
from sig import read_wav as _rw, write_wav, db, amp
def read_wav(p):
    x, sr = _rw(p)
    return x.T.copy(), sr

def analytic(x):
    n = len(x); X = np.fft.fft(x); h = np.zeros(n)
    h[0] = 1
    if n % 2 == 0: h[n // 2] = 1; h[1:n // 2] = 2
    else: h[1:(n + 1) // 2] = 2
    return np.fft.ifft(X * h)

def gen(a):
    n = int(a.secs * a.sr); x = np.zeros(n, dtype=np.float32)
    x[a.period // 4::a.period] = amp(a.db)
    if a.stereo: x = np.stack([x, x], axis=1)
    write_wav(a.out, x, a.sr)

def taps(a):
    y, sr = read_wav(a.wav)
    y = y[:, a.ch].astype(np.float64)
    P = a.period; pre = 24; off = P // 4 - pre
    print("# t_s	n_taps	(delay_ms:amp_signed)...   delay measured from the click; amp is the largest sample near the envelope peak")
    f = a.skip
    while off + f * P + P <= len(y):
        seg = y[off + f * P: off + f * P + P]
        env = np.abs(analytic(seg))
        peak = np.abs(seg).max()
        if peak < 1e-7: print(f"{(off+pre+f*P)/sr:.4f}	0"); f += 1; continue
        thr = env[8:P-8].max() * amp(a.thr)
        out = []
        for i in range(8, P - 8):
            if env[i] >= thr and env[i] >= env[i-1] and env[i] > env[i+1]:
                d = env[i-1] - 2*env[i] + env[i+1]
                frac = 0.5*(env[i-1]-env[i+1])/d if d != 0 else 0.0
                j = int(np.argmax(np.abs(seg[i-2:i+3]))) + i - 2
                out.append(((i + frac - pre) / sr * 1000.0, seg[j]))
        s = "	".join(f"{d:.4f}:{v:+.4f}" for d, v in out[:a.max])
        print(f"{(off+pre+f*P)/sr:.4f}	{len(out)}	{s}")
        f += 1

def frame_taps(seg, sr, pre, thr_db, mind_ms=0.5):
    env = np.abs(analytic(seg)); P = len(seg)
    lo = pre + int(mind_ms*sr/1000)
    if np.abs(seg[lo:P-8]).max() < 1e-6: return []
    thr = env[lo:P-8].max() * amp(thr_db); out = []
    for i in range(lo, P - 8):
        if env[i] >= thr and env[i] >= env[i-1] and env[i] > env[i+1]:
            d = env[i-1] - 2*env[i] + env[i+1]
            frac = 0.5*(env[i-1]-env[i+1])/d if d != 0 else 0.0
            j = int(np.argmax(np.abs(seg[i-2:i+3]))) + i - 2
            if abs(seg[j]) >= 0.5*thr: out.append(((i + frac - pre) / sr * 1000.0, seg[j], env[i]))
    return out

def trk(a):
    """Track the k-th wet tap (by delay order, or the strongest with --k -1) across click frames."""
    y, sr = read_wav(a.wav)
    y = y[:, a.ch].astype(np.float64)
    P = a.period; pre = 24; off = P // 4 - pre
    ts, ds, av, nt = [], [], [], []
    f = a.skip
    while off + f * P + P <= len(y):
        tp = [(d - a.lat, v, e) for d, v, e in frame_taps(y[off + f*P: off + f*P + P], sr, pre, a.thr, a.mind)]
        nt.append(len(tp))
        if tp:
            if a.k < 0: t = max(tp, key=lambda q: q[2])
            elif a.k < len(tp): t = tp[a.k]
            else: t = None
            if t: ts.append((off+pre+f*P)/sr); ds.append(t[0]); av.append(t[1])
        f += 1
    ts = np.array(ts); ds = np.array(ds); av = np.array(av)
    if a.series:
        print("# t_s	delay_ms	tap_amp")
        for t, d, v in zip(ts, ds, av): print(f"{t:.4f}	{d:.4f}	{v:+.4f}")
    if len(ds) < 4: print("# no taps tracked"); return
    slope = np.diff(ds) / np.diff(ts) / 1000.0           # seconds of delay per second
    cents = 1200*np.log2(np.clip(1 - slope, 1e-6, None))
    d0 = ds - ds.mean(); w = np.hanning(len(d0)); n = 16*len(d0)
    D = np.abs(np.fft.rfft(d0*w, n)); fax = np.fft.rfftfreq(n, P/sr); kk = np.argmax(D[1:]) + 1
    rms = np.sqrt((d0**2).mean()); crest = np.abs(d0).max()/max(rms, 1e-12)
    c0 = cents - cents.mean(); crc = np.abs(c0).max()/max(np.sqrt((c0**2).mean()), 1e-12)
    if a.short:
        print(f"taps/frame {min(nt)}-{max(nt)}	delay_min_ms {ds.min():.3f}	delay_max_ms {ds.max():.3f}	pp_ms {ds.max()-ds.min():.3f}	lfo_hz {fax[kk]:.3f}	delay_crest {crest:.2f}	cents_p95 {np.percentile(np.abs(cents),95):.1f}	cents_max {np.abs(cents).max():.1f}	cents_crest {crc:.2f}	tap_amp {np.abs(av).mean():.4f}{'+' if av.mean()>0 else '-'}"); return
    print(f"# frames {len(nt)} taps_per_frame min {min(nt)} max {max(nt)} | delay_ms min {ds.min():.3f} max {ds.max():.3f} mean {ds.mean():.3f} pp {ds.max()-ds.min():.3f} | lfo_hz {fax[kk]:.3f} | delay_crest {crest:.3f} (sine 1.414 tri 1.732) | cents min {cents.min():+.1f} max {cents.max():+.1f} p95abs {np.percentile(np.abs(cents),95):.1f} cents_crest {crc:.2f} (square 1.0 sine 1.41) | tap_amp mean {np.abs(av).mean():.4f} sign {'+' if av.mean()>0 else '-'}")

def tapfr(a):
    """Wet-path response from the isolated strongest wet tap of each click frame, relative to the click."""
    y, sr = read_wav(a.wav)
    y = y[:, a.ch].astype(np.float64)
    P = a.period; pre = 24; off = P // 4 - pre
    freqs = [float(v) for v in a.freqs.split(",")]
    half = int(a.win_ms*sr/1000); N = 4096
    rows = []; f = a.skip
    while off + f * P + P <= len(y):
        seg = y[off + f*P: off + f*P + P]
        tp = frame_taps(seg, sr, pre, -6, a.mind)
        if tp:
            t = max(tp, key=lambda q: q[2]); c = int(round(t[0]*sr/1000)) + pre
            lo = max(c - half, pre + 4); hi = min(c + half, P)
            w = seg[lo:hi] * np.hanning(hi - lo) ** 0.25
            H = np.abs(np.fft.rfft(w, N)) / amp(a.db); fax = np.fft.rfftfreq(N, 1/sr)
            rows.append([db(np.interp(q, fax, H)) for q in freqs])
        f += 1
    R = np.array(rows)
    print("# freq_hz	gain_db_min	gain_db_median	gain_db_max	(n_frames=%d, window +-%.1f ms around the strongest wet tap)" % (len(R), a.win_ms))
    for i, q in enumerate(freqs): print(f"{q:.0f}	{R[:,i].min():.2f}	{np.median(R[:,i]):.2f}	{R[:,i].max():.2f}")

def lr(a):
    """Phase of the R delay LFO relative to L (degrees), from the strongest wet tap per click."""
    y, sr = read_wav(a.wav); P = a.period; pre = 24; off = P // 4 - pre
    ser = []
    for ch in (0, 1):
        v = y[:, ch].astype(np.float64); ts = []; ds = []; f = 0
        while off + f*P + P <= len(v):
            tp = frame_taps(v[off+f*P: off+f*P+P], sr, pre, a.thr, a.mind)
            if tp: t = max(tp, key=lambda q: q[2]); ts.append((off+pre+f*P)/sr); ds.append(t[0])
            f += 1
        ser.append((np.array(ts), np.array(ds)))
    (t0, d0), (t1, d1) = ser
    x = d0 - d0.mean(); n = 32*len(x); D = np.abs(np.fft.rfft(x*np.hanning(len(x)), n)); fax = np.fft.rfftfreq(n, P/sr); f0 = fax[np.argmax(D[1:])+1]
    c0 = np.sum((d0-d0.mean())*np.exp(-2j*np.pi*f0*t0)); c1 = np.sum((d1-d1.mean())*np.exp(-2j*np.pi*f0*t1))
    ph = np.degrees(np.angle(c1/c0))
    print(f"lfo_hz {f0:.3f}	R_minus_L_phase_deg {ph:+.1f}	L_pp_ms {d0.max()-d0.min():.3f}	R_pp_ms {d1.max()-d1.min():.3f}	L_range_ms {d0.min()-a.lat:.3f}-{d0.max()-a.lat:.3f}	R_range_ms {d1.min()-a.lat:.3f}-{d1.max()-a.lat:.3f}")

def ir(a):
    y, sr = read_wav(a.wav)
    y = y[:, a.ch].astype(np.float64)
    P = a.period; start = P // 4 + a.frame * P
    seg = y[start: start + P]
    lo = int(a.t0 * sr / 1000); hi = int(a.t1 * sr / 1000)
    print("# n	delay_ms	value")
    for n in range(lo, hi): print(f"{n}	{n/sr*1000:.4f}	{seg[n]:+.6f}")

def fm(a):
    y, sr = read_wav(a.wav)
    if y.ndim == 1: y = y[:, None]
    y = y[:, a.ch].astype(np.float64)
    z = analytic(y)
    # band-limit around the carrier to drop harmonics/noise
    Z = np.fft.fft(z); fr = np.fft.fftfreq(len(z), 1/sr)
    Z[(fr < a.freq*(1-a.bw)) | (fr > a.freq*(1+a.bw))] = 0
    z = np.fft.ifft(Z)
    ph = np.unwrap(np.angle(z)); inst = np.diff(ph) * sr / (2*np.pi)
    envl = np.abs(z)
    hop = int(sr * a.hop_ms / 1000)
    lo = int(sr*a.t0); hi = len(inst) - int(sr*0.05)
    print("# t_s\tfreq_hz\tcents\tlevel_db")
    cents_all = []
    for i in range(lo, hi, hop):
        fq = inst[i:i+hop].mean(); c = 1200*np.log2(max(fq,1e-9)/a.freq); cents_all.append(c)
        if not a.summary: print(f"{i/sr:.4f}\t{fq:.3f}\t{c:+.2f}\t{db(envl[i:i+hop].mean()):.2f}")
    c = np.array(cents_all); c0 = c - c.mean()
    # LFO rate from FFT of the cents trace
    w = np.hanning(len(c0)); C = np.abs(np.fft.rfft(c0*w, 8*len(c0))); fax = np.fft.rfftfreq(8*len(c0), a.hop_ms/1000)
    k = np.argmax(C[1:])+1
    lev = np.array([db(envl[i:i+hop].mean()) for i in range(lo, hi, hop)])
    print(f"# cents_min {c.min():+.2f} cents_max {c.max():+.2f} cents_mean {c.mean():+.3f} lfo_hz {fax[k]:.4f} level_min_db {lev.min():.2f} level_max_db {lev.max():.2f}")
    # shape: crest factor of cents trace (sine 1.414, triangle 1.732, square 1.0)
    rms = np.sqrt((c0**2).mean()); print(f"# cents_crest {np.abs(c0).max()/max(rms,1e-12):.3f} (sine 1.414, triangle 1.732, square 1.000)")

def width(a):
    y, sr = read_wav(a.wav)
    L = y[:, 0].astype(np.float64); R = y[:, 1].astype(np.float64)
    n0 = int(sr*a.t0); L = L[n0:]; R = R[n0:]
    M = 0.5*(L+R); S = 0.5*(L-R)
    r = np.dot(L, R)/np.sqrt(np.dot(L, L)*np.dot(R, R)+1e-30)
    print("# corr\tL_rms_db\tR_rms_db\tmid_rms_db\tside_rms_db\tside_minus_mid_db")
    f = lambda v: db(np.sqrt((v**2).mean()))
    print(f"{r:+.4f}\t{f(L):.2f}\t{f(R):.2f}\t{f(M):.2f}\t{f(S):.2f}\t{f(S)-f(M):.2f}")

def spec(a):
    x, sr = read_wav(a.inp); y, _ = read_wav(a.out)
    if x.ndim > 1: x = x[:, 0]
    if a.mono_sum and y.ndim > 1: y = 0.5*(y[:,0]+y[:,1])
    elif y.ndim > 1: y = y[:, a.ch]
    n = min(len(x), len(y)); x = x[:n].astype(np.float64); y = y[:n].astype(np.float64)
    nfft = a.nfft; hop = nfft//2; w = np.hanning(nfft)
    Px = np.zeros(nfft//2+1); Py = np.zeros(nfft//2+1)
    for i in range(int(sr*a.t0), n-nfft, hop):
        Px += np.abs(np.fft.rfft(x[i:i+nfft]*w))**2; Py += np.abs(np.fft.rfft(y[i:i+nfft]*w))**2
    fax = np.fft.rfftfreq(nfft, 1/sr)
    edges = np.geomspace(a.f0, a.f1, a.points+1)
    print("# freq_hz\tgain_db")
    for lo, hi in zip(edges[:-1], edges[1:]):
        m = (fax >= lo) & (fax < hi)
        if m.any(): print(f"{np.sqrt(lo*hi):.1f}\t{10*np.log10(Py[m].sum()/Px[m].sum()+1e-30):.2f}")

p = argparse.ArgumentParser(); sp = p.add_subparsers(dest="cmd", required=True)
g = sp.add_parser("gen"); g.add_argument("kind"); g.add_argument("out"); g.add_argument("--period", type=int, default=2400); g.add_argument("--secs", type=float, default=8); g.add_argument("--db", type=float, default=-6); g.add_argument("--sr", type=int, default=48000); g.add_argument("--stereo", action="store_true"); g.set_defaults(fn=gen)
t = sp.add_parser("taps"); t.add_argument("wav"); t.add_argument("--period", type=int, default=2400); t.add_argument("--ch", type=int, default=0); t.add_argument("--thr", type=float, default=-18); t.add_argument("--skip", type=int, default=0); t.add_argument("--max", type=int, default=12); t.set_defaults(fn=taps)
k = sp.add_parser("trk"); k.add_argument("wav"); k.add_argument("--period", type=int, default=2400); k.add_argument("--ch", type=int, default=0); k.add_argument("--thr", type=float, default=-12); k.add_argument("--skip", type=int, default=0); k.add_argument("--k", type=int, default=-1); k.add_argument("--mind", type=float, default=0.5); k.add_argument("--lat", type=float, default=0.0); k.add_argument("--series", action="store_true"); k.add_argument("--short", action="store_true"); k.set_defaults(fn=trk)
u = sp.add_parser("tapfr"); u.add_argument("wav"); u.add_argument("--period", type=int, default=2400); u.add_argument("--ch", type=int, default=0); u.add_argument("--skip", type=int, default=0); u.add_argument("--db", type=float, default=-6); u.add_argument("--win-ms", dest="win_ms", type=float, default=1.5); u.add_argument("--mind", type=float, default=0.5); u.add_argument("--freqs", default="50,100,200,500,1000,2000,3000,5000,7000,10000,12000,15000,18000"); u.set_defaults(fn=tapfr)
z = sp.add_parser("lr"); z.add_argument("wav"); z.add_argument("--period", type=int, default=2400); z.add_argument("--thr", type=float, default=-12); z.add_argument("--mind", type=float, default=0.5); z.add_argument("--lat", type=float, default=0.0); z.set_defaults(fn=lr)
q = sp.add_parser("ir"); q.add_argument("wav"); q.add_argument("--period", type=int, default=2400); q.add_argument("--ch", type=int, default=0); q.add_argument("--frame", type=int, default=4); q.add_argument("--t0", type=float, default=0); q.add_argument("--t1", type=float, default=1); q.set_defaults(fn=ir)
f = sp.add_parser("fm"); f.add_argument("wav"); f.add_argument("--freq", type=float, default=1000); f.add_argument("--ch", type=int, default=0); f.add_argument("--bw", type=float, default=0.3); f.add_argument("--hop-ms", dest="hop_ms", type=float, default=10); f.add_argument("--t0", type=float, default=0.5); f.add_argument("--summary", action="store_true"); f.set_defaults(fn=fm)
w = sp.add_parser("width"); w.add_argument("wav"); w.add_argument("--t0", type=float, default=0.5); w.set_defaults(fn=width)
s = sp.add_parser("spec"); s.add_argument("inp"); s.add_argument("out"); s.add_argument("--ch", type=int, default=0); s.add_argument("--points", type=int, default=40); s.add_argument("--nfft", type=int, default=8192); s.add_argument("--f0", type=float, default=30); s.add_argument("--f1", type=float, default=20000); s.add_argument("--t0", type=float, default=0.5); s.add_argument("--mono-sum", dest="mono_sum", action="store_true"); s.set_defaults(fn=spec)
a = p.parse_args(); a.fn(a)
