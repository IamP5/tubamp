#!/usr/bin/env python3
"""Acceptance for the drive engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.3.9, rows D1-D18) and a few extra rows (X*). Prints a PASS/FAIL
table, then the alias, latency and character tables; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_drive.py [--quick] [--types 1,2] [--only-specific]

The circuit models below mirror src/dsp/Drive.cpp constant for constant. Analog transfer
functions are what the D8/D12 rows compare against; the exact discretisations are only used
to take the circuit's own phase out of the latency measurement (D3, L rows), so what is left
is the delay the oversampling and the ADAA add.
"""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np  # noqa: E402

import fxcheck as fx  # noqa: E402

EFFECT = "drive"
TYPE_KEY = "type"
TYPES = {0: "Classic", 1: "Screamer", 2: "Distortion", 3: "Fuzz", 4: "Boost", 5: "Treble"}
# Float keys at their documented limits, for the finite-output test.
RANGES = {"gain": (0.0, 36.0), "tone": (500.0, 12000.0), "level": (-24.0, 12.0)}
# Percent of real time per instance (§4.3.7; Distortion and Fuzz raised to 3.5 / 4.0 % by the
# coordinator for the anti-aliasing scheme).
BUDGETS = {0: 0.35, 1: 2.0, 2: 3.5, 3: 4.0, 4: 0.05, 5: 0.05}
VARIANTS = {}
NULL_KEYS = [{}, {"gain": 36, "tone": 12000, "level": -6}, {"gain": 0, "tone": 500}]
PEDALS = (1, 2, 3)
DRIVE_CLASSIC_LATENCY = 3
ALIAS_F0 = (3830, 5000, 6100, 7919)

# --------------------------------------------------------------------------- circuit
IS, A = 2.52e-9, 1.752 * 0.02585
TS = dict(R1=4.7e3, Cz=47e-9, Cc=51e-12, R2=51e3, Pot=500e3, Rs=1e3, Cs=0.22e-6, Ri=10e3, Rf=1e3,
          Tone=20e3, Rz=220.0, Cz2=0.22e-6, End=0.005)
RAT = dict(R1=47.0, C1=2.2e-6, R2=560.0, C2=4.7e-6, Pot=100e3, Cf=100e-12, Rclip=1e3, Gbw=1.0e6,
           Rfix=1.5e3, Rpot=100e3, C=3.3e-9)
MUFF = dict(Boost=470e3 / 39e3, Stage=470e3 / 10e3, Rfb=470e3, Cfb=470e-12, Sus=100e3, Load=10e3,
            SusMin=0.018745, Rt=39e3, Ct=10e-9, Cb=4e-9, Rb=22e3, Pot=100e3, End=0.005,
            ToneLoad=470e3 * 100e3 / 570e3)
COUPLE_HZ = 1.0 / (2 * np.pi * 1e-6 * 10e3)
INPUT_HP_HZ = 15.9


def taper(p):
    return (81.0 ** p - 1.0) / 80.0


def knob_d(gain_db):
    return gain_db / 36.0


def knob_t(tone_hz):
    return np.log(tone_hz / 500.0) / np.log(24.0)


def tone_hz(t):
    return 500.0 * 24.0 ** t


def clip_g0(r_series):
    return 1.0 / (1.0 + 2 * IS * r_series / A)


def factor(sr, typ=1):
    base = 4 if sr < 80000 else (2 if sr < 160000 else 1)
    return 2 * base if typ == 2 else base


def hp1(s, hz):
    w = 2 * np.pi * hz
    return (s / w) / (1 + s / w)


def lp1(s, hz):
    return 1.0 / (1 + s / (2 * np.pi * hz))


def ts_pre(s, d):
    r2 = TS["R2"] + TS["Pot"] * taper(d)
    return (r2 / TS["R1"]) * (s * TS["R1"] * TS["Cz"]) / (1 + s * TS["R1"] * TS["Cz"]) / (1 + s * r2 * TS["Cc"])


def ts_tone(s, t):
    tp = np.clip(t, TS["End"], 1 - TS["End"])
    gl, gr = 1 / (tp * TS["Tone"]), 1 / ((1 - tp) * TS["Tone"])
    g, gs, c = gl + gr, 1 / TS["Rs"] + 1 / TS["Ri"], TS["Cz2"]
    n0, n1 = g / TS["Rs"], c * (g * TS["Rz"] + 1 + TS["Rf"] * gr) / TS["Rs"]
    d0 = gs * g
    d1 = gs * c * (g * TS["Rz"] + 1) + TS["Cs"] * g + gl * c
    d2 = TS["Cs"] * c * (g * TS["Rz"] + 1)
    return (n0 + n1 * s) / (d0 + d1 * s + d2 * s * s)


def ts_g0(d):
    return clip_g0(TS["R2"] + TS["Pot"] * taper(d))


def rat_gain(s, d):
    rd = RAT["Pot"] * taper(d)
    y = s * RAT["C1"] / (1 + s * RAT["R1"] * RAT["C1"]) + s * RAT["C2"] / (1 + s * RAT["R2"] * RAT["C2"])
    return 1 + rd / (1 + s * rd * RAT["Cf"]) * y


def rat_gbw_hz(d):
    return RAT["Gbw"] / abs(rat_gain(2j * np.pi * 1000.0, d))


