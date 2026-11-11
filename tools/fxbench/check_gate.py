#!/usr/bin/env python3
"""Acceptance for the gate engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.1.10), then the extra proofs (X rows). Prints a PASS/FAIL
table; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_gate.py [--quick] [--types 1,2] [--grid]

--grid also prints the full close-time grid (threshold x level x frequency, Tight and
Smooth, worst and best over the note-off phase sweep).

Close time (§4.1.10): on the per-sample gain trace, from the end of the burst's 1 ms
raised-cosine fade to the first sample with gain <= -20 dB; each cell is the max over
8 start phases x 4 burst lengths (0.3 s + {0, 0.37, 1.13, 2.9} ms), i.e. 32 note-off
phases. The 32 bursts of a cell run in one render, each after 0.6 s of digital silence,
which leaves the gate in exactly its reset state (closed, window empty, filter flushed).
"""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np  # noqa: E402

import fxcheck as fx  # noqa: E402

EFFECT = "gate"
TYPE_KEY = "mode"
TYPES = {0: "Classic", 1: "Tight", 2: "Smooth"}
# Float keys at their documented limits, for the finite-output test.
RANGES = {"threshold": (-100.0, 0.0), "release": (5.0, 500.0), "hold": (0.0, 250.0), "range": (-90.0, 0.0),
          "sc_hpf": (20.0, 300.0)}
# Percent of real time per instance (§4.1.9): Tight/Smooth 0.10; Classic 0.25 (as today).
BUDGETS = {0: 0.25, 1: 0.10, 2: 0.10}
VARIANTS = {}
STEPS = {"threshold": -60, "threshold@1.3": -40, "threshold@2.6": -25, "threshold@4.2": -75}
NULL_KEYS = [{}, {"threshold": -40}, STEPS]

# The engine's per-mode starting points (Gate.h kGateReleaseDefault, kGateHoldDefault). Tight's
# hold is 6 ms, not the contract's 10 (coordinator decision, see X2/X9); G2-G6 keep their bounds.
TIGHT = {"mode": 1, "release": 30.0, "hold": 6.0, "range": -90.0, "sc_hpf": 90.0}
SMOOTH = {"mode": 2, "release": 80.0, "hold": 10.0, "range": -90.0, "sc_hpf": 90.0}
EXTRAS_MS = (0.0, 0.37, 1.13, 2.9)
PHASES = 8
THRESHOLDS = (-30, -40, -50, -60, -70)
LEVELS = (-6, -20)
FREQS = (1000.0, 82.41)


def gain_db(trace):
    return fx.db(fx.mono(trace))


# ---------------------------------------------------------------------------- close time

def close_times(freq, level, keys, sr=48000, block=512):
    """Close times in ms over the 32 note-off phases of one cell."""
    pieces, ends, pos = [np.zeros(int(0.05 * sr))], [], int(0.05 * sr)
    for ph in range(PHASES):
        for extra in EXTRAS_MS:
            x, end = fx.burst(freq, level, 0.3 + extra * 1e-3, sr, pre=0.0, post=0.6,
                              phase=2.0 * np.pi * ph / PHASES)
            pieces.append(x)
            ends.append(pos + end)
            pos += len(x)
    r = fx.invoke(EFFECT, np.concatenate(pieces), keys, [f"--block={block}", "--trace"], sr)
    g = gain_db(r.trace)
    out = []
    for e in ends:
        hit = np.nonzero(g[e:e + int(0.5 * sr)] <= -20.0)[0]
        out.append(hit[0] / sr * 1e3 if len(hit) else float("inf"))
    return out


class Grid:
    """Close-time cells, computed once and shared by the rows that read them."""

    def __init__(self):
        self.cells = {}

    def get(self, mode, freq, level, thr, sr=48000, hpf=90.0):
        key = (mode, freq, level, thr, sr, hpf)
        if key not in self.cells:
            base = dict(TIGHT if mode == 1 else SMOOTH, threshold=thr, sc_hpf=hpf)
            self.cells[key] = close_times(freq, level, base, sr)
        return self.cells[key]

    def worst(self, *a, **k):
        return max(self.get(*a, **k))

    def best(self, *a, **k):
        return min(self.get(*a, **k))


