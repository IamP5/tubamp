#!/usr/bin/env python3
"""Acceptance for the eq engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.4.9, rows E1-E19). Prints a PASS/FAIL table and a table of
measurements; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_eq.py [--quick] [--types 1,2] [--only E5,E12] [--no-common]

Response checks render an impulse through tubamp_fxbench and take its FFT. The Stack
reference and the Orfanidis numbers come from the study's own design script
(docs/research/fx/eq/evidence/scripts/design.py), imported read-only.
"""
import argparse
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent.parent / "docs" / "research" / "fx" / "eq" / "evidence" / "scripts"))
import fxcheck as fx  # noqa: E402
import design as ref  # noqa: E402  (the study's design script: yeh_coeffs, bilinear3, taper, ana_peak)

EFFECT = "eq"
TYPE_KEY = "type"
TYPES = {0: "Classic", 1: "Parametric", 2: "Stack", 3: "Graphic"}
# Float keys at their documented limits, for the finite-output test.
RANGES = {"bass": (0.0, 10.0), "mid": (0.0, 10.0), "treble": (0.0, 10.0), "lowcut": (20.0, 400.0),
          "highcut": (2000.0, 20000.0), "mid_freq": (100.0, 8000.0), "mid_q": (0.5, 8.0), "tilt": (-6.0, 6.0),
          "level": (-12.0, 12.0), "g100": (-12.0, 12.0), "g200": (-12.0, 12.0), "g400": (-12.0, 12.0),
          "g800": (-12.0, 12.0), "g1k6": (-12.0, 12.0), "g3k2": (-12.0, 12.0), "g6k4": (-12.0, 12.0)}
# Percent of real time per instance (§4.4.7).
BUDGETS = {0: 0.1, 1: 0.30, 2: 0.15, 3: 0.30}
VARIANTS = {2: [{"voice": v} for v in range(4)]}
NULL_KEYS = [{"bass": 10, "mid": 0, "treble": 7}, {"bass": 0, "mid": 0, "treble": 0}, {"bass": 5, "mid": 5, "treble": 5.1}]

VOICES = ["Bassman 5F6-A", "Blackface AB763", "Marshall 1959/2203", "Vox AC30 TB"]   # design.py names, eq_voice order
VOICE_NAMES = ["Tweed", "Blackface", "British", "Top Boost"]
MAKEUP_DB = [8.9, 15.1, 6.4, 13.6]       # §4.4.5 table
GRAPHIC_KEYS = ["g100", "g200", "g400", "g800", "g1k6", "g3k2", "g6k4"]
GRAPHIC_HZ = [100, 200, 400, 800, 1600, 3200, 6400]
GRAPHIC_Q = np.sqrt(2.0)

MEASURES = []   # (topic, scope, value): reported, not judged


def measure(topic, scope, value):
    MEASURES.append((topic, scope, value))
    print(f"  INFO  {topic:<22} {scope:<34} {value}", flush=True)


def pmap(fn, items, workers=8):
    with ThreadPoolExecutor(max_workers=workers) as pool:
        return list(pool.map(fn, items))


# ---------------------------------------------------------------------------- analysis

def ir_length(sr):
    return 1 << int(np.ceil(np.log2(1.4 * sr)))


def impulse_response(keys, sr, n=None):
    """The engine's impulse response (mono, float32 output)."""
    n = n or ir_length(sr)
    x = np.zeros(n)
    x[0] = 1.0
    return fx.run(EFFECT, x, keys, ["--channels=1"], sr)[0][0]


def response_db(h, sr, freqs, pad=1 << 19):
    """|H| in dB at arbitrary frequencies, from an impulse response (zero-padded FFT, linear
    interpolation of the complex spectrum)."""
    n = max(pad, len(h))
    spec = np.fft.rfft(h, n)
    bins = np.asarray(freqs, float) / sr * n
    lo = np.clip(np.floor(bins).astype(int), 0, len(spec) - 2)
    frac = bins - lo
    hv = spec[lo] * (1 - frac) + spec[lo + 1] * frac
    return fx.db(np.abs(hv))


def at_freq_db(h, sr, f):
    """Exact DFT of the impulse response at one frequency."""
    n = np.arange(len(h))
    return float(fx.db(np.abs(np.sum(h * np.exp(-2j * np.pi * f * n / sr)))))


def nyquist_db(h):
    return float(fx.db(abs(np.sum(h * (-1.0) ** np.arange(len(h))))))


def ana_lowshelf(f, f0, q, gdb):
    A = 10 ** (gdb / 40); s = 1j * f / f0
    return A * (s * s + np.sqrt(A) / q * s + A) / (A * s * s + np.sqrt(A) / q * s + 1)


def ana_highshelf(f, f0, q, gdb):
    A = 10 ** (gdb / 40); s = 1j * f / f0
    return A * (A * s * s + np.sqrt(A) / q * s + 1) / (s * s + np.sqrt(A) / q * s + A)


def stack_digital_db(t, m, l, voice, sr, freqs):
    b, a = ref.yeh_coeffs(t, m, l, *ref.VOICINGS[VOICES[voice]])
    B, A = ref.bilinear3(b, a, sr)
    z = np.exp(-2j * np.pi * np.asarray(freqs) / sr)
    h = (B[0] + B[1] * z + B[2] * z ** 2 + B[3] * z ** 3) / (A[0] + A[1] * z + A[2] * z ** 2 + A[3] * z ** 3)
    return fx.db(np.abs(h)) + MAKEUP_DB[voice]


def stack_analog_db(t, m, l, voice, freqs):
    return fx.db(np.abs(ref.yeh_H(np.asarray(freqs), t, m, l, *ref.VOICINGS[VOICES[voice]]))) + MAKEUP_DB[voice]


