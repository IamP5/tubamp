#!/usr/bin/env python3
"""Acceptance for the comp engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.2.9). Prints a PASS/FAIL table; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_comp.py [--quick] [--types 1,2] [--rows C3,C10] [--no-common]
                                           [--no-tables]

Beyond §4.2.9: C21 is the broadband click metric under automation of every control (the
old line-at-k*fs/block test counted the modulation's own sidebands; C21b isolates the block
delivery), C22 level/settles/click for type switches, C24 NaN and infinity, C25 the output
bound, C26/C27 what Squash's release and knee knobs promise, C28 FET/Opto programme-dependent
release, and the K rows: every control each type reads, moved while the block runs.

After the table it prints three information tables (not pass/fail): the low-frequency
residual split into harmonic distortion and inharmonic sidebands for every type at its
starting point and at the fastest settings, 41.2/82.4/98/110 Hz at 44.1 and 48 kHz;
attack and release per type; and the Classic crossfade timing at 44.1 kHz.
"""
import argparse
import sys
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxcheck as fx  # noqa: E402

EFFECT = "comp"
TYPE_KEY = "type"
TYPES = {0: "Classic", 1: "Clean", 2: "Squash", 3: "FET", 4: "Opto"}
NEW_TYPES = {k: v for k, v in TYPES.items() if k != 0}
# Float keys at their documented limits, for the finite-output test.
RANGES = {"threshold": (-60.0, 0.0), "ratio": (1.0, 20.0), "attack": (0.1, 100.0), "release": (10.0, 1000.0),
          "makeup": (0.0, 24.0), "knee": (0.0, 24.0), "mix": (0.0, 100.0), "sc_hpf": (20.0, 300.0)}
# Percent of real time per instance (§2.10, §4.2.7): Classic as today (<= 0.3 %), new types 0.35 %.
BUDGETS = {0: 0.3, 1: 0.35, 2: 0.35, 3: 0.35, 4: 0.35}
VARIANTS = {}
# Classic null at the defaults and at settings that compress hard with makeup.
NULL_KEYS = [{}, {"threshold": -40.0, "ratio": 12.0, "attack": 0.3, "release": 30.0, "makeup": 9.0}]

# Per-type starting points (src/dsp/Compressor.h kCompKneeDefault / Attack / Release).
START = {0: dict(knee=6.0, attack=5.0, release=120.0), 1: dict(knee=6.0, attack=5.0, release=120.0),
         2: dict(knee=6.0, attack=2.5, release=150.0), 3: dict(knee=4.0, attack=0.8, release=55.0),
         4: dict(knee=12.0, attack=10.0, release=60.0)}
FASTEST = dict(attack=0.1, release=10.0)
LF_FREQS = [41.2, 82.4, 98.0, 110.0]
SR = 48000


# ---------------------------------------------------------------------------- helpers

def keys_for(t, **kw):
    k = {TYPE_KEY: t, "threshold": -20.0, "ratio": 4.0}
    k.update(kw)
    return k


def render(keys, x, sr=SR, opts=()):
    """-> (out mono, gain-reduction trace in dB >= 0)."""
    out, trace = fx.run(EFFECT, x, keys, ["--trace=gain"] + list(opts), sr)
    return fx.mono(out), fx.mono(trace)


def rms_db(y):
    return float(fx.db(np.sqrt(np.mean(np.square(y)))))


def static_gr(t, level_db, sr=SR, freq=1000.0, secs=1.0, **kw):
    """Gain reduction (dB) of a steady tone from the output/input level ratio."""
    x = fx.sine(freq, level_db, secs, sr)
    y, _ = render(keys_for(t, **kw), x, sr)
    tail = slice(int(0.5 * len(x)), None)
    return rms_db(x[tail]) - rms_db(y[tail])


def burst_times(t, level_db, sr=SR, post=1.5, **kw):
    """1 kHz burst, 0.5 s, from and to silence. -> dict with attack t63, release t50/t63/t90
    (ms) and the trace, all on the gain-reduction trace."""
    pre = 0.2
    x, end = fx.burst(1000.0, level_db, 0.5, sr, pre=pre, post=post)
    _, tr = render(keys_for(t, **kw), x, sr)
    i0 = int(round(pre * sr))
    settled = float(np.mean(tr[end - int(0.012 * sr):end - int(0.002 * sr)]))
    att = fx.settle_times(tr, sr, pre, (0.63,), final=settled)[0.63]
    rel = fx.settle_times(tr, sr, end / sr, (0.5, 0.63, 0.9), final=0.0)
    ms = lambda v: None if v is None else 1e3 * v  # noqa: E731
    return dict(att=ms(att), r50=ms(rel[0.5]), r63=ms(rel[0.63]), r90=ms(rel[0.9]), settled=settled,
                peak=float(np.max(tr[i0:end])), trace=tr, start=i0, end=end)


def residual(t, freq, sr=SR, secs=2.5, level_db=-10.0, **kw):
    """§4.2.9 residual: everything but DC + the fundamental, LS fit over the last 1 s (%)."""
    x = fx.sine(freq, level_db, secs, sr)
    y, _ = render(keys_for(t, **kw), x, sr)
    return fx.fundamental_residual(y, sr, freq, start=secs - 1.0)


def blackman_harris(n):
    k = np.arange(n) / n
    return (0.35875 - 0.48829 * np.cos(2 * np.pi * k) + 0.14128 * np.cos(4 * np.pi * k)
            - 0.01168 * np.cos(6 * np.pi * k))


def split_distortion(y, sr, f0, secs=2.0, lobe=6):
    """Splits a steady tone's residual (output minus its least-squares DC + fundamental, over
    the last 2 s) into harmonic distortion (k * f0, k >= 2, % of the fundamental) and
    everything else: the inharmonic sidebands a modulated gain produces, including those
    within a few Hz of the carrier (total, and the largest single line, dBc). Removing the
    fitted fundamental first keeps its window leakage out of the sideband figure.
    4-term Blackman-Harris, 0.5 Hz bins."""
    seg = fx.mono(y)[-int(secs * sr):]
    t = np.arange(len(seg)) / sr
    basis = np.vstack([np.ones_like(t), np.cos(2 * np.pi * f0 * t), np.sin(2 * np.pi * f0 * t)]).T
    coef, *_ = np.linalg.lstsq(basis, seg, rcond=None)
    win = blackman_harris(len(seg))
    fund = np.sum(win ** 2) * (coef[1] ** 2 + coef[2] ** 2) / 2.0   # Parseval: windowed sine power
    spec = np.abs(np.fft.rfft((seg - basis @ coef) * win)) ** 2 * 2.0 / len(seg)
    df = sr / len(seg)
    used = np.zeros(len(spec), bool)
    used[:lobe + 1] = True
    harm = 0.0
    k = 2
    while k * f0 < sr / 2 - lobe * df:
        i = int(round(k * f0 / df))
        lo, hi = max(0, i - lobe), min(len(spec), i + lobe + 1)
        harm += spec[lo:hi][~used[lo:hi]].sum()
        used[lo:hi] = True
        k += 1
    rest = np.where(used, 0.0, spec)
    peak = int(np.argmax(rest))
    worst = rest[max(0, peak - lobe):peak + lobe + 1].sum()
    to_db = lambda p: float(10 * np.log10(max(p, 1e-30) / max(fund, 1e-30)))  # noqa: E731
    return dict(harm_pct=100.0 * np.sqrt(harm / max(fund, 1e-30)), side_db=to_db(rest.sum()),
                worst_db=to_db(worst), worst_hz=peak * df)


