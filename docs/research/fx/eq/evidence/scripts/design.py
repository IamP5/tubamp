#!/usr/bin/env python3
"""Design checks for the proposed tubamp EQ types (numpy only). Our own numerics; nothing here comes from a vendor binary.

  python3 design.py svf        TPT SVF bell/shelf mix coefficients against the RBJ biquad (same response?)
  python3 design.py orfsvf     the decramped bell expressed as SVF parameters; robustness over the parameter range
  python3 design.py decramp    RBJ bell against Orfanidis prescribed-Nyquist-gain bell against the analog prototype
  python3 design.py stack      passive FMV tone stack: Yeh closed form against a nodal solve; voicing numbers; bilinear error
  python3 design.py graphic    octave graphic EQ: ripple and peak against Q, all bands up and one band up
  python3 design.py tilt       first-order tilt: response numbers
"""
import sys
import numpy as np

PI = np.pi


def H_biquad(b, a, f, fs):
    z = np.exp(-2j * PI * f / fs)
    return (b[0] + b[1] * z + b[2] * z * z) / (a[0] + a[1] * z + a[2] * z * z)


def dB(h):
    return 20 * np.log10(np.abs(h) + 1e-300)


# ---------- RBJ cookbook (reference) ----------
def rbj_peak(fs, f0, Q, gdb):
    A = 10 ** (gdb / 40); w = 2 * PI * f0 / fs; al = np.sin(w) / (2 * Q); c = np.cos(w)
    return [1 + al * A, -2 * c, 1 - al * A], [1 + al / A, -2 * c, 1 - al / A]


def rbj_lowshelf(fs, f0, Q, gdb):
    A = 10 ** (gdb / 40); w = 2 * PI * f0 / fs; c = np.cos(w); be = np.sin(w) * np.sqrt(A) / Q
    return ([A * ((A + 1) - (A - 1) * c + be), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - be)],
            [(A + 1) + (A - 1) * c + be, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - be])


def rbj_highshelf(fs, f0, Q, gdb):
    A = 10 ** (gdb / 40); w = 2 * PI * f0 / fs; c = np.cos(w); be = np.sin(w) * np.sqrt(A) / Q
    return ([A * ((A + 1) + (A - 1) * c + be), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - be)],
            [(A + 1) - (A - 1) * c + be, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - be])


def ana_peak(f, f0, Q, gdb):
    A = 10 ** (gdb / 40); s = 1j * f / f0
    return (s * s + s * A / Q + 1) / (s * s + s / (A * Q) + 1)


# ---------- TPT SVF (Zavalishin / Simper form) ----------
def svf_coeffs(kind, fs, f0, Q, gdb):
    A = 10 ** (gdb / 40); g = np.tan(PI * f0 / fs); k = 1 / Q
    if kind == "bell":
        k = 1 / (Q * A); m = (1.0, k * (A * A - 1), 0.0)
    elif kind == "lowshelf":
        g = g / np.sqrt(A); m = (1.0, k * (A - 1), A * A - 1)
    elif kind == "highshelf":
        g = g * np.sqrt(A); m = (A * A, k * (1 - A) * A, 1 - A * A)
    return g, k, m


def svf_response(kind, fs, f0, Q, gdb, f):
    """Exact frequency response of the trapezoidal SVF: band-pass and low-pass of the bilinear-mapped analog SVF."""
    g, k, m = svf_coeffs(kind, fs, f0, Q, gdb)
    s = 1j * np.tan(PI * f / fs) / g          # bilinear: s/wc
    den = s * s + k * s + 1
    return m[0] + m[1] * (s / den) + m[2] * (1 / den)


def svf_run(kind, fs, f0, Q, gdb, x):
    g, k, m = svf_coeffs(kind, fs, f0, Q, gdb)
    a1 = 1 / (1 + g * (g + k)); a2 = g * a1; a3 = g * a2; ic1 = ic2 = 0.0; y = np.zeros(len(x))
    for i, v0 in enumerate(x):
        v3 = v0 - ic2; v1 = a1 * ic1 + a2 * v3; v2 = ic2 + a2 * ic1 + a3 * v3
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2
        y[i] = m[0] * v0 + m[1] * v1 + m[2] * v2
    return y


