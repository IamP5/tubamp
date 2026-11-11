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
from concurrent.futures import ThreadPoolExecutor
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
# Since the adaptive wait (Gate.cpp kDeepFallDb...) the hold is the wait after a note stops; a
# shallow, slow fall below the close level is ridden for up to 0.4 s (X10-X12).
TIGHT = {"mode": 1, "release": 30.0, "hold": 6.0, "range": -90.0, "sc_hpf": 75.0}
SMOOTH = {"mode": 2, "release": 80.0, "hold": 10.0, "range": -90.0, "sc_hpf": 75.0}
HPF = TIGHT["sc_hpf"]   # Gate.h kGateScHpfDefault: 4th-order Butterworth (was 90 Hz, 2nd order)
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

    def get(self, mode, freq, level, thr, sr=48000, hpf=HPF):
        key = (mode, freq, level, thr, sr, hpf)
        if key not in self.cells:
            base = dict(TIGHT if mode == 1 else SMOOTH, threshold=thr, sc_hpf=hpf)
            self.cells[key] = close_times(freq, level, base, sr)
        return self.cells[key]

    def worst(self, *a, **k):
        return max(self.get(*a, **k))

    def best(self, *a, **k):
        return min(self.get(*a, **k))


def close_row(report, grid, row, mode, freq, level, thrs, bound, sr=48000, hpf=HPF, scope=None):
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
    report.add("G4 close time", f"Tight 1 kHz, HPF {HPF:g}", all(v <= 44.0 for v in worst.values()),
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
    med_on, _, _, _ = chug_stats(dict(TIGHT, threshold=-40), sr, extra=hum(-44.0))
    med_off, _, _, _ = chug_stats(dict(TIGHT, threshold=-40, sc_hpf=20.0), sr, extra=hum(-44.0))
    report.add("G15 SC HPF", "50 Hz hum -44 + chug, thr -40", med_on <= -30.0 and med_off >= -1.0,
               f"HPF {HPF:g} gap median {med_on:.1f} dB (limit -30); HPF off {med_off:.1f} dB (open = >= -1)")
    med_on, _, _, _ = chug_stats(dict(TIGHT, threshold=-50), sr, extra=hum(-44.0))
    med_off, _, _, _ = chug_stats(dict(TIGHT, threshold=-50, sc_hpf=20.0), sr, extra=hum(-44.0))
    info(report, "G15 at thr -50", "not enforced: invalid test",
         f"HPF {HPF:g} gap median {med_on:.1f} dB, HPF off {med_off:.1f} dB: the chug noise alone peaks at "
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
                w = (freq / HPF) ** 4
                sc_env = env * w / np.sqrt(1.0 + w * w)   # 4th-order Butterworth magnitude
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


# ---------------------------------------------------------------------------- verifier signals
# The independent verification's reproduction signals (build-verify/dynamics/gb.py, gb2.py,
# gb3b.py, gc.py, gd2.py), rebuilt here sample for sample so the rows run on the engine.

def pluck(f0, secs, tau, sr, level=-6.0, nh=12, am_depth=0.0, am_rate=0.0, seed=0):
    """gb.py's pluck: 12 harmonics with differential decay, random phases, 1.5 ms rise."""
    n = int(secs * sr)
    t = np.arange(n) / sr
    r = np.random.default_rng(seed)
    y = np.zeros(n)
    for k in range(1, nh + 1):
        if k * f0 > 0.45 * sr:
            break
        y += (1.0 / k) * np.exp(-t * (1 + 0.35 * (k - 1)) / tau) * np.sin(k * 2 * np.pi * f0 * t + r.uniform(0, 2 * np.pi))
    y *= 1 - np.exp(-t / 0.0015)
    if am_depth:
        y *= 1 + am_depth * np.sin(2 * np.pi * am_rate * t)
    return y * fx.amp(level) / np.max(np.abs(y))


def floor_noise(n, level_db, seed=5):
    return np.random.default_rng(seed).standard_normal(n) * fx.amp(level_db)


CHORDS = [("E2+B2", (82.41, 123.47)), ("E2+B2+E3", (82.41, 123.47, 164.81)), ("D2+A2", (73.42, 110.0)),
          ("D2+A2+D3", (73.42, 110.0, 146.83)), ("C2+G2", (65.41, 98.0)), ("C2+G2+C3", (65.41, 98.0, 130.81)),
          ("B1+F#2", (61.74, 92.5)), ("A1+E2", (55.0, 82.41)), ("A1+E2+A2", (55.0, 82.41, 110.0)),
          ("F#1+C#2", (46.25, 69.3)), ("E1+B1", (41.2, 61.74)), ("E2+G#2", (82.41, 103.83)), ("E2+G2", (82.41, 98.0)),
          ("open E", (82.41, 123.47, 164.81, 207.65, 246.94, 329.63))]
HARD_CHORDS = ("C2+G2+C3", "A1+E2+A2", "E1+B1", "E2+G2")
TWO_STRINGS = [("unison +0.5 Hz", 82.41, 82.91), ("unison +2 Hz", 82.41, 84.41), ("unison +6 Hz", 110.0, 116.0),
               ("fifth E2+B2", 82.41, 123.47), ("octave E1+E2", 41.2, 82.41), ("fifth E1+B1", 41.2, 61.74),
               ("min2 E2+F2", 82.41, 87.31), ("B0+F#1", 30.87, 46.25)]


def chord_signal(freqs, sr, tau=1.5, secs=12.0):
    """gb2.py: strings decaying at tau (each 7 % faster than the last), 0.1 s lead-in, -85 dBFS floor."""
    x = sum(pluck(f, secs, tau * (1 - 0.07 * i), sr, seed=i + 1) for i, f in enumerate(freqs)) / len(freqs)
    x = np.concatenate([np.zeros(int(0.1 * sr)), x])
    return x + floor_noise(len(x), -85)


def two_strings(fa, fb, sr, tau=2.5, secs=12.0):
    x = 0.5 * (pluck(fa, secs, tau, sr, seed=1) + pluck(fb, secs, tau * 0.9, sr, seed=2))
    return x + floor_noise(len(x), -80)


def am_note(f0, depth, rate, sr):
    x = pluck(f0, 12.0, 2.5, sr, am_depth=depth, am_rate=rate)
    return x + floor_noise(len(x), -80)


def single_note(f0, tau, sr):
    x = pluck(f0, min(12.0, 1.0 + tau * 9), tau, sr)
    x = np.concatenate([np.zeros(int(0.1 * sr)), x])
    return x + floor_noise(len(x), -80)


def picked_notes(f0, level, sr, reps=12):
    """gc.py's pluck with a pick click: 150 ms notes after 350 ms of silence."""
    rng = np.random.default_rng(int(f0) + reps)
    n = int(0.15 * sr)
    t = np.arange(n) / sr
    pieces, onsets, pos = [np.zeros(int(0.1 * sr))], [], int(0.1 * sr)
    for _ in range(reps):
        rng.uniform(0, 2 * np.pi)
        y = sum((1.0 / k) * np.exp(-t * (1 + 0.35 * (k - 1)) / 0.4) * np.sin(2 * np.pi * k * f0 * t + rng.uniform(0, 2 * np.pi))
                for k in range(1, 13))
        y *= 1 - np.exp(-t / 0.0003)
        y += 0.5 * np.max(np.abs(y)) * rng.standard_normal(n) * np.exp(-t / 0.0007)
        y = y / np.max(np.abs(y)) * fx.amp(level)
        y[-96:] *= np.linspace(1, 0, 96)
        onsets.append(pos)
        pieces += [y, np.zeros(int(0.35 * sr))]
        pos += n + int(0.35 * sr)
    return np.concatenate(pieces), onsets


def floors(sr, secs=6.0):
    """gd2.py's stationary inputs below a -40 dB threshold."""
    n = int(secs * sr)
    t = np.arange(n) / sr
    noise = lambda level: np.random.default_rng(3).standard_normal(n) * fx.amp(level)  # noqa: E731
    buzz = sum(np.sin(2 * np.pi * 60 * k * t) / k for k in range(1, 31))
    return {"white -58 dBFS RMS": noise(-58), "white -62": noise(-62), "white -66": noise(-66),
            "hum 50+100+150 Hz -52 pk": fx.amp(-52) * (np.sin(2 * np.pi * 50 * t) + 0.5 * np.sin(2 * np.pi * 100 * t + 1)
                                                       + 0.35 * np.sin(2 * np.pi * 150 * t + 2)) / 1.5 + noise(-70),
            "60 Hz buzz -52 pk": buzz / np.max(np.abs(buzz)) * fx.amp(-52) + noise(-70)}


def parallel(fn, items):
    with ThreadPoolExecutor(max_workers=8) as pool:
        return list(pool.map(fn, items))


def gap_energy(keys, gap, sr=48000, extra=None):
    """Palm-mute chug with `gap` ms between 75 ms notes: per gap the gain energy over its last
    10 ms (what the verifier measured from the audio); -> (median, worst) in dB."""
    x = fx.chug(sr, secs=3.0, note=0.075, period=0.075 + gap)
    if extra is not None:
        x = x + extra(sr, len(x))
    g = fx.mono(fx.run(EFFECT, x, keys, ["--trace"], sr)[1])
    nn, step = int(round(0.075 * sr)), int(round((0.075 + gap) * sr))
    w = int(round(0.010 * sr))
    e = [10 * np.log10(max(np.mean(g[o - w:o] ** 2), 1e-12)) for o in range(step, len(x) - nn, step)]
    return float(np.median(e)), float(max(e))


def splatter(gain, freqs, sr):
    """Worst share (dB) of energy more than 1.5 kHz above twice the tone, over 16 phases, when a
    gain ramp is applied to a steady tone (the click an opening adds)."""
    n = len(gain)
    t = np.arange(n) / sr
    w = np.hanning(n)
    f = np.fft.rfftfreq(n, 1 / sr)
    out = {}
    for freq in freqs:
        worst = -1e9
        for ph in np.linspace(0, 2 * np.pi, 16, endpoint=False):
            spec = np.abs(np.fft.rfft(gain * np.sin(2 * np.pi * freq * t + ph) * w)) ** 2
            worst = max(worst, 10 * np.log10(np.sum(spec[f > 2 * freq + 1500]) / np.sum(spec)))
        out[freq] = worst
    return out


def robustness_checks(report, args, grid):
    """The defects the independent verification found with chords, noise floors, picked
    attacks and bad samples (.audit/fx-verify/dynamics.md, code.md), as enforced rows."""
    sr = 48000
    T = 1

    def changes(item):
        x, keys, srate = item
        return open_changes(fx.run(EFFECT, x, keys, ["--trace"], srate)[1])

    # X10: decaying chords, every verifier case, opens once and closes once.
    sigs = {name: chord_signal(fr, sr) for name, fr in CHORDS}
    cells = [(name, thr) for name, _ in CHORDS for thr in range(-30, -70, -5)]
    counts = parallel(changes, [(sigs[n], dict(TIGHT, threshold=t), sr) for n, t in cells])
    bad = [f"{n} thr {t}: {c}" for (n, t), c in zip(cells, counts) if c > 2]
    report.add("X10 chord decay", "Tight, 14 chords x thr -30..-65", not bad,
               f"{len(bad)} of {len(cells)} cells with more than one open + one close"
               + (": " + "; ".join(bad[:6]) if bad else f" (max {max(counts)})"))
    for srate in ((44100, 96000) if args.quick else (22050, 44100, 96000, 192000)):
        sigs = {name: chord_signal(fr, srate) for name, fr in CHORDS if name in HARD_CHORDS}
        cells = [(name, thr) for name in HARD_CHORDS for thr in range(-30, -70, -5)]
        counts = parallel(changes, [(sigs[n], dict(TIGHT, threshold=t), srate) for n, t in cells])
        bad = [f"{n} thr {t}: {c}" for (n, t), c in zip(cells, counts) if c > 2]
        report.add("X10 chord decay", f"Tight, 4 hardest chords, {srate} Hz", not bad,
                   f"{len(bad)} of {len(cells)} cells chatter" + (": " + "; ".join(bad[:6]) if bad else ""))

    # X11: beating, amplitude-modulated notes and two strings a few hertz apart.
    cells = [(f, d, r) for f in (82.41, 110.0) for d in (0.2, 0.33, 0.5, 0.7) for r in (1, 3, 7, 15, 30)]
    counts = parallel(changes, [(am_note(f, d, r, sr), dict(TIGHT, threshold=-40), sr) for f, d, r in cells])
    bad = [f"{f:g} Hz depth {d} at {r} Hz: {c}" for (f, d, r), c in zip(cells, counts) if c > 2]
    report.add("X11 beating", "Tight, AM 3.5-15 dB x 1-30 Hz, thr -40", not bad,
               f"{len(bad)} of {len(cells)} chatter" + (": " + "; ".join(bad[:6]) if bad else f" (max {max(counts)})"))
    cells = [(name, fa, fb, t) for name, fa, fb in TWO_STRINGS for t in (-30, -40, -50, -60)]
    sigs = {name: two_strings(fa, fb, sr) for name, fa, fb in TWO_STRINGS}
    counts = parallel(changes, [(sigs[n], dict(TIGHT, threshold=t), sr) for n, _, _, t in cells])
    bad = [f"{n} thr {t}: {c}" for (n, _, _, t), c in zip(cells, counts) if c > 2]
    report.add("X11 beating", "Tight, two strings, thr -30..-60", not bad,
               f"{len(bad)} of {len(cells)} chatter" + (": " + "; ".join(bad[:6]) if bad else f" (max {max(counts)})"))

    # X12: single notes decaying into a -80 dBFS floor still open and close once.
    cells = [(f, tau, t) for f in (41.2, 55.0, 82.41, 110.0, 196.0, 330.0) for tau in (0.15, 0.4, 1.0, 2.5)
             for t in (-20, -30, -40, -50, -60, -70)]
    sigs = {(f, tau): single_note(f, tau, sr) for f, tau, _ in cells}
    counts = parallel(changes, [(sigs[(f, tau)], dict(TIGHT, threshold=t), sr) for f, tau, t in cells])
    bad = [f"{f:g} Hz tau {tau} thr {t}: {c}" for (f, tau, t), c in zip(cells, counts) if c > 2]
    report.add("X12 single decays", "Tight, 41.2-330 Hz x tau x thr", not bad,
               f"{len(bad)} of {len(cells)} chatter" + (": " + "; ".join(bad[:6]) if bad else f" (max {max(counts)})"))

    # X13: palm-mute gaps, by the energy of the gain over each gap's last 10 ms.
    res = {gap: gap_energy(dict(TIGHT, threshold=-40), gap) for gap in (0.030, 0.040, 0.050, 0.075, 0.100)}
    med50, worst50 = res[0.050]
    report.add("X13 palm-mute gaps", "Tight thr -40, 50 ms gaps", med50 <= -36.0 and worst50 <= -30.0,
               f"median {med50:.1f} dB (limit -36), worst gap {worst50:.1f} dB (limit -30)")
    report.add("X13 palm-mute gaps", "Tight thr -40, 40 ms gaps", res[0.040][0] <= -20.0,
               f"median {res[0.040][0]:.1f} dB (limit -20), worst gap {res[0.040][1]:.1f} dB")
    info(report, "X13 gap table", "Tight thr -40, energy, last 10 ms",
         " / ".join(f"{g * 1e3:.0f} ms {m:.1f} (worst {w:.1f})" for g, (m, w) in res.items()) + " dB")
    hummed = {gap: gap_energy(dict(TIGHT, threshold=-40), gap, extra=hum(-44.0)) for gap in (0.040, 0.050)}
    info(report, "X13 gaps over hum", "Tight thr -40, 50 Hz hum -44 dBFS",
         " / ".join(f"{g * 1e3:.0f} ms {m:.1f} (worst {w:.1f})" for g, (m, w) in hummed.items()) + " dB")

    # X14: Smooth holds still on a stationary floor below the threshold.
    worst_move, worst_step, where = 0.0, 0.0, ""
    for srate in (44100, 48000, 96000):
        for name, x in floors(srate).items():
            g = fx.mono(fx.run(EFFECT, np.vstack([x, x]), dict(SMOOTH, threshold=-40), ["--block=64", "--trace"], srate)[1])
            gd = fx.db(np.maximum(g[srate:], 1e-9))
            win = int(round(0.010 * srate))
            view = np.lib.stride_tricks.sliding_window_view(gd, win)
            move = float(np.max(view.max(axis=1) - view.min(axis=1)))
            step = float(np.max(np.abs(np.diff(gd))))
            if move > worst_move or step > worst_step:
                where = f"{name}, {srate} Hz"
            worst_move, worst_step = max(worst_move, move), max(worst_step, step)
    report.add("X14 Smooth floor", "noise / hum / buzz under thr -40", worst_move <= 0.5 and worst_step <= 0.1,
               f"max gain move in 10 ms {worst_move:.2f} dB (limit 0.5), max one-sample step {worst_step:.3f} dB "
               f"(limit 0.1) ({where})")

    # X15: the front of a picked note: energy kept in the first millisecond, 12-34 dB over thr.
    worst, peaks = 0.0, []
    for f0 in (82.41, 196.0, 1000.0):
        lost1, pk = 0.0, 0.0
        for over in (12, 20, 34):
            x, onsets = picked_notes(f0, -40 + over, sr)
            y = fx.mono(fx.run(EFFECT, x, dict(TIGHT, threshold=-40), ["--block=64"], sr)[0])
            w = int(0.001 * sr)
            lost1 = min(lost1, min(10 * np.log10(max(np.sum(y[o:o + w] ** 2), 1e-30) / np.sum(x[o:o + w] ** 2)) for o in onsets))
            pk = min(pk, min(fx.db(np.max(np.abs(y[o:o + 240])) / np.max(np.abs(x[o:o + 240]))) for o in onsets))
        worst = min(worst, lost1)
        peaks.append(f"{f0:g} Hz {lost1:.2f} / peak {pk:.2f}")
    report.add("X15 pick attack", "Tight, picked notes thr+12..34 dB", worst >= -3.0,
               "first 1 ms / peak of first 5 ms: " + "; ".join(peaks) + " dB (first-ms limit -3)")

    # X16: the opening adds no more splatter than the 0.5 ms / 60 dB dB-ramp it replaced.
    x, _ = fx.burst(1000.0, -20, 0.1, sr, pre=0.05, post=0.05)
    onset = int(round(0.05 * sr))
    g = fx.mono(fx.run(EFFECT, x, dict(TIGHT, threshold=-40), ["--block=1", "--trace"], sr)[1])
    seg = slice(onset - 2400, onset + 2400)
    old = np.zeros(len(g))
    level = -90.0
    for i in range(onset, len(g)):
        level = min(0.0, level + 60.0 / (0.0005 * sr))
        old[i] = fx.amp(level)
    new_s, old_s = splatter(g[seg], (82.41, 200.0, 1000.0), sr), splatter(old[seg], (82.41, 200.0, 1000.0), sr)
    t_open = np.nonzero(g[onset:] >= fx.amp(-1.0))[0][0] / sr * 1e3
    report.add("X16 opening click", "Tight, splatter vs the old ramp", all(new_s[f] <= old_s[f] for f in new_s),
               " / ".join(f"{f:g} Hz {new_s[f]:.1f} (old {old_s[f]:.1f})" for f in new_s) + f" dB; -1 dB after {t_open:.3f} ms")

    # X17: NaN and Inf bursts, in each mode and across a switch: nothing non-finite stays in a
    # state, so the gain is exactly that of the same input with zeros in place of the bursts.
    base = np.concatenate([fx.burst(1000.0, -20, 0.3, sr, pre=0.1, post=0.4)[0], fx.sine(1000.0, -100, 1.0, sr)])
    jobs = []
    for bad_value in (np.nan, np.inf, -np.inf):
        y = base.copy()
        for start in (int(0.2 * sr), int(0.6 * sr)):
            y[start:start + 8] = bad_value
        zero = np.where(np.isfinite(y), y, 0.0)
        for keys in ({"mode": 0}, {"mode": 1}, {"mode": 2}, ["mode=1", "mode@1.0=0"], ["mode=2", "mode@0.7=1"]):
            keys = dict(keys, threshold=-40) if isinstance(keys, dict) else keys + ["threshold=-40"]
            for block in (1, 37, 512):
                for ch in (1, 2):
                    jobs.append((bad_value, keys, block, ch, y, zero))

    def same_as_zeros(job):
        bad_value, keys, block, ch, y, zero = job
        yy, zz = (y, zero) if ch == 1 else (np.vstack([y, y]), np.vstack([zero, zero]))
        opts = [f"--block={block}", "--trace", "--meters"]
        a, b = fx.invoke(EFFECT, yy, keys, opts, sr), fx.invoke(EFFECT, zz, keys, opts, sr)
        return np.array_equal(fx.mono(a.trace), fx.mono(b.trace)) and bool(np.all(np.isfinite(meters(a))))

    results = parallel(same_as_zeros, jobs)
    failed = [f"{j[0]} {j[1]} block {j[2]} {j[3]} ch" for j, good in zip(jobs, results) if not good]
    ok, renders = not failed, len(jobs)
    detail = ("gain differs or meters not finite: " + "; ".join(failed[:3])) if failed else ""
    report.add("X17 non-finite input", "NaN/Inf bursts, every mode + switches", ok,
               detail or f"gain equals the zero-substituted render, meters finite ({renders} pairs)")

    # X18: blocks larger than the prepared maximum pass ungated, but the detector follows them
    # and the meters are those of the same input in normal blocks (Classic: open, 0 dB).
    x = fx.burst(1000.0, -20, 0.3, sr, pre=0.1, post=0.4)[0]
    ok, parts = True, []
    for mode in (0, 1, 2):
        for ch in (1, 2):
            xx = x if ch == 1 else np.vstack([x, x])
            ref = fx.invoke(EFFECT, xx, {"mode": mode, "threshold": -40}, ["--block=512", "--meters"], sr)
            big = fx.invoke(EFFECT, xx, {"mode": mode, "threshold": -40, "prepare_block": 256}, ["--block=512", "--meters"], sr)
            mr, mb = meters(ref), meters(big)
            passed = np.array_equal(fx.mono(big.out), np.atleast_2d(xx)[0].astype(np.float32))
            if mode == 0:
                lvl = fx.db(np.max(np.abs(x.reshape(-1)[:len(mb) * 512].reshape(len(mb), 512)), axis=1))
                good = passed and np.all(mb[:, 1] == 2) and np.all(mb[:, 2] == 0.0) and np.allclose(mb[:, 3], np.maximum(lvl, -120.0), atol=1e-3)
            else:
                good = passed and np.array_equal(mr[:, 1:], mb[:, 1:]) and len(set(mb[:, 1])) == 3
            ok &= bool(good)
            if not good:
                parts.append(f"{TYPES[mode]} {ch} ch")
    report.add("X18 oversized blocks", "block 512 on a 256-sample prepare", ok,
               "audio unchanged, meters live in every mode, mono + stereo" if ok else "stale or wrong: " + ", ".join(parts))

    # X19: curve delays past the old 8192 clamp, up to what the processor can ask for.
    ok, detail = True, ""
    x = np.vstack([np.concatenate([fx.burst(1000.0, -20, 0.3, sr, pre=0.1, post=0.4)[0]] * 4)] * 2)
    for m in (1, 2):
        keys = dict(TIGHT if m == 1 else SMOOTH, threshold=-40, deferred=1)
        refo = fx.mono(fx.run(EFFECT, x, keys, ["--block=512"], sr)[0])
        for d in (8193, 30000, 65535):
            out = fx.mono(fx.run(EFFECT, x, dict(keys, lag=d, delay=d), ["--block=512"], sr)[0])
            if not np.array_equal(out[d:], refo[:-d]):
                ok, detail = False, f"{TYPES[m]} D {d}: max |diff| {np.max(np.abs(out[d:] - refo[:-d])):.3g}"
    report.add("X19 long curve delay", "deferred, D = 8193 / 30000 / 65535", ok, detail or "bit-exact")

    # X20: what the threshold means for low notes through the default sidechain filter.
    parts = []
    for f in (41.2, 73.42, 82.41, 110.0):
        lo, hi = -10.0, 30.0
        while hi - lo > 0.05:
            mid = (lo + hi) / 2
            g = fx.mono(fx.run(EFFECT, fx.sine(f, -40 + mid, 1.0, sr), dict(TIGHT, threshold=-40), ["--trace"], sr)[1])
            lo, hi = (lo, mid) if np.max(g[sr // 2:]) >= 1.0 else (mid, hi)
        parts.append(f"{f:g} Hz +{hi:.1f}")
    info(report, "X20 low-note threshold", "steady sine needed to open, thr -40", " / ".join(parts) + " dB over threshold")


def print_grid(grid):
    for m in (1, 2):
        d = TIGHT if m == 1 else SMOOTH
        print(f"\nclose time, {TYPES[m]} (release {d['release']:g} ms, hold {d['hold']:g}, HPF {HPF:g}), "
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
    robustness_checks(report, args, grid)
    if show_grid:
        print_grid(grid)
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
