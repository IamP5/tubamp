#!/usr/bin/env python3
"""Phaser notch tracking with a periodic flat-spectrum probe (numpy only).

  ptrack.py gen out.wav [--period 2048 --secs 12 --db -20 --sr 48000]
  ptrack.py track in.wav out.wav [--period 2048 --hop 256 --fmin 60 --fmax 14000 --prom 6 --skip 0.5]
        -> TSV: t, n_notches, notch Hz/dB list, peak (max |H|) Hz/dB, mean |H| dB
  ptrack.py summary track.tsv [--n 2]
        -> per notch: min/max Hz, LFO period (s), rise fraction, triangle-vs-sine fit of log f(t)
  ptrack.py snap in.wav out.wav --t 1.0 [--period 2048]   -> |H(f)| dB table at one instant

The probe is one period of random-phase, unit-magnitude noise repeated, so any period-long
window of input and output gives H = FFT(out)/FFT(in) without leakage.
"""
import argparse, struct, sys
import numpy as np

def write_wav(path, x, sr):
    x = np.atleast_2d(np.asarray(x, dtype=np.float32))
    if x.shape[0] > x.shape[1]: x = x.T
    ch, n = x.shape; body = x.T.astype('<f4').tobytes()
    hdr = b'RIFF' + struct.pack('<I', 36+len(body)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 3, ch, int(sr), int(sr)*ch*4, ch*4, 32)
    open(path, 'wb').write(hdr + b'data' + struct.pack('<I', len(body)) + body)

def read_wav(path):
    raw = open(path, 'rb').read(); pos = 12; fmt = data = None
    while pos + 8 <= len(raw):
        tag, size = raw[pos:pos+4], struct.unpack('<I', raw[pos+4:pos+8])[0]
        chunk = raw[pos+8:pos+8+size]
        if tag == b'fmt ': fmt = struct.unpack('<HHIIHH', chunk[:16])
        elif tag == b'data': data = chunk
        pos += 8 + size + (size & 1)
    code, ch, sr, _, _, bits = fmt
    if code == 0xFFFE: code = 3 if bits == 32 else 1
    if code == 3: a = np.frombuffer(data, '<f4' if bits == 32 else '<f8').astype(np.float64)
    elif bits == 16: a = np.frombuffer(data, '<i2') / 32768.0
    elif bits == 24:
        b = np.frombuffer(data, np.uint8).reshape(-1, 3).astype(np.int32)
        a = ((b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)) << 8 >> 8) / 8388608.0
    else: a = np.frombuffer(data, '<i4') / 2147483648.0
    return a.reshape(-1, ch).T, sr

