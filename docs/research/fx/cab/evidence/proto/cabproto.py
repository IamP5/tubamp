#!/usr/bin/env python3
"""Numeric prototypes for the tubamp cab proposals (numpy only). Our own formulas and constants.

    cabproto.py norm <dir with txh_*.npy>     level-normalisation rules compared on a set of measured cab IRs
    cabproto.py align <dir with txh_*.npy>    dual-IR blend with and without cross-correlation alignment
    cabproto.py load                          speaker-load (resonance / presence) filter responses
    cabproto.py resample <ir.npy> <src> <dst> windowed-sinc resampler error against FFT band-limited resampling

The .npy inputs are impulse responses measured from TONEX's VIR cab (see evidence/tonex_blackbox.md);
they are test signals only and stay in $RE_STATE/work/cab/bb.
"""
import glob, os, sys
import numpy as np

SR = 48000

# ---------------------------------------------------------------- level normalisation
def k_weight(f, sr=SR):
    """ITU-R BS.1770 K-weighting magnitude (48 kHz coefficients)."""
    z = np.exp(-2j * np.pi * f / sr)
    b1 = [1.53512485958697, -2.69169618940638, 1.19839281085285]; a1 = [1.0, -1.69065929318241, 0.73248077421585]
    b2 = [1.0, -2.0, 1.0]; a2 = [1.0, -1.99004745483398, 0.99007225036621]
    h = lambda b, a: (b[0] + b[1] * z + b[2] * z * z) / (a[0] + a[1] * z + a[2] * z * z)
    return np.abs(h(b1, a1) * h(b2, a2))

def guitar_spectrum(f):
    """Power spectrum of a driven-amp output before the cab: flat 80 Hz..1 kHz, -6 dB/oct above, band 70 Hz..12 kHz."""
    s = np.where(f < 1000, 1.0, (1000 / np.maximum(f, 1)) ** 2)
    return s * (f > 70) * (f < 12000)

def pink_gain_db(h, sr=SR, n=1 << 16):
    """Rejected rule: gain of the IR for a 1/f-weighted spectrum over 80 Hz..5 kHz."""
    H = np.abs(np.fft.rfft(h, n)) ** 2; f = np.fft.rfftfreq(n, 1 / sr)
    w = np.where((f >= 80) & (f <= 5000), 1 / np.maximum(f, 1), 0.0)
    return 10 * np.log10(np.sum(w * H) / np.sum(w))

def l2_ref48_db(h, sr=SR, n=1 << 16):
    """Proposed rule: mean of |H(f)|^2 over 0..24 kHz, H in response units (IR scaled so that the response,
    not the waveform, is rate independent). Equals sum(h^2) at 48 kHz; does not move with the session rate."""
    H = np.abs(np.fft.rfft(h, n)) ** 2; f = np.fft.rfftfreq(n, 1 / sr)
    m = f <= 24000.0
    df = sr / n
    return 10 * np.log10(np.sum(H[m]) * df / 24000.0)

def loudness_db(h, sr=SR, n=1 << 16):
    """Judge: K-weighted level of guitar_spectrum through the IR, relative to the same signal without the IR."""
    H = np.abs(np.fft.rfft(h, n)) ** 2; f = np.fft.rfftfreq(n, 1 / sr)
    w = guitar_spectrum(f) * k_weight(f, sr) ** 2
    return 10 * np.log10(np.sum(w * H) / np.sum(w))