def t_svf():
    print("# TPT SVF with the bell / low-shelf / high-shelf mix against the RBJ biquad of the same f0, Q, gain.")
    print("# max |dB difference| over 20 Hz..20 kHz, fs 48 kHz: closed-form SVF response, and SVF impulse response run in the time domain.")
    print("kind\tf0\tQ\tgain_dB\tmax_diff_closed_form_dB\tmax_diff_time_domain_dB")
    fs = 48000; f = np.geomspace(20, 20000, 400)
    for kind, rb in (("bell", rbj_peak), ("lowshelf", rbj_lowshelf), ("highshelf", rbj_highshelf)):
        for f0, Q, g in ((150, 0.707, 20), (425, 1.5, -15), (1800, 0.707, 10), (6000, 2.0, 12), (425, 0.7, 0)):
            b, a = rb(fs, f0, Q, g); ref = dB(H_biquad(b, a, f, fs))
            d1 = np.max(np.abs(dB(svf_response(kind, fs, f0, Q, g, f)) - ref))
            x = np.zeros(1 << 16); x[0] = 1; Y = np.fft.rfft(svf_run(kind, fs, f0, Q, g, x)); ff = np.fft.rfftfreq(len(x), 1 / fs)
            d2 = np.max(np.abs(np.interp(f, ff, dB(Y)) - ref))
            print(f"{kind}\t{f0}\t{Q}\t{g}\t{d1:.5f}\t{d2:.5f}")
    print("# flat: with gain 0 dB the mix is (1, 0, 0): the output is the input sample itself, bit for bit.")


# ---------- Orfanidis prescribed-Nyquist-gain peaking EQ ----------
def orfanidis_peak(fs, f0, Q, gdb, G1=None):
    G0 = 1.0; G = 10 ** (gdb / 20); GB = 10 ** (gdb / 40); w0 = 2 * PI * f0 / fs; Dw = w0 / Q
    F = abs(G * G - GB * GB); G00 = abs(G * G - G0 * G0); F00 = abs(GB * GB - G0 * G0)
    if G1 is None:   # gain of the analog prototype at the Nyquist frequency
        num = G0 * G0 * (w0 * w0 - PI * PI) ** 2 + G * G * F00 * PI * PI * Dw * Dw / F
        den = (w0 * w0 - PI * PI) ** 2 + F00 * PI * PI * Dw * Dw / F
        G1 = np.sqrt(num / den)
    G01 = abs(G * G - G0 * G1); G11 = abs(G * G - G1 * G1); F01 = abs(GB * GB - G0 * G1); F11 = abs(GB * GB - G1 * G1)
    W2 = np.sqrt(G11 / G00) * np.tan(w0 / 2) ** 2
    DW = (1 + np.sqrt(F00 / F11) * W2) * np.tan(Dw / 2)
    C = F11 * DW * DW - 2 * W2 * (F01 - np.sqrt(F00 * F11)); D = 2 * W2 * (G01 - np.sqrt(G00 * G11))
    A = np.sqrt((C + D) / F); B = np.sqrt((G * G * C + GB * GB * D) / F)
    return [(G1 + G0 * W2 + B), -2 * (G1 - G0 * W2), (G1 + G0 * W2 - B)], [(1 + W2 + A), -2 * (1 - W2), (1 + W2 - A)]