def zipper_peak(y, sr, fc=1000.0, window=(0.45, 1.1)):
    """The study's zipper metric (zip_an.py): 200 ms raised-cosine fades at both ends, a
    brick-wall FFT high-pass at fc, the peak of what remains during the sweep, dBFS."""
    y = fx.mono(y).astype(np.float64).copy()
    n = int(0.2 * sr)
    w = np.ones(len(y))
    w[:n] = 0.5 - 0.5 * np.cos(np.pi * np.arange(n) / n)
    w[-n:] = w[:n][::-1]
    spec = np.fft.rfft(y * w)
    spec[np.fft.rfftfreq(len(y), 1 / sr) < fc] = 0
    r = np.fft.irfft(spec, len(y))
    seg = r[int(window[0] * sr):int(window[1] * sr)]
    return float(fx.db(np.max(np.abs(seg)) + 1e-30))


def stft_peak(y, sr, fc=1000.0, window=(0.45, 1.1), nfft=2048):
    """The contract's wording of the same metric: peak spectral line above fc, short-time
    FFT, Hann 2048, hop 512, amplitude-calibrated (a full-scale sine reads 0 dBFS)."""
    y = fx.mono(y)[int(window[0] * sr):int(window[1] * sr)]
    win = np.hanning(nfft)
    f = np.fft.rfftfreq(nfft, 1 / sr)
    peak = 1e-30
    for s in range(0, len(y) - nfft, nfft // 4):
        mag = np.abs(np.fft.rfft(y[s:s + nfft] * win)) * 2 / np.sum(win)
        peak = max(peak, float(np.max(mag[f >= fc])))
    return float(fx.db(peak))


def zipper(keys, sr=48000, freq=100.0, level=-12.0, block=512, secs=2.0):
    x = fx.sine(freq, level, secs, sr)
    y = fx.run(EFFECT, x, keys, [f"--block={block}", "--channels=1"], sr)[0]
    return zipper_peak(y, sr), stft_peak(y, sr)


def residual_dbc(keys, sr, f0=82.4, level=-20.0, secs=3.0, legacy=False):
    """Everything but the fundamental, dB re the fundamental (the study's tb.py noise method):
    a 20 ms fade-in, and the fit from 1 s so even a Q 8 bell's ringing has died away."""
    x = fx.sine(f0, level, secs, sr)
    nf = int(0.02 * sr)
    x[:nf] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(nf) / nf)
    opts = ["--channels=1"] + (["--legacy"] if legacy else [])
    y = fx.run(EFFECT, x, keys, opts, sr)[0]
    pct = fx.fundamental_residual(y, sr, f0, start=1.0)
    return 20 * np.log10(max(pct, 1e-30) / 100.0)


def env_rms(x, sr, ms=1.0):
    x = fx.mono(x)
    n = max(1, int(sr * ms / 1000))
    c = np.concatenate([[0.0], np.cumsum(x * x)])
    e = np.sqrt(np.maximum(c[n:] - c[:-n], 0.0) / n)
    return np.concatenate([e, np.full(n - 1, e[-1])])


# ---------------------------------------------------------------------------- E rows

def e1_classic_null(report, rates, blocks):
    for keys in NULL_KEYS:
        ok, detail = True, "0 differing samples"
        for ch in (1, 2):
            good, why = fx.classic_null(EFFECT, keys, [f"--channels={ch}"], rates=rates, blocks=blocks)
            if not good:
                ok, detail = False, f"{ch} ch: {why}"
        scope = "Classic " + "/".join(str(keys[k]) for k in ("bass", "mid", "treble"))
        report.add("E1 classic null", scope, ok, detail + f" ({len(rates)} rates x blocks {blocks} x 1/2 ch)")


def e2_exact_flat(report, rates):
    ok, worst = True, []
    for sr in rates:
        noise = fx.white(3.0, -12.0, sr).astype(np.float32).astype(np.float64)
        for ch, name in ((2, "EQ block"), (1, "amp EQ")):
            inp = np.vstack([noise] * ch)
            out = fx.run(EFFECT, inp, {"type": 0, "bass": 5, "mid": 5, "treble": 5}, [f"--channels={ch}"], sr)[0]
            differ = int(np.count_nonzero(out.view(np.uint64) != inp.astype(np.float32).astype(np.float64).view(np.uint64)))
            if differ:
                ok = False
                worst.append(f"{name} {sr}: {differ} differ")
        old = fx.run(EFFECT, noise, {"type": 0, "bass": 5, "mid": 5, "treble": 5}, ["--channels=1", "--legacy"], sr)[0][0]
        pct = 100.0 * np.count_nonzero(old != noise) / len(noise)
        res = fx.db(np.sqrt(np.mean((old - noise) ** 2)) / np.sqrt(np.mean(noise ** 2)))
        measure("flat residual (legacy)", f"Classic 5/5/5 {sr} Hz", f"{pct:.1f} % differ, {res:.1f} dB")
    report.add("E2 exact flat", "Classic + amp EQ (mono) 5/5/5", ok,
               "; ".join(worst) or f"output == input bit for bit at {', '.join(str(r) for r in rates)} Hz")
    return ok


def e3_classic_zipper(report):
    keys = ["type=0", "bass@0.5..1.0=5..10"]
    peak, line = zipper(keys)
    old, _ = (lambda y: (zipper_peak(y, 48000), None))(
        fx.run(EFFECT, fx.sine(100, -12, 2.0, 48000), keys, ["--channels=1", "--legacy"], 48000)[0])
    p64, _ = zipper(keys, block=64)
    report.add("E3 classic zipper", "Classic bass 5->10, 100 Hz", peak <= -88.0,
               f"{peak:.1f} dBFS (block 64: {p64:.1f}; STFT line {line:.1f}; legacy {old:.1f}; limit -88)")