def rat_filter_hz(t):
    return 1.0 / (2 * np.pi * (RAT["Rfix"] + RAT["Rpot"] * taper(1 - t)) * RAT["C"])


def muff_sustain(d):
    p = MUFF["SusMin"] + (1 - MUFF["SusMin"]) * d
    return p * MUFF["Load"] / (MUFF["Load"] + p * (1 - p) * MUFF["Sus"])


def muff_tone(s, t):
    """The passive tone stack by plain nodal analysis (nodes A, B and the wiper W, which is
    loaded by the output stage's bias divider), solved numerically: an independent check of
    the closed form Drive.cpp uses."""
    tp = float(np.clip(t, MUFF["End"], 1 - MUFF["End"]))
    ga, gb = 1 / (tp * MUFF["Pot"]), 1 / ((1 - tp) * MUFF["Pot"])
    gT, gB, gL = 1 / MUFF["Rt"], 1 / MUFF["Rb"], 1 / MUFF["ToneLoad"]
    out = []
    for sv in np.atleast_1d(s):
        y = np.array([[gT + sv * MUFF["Ct"] + ga, 0, -ga],
                      [0, sv * MUFF["Cb"] + gB + gb, -gb],
                      [-ga, -gb, ga + gb + gL]], complex)
        out.append(np.linalg.solve(y, np.array([gT, sv * MUFF["Cb"], 0], complex))[2])
    return np.array(out) if np.ndim(s) else out[0]


def muff_lp_hz():
    return 1.0 / (2 * np.pi * MUFF["Rfb"] * MUFF["Cfb"])


def analog(typ, f, gain_db, tone, sr=48000):
    """The circuit's analog small-signal response (clippers at their small-signal gain)."""
    s = 2j * np.pi * np.asarray(f, float)
    d, t = knob_d(gain_db), knob_t(tone)
    hp = hp1(s, INPUT_HP_HZ)
    if typ == 1:
        return hp * (1 + ts_pre(s, d) * ts_g0(d)) * ts_tone(s, t)
    if typ == 2:
        fos = sr * factor(sr, 2)
        bw = min(rat_gbw_hz(d), 0.45 * fos)
        fc = min(rat_filter_hz(t), 0.45 * fos)
        return hp * rat_gain(s, d) * lp1(s, bw) * clip_g0(RAT["Rclip"]) * lp1(s, fc)
    if typ == 3:
        lp, g0 = lp1(s, muff_lp_hz()), clip_g0(MUFF["Rfb"])
        return (hp * MUFF["Boost"] * lp * muff_sustain(d) * MUFF["Stage"] * lp * g0 * hp1(s, COUPLE_HZ)
                * MUFF["Stage"] * lp * g0 * hp1(s, COUPLE_HZ) * muff_tone(s, t))
    raise ValueError(typ)


def bilinear_s(f, rate, c):
    """s on the unit circle under s = c (1 - z^-1) / (1 + z^-1)."""
    return 1j * c * np.tan(np.pi * np.asarray(f, float) / rate)


def warp_c(hz, rate):
    w = 2 * np.pi * min(hz, 0.49 * rate)
    return w / np.tan(w / (2 * rate))


def discrete(typ, f, gain_db, tone, sr):
    """The same circuit as implemented: every section's own bilinear constant at its own
    rate, without the half-band filters and without the ADAA half-sample (that is the
    latency). Used only to take the circuit phase out of a delay measurement."""
    f = np.asarray(f, float)
    n = factor(sr, typ)
    fos = sr * n
    d, t = knob_d(gain_db), knob_t(tone)
    hp = hp1(bilinear_s(f, sr, warp_c(INPUT_HP_HZ, sr)), INPUT_HP_HZ)
    sos = bilinear_s(f, fos, 2 * fos)
    if typ == 1:
        pre = ts_pre(bilinear_s(f, fos, warp_c(720.0, fos)), d)
        return hp * (1 + pre * ts_g0(d)) * ts_tone(sos, t)
    if typ == 2:
        rd = RAT["Pot"] * taper(d)
        tau = max(rd * RAT["Cf"], 1 / (2 * np.pi * 0.45 * fos))
        y = sos * RAT["C1"] / (1 + sos * RAT["R1"] * RAT["C1"]) + sos * RAT["C2"] / (1 + sos * RAT["R2"] * RAT["C2"])
        g = 1 + rd / (1 + sos * tau) * y
        bw = min(rat_gbw_hz(d), 0.45 * fos)
        fc = min(rat_filter_hz(t), 0.45 * fos)
        return hp * g * lp1(sos, bw) * clip_g0(RAT["Rclip"]) * lp1(bilinear_s(f, fos, warp_c(fc, fos)), fc)
    if typ == 3:
        lp = lp1(bilinear_s(f, fos, warp_c(muff_lp_hz(), fos)), muff_lp_hz())
        cp = hp1(bilinear_s(f, fos, warp_c(COUPLE_HZ, fos)), COUPLE_HZ)
        g0 = clip_g0(MUFF["Rfb"])
        return (hp * MUFF["Boost"] * lp * muff_sustain(d) * MUFF["Stage"] * lp * g0 * cp
                * MUFF["Stage"] * lp * g0 * cp * muff_tone(sos, t))
    raise ValueError(typ)