def t_decramp():
    print("# bell, +12 dB and -12 dB: digital minus analog prototype (dB). RBJ = bilinear cookbook; ORF = Orfanidis with G1 = analog gain at Nyquist.")
    grid = [2000, 5000, 8000, 10000, 12000, 15000, 18000, 20000]
    print("design\tfs\tf0\tQ\tgain\t" + "\t".join(map(str, grid)) + "\tmax_abs_20-20k\tgain_at_Nyquist_dB")
    f = np.geomspace(20, 20000, 600)
    for fs in (44100, 48000, 96000):
        for f0, Q in ((3200, 1.4), (6400, 1.4), (8000, 1.0), (10000, 0.7), (12000, 2.0)):
            for g in (12, -12):
                for name, fn in (("RBJ", rbj_peak), ("ORF", orfanidis_peak)):
                    b, a = fn(fs, f0, Q, g)
                    e = dB(H_biquad(b, a, f, fs)) - dB(ana_peak(f, f0, Q, g))
                    row = [np.interp(q, f, e) for q in grid]
                    ny = dB(H_biquad(b, a, np.array([fs / 2]), fs))[0]
                    print(f"{name}\t{fs}\t{f0}\t{Q}\t{g}\t" + "\t".join(f"{v:.2f}" for v in row) + f"\t{np.max(np.abs(e)):.2f}\t{ny:.2f}")


def decramped_bell_svf(fs, f0, Q, gdb):
    """Orfanidis bell expressed as TPT SVF parameters (g, k, m0, m1, m2). In the bilinear s-plane the design is
    H(s) = (G1 s^2 + B s + G0 W2)/(s^2 + A s + W2), so g = sqrt(W2), k = A/g, m0 = G1, m1 = (B - G1 A)/g, m2 = G0 - G1."""
    if abs(gdb) < 1e-6: return np.tan(PI * f0 / fs), 1 / Q, (1.0, 0.0, 0.0)
    G0 = 1.0; G = 10 ** (gdb / 20); GB = 10 ** (gdb / 40); w0 = 2 * PI * f0 / fs; Dw = w0 / Q
    F = abs(G * G - GB * GB); G00 = abs(G * G - 1); F00 = abs(GB * GB - 1)
    G1 = np.sqrt(((w0 * w0 - PI * PI) ** 2 + G * G * F00 * PI * PI * Dw * Dw / F) / ((w0 * w0 - PI * PI) ** 2 + F00 * PI * PI * Dw * Dw / F))
    G01 = abs(G * G - G1); G11 = abs(G * G - G1 * G1); F01 = abs(GB * GB - G1); F11 = abs(GB * GB - G1 * G1)
    W2 = np.sqrt(G11 / G00) * np.tan(w0 / 2) ** 2
    DW = (1 + np.sqrt(F00 / F11) * W2) * np.tan(min(Dw, 0.98 * PI) / 2)
    C = F11 * DW * DW - 2 * W2 * (F01 - np.sqrt(F00 * F11)); D = 2 * W2 * (G01 - np.sqrt(G00 * G11))
    A = np.sqrt((C + D) / F); B = np.sqrt((G * G * C + GB * GB * D) / F)
    g = np.sqrt(W2)
    return g, A / g, (G1, (B - G1 * A) / g, G0 - G1)


