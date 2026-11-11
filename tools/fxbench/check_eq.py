#!/usr/bin/env python3
"""Acceptance for the eq engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.4.9, rows E1-E19), then E20-E24, added after the verification of
.audit/fx-verify/drive-eq.md and code.md: the bell against the analog curve over its whole
range, non-finite input, host float noise on neutral controls, re-enabling after a bypass, and
a Classic retarget in a ramp's last segment; then E25-E28, added after the second
verification (.audit/fx-verify/r2-tone.md): overshoot when a control moves under a held note,
Classic knob moves at every rate and block, type-switch click and level, absurd finite input.
Prints a PASS/FAIL table and a table of measurements; exit status 1 on any FAIL.

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


# The study's table has +10.1 dB at 150 Hz; the old and the new Classic both read +10.00 there,
# which is what a +20 dB shelf with its corner at 150 Hz gives (half its gain). The +10.1 was a
# misreading of the installed-AU table, so the point is +10.0 within the same 0.05 dB.
E6_POINTS = [("bass", 50.0, 19.5), ("bass", 150.0, 10.0), ("mid", 425.0, 15.0), ("treble", 1800.0, 5.0),
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
               ("; ".join(r for r in rows if "FAIL" in r) or "all points within 0.05 dB")
               + " (150 Hz point +10.0, restated from the study's +10.1: legacy Classic reads +10.00 too)")
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
    # Restated from 1.6 / 0.3 dB: E12 requires the engine to be design.py's own bilinear
    # transform, and that reference is itself 1.65 dB (44.1 kHz) and 0.31 dB (96 kHz) off the
    # analog curve to 10 kHz on this grid, so the old bars sat below what E12 allows.
    lim = {44100: 1.7, 96000: 0.35}
    ref13 = {}
    f10 = f20[f20 <= 10000]
    for sr in (44100, 96000):
        ref13[sr] = max(float(np.max(np.abs(stack_digital_db(kt / 10, ref.taper(km / 10), ref.taper(kl / 10), v, sr, f10)
                                            - stack_analog_db(kt / 10, ref.taper(km / 10), ref.taper(kl / 10), v, f10))))
                        for v in range(4) for kt, km, kl in grid)
    ok13 = all(e <= lim[sr] for (sr, v), e in worst13.items())
    report.add("E13 stack vs analog", "to 10 kHz, 5x5x5 grid, 4 voices", ok13,
               "; ".join(f"{sr // 1000}k {VOICE_NAMES[v]} {e:.3f}" for (sr, v), e in sorted(worst13.items()))
               + f" (limits 1.7 / 0.35, restated from 1.6 / 0.3: the bilinear reference E12 requires is itself "
               f"{ref13[44100]:.3f} / {ref13[96000]:.3f} dB off)")


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


# The engine's switch: the incoming type is reset on the 16-sample grid and runs unheard for
# kSwitchSettleSeconds (Eq.cpp), rounded up to the grid, before its fade starts.
SWITCH_SETTLE_S = 0.05


def switch_case(keys_old, keys_new, sr=48000, block=64, t_sw=1.0):
    """Renders a switch at t_sw and measures (fade ms, overshoot dB). The fade runs from the
    first sample that leaves the outgoing type's own output to the first sample from which
    the output equals a fresh render of the incoming type started where the switch reset it
    (the settle time before the fade's first sample; the switch used to fade through a run
    of dry samples, and the reset was found at its end)."""
    x = chord(sr, 2.0)
    toks_old = [f"{k}={v}" for k, v in keys_old.items()]
    toks_sw = toks_old + [f"{k}@{t_sw}={v}" for k, v in keys_new.items()]
    opts = [f"--block={block}", "--channels=1"]
    old = fx.run(EFFECT, x, toks_old, opts, sr)[0][0]
    sw = fx.run(EFFECT, x, toks_sw, opts, sr)[0][0]
    xf = x.astype(np.float32).astype(np.float64)
    merged = dict(keys_old, **keys_new)
    if np.array_equal(sw, old):
        # Two types that give the same output (Parametric and Graphic at neutral, same level):
        # the crossfade between them leaves the audio untouched, and there is no fade to time.
        new_static = fx.run(EFFECT, x, [f"{k}={v}" for k, v in merged.items()], opts, sr)[0][0]
        return ("identical", 0.0) if np.array_equal(sw, new_static) else None
    start = int(np.argmax(sw != old))
    # The fade's first samples can round to the outgoing output exactly (its weight starts at
    # ~1e-6), so the reset is the grid point, settle time before the fade, that a fresh render
    # of the incoming type started there matches.
    end = None
    first = (start - 16 * int(np.ceil(SWITCH_SETTLE_S * sr / 16))) // 16 * 16
    for h1 in range(first, max(first - 16 * 16, 0), -16):
        fresh = fx.run(EFFECT, x[h1:], [f"{k}={v}" for k, v in merged.items()], opts, sr)[0][0]
        diff = np.abs(sw[h1:] - fresh)
        tol = 1e-6 * max(np.max(np.abs(fresh)), 1e-9)
        bad = np.nonzero(diff > tol)[0]
        if len(bad) == 0 or bad[-1] < len(diff) - int(0.5 * sr):
            end = h1 + (int(bad[-1]) + 1 if len(bad) else 0)
            break
    if end is None:
        return None
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
    ok, fades, overs, bad, same = True, [], [], [], set()
    for block in (37, 64, 512):
        for name, old, new in cases:
            r = switch_case(old, new, block=block)
            if r is None:
                ok = False
                bad.append(f"{name} block {block}: no reset point found")
                continue
            fade, over = r
            overs.append(over)
            if fade == "identical":
                same.add(name)
                continue
            fades.append(fade)
            if abs(fade - 40.0) > 2.0 or over > 3.0:
                ok = False
                bad.append(f"{name} block {block}: {fade:.1f} ms {over:+.1f} dB")
    report.add("E18 switches", f"{len(cases)} type/voice switches x blocks 37/64/512", ok,
               ("; ".join(bad) + "; " if bad else "") + f"fade {min(fades):.2f}..{max(fades):.2f} ms (40 +- 2), "
               f"envelope at most {max(overs):+.2f} dB over the louder of old/new/dry (limit 3)"
               + (f"; {', '.join(sorted(same))}: output identical to both types throughout, no fade to time" if same else ""))


# ---------------------------------------------------------------------------- E20-E24 (post-verification)

# The bilinear bell with Orfanidis decramping above 2.5 kHz (the engine before E20), worst error
# at Q 8 on the same grid: the matched bell must not be worse for narrow bells.
E20_OLD_Q8 = {22050: 0.415, 44100: 0.067, 48000: 0.055, 96000: 0.013, 192000: 0.003}
E20_Q = [0.5, 0.7, 1.0, 1.4, 2.0, 3.0, 4.0, 8.0]


def e20_bell_map(report, quick):
    """Parametric bell against the analog prototype, 20 Hz to min(20 kHz, 0.45 fs), over f0, Q,
    gain and rate. Verification found 1.91 dB at Q 0.5, 8 kHz, 44.1 kHz (the decramping
    overshot on wide bells); 22.05 kHz was 13.5 dB off."""
    rates = [22050, 48000] if quick else [22050, 44100, 48000, 96000, 192000]
    f0s = [100, 250, 500, 1000, 1500, 2000, 2500, 3000, 4000, 5000, 6000, 7000, 8000]
    gains = [(10, 15.0), (0, -15.0), (7, 6.0), (4, -3.0)]
    jobs = [(sr, f0, q, knob, g) for sr in rates for f0 in f0s for q in E20_Q for knob, g in gains]

    def one(job):
        sr, f0, q, knob, g = job
        h = impulse_response({"type": 1, "mid": knob, "mid_freq": f0, "mid_q": q}, sr)
        f = np.geomspace(20, min(20000, 0.45 * sr), 1500)
        # Bins of at most 0.1 Hz: a Q 8 peak at 100 Hz is misread by 0.03 dB at 0.37 Hz.
        pad = 1 << int(np.ceil(np.log2(sr / 0.1)))
        e = np.abs(response_db(h, sr, f, pad=pad) - fx.db(np.abs(ref.ana_peak(f, f0, q, g))))
        return float(np.max(e))

    errs = pmap(one, jobs)
    worst = {}
    for (sr, f0, q, knob, g), e in zip(jobs, errs):
        key = (sr, q)
        if e > worst.get(key, (0.0,))[0]:
            worst[key] = (e, f0, g)
    ok, rows = True, []
    for sr in rates:
        wide = max(worst[(sr, q)] for q in E20_Q if q <= 4.0)
        narrow = worst[(sr, 8.0)]
        good = wide[0] < 1.0 and narrow[0] <= E20_OLD_Q8[sr]
        ok &= good
        rows.append(f"{sr // 1000 if sr % 1000 == 0 else sr / 1000:g}k: Q 0.5-4 {wide[0]:.2f} dB (f0 {wide[1]} Hz {wide[2]:+g} dB), "
                    f"Q 8 {narrow[0]:.3f} (old {E20_OLD_Q8[sr]})" + ("" if good else " FAIL"))
        measure("E20 bell error by Q", f"{sr} Hz", " ".join(f"Q{q:g} {worst[(sr, q)][0]:.2f}" for q in E20_Q))
    report.add("E20 bell vs analog", f"Q 0.5-8, f0 100 Hz-8 kHz, +-15/+6/-3 dB", ok,
               "; ".join(rows) + " (limits: Q 0.5-4 < 1 dB; Q 8 no worse than the old design)")


def e21_non_finite(report, quick):
    """One run of NaN, +Inf or -Inf (10 samples) in a 220 Hz sine: the output must hold no
    non-finite sample and equal, bit for bit, the render of the same input with zeros in their
    place (no state kept anything of them). A finite 3e38 sample (it overflows Classic's float
    biquads) must leave the output finite and back within 1e-4 of that render 100 ms later.
    Verification: one NaN latched Classic, Parametric, Stack and non-flat Graphic for good."""
    sr = 48000
    cases = [("Classic 10/5/7", {"type": 0, "bass": 10, "treble": 7}), ("Classic flat", {"type": 0}),
             ("Parametric all sections", {"type": 1, "bass": 7, "mid": 10, "mid_freq": 100, "mid_q": 8, "lowcut": 80,
                                          "highcut": 5000, "tilt": 3, "level": 3}),
             ("Parametric defaults", {"type": 1}), ("Graphic", {"type": 3, "g100": 12, "g1k6": -6, "level": 2}),
             ("Graphic flat", {"type": 3})] + [(f"Stack {VOICE_NAMES[v]}", {"type": 2, "voice": v, "bass": 10}) for v in range(4)]
    blocks = [37, 512] if quick else [1, 37, 512]
    chans = [2] if quick else [1, 2]
    at = sr // 2
    jobs = [(name, keys, bad, bs, ch) for name, keys in cases for bad in (np.nan, np.inf, -np.inf, 3e38)
            for bs in blocks for ch in chans]

    def one(job):
        name, keys, bad, bs, ch = job
        x = 0.25 * np.sin(2 * np.pi * 220 * np.arange(sr) / sr)
        clean = x.copy()
        clean[at:at + 10] = 0.0
        x[at:at + 10] = bad
        opts = [f"--block={bs}", f"--channels={ch}"]
        y = fx.run(EFFECT, np.vstack([x] * ch), keys, opts, sr)[0]
        yc = fx.run(EFFECT, np.vstack([clean] * ch), keys, opts, sr)[0]
        nonfinite = int(np.count_nonzero(~np.isfinite(y)))
        if bad == 3e38:
            tail = slice(at + int(0.1 * sr), None)
            dev = float(np.max(np.abs(y[:, tail] - yc[:, tail]))) if nonfinite == 0 else float("inf")
            good = nonfinite == 0 and dev < 1e-4
        else:
            dev = 0.0 if np.array_equal(y.view(np.uint64), yc.view(np.uint64)) else float("inf")
            good = nonfinite == 0 and dev == 0.0
        return good, f"{name} {bad:g} block {bs} {ch} ch: {nonfinite} non-finite" + ("" if nonfinite else f", |dev| {dev:.3g}")

    res = pmap(one, jobs)
    bad_rows = [d for good, d in res if not good]
    report.add("E21 non-finite input", f"NaN/+-Inf/3e38 x {len(cases)} settings x blocks {blocks} x {chans} ch",
               not bad_rows, "; ".join(bad_rows[:6]) or f"{len(jobs)} renders: never a non-finite output sample; "
               "NaN/Inf renders equal the zero-substituted input bit for bit, 3e38 back within 1e-4 after 100 ms")


# The values the host hands back for neutral defaults (drive-eq.md E-f): eq_level and the graphic
# bands 1.79e-7 dB, eq_tilt 8.94e-8 dB; the knobs 5 + 1 ulp; the cut end stops a float step in.
NOISE = 1.79e-7


def e22_neutral(report, quick):
    """Every control at its neutral value plus host float noise must leave an exact
    pass-through (fresh instance, and after moving away and back), and one 0.1 dB step must
    still be honoured."""
    rates = [48000] if quick else [44100, 96000, 192000]
    neutral = [("tilt", {"type": 1, "tilt": 8.94e-8}), ("level", {"type": 1, "level": NOISE}),
               ("-level", {"type": 1, "level": -NOISE}), ("bass", {"type": 1, "bass": 5.00000048}),
               ("mid", {"type": 1, "mid": 4.99999952}), ("treble", {"type": 1, "treble": 5.00000048}),
               ("lowcut", {"type": 1, "lowcut": 20.0000019}), ("highcut", {"type": 1, "highcut": 19999.998}),
               ("all Parametric", {"type": 1, "tilt": 8.94e-8, "level": NOISE, "bass": 5.00000048, "lowcut": 20.0000019,
                                   "highcut": 19999.998}),
               ("Graphic level", {"type": 3, "level": NOISE})]
    neutral += [(f"Graphic {k}", {"type": 3, k: NOISE}) for k in GRAPHIC_KEYS] + [(f"Graphic -{GRAPHIC_KEYS[0]}", {"type": 3, GRAPHIC_KEYS[0]: -NOISE})]
    neutral += [("all Graphic", dict({"type": 3, "level": NOISE}, **{k: NOISE for k in GRAPHIC_KEYS}))]
    moved = [("Parametric moved and back", ["type=1", "tilt=3", "level=6", "bass=8", "tilt@0.3=8.94e-8", f"level@0.3={NOISE}",
                                            "bass@0.3=5.00000048"]),
             ("Graphic moved and back", ["type=3", "level=6"] + [f"{k}=6" for k in GRAPHIC_KEYS]
              + [f"level@0.3={NOISE}"] + [f"{k}@0.3={NOISE}" for k in GRAPHIC_KEYS])]
    jobs = [(sr, ch, name, keys) for sr in rates for ch in (1, 2) for name, keys in neutral + moved]

    def one(job):
        sr, ch, name, keys = job
        noise = fx.white(2.0, -12.0, sr).astype(np.float32).astype(np.float64)
        inp = np.vstack([noise] * ch)
        out = fx.run(EFFECT, inp, keys, [f"--channels={ch}"], sr)[0]
        start = int(1.0 * sr) if isinstance(keys, list) else 0
        differ = int(np.count_nonzero(out[:, start:].view(np.uint64) != inp[:, start:].view(np.uint64)))
        return differ == 0, f"{name} {sr} {ch} ch: {differ} differ"

    res = pmap(one, jobs)
    bad = [d for good, d in res if not good]
    # Stack is never a pass-through, but level at noise must be level at 0 exactly.
    sr = 48000
    noise = fx.white(1.0, -12.0, sr).astype(np.float32).astype(np.float64)
    st = fx.run(EFFECT, noise, {"type": 2, "level": NOISE}, ["--channels=1"], sr)[0]
    st0 = fx.run(EFFECT, noise, {"type": 2, "level": 0.0}, ["--channels=1"], sr)[0]
    stack_ok = bool(np.array_equal(st.view(np.uint64), st0.view(np.uint64)))
    report.add("E22a neutral float noise", f"{len(neutral) + len(moved)} cases x {len(rates)} rates x 1/2 ch", not bad and stack_ok,
               "; ".join(bad[:6]) or f"output == input bit for bit in all {len(jobs)}; Stack level {NOISE:g} == level 0: {stack_ok}")

    # One deliberate step of 0.1 dB is still a 0.1 dB change.
    ok, rows = True, []
    for name, keys, f, want in ([("level", {"type": 1, "level": d}, 1000.0, d) for d in (0.1, -0.1)]
                                + [("Graphic level", {"type": 3, "level": 0.1}, 1000.0, 0.1)]
                                + [(k, {"type": 3, k: d}, c, d) for k, c in zip(GRAPHIC_KEYS, GRAPHIC_HZ) for d in (0.1, -0.1)]
                                + [("tilt", {"type": 1, "tilt": d}, 20.0, -d) for d in (0.1, -0.1)]
                                + [("bass 5.1", {"type": 1, "bass": 5.1}, 20.0, 0.4)]):
        got = at_freq_db(impulse_response(keys, 48000), 48000, f)
        good = abs(got - want) <= 0.005
        ok &= good
        if not good or name in ("level", "tilt", "g100"):
            rows.append(f"{name} {keys.get(name, keys.get('level', ''))}: {got:+.4f} dB at {f:g} Hz" + ("" if good else " FAIL"))
    report.add("E22b smallest step", "0.1 dB on level, tilt, each band; 0.1 knob on bass", ok,
               "; ".join(rows) + " (each within 0.005 dB of the step)")


def e23_reenable(report, quick):
    """The processor's bypass: the block is processed, not called at all for a second, then
    reset() and processed again (fxbench on=0 / on=1; reenable=0 shows the frozen state the
    verification heard). A +12 dB boost on a low band under a 98 Hz -12 dBFS note.
    Silent input at the return: nothing may come out (frozen state replayed before). Note held:
    no output step larger than the settled output's own (continuous with the dry signal before
    it), the first sample within -20 dB of the note's peak of the dry sample (a fade from silence
    or a replay reads near 0 dB), and no overshoot (+0.5 dB) in the first 100 ms."""
    sr = 48000
    cases = [("Parametric mid +12 @100 Hz", {"type": 1, "mid": 9, "mid_freq": 100, "mid_q": 0.7}),
             ("Parametric bass +12", {"type": 1, "bass": 8}),
             ("Parametric bass +20, mid +15 @100 Hz Q 8", {"type": 1, "bass": 10, "mid": 10, "mid_freq": 100, "mid_q": 8}),
             ("Graphic 100 Hz +12", {"type": 3, "g100": 12})]
    blocks = [37] if quick else [1, 37, 512]
    ok, rows = True, []
    t_on = 2.0031
    for block in blocks:
        for name, keys in cases:
            toks = [f"{k}={v}" for k, v in keys.items()] + ["on@1.0=0", f"on@{t_on}=1"]
            opts = [f"--block={block}", "--channels=1"]
            xa = fx.sine(98, -12, 3.0, sr)
            xa[int(1.2 * sr):] = 0
            n0 = int(np.ceil(t_on * sr / block)) * block
            frozen = float(np.max(np.abs(fx.run(EFFECT, xa, toks + ["reenable=0"], opts, sr)[0][0][n0:])))
            silent = float(np.max(np.abs(fx.run(EFFECT, xa, toks, opts, sr)[0][0][n0:])))
            xb = fx.sine(98, -12, 3.0, sr)
            xf = xb.astype(np.float32).astype(np.float64)
            y = fx.run(EFFECT, xb, toks, opts, sr)[0][0]
            ys = fx.run(EFFECT, xb, keys, opts, sr)[0][0]
            settled = slice(int(2.5 * sr), int(2.9 * sr))
            first = abs(y[n0] - xf[n0]) / np.max(np.abs(xf))
            step = float(np.max(np.abs(np.diff(y[n0 - 1:n0 + int(0.005 * sr)]))))
            steady_step = float(np.max(np.abs(np.diff(ys[settled]))))
            over = float(fx.db(np.max(np.abs(y[n0:n0 + int(0.1 * sr)])) / np.max(np.abs(ys[settled]))))
            good = silent == 0.0 and first <= 0.1 and step <= steady_step and over <= 0.5
            ok &= good
            rows.append(f"{name} blk {block}: silent return {fx.db(frozen + 1e-30):+.1f} dBFS frozen -> "
                        f"{'nothing' if silent == 0 else f'{fx.db(silent):+.1f} dBFS'}; held note first sample "
                        f"{fx.db(first + 1e-30):.0f} dB re dry, step {fx.db(step / steady_step):+.1f} dB re settled, "
                        f"peak {over:+.2f} dB" + ("" if good else " FAIL"))
    report.add("E23 re-enable after bypass", f"process, 1 s not called, reset, process; blocks {blocks}", ok,
               "; ".join(r for r in rows if "FAIL" in r or "blk 37" in r))
    for r in rows:
        measure("E23 re-enable", r.split(":")[0], r.split(": ", 1)[1])


def e24_retarget(report, quick):
    """Classic: a knob moved again while the previous ramp's last 16-sample segment straddles a
    block boundary. The engine used to snap the smoother to the new target while the filter
    held the old one, then jump to the new design in one step (code.md 8; -8 dBFS on a treble
    move under a low note). Over the same second moves at the same instants, a retarget must
    click no more than the move made from rest (first move 0.25 s earlier) +3 dB."""
    sr = 48000
    x = fx.sine(82.41, -12, 1.0, sr)

    def hf(y, a, b):
        y = fx.mono(y).astype(np.float64)
        n = len(y)
        spec = np.fft.rfft(y * np.hanning(n))
        spec[np.fft.rfftfreq(n, 1 / sr) < 1000.0] = 0
        return float(fx.db(np.max(np.abs(np.fft.irfft(spec, n)[a:b])) + 1e-30))

    jobs = []
    for band in ("treble", "mid"):
        k1 = int(0.3 * sr)
        jobs += [(band, 1, 10, 0, k1, k1 + d) for d in range(int(0.0665 * sr), int(0.0685 * sr), 2 if quick else 1)]
        for block in ([37] if quick else [37, 77, 100]):
            k1 = int(0.3 * sr / block)
            jobs += [(band, block, v1, 0 if v1 > 5 else 10, k1, k1 + d) for v1 in (10, 9, 8, 7, 0, 2)
                     for d in range(int(0.05 * sr / block), int(0.09 * sr / block) + 1)]

    def one(job):
        band, block, v1, v2, k1, k2 = job
        t2 = k2 * block / sr
        out = []
        for t1 in (k1 * block / sr, k1 * block / sr - 0.25):
            toks = ["type=0", f"{band}@{t1!r}={v1}", f"{band}@{t2!r}={v2}"]
            y = fx.run(EFFECT, x, toks, [f"--block={block}", "--channels=1"], sr)[0]
            out.append(hf(y, int(t2 * sr) - 200, int(t2 * sr) + 2400))
        return band, block, out[0], out[1]

    res = pmap(one, jobs)
    ok, rows = True, []
    for band in ("treble", "mid"):
        for block in sorted({j[1] for j in jobs}):
            r = [q for q in res if q[0] == band and q[1] == block]
            worst, rest = max(q[2] for q in r), max(q[3] for q in r)
            good = worst <= rest + 3.0
            ok &= good
            rows.append(f"{band} blk {block}: {worst:.1f} dBFS (from rest {rest:.1f}, {len(r)} moves)" + ("" if good else " FAIL"))
    report.add("E24 classic retarget", "move again in a ramp's last segment, treble/mid", ok, "; ".join(rows))


# ---------------------------------------------------------------------------- E25-E28 (second verification)

def guitar(f0, secs, sr, peak_db=-12.0, harmonics=12):
    """A guitar-like held note: 12 harmonics at 1/h^1.2 with scrambled phases (an asymmetric
    waveform), peak-normalised."""
    t = np.arange(int(round(secs * sr))) / sr
    x = sum(h ** -1.2 * np.sin(2 * np.pi * f0 * h * t + 0.7 * h * h) for h in range(1, harmonics + 1) if f0 * h < 0.45 * sr)
    return x * fx.amp(peak_db) / np.max(np.abs(x))


def hp_peak(y, sr, a, b, fc=1000.0):
    """Peak in samples a..b of y high-passed at fc: zero-phase, eighth-order Butterworth magnitude,
    applied in the frequency domain (the verification's click metric)."""
    y = fx.mono(y).astype(np.float64)
    f = np.fft.rfftfreq(len(y), 1 / sr)
    r = (np.maximum(f, 1e-9) / fc) ** 8
    yh = np.fft.irfft(np.fft.rfft(y) * (r / np.sqrt(1 + r * r)), len(y))
    return float(fx.db(np.max(np.abs(yh[a:b])) + 1e-30))


LOG_KEYS = ("mid_freq", "mid_q", "lowcut", "highcut")


def e25_cases(quick):
    """(row, name, static keys, moved key, from, to, note Hz)."""
    c = []
    for g in (10, 0):
        for qa, qb in ((8, 0.5), (0.5, 8)):
            c.append(("E25a", f"P mid Q {qa}->{qb} at knob {g}", {"type": 1, "mid": g, "mid_freq": 110}, "mid_q", qa, qb, 110))
    for q in (8, 0.7):
        for a, b in ((10, 0), (0, 10), (5, 10), (10, 5), (5, 0), (0, 5)):
            c.append(("E25a", f"P mid {a}->{b} Q {q}", {"type": 1, "mid_freq": 110, "mid_q": q}, "mid", a, b, 110))
        for a, b in ((220, 110), (110, 220), (8000, 110), (110, 8000)):
            for g in (10, 0):
                c.append(("E25b", f"P mid freq {a}->{b} Q {q} knob {g}", {"type": 1, "mid": g, "mid_q": q}, "mid_freq", a, b, 110))
    for a, b in ((0, 10), (10, 0)):
        c.append(("E25a", f"P bass {a}->{b}", {"type": 1}, "bass", a, b, 150))
        c.append(("E25a", f"P treble {a}->{b}", {"type": 1}, "treble", a, b, 1800))
    for a, b in ((20, 400), (400, 20), (400, 100), (100, 400)):
        c.append(("E25a", f"P low cut {a}->{b}", {"type": 1}, "lowcut", a, b, 110))
    for a, b in ((20000, 2000), (2000, 20000), (2000, 8000), (8000, 2000)):
        c.append(("E25a", f"P high cut {a}->{b}", {"type": 1}, "highcut", a, b, 2000))
    for a, b in ((-6, 6), (6, -6)):
        c.append(("E25a", f"P tilt {a}->{b} at 110 Hz", {"type": 1}, "tilt", a, b, 110))
        c.append(("E25a", f"P tilt {a}->{b} at 5 kHz", {"type": 1}, "tilt", a, b, 5000))
    for a, b in ((-12, 12), (12, -12)):
        c.append(("E25a", f"P level {a}->{b}", {"type": 1}, "level", a, b, 110))
        c.append(("E25a", f"G level {a}->{b}", {"type": 3, "g100": 12}, "level", a, b, 110))
        for k, f in zip(GRAPHIC_KEYS, GRAPHIC_HZ):
            c.append(("E25a", f"G {k} {a}->{b}", {"type": 3}, k, a, b, f))
    for v in range(4):
        for k in ("bass", "mid", "treble"):
            for a, b in ((0, 10), (10, 0)):
                for other in (0, 10):
                    for f in (41.2, 196.0):
                        c.append(("E25c", f"S {VOICE_NAMES[v]} {k} {a}->{b} others {other} at {f:g} Hz",
                                  {"type": 2, "voice": v, "bass": other, "mid": other, "treble": other}, k, a, b, f))
    if quick:
        c = c[::3]
    return c


def e25_overshoot(report, quick):
    """A control moved under a held note at the band's centre (a sine at -12 dBFS and a
    guitar-like note of the same peak): the output's peak during the move must not exceed the
    steady peak of any setting on the move's path (both ends and nine settings between, spaced in
    the control's smoothing domain) by more than 1 dB. Steps and ramps of 10 ms, 50 ms, 0.3 s;
    block 64; 48 kHz (96 kHz too for the Q and bell-gain moves). Verification (r2-tone N2): Q 8 to
    0.5 at +15 dB overshot +11.6 dB (+10.0 on the guitar note); the Stack, never tested, +15.5 dB."""
    t_move, settle = 0.6, 0.9
    durs = [0.0, 0.01, 0.05, 0.3]
    cases = e25_cases(quick)

    def path(key, a, b):
        if key in LOG_KEYS:
            return list(np.exp2(np.linspace(np.log2(a), np.log2(b), 11)))
        return list(np.linspace(a, b, 11))

    def signal(kind, f0, secs, sr):
        return fx.sine(f0, -12.0, secs, sr) if kind == "sine" else guitar(f0, secs, sr)

    def steady(job):
        (_, _, base, key, _, _, f0), value, kind, sr = job
        secs = 1.2
        y = fx.run(EFFECT, signal(kind, f0, secs, sr), [f"{k}={v}" for k, v in base.items()] + [f"{key}={float(value)!r}"],
                   ["--block=64", "--channels=1"], sr)[0][0]
        return float(np.max(np.abs(y[-int(0.3 * sr):])))

    def moved(job):
        (_, _, base, key, a, b, f0), dur, kind, sr = job
        secs = t_move + dur + settle
        toks = [f"{k}={v}" for k, v in base.items()] + [f"{key}={a}",
                                                         f"{key}@{t_move}={b}" if dur == 0 else f"{key}@{t_move}..{t_move + dur}={a}..{b}"]
        y = fx.run(EFFECT, signal(kind, f0, secs, sr), toks, ["--block=64", "--channels=1"], sr)[0][0]
        return float(np.max(np.abs(y[int(t_move * sr):len(y) - int(0.3 * sr)])))

    jobs = [(c, kind, sr) for c in cases for kind in ("sine", "guitar")
            for sr in ([48000, 96000] if c[3] in ("mid_q", "mid") and not quick else [48000])]
    statics = pmap(steady, [(c, v, kind, sr) for c, kind, sr in jobs for v in path(c[3], c[4], c[5])])
    bound = {}
    for i, (c, kind, sr) in enumerate(jobs):
        bound[(c[1], kind, sr)] = max(statics[11 * i:11 * i + 11])
    moves = [(c, d, kind, sr) for c, kind, sr in jobs for d in durs]
    peaks = pmap(moved, moves)
    worst = {}
    for (c, d, kind, sr), p in zip(moves, peaks):
        over = float(fx.db(p / max(bound[(c[1], kind, sr)], 1e-12)))
        row = c[0]
        if over > worst.get(row, (-1e9,))[0]:
            worst[row] = (over, f"{c[1]}, {'step' if d == 0 else f'{d:g} s'}, {kind}, {sr // 1000}k")
        worst.setdefault(row + "n", [0, 0])
        worst[row + "n"][0] += 1
        worst[row + "n"][1] += over > 1.0
    scopes = {"E25a": "Parametric gain, Q, shelves, cuts, tilt, level; Graphic",
              "E25b": "Parametric mid-frequency moves", "E25c": "Stack, every knob and voice"}
    for row in ("E25a", "E25b", "E25c"):
        if row not in worst:
            continue
        over, where = worst[row]
        n, bad = worst[row + "n"]
        report.add(f"{row} move overshoot", scopes[row], over <= 1.0,
                   f"worst {over:+.2f} dB over the path's steady peak ({where}); {bad} of {n} moves above +1 dB (limit +1)")


def e26_classic_moves(report, quick):
    """Classic (and the amp tone stack): bass, mid and treble moved under a low E (82.41 Hz,
    -12 dBFS), steps of every size across 5 and to and from it, ramps of 10 ms to 1 s; the
    output's peak above 1 kHz (the verification's eighth-order high-pass) from 50 ms before the
    move to 1.5 s, at 44.1 / 48 / 96 / 192 kHz and blocks 1 / 37 / 64 / 512. Verification
    (r2-tone N3, N9): mid 10 to 0 read -59.9 dBFS at 48 kHz and -51.7 at 192 kHz (the frozen
    design switches Q at 5); bass 5 to 10 -79.6 at 192 kHz; the bound for knob moves is -80."""
    rates = [48000, 192000] if quick else [44100, 48000, 96000, 192000]
    blocks = [37, 512] if quick else [1, 37, 64, 512]
    moves = []
    for k in ("bass", "mid", "treble"):
        for a, b in ((10, 0), (0, 10), (8, 2), (2, 8), (6, 4), (5.5, 4.5), (5.1, 4.9), (5, 10), (5, 0), (10, 5), (0, 5)):
            moves.append((f"{k} {a}->{b} step", [f"{k}={a}", f"{k}@0.5={b}"]))
        for d in (0.01, 0.05, 0.3, 1.0):
            moves.append((f"{k} 10->0 in {d:g} s", [f"{k}=10", f"{k}@0.5..{0.5 + d}=10..0"]))
            moves.append((f"{k} 0->10 in {d:g} s", [f"{k}=0", f"{k}@0.5..{0.5 + d}=0..10"]))
    if quick:
        moves = moves[::3]

    def one(job):
        (name, toks), sr, block = job
        x = fx.sine(82.41, -12, 2.2, sr)
        y = fx.run(EFFECT, x, ["type=0"] + toks, [f"--block={block}", "--channels=1"], sr)[0]
        return hp_peak(y, sr, int(0.45 * sr), int(2.0 * sr))

    jobs = [(m, sr, bs) for m in moves for sr in rates for bs in blocks]
    peaks = pmap(one, jobs)
    worst = max(zip(peaks, jobs), key=lambda r: r[0])
    by_knob = {k: max(p for p, j in zip(peaks, jobs) if j[0][0].startswith(k)) for k in ("bass", "mid", "treble")}
    report.add("E26 classic knob moves", f"{len(moves)} moves x {len(rates)} rates x blocks {blocks}", worst[0] <= -80.0,
               f"worst {worst[0]:.1f} dBFS above 1 kHz ({worst[1][0][0]}, {worst[1][1]} Hz, block {worst[1][2]}); "
               + ", ".join(f"{k} {v:.1f}" for k, v in by_knob.items()) + " (limit -80)")


# Strong settings for the switch rows; every type reads its own keys out of the same set.
E27_SETS = {
    "low boost": {"bass": 8, "mid": 10, "mid_freq": 100, "mid_q": 0.7, "treble": 5, "g100": 12, "g200": 12},
    "high cut": {"bass": 5, "mid": 5, "treble": 0, "highcut": 2000, "g3k2": -12, "g6k4": -12},
    "extremes": {"bass": 10, "mid": 0, "treble": 10, "mid_freq": 300, "mid_q": 4, "lowcut": 80, "tilt": 3, "level": 3,
                 "g100": 12, "g200": -12, "g400": 12, "g800": -12, "g1k6": 12, "g3k2": -12, "g6k4": 12},
}


def e27_switch_quality(report, quick):
    """Every ordered pair of the four types, and of the four Stack voices, switched at 1 s under
    strong settings. Click: the peak above 1 kHz (eighth-order high-pass) under a 98 Hz -12 dBFS
    note, from 10 ms before the switch to 250 ms after. Level: the 5 ms RMS of a guitar-like
    110 Hz note during the switch against both types' own steady ranges (before the switch, and
    from 0.5 s after it): never more than 1 dB below the lower or above the higher. 44.1, 48,
    96, 192 kHz, block 64 (37 and 512 at 48 kHz). Verification (r2-tone N4): Parametric to
    Graphic under a +15 dB low boost read -54.8 dBFS at 96 kHz and dipped 9.1 dB."""
    t_sw = 1.0
    rates = [48000, 192000] if quick else [44100, 48000, 96000, 192000]
    pairs = [((a, None), (b, None)) for a in range(4) for b in range(4) if a != b]
    pairs += [((2, va), (2, vb)) for va in range(4) for vb in range(4) if va != vb]
    jobs = []
    for sr in rates:
        for block in ([37, 64, 512] if sr == 48000 and not quick else [64]):
            for name, keys in E27_SETS.items():
                for (a, va), (b, vb) in pairs:
                    if va is not None and name != "low boost":
                        continue
                    jobs.append((sr, block, name, keys, a, va, b, vb))
    if quick:
        jobs = jobs[::4]

    def env(y, sr):
        n = int(0.005 * sr)
        c = np.concatenate([[0.0], np.cumsum(y * y)])
        return np.sqrt(np.maximum(c[n:] - c[:-n], 0.0) / n)

    def one(job):
        sr, block, name, keys, a, va, b, vb = job
        toks = [f"{k}={v}" for k, v in keys.items()] + [f"type={a}", f"type@{t_sw}={b}"]
        if va is not None:
            toks += [f"voice={va}", f"voice@{t_sw}={vb}"]
        opts = [f"--block={block}", "--channels=1"]
        y = fx.run(EFFECT, fx.sine(98.0, -12.0, 2.0, sr), toks, opts, sr)[0][0]
        click = hp_peak(y, sr, int((t_sw - 0.01) * sr), int((t_sw + 0.25) * sr))
        g = fx.run(EFFECT, guitar(110.0, 2.2, sr), toks, opts, sr)[0][0]
        e = env(g, sr)
        t = np.arange(len(e)) / sr
        before, after = e[(t > 0.5) & (t < t_sw - 0.01)], e[t > t_sw + 0.5]
        during = e[(t >= t_sw - 0.005) & (t <= t_sw + 0.5)]
        dip = float(fx.db(during.min() / min(before.min(), after.min())))
        bump = float(fx.db(during.max() / max(before.max(), after.max())))
        label = f"{a}{'' if va is None else f'v{va}'}->{b}{'' if vb is None else f'v{vb}'} {name} {sr // 1000}k/{block}"
        return click, dip, bump, label

    res = pmap(one, jobs)
    wc = max(res, key=lambda r: r[0])
    wd = min(res, key=lambda r: r[1])
    wb = max(res, key=lambda r: r[2])
    ok = wc[0] <= -75.0 and wd[1] >= -1.0 and wb[2] <= 1.0
    report.add("E27 switch click/level", f"{len(jobs)} type/voice switches, strong settings", ok,
               f"click worst {wc[0]:.1f} dBFS ({wc[3]}; limit -75); level dip {wd[1]:+.2f} dB ({wd[3]}), "
               f"bump {wb[2]:+.2f} dB ({wb[3]}) (limits +-1 dB outside both types' own ranges)")


def e28_absurd_input(report, quick):
    """One finite sample no instrument produces (1e4, 1e10, 1e30, 3e38) in a 220 Hz -12 dBFS
    sine: Classic (and every type) must give, bit for bit, the render with that sample at 0, so
    the output never passes +12 dBFS. One sample of 999 (+59.99 dBFS) must still be processed
    as the old code did (--legacy, Classic 10/5/7). Verification (r2-tone N11): 1e30 kept
    Classic above +12 dBFS for 173 ms."""
    sr = 48000
    at = sr // 2
    cases = [("Classic 10/5/7", {"type": 0, "bass": 10, "treble": 7}), ("Classic 0/0/0", {"type": 0, "bass": 0, "mid": 0, "treble": 0}),
             ("Parametric", {"type": 1, "bass": 8, "mid": 10, "mid_freq": 100}), ("Stack", {"type": 2, "bass": 10}),
             ("Graphic", {"type": 3, "g100": 12})]
    jobs = [(name, keys, big, bs) for name, keys in cases for big in (1e4, 1e10, 1e30, 3e38) for bs in ([37] if quick else [1, 37, 512])]

    def one(job):
        name, keys, big, bs = job
        x = 0.25 * np.sin(2 * np.pi * 220 * np.arange(sr) / sr)
        clean = x.copy()
        clean[at] = 0.0
        x[at] = big
        opts = [f"--block={bs}", "--channels=1"]
        y = fx.run(EFFECT, x, keys, opts, sr)[0][0]
        yc = fx.run(EFFECT, clean, keys, opts, sr)[0][0]
        same = bool(np.array_equal(y.view(np.uint64), yc.view(np.uint64)))
        loud = float(np.max(np.abs(np.where(np.isfinite(y), y, np.inf))))
        return same and loud < 4.0, f"{name} {big:g} block {bs}: {'equal' if same else 'differs'}, peak {fx.db(loud):+.1f} dBFS"

    res = pmap(one, jobs)
    bad = [d for good, d in res if not good]
    x = 0.25 * np.sin(2 * np.pi * 220 * np.arange(sr) / sr)
    x[at] = 999.0
    keys = {"type": 0, "bass": 10, "treble": 7}
    y = fx.run(EFFECT, x, keys, ["--channels=1"], sr)[0][0]
    old = fx.run(EFFECT, x, keys, ["--channels=1", "--legacy"], sr)[0][0]
    kept = bool(np.array_equal(y.view(np.uint64), old.view(np.uint64)))
    report.add("E28 absurd input", f"one sample of 1e4..3e38, {len(cases)} settings x blocks", not bad and kept,
               ("; ".join(bad[:4]) + "; " if bad else f"{len(jobs)} renders equal the render with the sample at 0; ")
               + f"one sample of 999 processed as the old code: {kept}")


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
    if want("E20"):
        e20_bell_map(report, quick)
    if want("E21"):
        e21_non_finite(report, quick)
    if want("E22"):
        e22_neutral(report, quick)
    if want("E23"):
        e23_reenable(report, quick)
    if want("E24"):
        e24_retarget(report, quick)
    if want("E25"):
        e25_overshoot(report, quick)
    if want("E26"):
        e26_classic_moves(report, quick)
    if want("E27"):
        e27_switch_quality(report, quick)
    if want("E28"):
        e28_absurd_input(report, quick)
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