def close_row(report, grid, row, mode, freq, level, thrs, bound, sr=48000, hpf=90.0, scope=None):
    worst = {t: grid.worst(mode, freq, level, t, sr, hpf) for t in thrs}
    ok = all(v <= bound for v in worst.values())
    detail = " / ".join(f"{t}: {v:.1f}" for t, v in worst.items()) + f" ms (limit {bound})"
    name = scope or f"{TYPES[mode]} {freq:g} Hz {level} dBFS{'' if sr == 48000 else f' {sr} Hz'}"
    report.add(row, name, ok, detail)


# ---------------------------------------------------------------------------- chug

def chug_stats(keys, sr=48000, note=0.075, period=0.15, extra=None, secs=3.0):
    """-> (median gap gain dB, worst gap gain dB, worst first-10-ms note level change dB,
    worst first-5-ms note level change dB). Gap gain: per gap the median of the gain trace
    over the last 10 ms before the next onset; note level change: output against input
    energy over the first 10 (5) ms of each note."""
    x = fx.chug(sr, secs=secs, note=note, period=period)
    if extra is not None:
        x = x + extra(sr, len(x))
    r = fx.invoke(EFFECT, x, keys, ["--trace"], sr)
    g = np.maximum(gain_db(r.trace), -120.0)
    y = fx.mono(r.out)
    nn, step = int(round(note * sr)), int(round(period * sr))
    onsets = list(range(0, len(x) - nn, step))
    w10, w5 = int(round(0.010 * sr)), int(round(0.005 * sr))
    gaps = [float(np.median(g[o - w10:o])) for o in onsets[1:]]

    def lost(win):
        return min(10 * np.log10(np.sum(y[o:o + win] ** 2) / np.sum(x[o:o + win] ** 2)) for o in onsets)

    return float(np.median(gaps)), float(max(gaps)), lost(w10), lost(w5)


def hum(level_db, freq=50.0):
    return lambda sr, n: fx.amp(level_db) * np.sin(2 * np.pi * freq * np.arange(n) / sr)


# ---------------------------------------------------------------------------- helpers

def info(report, check, scope, detail):
    """A reported line that is not enforced (never counts as a failure)."""
    report.rows.append((check, scope, "INFO", detail))
    print(f"  INFO  {check:<22} {scope:<34} {detail}", flush=True)


def open_changes(gain, start=0):
    """Open/closed changes of Tight's state, exact: its gain is exactly 1 only while open."""
    is_open = fx.mono(gain)[start:] >= 1.0
    return int(np.count_nonzero(is_open[1:] != is_open[:-1]))


def crossings(g_db, sr, level=-6.0, after=0.5):
    seg = g_db[int(after * sr):] > level
    return int(np.count_nonzero(seg[1:] != seg[:-1]))


def meters(r):
    rows = [line.split("\t") for line in r.meters]
    return np.array([[float(v) for v in row] for row in rows])


# ---------------------------------------------------------------------------- the table