def t_orfsvf():
    print("# 1. the Orfanidis bell run as a TPT SVF with (g, k, m0, m1, m2) = (W, A/W, G1, (B - G1 A)/W, G0 - G1): max |dB difference| to the Orfanidis biquad")
    print("fs\tf0\tQ\tgain\tmax_diff_dB")
    f = np.geomspace(20, 20000, 500)
    for fs, f0, Q, g in ((44100, 6400, 1.41, 12), (44100, 8000, 1.0, -12), (48000, 3200, 0.7, 15), (96000, 8000, 4.0, 6)):
        gg, k, m = decramped_bell_svf(fs, f0, Q, g); s = 1j * np.tan(PI * f / fs) / gg; den = s * s + k * s + 1
        hs = m[0] + m[1] * s / den + m[2] / den
        b, a = orfanidis_peak(fs, f0, Q, g)
        print(f"{fs}\t{f0}\t{Q}\t{g}\t{np.max(np.abs(dB(hs) - dB(H_biquad(b, a, f, fs)))):.6f}")
    print("# 2. worst error against the analog bell, 20 Hz..0.45 fs, over f0 100 Hz..8 kHz (33 log steps) and gain -15..+15 dB (6 values), per Q.")
    print("#    invalid = designs that are not finite or not stable. ORF = decramped bell above, RBJ = bilinear cookbook bell.")
    print("fs\tQ\tdesigns\tinvalid\tORF_worst_dB\tRBJ_worst_dB\tORF_worst_f0<=4k_dB\tRBJ_worst_f0<=4k_dB")
    for fs in (44100, 48000, 96000):
        ff = np.geomspace(20, 0.45 * fs if fs < 50000 else 20000, 300)
        for Q in (0.3, 0.5, 0.7, 1.0, 1.41, 2.0, 4.0, 8.0):
            n = bad = 0; wo = wr = wo4 = wr4 = 0.0
            for f0 in np.geomspace(100, 8000, 33):
                for g in (-15, -9, -3, 3, 9, 15):
                    n += 1; ana = dB(ana_peak(ff, f0, Q, g))
                    with np.errstate(all="ignore"):
                        gg, k, m = decramped_bell_svf(fs, f0, Q, g)
                        s = 1j * np.tan(PI * ff / fs) / gg; den = s * s + k * s + 1
                        e = np.max(np.abs(dB(m[0] + m[1] * s / den + m[2] / den) - ana))
                    if not np.all(np.isfinite([gg, k, *m, e])) or gg <= 0 or k <= 0:
                        bad += 1; continue
                    b, a = rbj_peak(fs, f0, Q, g); r = np.max(np.abs(dB(H_biquad(b, a, ff, fs)) - ana))
                    wo = max(wo, e); wr = max(wr, r)
                    if f0 <= 4000: wo4 = max(wo4, e); wr4 = max(wr4, r)
            print(f"{fs}\t{Q}\t{n}\t{bad}\t{wo:.2f}\t{wr:.2f}\t{wo4:.2f}\t{wr4:.2f}")


# ---------- FMV (Fender/Marshall/Vox) passive tone stack ----------
def yeh_coeffs(t, m, l, R1, R2, R3, R4, C1, C2, C3):
    """Analog H(s) = (b1 s + b2 s^2 + b3 s^3)/(a0 + a1 s + a2 s^2 + a3 s^3), Yeh & Smith DAFx-06."""
    b1 = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3)
    b2 = (t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
          + m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
          + l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4) + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3)
          + (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4))
    b3 = (l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) - m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4)
          + m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4)
          + t * C1 * C2 * C3 * R1 * R3 * R4 - t * m * C1 * C2 * C3 * R1 * R3 * R4 + t * l * C1 * C2 * C3 * R1 * R2 * R4)
    a0 = 1.0
    a1 = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2)
    a2 = (m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3)
          - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) + l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4)
          + (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 + C1 * C3 * R3 * R4 + C2 * C3 * R3 * R4))
    a3 = (l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) - m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4)
          + m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3 - C1 * C2 * C3 * R1 * R3 * R4)
          + l * C1 * C2 * C3 * R1 * R2 * R4 + C1 * C2 * C3 * R1 * R3 * R4)
    return (0.0, b1, b2, b3), (a0, a1, a2, a3)


def yeh_H(f, *args):
    b, a = yeh_coeffs(*args); s = 2j * PI * f
    return (b[1] * s + b[2] * s ** 2 + b[3] * s ** 3) / (a[0] + a[1] * s + a[2] * s ** 2 + a[3] * s ** 3)


def nodal_H(f, t, m, l, R1, R2, R3, R4, C1, C2, C3):
    """Independent check: nodal analysis of the FMV network, ideal source, unloaded output.
    Nodes: A = C1/treble-pot top, O = treble wiper (output), B = treble-pot bottom / C2 / bass-pot top,
           C = slope resistor R4 / C2 / C3, D = bass-pot bottom / mid-pot top, E = mid wiper / C3."""
    eps = 1e-9
    out = np.zeros(len(f), complex)
    for i, fr in enumerate(f):
        s = 2j * PI * fr
        N = 6; Y = np.zeros((N, N), complex); I = np.zeros(N, complex); A, O, B, C, D, E = range(6)

        def adm(n1, n2, y):   # n = -1: the source (1 V); n = -2: ground
            for p, q in ((n1, n2), (n2, n1)):
                if p >= 0:
                    Y[p, p] += y
                    if q >= 0: Y[p, q] -= y
                    elif q == -1: I[p] += y
        adm(-1, A, s * C1)
        adm(A, O, 1 / max((1 - t) * R1, eps)); adm(O, B, 1 / max(t * R1, eps))
        adm(-1, C, 1 / R4)
        adm(C, B, s * C2); adm(C, E, s * C3)
        adm(B, D, 1 / max(l * R2, eps))
        adm(D, E, 1 / max((1 - m) * R3, eps)); adm(E, -2, 1 / max(m * R3, eps))
        out[i] = np.linalg.solve(Y, I)[O]
    return out


