#!/usr/bin/env python3
"""Black-box helper for Solid EQ (aufx 'Ni$8' -NI-).

  python3 m.py <table>      table in: base bands q shelf cramp sym thd interact

Every curve is the FFT ratio of an impulse pair (-12 dBFS, 2 s) rendered with
  $RE_STATE/bin/auhost render aufx 'Ni$8' -NI- imp_<sr>.wav out.wav --set id=value ...
(parameter values are the normalised 0..1 numbers `auhost params` prints).
"""
import os, subprocess, sys
import numpy as np

RE = os.environ["RE"]; RS = os.environ["RE_STATE"]
sys.path.insert(0, RE)
import sig  # noqa: E402

W = os.path.join(RS, "work/eq/solid")
AU = ["aufx", "Ni$8", "-NI-"]
# ids
HPF, LFG, LFF, LFM, LMFG, LMFF, LMFQ, MODE, HMFG, HMFF, HMFQ, _, HFG, HFF, HFM, LPF = range(16)
E, G = 1, 0


def imp(sr):
    p = os.path.join(W, f"imp_{sr}.wav")
    if not os.path.exists(p):
        x = np.zeros(int(sr * 2), dtype=np.float32); x[100] = sig.amp(-12)
        sig.write_wav(p, x, sr)
    return p