def svf_lowpass(f, fc, sr):
    """juce::dsp::StateVariableTPTFilter low-pass, resonance 1/sqrt 2 (Classic's tone)."""
    fc = min(max(fc, 20.0), max(21.0, 0.49 * sr))
    w = np.tan(np.pi * np.asarray(f, float) / sr) / np.tan(np.pi * fc / sr)
    return 1.0 / (1 - w * w + 1j * np.sqrt(2) * w)


# --------------------------------------------------------------------------- measuring

def run(x, keys, sr, opts=()):
    return fx.invoke(EFFECT, x, keys, list(opts), sr)


def tones(freqs, amps, secs, sr):
    t = np.arange(int(round(secs * sr))) / sr
    return sum(a * np.sin(2 * np.pi * f * t) for f, a in zip(freqs, amps))


def fit(y, sr, freqs, start, stop=None):
    """Least-squares complex amplitudes of `freqs` (plus DC) over [start, stop) s."""
    y = fx.mono(y)
    a, b = int(start * sr), len(y) if stop is None else int(stop * sr)
    t = np.arange(a, b) / sr
    cols = [np.ones_like(t)]
    for f in freqs:
        cols += [np.cos(2 * np.pi * f * t), np.sin(2 * np.pi * f * t)]
    coef, *_ = np.linalg.lstsq(np.vstack(cols).T, y[a:b], rcond=None)
    return np.array([coef[1 + 2 * i] - 1j * coef[2 + 2 * i] for i in range(len(freqs))])


def sweep_response(keys, sr, level_db, freqs, secs=3.0, octaves=1 / 24):
    """|H| at `freqs` from an exponential sweep, by spectral division, smoothed over
    `octaves`. Valid where the system is linear at `level_db`."""
    x = fx.sweep(10.0, min(24000.0, 0.48 * sr), secs, level_db, sr)
    x = np.concatenate([np.zeros(int(0.05 * sr)), x, np.zeros(int(0.5 * sr))])
    y = fx.mono(run(x, keys, sr).out)
    n = 1 << int(np.ceil(np.log2(len(x))))
    X, Y = np.fft.rfft(x, n), np.fft.rfft(y, n)
    fr = np.fft.rfftfreq(n, 1.0 / sr)
    out = []
    for f in freqs:
        band = (fr > f * 2 ** (-octaves / 2)) & (fr < f * 2 ** (octaves / 2))
        out.append(np.sqrt(np.sum(np.abs(Y[band]) ** 2) / np.sum(np.abs(X[band]) ** 2)))
    return np.array(out)


def sweep_complex(keys, sr, level_db, secs=3.0):
    x = fx.sweep(10.0, min(24000.0, 0.48 * sr), secs, level_db, sr)
    x = np.concatenate([np.zeros(int(0.05 * sr)), x, np.zeros(int(0.5 * sr))])
    y = fx.mono(run(x, keys, sr).out)
    n = 1 << int(np.ceil(np.log2(len(x))))
    return np.fft.rfftfreq(n, 1.0 / sr), np.fft.rfft(y, n) / np.fft.rfft(x, n)


def alias_metrics(keys, sr, f0, level_db=-6.0):
    """1 s of a periodic sine (integer Hz, rectangular window, 1 Hz bins) after 0.5 s of
    settling: worst and RSS inharmonic component below 20 kHz re the fundamental. Every bin
    not within +-2 of a true harmonic (including DC) counts, above f0 as well as below."""
    x = fx.sine(f0, level_db, 1.5, sr)
    y = fx.mono(run(x, keys, sr).out)[-sr:]
    mags = np.abs(np.fft.rfft(y)) / sr * 2
    fund = mags[f0]
    excl = np.zeros(len(mags), bool)
    k = 0
    while k * f0 < sr / 2 + 3:
        excl[max(0, k * f0 - 2):k * f0 + 3] = True
        k += 1
    sel = ~excl
    sel[20001:] = False
    inh = mags[sel]
    worst_bin = np.flatnonzero(sel)[np.argmax(inh)]
    return fx.db(inh.max() / fund), fx.db(np.sqrt(np.sum(inh ** 2)) / fund), int(worst_bin)


def residual_delay(typ, keys, sr, level_db):
    """Group and phase delay at 1 kHz (samples) of the output against the discrete circuit
    model: what remains is the oversampling and ADAA delay."""
    freqs = (900.0, 1000.0, 1100.0)
    amp = fx.amp(level_db) / 3
    x = tones(freqs, [amp] * 3, 1.5, sr)
    r = run(x, keys, sr, ["--latency"])
    hm = fit(r.out, sr, freqs, 0.5) / fit(x, sr, freqs, 0.5)
    if typ == 0:
        href = svf_lowpass(freqs, keys.get("tone", 4000.0), sr)
    else:
        href = discrete(typ, freqs, keys.get("gain", 12.0), keys.get("tone", 4000.0), sr)
    ph = np.unwrap(np.angle(hm / href))
    w = 2 * np.pi * np.array(freqs) / sr
    group = -(ph[2] - ph[0]) / (w[2] - w[0])
    phase = -ph[1] / w[1]
    return group, phase, int(r.number(r"latency\t(-?\d+)"))