VOICINGS = {   # R1 treble, R2 bass, R3 mid, R4 slope, C1 treble cap, C2 bass cap, C3 mid cap  (published schematic values)
    "Bassman 5F6-A": (250e3, 1e6, 25e3, 56e3, 250e-12, 20e-9, 20e-9),
    "Blackface AB763": (250e3, 250e3, 10e3, 100e3, 250e-12, 100e-9, 47e-9),
    "Marshall 1959/2203": (220e3, 1e6, 22e3, 33e3, 470e-12, 22e-9, 22e-9),
    "Vox AC30 TB": (1e6, 1e6, 10e3, 100e3, 50e-12, 22e-9, 22e-9),
}


def bilinear3(b, a, fs):
    c = 2 * fs
    B = [-b[1] * c - b[2] * c ** 2 - b[3] * c ** 3, -b[1] * c + b[2] * c ** 2 + 3 * b[3] * c ** 3, b[1] * c + b[2] * c ** 2 - 3 * b[3] * c ** 3, b[1] * c - b[2] * c ** 2 + b[3] * c ** 3]
    A = [-a[0] - a[1] * c - a[2] * c ** 2 - a[3] * c ** 3, -3 * a[0] - a[1] * c + a[2] * c ** 2 + 3 * a[3] * c ** 3,
         -3 * a[0] + a[1] * c + a[2] * c ** 2 - 3 * a[3] * c ** 3, -a[0] + a[1] * c - a[2] * c ** 2 + a[3] * c ** 3]
    return B, A


def taper(x, mid=0.3):
    """Audio (log) pot law through (0,0), (0.5, mid), (1,1): x = knob 0..1 -> fraction of track resistance."""
    if abs(mid - 0.5) < 1e-9: return x
    bb = (1 / mid - 1) ** 2
    return (bb ** x - 1) / (bb - 1)