def probe(period, secs, db, sr, seed=7):
    rng = np.random.default_rng(seed)
    spec = np.zeros(period//2 + 1, complex)
    spec[1:period//2] = np.exp(2j*np.pi*rng.random(period//2 - 1))
    one = np.fft.irfft(spec, period); one *= 10**(db/20) / np.sqrt(np.mean(one**2))
    return np.tile(one, int(np.ceil(secs*sr/period)))[:int(secs*sr)]

def tf_frames(x, y, period, hop, skip, sr):
    start = int(skip*sr); out = []
    X0 = None
    for k in range(start, min(len(x), len(y)) - period, hop):
        X = np.fft.rfft(x[k:k+period]); Y = np.fft.rfft(y[k:k+period])
        H = np.zeros(len(X), complex); H[1:-1] = Y[1:-1] / X[1:-1]
        out.append(((k + period/2)/sr, H))
    return out

def find_notches(f, db, fmin, fmax, prom):
    idx = []
    lo = np.searchsorted(f, fmin); hi = np.searchsorted(f, fmax)
    for i in range(max(lo, 2), min(hi, len(f)-2)):
        if db[i] < db[i-1] and db[i] <= db[i+1]:
            # prominence: lower of the two maxima either side, up to the next lower minimum
            l = i
            while l > 1 and db[l-1] >= db[i]: l -= 1
            r = i
            while r < len(f)-2 and db[r+1] >= db[i]: r += 1
            left = db[max(l, lo-8):i].max() if i > max(l, lo-8) else db[i]
            right = db[i+1:min(r, hi+8)+1].max() if min(r, hi+8) > i else db[i]
            p = min(left, right) - db[i]
            if p >= prom:
                # refine on linear magnitude: |H|^2 is near-quadratic around a notch
                a, b, c = (10**(db[i-1]/10)), (10**(db[i]/10)), (10**(db[i+1]/10))
                den = a - 2*b + c; d = 0.5*(a - c)/den if den > 0 else 0.0
                d = float(np.clip(d, -0.5, 0.5))
                idx.append((f[i] + d*(f[1]-f[0]), db[i], p))
    return idx

def merge_notches(nn, ratio):
    """Cluster minima closer than `ratio` (a sweeping notch smears into a ragged trough within one frame)."""
    out = []; cur = []
    for n in nn:
        if cur and n[0] / cur[-1][0] > ratio:
            out.append(cur); cur = []
        cur.append(n)
    if cur: out.append(cur)
    res = []
    for c in out:
        fs = np.array([n[0] for n in c]); w = np.array([10**(-n[1]/10) for n in c])
        res.append((float(np.exp((np.log(fs)*w).sum()/w.sum())), min(n[1] for n in c), max(n[2] for n in c)))
    return res

def cmd_track(a):
    x, sr = read_wav(a.inp); y, sr2 = read_wav(a.out)
    x = x[0]; y = y[a.ch]
    f = np.fft.rfftfreq(a.period, 1/sr)
    print('# t_s\tn\tnotches_hz\tnotches_db\tpeak_hz\tpeak_db\tmean_db')
    for t, H in tf_frames(x, y, a.period, a.hop, a.skip, sr):
        db = 20*np.log10(np.abs(H) + 1e-12)
        nn = merge_notches(find_notches(f, db, a.fmin, a.fmax, a.prom), a.merge)
        band = (f >= a.fmin) & (f <= a.fmax)
        pk = np.argmax(np.where(band, db, -999))
        mean = 10*np.log10(np.mean(np.abs(H[band])**2))
        print(f"{t:.4f}\t{len(nn)}\t{','.join(f'{n[0]:.1f}' for n in nn)}\t{','.join(f'{n[1]:.1f}' for n in nn)}\t{f[pk]:.0f}\t{db[pk]:.2f}\t{mean:.2f}")

def load_track(path, n):
    T = []; F = []; D = []; PK = []; MEAN = []
    for line in open(path):
        if line.startswith('#'): continue
        p = line.rstrip('\n').split('\t')
        if int(p[1]) != n: continue
        T.append(float(p[0])); F.append([float(v) for v in p[2].split(',')]); D.append([float(v) for v in p[3].split(',')])
        PK.append((float(p[4]), float(p[5]))); MEAN.append(float(p[6]))
    return np.array(T), np.array(F), np.array(D), np.array(PK), np.array(MEAN)

def lfo_period(t, v):
    """Period from the autocorrelation of a uniformly resampled, mean-removed track."""
    if len(t) < 16: return float('nan')
    dt = np.median(np.diff(t)); tu = np.arange(t[0], t[-1], dt); vu = np.interp(tu, t, v)
    vu = vu - vu.mean(); ac = np.correlate(vu, vu, 'full')[len(vu)-1:]; ac /= ac[0]
    # first peak after the first zero crossing
    z = np.where(ac < 0)[0]
    if len(z) == 0: return float('nan')
    i0 = z[0]; seg = ac[i0:]
    pk = [i for i in range(1, len(seg)-1) if seg[i] > seg[i-1] and seg[i] >= seg[i+1] and seg[i] > 0.3]
    if not pk: return float('nan')
    i = pk[0] + i0
    a_, b_, c_ = ac[i-1], ac[i], ac[i+1]; d = 0.5*(a_-c_)/(a_-2*b_+c_)
    return (i + d)*dt

def shape_fit(t, v, period):
    """Least-squares fit of v(t) to offset + amp*shape(phase) for triangle and sine; returns R2 each and rise fraction."""
    if not np.isfinite(period): return float('nan'), float('nan'), float('nan')
    best = {}
    for name in ('tri', 'sin'):
        r2best = -1
        for ph in np.linspace(0, 1, 200, endpoint=False):
            u = (t/period + ph) % 1.0
            s = (1 - 4*np.abs(u - 0.5)) if name == 'tri' else np.cos(2*np.pi*(u - 0.5))
            A = np.vstack([np.ones_like(s), s]).T; coef, res, *_ = np.linalg.lstsq(A, v, rcond=None)
            r = v - A @ coef; r2 = 1 - (r @ r)/(((v - v.mean())**2).sum() + 1e-30)
            r2best = max(r2best, r2)
        best[name] = r2best
    dv = np.diff(v); rise = float((dv > 0).sum())/max(1, (dv != 0).sum())
    return best['tri'], best['sin'], rise

def cmd_summary(a):
    T, F, D, PK, MEAN = load_track(a.track, a.n)
    total = sum(1 for l in open(a.track) if not l.startswith('#'))
    print(f'frames_with_{a.n}_notches\t{len(T)}\tof\t{total}')
    if len(T) < 8: return
    for k in range(a.n):
        lf = np.log2(F[:, k]); per = lfo_period(T, lf)
        tri, sin_, rise = shape_fit(T, lf, per)
        lin_tri, lin_sin, _ = shape_fit(T, F[:, k], per)
        print(f'notch{k+1}\tmin_hz\t{F[:,k].min():.0f}\tmax_hz\t{F[:,k].max():.0f}\tratio\t{F[:,k].max()/F[:,k].min():.2f}\tperiod_s\t{per:.4f}\t'
              f'rate_hz\t{1/per:.4f}\trise_frac\t{rise:.2f}\tlogf_R2_tri\t{tri:.4f}\tlogf_R2_sin\t{sin_:.4f}\tlinf_R2_tri\t{lin_tri:.4f}\tlinf_R2_sin\t{lin_sin:.4f}\t'
              f'depth_db_med\t{np.median(D[:,k]):.1f}')
    print(f'peak_db\tmax\t{PK[:,1].max():.2f}\tmedian\t{np.median(PK[:,1]):.2f}\tmean_level_db\t{MEAN.mean():.2f}')
    if a.n >= 2:
        print(f'notch2/notch1 ratio\tmedian\t{np.median(F[:,1]/F[:,0]):.3f}\tmin\t{(F[:,1]/F[:,0]).min():.3f}\tmax\t{(F[:,1]/F[:,0]).max():.3f}')

def cmd_snap(a):
    x, sr = read_wav(a.inp); y, _ = read_wav(a.out); x = x[0]; y = y[a.ch]
    k = int(a.t*sr); f = np.fft.rfftfreq(a.period, 1/sr)
    H = np.fft.rfft(y[k:k+a.period])[1:-1] / np.fft.rfft(x[k:k+a.period])[1:-1]
    print('# hz\tdb\tphase_deg')
    for i in range(len(H)):
        if a.fmin <= f[i+1] <= a.fmax: print(f'{f[i+1]:.1f}\t{20*np.log10(abs(H[i])+1e-12):.2f}\t{np.degrees(np.angle(H[i])):.1f}')

def main():
    ap = argparse.ArgumentParser(); sub = ap.add_subparsers(dest='cmd', required=True)
    g = sub.add_parser('gen'); g.add_argument('out'); g.add_argument('--period', type=int, default=2048); g.add_argument('--secs', type=float, default=12)
    g.add_argument('--db', type=float, default=-20); g.add_argument('--sr', type=int, default=48000)
    t = sub.add_parser('track'); t.add_argument('inp'); t.add_argument('out'); t.add_argument('--period', type=int, default=2048); t.add_argument('--hop', type=int, default=256)
    t.add_argument('--fmin', type=float, default=60); t.add_argument('--fmax', type=float, default=14000); t.add_argument('--prom', type=float, default=6); t.add_argument('--skip', type=float, default=0.5); t.add_argument('--ch', type=int, default=0); t.add_argument('--merge', type=float, default=1.6)
    s = sub.add_parser('summary'); s.add_argument('track'); s.add_argument('--n', type=int, default=2)
    n = sub.add_parser('snap'); n.add_argument('inp'); n.add_argument('out'); n.add_argument('--t', type=float, default=1.0); n.add_argument('--period', type=int, default=2048)
    n.add_argument('--fmin', type=float, default=40); n.add_argument('--fmax', type=float, default=20000); n.add_argument('--ch', type=int, default=0)
    a = ap.parse_args()
    if a.cmd == 'gen': write_wav(a.out, probe(a.period, a.secs, a.db, a.sr), a.sr)
    elif a.cmd == 'track': cmd_track(a)
    elif a.cmd == 'summary': cmd_summary(a)
    else: cmd_snap(a)
if __name__ == '__main__': main()