def specific_checks(report, args, grid):
    sr = 48000

    # G1: Classic null on both apply paths, rates x blocks 1/37/512, mono and stereo, with
    # threshold steps; Classic must also ignore the curve delay.
    ok, detail = True, ""
    for keys in ({"deferred": 0}, {"deferred": 1}, {"deferred": 1, "delay": 300}):
        for ch in (1, 2):
            good, why = fx.classic_null(EFFECT, dict(STEPS, **keys), [f"--channels={ch}"], blocks=[1, 37, 512])
            if not good:
                ok, detail = False, f"{keys} {ch} ch: {why}"
    report.add("G1 classic null", "Classic in place + deferred", ok, detail or "0 differing samples in 54 renders")

    T, S = 1, 2
    close_row(report, grid, "G2 close time", T, 1000.0, -20, (-30, -40, -50, -60), 35.0)
    close_row(report, grid, "G3 close time", T, 1000.0, -6, (-30, -40, -50), 36.0)
    worst = {"-6/-60": grid.worst(T, 1000.0, -6, -60), "-6/-70": grid.worst(T, 1000.0, -6, -70),
             "-20/-70": grid.worst(T, 1000.0, -20, -70)}
    report.add("G4 close time", "Tight 1 kHz, HPF 90", all(v <= 44.0 for v in worst.values()),
               " / ".join(f"{k}: {v:.1f}" for k, v in worst.items()) + " ms (limit 44)")
    off = {"-6/-60": grid.worst(T, 1000.0, -6, -60, hpf=20.0), "-6/-70": grid.worst(T, 1000.0, -6, -70, hpf=20.0),
           "-20/-70": grid.worst(T, 1000.0, -20, -70, hpf=20.0)}
    report.add("G4 close time", "Tight 1 kHz, HPF off", all(v <= 35.0 for v in off.values()),
               " / ".join(f"{k}: {v:.1f}" for k, v in off.items()) + " ms (limit 35)")
    close_row(report, grid, "G5 close time", T, 82.41, -20, (-40, -60), 50.0)
    cells = {(lv, t): grid.worst(T, 82.41, lv, t) for lv in LEVELS for t in THRESHOLDS}
    (lv, t), v = max(cells.items(), key=lambda kv: kv[1])
    report.add("G6 close time", "Tight 82.41 Hz, all 10 cells", v <= 53.0, f"max {v:.1f} ms at {lv} dBFS / thr {t} (limit 53)")

    shortest = min((grid.best(m, f, lv, t), m, f, lv, t) for m in (T, S) for f in FREQS for lv in LEVELS
                   for t in THRESHOLDS)
    report.add("G7 never shorter", "Tight + Smooth, all 40 cells", shortest[0] >= 18.0,
               f"min {shortest[0]:.1f} ms ({TYPES[shortest[1]]} {shortest[2]:g} Hz {shortest[3]} dBFS thr {shortest[4]})"
               " (limit >= 18)")

    # G8: open. Onset after silence, worst over 8 start phases.
    worst_open = 0.0
    for ph in range(PHASES):
        x, _ = fx.burst(1000.0, -20, 0.1, sr, pre=0.1, post=0.1, phase=2 * np.pi * ph / PHASES)
        g = gain_db(fx.run(EFFECT, x, dict(TIGHT, threshold=-40), ["--trace"], sr)[1])
        onset = int(round(0.1 * sr))
        worst_open = max(worst_open, np.nonzero(g[onset:] >= -1.0)[0][0] / sr * 1e3)
    report.add("G8 open", "Tight 1 kHz -20 dBFS thr -40", worst_open <= 1.0, f"{worst_open:.3f} ms to -1 dB (limit 1.0)")

    def chug_rows(row, srate, slack=""):
        med, worst_gap, first10, _ = chug_stats(dict(TIGHT, threshold=-40), srate)
        report.add(row, f"Tight chug thr -40{slack}", med <= -30.0 and first10 >= -1.0,
                   f"gap median {med:.1f} dB (worst {worst_gap:.1f}, limit -30); first 10 ms {first10:.2f} dB (limit -1)")

    chug_rows("G9 chug", sr)
    med, worst_gap, first10, _ = chug_stats(dict(SMOOTH, threshold=-40), sr)
    report.add("G10 chug", "Smooth (release 80) thr -40", -25.0 <= med <= -14.0 and first10 >= -0.5,
               f"gap median {med:.1f} dB (worst {worst_gap:.1f}, limit -25..-14); first 10 ms {first10:.2f} dB (limit -0.5)")

    for m in (T, S):
        g = gain_db(fx.run(EFFECT, fx.hover(sr), dict(TIGHT if m == T else SMOOTH, threshold=-40), ["--trace"], sr)[1])
        n = crossings(g, sr)
        report.add("G11 hover", f"{TYPES[m]} 1 kHz -40 +-3 dB, thr -40", n == 0, f"{n} crossings of -6 dB after 0.5 s")

    ok, parts = True, []
    for level, want_open in ((-45.0, True), (-45.5, True), (-46.5, False), (-47.0, False)):
        x = fx.step(sr, 1000.0, -20.0, level, 0.5, 1.5)
        g = gain_db(fx.run(EFFECT, x, dict(TIGHT, threshold=-40), ["--trace"], sr)[1])[int(1.3 * sr)]
        good = g >= -0.01 if want_open else g <= -89.0
        ok &= good
        parts.append(f"{level:g}: {g:.2f}" if g > -200 else f"{level:g}: mute")
    report.add("G12 step-down", "Tight thr -40, 0.8 s after", ok, " / ".join(parts) + " dB")

    ok, parts = True, []
    for level, want in ((-50, -8.0), (-55, -18.0), (-60, -28.0)):
        g = gain_db(fx.run(EFFECT, fx.sine(1000.0, level, 2.0, sr), dict(SMOOTH, threshold=-40), ["--trace"], sr)[1])
        v = float(np.median(g[int(1.5 * sr):]))
        ok &= abs(v - want) <= 0.5
        parts.append(f"{level}: {v:.2f}")
    report.add("G13 Smooth static", "1 kHz, thr -40", ok, " / ".join(parts) + " dB (want -8/-18/-28 +-0.5)")

    x, _ = fx.burst(1000.0, -20, 0.3, sr, pre=0.1, post=0.6)
    g20 = gain_db(fx.run(EFFECT, x, dict(TIGHT, threshold=-40, range=-20.0), ["--trace"], sr)[1])[-1]
    tr90 = fx.mono(fx.run(EFFECT, x, dict(TIGHT, threshold=-40, range=-90.0), ["--trace"], sr)[1])
    report.add("G14 range", "Tight closed", abs(g20 + 20.0) <= 0.1 and tr90[-1] == 0.0,
               f"range -20: {g20:.3f} dB; range -90: gain {tr90[-1]:g}")

    # G15 as written (thr -50) cannot be met by any gate: the chug's own -66 dBFS RMS noise
    # peaks at -55 dBFS in the 12 ms window, over the -56 dBFS close level. Enforced at thr -40
    # (coordinator decision); the -50 result is reported only.
    med_on, _, _, _ = chug_stats(dict(TIGHT, threshold=-40, sc_hpf=90.0), sr, extra=hum(-44.0))
    med_off, _, _, _ = chug_stats(dict(TIGHT, threshold=-40, sc_hpf=20.0), sr, extra=hum(-44.0))
    report.add("G15 SC HPF", "50 Hz hum -44 + chug, thr -40", med_on <= -30.0 and med_off >= -1.0,
               f"HPF 90 gap median {med_on:.1f} dB (limit -30); HPF off {med_off:.1f} dB (open = >= -1)")
    med_on, _, _, _ = chug_stats(dict(TIGHT, threshold=-50, sc_hpf=90.0), sr, extra=hum(-44.0))
    med_off, _, _, _ = chug_stats(dict(TIGHT, threshold=-50, sc_hpf=20.0), sr, extra=hum(-44.0))
    info(report, "G15 at thr -50", "not enforced: invalid test",
         f"HPF 90 gap median {med_on:.1f} dB, HPF off {med_off:.1f} dB: the chug noise alone peaks at "
         "-55 dBFS, over the -56 dBFS close level, so no gate can close there")

    # G16 is a processor test (§6.1). Proxy: lag=D delays the deferred audio as the amp would;
    # with delay=D the output must be the undelayed render, D samples later, bit for bit.
    ok, detail = True, ""
    x = np.vstack([fx.burst(1000.0, -20, 0.3, sr, pre=0.1, post=0.4)[0]] * 2)
    for m in (T, S):
        for block in (64, 512):
            ref = fx.mono(fx.run(EFFECT, x, dict(TIGHT if m == T else SMOOTH, threshold=-40, deferred=1),
                                 [f"--block={block}"], sr)[0])
            for d in (1, 37, 512, 3000):
                out = fx.mono(fx.run(EFFECT, x, dict(TIGHT if m == T else SMOOTH, threshold=-40, deferred=1, lag=d,
                                                     delay=d), [f"--block={block}"], sr)[0])
                if not np.array_equal(out[d:], ref[:-d]):
                    ok, detail = False, f"{TYPES[m]} block {block} D {d}: max |diff| {np.max(np.abs(out[d:] - ref[:-d])):.3g}"
    report.add("G16 deferred align", "fxbench proxy (lag = delay)", ok,
               detail or "bit-exact at D = 1/37/512/3000, blocks 64/512; processor test is the integrator's")

    for srate in (44100, 96000):
        close_row(report, grid, "G17 rates", T, 1000.0, -20, (-30, -40, -50, -60), 36.0, sr=srate)
        close_row(report, grid, "G17 rates", T, 82.41, -20, (-40, -60), 51.0, sr=srate)
        chug_rows("G17 rates", srate, f" {srate} Hz")

    # G18: meters, per sample (block 1).
    x, end = fx.burst(1000.0, -20, 0.3, sr, pre=0.1, post=0.4)
    r = fx.invoke(EFFECT, x, dict(TIGHT, threshold=-40), ["--block=1", "--meters"], sr)
    m = meters(r)
    state, gr, level = m[:, 1], m[:, 2], m[:, 3]
    first2 = int(np.nonzero(state == 2)[0][0])
    first_above = int(np.nonzero(level >= -40.0)[0][0])
    closed_after = np.nonzero(state[end:] == 0)[0]
    t_closed = closed_after[0] / sr * 1e3 if len(closed_after) else float("inf")
    ok = first2 == first_above and t_closed <= 100.0 and gr[-1] == 90.0 and state[-1] == 0
    report.add("G18 meter", "Tight thr -40, block 1", ok,
               f"state 2 at sample {first2 - int(0.1 * sr)} after onset, first above thr {first_above - int(0.1 * sr)}; "
               f"state 0 {t_closed:.1f} ms after note end; GR closed {gr[-1]:g} dB")