def e4_parametric_flat(report, rates):
    ok, detail = True, []
    for sr in rates:
        noise = fx.white(3.0, -12.0, sr).astype(np.float32).astype(np.float64)
        for ch in (1, 2):
            inp = np.vstack([noise] * ch)
            out = fx.run(EFFECT, inp, {"type": 1}, [f"--channels={ch}"], sr)[0]
            differ = int(np.count_nonzero(out.view(np.uint64) != inp.view(np.uint64)))
            if differ:
                ok = False
                detail.append(f"{sr} Hz {ch} ch: {differ} differ")
    report.add("E4 parametric flat", "Parametric defaults vs input", ok,
               "; ".join(detail) or f"0 differing samples at {', '.join(str(r) for r in rates)} Hz, 1 and 2 ch")
    return ok


def e5_residual(report):
    sr = 192000
    r = residual_dbc({"type": 1, "bass": 10}, sr)
    c = residual_dbc({"type": 0, "bass": 10}, sr)
    report.add("E5 residual 192k", "Parametric bass 10, 82.4 Hz -20 dBFS", r <= -120.0,
               f"{r:.1f} dBc (Classic {c:.1f}; limit -120)")


E6_POINTS = [("bass", 50.0, 19.5), ("bass", 150.0, 10.1), ("mid", 425.0, 15.0), ("treble", 1800.0, 5.0),
             ("treble", 10000.0, 10.0)]


def e6_response(report, rates):
    ok, rows = True, []
    for sr in rates:
        hs = {band: impulse_response({"type": 1, band: 10}, sr) for band in ("bass", "mid", "treble")}
        old = {band: impulse_response({"type": 0, band: 10}, sr) for band in ("bass",)}
        for band, f, want in E6_POINTS:
            got = at_freq_db(hs[band], sr, f)
            good = abs(got - want) <= 0.05
            ok &= good
            rows.append(f"{sr // 1000}k {band}@{f:g}: {got:+.3f}{'' if good else ' FAIL'}")
        measure("E6 Classic reference", f"Classic bass 10 @150 Hz {sr} Hz", f"{at_freq_db(old['bass'], sr, 150.0):+.3f} dB")
    report.add("E6 response at 10", "Parametric single band at 10", ok,
               "; ".join(r for r in rows if "FAIL" in r) or "all points within 0.05 dB")
    measure("E6 detail", "Parametric", "; ".join(rows))
    return ok


def e7_parametric_zipper(report, rates):
    ok, rows = True, []
    cases = [("bass 5->10", ["type=1", "bass@0.5..1.0=5..10"]),
             ("mid freq 100->8k at +15", ["type=1", "mid=10", "mid_q=0.7", "mid_freq@0.5..1.0=100..8000"])]
    for sr in rates:
        for name, keys in cases:
            peak, line = zipper(keys, sr=sr)
            ok &= peak <= -90.0
            rows.append(f"{name} {sr // 1000}k {peak:.1f} (STFT {line:.1f})")
    report.add("E7 parametric zipper", "bass and mid-freq sweeps", ok, "; ".join(rows) + "; limit -90")
    return ok


def e8_decramp(report):
    sr = 44100
    h = impulse_response({"type": 1, "mid": 9, "mid_freq": 8000, "mid_q": 1.0}, sr)
    f = np.geomspace(20, 20000, 2000)
    err = np.max(np.abs(response_db(h, sr, f) - fx.db(np.abs(ref.ana_peak(f, 8000, 1.0, 12.0)))))
    ny = nyquist_db(h)
    report.add("E8 decramp", "mid 8 kHz Q1 +12 dB, 44.1 kHz", err <= 0.5 and ny >= 1.5,
               f"error {err:.3f} dB (limit 0.5); {ny:+.2f} dB at Nyquist (limit +1.5)")


def e9_blend(report, rates):
    ok, rows = True, []
    centres = np.arange(1400, 2601, 10)
    for sr in rates:
        hs = pmap(lambda c: impulse_response({"type": 1, "mid": 9, "mid_freq": float(c), "mid_q": 1.4}, sr), centres)
        g = np.array([at_freq_db(h, sr, c) for h, c in zip(hs, centres)])
        ny = np.array([nyquist_db(h) for h in hs])
        fine = np.geomspace(1000, 4000, 3000)
        peaks = np.array([fine[int(np.argmax(response_db(h, sr, fine)))] for h in hs])
        step = float(np.max(np.abs(np.diff(g))))
        mono = bool(np.all(np.diff(peaks) > 0) and (np.all(np.diff(ny) >= -1e-6) or np.all(np.diff(ny) <= 1e-6)))
        ok &= step <= 0.05 and mono
        rows.append(f"{sr // 1000}k: max centre step {step:.5f} dB, centre gain {g.min():.4f}..{g.max():.4f}, "
                    f"peak freq and Nyquist gain monotonic {mono}")
    report.add("E9 blend continuity", "mid 1.4 -> 2.6 kHz, +12 dB, Q 1.4", ok, "; ".join(rows))