def render(inp, out, sets):
    cmd = [os.path.join(RS, "bin/auhost"), "render", *AU, inp, out, "--warmup", "0.5"]
    for k, v in sets.items():
        cmd += ["--set", f"{k}={v}"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise RuntimeError(r.stderr)
    return " ".join(cmd[:1] + ["render", "aufx", "'Ni$8'", "-NI-"] + cmd[5:])


def H(sets, sr=48000):
    inp = imp(sr); out = os.path.join(W, "o.wav")
    render(inp, out, sets)
    x, _ = sig.read_wav(inp); y, _ = sig.read_wav(out)
    x = x[0]; y = y[0]
    n = min(len(x), len(y))
    X = np.fft.rfft(x[:n]); Y = np.fft.rfft(y[:n])
    f = np.fft.rfftfreq(n, 1 / sr)
    return f, 20 * np.log10(np.abs(Y / X) + 1e-30)


def at(f, d, freqs):
    return [float(np.interp(q, f, d)) for q in freqs]


def bell(f, d, lo=20, hi=20000):
    """peak gain, peak freq, Q from the half-gain (dB/2) points, Q from -3 dB-from-peak points."""
    m = (f >= lo) & (f <= hi)
    ff, dd = f[m], d[m]
    i = int(np.argmax(np.abs(dd))); g = dd[i]; f0 = ff[i]

    def width(level):
        s = np.sign(g)
        a = i
        while a > 0 and s * dd[a] > s * level: a -= 1
        b = i
        while b < len(dd) - 1 and s * dd[b] > s * level: b += 1
        return ff[a], ff[b]
    a, b = width(g / 2); qh = f0 / (b - a) if b > a else float("nan")
    a3, b3 = width(g - 3 * np.sign(g)) if abs(g) > 3.5 else (np.nan, np.nan)
    q3 = f0 / (b3 - a3) if abs(g) > 3.5 else float("nan")
    return g, f0, qh, q3


GRID = [20, 30, 50, 80, 100, 150, 200, 300, 500, 800, 1000, 1500, 2000, 3000, 5000, 8000, 10000, 12000, 14000, 16000, 18000, 20000]


def row(label, vals, fmt="{:.2f}"):
    print(label + "\t" + "\t".join(fmt.format(v) for v in vals))


def t_base():
    print("# flat response, all defaults, E and G; dB at grid"); print("mode\t" + "\t".join(map(str, GRID)))
    for name, m in (("E", E), ("G", G)):
        f, d = H({MODE: m}); row(name, at(f, d, GRID), "{:.4f}")


def t_bands():
    print("# each band at min/max gain, freq and Q at default (0.5); bell/shelf modes; E and G")
    print("mode\tband\tgainparam\tpeak_or_plateau_dB\tpeak_f_Hz\tQ_halfgain\tQ_3dB\tdB@20\tdB@20k")
    for name, m in (("E", E), ("G", G)):
        for band, gid, extra in (("LF shelf", LFG, {LFM: 0}), ("LF bell", LFG, {LFM: 1}), ("LMF", LMFG, {}), ("HMF", HMFG, {}),
                                 ("HF shelf", HFG, {HFM: 0}), ("HF bell", HFG, {HFM: 1})):
            for v in (0.0, 0.25, 0.75, 1.0):
                s = {MODE: m, gid: v}; s.update(extra)
                f, d = H(s); g, f0, qh, q3 = bell(f, d)
                lo, hi = at(f, d, [20, 20000])
                print(f"{name}\t{band}\t{v}\t{g:.2f}\t{f0:.0f}\t{qh:.2f}\t{q3:.2f}\t{lo:.2f}\t{hi:.2f}")


def t_q():
    print("# HMF bell: Q (half-gain definition: f0 / width between the points at half the peak dB) against gain and Q knob")
    print("mode\tQknob\tgainparam\tpeak_dB\tpeak_f\tQ_halfgain\tQ_3dB")
    for name, m in (("E", E), ("G", G)):
        for qk in (0.0, 0.5, 1.0):
            for v in (0.55, 0.6, 0.7, 0.8, 0.9, 1.0, 0.4, 0.2, 0.0):
                f, d = H({MODE: m, HMFG: v, HMFQ: qk}); g, f0, qh, q3 = bell(f, d)
                print(f"{name}\t{qk}\t{v}\t{g:.2f}\t{f0:.0f}\t{qh:.2f}\t{q3:.2f}")


def t_shelf():
    print("# LF and HF shelves, E vs G, full curve at gain max and min (dB at grid)")
    print("mode\tband\tgain\t" + "\t".join(map(str, GRID)))
    for name, m in (("E", E), ("G", G)):
        for band, gid, mid in (("LF", LFG, LFM), ("HF", HFG, HFM)):
            for v in (1.0, 0.75, 0.0):
                f, d = H({MODE: m, gid: v, mid: 0}); row(f"{name}\t{band}\t{v}", at(f, d, GRID))


CR = [2000, 5000, 8000, 10000, 12000, 14000, 16000, 18000, 20000]


def t_cramp():
    print("# top-band response at 44.1 / 48 / 96 kHz; dB at freqs. Analog-like behaviour = same curve at every rate.")
    print("case\tsr\t" + "\t".join(map(str, CR)) + "\tnyq-1%")
    cases = (("E HF shelf max, HF freq max", {MODE: E, HFG: 1.0, HFF: 1.0, HFM: 0}),
             ("E HF bell max, HF freq max", {MODE: E, HFG: 1.0, HFF: 1.0, HFM: 1}),
             ("E HF bell max, HF freq 0.75", {MODE: E, HFG: 1.0, HFF: 0.75, HFM: 1}),
             ("E HMF max gain, freq max, Q max", {MODE: E, HMFG: 1.0, HMFF: 1.0, HMFQ: 1.0}),
             ("G HF shelf max, HF freq max", {MODE: G, HFG: 1.0, HFF: 1.0, HFM: 0}),
             ("E LPF mid", {MODE: E, LPF: 0.5}))
    for label, s in cases:
        for sr in (44100, 48000, 96000):
            f, d = H(s, sr); row(f"{label}\t{sr}", at(f, d, CR) + at(f, d, [sr * 0.495]))


def t_sym():
    print("# boost/cut symmetry: max |dB(boost)+dB(cut)| over 20 Hz..20 kHz, gain param v and 1-v")
    print("mode\tband\tv\tresidual_dB")
    f = None
    for name, m in (("E", E), ("G", G)):
        for band, gid, extra in (("LF shelf", LFG, {LFM: 0}), ("LMF", LMFG, {}), ("HMF", HMFG, {}), ("HF shelf", HFG, {HFM: 0}), ("HF bell", HFG, {HFM: 1})):
            for v in (1.0, 0.75):
                a = {MODE: m, gid: v}; a.update(extra); b = {MODE: m, gid: round(1 - v, 3)}; b.update(extra)
                f, da = H(a); _, dbb = H(b); k = (f >= 20) & (f <= 20000)
                print(f"{name}\t{band}\t{v}\t{np.max(np.abs(da[k] + dbb[k])):.3f}")


def t_interact():
    print("# series or parallel: LMF and HMF both at max, same frequency region; compare with dB sum of the single-band curves")
    for name, m in (("E", E), ("G", G)):
        f, a = H({MODE: m, LMFG: 1.0, LMFF: 1.0}); _, b = H({MODE: m, HMFG: 1.0, HMFF: 0.0}); _, c = H({MODE: m, LMFG: 1.0, LMFF: 1.0, HMFG: 1.0, HMFF: 0.0})
        k = (f >= 20) & (f <= 20000)
        print(f"{name}\tpeak LMF {a[k].max():.2f}\tpeak HMF {b[k].max():.2f}\tboth {c[k].max():.2f}\tmax|both-(sum dB)| {np.max(np.abs(c[k] - a[k] - b[k])):.4f}")


def t_thd():
    print("# harmonic distortion: sine through EQ; level of H2,H3 relative to fundamental (dB). 0 dBFS and +12 dBFS input.")
    print("case\tfreq\tin_dBFS\tH2_dBc\tH3_dBc\tTHD_dB")
    sr = 48000
    for label, s in (("flat E", {MODE: E}), ("E LF shelf max + HMF max", {MODE: E, LFG: 1.0, HMFG: 1.0}), ("G LF shelf max", {MODE: G, LFG: 1.0})):
        for fq in (100, 1000):
            for lv in (-18, 0, 12):
                n = sr * 2; t = np.arange(n) / sr
                x = (sig.amp(lv) * np.sin(2 * np.pi * fq * t)).astype(np.float32)
                inp = os.path.join(W, "sine.wav"); out = os.path.join(W, "sine_o.wav")
                sig.write_wav(inp, x, sr); render(inp, out, s)
                y, _ = sig.read_wav(out); y = y[0]
                y = y[sr // 2: sr // 2 + sr].astype(np.float64)
                Y = np.abs(np.fft.rfft(y * np.blackman(len(y))))
                def lvl(k):
                    i = int(round(k * fq)); return Y[i - 3:i + 4].max()
                h1 = lvl(1); hs = [lvl(k) for k in range(2, 10)]
                print(f"{label}\t{fq}\t{lv}\t{20*np.log10(hs[0]/h1+1e-30):.1f}\t{20*np.log10(hs[1]/h1+1e-30):.1f}\t{20*np.log10(np.sqrt(sum(h*h for h in hs))/h1+1e-30):.1f}")


if __name__ == "__main__":
    {"base": t_base, "bands": t_bands, "q": t_q, "shelf": t_shelf, "cramp": t_cramp, "sym": t_sym, "interact": t_interact, "thd": t_thd}[sys.argv[1]]()