def p1db(keys, sr, f, lo=-80.0, hi=-6.0, step=0.25, hold=0.08):
    """Input level (dBFS) at which the fundamental's gain has fallen 1 dB from small signal."""
    levels = np.arange(lo, hi + 1e-9, step)
    n = int(round(hold * sr))
    t = np.arange(n * len(levels)) / sr
    env = np.repeat(fx.amp(levels), n)
    x = env * np.sin(2 * np.pi * f * t)
    y = fx.mono(run(x, keys, sr).out)
    gains = []
    skip = int(0.03 * sr)
    for i in range(len(levels)):
        a, b = i * n + skip, (i + 1) * n
        tt = np.arange(a, b) / sr
        basis = np.vstack([np.cos(2 * np.pi * f * tt), np.sin(2 * np.pi * f * tt)]).T
        cy, *_ = np.linalg.lstsq(basis, y[a:b], rcond=None)
        gains.append(fx.db(np.hypot(*cy) / fx.amp(levels[i])))
    gains = np.array(gains)
    ref = np.median(gains[:8])
    below = np.flatnonzero(gains <= ref - 1.0)
    if not len(below):
        return None
    i = below[0]
    g0, g1 = gains[i - 1] - ref, gains[i] - ref
    return levels[i - 1] + (-1.0 - g0) / (g1 - g0) * (levels[i] - levels[i - 1])


def steady(keys, sr, f, level_db, secs=1.5):
    x = fx.sine(f, level_db, secs, sr)
    return fx.mono(run(x, keys, sr).out)[-sr:]


def harmonics(y, sr, f0, count=5):
    mags = np.abs(np.fft.rfft(y)) / len(y) * 2
    df = sr / len(y)
    return [mags[int(round(k * f0 / df))] for k in range(1, count + 1)]


# --------------------------------------------------------------------------- the table