def e10_cuts(report):
    ok, rows = True, []
    for sr in (44100, 48000, 96000):
        for key, freqs in (("lowcut", (20.6, 80.0, 200.0, 400.0)), ("highcut", (2000.0, 8000.0, 15000.0, 19000.0))):
            for f in freqs:
                h = impulse_response({"type": 1, key: f}, sr)
                expect_f = min(f, 0.45 * sr) if key == "highcut" else f
                got = at_freq_db(h, sr, expect_f)
                good = abs(got + 3.0) <= 0.1
                ok &= good
                if not good:
                    rows.append(f"{key} {f:g} @ {sr}: {got:.3f} dB")
    # End stops: bit-exact pass-through, from the start and after coming back to them.
    sr = 48000
    noise = fx.white(2.0, -12.0, sr).astype(np.float32).astype(np.float64)
    out = fx.run(EFFECT, noise, {"type": 1, "lowcut": 20, "highcut": 20000}, ["--channels=1"], sr)[0][0]
    exact = bool(np.array_equal(out.view(np.uint64), noise.view(np.uint64)))
    back = fx.run(EFFECT, noise, ["type=1", "lowcut=120", "highcut=5000", "tilt=3", "level=6",
                                  "lowcut@0.5=20", "highcut@0.5=20000", "tilt@0.5=0", "level@0.5=0"], ["--channels=1"], sr)[0][0]
    tail = int(1.0 * sr)
    back_exact = bool(np.array_equal(back[tail:].view(np.uint64), noise[tail:].view(np.uint64)))
    # Off at the end stops, then engaged on a fresh start of the type (after a switch away
    # and back): the cuts, tilt and level must come back exactly as a static render has them.
    keys_on = ["lowcut=150", "highcut=4000", "tilt=3", "level=-4"]
    sw = fx.run(EFFECT, noise, ["type=1", "lowcut=20", "highcut=20000", "tilt=0", "level=0", "type@0.3=3"]
                + [k.replace("=", "@0.4=") for k in keys_on] + ["type@0.6=1"], ["--channels=1"], sr)[0][0]
    static = fx.run(EFFECT, noise, ["type=1"] + keys_on, ["--channels=1"], sr)[0][0]
    settle = int(1.2 * sr)
    reengaged = bool(np.max(np.abs(sw[settle:] - static[settle:])) < 1e-6)
    ok &= exact and back_exact and reengaged
    report.add("E10 low/high cut", "-3 dB at the set frequency; end stops", ok,
               ("; ".join(rows) + "; " if rows else "all within -3.0 +- 0.1 dB (44.1/48/96 k); ")
               + f"end stops bit-exact {exact}, after returning to them {back_exact}, re-engaged after a reset {reengaged}")


def e11_tilt(report):
    sr = 48000
    ok, rows = True, []
    for t in (-6.0, 6.0):
        h = impulse_response({"type": 1, "tilt": t}, sr)
        lo, piv, hi = at_freq_db(h, sr, 20.0), at_freq_db(h, sr, 700.0), at_freq_db(h, sr, 20000.0)
        good = abs(lo + t) <= 0.1 and abs(piv) <= 0.05 and abs(hi - t) <= 0.2
        ok &= good
        rows.append(f"tilt {t:+g}: {lo:+.3f} @20, {piv:+.3f} @700, {hi:+.3f} @20k")
    report.add("E11 tilt", "+-6 dB, 48 kHz", ok, "; ".join(rows))


def knob_grid():
    steps = [0.0, 2.5, 5.0, 7.5, 10.0]
    return [(kt, km, kl) for kt in steps for km in steps for kl in steps]


def stack_ir(voice, kt, km, kl, sr):
    return impulse_response({"type": 2, "voice": voice, "treble": kt, "mid": km, "bass": kl}, sr)


def e12_e13_stack(report, rates12, quick):
    grid = knob_grid()
    if quick:
        grid = grid[::7]
    f20 = np.geomspace(20, 20000, 1500)
    worst12 = {}
    worst13 = {}
    for sr in sorted(set(rates12) | {44100, 96000}):
        for v in range(4):
            hs = pmap(lambda g: stack_ir(v, *g, sr), grid)
            e12 = e13 = 0.0
            f10 = f20[f20 <= 10000]
            for (kt, km, kl), h in zip(grid, hs):
                t, m, l = kt / 10, ref.taper(km / 10), ref.taper(kl / 10)
                if sr in rates12:
                    fr = f20[f20 < 0.5 * sr]
                    e12 = max(e12, float(np.max(np.abs(response_db(h, sr, fr) - stack_digital_db(t, m, l, v, sr, fr)))))
                if sr in (44100, 96000):
                    e13 = max(e13, float(np.max(np.abs(response_db(h, sr, f10) - stack_analog_db(t, m, l, v, f10)))))
            if sr in rates12:
                worst12[(sr, v)] = e12
            if sr in (44100, 96000):
                worst13[(sr, v)] = e13
    ok12 = all(e <= 0.01 for e in worst12.values())
    report.add("E12 stack vs design.py", f"{len(grid)} knob settings x 4 voices", ok12,
               f"worst {max(worst12.values()):.5f} dB at " + ", ".join(f"{r // 1000}k" for r in rates12) + " (limit 0.01)")
    lim = {44100: 1.6, 96000: 0.3}
    ok13 = all(e <= lim[sr] for (sr, v), e in worst13.items())
    report.add("E13 stack vs analog", "to 10 kHz, 5x5x5 grid, 4 voices", ok13,
               "; ".join(f"{sr // 1000}k {VOICE_NAMES[v]} {e:.3f}" for (sr, v), e in sorted(worst13.items()))
               + " (limits 1.6 / 0.3)")


def dip_hz(h, sr, lo=100.0, hi=3000.0):
    f = np.geomspace(lo, hi, 6000)
    return float(f[int(np.argmin(response_db(h, sr, f)))])


