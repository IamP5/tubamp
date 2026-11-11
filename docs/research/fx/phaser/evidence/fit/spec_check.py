#!/usr/bin/env python3
"""spec_check.py: evaluate the equations of ../../voicings.md and print the numbers it quotes.

    python3 spec_check.py > spec_check.txt

Constants are read from prototype/phaser_ref.py (the executable spec; voicings.md lists the same
values). The response equations below are typed from voicings.md, not called from the prototype;
the TPT coefficients of the four stages (and the lag) at a sweep end are the end rows of the
prototype's coeff_table(), the solved discretisation that voicings.md describes:

  break frequency   Phase 90: fc(u) = (1/Rfix + 1/R(u)) / (2 pi C),  R(u) = (RS^m + (R1 e^(-gamma u))^m)^(1/m),
                    u the smoothed LFO in [-1, 1] (quasi-static ends: u = -1 and +1)
                    Small Stone: fc = sqrt(lo hi) (hi/lo)^(v/2), v in [-1, 1]
  one-pole (TPT)    s' = (1 - 1/z) / (1 + 1/z), lowpass g / (g + s'), g_k from the end rows of coeff_table()
  Phase 90 script   H = M (0.5 + 0.5 A1 A2 A3 A4),  A_k = (s' - g_k) / (s' + g_k)
  Phase 90 block    H = M (0.5 + 0.5 A1 S2 A3 A4 / (1 + beta L A3 A4)),  S2 = ((1 + beta) s' - g_2) / (s' + g_2), L the lag lowpass
  Small Stone off   H = M (dry + wet Hp P1 P2 P3 P4),  P_k = (g_k - s') / (g_k + s'),  Hp = 1 - lp_hp (the wet input's highpass)
  Small Stone on    H = M (dry + wet Ha P^4 / (1 - c F P^4)),  Ha = (1 - ka lp_p1)(1 - kb lp_p2),  F = (1 - lp_p1)(1 - kf lp_p2)
  M the voicing's make-up gain; "analog" rows: s' = j f and g = the analog break (no discretisation).

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
    if fs is None: return 1j * np.asarray(f, float)
    z = np.exp(2j * np.pi * f / fs); return (1 - 1 / z) / (1 + 1 / z)


def coeffs(voicing, fc, fs, end):
    """The four stage coefficients and the lag at a sweep end: the end rows of the prototype's
    coefficient table (its solve runs along the sweep from the bottom)."""
    if fs is None: return (fc,) * 4 + (P.P90_FB_LAG_HZ,)
    G = P.coeff_table({"Phase 90 script": "script", "Phase 90 block": "block", "Small Stone Color off": "off",
                       "Small Stone Color on": "on"}[voicing], fs)[0 if end == "lo" else -1]
    return tuple(G / (1.0 - G))


def p90_fc(u):
    r = (P.P90_RS ** P.P90_KNEE + (P.P90_R1 * math.exp(-P.P90_GAMMA * u)) ** P.P90_KNEE) ** (1 / P.P90_KNEE)
    return (1 / P.P90_RFIX + 1 / r) / (2 * math.pi * P.P90_C)


def H_p90(f, g, fs, block):
    sp = sprime(f, fs); A = [(sp - gk) / (sp + gk) for gk in g[:4]]
    if not block: return P.P90_SCRIPT_MAKEUP * (0.5 + 0.5 * A[0] * A[1] * A[2] * A[3])
    b = P.P90_BETA; L = g[4] / (g[4] + sp)
    S2 = ((1 + b) * sp - g[1]) / (sp + g[1])
    return P.P90_BLOCK_MAKEUP * (0.5 + 0.5 * A[0] * S2 * A[2] * A[3] / (1 + b * L * A[2] * A[3]))


def H_ss(f, g, fs, color):
    sp = sprime(f, fs); P4 = 1.0
    for gk in g[:4]: P4 = P4 * (gk - sp) / (gk + sp)
    p = P.SS_ON if color else P.SS_OFF
    def lp(fx): gx = fx if fs is None else math.tan(math.pi * fx / fs); return gx / (gx + sp)
    if not color: return P.SS_OFF_MAKEUP * (p["dry"] + p["wet"] * (1 - lp(p["f_hp"])) * P4)
    ka, kb, kf = 1 - p["f_za"] / p["f_p1"], 1 - p["f_zb"] / p["f_p2"], 1 - p["f_zf"] / p["f_p2"]
    Ha = (1 - ka * lp(p["f_p1"])) * (1 - kb * lp(p["f_p2"])); F = (1 - lp(p["f_p1"])) * (1 - kf * lp(p["f_p2"]))
    return P.SS_ON_MAKEUP * (p["dry"] + p["wet"] * Ha * P4 / (1 - p["c"] * F * P4))


def analyse(Hf, fs):
    top = min(20000.0, 0.49 * fs) if fs is not None else 20000.0
    f = 20.0 * 1.001 ** np.arange(int(math.log(top / 20.0) / math.log(1.001)))
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


def proto_check(name, fc, fs, voicing, flag, end):
    n = 1 << 15; x = np.zeros(n); x[0] = 1.0
    g = np.array(coeffs(name, fc, fs, end)); G = np.tile(g / (1 + g), (n, 1))
    if voicing == "phase90":
        y = P.run_phase90(x, fs, G, flag, 0.5, P.P90_BLOCK_MAKEUP if flag else P.P90_SCRIPT_MAKEUP)
        Hf = lambda f: H_p90(f, g, fs, flag)  # noqa: E731
    else:
        y = P.run_smallstone(x, fs, G, flag, 0.5, P.SS_ON_MAKEUP if flag else P.SS_OFF_MAKEUP)
        Hf = lambda f: H_ss(f, g, fs, flag)  # noqa: E731
    Y = np.fft.rfft(y); f = np.fft.rfftfreq(n, 1 / fs); k = (f >= 20) & (f <= 20000)
    return float(np.max(np.abs(20 * np.log10(np.abs(Y[k])) - 20 * np.log10(np.abs(Hf(f[k]))))))


VOICINGS = [("Phase 90 script", "phase90", False), ("Phase 90 block", "phase90", True),
            ("Small Stone Color off", "smallstone", False), ("Small Stone Color on", "smallstone", True)]


def ends(voicing, flag):
    if voicing == "phase90": return p90_fc(-1.0), p90_fc(1.0)
    p = P.SS_ON if flag else P.SS_OFF; return p["f_lo"], p["f_hi"]


def H_for(name, voicing, flag, fc, fs, end):
    g = coeffs(name, fc, fs, end)
    return (lambda f: H_p90(f, g, fs, flag)) if voicing == "phase90" else (lambda f: H_ss(f, g, fs, flag))


def main():
    print("# spec_check.py: the equations of voicings.md evaluated with the prototype's constants (frozen LFO).")
    print(f"# Phase 90 law: RS {P.P90_RS} ohm, R1 {P.P90_R1} ohm, gamma {P.P90_GAMMA}, m {P.P90_KNEE}, Rfix {P.P90_RFIX:.0f} ohm, "
          f"C {P.P90_C * 1e9:.0f} nF (floor 1/(2 pi Rfix C) = {1 / (2 * math.pi * P.P90_RFIX * P.P90_C):.1f} Hz); beta {P.P90_BETA}, lag {P.P90_FB_LAG_HZ:.0f} Hz")
    print(f"# Make-up gains (linear): script {P.P90_SCRIPT_MAKEUP}, block {P.P90_BLOCK_MAKEUP}, Small Stone off {P.SS_OFF_MAKEUP}, on {P.SS_ON_MAKEUP}")
    print("# 'lo'/'hi' = quasi-static sweep ends (Phase 90: u = -1 / +1; Small Stone: f_lo / f_hi). The sweep at a running rate")
    print("# is narrower (gate smoothing, Phase 90); compare.txt has those ends. nan: no such notch with 3 dB prominence in")
    print("# 20 Hz to min(20 kHz, 0.49 fs) (Small Stone Color on: notch 1 fades at the sweep bottom; Color off: notch 2 leaves the band).")
    print("voicing\tfs_hz\tend\tfc_hz\tnotch1_hz\tnotch1_db\tnotch1/fc\tnotch2_hz\tnotch2_db\tnotch2/fc\tn2/n1\tpeak_db\tpeak_hz\tdc_db")
    def row(name, voicing, flag, lab, fc, fs, tag):
        nt, (pf, pdb), dc = analyse(H_for(name, voicing, flag, fc, fs, lab), fs)
        nan = (float("nan"), float("nan"))
        if nt and nt[0][0] / fc > 1.5: nt = [nan] + nt     # notch 1 below 3 dB prominence (Color on, sweep bottom)
        n1 = nt[0] if nt else nan; n2 = nt[1] if len(nt) > 1 else nan
        def lev(x): return "< -120" if x < -120 else f"{x:.2f}"
        return (f"{name}\t{tag}\t{lab}\t{fc:.1f}\t{n1[0]:.1f}\t{lev(n1[1])}\t{n1[0] / fc:.4f}\t{n2[0]:.0f}\t{lev(n2[1])}\t"
                f"{n2[0] / fc:.3f}\t{n2[0] / n1[0]:.3f}\t{pdb:+.2f}\t{pf:.0f}\t{dc:+.2f}")
    for fs in (48000.0, 44100.0, 88200.0, 96000.0, 192000.0):
        for name, voicing, flag in VOICINGS:
            for lab, fc in zip(("lo", "hi"), ends(voicing, flag)):
                print(row(name, voicing, flag, lab, fc, fs, f"{fs:.0f}"))
    print("# The same equations in the analog domain (s' = j f, no discretisation), for reference:")
    for name, voicing, flag in VOICINGS:
        for lab, fc in zip(("lo", "hi"), ends(voicing, flag)):
            print("#   " + row(name, voicing, flag, lab, fc, None, "analog"))
    # analog notch ratios of the block loop (s-domain, no lag)
    f = np.exp(np.linspace(math.log(0.05), math.log(20), 400001)); s = 1j * f; b = P.P90_BETA
    A = (s - 1) / (s + 1); S2 = ((1 + b) * s - 1) / (s + 1); h = np.abs(0.5 + 0.5 * A * S2 * A * A / (1 + b * A * A))
    mins = [f[i] for i in range(1, len(f) - 1) if h[i] < h[i - 1] and h[i] < h[i + 1]]
    print(f"# Phase 90 block, analog (no lag, s-domain, no make-up): notch 1 at {mins[0]:.4f} fc, notch 2 at {mins[1]:.3f} fc, depth "
          f"{20 * math.log10(h[np.argmin(np.abs(f - mins[0]))]):.2f} dB, peak {20 * math.log10(h.max()):+.2f} dB")
    print("# Equations against the prototype's impulse response at the same constant fc, 48 kHz (largest |dB difference|, 20 Hz-20 kHz):")
    for name, voicing, flag in VOICINGS:
        lo, hi = ends(voicing, flag)
        print(f"#   {name}: {max(proto_check(name, lo, 48000.0, voicing, flag, "lo"), proto_check(name, hi, 48000.0, voicing, flag, "hi")):.1e} dB")


if __name__ == "__main__":
    main()