def envelope_db(y):
    return fx.db(np.maximum(np.abs(fx.analytic(y)), 1e-12))


def block_start_at(t_s, sr, block):
    """First block start at or after t_s: where a timeline step lands."""
    i = int(np.ceil(round(t_s * sr, 6) / block)) * block
    return i


def fmt(v, spec=".2f"):
    return "n/a" if v is None else format(v, spec)


# ---------------------------------------------------------------------------- C rows

def c2(report):
    # Classic meter against the measured peak envelope ratio, block by block, in steady state.
    sr, block = SR, 512
    x = fx.sine(1000.0, -10.0, 2.0, sr)
    r = fx.invoke(EFFECT, x, keys_for(0, attack=5.0, release=120.0), [f"--block={block}", "--meters"], sr)
    y = fx.mono(r.out)
    meter = np.array([float(row.split("\t")[1]) for row in r.meters])
    worst = 0.0
    for b in range(int(1.0 * sr / block), len(meter) - 1):
        a, e = b * block, (b + 1) * block
        measured = -fx.db(np.max(np.abs(y[a:e])) / np.max(np.abs(x[a:e])))
        worst = max(worst, abs(meter[b] - measured))
    report.add("C2 Classic meter", "Classic -10 dBFS 1 kHz", worst <= 0.1,
               f"meter {np.mean(meter[-20:]):.3f} dB, worst block |meter - envelope ratio| {worst:.3f} dB (<= 0.1)")


def c3(report, sr=SR, tag=""):
    gr = static_gr(1, -10.0, sr)
    lo = [static_gr(1, v, sr) for v in (-15.0, -12.0)]
    hi = [static_gr(1, v, sr) for v in (-3.0, 0.0)]
    near = 3.0 / (3.0 - (lo[1] - lo[0]))
    top = 3.0 / (3.0 - (hi[1] - hi[0]))
    ok = abs(gr - 7.5) <= 0.3 and abs(near - 4.0) <= 0.1 and abs(top - 4.0) <= 0.1
    report.add("C3 Clean static" + tag, f"Clean {sr} Hz", ok,
               f"GR at -10 dBFS {gr:.3f} dB (7.5 +-0.3); slope T+5..T+8 {near:.3f}, top -3..0 {top:.3f} (4.0 +-0.1)")