def e14_interaction(report):
    sr = 48000
    d0 = dip_hz(impulse_response({"type": 2, "voice": 2, "mid": 0}, sr), sr)
    d1 = dip_hz(impulse_response({"type": 2, "voice": 2, "mid": 10}, sr), sr)
    ok = abs(d0 / 852 - 1) <= 0.03 and abs(d1 / 603 - 1) <= 0.03
    report.add("E14 stack interaction", "British, mid 0 -> 10", ok, f"dip {d0:.0f} -> {d1:.0f} Hz (852 -> 603 +- 3 %)")


def e15_stack_zipper(report):
    ok, rows = True, []
    for v in range(4):
        peak, line = zipper([f"type=2", f"voice={v}", "bass@0.5..1.0=5..10"])
        ok &= peak <= -85.0
        rows.append(f"{VOICE_NAMES[v]} {peak:.1f} (STFT {line:.1f})")
    report.add("E15 stack zipper", "bass 5->10, 100 Hz, every voice", ok, "; ".join(rows) + "; limit -85")


def half_gain_width(h, sr, centre):
    f = np.geomspace(centre / 4, min(centre * 4, 0.499 * sr), 20000)
    r = response_db(h, sr, f)
    up = f[r >= r.max() / 2]
    return float(np.log2(up[-1] / up[0])), float(r.max())


def e16_graphic(report):
    ok, rows = True, []
    for sr in (44100, 48000, 96000):
        for key, c in zip(GRAPHIC_KEYS, GRAPHIC_HZ):
            h = impulse_response({"type": 3, key: 12}, sr)
            centre = at_freq_db(h, sr, c)
            width, _ = half_gain_width(h, sr, c)
            good = abs(centre - 12) <= 0.1 and abs(width - 1) <= 0.05
            ok &= good
            if not good or sr == 44100:
                rows.append(f"{sr // 1000}k {c}: {centre:.3f} dB, {width:.3f} oct")
    h = impulse_response({"type": 3, "g6k4": 12}, 44100)
    ff = np.array([10000.0, 15000.0, 20000.0])
    err = np.abs(np.array([at_freq_db(h, 44100, f) for f in ff]) - fx.db(np.abs(ref.ana_peak(ff, 6400, GRAPHIC_Q, 12))))
    ok &= bool(np.all(err <= 0.3))
    report.add("E16 graphic band", "each band +12 dB alone", ok,
               "; ".join(r for r in rows if "44k" not in r or True)[:400]
               + f"; 6.4k band error at 10/15/20 kHz {err[0]:.3f}/{err[1]:.3f}/{err[2]:.3f} dB (limit 0.3)")


def e17_graphic_flat(report):
    ok = True
    for sr in (44100, 96000, 192000):
        noise = fx.white(2.0, -12.0, sr).astype(np.float32).astype(np.float64)
        for ch in (1, 2):
            inp = np.vstack([noise] * ch)
            out = fx.run(EFFECT, inp, {"type": 3}, [f"--channels={ch}"], sr)[0]
            ok &= bool(np.array_equal(out.view(np.uint64), inp.view(np.uint64)))
    report.add("E17 graphic flat", "all bands 0", ok, "bit-exact pass-through" if ok else "output differs from input")


def chord(sr, secs):
    t = np.arange(int(secs * sr)) / sr
    x = sum(a * np.sin(2 * np.pi * f * h * t) for f in (98.0, 123.47, 146.83) for h, a in ((1, 1), (2, 0.5), (3, 0.3)))
    return 0.25 * x / np.max(np.abs(x))


def switch_case(keys_old, keys_new, sr=48000, block=64, t_sw=1.0):
    """Renders a switch at t_sw and measures (fade ms, overshoot dB). The fade runs from the
    first sample that leaves the outgoing type's own output to the first sample from which
    the output equals a fresh render of the incoming type started where the switcher reset
    it (the end of the run of samples that equal the dry input exactly)."""
    x = chord(sr, 2.0)
    toks_old = [f"{k}={v}" for k, v in keys_old.items()]
    toks_sw = toks_old + [f"{k}@{t_sw}={v}" for k, v in keys_new.items()]
    opts = [f"--block={block}", "--channels=1"]
    old = fx.run(EFFECT, x, toks_old, opts, sr)[0][0]
    sw = fx.run(EFFECT, x, toks_sw, opts, sr)[0][0]
    xf = x.astype(np.float32).astype(np.float64)
    start = int(np.argmax(sw != old))
    dry = np.nonzero(sw[start:] == xf[start:])[0]
    if len(dry) == 0:
        return None
    # The engine is reset on the 16-sample grid that follows the faded-out run of dry samples.
    run_end = start + int(dry[-1]) if np.all(np.diff(dry) == 1) else start + int(dry[np.argmax(np.diff(dry) > 1)])
    h1 = (run_end + 1 + 15) // 16 * 16
    merged = dict(keys_old, **keys_new)
    fresh = fx.run(EFFECT, x[h1:], [f"{k}={v}" for k, v in merged.items()], opts, sr)[0][0]
    diff = np.abs(sw[h1:] - fresh)
    tol = 1e-6 * max(np.max(np.abs(fresh)), 1e-9)
    bad = np.nonzero(diff > tol)[0]
    end = h1 + (int(bad[-1]) + 1 if len(bad) else 0)
    new_static = fx.run(EFFECT, x, [f"{k}={v}" for k, v in merged.items()], opts, sr)[0][0]
    a, b = start, end + int(0.005 * sr)
    bound = np.maximum.reduce([env_rms(old, sr), env_rms(new_static, sr), env_rms(xf, sr)])
    over = float(np.max(fx.db(env_rms(sw, sr)[a:b] / np.maximum(bound[a:b], 1e-12))))
    return 1000.0 * (end - start) / sr, over