def extra_checks(report, args, grid):
    sr = 48000
    T, S = 1, 2

    # X1: Smooth runs Tight's detector and state machine, so after a note it closes exactly
    # its longer hold plus 20 dB x (80 - 30) / 60 ms later than Tight; any more means its
    # expander law held the gain up after the note (the grid itself is printed with --grid).
    lag = SMOOTH["hold"] - TIGHT["hold"] + 20.0 * (SMOOTH["release"] - TIGHT["release"]) / 60.0
    excess = max((grid.worst(S, f, lv, t) - grid.worst(T, f, lv, t) - lag, f, lv, t)
                 for f in FREQS for lv in LEVELS for t in THRESHOLDS)
    worst = max((grid.worst(S, f, lv, t), f, lv, t) for f in FREQS for lv in LEVELS for t in THRESHOLDS)
    report.add("X1 Smooth close", "release 80, all 20 cells", excess[0] <= 1.0,
               f"worst {worst[0]:.1f} ms ({worst[1]:g} Hz {worst[2]} dBFS thr {worst[3]}); "
               f"max over Tight + {lag:.1f} ms: {excess[0]:+.2f} ms (limit +1)")

    # X2: palm-mute gaps actually mute (G9's bound).
    parts, ok = [], True
    for gap in (0.050, 0.075, 0.100):
        med, worst_gap, _, _ = chug_stats(dict(TIGHT, threshold=-40), sr, period=0.075 + gap)
        ok &= med <= -30.0
        parts.append(f"{gap * 1e3:.0f} ms: {med:.1f} (worst {worst_gap:.1f})")
    report.add("X2 palm-mute gaps", "Tight chug thr -40, 75 ms notes", ok, "; ".join(parts) + " dB (limit -30)")

    # X3: a decaying note is not chopped early: while the note's sidechain peak is still at
    # least 0.5 dB over the close level, the gain stays at 0 dB.
    worst, where = 0.0, ""
    for m in (T, S):
        for freq in (82.41, 1000.0):
            for tau in (0.4, 1.5):
                t = np.arange(int((tau * 9 + 0.5) * sr)) / sr
                env = fx.amp(-6.0) * np.exp(-t / tau)
                x = env * np.sin(2 * np.pi * freq * t)
                g = gain_db(fx.run(EFFECT, x, dict(TIGHT if m == T else SMOOTH, threshold=-40), ["--trace"], sr)[1])
                w = (freq / 90.0) ** 2
                sc_env = env * w / np.sqrt(1.0 + w * w)
                live = sc_env >= fx.amp(-46.0 + 0.5)
                live[:int(0.005 * sr)] = False  # the gate opening from reset is G8's business
                v = float(np.min(g[live]))
                if v < worst:
                    worst, where = v, f"{TYPES[m]} {freq:g} Hz tau {tau} s"
    report.add("X3 decay not chopped", "thr -40, -6 dBFS exp decays", worst >= -0.1,
               f"min gain above close+0.5 dB: {worst:.3f} dB{' (' + where + ')' if where else ''}")

    # X4: no chatter on stationary noise at levels around the threshold: the state machine
    # changes between closed and open at most twice in 5.5 s. Counted exactly on Tight's gain,
    # which is exactly 1 only while open (the first closed sample already lowers it); Smooth
    # runs the same state machine, and its expander gain follows the noise peaks by design.
    worst, where = 0, ""
    for rms in range(-62, -44, 2):
        n = open_changes(fx.run(EFFECT, fx.white(6.0, rms, sr, seed=rms + 100), dict(TIGHT, threshold=-40),
                                ["--trace"], sr)[1], int(0.5 * sr))
        if n > worst:
            worst, where = n, f"noise {rms} dBFS RMS"
    report.add("X4 no chatter", "white noise -62..-46 dBFS RMS, thr -40", worst <= 2,
               f"max {worst} open/close changes in 5.5 s{' (' + where + ')' if where else ''} (limit 2)")

    # X5: opening does not eat the pick attack: level lost in the first 5 ms of a note.
    _, _, _, chug5 = chug_stats(dict(TIGHT, threshold=-40), sr)
    abrupt = 0.0
    for ph in range(PHASES):
        x, _ = fx.burst(1000.0, -20, 0.1, sr, pre=0.1, post=0.1, phase=2 * np.pi * ph / PHASES)
        y = fx.mono(fx.run(EFFECT, x, dict(TIGHT, threshold=-40), [], sr)[0])
        o, w5 = int(round(0.1 * sr)), int(round(0.005 * sr))
        abrupt = min(abrupt, 10 * np.log10(np.sum(y[o:o + w5] ** 2) / np.sum(x[o:o + w5] ** 2)))
    report.add("X5 pick attack", "Tight, first 5 ms of a note", chug5 >= -1.0 and abrupt >= -1.0,
               f"chug notes {chug5:.2f} dB; abrupt 1 kHz onset (worst of 8 phases) {abrupt:.2f} dB (limit -1)")

    # X6: mode switches do not click. During a switch g = (1 - w) a + w b, so beyond what the
    # two curves do on their own (rendered steady, same input) the switch may add only
    # dw |b - a| per sample: at most a full-scale 20 ms linear fade. Limit 1.5 x that.
    schedule = [(1.0, 1, 0), (1.5, 0, 2), (2.0, 2, 1), (2.5, 1, 2), (3.0, 2, 0), (3.5, 0, 1)]
    seq = ["mode=1"] + [f"mode@{t}={b}" for t, _, b in schedule] + ["threshold=-40", "release=30"]
    limit = 1.5 / (0.02 * sr)
    worst, where = 0.0, ""
    signals = {"1 kHz -20 dBFS (open)": fx.sine(1000.0, -20, 4.2, sr),
               "1 kHz -50 dBFS (closed)": fx.sine(1000.0, -50, 4.2, sr),
               "noise -66 dBFS RMS": fx.white(4.2, -66, sr)}
    for name, x in signals.items():
        for block in (64, 512):
            g = fx.mono(fx.run(EFFECT, x, seq, [f"--block={block}", "--trace"], sr)[1])
            steady = {m: fx.mono(fx.run(EFFECT, x, {"mode": m, "threshold": -40, "release": 30},
                                        [f"--block={block}", "--trace"], sr)[1]) for m in (0, 1, 2)}
            for t, a, b in schedule:
                lo, hi = int(t * sr) - block, int((t + 0.07) * sr)
                dg = np.abs(np.diff(g[lo:hi]))
                own = np.maximum(np.abs(np.diff(steady[a][lo:hi])), np.abs(np.diff(steady[b][lo:hi])))
                excess = float(np.max(dg - own))
                if excess > worst:
                    worst, where = excess, f"{name}, {TYPES[a]} -> {TYPES[b]}, block {block}"
    report.add("X6 switch clicks", "all 6 transitions, steady input", worst <= limit,
               f"max added gain step {worst:.2e}/sample ({where}); limit {limit:.2e}")

    # X9: a harmonic-rich low note decaying through the threshold opens once and closes once.
    # The window + hold span must bridge the note's strongest crests a full period apart; 41.2 Hz
    # (8-string drop E) is the tightest case the 12 ms window was sized for (42 Hz).
    t = np.arange(int(8 * sr)) / sr
    worst, where = 0, ""
    for freq in (41.2, 46.25, 82.41):
        for tau in (0.7, 1.5, 3.0):
            env = fx.amp(-6.0) * np.exp(-t / tau)
            x = env * sum(a * np.sin(2 * np.pi * freq * h * t) for h, a in zip((1, 2, 3, 4), (1, 0.6, 0.4, 0.25))) / 2.25
            for thr in (-40, -55):
                n = open_changes(fx.run(EFFECT, x, dict(TIGHT, threshold=thr), ["--trace"], sr)[1])
                if n > worst:
                    worst, where = n, f"{freq:g} Hz tau {tau} s thr {thr}"
    report.add("X9 low-note decay", "Tight, 41.2/46.25/82.41 Hz decays", worst <= 2,
               f"max {worst} open/close changes ({where}) (limit 2: one open, one close)")

    # X7: the close-time rows at the ends of the rate range (G17's +1 ms rule).
    for srate in (22050, 192000):
        close_row(report, grid, "X7 rates", T, 1000.0, -20, (-30, -40, -50, -60), 36.0, sr=srate)
        close_row(report, grid, "X7 rates", T, 82.41, -20, (-40, -60), 51.0, sr=srate)


def print_grid(grid):
    for m in (1, 2):
        print(f"\nclose time, {TYPES[m]} (release {30 if m == 1 else 80} ms, hold 10, HPF 90), "
              "worst / best over 32 note-off phases, ms")
        for f in FREQS:
            print(f"  {f:g} Hz   " + "".join(f"thr {t:<9}" for t in THRESHOLDS))
            for lv in LEVELS:
                cells = "".join(f"{grid.worst(m, f, lv, t):5.1f}/{grid.best(m, f, lv, t):<6.1f}" for t in THRESHOLDS)
                print(f"  {lv:>4} dBFS  {cells}")


def main():
    argv = list(sys.argv)
    show_grid = "--grid" in argv
    if show_grid:
        sys.argv.remove("--grid")
    args = fx.parse_args(__doc__)
    report = fx.Report(EFFECT)
    fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                     quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
    grid = Grid()
    specific_checks(report, args, grid)
    extra_checks(report, args, grid)
    if show_grid:
        print_grid(grid)
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