def specific_checks(report, args, extra):
    sel = (lambda t: True) if args.types is None else (lambda t: t in args.types)
    rates_d7 = (44100, 96000, 192000)

    # D1 ----------------------------------------------------------------------
    if sel(0):
        ok, detail = fx.classic_null(EFFECT, ["gain@1=30", "tone@2=800", "level@3=-6", "on@4=0", "on@5=1"])
        report.add("D1 steps", "Classic", ok, detail)
        ok, detail = fx.classic_null(EFFECT, ["gain@1=30", "tone@2=800", "level@3=-6"], blocks=[1])
        report.add("D1 steps", "Classic block 1", ok, detail)

    # D2 ----------------------------------------------------------------------
    if sel(0):
        ok, rows = True, []
        for sr in (44100, 48000, 96000, 192000):
            g, p, rep = residual_delay(0, {"type": 0, "gain": 0, "tone": 12000, "level": 0}, sr, -60.0)
            rows.append(f"{sr // 1000}k rep {rep} meas {p:.3f}")
            ok &= rep == 3 and abs(p - 3.14) <= 0.05
            extra["latency"].append(("Classic", sr, rep, p, g))
        report.add("D2 Classic latency", "44.1/48/96/192 kHz", ok, "; ".join(rows))

    # D3 + latency of every oversampled type ---------------------------------
    lat_keys = {1: ({"gain": 0}, -60.0), 2: ({"gain": 0}, -60.0), 3: ({"gain": 18}, -120.0)}
    for typ in PEDALS:
        if not sel(typ):
            continue
        ok, rows = True, []
        for sr in (22050, 44100, 48000, 96000, 192000):
            k, lv = lat_keys[typ]
            g, p, rep = residual_delay(typ, dict(type=typ, tone=4000, **k), sr, lv)
            # lround of the delay at the measurement's resolution (1e-3 sample): at 192 kHz
            # Screamer and Distortion sit at exactly 0.5 (the ADAA half sample alone).
            want = int(np.floor(round(g, 3) + 0.5))
            ok &= rep == want and (sr != 48000 or rep == 5)
            rows.append(f"{sr / 1000:g}k {rep}/{g:.3f}")
            extra["latency"].append((TYPES[typ], sr, rep, p, g))
        name = "D3 latency" if typ == 1 else "L latency"
        report.add(name, f"{TYPES[typ]} reported/measured", ok, " ".join(rows))

    # D4-D7 aliasing -----------------------------------------------------------
    alias_keys = lambda t: {"type": t, "gain": 36, "tone": 12000}  # noqa: E731
    for typ in PEDALS:
        if not sel(typ):
            continue
        worst_lim, sum_lim = (-50.0, -45.0) if typ == 1 else (-45.0, -40.0)
        row = "D4 alias" if typ == 1 else "D5 alias"
        for sr in (48000,) + rates_d7:
            ok, cells = True, []
            for f0 in ALIAS_F0:
                w, s, b = alias_metrics(alias_keys(typ), sr, f0)
                ok &= w <= worst_lim and s <= sum_lim
                cells.append(f"{f0}:{w:.1f}/{s:.1f}")
            report.add(row if sr == 48000 else "D7 " + row[3:], f"{TYPES[typ]} {sr / 1000:g}k worst/sum", ok,
                       " ".join(cells))
        for sr in (48000,) + rates_d7:
            w, s, b = alias_metrics(alias_keys(typ), sr, 1244)
            report.add("D6 alias 1244" if sr == 48000 else "D7 alias 1244", f"{TYPES[typ]} {sr / 1000:g}k sum",
                       s <= -80.0, f"sum {s:.1f} dB (worst {w:.1f})")
        for sr in (44100, 48000):
            for f0 in (1000, 2500, 3137, 5000, 7500):
                w, s, b = alias_metrics(alias_keys(typ), sr, f0)
                extra["alias"].append((TYPES[typ], sr, f0, w, s, b))

    # D8 Screamer small-signal --------------------------------------------------
    if sel(1):
        freqs = np.geomspace(30, 10000, 40)
        worst, where = 0.0, ""
        for d in (0.0, 0.5, 1.0):
            for t in (0.0, 0.5, 1.0):
                keys = {"type": 1, "gain": 36 * d, "tone": tone_hz(t)}
                h = sweep_response(keys, 48000, -70.0, freqs)
                err = fx.db(h) - fx.db(np.abs(analog(1, freqs, 36 * d, tone_hz(t))))
                i = np.argmax(np.abs(err))
                if abs(err[i]) > abs(worst):
                    worst, where = err[i], f"d={d} T={t} at {freqs[i]:.0f} Hz"
        report.add("D8 Screamer vs analog", "d,T in {0,.5,1} 30 Hz-10 kHz", abs(worst) <= 0.5,
                   f"worst {worst:+.2f} dB ({where})")

    # D9 pre-clip emphasis ------------------------------------------------------
    if sel(1):
        keys = {"type": 1, "gain": 18, "tone": tone_hz(0.5)}
        p = {f: p1db(keys, 48000, f) for f in (110, 640, 2500)}
        e110, e640 = p[2500] - p[110], p[2500] - p[640]
        ok = abs(e110 + 16.4) <= 1.0 and abs(e640 + 3.5) <= 0.7
        report.add("D9 Screamer pre-clip", "d=0.5, re 2.5 kHz", ok,
                   f"110 Hz {e110:+.2f} dB, 640 Hz {e640:+.2f} dB (P1dB 2.5k {p[2500]:.1f} dBFS)")

    # D10 response peak -----------------------------------------------------------
    if sel(1):
        # Fine grid, light smoothing and a parabola through the top three points: the peak is
        # broad (about 0.3 dB across 650-950 Hz), so a coarse grid moves it by tens of Hz.
        freqs = np.geomspace(400, 1600, 241)
        keys = {"type": 1, "gain": 18, "tone": tone_hz(0.5)}
        h = fx.db(sweep_response(keys, 48000, -70.0, freqs, secs=6.0, octaves=1 / 96))
        i = int(np.clip(np.argmax(h), 1, len(h) - 2))
        lf = np.log(freqs[i - 1:i + 2])
        cf = np.polyfit(lf, h[i - 1:i + 2], 2)
        fpk = float(np.exp(-cf[1] / (2 * cf[0])))
        hpk = float(np.polyval(cf, np.log(fpk)))
        r = fx.db(sweep_response(keys, 48000, -70.0, [110.0, 5000.0], secs=6.0, octaves=1 / 96)) - hpk
        ok = 650 <= fpk <= 800 and all(-13 <= v <= -9 for v in r)
        report.add("D10 Screamer peak", "d=0.5 T=0.5", ok,
                   f"peak {fpk:.0f} Hz; 110 Hz {r[0]:+.1f} dB, 5 kHz {r[1]:+.1f} dB re peak")

    # D11 symmetry ------------------------------------------------------------------
    if sel(1):
        worst = -300.0
        for g in (0, 12, 36):
            hs = harmonics(steady({"type": 1, "gain": g}, 48000, 1000, -18.0), 48000, 1000)
            worst = max(worst, fx.db(hs[1] / hs[0]))
        report.add("D11 Screamer symmetry", "H2, 1 kHz -18 dBFS, gain 0/12/36", worst <= -60.0, f"H2 {worst:.1f} dB")

    # D12 Distortion gain stage -------------------------------------------------------
    if sel(2):
        freqs = np.geomspace(30, 10000, 40)
        worst, where, peak = 0.0, "", None
        for d in (0.25, 0.5, 1.0):
            keys = {"type": 2, "gain": 36 * d, "tone": 12000}
            # -100 dBFS: at d = 1 the stage has 67 dB of gain, so -70 dBFS would already be
            # 0.7 V at the diodes, well into clipping; this level keeps them below 10 mV.
            h = sweep_response(keys, 48000, -100.0, freqs)
            ref = analog(2, freqs, 36 * d, 12000)
            err = fx.db(h) - fx.db(np.abs(ref))
            i = np.argmax(np.abs(err))
            if abs(err[i]) > abs(worst):
                worst, where = err[i], f"d={d} at {freqs[i]:.0f} Hz"
            if d == 1.0:
                s = 2j * np.pi * freqs
                fos = 48000 * factor(48000, 2)
                stage = h / np.abs(hp1(s, INPUT_HP_HZ) * lp1(s, min(rat_gbw_hz(1.0), 0.45 * fos))
                                   * clip_g0(RAT["Rclip"]) * lp1(s, min(rat_filter_hz(1.0), 0.45 * fos)))
                peak = fx.db(stage.max())
        target = fx.db(1 + RAT["Pot"] / (RAT["R1"] * RAT["R2"] / (RAT["R1"] + RAT["R2"])))
        ok = abs(worst) <= 0.5 and abs(peak - target) <= 1.0
        report.add("D12 Distortion stage", "d 0.25/0.5/1, -100 dBFS", ok,
                   f"worst {worst:+.2f} dB ({where}); stage max {peak:.1f} dB vs {target:.1f}")

    # D13 Distortion filter ---------------------------------------------------------------
    if sel(2):
        freqs = np.geomspace(40, 20000, 400)
        s = 2j * np.pi * freqs
        cells, ok = [], True
        for t in (1.0, 0.0):
            keys = {"type": 2, "gain": 0, "tone": tone_hz(t)}
            h = fx.db(sweep_response(keys, 48000, -40.0, freqs) / np.abs(hp1(s, INPUT_HP_HZ)))
            h -= h[(freqs > 40) & (freqs < 300)].max() if t == 0 else h[(freqs > 100) & (freqs < 2000)].max()
            below = np.flatnonzero(h <= -3.0)
            f3 = freqs[below[0]] if len(below) else np.inf
            if t == 1.0:
                ok &= f3 > 15000
            else:
                ok &= abs(f3 - 475) <= 50
            cells.append(f"T={t:g}: -3 dB at {f3:.0f} Hz")
        report.add("D13 Distortion filter", "48 kHz", ok, "; ".join(cells))

    # D14 Fuzz ------------------------------------------------------------------------------
    if sel(3):
        keys = {"type": 3, "gain": 18, "tone": tone_hz(0.5)}
        y = steady(keys, 48000, 1000, -18.0)
        thd = fx.fundamental_residual(y, 48000, 1000)
        # The response is read where the pedal is used, fully saturated (-18 dBFS in): the
        # clipped stages then deliver a near-constant fundamental and the tone network
        # shapes it. Small-signal the three 720 Hz pre-clip poles would dominate instead.
        lv = {f: fx.db(harmonics(steady(keys, 48000, f, -18.0), 48000, f, 1)[0]) for f in (220, 900, 5000)}
        w = np.log(900 / 220) / np.log(5000 / 220)
        line = lv[220] + w * (lv[5000] - lv[220])
        dip = lv[900] - line
        s5 = 2j * np.pi * np.array([220.0, 900.0, 5000.0])
        net = fx.db(np.abs(muff_tone(s5, 0.5)))
        net_dip = net[1] - (net[0] + w * (net[2] - net[0]))
        report.add("D14 Fuzz", "d=0.5 T=0.5, -18 dBFS", thd > 70.0 and dip <= -3.0,
                   f"THD {thd:.0f} %; 900 Hz {dip:+.1f} dB re the 220 Hz-5 kHz line "
                   f"(tone network alone {net_dip:+.2f} dB)")

    # D15 Boost / Treble --------------------------------------------------------------------
    if sel(4) or sel(5):
        y = steady({"type": 4, "gain": 12, "tone": 12000}, 48000, 1000, -18.0)
        thd_b = fx.fundamental_residual(y, 48000, 1000)
        y = steady({"type": 5, "gain": 12, "tone": 1000}, 48000, 1000, -12.0)
        thd_t = fx.fundamental_residual(y, 48000, 1000)
        freqs = np.geomspace(20, 2000, 30)
        hb = fx.db(sweep_response({"type": 4, "gain": 12, "tone": 12000}, 48000, -30.0, freqs)) - 12.0
        ht = fx.db(sweep_response({"type": 5, "gain": 12, "tone": 1000}, 48000, -30.0, [100.0, 10000.0]))
        sl = fx.db(sweep_response({"type": 5, "gain": 36, "tone": 1000}, 48000, -50.0,
                                  [1000 * 2 ** (-1 / 6), 1000 * 2 ** (1 / 6)]))
        slope = (sl[1] - sl[0]) * 3.0
        ok = (thd_b < 0.01 and thd_t < 0.01 and np.max(np.abs(hb)) <= 0.1 and abs(ht[1] - 12) <= 0.5
              and abs(ht[0]) <= 0.5 and abs(slope - 6.0) <= 0.5)
        report.add("D15 Boost/Treble", "48 kHz", ok,
                   f"THD {thd_b:.5f}/{thd_t:.5f} %; Boost flat {np.max(np.abs(hb)):.3f} dB; Treble "
                   f"{ht[0]:+.2f} dB at fc/10, {ht[1]:+.2f} dB at 10 fc (gain 12), {slope:.2f} dB/oct at fc (gain 36)")

    # D16 Level -------------------------------------------------------------------------------
    for typ in (2, 3):
        if not sel(typ):
            continue
        keys = {"type": typ, "gain": 18, "tone": 2449}
        lvl = extra["levels"].get(typ)
        if lvl is not None:
            keys["level"] = lvl
        pk = fx.db(np.max(np.abs(steady(keys, 48000, 1000, -18.0))))
        report.add("D16 level", f"{TYPES[typ]} start (level {keys.get('level', 0):g})", abs(pk + 12.0) <= 1.0,
                   f"{pk:.2f} dBFS peak")

    # D17 tone zipper ---------------------------------------------------------------------------
    if sel(1):
        sr = 48000
        events = [f"tone@{b * 512 / sr:.6f}={tone_hz(min(1.0, (b * 512 / sr - 0.5) / 0.5)):.3f}"
                  for b in range(int(0.5 * sr / 512), int(1.0 * sr / 512) + 2)]
        x = fx.sine(1000, -40.0, 1.6, sr)
        y = fx.mono(run(x, ["type=1", "gain=0", "tone=500"] + events, sr).out)
        seg = y[int(0.45 * sr):int(1.25 * sr)]
        win = np.blackman(len(seg))
        mags = np.abs(np.fft.rfft(seg * win))
        fr = np.fft.rfftfreq(len(seg), 1 / sr)
        fund = np.sqrt(np.sum(mags[(fr > 950) & (fr < 1050)] ** 2))
        harm = np.zeros(len(fr), bool)
        for k in range(1, 25):
            harm |= np.abs(fr - 1000 * k) <= 50
        sel_hi = (fr > 4000) & ~harm
        level = fx.db(np.sqrt(np.sum(mags[sel_hi] ** 2)) / fund)
        report.add("D17 tone zipper", "Screamer d=0, T 0->1 in 0.5 s", level <= -80.0, f"{level:.1f} dBc above 4 kHz")

    # D18 split-lane alignment (offline model of the processor's lane ring) ---------------------
    # Lane B empty means the lane ring hands the dry signal on delayed by the drive's reported
    # latency. Two otherwise identical lanes then comb by |1 + exp (-j w (D(f) - L))| / 2, where
    # D(f) is the delay the drive's processing adds (its phase against its own circuit model).
    # Restated by the coordinator: Classic's frozen 2x IIR delay rises from 3.14 samples at low
    # frequency to 3.84 at 16 kHz, so the bar is 0.5 dB to 10 kHz; 16 kHz is reported. D18n is
    # the same test for the new types (their IIR half-bands rise the same way, see Drive.cpp).
    def lane_comb(typ, keys, sr, level_db, lag):
        fr, h = sweep_complex(keys, sr, level_db)
        band = (fr >= 50) & (fr <= 16000)
        f = fr[band]
        w = 2 * np.pi * f / sr
        if typ == 0:
            ref = svf_lowpass(f, keys["tone"], sr)
        else:
            ref = discrete(typ, f, keys["gain"], keys["tone"], sr)
        delay = -np.unwrap(np.angle(h[band] / ref)) / w
        return f, fx.db(np.abs(1 + np.exp(-1j * w * (delay - lag))) / 2)

    def lane_row(name, scope, typ, keys, level_db, lag_of):
        cells, ok = [], True
        for sr in (44100, 48000):
            lag = lag_of(sr)
            f, comb = lane_comb(typ, keys, sr, level_db, lag)
            low = f <= 10000
            bad = np.flatnonzero(comb < -0.5)
            holds = f[bad[0]] if len(bad) else 16000.0
            ok &= comb[low].min() >= -0.5
            cells.append(f"{sr / 1000:g}k lag {lag}: min to 10 kHz {comb[low].min():.2f} dB, "
                         f"{comb[-1]:.2f} dB at 16 kHz, within 0.5 dB to {holds:.0f} Hz")
        report.add(name, scope, ok, "; ".join(cells))

    if sel(0):
        lane_row("D18 split lanes", "Classic gain 0, 0.5 dB to 10 kHz", 0, {"type": 0, "gain": 0, "tone": 12000},
                 -40.0, lambda sr: DRIVE_CLASSIC_LATENCY)
    for typ, gain, lv in ((1, 0, -60.0), (2, 0, -60.0), (3, 0, -120.0)):
        if sel(typ):
            lane_row("D18n split lanes", f"{TYPES[typ]} gain 0, 0.5 dB to 10 kHz", typ,
                     {"type": typ, "gain": gain, "tone": 12000}, lv,
                     lambda sr, t=typ: int(run(np.zeros(64), {"type": t}, sr, ["--latency"]).number(r"latency\t(-?\d+)")))

    # X: DC, gain smoothing, type switches ---------------------------------------------------------
    for typ in (1, 2, 3, 4, 5):
        if not sel(typ):
            continue
        y = steady({"type": typ, "gain": 36, "tone": 12000}, 48000, 100, -6.0, secs=2.0)
        dc = fx.db(abs(np.mean(y)))
        report.add("X1 DC", f"{TYPES[typ]} gain 36, 100 Hz -6 dBFS", dc <= -80.0, f"{dc:.1f} dBFS")
    for typ in (1, 2, 3, 4, 5):
        if not sel(typ):
            continue
        sr = 48000
        x = fx.sine(1000, -18.0, 2.0, sr)
        y = fx.mono(run(x, [f"type={typ}", "gain=0", "gain@1=36", "tone=4000"], sr).out)
        trans = np.abs(np.diff(y))[int(0.99 * sr):int(1.1 * sr)].max()
        # A click is a step larger than the steady signal makes at any gain the ramp passes
        # through (a RAT's edges are steepest near gain 24, not at 36).
        held = max(np.abs(np.diff(steady({"type": typ, "gain": g, "tone": 4000}, sr, 1000, -18.0))).max()
                   for g in range(0, 37, 3))
        report.add("X2 gain step", f"{TYPES[typ]} 0->36 dB at 1 s", trans <= 1.1 * held,
                   f"max step {trans:.4f} vs steady {held:.4f}")
    if args.types is None:
        sr = 48000
        x = fx.sine(220, -12.0, 7.0, sr)
        seq = [0, 1, 2, 3, 4, 5, 1]
        toks = [f"type@{i}={t}" for i, t in enumerate(seq)] + ["gain=24", "tone=4000"]
        y = fx.mono(run(x, toks, sr).out)
        jump = np.abs(np.diff(y))
        worst = 0.0
        for i in range(1, len(seq)):
            around = jump[int((i - 0.01) * sr):int((i + 0.1) * sr)].max()
            steadies = max(jump[int((i - 0.5) * sr):int((i - 0.05) * sr)].max(),
                           jump[int((i + 0.5) * sr):int((i + 0.95) * sr)].max())
            worst = max(worst, around / steadies)
        report.add("X3 type switches", "0-1-2-3-4-5-1, 220 Hz", worst <= 1.1,
                   f"largest step in a switch {worst:.2f}x the larger neighbour's")