def t_stack():
    f = np.geomspace(20, 20000, 500)
    print("# 1. Yeh closed form against nodal solve, Bassman values: max |dB difference| 20 Hz..20 kHz")
    print("t\tm\tl\tmax_diff_dB")
    v = VOICINGS["Bassman 5F6-A"]
    for t, m, l in ((0.5, 0.5, 0.5), (0.0, 0.0, 1.0), (1.0, 1.0, 0.0), (0.3, 0.8, 0.1), (0.99, 0.01, 0.99), (0.01, 0.99, 0.01)):
        d = np.max(np.abs(dB(yeh_H(f, t, m, l, *v)) - dB(nodal_H(f, t, m, l, *v))))
        print(f"{t}\t{m}\t{l}\t{d:.6f}")
    print("# 2. voicings, knobs at noon (treble linear, bass and mid through a 30 % audio taper): level at the response maximum (= insertion loss),")
    print("#    dip frequency and depth below the lower of the two shoulders, and the range of each knob (dB swing at 80 Hz / at the dip / at 5 kHz)")
    print("voicing\tmax_dB\tat_Hz\tdip_Hz\tdip_dB\tdip_depth_dB\tbass_range_dB@80\tmid_range_dB@dip\ttreble_range_dB@5k")
    for name, v in VOICINGS.items():
        h = dB(yeh_H(f, 0.5, taper(0.5), taper(0.5), *v))
        imax = int(np.argmax(h)); k = (f > 150) & (f < 2500); idip = np.where(k)[0][int(np.argmin(h[k]))]
        shoulder = min(h[:idip].max(), h[idip:].max())
        g = lambda tt, mm, ll, fq: float(np.interp(fq, f, dB(yeh_H(f, tt, taper(mm), taper(ll), *v))))
        br = g(.5, .5, 1, 80) - g(.5, .5, 0, 80); mr = g(.5, 1, .5, f[idip]) - g(.5, 0, .5, f[idip]); tr = g(1, .5, .5, 5000) - g(0, .5, .5, 5000)
        print(f"{name}\t{h[imax]:.1f}\t{f[imax]:.0f}\t{f[idip]:.0f}\t{h[idip]:.1f}\t{shoulder - h[idip]:.1f}\t{br:.1f}\t{mr:.1f}\t{tr:.1f}")
    print("# 3. bilinear third-order digital filter against the analog response: max |dB error| 20 Hz..20 kHz over a 5x5x5 knob grid")
    print("voicing\tfs\tmax_err_dB\tworst_at_Hz\tworst_knobs_t_m_l")
    for name, v in VOICINGS.items():
        for fs in (44100, 48000, 96000):
            worst = (0, 0, None)
            for t in np.linspace(0, 1, 5):
                for m in np.linspace(0, 1, 5):
                    for l in np.linspace(0, 1, 5):
                        b, a = yeh_coeffs(t, m, l, *v); B, A = bilinear3(b, a, fs); z = np.exp(-2j * PI * f / fs)
                        hd = (B[0] + B[1] * z + B[2] * z ** 2 + B[3] * z ** 3) / (A[0] + A[1] * z + A[2] * z ** 2 + A[3] * z ** 3)
                        e = np.abs(dB(hd) - dB(yeh_H(f, t, m, l, *v))); j = int(np.argmax(e))
                        if e[j] > worst[0]: worst = (e[j], f[j], (t, m, l))
            print(f"{name}\t{fs}\t{worst[0]:.2f}\t{worst[1]:.0f}\t{worst[2]}")
    print("# 3b. same, restricted to 20 Hz..10 kHz, and the makeup gain that brings the noon response to 0 dB mean over 80 Hz..5 kHz (log frequency)")
    print("voicing\tfs\tmax_err_dB_to_10k\tnoon_mean_dB_80-5k\tnoon_at_1k_dB")
    f10 = f[f <= 10000]
    for name, v in VOICINGS.items():
        hn = dB(yeh_H(f, 0.5, taper(0.5), taper(0.5), *v)); kk = (f >= 80) & (f <= 5000)
        for fs in (44100, 96000):
            worst = 0
            for t in np.linspace(0, 1, 5):
                for m in np.linspace(0, 1, 5):
                    for l in np.linspace(0, 1, 5):
                        b, a = yeh_coeffs(t, m, l, *v); B, A = bilinear3(b, a, fs); z = np.exp(-2j * PI * f10 / fs)
                        hd = (B[0] + B[1] * z + B[2] * z ** 2 + B[3] * z ** 3) / (A[0] + A[1] * z + A[2] * z ** 2 + A[3] * z ** 3)
                        worst = max(worst, np.max(np.abs(dB(hd) - dB(yeh_H(f10, t, m, l, *v)))))
            print(f"{name}\t{fs}\t{worst:.2f}\t{np.mean(hn[kk]):.1f}\t{np.interp(1000, f, hn):.1f}")
    print("# 4. interaction: Marshall voicing, treble knob 0 -> 1 changes the level at 100 Hz by, and the mid knob moves the dip frequency by")
    v = VOICINGS["Marshall 1959/2203"]
    h0 = dB(yeh_H(f, 0.0, taper(.5), taper(.5), *v)); h1 = dB(yeh_H(f, 1.0, taper(.5), taper(.5), *v))
    print(f"treble 0->1: level at 100 Hz changes {np.interp(100, f, h1) - np.interp(100, f, h0):.1f} dB, at 5 kHz {np.interp(5000, f, h1) - np.interp(5000, f, h0):.1f} dB")
    for mm in (0.0, 0.5, 1.0):
        h = dB(yeh_H(f, 0.5, taper(mm), taper(.5), *v)); k = (f > 100) & (f < 3000); i = np.where(k)[0][int(np.argmin(h[k]))]
        print(f"mid {mm}: dip at {f[i]:.0f} Hz, {h[i]:.1f} dB")


