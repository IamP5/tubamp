#!/usr/bin/env python3
"""spec_check.py: evaluate the equations of ../../voicings.md and print the numbers it quotes.

    python3 spec_check.py > spec_check.txt

Constants are read from prototype/phaser_ref.py (the executable spec; voicings.md lists the same
values). The equations below are typed from voicings.md, not called from the prototype:

  break frequency   Phase 90: fc(u) = (1/Rfix + 1/R(u)) / (2 pi C),  R(u) = (RS^m + (R1 e^(-gamma u))^m)^(1/m),
                    u the smoothed LFO in [-1, 1] (quasi-static ends: u = -1 and +1)
                    Small Stone: fc = sqrt(lo hi) (hi/lo)^(v/2), v in [-1, 1]
  one-pole (TPT)    s' = (1 - 1/z) / (1 + 1/z), g = tan(pi f / fs), lowpass g / (g + s')
  Phase 90 script   H = 0.5 + 0.5 A^4,  A = (s' - g) / (s' + g)
  Phase 90 block    H = 0.5 + 0.5 A S2 A^2 / (1 + beta L A^2),  S2 = ((1 + beta) s' - g) / (s' + g), L the lag lowpass
  Small Stone off   H = dry + wet P^4,  P = (g - s') / (g + s')
  Small Stone on    H = dry + wet Ha P^4 / (1 - c F P^4),  Ha = (1 - ka lp_p1)(1 - kb lp_p2),  F = (1 - lp_p1)(1 - kf lp_p2)

Per voicing and sweep end: break frequency, notch 1 and notch 2 (local minima of |H| with at least
3 dB prominence, 20 Hz to min(20 kHz, 0.49 fs), refined by golden section), notch depth, highest
|H| (resonance) and its frequency, DC gain. Last, each frozen response is checked against the
prototype's own impulse response at the same constant fc (largest dB difference, 20 Hz to 20 kHz).
"""
import importlib.util, math, os
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
PROTO = os.path.join(HERE, "..", "..", "prototype", "phaser_ref.py")
spec = importlib.util.spec_from_file_location("phaser_ref_spec", PROTO)
P = importlib.util.module_from_spec(spec); spec.loader.exec_module(P)


def sprime(f, fs):
    z = np.exp(2j * np.pi * f / fs); return (1 - 1 / z) / (1 + 1 / z)


def g_of(f, fs): return math.tan(math.pi * min(f, 0.49 * fs) / fs)


def p90_fc(u):
    r = (P.P90_RS ** P.P90_KNEE + (P.P90_R1 * math.exp(-P.P90_GAMMA * u)) ** P.P90_KNEE) ** (1 / P.P90_KNEE)
    return (1 / P.P90_RFIX + 1 / r) / (2 * math.pi * P.P90_C)


def H_p90(f, fc, fs, block):
    sp = sprime(f, fs); g = g_of(fc, fs); A = (sp - g) / (sp + g)
    if not block: return 0.5 + 0.5 * A ** 4
    b = P.P90_BETA; gl = g_of(P.P90_FB_LAG_HZ, fs); L = gl / (gl + sp)
    S2 = ((1 + b) * sp - g) / (sp + g)
    return 0.5 + 0.5 * A * S2 * A * A / (1 + b * L * A * A)


def H_ss(f, fc, fs, color):
    sp = sprime(f, fs); g = g_of(fc, fs); P4 = ((g - sp) / (g + sp)) ** 4
    p = P.SS_ON if color else P.SS_OFF
    if not color: return p["dry"] + p["wet"] * P4
    def lp(fx): gx = g_of(fx, fs); return gx / (gx + sp)
    ka, kb, kf = 1 - p["f_za"] / p["f_p1"], 1 - p["f_zb"] / p["f_p2"], 1 - p["f_zf"] / p["f_p2"]
    Ha = (1 - ka * lp(p["f_p1"])) * (1 - kb * lp(p["f_p2"])); F = (1 - lp(p["f_p1"])) * (1 - kf * lp(p["f_p2"]))
    return p["dry"] + p["wet"] * Ha * P4 / (1 - p["c"] * F * P4)


def analyse(Hf, fs):
    f = 20.0 * 1.001 ** np.arange(int(math.log(min(20000.0, 0.49 * fs) / 20.0) / math.log(1.001)))
    db = 20 * np.log10(np.abs(Hf(f)))
    notches = []
    for i in range(1, len(f) - 1):
        if not (db[i] <= db[i - 1] and db[i] < db[i + 1]): continue
        j = i - 1
        while j > 0 and db[j - 1] >= db[j]: j -= 1
        k = i + 1
        while k < len(f) - 1 and db[k + 1] >= db[k]: k += 1
        if min(db[j], db[k]) - db[i] < 3.0: continue
        a, b = math.log(f[i - 1]), math.log(f[i + 1]); gr = (math.sqrt(5) - 1) / 2
        def m(x): return abs(Hf(np.array([math.exp(x)]))[0])
        c, d = b - gr * (b - a), a + gr * (b - a)
        for _ in range(60):
            if m(c) < m(d): b, d = d, c; c = b - gr * (b - a)
            else: a, c = c, d; d = a + gr * (b - a)
        x = 0.5 * (a + b); notches.append((math.exp(x), 20 * math.log10(m(x))))
    k = int(np.argmax(db))
    return notches, (f[k], db[k]), float(20 * np.log10(abs(Hf(np.array([1.0]))[0])))