def character(extra, args):
    """Harmonic profile at three levels, level against gain (informational)."""
    rows = []
    for typ in (1, 2, 3):
        if args.types is not None and typ not in args.types:
            continue
        for lv in (-30.0, -18.0, -6.0):
            y = steady({"type": typ}, 48000, 1000, lv)
            hs = harmonics(y, 48000, 1000)
            thd = fx.fundamental_residual(y, 48000, 1000)
            rows.append((TYPES[typ], f"{lv:g} dBFS", fx.db(hs[1] / hs[0]), fx.db(hs[2] / hs[0]), thd))
        gains = []
        for g in (0, 12, 24, 36):
            y = steady({"type": typ, "gain": g}, 48000, 1000, -18.0)
            gains.append(fx.db(np.max(np.abs(y))))
        extra["gain"].append((TYPES[typ], gains))
    extra["harm"] = rows
    # Pre-clip voicing: input for 1 dB compression vs frequency, re the frequency that clips
    # first (the study's Table 1 form). Distortion at the gain that gives 18 dB at 1 kHz.
    for typ, gain, lo in ((1, 18.0, -80.0), (2, None, -100.0), (3, 18.0, -140.0)):
        if args.types is not None and typ not in args.types:
            continue
        if gain is None:
            grid = np.linspace(0, 36, 721)
            g1k = [fx.db(abs(rat_gain(2j * np.pi * 1000.0, knob_d(g)))) for g in grid]
            gain = float(grid[np.argmin(np.abs(np.array(g1k) - 18.0))])
        keys = {"type": typ, "gain": gain, "tone": 12000}
        p = [p1db(keys, 48000, f, lo=lo, hi=0.0) for f in (110, 220, 640, 1280, 2500, 5000)]
        ref = min(v for v in p if v is not None)
        extra["pre"].append((TYPES[typ], gain, [ref - v if v is not None else None for v in p]))