def c3_link_meter(report):
    # Linked stereo: a loud left and a quiet right get the same gain. Meter: the GR the
    # block reports equals the measured reduction (study R2 proof: 7.5 dB +-0.3).
    x = fx.sine(1000.0, -10.0, 1.0, SR)
    inp = np.vstack([x, fx.amp(-20.0) * x])
    worst_link, worst_meter, where = 0.0, 0.0, ""
    for t in NEW_TYPES:
        r = fx.invoke(EFFECT, inp, keys_for(t), ["--meters"], SR)
        tail = slice(SR // 2, None)
        gl = rms_db(inp[0, tail]) - rms_db(r.out[0, tail])
        gr = rms_db(inp[1, tail]) - rms_db(r.out[1, tail])
        meter = float(np.mean([float(row.split("\t")[1]) for row in r.meters[-20:]]))
        worst_link = max(worst_link, abs(gl - gr))
        if abs(meter - 7.5) > worst_meter:
            worst_meter, where = abs(meter - 7.5), f"{TYPES[t]} {meter:.3f} dB"
    report.add("C3b linked stereo", "new types, L -10 / R -30 dBFS", worst_link <= 0.01,
               f"worst |GR left - GR right| {worst_link:.4f} dB (<= 0.01)")
    report.add("C3c GR meter", "new types, -10 dBFS, T -20 R 4", worst_meter <= 0.3,
               f"worst |meter - 7.5| {worst_meter:.3f} dB ({where}) (<= 0.3)")


def c4(report, sr=SR, tag=""):
    t = burst_times(1, -10.0, sr, attack=5.0, release=120.0)
    report.add("C4 Clean attack" + tag, f"Clean 5 ms {sr} Hz", t["att"] is not None and abs(t["att"] - 5.0) <= 1.0,
               f"t63 {fmt(t['att'])} ms (5.0 +-1.0)")


def c5(report, sr=SR, tag=""):
    a = burst_times(1, -10.0, sr, attack=5.0, release=120.0)
    b = burst_times(1, -15.0, sr, attack=5.0, release=120.0)
    ok = all(v is not None for v in (a["r63"], b["r63"])) and abs(a["r63"] - 120) <= 6 and abs(b["r63"] - 120) <= 6 \
        and abs(a["r63"] - b["r63"]) <= 3
    report.add("C5 Clean release" + tag, f"Clean 120 ms {sr} Hz", ok,
               f"t63 {fmt(a['r63'])} ms (-10 dBFS), {fmt(b['r63'])} ms (-15 dBFS); 120 +-6, within 3 ms")


def residual_row(report, name, scope, t, freqs, limits, sr=SR, **kw):
    vals = [residual(t, f, sr, **kw) for f in freqs]
    ok = all(v <= lim for v, lim in zip(vals, limits))
    report.add(name, scope, ok, ", ".join(f"{f:g} Hz {v:.4f} % (<= {lim:g})" for f, v, lim in zip(freqs, vals, limits)))
    return vals


def c6(report, sr=SR, tag=""):
    residual_row(report, "C6 Clean residual" + tag, f"Clean 5/120 {sr} Hz", 1, [41.2, 82.4, 98.0, 100.0], [0.05] * 4, sr,
                 attack=5.0, release=120.0)


def c7(report):
    for att, rel in ((30.0, 1000.0), (1.0, 50.0)):
        residual_row(report, "C7 Clean residual", f"Clean {att:g}/{rel:g}", 1, [41.2, 82.4, 98.0, 100.0], [0.05] * 4,
                     attack=att, release=rel)


def c8(report):
    residual_row(report, "C8 Clean residual fast", "Clean 0.1/10", 1, [41.2, 82.4, 98.0], [6.0, 2.0, 1.2],
                 attack=0.1, release=10.0)


def c9(report):
    residual_row(report, "C9 Clean 1 kHz", "Clean 5/120", 1, [1000.0], [0.01], attack=5.0, release=120.0)


def c10(report, sr=SR, tag=""):
    worst_under, fails = 0.0, []
    for ratio in (2.0, 4.0, 8.0, 12.0, 20.0):
        for att in (0.1, 2.5, 20.0):
            t = burst_times(2, -10.0, sr, post=0.5, threshold=-34.0, knee=6.0, ratio=ratio, attack=att, release=150.0)
            under = t["peak"] - t["settled"]  # GR above its settled value = gain below settled
            worst_under = max(worst_under, under)
            if under > 0.5 or t["att"] is None or abs(t["att"] - att) > max(0.1 * att, 0.12):
                fails.append(f"R {ratio:g} att {att:g}: undershoot {under:.3f} dB, t63 {fmt(t['att'], '.3f')} ms")
            print(f"        C10 {sr} Hz R {ratio:g} att {att:g}: undershoot {under:.3f} dB, t63 {fmt(t['att'], '.3f')} ms",
                  flush=True)
    report.add("C10 Squash onset" + tag, f"Squash T-34 15 settings {sr} Hz", not fails,
               f"worst undershoot {worst_under:.3f} dB (<= 0.5); attack t63 = knob +-max(10 %, 0.12 ms)"
               + ("; FAIL " + "; ".join(fails) if fails else ""))


def c10_grid(report):
    # Beyond the table: the feedback loop at every ratio, attack and knee extreme, at the
    # lowest and highest rate. A -40 -> 0 dBFS step and back to -20 dBFS (release inside
    # the loop, still above threshold): the gain must move monotonically each way. The
    # tones are sample-locked (21 and 192 samples per period) so sampled crest heights do
    # not beat and the loop's own dynamics are what is measured.
    worst, where = 0.0, ""
    for sr, freq in ((22050, 1050.0), (192000, 1000.0)):
        # Each segment lasts ten of the slowest attack's time constants, so both settle.
        x = np.concatenate([fx.step(sr, freq, -40.0, 0.0, 0.1, 1.1), fx.amp(-20.0) * np.sin(
            2 * np.pi * freq * (np.arange(int(1.0 * sr)) / sr + 1.1))])
        i_up, i_down = int(0.1 * sr), int(1.1 * sr)
        for ratio in (1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0, 20.0):
            for att in (0.1, 0.3, 1.0, 3.0, 10.0, 30.0, 100.0):
                for knee in (0.0, 6.0, 24.0):
                    _, tr = render(keys_for(2, threshold=-34.0, ratio=ratio, attack=att, release=10.0, knee=knee), x, sr)
                    up = tr[i_up:i_down]
                    down = tr[i_down:]
                    bad = max(float(np.max(up) - up[-1]), float(np.max(np.maximum.accumulate(down[::-1])[::-1] - down)))
                    if bad > worst:
                        worst, where = bad, f"{sr} Hz R {ratio:g} att {att:g} knee {knee:g}"
    report.add("C10+ Squash stability", "R 1-20 x att 0.1-100 x knee 0/6/24", worst <= 0.05,
               f"largest overshoot past the settled gain {worst:.4f} dB at {where} (<= 0.05)")


def c11(report):
    t = burst_times(2, -10.0, post=3.0, attack=2.5, release=600.0)
    report.add("C11 Squash release", "Squash 600 ms", t["r63"] is not None and abs(t["r63"] - 600) <= 30,
               f"t63 {fmt(t['r63'])} ms (600 +-30)")


def c12(report):
    kw = dict(threshold=-34.0, ratio=8.0, knee=6.0, attack=2.5, release=150.0)
    a, b = static_gr(2, -6.0, **kw), static_gr(2, 0.0, **kw)
    slope = 6.0 / (6.0 - (b - a))
    report.add("C12 Squash slope", "Squash R 8, T -34, -6..0 dBFS", 7.9 <= slope <= 8.1, f"slope {slope:.3f} (7.9-8.1)")


def c13(report):
    residual_row(report, "C13 Squash residual", "Squash R 8 2.5/600 T -34", 2, [41.2, 82.4, 98.0], [0.05] * 3,
                 threshold=-34.0, ratio=8.0, attack=2.5, release=600.0)


def two_stage(report, name, t, bands, **kw):
    r = burst_times(t, -10.0, post=2.5, **kw)
    vals = (r["r50"], r["r63"], r["r90"])
    ok = all(v is not None and lo <= v <= hi for v, (lo, hi) in zip(vals, bands))
    report.add(name, TYPES[t] + " " + " ".join(f"{k}={v:g}" for k, v in kw.items()), ok,
               "t50/t63/t90 " + " / ".join(fmt(v, ".1f") for v in vals) + " ms (bands "
               + " / ".join(f"{lo}-{hi}" for lo, hi in bands) + ")")


def c14(report):
    two_stage(report, "C14 FET release", 3, [(60, 70), (90, 105), (370, 420)],
              threshold=-24.0, ratio=6.0, knee=4.0, attack=0.3, release=55.0)


def c15(report):
    two_stage(report, "C15 Opto release", 4, [(80, 95), (130, 150), (650, 750)],
              threshold=-24.0, ratio=4.0, knee=12.0, attack=2.0, release=60.0)


def c16(report):
    for t in (3, 4):
        residual_row(report, "C16 residual", f"{TYPES[t]} starting point", t, [41.2, 82.4, 98.0], [0.05] * 3, **START[t])


def c17(report):
    # Level ramp -30 -> 0 dBFS over 6 s (5 dB/s): the GR trace follows the static curve
    # within ~0.01 dB at the FET starting attack; the -1 dB point is read off it.
    sr, secs = SR, 6.0
    tt = np.arange(int(secs * sr)) / sr
    level = -30.0 + 30.0 * tt / secs
    x = fx.amp(level) * np.sin(2 * np.pi * 1000.0 * tt)
    points = {}
    for ratio in (4.0, 8.0, 16.0):
        _, tr = render(keys_for(3, ratio=ratio, **START[3]), x, sr)
        i = int(np.argmax(tr >= 1.0))
        points[ratio] = level[i] if tr[i] >= 1.0 else None
    if None in points.values():
        report.add("C17 FET threshold", "FET", False, f"-1 dB point not reached: {points}")
        return
    d1, d2 = points[8.0] - points[4.0], points[16.0] - points[8.0]
    ok = abs(d1 - 2.6) <= 0.3 and abs(d2 - 2.6) <= 0.3
    report.add("C17 FET threshold", "FET R 4/8/16 knee 4", ok,
               f"-1 dB points {points[4.0]:.2f} / {points[8.0]:.2f} / {points[16.0]:.2f} dBFS; "
               f"per doubling {d1:.2f}, {d2:.2f} (2.6 +-0.3)")


def c18(report):
    x = fx.music(SR)
    worst = -300.0
    for t in NEW_TYPES:
        wet = fx.run(EFFECT, x, keys_for(t, mix=100.0), [], SR)[0]
        half = fx.run(EFFECT, x, keys_for(t, mix=50.0), [], SR)[0]
        err = half - 0.5 * (x + wet)
        worst = max(worst, float(fx.db(np.sqrt(np.mean(err ** 2)) / np.sqrt(np.mean(half ** 2)))))
    report.add("C18 mix 50 %", "all new types, gen:music", worst <= -80.0, f"worst error {worst:.1f} dB re output (<= -80)")


def c19(report):
    lo = static_gr(1, -10.0, freq=40.0, secs=2.0, sc_hpf=100.0)
    hi = static_gr(1, -10.0, freq=1000.0, secs=2.0, sc_hpf=100.0)
    report.add("C19 SC HPF", "Clean, HPF 100 Hz", hi - lo >= 5.0,
               f"GR 40 Hz {lo:.2f} dB, 1 kHz {hi:.2f} dB, difference {hi - lo:.2f} dB (>= 5)")


def c20(report):
    worst, where = 0.0, ""
    for t in NEW_TYPES:
        for ratio in (2.0, 4.0, 8.0, 20.0):
            d = abs(static_gr(t, -10.0, ratio=ratio, knee=6.0, auto_makeup=1))
            if d > worst:
                worst, where = d, f"{TYPES[t]} R {ratio:g}"
    report.add("C20 auto makeup", "new types, R 2/4/8/20, knee 6", worst <= 0.5,
               f"worst |out - in| {worst:.3f} dB at {where} (<= 0.5)")


def hf_click(y, sr, carrier_hz, t0=0.3):
    """Broadband click metric (verifier build-verify/dynamics/cf.py): the loudest 21 ms frame of
    energy more than 3 kHz above the carrier, dB re the carrier. A gain that moves on a
    millisecond scale or slower puts nothing there, so a modulation's own sidebands (within
    a few hundred Hz of the carrier) do not count; a step in the gain does."""
    n, hop = 1024, 256
    y = fx.mono(y)[int(t0 * sr):]
    frames = np.lib.stride_tricks.sliding_window_view(y, n)[::hop] * np.blackman(n)
    spec = np.abs(np.fft.rfft(frames, axis=1)) ** 2
    f = np.fft.rfftfreq(n, 1.0 / sr)
    carrier = np.max(spec[:, (f > carrier_hz - 100) & (f < carrier_hz + 100)].sum(axis=1))
    hf = spec[:, f > carrier_hz + 3000].sum(axis=1)
    return float(10 * np.log10(max(hf.max(), 1e-300) / max(carrier, 1e-300)))


# Each control between its extremes (threshold -40 <-> -10: the old C21's own move).
AUTOMATION = [("threshold", -40.0, -10.0), ("ratio", 1.0, 20.0), ("attack", 0.1, 100.0), ("release", 10.0, 1000.0),
              ("knee", 0.0, 24.0), ("mix", 0.0, 100.0), ("makeup", 0.0, 24.0), ("sc_hpf", 20.0, 300.0),
              ("auto_makeup", 0, 1)]


def square_tokens(key, lo, hi, secs, rate=10.0, start=0.3):
    toks, t, k = [], start, 0
    while t < secs - 0.05:
        toks.append(f"{key}@{t:.6f}={hi if k % 2 == 0 else lo}")
        t, k = t + 0.5 / rate, k + 1
    return toks


def c21(report):
    # Replaces the old C21, whose lines at k * fs / block were the 10 Hz modulation's own
    # sidebands (C21b proves the per-block delivery adds nothing there). Here: every control
    # stepped between its extremes as a 10 Hz square wave delivered per block, like host
    # automation; (a) on a 1 kHz tone, the broadband click metric stays under -70 dBc;
    # (b) on an 82.4 Hz bass note, where the fastest settings legitimately move the gain in a
    # millisecond, the block render adds no more than 3 dB to the same automation rendered
    # sample-accurately (block 1).
    secs = 3.0
    for carrier, tag in ((1000.0, "1 kHz"), (82.4, "82.4 Hz")):
        x = fx.sine(carrier, -10.0, secs, SR)
        worst, where = -300.0, ""
        for t in NEW_TYPES:
            base = [f"{k}={v}" for k, v in keys_for(t, threshold=-25.0, **START[t]).items()]
            for key, lo, hi in AUTOMATION:
                toks = base + square_tokens(key, lo, hi, secs)
                ref = hf_click(fx.run(EFFECT, x, toks, ["--block=1"], SR)[0], SR, carrier) if carrier < 500 else None
                for block in (512, 64, 37):
                    v = hf_click(fx.run(EFFECT, x, toks, [f"--block={block}"], SR)[0], SR, carrier)
                    v = v if ref is None else v - ref
                    if v > worst:
                        worst, where = v, f"{TYPES[t]} {key} block {block}"
        if carrier > 500:
            report.add("C21 automation clicks", f"9 controls x 4 types, {tag}", worst <= -70.0,
                       f"loudest frame > carrier + 3 kHz {worst:.1f} dBc ({where}) (<= -70)")
        else:
            report.add("C21 automation zipper", f"9 controls x 4 types, {tag}", worst <= 3.0,
                       f"block render minus block-1 render, click metric {worst:+.1f} dB ({where}) (<= +3)")


def c21_isolated(report):
    # What the per-block delivery itself adds: the same square wave with its steps on
    # sample positions that are block boundaries for every block size compared (9.375 Hz,
    # 2560-sample half period), rendered at the block size and at block 1. The difference
    # is the zipper; its lines at k * fs / block are measured against the carrier.
    secs, half = 3.0, 2560
    toks = [f"threshold@{(k * half - 0.5) / SR:.9f}={-40.0 if k % 2 == 0 else -10.0}"
            for k in range(int(secs * SR / half))]
    x = fx.sine(1000.0, -10.0, secs, SR)
    worst, where = -300.0, ""
    for t in NEW_TYPES:
        ref = fx.mono(fx.run(EFFECT, x, [f"type={t}", "ratio=4.0"] + toks, ["--block=1"], SR)[0])
        for block in (512, 64):
            y = fx.mono(fx.run(EFFECT, x, [f"type={t}", "ratio=4.0"] + toks, [f"--block={block}"], SR)[0])
            z = block_lines(y - ref, y, SR, block, 1000.0)
            if z > worst:
                worst, where = z, f"{TYPES[t]} block {block}"
    report.add("C21b zipper isolated", "block-aligned steps vs block 1", worst <= -70.0,
               f"largest line at k*fs/block in (block render - block-1 render) {worst:.1f} dBc ({where}) (<= -70)")


def block_lines(diff, ref, sr, block, carrier_hz, skip_s=0.5, lobe=4):
    freqs, spec = fx._spectrum(diff[int(skip_s * sr):], sr)
    _, rspec = fx._spectrum(ref[int(skip_s * sr):], sr)
    df = freqs[1]
    ic = int(round(carrier_hz / df))
    carrier = np.sqrt(np.sum(rspec[max(0, ic - lobe):ic + lobe + 1] ** 2))
    worst, k = -300.0, 1
    while k * sr / block < sr / 2:
        for f in (k * sr / block, abs(carrier_hz - k * sr / block), carrier_hz + k * sr / block):
            i = int(round(f / df))
            if i < len(spec):
                worst = max(worst, float(fx.db(np.sqrt(np.sum(spec[max(0, i - lobe):i + lobe + 1] ** 2))
                                                / max(carrier, 1e-30))))
        k += 1
    return worst


def switch_render(a, b, sr, block, t_switch=1.0, secs=2.0, **kw):
    x = fx.sine(1000.0, -10.0, secs, sr)
    toks = [f"type={a}", f"type@{t_switch}={b}"] + [f"{k}={v}" for k, v in keys_for(a, **kw).items() if k != TYPE_KEY]
    y = fx.run(EFFECT, x, toks, [f"--block={block}"], sr)[0]
    return fx.mono(y), block_start_at(t_switch, sr, block)


def demod_db(y, sr, f, win_ms=2.0):
    """Level of the component at f by complex demodulation (Hann window), dB."""
    y = fx.mono(y)
    t = np.arange(len(y)) / sr
    w = max(3, int(round(win_ms * 1e-3 * sr)) | 1)
    h = np.hanning(w)
    return fx.db(np.maximum(2 * np.abs(np.convolve(y * np.exp(-2j * np.pi * f * t), h / h.sum(), "same")), 1e-12))


def c22(report, sr=SR):
    # (a) steps: every ordered pair of types at the defaults, -10 dBFS sustained note.
    worst, where = 0.0, ""
    one_ms = int(0.001 * sr)
    for a in TYPES:
        for b in TYPES:
            if a == b:
                continue
            y, i0 = switch_render(a, b, sr, 512)
            env = envelope_db(y)[i0 - int(0.01 * sr):i0 + int(0.1 * sr)]
            step = float(np.max(np.abs(env[one_ms:] - env[:-one_ms])))
            if step > worst:
                worst, where = step, f"{TYPES[a]}->{TYPES[b]}"
    report.add("C22 switch steps", "all 20 type pairs, block 512", worst <= 3.0,
               f"largest 1 ms level change {worst:.2f} dB at {where} (<= 3)")
    # (b) the crossfade's length. The old row timed the fade through the dry signal, which
    # is gone: the new types hand their gain state over and need no fade at all. What still
    # fades is a switch with Classic; Clean at mix 0 (its output is the input) makes the
    # crossfade visible as a 15 dB rise that must end 40 +-2 ms after the switch.
    ok, details = True, []
    for block in (64, 512, 37):
        ms = crossfade_ms(sr, block)
        details.append(f"block {block}: {fmt(ms, '.2f')} ms")
        ok = ok and ms is not None and abs(ms - 40.0) <= 2.0
    report.add("C22 switch fade", f"Classic->Clean crossfade {sr} Hz", ok, ", ".join(details) + " (40 +-2)")
    c22_level(report, sr)


def crossfade_ms(sr, block):
    y, i0 = switch_render(0, 1, sr, block, attack=0.5, threshold=-30.0, mix=0.0)
    env = demod_db(y, sr, 1000.0)
    final = float(np.median(env[i0 + int(0.2 * sr):i0 + int(0.3 * sr)]))
    hit = np.flatnonzero(env[i0:i0 + int(0.2 * sr)] >= final - 0.05)
    return None if not len(hit) else 1e3 * hit[0] / sr


def hf_frames(y, sr, carrier_hz, n=1024, hop=256):
    """Per-frame form of hf_click, each frame against its own carrier: -> (dBc, frame centre s)."""
    frames = np.lib.stride_tricks.sliding_window_view(fx.mono(y), n)[::hop] * np.blackman(n)
    spec = np.abs(np.fft.rfft(frames, axis=1)) ** 2
    f = np.fft.rfftfreq(n, 1.0 / sr)
    carrier = spec[:, (f > carrier_hz - 100) & (f < carrier_hz + 100)].sum(axis=1)
    hf = spec[:, f > carrier_hz + 3000].sum(axis=1)
    return 10 * np.log10(np.maximum(hf, 1e-300) / np.maximum(carrier, 1e-300)), (np.arange(len(hf)) * hop + n / 2) / sr


def c22_level(report, sr=SR):
    # A compressor must not get louder when its type changes (verifier cf.py: +15.0 dB for the
    # 40 ms fade). Sustained 1 kHz note at -10 dBFS, T -30, R 4 (15 dB of reduction in every
    # type), the outgoing type's starting knobs; every ordered pair, blocks 64 / 512 / 37.
    # Level never more than 1 dB over the level before the switch, or over the new type's own
    # settled level where that is higher (Classic at Opto's knobs, 9.5 ms release after its
    # 2 pi, reduces 1.2 dB less than the others); 0.5 s later within 0.5 dB of a render that was
    # always on the new type. Clicks: from 50 ms before to 0.5 s after the
    # switch, no frame's energy above carrier + 3 kHz over -70 dBc, unless one of the two types
    # makes as much on its own (Classic's gain ripple does at fast settings: -54 dBc with FET's
    # knobs), in which case the switch may not add more than 3 dB to it.
    secs, t_sw = 2.5, 1.0
    x = fx.sine(1000.0, -10.0, secs, sr)
    worst_up, worst_settle, worst_click, literal = -99.0, 0.0, -300.0, -99.0
    where_up = where_settle = where_click = ""
    click_ok = True
    for a in TYPES:
        knobs = dict(START[a], threshold=-30.0)
        alone = {b: fx.run(EFFECT, x, keys_for(b, **knobs), [], sr)[0] for b in TYPES}
        level = {b: demod_db(y, sr, 1000.0) for b, y in alone.items()}
        roughness = {b: hf_frames(y, sr, 1000.0) for b, y in alone.items()}
        for b in TYPES:
            if a == b:
                continue
            for block in (64, 512, 37):
                toks = [f"{k}={v}" for k, v in keys_for(a, **knobs).items()] + [f"type@{t_sw}={b}"]
                y = fx.run(EFFECT, x, toks, [f"--block={block}"], sr)[0]
                env = demod_db(y, sr, 1000.0)
                i0 = block_start_at(t_sw, sr, block)
                pre = float(np.median(env[i0 - int(0.1 * sr):i0 - int(0.01 * sr)]))
                late = slice(i0 + int(0.5 * sr), i0 + int(1.0 * sr))
                own_b = float(np.median(level[b][late]))
                peak = float(np.max(env[i0:i0 + int(1.0 * sr)]))
                up = peak - max(pre, own_b)
                literal = max(literal, peak - pre)
                settle = abs(float(np.median(env[late])) - own_b)
                v, t = hf_frames(y, sr, 1000.0)
                near = (t >= i0 / sr - 0.05) & (t <= i0 / sr + 0.5)
                click = float(np.max(v[near]))
                own = float(np.max(np.maximum(roughness[a][0], roughness[b][0])[near]))
                ok = click <= -70.0 or click <= own + 3.0
                click_ok = click_ok and ok
                tag = f"{TYPES[a]}->{TYPES[b]} block {block}"
                if up > worst_up:
                    worst_up, where_up = up, tag
                if settle > worst_settle:
                    worst_settle, where_settle = settle, tag
                if click - max(own, -70.0) > worst_click:
                    worst_click, where_click = click - max(own, -70.0), f"{tag}: {click:.1f} dBc, alone {own:.1f}"
    report.add("C22 switch level", "20 pairs x 3 blocks, 15 dB GR", worst_up <= 1.0,
               f"largest rise over max(pre-switch level, new type's own) {worst_up:+.2f} dB ({where_up}) (<= +1); "
               f"over the pre-switch level alone {literal:+.2f} dB")
    report.add("C22 switch settles", "20 pairs x 3 blocks, 15 dB GR", worst_settle <= 0.5,
               f"0.5-1 s after: worst |level - new type's own| {worst_settle:.2f} dB ({where_settle}) (<= 0.5)")
    report.add("C22 switch click", "20 pairs x 3 blocks, 15 dB GR", click_ok,
               f"worst frame > carrier + 3 kHz against max(-70 dBc, the louder type alone) {worst_click:+.1f} dB "
               f"({where_click}) (<= -70 dBc, or <= +3 dB over a type alone)")


def c23(report):
    # 44.1 and 96 kHz as the contract asks, plus the ends of the supported range.
    for sr in (44100, 96000, 22050, 192000):
        tag = f" @{sr // 1000}k"
        c3(report, sr, tag)
        c4(report, sr, tag)
        c5(report, sr, tag)
        c6(report, sr, tag)
        c10(report, sr, tag)


def c24(report):
    # NaN and infinity (verifier xa.py / code.md: eight NaN samples with the detector filter on
    # made the output NaN for good). A 1 kHz note with 8 NaN, then 8 +inf, then 8 -inf input
    # samples on the left channel (and on the only channel, mono): every output sample from one
    # detector window (12.5 ms) after the first bad one is finite, and the level 0.5 s later
    # equals a clean render within 0.1 dB. Each type, detector filter off and at 100 Hz,
    # blocks 512 and 37.
    sr = SR
    x = fx.sine(1000.0, -10.0, 2.0, sr)
    bad = x.copy()
    for t0, v in ((0.5, np.nan), (0.7, np.inf), (0.9, -np.inf)):
        bad[int(t0 * sr):int(t0 * sr) + 8] = v
    window = int(0.0125 * sr)
    for t in TYPES:
        fails, worst = [], 0.0
        for hpf in (20.0, 100.0):
            for ch in (2, 1):
                inp = np.vstack([bad, x]) if ch == 2 else bad
                clean = np.vstack([x, x]) if ch == 2 else x
                for block in (512, 37):
                    keys = keys_for(t, sc_hpf=hpf, **START[t])
                    r = fx.invoke(EFFECT, inp, keys, [f"--block={block}", "--trace=gain"], sr)
                    y, ref = r.out, fx.run(EFFECT, clean, keys, [f"--block={block}"], sr)[0]
                    after = slice(int(0.5 * sr) + window, None)
                    if not (np.all(np.isfinite(y[:, after])) and np.all(np.isfinite(r.trace))):
                        fails.append(f"sc_hpf {hpf:g} {ch} ch block {block}: non-finite output")
                        continue
                    tail = slice(int(1.5 * sr), None)
                    d = abs(rms_db(y[0, tail]) - rms_db(ref[0, tail]))
                    worst = max(worst, d)
                    if d > 0.1:
                        fails.append(f"sc_hpf {hpf:g} {ch} ch block {block}: level {d:.2f} dB off")
        report.add("C24 NaN / inf", f"{TYPES[t]}, sc_hpf off/100, mono+stereo", not fails,
                   f"finite from one window after; worst level difference after 0.5 s {worst:.3f} dB (<= 0.1)"
                   + ("; FAIL " + "; ".join(fails[:3]) if fails else ""))


def c25(report):
    # Output bound of contract §2.2 (|out| < 1e3 for finite input, any parameters).
    # (a) the fxbench fuzz case that peaked at 1324 (build-verify/code/logs/fxbench-fuzz.log):
    # makeup to 24 and auto makeup on at T -60 R 17.4 while bypassed, type changes, re-enable;
    # (b) a full-scale 41 Hz onset after 1 s of silence at the slowest attack with maximum
    # makeup and auto makeup, every new type; (c) as (b) but the note runs through a bypass
    # with a type change in it, from every type to every type.
    fuzz = ["type=1", "type@1.1335=4", "type@0.0762=0", "threshold=-60", "ratio=17.4201",
            "ratio@0.2993..0.8809=14.9541..17.5817", "attack=36.0235", "release=43.2749", "release@2.3466=502.756",
            "release@1.1342=1000", "makeup=8.63839", "makeup@0.9077=24", "knee=19.345", "knee@1.0366=14.5512",
            "knee@1.4050=12.5357", "mix=95.0152", "sc_hpf=59.5858", "auto_makeup=1", "on=1", "on@0.8347=0",
            "on@1.5837=1"]
    r = fx.invoke(EFFECT, "gen:sine:41:0", fuzz, ["--block=37", "--seconds=2.5", "--channels=1"], 176400)
    peaks = {"fuzz": float(np.max(np.abs(r.out))) if np.all(np.isfinite(r.out)) else np.inf}
    hot = dict(threshold=-60.0, ratio=20.0, attack=100.0, makeup=24.0, auto_makeup=1)
    onset = np.concatenate([np.zeros(SR), fx.sine(41.0, 0.0, 1.5, SR)])
    for t in NEW_TYPES:
        y = fx.run(EFFECT, onset, keys_for(t, **hot), [], SR)[0]
        peaks[f"onset {TYPES[t]}"] = float(np.max(np.abs(y))) if np.all(np.isfinite(y)) else np.inf
    note = fx.sine(41.0, 0.0, 3.0, SR)
    for a in TYPES:
        for b in TYPES:
            toks = [f"{k}={v}" for k, v in keys_for(a, **hot).items()] + ["on@1.0=0", f"type@1.3={b}", "on@1.6=1"]
            y = fx.run(EFFECT, note, toks, ["--block=37"], SR)[0]
            peaks[f"bypass {TYPES[a]}->{TYPES[b]}"] = float(np.max(np.abs(y))) if np.all(np.isfinite(y)) else np.inf
    where = max(peaks, key=peaks.get)
    report.add("C25 output bound", "fuzz case, onsets, bypass switches", peaks[where] < 1e3,
               f"fuzz case peak {peaks['fuzz']:.1f}; worst {peaks[where]:.1f} x full scale ({where}) (< 1000)")


def c26(report):
    # What Squash's release knob promises: the gain's t63 when the level falls, (a) after the
    # signal falls below the threshold (and the knee), whatever the ratio, the knee and the
    # level it fell from, and (b) after a drop that leaves the compressor's output above the
    # threshold (beyond the knee): the knob within 20 % (verifier cb.py: 52 ms against 150 at
    # R 8; the loop used to speed the release up R times). Between the two, a drop that leaves
    # the input above the threshold but takes the output under it recovers sooner: the open
    # loop heads for 0 dB (the feedback character); printed, -4 -> -16 dBFS as in cb.py.
    worst, where = 0.0, ""
    for level in (-16.0, -10.0, 0.0):
        for ratio in (2.0, 4.0, 8.0, 12.0, 20.0):
            for knee in (0.0, 6.0, 24.0):
                r = burst_times(2, level, post=1.0, threshold=-34.0, ratio=ratio, knee=knee, attack=2.5, release=150.0)
                err = abs(r["r63"] / 150.0 - 1.0) if r["r63"] is not None else 9.9
                if err > worst:
                    worst, where = err, f"{level:g} dBFS R {ratio:g} knee {knee:g}: {fmt(r['r63'], '.1f')} ms"

    def drop_t63(low, ratio, knee):
        x = fx.step(SR, 1000.0, -4.0, low, 0.6, 2.0)
        _, tr = render(keys_for(2, threshold=-34.0, ratio=ratio, knee=knee, attack=2.5, release=150.0), x)
        t63 = fx.settle_times(tr, SR, 0.6, (0.63,), final=float(np.mean(tr[-int(0.2 * SR):])))[0.63]
        return None if t63 is None else 1e3 * t63

    worst_above, where_above, count = 0.0, "", 0
    for low in (-6.0, -8.0, -10.0, -16.0):
        for ratio in (2.0, 4.0, 8.0, 20.0):
            for knee in (0.0, 6.0, 24.0):
                if (low + 34.0) - textbook_gr(30.0, ratio, knee) < knee / 2:
                    continue
                count += 1
                t63 = drop_t63(low, ratio, knee)
                err = abs(t63 / 150.0 - 1.0) if t63 is not None else 9.9
                if err > worst_above:
                    worst_above, where_above = err, f"-4 -> {low:g} dBFS R {ratio:g} knee {knee:g}: {fmt(t63, '.1f')} ms"
    dips = ", ".join(f"R {ratio:g} {fmt(drop_t63(-16.0, ratio, 6.0), '.1f')} ms" for ratio in (2.0, 8.0, 20.0))
    report.add("C26 Squash release", "below threshold, and output above", worst <= 0.2 and worst_above <= 0.2,
               f"(a) 45 bursts to silence: within {100 * worst:.1f} % of the 150 ms knob, worst {where}; (b) {count} drops "
               f"with the output above the threshold: within {100 * worst_above:.1f} %, worst {where_above} (<= 20 %); "
               f"output dips under (-4 -> -16 dBFS, knee 6): {dips}")


def textbook_gr(o, ratio, knee):
    """Gain reduction (dB >= 0) of the soft-knee curve at o dB over the threshold."""
    return -gain_computer(o, 1.0 / ratio - 1.0, knee)


def gain_computer(o, s, wk):
    if 2 * o <= -wk:
        return 0.0
    if 2 * o < wk:
        return s * (o + wk / 2) ** 2 / (2 * wk)
    return s * o


def c27(report):
    # What Squash's knee knob promises: the textbook knee of the other types, its width on the
    # input level and centred on the threshold (verifier cd.py signal: R 20 knee 24 gave 7.61 dB
    # at the threshold against 2.85). GR of a steady 1 kHz tone at T - k/2, T, T + k/2, T + 10
    # against the textbook curve: within 20 % (0.1 dB where the curve is under 0.5 dB).
    worst, where, cells = 0.0, "", []
    for ratio, knee, thr in ((4.0, 6.0, -20.0), (8.0, 6.0, -34.0), (20.0, 24.0, -30.0), (1.5, 12.0, -20.0)):
        row = []
        for o in (-knee / 2, 0.0, knee / 2, 10.0):
            want = textbook_gr(o, ratio, knee)
            got = static_gr(2, thr + o, threshold=thr, ratio=ratio, knee=knee, attack=2.5, release=150.0)
            err = abs(got - want) / (0.2 * want if want >= 0.5 else 0.1)
            row.append(f"{got:.2f}({want:.2f})")
            if err > worst:
                worst, where = err, f"R {ratio:g} knee {knee:g} at T{o:+g}: {got:.2f} dB, textbook {want:.2f}"
        cells.append(f"R {ratio:g} k {knee:g}: " + " ".join(row))
    report.add("C27 Squash knee", "R 4/8/20/1.5, GR at T-k/2, T, T+k/2, T+10", worst <= 1.0,
               f"worst {where} ({100 * worst:.0f} % of the tolerance); measured(textbook) dB " + "; ".join(cells))


def c28(report):
    # Programme-dependent release in FET and Opto: the release after a long, heavy compression
    # is slower. Reference shapes: 1176 Rev A fastest (study comp/evidence/bb/ua_1176a/
    # t_a100_r100.tbA/.tbL, and the 56 ms after 0.1 s of verification.md C14): t63 56 / 95 /
    # 123 ms after 0.1 / 0.5 / 5 s; LA-2A (ua_la2a_tc/t_comp_pr50.tbA/.tbL): 157 -> 184 ms from
    # 0.5 to 5 s. Rows: t63 grows with the burst length by at least 5 % per step; the 5 s / 0.5 s
    # and 0.1 s / 0.5 s ratios within 20 % of the reference unit's; a shallower burst releases
    # faster (1176 tbB, 4 dB of reduction: 83 against 95 ms); and the memory fades: a 0.5 s burst
    # 12 s after a 5 s one releases within 5 % of a fresh one. C14 / C15 hold the 0.5 s shape.
    cases = {3: (dict(threshold=-24.0, ratio=6.0, knee=4.0, attack=0.3, release=55.0), {5.0: 123 / 95, 0.1: 56 / 95}),
             4: (dict(threshold=-24.0, ratio=4.0, knee=12.0, attack=2.0, release=60.0), {5.0: 184 / 157})}
    for t, (kw, refs) in cases.items():
        r63 = {}
        for length in (0.1, 0.5, 5.0):
            x, end = fx.burst(1000.0, -10.0, length, SR, pre=0.2, post=2.5)
            _, tr = render(keys_for(t, **kw), x)
            r63[length] = 1e3 * fx.settle_times(tr, SR, end / SR, (0.63,), final=0.0)[0.63]
        shallow = burst_times(t, -20.0, post=2.5, **kw)["r63"]
        x5, _ = fx.burst(1000.0, -10.0, 5.0, SR, pre=0.2, post=12.0)
        xb, end = fx.burst(1000.0, -10.0, 0.5, SR, pre=0.0, post=2.5)
        _, tr = render(keys_for(t, **kw), np.concatenate([x5, xb]))
        after = 1e3 * fx.settle_times(tr, SR, (len(x5) + end) / SR, (0.63,), final=0.0)[0.63]
        grows = r63[0.1] * 1.05 <= r63[0.5] and r63[0.5] * 1.05 <= r63[5.0]
        sizes = all(abs((r63[k] / r63[0.5]) / ref - 1.0) <= 0.2 for k, ref in refs.items())
        ok = grows and sizes and shallow < r63[0.5] and abs(after / r63[0.5] - 1.0) <= 0.05
        report.add("C28 programme release", f"{TYPES[t]} " + " ".join(f"{k}={v:g}" for k, v in kw.items()), ok,
                   "t63 after 0.1 / 0.5 / 5 s: " + " / ".join(f"{r63[k]:.1f}" for k in (0.1, 0.5, 5.0)) + " ms; "
                   + ", ".join(f"{k:g} s / 0.5 s {r63[k] / r63[0.5]:.2f} (ref {ref:.2f} +-20 %)" for k, ref in refs.items())
                   + f"; -20 dBFS burst {shallow:.1f} ms (< 0.5 s value); 0.5 s burst 12 s after a 5 s one {after:.1f} ms "
                   f"(within 5 %)")


# ---------------------------------------------------------------------------- per-control effect rows

def model_gr(t, level, thr, ratio, knee):
    """Static gain reduction the type's curve gives a steady tone (Classic: hard knee)."""
    if t == 0:
        return max(0.0, (1.0 - 1.0 / ratio) * (level - thr))
    if t == 3 and ratio > 4.0:
        thr += 2.6 * np.log2(ratio / 4.0)
    return textbook_gr(level - thr, ratio, knee)


def level_step(t, x, steps, sr=SR, t_step=1.0, **kw):
    """Output level change (dB) and GR change (dB) across a parameter step at t_step."""
    keys = [f"{k}={v}" for k, v in keys_for(t, **kw).items()] + steps
    y, tr = render(keys, x, sr)
    i = block_start_at(t_step, sr, 512)
    before, after = slice(i - int(0.35 * sr), i - int(0.05 * sr)), slice(i + int(0.5 * sr), i + int(0.8 * sr))
    return rms_db(y[after]) - rms_db(y[before]), float(np.mean(tr[after]) - np.mean(tr[before]))


def burst_pair(t, key, a, b, **kw):
    """Two 1 kHz bursts at -10 dBFS, `key` = a for the first and b for the second (the step
    lands in the 2 s gap, with the block on). -> attack t63 and release t63 of each (ms)."""
    sr, n = SR, int(0.5 * SR)
    one, end = fx.burst(1000.0, -10.0, 0.5, sr, pre=0.2, post=2.0)
    x = np.concatenate([one, one])
    keys = [f"{k}={v}" for k, v in keys_for(t, **dict(kw, **{key: a})).items()] + [f"{key}@{len(one) / sr - 1.0:.4f}={b}"]
    _, tr = render(keys, x, sr)
    out = []
    for off in (0, len(one)):
        i0, i1 = off + int(0.2 * sr), off + end
        seg = tr[off:off + len(one)]
        settled = float(np.mean(tr[i1 - int(0.012 * sr):i1 - int(0.002 * sr)]))
        att = fx.settle_times(seg, sr, 0.2, (0.63,), final=settled)[0.63]
        rel = fx.settle_times(seg, sr, (i1 - off) / sr, (0.63,), final=0.0)[0.63]
        out.append((None if att is None else 1e3 * att, None if rel is None else 1e3 * rel))
    return out


def k_rows(report):
    # One row per type and per control the type reads: the control is moved WHILE THE BLOCK
    # RUNS and the output level or the gain trace must change the way the control says, in
    # direction and within 20 % of the size the curve predicts (25 % of the time ratio for
    # attack and release). The makeup knob that did nothing in four types passed 87 rows
    # because every row set its controls before the first block (where the makeup smoother is
    # primed) and none moved one later; a control with no effect fails here. Controls Classic
    # does not read must leave its output bit for bit as it was.
    x = fx.sine(1000.0, -10.0, 2.0, SR)
    hot = fx.sine(1000.0, -4.0, 2.0, SR)
    at_thr = fx.sine(1000.0, -20.0, 2.0, SR)
    bass = fx.sine(82.4, -10.0, 2.5, SR)
    hpf_db = 20 * np.log10(82.4 / np.hypot(82.4, 300.0))

    def static(t, name, sig, steps, want, gr_too=None, **kw):
        d_out, d_gr = level_step(t, sig, steps, **dict(START[t], **kw))
        ok = np.sign(d_out) == np.sign(want) and abs(d_out - want) <= max(0.25, 0.2 * abs(want)) and abs(want) >= 1.0
        detail = f"output {d_out:+.2f} dB (expected {want:+.2f} +-20 %)"
        if gr_too is not None:
            ok = ok and abs(d_gr - gr_too) <= max(0.1, 0.2 * abs(gr_too))
            detail += f", GR {d_gr:+.2f} dB (expected {gr_too:+.2f})"
        report.add(f"K {name}", TYPES[t], ok, detail)

    def timing(t, name, key, a, b):
        (att_a, rel_a), (att_b, rel_b) = burst_pair(t, key, a, b, **START[t])
        va, vb = (att_a, att_b) if key == "attack" else (rel_a, rel_b)
        ratio = None if not va or vb is None else vb / va
        ok = ratio is not None and abs(ratio / (b / a) - 1.0) <= 0.25
        report.add(f"K {name}", TYPES[t], ok, f"t63 {fmt(va, '.2f')} -> {fmt(vb, '.2f')} ms for {a:g} -> {b:g}: "
                   f"ratio {fmt(ratio, '.2f')} (expected {b / a:g} +-25 %)")

    for t in TYPES:
        knee = START[t]["knee"]
        g = lambda level, thr=-20.0, ratio=4.0, k=knee: model_gr(t, level, thr, ratio, k)  # noqa: E731
        static(t, "threshold", x, ["threshold@1.0=-30"], -(g(-10, -30) - g(-10)))
        static(t, "ratio", hot, ["ratio=2", "ratio@1.0=8"], -(g(-4, ratio=8) - g(-4, ratio=2)))
        timing(t, "attack", "attack", 1.0, 20.0)
        timing(t, "release", "release", 50.0, 400.0)
        static(t, "makeup", x, ["makeup@1.0=12"], 12.0, gr_too=0.0)
        if t == 0:
            for key, a, b in (("knee", 6, 24), ("mix", 100, 0), ("sc_hpf", 20, 300), ("auto_makeup", 0, 1)):
                base = [f"{k}={v}" for k, v in keys_for(0, **START[0]).items()] + [f"{key}={a}"]
                y0 = fx.run(EFFECT, x, base, [], SR)[0]
                y1 = fx.run(EFFECT, x, base + [f"{key}@1.0={b}"], [], SR)[0]
                report.add(f"K {key}", "Classic (not read)", np.array_equal(y0, y1),
                           f"output bit for bit unchanged by {key} {a} -> {b}: {np.array_equal(y0, y1)}")
            continue
        static(t, "knee", at_thr, ["knee=0", "knee@1.0=24"], -(g(-20, k=24) - g(-20, k=0)))
        static(t, "mix", x, ["mix@1.0=0"], g(-10), gr_too=0.0)
        static(t, "sc_hpf", bass, ["sc_hpf@1.0=300"], g(-10) - g(-10 + hpf_db))
        static(t, "auto_makeup", x, ["auto_makeup@1.0=1"], g(-10), gr_too=0.0)


ROWS = {"C2": c2, "C3": c3, "C3b": c3_link_meter, "C4": c4, "C5": c5, "C6": c6, "C7": c7, "C8": c8, "C9": c9, "C10": c10, "C10+": c10_grid, "C11": c11,
        "C12": c12, "C13": c13, "C14": c14, "C15": c15, "C16": c16, "C17": c17, "C18": c18, "C19": c19, "C20": c20,
        "C21": c21, "C21b": c21_isolated, "C22": c22, "C23": c23, "C24": c24, "C25": c25, "C26": c26, "C27": c27,
        "C28": c28, "K": k_rows}


def specific_checks(report, rows=None):
    if rows is None or "C1" in rows:
        # Makeup ramps and skip() sequences: a makeup step, a threshold step, off for a
        # second (skip), a makeup change while off, back on; ratio, attack and release ramps.
        steps = ["makeup@1.5=12", "threshold@2.2=-35", "on@3=0", "makeup@3.5=3", "on@4=1", "ratio@0.5..2.5=2..12",
                 "attack@1..2=1..30", "release@2..4=200..20"]
        ok, detail = fx.classic_null(EFFECT, steps, blocks=[512, 37, 1])
        report.add("C1 ramps and skip", "Classic 44.1/48/96k blocks 512/37/1", ok, detail)
        ok, detail = fx.classic_null(EFFECT, ["on=0", "makeup=6", "on@0.7=1", "makeup@2=0", "on@2.5=0", "on@2.6=1"],
                                     opts=["--channels=1"])
        report.add("C1 skip first, mono", "Classic mono", ok, detail)
    for name, fn in ROWS.items():
        if rows is None or name in rows:
            fn(report)


# ---------------------------------------------------------------------------- information tables

def lf_table():
    print("\nLow-frequency residual, -10 dBFS sine, T -20 R 4 (harmonic THD % / inharmonic sidebands dBc total,"
          " largest line dBc @ Hz / LS residual %)")
    for label, settings in (("starting point", None), ("fastest 0.1/10", FASTEST)):
        for sr in (44100, 48000):
            print(f"  {label}, {sr} Hz")
            floor = []
            for f in LF_FREQS:
                x = fx.sine(f, -10.0, 4.0, sr).astype(np.float32).astype(np.float64)
                d = split_distortion(x, sr, f)
                floor.append(f"{f:g}: {d['harm_pct']:.4f} % / {d['side_db']:.1f} ({d['worst_db']:.1f} @ "
                             f"{d['worst_hz']:.0f}) / {fx.fundamental_residual(x, sr, f, start=3.0):.4f} %")
            print(f"    {'input':<8}" + " | ".join(floor))
            for t, name in TYPES.items():
                kw = dict(START[t]) if settings is None else dict(START[t], **settings)
                cells = []
                for f in LF_FREQS:
                    x = fx.sine(f, -10.0, 4.0, sr)
                    y, _ = render(keys_for(t, **kw), x, sr)
                    d = split_distortion(y, sr, f)
                    res = fx.fundamental_residual(y, sr, f, start=3.0)
                    cells.append(f"{f:g}: {d['harm_pct']:.4f} % / {d['side_db']:.1f} ({d['worst_db']:.1f} @ "
                                 f"{d['worst_hz']:.0f}) / {res:.4f} %")
                print(f"    {name:<8}" + " | ".join(cells))


def timing_table():
    print("\nAttack / release per type at its starting point, T -20 R 4, 1 kHz 0.5 s burst from silence (ms)")
    for t, name in TYPES.items():
        parts = []
        for level in (-10.0, -15.0):
            r = burst_times(t, level, post=2.5, **START[t])
            parts.append(f"{level:g} dBFS: GR {r['settled']:.2f} dB, attack t63 {fmt(r['att'])}, release "
                         f"t50/t63/t90 {fmt(r['r50'], '.1f')}/{fmt(r['r63'], '.1f')}/{fmt(r['r90'], '.1f')}")
        print(f"  {name:<8}" + " | ".join(parts))
    print("\nClassic->Clean crossfade at 44.1 kHz: "
          + ", ".join(f"block {b}: {fmt(crossfade_ms(44100, b), '.2f')} ms" for b in (64, 512, 2048)))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--quick", action="store_true", help="48 kHz stereo, blocks 37 and 512 for the common checks")
    ap.add_argument("--types", type=lambda s: [int(v) for v in s.split(",")], default=None)
    ap.add_argument("--rows", type=lambda s: s.split(","), default=None, help="e.g. C3,C10 (default: all)")
    ap.add_argument("--no-common", action="store_true")
    ap.add_argument("--no-tables", action="store_true")
    args = ap.parse_args()
    report = fx.Report(EFFECT)
    if not args.no_common:
        fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                         quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
    specific_checks(report, args.rows)
    if not args.no_tables:
        lf_table()
        timing_table()
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