def cmd_norm(d):
    names = sorted(glob.glob(os.path.join(d, "txh_*.npy")))
    names = [p for p in names if not any(t in p for t in ("bypass", "_44100", "_96000", "blend0", "pres_", "depth_", "res_"))]
    rows = []
    for p in names:
        h = np.load(p)[:8192]
        l2 = 10 * np.log10(np.sum(h ** 2)); pk = 20 * np.log10(np.max(np.abs(h)))
        rows.append((os.path.basename(p)[4:-4], loudness_db(h) - (l2 + 18.06), loudness_db(h) - pk, loudness_db(h) - pink_gain_db(h),
                     loudness_db(h) - (l2_ref48_db(h) + 9.0), l2))
    a = np.array([r[1:] for r in rows])
    print("# K-weighted loudness (BS.1770 weighting) of a guitar-amp spectrum through each IR, after normalising the IR by four rules.")
    print("# 0 dB = as loud as the same signal with the cab off. Judge spectrum: flat 70 Hz..1 kHz, -6 dB/oct to 12 kHz.")
    print("# python3 cabproto.py norm $RE_STATE/work/cab/bb ; %d IRs measured from TONEX VIR at 48 kHz (cabs, mics, positions)" % len(rows))
    print("ir\tjuce_l2_0.125_db\tpeak_1.0_db\tpink_80_5k_db\tl2_ref48k_minus9_db\traw_l2_db")
    for r in rows: print("%s\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f" % r)
    print("mean\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f" % tuple(a.mean(0)))
    print("std\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f" % tuple(a.std(0)))
    print("max-min\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f" % tuple(a.max(0) - a.min(0)))
    lp = a[:, 4] - np.array([20 * np.log10(np.max(np.abs(np.load(p)[:8192]))) for p in names])
    print("# L2 above peak for these IRs: mean %.2f dB, min %.2f, max %.2f (a file peaking at 0 dBFS has this L2)" % (lp.mean(), lp.min(), lp.max()))
    h = np.load(os.path.join(d, "txh_vir_48000.npy"))[:8192]
    h2 = resample_fft(h, 48000, 96000) * 0.5          # response-preserving: scale by src/dst
    h3 = resample_fft(h, 48000, 44100) * (48000 / 44100)
    for nm, x, sr in (("44.1k", h3, 44100), ("48k", h, 48000), ("96k", h2, 96000)):
        g_juce = -10 * np.log10(np.sum(x ** 2)) - 18.06; g_new = -l2_ref48_db(x, sr) - 9.0
        print("# rate check, session %s: gain applied by the JUCE L2 rule %.2f dB, by the 48k-referenced rule %.2f dB" % (nm, g_juce, g_new))