def measure_levels(extra, args):
    """L2, L3: the level that puts a -18 dBFS 1 kHz sine at -12 dBFS peak at the row's start."""
    for typ in (2, 3):
        if args.types is not None and typ not in args.types:
            continue
        pk = fx.db(np.max(np.abs(steady({"type": typ, "gain": 18, "tone": 2449, "level": 0}, 48000, 1000, -18.0))))
        extra["level_needed"][typ] = -12.0 - pk


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--types", type=lambda s: [int(v) for v in s.split(",")], default=None)
    ap.add_argument("--only-specific", action="store_true", help="skip the §2.11 common checks")
    args = ap.parse_args()
    report = fx.Report(EFFECT)
    if not args.only_specific:
        fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                         quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
    extra = {"latency": [], "alias": [], "gain": [], "harm": [], "pre": [], "levels": {}, "level_needed": {}}
    # D16 runs at the starting points recorded in Drive.h (kDriveLevelDefault).
    header = (Path(__file__).resolve().parents[2] / "src" / "dsp" / "Drive.h").read_text()
    line = [ln for ln in header.splitlines() if "kDriveLevelDefault[DriveFx::numTypes]" in ln][0]
    vals = [float(v.strip().rstrip("f")) for v in line.split("{")[1].split("}")[0].split(",")]
    extra["levels"] = {2: vals[2], 3: vals[3]}
    specific_checks(report, args, extra)
    measure_levels(extra, args)
    character(extra, args)
    status = report.finish()

    print("\nlatency (samples): type, rate, reported, measured phase delay, measured group delay at 1 kHz")
    for typ, sr, rep, p, g in extra["latency"]:
        print(f"  {typ:<11}{sr:>7}  {rep:>2}  {p:7.3f}  {g:7.3f}")
    print("\nalias, gain 36, tone 12 kHz, -6 dBFS: worst inharmonic below 20 kHz / RSS sum, dB re fundamental"
          " (bin of the worst)")
    for typ, sr, f0, w, s, b in extra["alias"]:
        print(f"  {typ:<11}{sr:>7} {f0:>6} Hz  worst {w:7.1f} at {b:>5} Hz   sum {s:7.1f}")
    print("\nharmonics at 1 kHz, starting point: H2 dB, H3 dB, THD+N %")
    for typ, lv, h2, h3, thd in extra["harm"]:
        print(f"  {typ:<11}{lv:>9}  {h2:7.1f}  {h3:7.1f}  {thd:7.2f}")
    print("\noutput peak dBFS for -18 dBFS 1 kHz at gain 0 / 12 / 24 / 36 dB (tone at the default)")
    for typ, g in extra["gain"]:
        print(f"  {typ:<11}" + "  ".join(f"{v:7.2f}" for v in g))
    print("\npre-clip emphasis, dB re the first frequency to clip: 110 220 640 1280 2500 5000 Hz")
    for typ, gain, e in extra["pre"]:
        print(f"  {typ:<11}gain {gain:5.1f}  " + "  ".join("   n/a" if v is None else f"{v:6.1f}" for v in e))
    for typ, lvl in extra["level_needed"].items():
        print(f"\nlevel for -12 dBFS peak at the {TYPES[typ]} starting point: {lvl:.2f} dB")
    return status


if __name__ == "__main__":
    sys.exit(main())