def e18_switch(report):
    cases = [("Classic -> Parametric", {"type": 0, "bass": 8, "mid": 3, "treble": 6}, {"type": 1}),
             ("Parametric -> Stack", {"type": 1, "bass": 8, "mid": 3, "treble": 6}, {"type": 2}),
             ("Stack -> Graphic", {"type": 2, "g200": 6, "g1k6": -6}, {"type": 3}),
             ("Graphic -> Classic", {"type": 3, "g200": 6, "g1k6": -6, "bass": 7}, {"type": 0}),
             ("level pair P -> G", {"type": 1, "level": 6}, {"type": 3})]
    cases += [(f"Stack voice {VOICE_NAMES[a]} -> {VOICE_NAMES[b]}", {"type": 2, "voice": a}, {"voice": b})
              for a, b in ((0, 1), (1, 2), (2, 3), (3, 0))]
    ok, fades, overs, bad = True, [], [], []
    for block in (37, 64, 512):
        for name, old, new in cases:
            r = switch_case(old, new, block=block)
            if r is None:
                ok = False
                bad.append(f"{name} block {block}: no dry point found")
                continue
            fade, over = r
            fades.append(fade)
            overs.append(over)
            if abs(fade - 40.0) > 2.0 or over > 3.0:
                ok = False
                bad.append(f"{name} block {block}: {fade:.1f} ms {over:+.1f} dB")
    report.add("E18 switches", f"{len(cases)} type/voice switches x blocks 37/64/512", ok,
               ("; ".join(bad) + "; " if bad else "") + f"fade {min(fades):.2f}..{max(fades):.2f} ms (40 +- 2), "
               f"envelope at most {max(overs):+.2f} dB over the louder of old/new/dry (limit 3)")


# ---------------------------------------------------------------------------- measurements

def measure_nyquist(rates):
    """Worst deviation from the analog prototype, 20 Hz .. min(20 kHz, 0.45 fs)."""
    for sr in rates:
        f = np.geomspace(20, min(20000, 0.45 * sr), 2500)
        worst_bell, worst_bell_at = 0.0, ""
        for fc in (2500, 4000, 6000, 8000):
            for q in (0.5, 0.7, 1.0, 2.0, 8.0):
                for knob, gdb in ((10, 15.0), (0, -15.0)):
                    h = impulse_response({"type": 1, "mid": knob, "mid_freq": fc, "mid_q": q}, sr)
                    e = float(np.max(np.abs(response_db(h, sr, f) - fx.db(np.abs(ref.ana_peak(f, fc, q, gdb))))))
                    if e > worst_bell:
                        worst_bell, worst_bell_at = e, f"{fc} Hz Q {q} {gdb:+g} dB"
        h = impulse_response({"type": 1, "treble": 10}, sr)
        shelf = float(np.max(np.abs(response_db(h, sr, f) - fx.db(np.abs(ana_highshelf(f, 1800, 0.707, 10.0))))))
        hc = impulse_response({"type": 0, "treble": 10}, sr)
        classic = float(np.max(np.abs(response_db(hc, sr, f) - fx.db(np.abs(ana_highshelf(f, 1800, 0.707, 10.0))))))
        g = 0.0
        for key, c in zip(GRAPHIC_KEYS[4:], GRAPHIC_HZ[4:]):
            for gdb in (12, -12):
                h = impulse_response({"type": 3, key: gdb}, sr)
                g = max(g, float(np.max(np.abs(response_db(h, sr, f) - fx.db(np.abs(ref.ana_peak(f, c, GRAPHIC_Q, gdb)))))))
        measure("near-Nyquist error", f"{sr} Hz to {f[-1] / 1000:.1f} kHz",
                f"Parametric bell >= 2.5 kHz worst {worst_bell:.2f} dB ({worst_bell_at}); treble shelf 1.8k "
                f"{shelf:.3f}; Classic treble {classic:.3f}; Graphic 1.6-6.4k {g:.3f}")
        # what the plain bilinear bell would have done, for comparison (closed form)
        cook = 0.0
        for fc in (2500, 4000, 6000, 8000):
            for q in (0.5, 0.7, 1.0, 2.0, 8.0):
                for gdb in (15.0, -15.0):
                    b, a = ref.rbj_peak(sr, fc, q, gdb)
                    cook = max(cook, float(np.max(np.abs(fx.db(np.abs(ref.H_biquad(b, a, f, sr)))
                                                          - fx.db(np.abs(ref.ana_peak(f, fc, q, gdb)))))))
        measure("cookbook bell (closed form)", f"{sr} Hz", f"worst {cook:.2f} dB over the same grid")


def measure_stack_bilinear():
    for sr in (44100, 48000, 96000, 192000):
        f = np.geomspace(20, min(20000, 0.45 * sr), 1500)
        rows = []
        for v in range(4):
            e = 0.0
            for kt, km, kl in knob_grid()[::5]:
                t, m, l = kt / 10, ref.taper(km / 10), ref.taper(kl / 10)
                e = max(e, float(np.max(np.abs(stack_digital_db(t, m, l, v, sr, f) - stack_analog_db(t, m, l, v, f)))))
            rows.append(f"{VOICE_NAMES[v]} {e:.2f}")
        measure("Stack bilinear error", f"{sr} Hz to {f[-1] / 1000:.1f} kHz", ", ".join(rows))


def measure_voicings():
    sr = 48000
    study = {0: (773, 9.7), 1: (409, 15.9), 2: (731, 6.3), 3: (664, 18.4)}
    for v in range(4):
        h = impulse_response({"type": 2, "voice": v}, sr)
        f = np.geomspace(20, 20000, 6000)
        r = response_db(h, sr, f)
        k = (f > 150) & (f < 2500)
        idip = np.nonzero(k)[0][int(np.argmin(r[k]))]
        shoulder = min(r[:idip].max(), r[idip:].max())
        measure("Stack noon dip", VOICE_NAMES[v],
                f"{f[idip]:.0f} Hz, {shoulder - r[idip]:.1f} dB deep (study analog {study[v][0]} Hz / {study[v][1]} dB); "
                f"mean 80 Hz-5 kHz {np.mean(r[(f >= 80) & (f <= 5000)]):+.2f} dB after makeup")