# ---------------------------------------------------------------- alignment
def resample_fft(h, src, dst):
    n = len(h); pad = 1 << int(np.ceil(np.log2(n * 4)))
    H = np.fft.rfft(h, pad); m = int(round(pad * dst / src)); Hm = np.zeros(m // 2 + 1, complex)
    k = min(len(H), len(Hm)); Hm[:k] = H[:k]
    return np.fft.irfft(Hm, m)[: int(round(n * dst / src))] * (m / pad)

def frac_shift(h, d, n=None):
    n = n or 1 << int(np.ceil(np.log2(len(h) + abs(d) + 64)))
    H = np.fft.rfft(h, n); k = np.arange(len(H))
    return np.fft.irfft(H * np.exp(-2j * np.pi * k * d / n), n)

def align(a, b, sr=SR, max_ms=5.0, up=8):
    """Delay (samples, fractional) and polarity that best align b to a: peak of |cross-correlation| of the
    80 Hz..5 kHz band-limited IRs, searched within +-max_ms, on an 8x oversampled grid."""
    n = 1 << int(np.ceil(np.log2(len(a) + len(b)))); f = np.fft.rfftfreq(n, 1 / sr)
    band = (f >= 80) & (f <= 5000)
    X = np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)) * band
    Xu = np.zeros(n * up // 2 + 1, complex); Xu[: len(X)] = X
    x = np.fft.irfft(Xu, n * up); lim = int(max_ms * 1e-3 * sr * up)
    idx = np.r_[np.arange(0, lim + 1), np.arange(n * up - lim, n * up)]
    k = idx[np.argmax(np.abs(x[idx]))]; pol = 1.0 if x[k] >= 0 else -1.0
    lag = k if k <= lim else k - n * up
    return lag / up, pol                      # delay b by `lag/up` samples (negative = advance)

def blend_loss_db(a, b, n=1 << 15, sr=SR):
    """Level of the 50/50 sum against the power sum of the two, 80 Hz..5 kHz, plus the deepest 1/3-octave dip."""
    A = np.fft.rfft(a, n); B = np.fft.rfft(b, n); f = np.fft.rfftfreq(n, 1 / sr)
    m = (f >= 80) & (f <= 5000)
    tot = 10 * np.log10(np.sum(np.abs(0.5 * (A + B))[m] ** 2) / np.sum(0.25 * (np.abs(A) ** 2 + np.abs(B) ** 2)[m]))
    worst = 0.0
    for fc in 80 * 2 ** (np.arange(0, 18) / 3):
        mm = (f >= fc * 2 ** (-1 / 6)) & (f <= fc * 2 ** (1 / 6))
        worst = min(worst, 10 * np.log10(np.sum(np.abs(0.5 * (A + B))[mm] ** 2) / np.sum(0.25 * (np.abs(A) ** 2 + np.abs(B) ** 2)[mm])))
    return tot, worst

def cmd_align(d):
    a = np.load(os.path.join(d, "txh_blend0_m1.npy"))[:4096]     # condenser, close
    b = np.load(os.path.join(d, "txh_blend1_m2.npy"))[:4096]     # dynamic, far
    print("# 50/50 blend of two measured mic IRs: level against the power sum (0 dB = no cancellation; +3 dB = fully coherent)")
    print("# python3 cabproto.py align $RE_STATE/work/cab/bb")
    print("case\toffset_samples\tpolarity\tblend_db_80_5k\tworst_third_octave_db\tauto_delay_found\tauto_polarity")
    for name, off, pol in (("as measured", 0.0, 1), ("B late 12 samples (0.25 ms)", 12.0, 1), ("B late 24 samples (0.5 ms)", 24.0, 1),
                           ("B late 48 samples (1 ms)", 48.0, 1), ("B inverted", 0.0, -1), ("B late 24 and inverted", 24.0, -1)):
        bb = pol * frac_shift(b, off)[: len(b) + 64]
        aa = np.r_[a, np.zeros(len(bb) - len(a))]
        raw = blend_loss_db(aa, bb)
        dly, p = align(aa, bb)
        fixed = p * frac_shift(bb, dly)[: len(bb)]
        fx = blend_loss_db(aa, fixed)
        print("%s\t%.1f\t%+d\t%.2f\t%.2f\t\t" % (name, off, pol, raw[0], raw[1]))
        print("%s + auto-align\t%.1f\t%+d\t%.2f\t%.2f\t%.2f\t%+d" % (name, off, pol, fx[0], fx[1], dly, p))

# ---------------------------------------------------------------- speaker-load filters
RE_OHM, RP_OHM, R2_OHM, LE_H, QMS = 6.5, 40.0, 40.0, 1.0e-3, 8.0   # 8-ohm guitar speaker, see report section 7.3

def load_ra(gain_db, rho):
    """Amp output resistance that gives `gain_db` at the top of the section (rho = R_peak / Re)."""
    g = 10 ** (gain_db / 20)
    if g <= 1.0: return 0.0
    return RE_OHM * (rho / ((1 + rho) / g - 1) - 1)

def resonance_biquad_s(f0, gain_db):
    """Analog peaking section: (s^2 + s w0/Qz + w0^2) / (s^2 + s w0/Qp + w0^2)."""
    rho = RP_OHM / RE_OHM; ra = load_ra(gain_db, rho); w0 = 2 * np.pi * f0
    qz = QMS / (1 + rho); qp = QMS / (1 + RP_OHM / (RE_OHM + ra))
    return w0, qz, qp

def presence_shelf_s(gain_db):
    """Analog first-order shelf: (s/wz + 1) / (s/wp + 1), unity at DC."""
    rho = R2_OHM / RE_OHM; rb = load_ra(gain_db, rho)
    wz = RE_OHM * R2_OHM / (LE_H * (RE_OHM + R2_OHM)); wp = (RE_OHM + rb) * R2_OHM / (LE_H * (RE_OHM + R2_OHM + rb))
    return wz, wp

def load_response(f, f0, res_db, pres_db):
    s = 2j * np.pi * f
    w0, qz, qp = resonance_biquad_s(f0, res_db); wz, wp = presence_shelf_s(pres_db)
    return (s * s + s * w0 / qz + w0 * w0) / (s * s + s * w0 / qp + w0 * w0) * (s / wz + 1) / (s / wp + 1)

def cmd_load():
    fr = np.array([40, 60, 80, 100, 125, 160, 250, 400, 630, 1000, 1600, 2500, 4000, 6300, 10000, 16000], float)
    print("# speaker-load filter (our lumped model: Re 6.5 ohm, Rpeak 40 ohm, Qms 8, Le 1 mH || 40 ohm), analog magnitude in dB")
    print("# python3 cabproto.py load")
    print("setting\t" + "\t".join("%g" % x for x in fr))
    for f0, r, p in ((100, 0, 0), (100, 3, 0), (100, 6, 0), (100, 9, 0), (100, 12, 0), (100, 0, 3), (100, 0, 6), (100, 0, 9), (100, 0, 12), (100, 6, 6), (75, 6, 6), (120, 6, 6)):
        g = 20 * np.log10(np.abs(load_response(fr, f0, r, p)))
        print("f0=%g res=%g pres=%g\t" % (f0, r, p) + "\t".join("%.1f" % v for v in g))
    # discrete version: bilinear transform with each corner prewarped, w' = 2 fs tan(w / (2 fs))
    for fs in (44100.0, 48000.0, 96000.0):
        pw = lambda w: 2 * fs * np.tan(w / (2 * fs))
        w0, qz, qp = resonance_biquad_s(100, 6); wz, wp = presence_shelf_s(6)
        z = np.exp(2j * np.pi * fr / fs); sd = 2 * fs * (z - 1) / (z + 1)
        w0p, wzp, wpp = pw(w0), pw(wz), pw(wp)
        hd = (sd * sd + sd * w0p / qz + w0p * w0p) / (sd * sd + sd * w0p / qp + w0p * w0p) * (sd / wzp + 1) / (sd / wpp + 1)
        err = 20 * np.log10(np.abs(hd)) - 20 * np.log10(np.abs(load_response(fr, 100, 6, 6)))
        print("# bilinear at %g Hz, res=6 pres=6: largest deviation from the analog curve %.2f dB (at %g Hz)" % (fs, np.max(np.abs(err)), fr[np.argmax(np.abs(err))]))
    for r in (3, 6, 9, 12):
        w0, qz, qp = resonance_biquad_s(100, r); wz, wp = presence_shelf_s(r)
        print("# gain %2d dB: Ra = %.1f ohm, bell Qz %.2f Qp %.2f ; shelf zero %.0f Hz pole %.0f Hz" % (r, load_ra(r, RP_OHM / RE_OHM), qz, qp, wz / 2 / np.pi, wp / 2 / np.pi))

# ---------------------------------------------------------------- resampler
def kaiser_sinc_resample(h, src, dst, half=32, beta=9.0):
    """Windowed-sinc resampling of a finite IR (offline). Cutoff at 0.5*min(src,dst)*0.98, `half` zero crossings per side.
    Output scaled by src/dst so the frequency response (not the waveform) is preserved."""
    ratio = dst / src; fc = 0.5 * min(1.0, ratio) * 0.98           # cycles per input sample
    n_out = int(np.ceil(len(h) * ratio)); out = np.zeros(n_out)
    w = half / min(1.0, ratio)                                     # kernel half-width in input samples
    i0 = np.i0(beta)
    for m in range(n_out):
        t = m / ratio; lo = int(np.floor(t - w)) + 1; hi = int(np.floor(t + w))
        k = np.arange(max(lo, 0), min(hi, len(h) - 1) + 1)
        if len(k) == 0: continue
        x = k - t
        win = np.i0(beta * np.sqrt(np.maximum(0, 1 - (x / w) ** 2))) / i0
        out[m] = np.sum(h[k] * 2 * fc * np.sinc(2 * fc * x) * win)
    return out / ratio                                              # response-preserving gain src/dst

def cmd_resample(path, src, dst):
    h = np.load(path)[:4096]; src = float(src); dst = float(dst)
    a = kaiser_sinc_resample(h, src, dst); b = resample_fft(h, src, dst) * (src / dst); n = min(len(a), len(b))
    N = 1 << 16; f = np.fft.rfftfreq(N, 1 / dst); A = np.fft.rfft(a[:n], N); B = np.fft.rfft(b[:n], N)
    print("# Kaiser windowed-sinc (32 zero crossings, beta 9) against FFT band-limited resampling, %g -> %g Hz" % (src, dst))
    print("freq_hz\terror_db")
    for x in (100, 1000, 4000, 8000, 12000, 15000, 18000, 20000):
        k = np.argmin(abs(f - x)); print("%d\t%.3f" % (x, 20 * np.log10(abs(A[k]) / abs(B[k]))))
    print("# time-domain residual %.1f dB" % (10 * np.log10(np.sum((a[:n] - b[:n]) ** 2) / np.sum(b[:n] ** 2))))

if __name__ == "__main__":
    c = sys.argv[1]
    if c == "norm": cmd_norm(sys.argv[2])
    elif c == "align": cmd_align(sys.argv[2])
    elif c == "load": cmd_load()
    elif c == "resample": cmd_resample(*sys.argv[2:5])
