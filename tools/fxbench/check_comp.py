#!/usr/bin/env python3
"""Acceptance for the comp engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.2.9). Prints a PASS/FAIL table; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_comp.py [--quick] [--types 1,2] [--rows C3,C10] [--no-common]
                                           [--no-tables]

After the table it prints three information tables (not pass/fail): the low-frequency
residual split into harmonic distortion and inharmonic sidebands for every type at its
starting point and at the fastest settings, 41.2/82.4/98/110 Hz at 44.1 and 48 kHz;
attack and release per type; and the switch timing at 44.1 kHz.
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


def c21(report):
    # As written: threshold square wave -40 <-> -10 dB at 10 Hz, steps delivered per block
    # like host automation, the largest line at k * fs / block (and the carrier +- it).
    secs = 3.0
    toks = [f"threshold@{k * 0.05:.2f}={-40.0 if k % 2 == 0 else -10.0}" for k in range(int(secs * 20))]
    x = fx.sine(1000.0, -10.0, secs, SR)
    for block in (512, 64, 37):
        worst, where = -300.0, ""
        for t in NEW_TYPES:
            y = fx.run(EFFECT, x, [f"type={t}", "ratio=4.0"] + toks, [f"--block={block}"], SR)[0]
            z = fx.zipper_lines(y, SR, block, 1000.0)
            if z > worst:
                worst, where = z, TYPES[t]
        report.add("C21 smoothing", f"threshold -40<->-10 dB 10 Hz, block {block}", worst <= -70.0,
                   f"largest line at k*fs/block {worst:.1f} dBc ({where}) (<= -70)")


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
    # (b) fade duration: Clean -> FET with a fast attack, so the incoming type has settled
    # by the end of its fade-in; the level returns to the compressed value when the fade ends.
    ok, details = True, []
    for block in (64, 512, 37):
        ms = fade_ms(1, 3, sr, block)
        details.append(f"block {block}: {fmt(ms, '.2f')} ms")
        ok = ok and ms is not None and abs(ms - 40.0) <= 2.0
    report.add("C22 switch fade", f"Clean->FET {sr} Hz", ok, ", ".join(details) + " (40 +-2)")


def fade_ms(a, b, sr, block):
    y, i0 = switch_render(a, b, sr, block, attack=0.5)
    env = envelope_db(y)
    final = float(np.mean(env[i0 + int(0.2 * sr):i0 + int(0.3 * sr)]))
    seg = env[i0:i0 + int(0.2 * sr)]
    top = int(np.argmax(seg))
    hit = np.flatnonzero(seg[top:] <= final + 0.05)
    return None if not len(hit) else 1e3 * (top + hit[0]) / sr


def c23(report):
    # 44.1 and 96 kHz as the contract asks, plus the ends of the supported range.
    for sr in (44100, 96000, 22050, 192000):
        tag = f" @{sr // 1000}k"
        c3(report, sr, tag)
        c4(report, sr, tag)
        c5(report, sr, tag)
        c6(report, sr, tag)
        c10(report, sr, tag)


ROWS = {"C2": c2, "C3": c3, "C3b": c3_link_meter, "C4": c4, "C5": c5, "C6": c6, "C7": c7, "C8": c8, "C9": c9, "C10": c10, "C10+": c10_grid, "C11": c11,
        "C12": c12, "C13": c13, "C14": c14, "C15": c15, "C16": c16, "C17": c17, "C18": c18, "C19": c19, "C20": c20,
        "C21": c21, "C21b": c21_isolated, "C22": c22, "C23": c23}


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
    print("\nType-switch fade (Clean->FET) at 44.1 kHz: "
          + ", ".join(f"block {b}: {fmt(fade_ms(1, 3, 44100, b), '.2f')} ms" for b in (64, 512, 2048)))


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