def measure_interaction():
    sr = 48000
    f = np.geomspace(20, 20000, 2000)
    for t, label in ((1, "Parametric"), (0, "Classic")):
        hs = [impulse_response({"type": t, b: 10}, sr) for b in ("bass", "mid", "treble")]
        both = impulse_response({"type": t, "bass": 10, "mid": 10, "treble": 10}, sr)
        s = sum(response_db(h, sr, f) for h in hs)
        d = float(np.max(np.abs(response_db(both, sr, f) - s)))
        measure("band interaction", f"{label} 10/10/10 vs dB sum of bands", f"{d:.4f} dB (series bands add in dB)")
    h = impulse_response(dict({"type": 3}, **{k: 12 for k in GRAPHIC_KEYS}), sr)
    r = response_db(h, sr, f)
    k = (f >= 100) & (f <= 6400)
    measure("band interaction", "Graphic all +12 dB", f"peak {r[k].max():.2f} dB, ripple {r[k].max() - r[k].min():.2f} dB "
                                                      "(study 18.3 / 5.7)")
    h = impulse_response({"type": 3, "g800": 12}, sr)
    measure("band interaction", "Graphic 800 Hz +12, gain at 1.6 kHz", f"{at_freq_db(h, sr, 1600):.2f} dB (study 2.5)")


def measure_residuals():
    sr = 192000
    for keys, label in (({"type": 1, "bass": 0}, "Parametric bass 0"), ({"type": 1, "bass": 10}, "Parametric bass 10"),
                        ({"type": 1, "mid": 10, "mid_freq": 100, "mid_q": 8}, "Parametric mid +15 100 Hz Q 8"),
                        ({"type": 1, "lowcut": 400, "bass": 10}, "Parametric low cut 400 + bass 10"),
                        ({"type": 3, "g100": 12}, "Graphic 100 Hz +12"), ({"type": 3, "g100": -12}, "Graphic 100 Hz -12"),
                        ({"type": 2, "bass": 10, "mid": 0}, "Stack British bass 10 mid 0"),
                        ({"type": 0, "bass": 10}, "Classic bass 10"), ({"type": 0, "bass": 0}, "Classic bass 0"),
                        ({"type": 0, "bass": 5, "mid": 5, "treble": 5}, "Classic flat"), ({"type": 1}, "Parametric flat")):
        r = residual_dbc(keys, sr)
        measure("residual 192 kHz", label, f"{r:.1f} dBc (82.4 Hz, -20 dBFS)")
    noise = np.zeros(int(2 * sr))
    out = fx.run(EFFECT, noise, {"type": 1, "bass": 10, "mid": 10, "treble": 10, "level": 12}, ["--channels=1"], sr)[0]
    measure("noise floor", "Parametric 10/10/10 +12 dB, silence in", f"max |out| {np.max(np.abs(out)):.3g}")


def measure_clicks():
    """Knob steps and fast automation under a low E (82.4 Hz, -12 dBFS): the zipper metric's
    peak above 1 kHz. A perfectly smooth 5 ms two-pole gain step of +19 dB on this note
    reads -72 dBFS (the floor of the specified smoothing itself, not an artefact)."""
    sr = 48000
    x = fx.sine(82.41, -12, 2.0, sr)

    def click(keys, legacy=False, block=512):
        opts = [f"--block={block}", "--channels=1"] + (["--legacy"] if legacy else [])
        return zipper_peak(fx.run(EFFECT, x, keys, opts, sr)[0], sr)

    steps = ["bass@0.5=10", "bass@0.5=0", "mid@0.5=10", "mid@0.5=0", "treble@0.5=10", "treble@0.5=0"]
    rows = [f"{k.replace('@0.5=', '->')} {click(['type=0', k]):.1f} ({click(['type=0', k], legacy=True):.1f})" for k in steps]
    measure("click: knob step", "Classic (legacy)", "; ".join(rows))
    rows = [f"{k.replace('@0.5=', '->')} {click(['type=1', k]):.1f}" for k in steps + ["level@0.5=12", "tilt@0.5=6"]]
    measure("click: knob step", "Parametric", "; ".join(rows))
    for v in range(4):
        rows = [f"{k.replace('@0.5=', '->')} {click(['type=2', f'voice={v}', k]):.1f}" for k in steps]
        measure("click: knob step", f"Stack {VOICE_NAMES[v]}", "; ".join(rows))
    rows = [f"{k}->12 {click(['type=3', f'{k}@0.5=12']):.1f}" for k in GRAPHIC_KEYS]
    measure("click: knob step", "Graphic", "; ".join(rows))
    fast = [("gain 0->10 in 50 ms @200 Hz", ["mid=0", "mid_freq=200", "mid@0.5..0.55=0..10"]),
            ("freq 100->8k in 50 ms, +15", ["mid=10", "mid_freq@0.5..0.55=100..8000"]),
            ("Q 0.5->8 in 50 ms, +15 @100", ["mid=10", "mid_freq=100", "mid_q@0.5..0.55=0.5..8"]),
            ("freq 200<->2k at 4 Hz, +15 Q2", ["mid=10", "mid_q=2"] + [
                f"mid_freq@{0.5 + 0.125 * i:.3f}..{0.625 + 0.125 * i:.3f}={200 if i % 2 == 0 else 2000}..{2000 if i % 2 == 0 else 200}"
                for i in range(4)]),
            ("low cut 20->400 step", ["lowcut@0.5=400"]), ("high cut 20k->2k step", ["highcut@0.5=2000"]),
            ("low cut 100->20 (off) step", ["lowcut=100", "lowcut@0.5=20"])]
    for bs in (512, 64):
        rows = [f"{name} {click(['type=1'] + keys, block=bs):.1f}" for name, keys in fast]
        measure("click: fast automation", f"Parametric block {bs}", "; ".join(rows))