# ---------- graphic EQ ----------
def t_graphic():
    fs = 48000; f = np.geomspace(30, 16000, 800)
    centres7 = [100, 200, 400, 800, 1600, 3200, 6400]
    print("# seven octave bands 100 Hz..6.4 kHz, series RBJ bells, +12 dB each. ripple = max-min of the summed response between the outer centres;")
    print("# single = width of one band alone at +12 dB between its +6 dB points, in octaves; neighbour = gain a single +12 dB band adds one octave away")
    print("Q\tall_up_peak_dB\tall_up_ripple_dB\tsingle_width_oct\tneighbour_dB\talternate_up_down_swing_dB")
    for Q in (0.7, 0.9, 1.0, 1.2, 1.41, 1.7, 2.0, 2.5):
        tot = np.zeros(len(f)); alt = np.zeros(len(f))
        for i, c in enumerate(centres7):
            b, a = rbj_peak(fs, c, Q, 12); tot += dB(H_biquad(b, a, f, fs))
            b, a = rbj_peak(fs, c, Q, 12 if i % 2 == 0 else -12); alt += dB(H_biquad(b, a, f, fs))
        k = (f >= 100) & (f <= 6400)
        b, a = rbj_peak(fs, 800, Q, 12); one = dB(H_biquad(b, a, f, fs)); up = f[one >= 6]
        print(f"{Q}\t{tot[k].max():.1f}\t{tot[k].max() - tot[k].min():.2f}\t{np.log2(up[-1] / up[0]):.2f}\t{np.interp(1600, f, one):.2f}\t{alt[k].max() - alt[k].min():.1f}")
    print("# top band 6.4 kHz +12 dB Q 1.41 at 44.1 kHz: error against analog at 10k / 15k / 20k, RBJ then Orfanidis")
    ff = np.array([10000.0, 15000.0, 20000.0])
    for name, fn in (("RBJ", rbj_peak), ("ORF", orfanidis_peak)):
        b, a = fn(44100, 6400, 1.41, 12); print(name + "\t" + "\t".join(f"{v:.2f}" for v in dB(H_biquad(b, a, ff, 44100)) - dB(ana_peak(ff, 6400, 1.41, 12))))


# ---------- tilt ----------
def t_tilt():
    print("# first-order tilt H(s) = (A s + wp)/(s + A wp), A = 10^(tilt_dB/20): gain -> -tilt at DC, +tilt at infinity, 0 dB at the pivot.")
    print("# bilinear with pre-warp at the pivot. dB at grid, fs 44.1 kHz, pivot 700 Hz; last column = max error against analog, 20 Hz..20 kHz")
    grid = [30, 80, 200, 700, 2500, 6000, 12000, 20000]; print("tilt_dB\t" + "\t".join(map(str, grid)) + "\tmax_err_dB")
    fs = 44100; fp = 700; f = np.geomspace(20, 20000, 500)
    for td in (6, 3, -3, -6):
        A = 10 ** (td / 20); g = np.tan(PI * fp / fs)
        # s -> (1 - z^-1)/(1 + z^-1) / g
        b = [A + g, g - A]; a = [1 + A * g, A * g - 1]; z = np.exp(-2j * PI * f / fs)
        hd = (b[0] + b[1] * z) / (a[0] + a[1] * z); s = 1j * f / fp; ha = (A * s + 1) / (s + A)
        print(f"{td}\t" + "\t".join(f"{np.interp(q, f, dB(hd)):.2f}" for q in grid) + f"\t{np.max(np.abs(dB(hd) - dB(ha))):.2f}")


if __name__ == "__main__":
    {"svf": t_svf, "orfsvf": t_orfsvf, "decramp": t_decramp, "stack": t_stack, "graphic": t_graphic, "tilt": t_tilt}[sys.argv[1]]()