def proto_check(fc, fs, voicing, flag):
    n = 1 << 15; x = np.zeros(n); x[0] = 1.0; fcv = np.full(n, fc)
    if voicing == "phase90": y = P.run_phase90(x, fs, fcv, flag, 0.5); Hf = lambda f: H_p90(f, fc, fs, flag)
    else: y = P.run_smallstone(x, fs, fcv, flag, 0.5); Hf = lambda f: H_ss(f, fc, fs, flag)
    Y = np.fft.rfft(y); f = np.fft.rfftfreq(n, 1 / fs); k = (f >= 20) & (f <= 20000)
    return float(np.max(np.abs(20 * np.log10(np.abs(Y[k])) - 20 * np.log10(np.abs(Hf(f[k]))))))


VOICINGS = [("Phase 90 script", "phase90", False), ("Phase 90 block", "phase90", True),
            ("Small Stone Color off", "smallstone", False), ("Small Stone Color on", "smallstone", True)]


def ends(voicing, flag):
    if voicing == "phase90": return p90_fc(-1.0), p90_fc(1.0)
    p = P.SS_ON if flag else P.SS_OFF; return p["f_lo"], p["f_hi"]


def H_for(voicing, flag, fc, fs):
    return (lambda f: H_p90(f, fc, fs, flag)) if voicing == "phase90" else (lambda f: H_ss(f, fc, fs, flag))


def main():
    print("# spec_check.py: the equations of voicings.md evaluated with the prototype's constants (frozen LFO).")
    print(f"# Phase 90 law: RS {P.P90_RS} ohm, R1 {P.P90_R1} ohm, gamma {P.P90_GAMMA}, m {P.P90_KNEE}, Rfix {P.P90_RFIX:.0f} ohm, "
          f"C {P.P90_C * 1e9:.0f} nF (floor 1/(2 pi Rfix C) = {1 / (2 * math.pi * P.P90_RFIX * P.P90_C):.1f} Hz); beta {P.P90_BETA}, lag {P.P90_FB_LAG_HZ:.0f} Hz")
    print("# 'lo'/'hi' = quasi-static sweep ends (Phase 90: u = -1 / +1; Small Stone: f_lo / f_hi). The sweep at a running rate")
    print("# is narrower (gate smoothing, Phase 90); compare.txt has those ends. nan: no such notch with 3 dB prominence in")
    print("# 20 Hz to min(20 kHz, 0.49 fs) (Small Stone Color on: notch 1 fades at the sweep bottom; Color off: notch 2 leaves the band).")
    print("voicing\tfs_hz\tend\tfc_hz\tnotch1_hz\tnotch1_db\tnotch1/fc\tnotch2_hz\tnotch2_db\tnotch2/fc\tn2/n1\tpeak_db\tpeak_hz\tdc_db")
    for fs in (48000.0, 44100.0, 96000.0):
        for name, voicing, flag in VOICINGS:
            for lab, fc in zip(("lo", "hi"), ends(voicing, flag)):
                nt, (pf, pdb), dc = analyse(H_for(voicing, flag, fc, fs), fs)
                nan = (float("nan"), float("nan"))
                if nt and nt[0][0] / fc > 1.5: nt = [nan] + nt     # notch 1 below 3 dB prominence (Color on, sweep bottom)
                n1 = nt[0] if nt else nan; n2 = nt[1] if len(nt) > 1 else nan
                def lev(x): return "< -120" if x < -120 else f"{x:.2f}"
                print(f"{name}\t{fs:.0f}\t{lab}\t{fc:.1f}\t{n1[0]:.1f}\t{lev(n1[1])}\t{n1[0] / fc:.4f}\t{n2[0]:.0f}\t{lev(n2[1])}\t"
                      f"{n2[0] / fc:.3f}\t{n2[0] / n1[0]:.3f}\t{pdb:+.2f}\t{pf:.0f}\t{dc:+.2f}")
    # analog notch ratios of the block loop (s-domain, no lag)
    f = np.exp(np.linspace(math.log(0.05), math.log(20), 400001)); s = 1j * f; b = P.P90_BETA
    A = (s - 1) / (s + 1); S2 = ((1 + b) * s - 1) / (s + 1); h = np.abs(0.5 + 0.5 * A * S2 * A * A / (1 + b * A * A))
    mins = [f[i] for i in range(1, len(f) - 1) if h[i] < h[i - 1] and h[i] < h[i + 1]]
    print(f"# Phase 90 block, analog (no lag, s-domain): notch 1 at {mins[0]:.4f} fc, notch 2 at {mins[1]:.3f} fc, depth "
          f"{20 * math.log10(h[np.argmin(np.abs(f - mins[0]))]):.2f} dB, peak {20 * math.log10(h.max()):+.2f} dB")
    print("# Equations against the prototype's impulse response at the same constant fc, 48 kHz (largest |dB difference|, 20 Hz-20 kHz):")
    for name, voicing, flag in VOICINGS:
        lo, hi = ends(voicing, flag)
        print(f"#   {name}: {max(proto_check(lo, 48000.0, voicing, flag), proto_check(hi, 48000.0, voicing, flag)):.1e} dB")


if __name__ == "__main__":
    main()