def measure_cpu():
    worst = {1: {"bass": 8, "mid": 3, "treble": 7, "lowcut": 80, "highcut": 9000, "mid_freq": 3000, "mid_q": 2, "tilt": 2,
                 "level": 3},
             2: {"bass": 7, "mid": 3, "treble": 6, "level": 3},
             3: {k: 3.0 for k in GRAPHIC_KEYS} | {"level": 3}}
    moving = {0: ["bass@0..10=0..10", "mid@0..10=10..0", "treble@0..10=0..10"],
              1: ["bass@0..10=0..10", "mid@0..10=10..0", "mid_freq@0..10=100..8000", "mid_q@0..10=0.5..8",
                  "lowcut@0..10=20..400", "highcut@0..10=20000..2000", "tilt@0..10=-6..6", "level@0..10=-12..12"],
              2: ["bass@0..10=0..10", "mid@0..10=10..0", "treble@0..10=0..10", "level@0..10=-12..12"],
              3: [f"{k}@0..10=-12..12" for k in GRAPHIC_KEYS] + ["level@0..10=-12..12"]}
    for t in TYPES:
        rows = []
        for label, keys in (("defaults", [f"type={t}"]), ("all sections active", [f"type={t}"] + [f"{k}={v}" for k, v in worst.get(t, {"bass": 7}).items()]),
                            ("every control moving", [f"type={t}"] + moving[t])):
            r = fx.invoke(EFFECT, "gen:noise:pink:-18", keys, ["--bench"], 48000)
            rows.append(f"{label} {r.number(r'([0-9.]+) % of real time'):.4f} %")
        measure("cpu 48 kHz stereo blk 64", TYPES[t], "; ".join(rows))
    r = fx.invoke(EFFECT, "gen:noise:pink:-18", ["type=0"], ["--bench", "--legacy"], 48000)
    measure("cpu 48 kHz stereo blk 64", "legacy ToneStackEQ", f"{r.number(r'([0-9.]+) % of real time'):.4f} %")


# ---------------------------------------------------------------------------- main

def specific_checks(report, args):
    quick = args.quick
    rates3 = [48000] if quick else [44100, 96000, 192000]
    only = set(args.only or [])

    def want(row):
        return not only or row in only

    results = {}
    if want("E1"):
        e1_classic_null(report, [48000] if quick else fx.NULL_RATES, [37, 512] if quick else [1, 37, 512])
    if want("E2") or want("E19"):
        results["E2"] = e2_exact_flat(report, rates3)
    if want("E3"):
        e3_classic_zipper(report)
    if want("E4") or want("E19"):
        results["E4"] = e4_parametric_flat(report, rates3)
    if want("E5"):
        e5_residual(report)
    if want("E6") or want("E19"):
        results["E6"] = e6_response(report, [48000] if quick else [44100, 48000, 96000, 192000])
    if want("E7") or want("E19"):
        results["E7"] = e7_parametric_zipper(report, [48000] if quick else [44100, 48000, 96000, 192000])
    if want("E8"):
        e8_decramp(report)
    if want("E9"):
        e9_blend(report, [44100] if quick else [44100, 48000])
    if want("E10"):
        e10_cuts(report)
    if want("E11"):
        e11_tilt(report)
    if want("E12") or want("E13"):
        e12_e13_stack(report, [48000] if quick else [44100, 48000, 96000, 192000], quick)
    if want("E14"):
        e14_interaction(report)
    if want("E15"):
        e15_stack_zipper(report)
    if want("E16"):
        e16_graphic(report)
    if want("E17"):
        e17_graphic_flat(report)
    if want("E18"):
        e18_switch(report)
    if want("E19") and len(results) == 4:
        report.add("E19 rates", "E2, E4, E6, E7 at " + ", ".join(f"{r // 1000}k" for r in rates3), all(results.values()),
                   ", ".join(f"{k} {'ok' if v else 'FAIL'}" for k, v in results.items()))
    if args.measure:
        measure_nyquist([44100, 48000, 96000, 192000])
        measure_stack_bilinear()
        measure_voicings()
        measure_interaction()
        measure_residuals()
        measure_clicks()
        measure_cpu()


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true", help="48 kHz stereo, blocks 37 and 512, fewer grid points")
    ap.add_argument("--types", type=lambda s: [int(v) for v in s.split(",")], default=None,
                    help="comma-separated type indices for the common checks (default: all)")
    ap.add_argument("--only", type=lambda s: s.split(","), default=None, help="run only these E rows, e.g. E5,E12")
    ap.add_argument("--no-common", action="store_true", help="skip the §2.11 common checks")
    ap.add_argument("--measure", action="store_true", help="also print the measurement tables (no pass/fail)")
    return ap.parse_args()


def main():
    args = parse_args()
    report = fx.Report(EFFECT)
    if not args.no_common:
        fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                         quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
    specific_checks(report, args)
    if MEASURES:
        print(f"\n{'measurement':<28}{'scope':<44}value")
        for topic, scope, value in MEASURES:
            print(f"{topic:<28}{scope:<44}{value}")
        print()
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
