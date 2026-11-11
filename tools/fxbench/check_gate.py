#!/usr/bin/env python3
"""Acceptance for the gate engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.1.10), then the extra proofs (X rows). Prints a PASS/FAIL
table; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_gate.py [--quick] [--types 1,2] [--grid] [--rows=A,B,...] [--verbose]

--grid also prints the full close-time grid (threshold x level x frequency, Tight and
Smooth, worst and best over the note-off phase sweep). --rows runs only the closing-rule rows
named (A to G; X adds X10-X20 and the other verifier rows), for iterating; the full run is the
acceptance. --verbose lists every dip the B and C rows classify.

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
# Under the closing rule (Gate.h) the hold is the wait after a fall through the hysteresis band
# within 50 ms, at any floor depth (A rows); slower falls are ridden (X10-X12, B and C rows).
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

    # X10 to X12 (restated for the closing rule): every reopening dip of the gain (any dip below
    # unity that comes back, counted exactly) must follow a fall through the hysteresis band as
    # fast as a stop; those are the C trade-off, counted and reported, never a dip after a slow fall.
    def judge(item):
        x, keys, srate = item
        return classify(x, fx.mono(fx.run(EFFECT, x, keys, ["--trace"], srate)[1]), srate, keys["threshold"], 1.0)

    def summary(labels, results):
        bad = [f"{lab}: {len(r[0])}" for lab, r in zip(labels, results) if r[0]]
        conf = [r[1] for r in results if r[1]]
        text = f"{len(bad)} of {len(results)} chatter after a slow fall" + (": " + "; ".join(bad[:6]) if bad else "")
        if conf:
            text += (f"; {len(conf)} with dips at stop-fast troughs (C), deepest {min(d[2] for c in conf for d in c):.1f} dB, "
                     f"longest {max(d[1] for c in conf for d in c):.0f} ms")
        return not bad, text

    # X10: decaying chords, every verifier case.
    sigs = {name: chord_signal(fr, sr) for name, fr in CHORDS}
    cells = [(name, thr) for name, _ in CHORDS for thr in range(-30, -70, -5)]
    res = parallel(judge, [(sigs[n], dict(TIGHT, threshold=t), sr) for n, t in cells])
    report.add("X10 chord decay", "Tight, 14 chords x thr -30..-65", *summary([f"{n} thr {t}" for n, t in cells], res))
    for srate in ((44100, 96000) if args.quick else (22050, 44100, 96000, 192000)):
        sigs = {name: chord_signal(fr, srate) for name, fr in CHORDS if name in HARD_CHORDS}
        cells = [(name, thr) for name in HARD_CHORDS for thr in range(-30, -70, -5)]
        res = parallel(judge, [(sigs[n], dict(TIGHT, threshold=t), srate) for n, t in cells])
        report.add("X10 chord decay", f"Tight, 4 hardest chords, {srate} Hz", *summary([f"{n} thr {t}" for n, t in cells], res))

    # X11: beating, amplitude-modulated notes and two strings a few hertz apart.
    cells = [(f, d, r) for f in (82.41, 110.0) for d in (0.2, 0.33, 0.5, 0.7) for r in (1, 3, 7, 15, 30)]
    res = parallel(judge, [(am_note(f, d, r, sr), dict(TIGHT, threshold=-40), sr) for f, d, r in cells])
    report.add("X11 beating", "Tight, AM 3.5-15 dB x 1-30 Hz, thr -40",
               *summary([f"{f:g} Hz depth {d} at {r} Hz" for f, d, r in cells], res))
    cells = [(name, fa, fb, t) for name, fa, fb in TWO_STRINGS for t in (-30, -40, -50, -60)]
    sigs = {name: two_strings(fa, fb, sr) for name, fa, fb in TWO_STRINGS}
    res = parallel(judge, [(sigs[n], dict(TIGHT, threshold=t), sr) for n, _, _, t in cells])
    report.add("X11 beating", "Tight, two strings, thr -30..-60", *summary([f"{n} thr {t}" for n, _, _, t in cells], res))

    # X12: single notes decaying into a -80 dBFS floor.
    cells = [(f, tau, t) for f in (41.2, 55.0, 82.41, 110.0, 196.0, 330.0) for tau in (0.15, 0.4, 1.0, 2.5)
             for t in (-20, -30, -40, -50, -60, -70)]
    sigs = {(f, tau): single_note(f, tau, sr) for f, tau, _ in cells}
    res = parallel(judge, [(sigs[(f, tau)], dict(TIGHT, threshold=t), sr) for f, tau, t in cells])
    report.add("X12 single decays", "Tight, 41.2-330 Hz x tau x thr", *summary([f"{f:g} Hz tau {tau} thr {t}" for f, tau, t in cells], res))

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

    # X20: what the threshold means for low notes through the default sidechain filter. Two figures:
    # a tone that rises slowly (the steady-state level that opens the gate at all) and one that starts
    # abruptly at a crest and must still hold the gate open 0.4 s later (its onset transient through
    # the filter opens it earlier; the second
    # verification found the old row's 15.4 dB was this figure, steady state being 20.7).
    def needed(item):
        f, abrupt = item
        t = np.arange(int(1.5 * sr)) / sr
        lo, hi = -10.0, 40.0
        while hi - lo > 0.05:
            mid = (lo + hi) / 2
            if abrupt:
                x = fx.amp(-40 + mid) * np.cos(2 * np.pi * f * t)
            else:
                x = fx.amp(-40 + mid) * np.sin(2 * np.pi * f * t) * np.minimum(1.0, t / 0.3)
            g = fx.mono(fx.run(EFFECT, x, dict(TIGHT, threshold=-40), ["--trace"], sr)[1])
            lo, hi = (lo, mid) if np.max(g[int(0.4 * sr):] if abrupt else g) >= 1.0 else (mid, hi)
        return hi
    freqs = (41.2, 73.42, 82.41, 110.0)
    res = parallel(needed, [(f, a) for f in freqs for a in (False, True)])
    parts = [f"{f:g} Hz +{res[2 * i]:.1f} steady / +{res[2 * i + 1]:.1f} abrupt" for i, f in enumerate(freqs)]
    info(report, "X20 low-note threshold", "sine needed to open, thr -40, HPF 75", " / ".join(parts) + " dB over threshold")


# ---------------------------------------------------------------------------- the closing rule
# Rows A to G prove the closing rule of Gate.h and the opening, floor, sidechain and range fixes
# that came with it, on the second verification's signals (.audit/fx-verify/r2-dynamics.md,
# build-verify/dynamics-r2/r2lib.py, g1-g11), rebuilt here with numpy only.

FAST_FALL_MS = 50.0     # Gate.cpp kFastFallSeconds: a fall through the band in less is a stop
WINDOW_MS = 12.0


def _svf_ir(sr, fc, n):
    """Impulse response of the engine's 4th-order sidechain filter (two TPT SVF sections)."""
    g = np.tan(np.pi * min(fc, 0.49 * sr) / sr)
    y = np.zeros(n)
    y[0] = 1.0
    for k in (1.8477590650225735, 0.7653668647301796):
        a1 = 1.0 / (1.0 + g * (g + k))
        a2, a3 = g * a1, g * g * a1
        ic1 = ic2 = 0.0
        out = np.empty(n)
        for i, v in enumerate(y):
            v3 = v - ic2
            v1 = a1 * ic1 + a2 * v3
            v2 = ic2 + a2 * ic1 + a3 * v3
            ic1, ic2 = 2 * v1 - ic1, 2 * v2 - ic2
            out[i] = v - k * v1 - v2
        y = out
    return y


_IR = {}


def _conv(x, h):
    n = len(x) + len(h)
    m = 1 << (n - 1).bit_length()
    return np.fft.irfft(np.fft.rfft(x, m) * np.fft.rfft(h, m), m)[:len(x)]


def wmax(a, w):
    """Trailing sliding maximum over w samples (van Herk / Gil-Werman, vectorised)."""
    n = len(a)
    b = np.concatenate([a, np.zeros((-n) % w)]).reshape(-1, w)
    pre = np.maximum.accumulate(b, axis=1).ravel()[:n]
    suf = np.maximum.accumulate(b[:, ::-1], axis=1)[:, ::-1].ravel()[:n]
    out = pre.copy()
    out[w - 1:] = np.maximum(suf[:n - w + 1], pre[w - 1:])
    return out


def reading(x, sr, fc=HPF, windowed=True):
    """What the state machine reads (Gate.cpp advance): the lower of the 4th-order filtered and the
    unfiltered peak, over the 12 ms window (or instantaneous)."""
    if (sr, fc) not in _IR:
        _IR[(sr, fc)] = _svf_ir(sr, fc, int(0.3 * sr))
    filtered, raw = np.abs(_conv(x, _IR[(sr, fc)])), np.abs(x)
    if not windowed:
        return np.minimum(filtered, raw)
    w = int(round(WINDOW_MS * 1e-3 * sr))
    return np.minimum(wmax(filtered, w), wmax(raw, w))


def dips(g, level):
    """Excursions of the linear gain below `level` that come back: [(start, end, min gain)]."""
    below = g < level
    d = np.diff(below.astype(int))
    starts, ends = np.nonzero(d == 1)[0] + 1, np.nonzero(d == -1)[0] + 1
    out = []
    for s in starts:
        e = ends[ends > s]
        if len(e):
            out.append((s, e[0], float(np.min(g[s:e[0]]))))
    return out


def band_fall_ms(lev, sr, thr, at):
    """How long the level took to fall from the open level to the close level, for the fall that
    led to sample `at`: from the last sample at or over the open level before it to the first
    sample under the close level after that one."""
    op, cl = fx.amp(thr), fx.amp(thr - 6.0)
    opened = np.nonzero(lev[:at + 1] >= op)[0]
    if not len(opened):
        return float("inf")
    fell = np.nonzero(lev[opened[-1]:at + 1] < cl)[0]
    return fell[0] / sr * 1e3 if len(fell) else float("inf")


def classify(x, g, sr, thr, level=1.0):
    """Gain dips below `level` that reopen, split into chatter (the level fell through the band
    slower than a stop) and conflict dips (as fast as a stop: C). -> (chatter, conflict) lists of
    (start s, length ms, depth dB, band fall ms)."""
    lev = reading(np.asarray(x, dtype=float), sr)
    chatter, conflict = [], []
    for s, e, lo in dips(g, level):
        tau = band_fall_ms(lev, sr, thr, s)
        item = (s / sr, (e - s) / sr * 1e3, float(fx.db(max(lo, 1e-12))), tau)
        (conflict if tau <= FAST_FALL_MS + 2.0 else chatter).append(item)
    return chatter, conflict


# --- the second verification's signals (r2lib.py), numpy only

def _biquad(x, b, a):
    y = np.empty(len(x))
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        o = b[0] * v + b[1] * x1 + b[2] * x2 - a[1] * y1 - a[2] * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def _rbj(kind, f, sr, q=0.7071):
    w = 2 * np.pi * f / sr
    al, c = np.sin(w) / (2 * q), np.cos(w)
    b = [(1 - c) / 2, 1 - c, (1 - c) / 2] if kind == "lp" else [(1 + c) / 2, -(1 + c), (1 + c) / 2]
    a = [1 + al, -2 * c, 1 - al]
    return [v / a[0] for v in b], [1.0, a[1] / a[0], a[2] / a[0]]


def string(f0, secs, sr, level_db=-6.0, t60=4.0, bright=0.8, pick=0.13, B=2e-4, seed=1, click_db=-12.0,
           detune_cents=1.5, attack_ms=0.4):
    """r2lib.string: inharmonic partials, pick-position comb, per-partial decay, two detuned
    polarisations, a band-passed pick click; peak-normalised to level_db."""
    n = int(round(secs * sr))
    t = np.arange(n) / sr
    r = np.random.default_rng(seed)
    y = np.zeros(n)
    k = 1
    while True:
        fk = k * f0 * np.sqrt(1 + B * k * k)
        if fk > min(0.45 * sr, 9000) or k > 60:
            break
        a = np.sin(np.pi * k * pick) / k ** (1.0 + (1 - bright))
        tau = (t60 / 6.91) / (1.0 + 0.012 * k * k + 0.25 * k * (f0 / 110.0) ** 0.3)
        ph = r.uniform(0, 2 * np.pi)
        fk2 = fk * 2 ** (detune_cents / 1200.0)
        y += a * np.exp(-t / tau) * (0.7 * np.sin(2 * np.pi * fk * t + ph)
                                     + 0.3 * np.exp(-t / (1.6 * tau) + t / tau) * np.sin(2 * np.pi * fk2 * t + ph + 0.4))
        k += 1
    y *= 1 - np.exp(-t / (attack_ms * 1e-3))
    y /= np.max(np.abs(y)) + 1e-30
    if click_db is not None:
        m = int(0.003 * sr)
        c = r.standard_normal(m) * np.exp(-np.arange(m) / (0.0006 * sr))
        c = _biquad(_biquad(c, *_rbj("hp", 1500, sr)), *_rbj("lp", min(7000, 0.45 * sr), sr))
        c /= np.max(np.abs(c)) + 1e-30
        y[:m] += fx.amp(click_db) * c
        y /= np.max(np.abs(y))
    return fx.amp(level_db) * y


def strum(freqs, secs, sr, level_db=-6.0, strum_ms=8.0, seed=3, **kw):
    n = int(round(secs * sr))
    y = np.zeros(n)
    for i, f in enumerate(freqs):
        d = int(round(i * strum_ms * 1e-3 * sr))
        y[d:] += string(f, secs, sr, level_db=0.0, seed=seed + 7 * i, **kw)[:n - d]
    return fx.amp(level_db) * y / np.max(np.abs(y))


def hiss(secs, sr, rms_db, seed=11):
    x = np.random.default_rng(seed).standard_normal(int(round(secs * sr)))
    return fx.amp(rms_db) * x / np.sqrt(np.mean(x ** 2))


def hand_stop(y, sr, at_s, ms):
    y = y.copy()
    i = int(round(at_s * sr))
    y[i:] *= np.exp(-np.arange(len(y) - i) / sr / (ms * 1e-3))
    return y


def place(secs, sr, items):
    y = np.zeros(int(round(secs * sr)))
    for s, a in items:
        i = int(round(s * sr))
        m = min(len(a), len(y) - i)
        if m > 0:
            y[i:i + m] += a[:m]
    return y


def peak_db(x, sr, start=0.0):
    """Peak of the sidechain reading (windowed) from `start` on, dBFS."""
    return float(fx.db(np.max(reading(x, sr)[int(start * sr):])))


E1, B1, C2, D2, E2, F2, G2, GS2, A1, A2, B2, C3, D3, E3, FS2 = (41.2, 61.74, 65.41, 73.42, 82.41, 87.31, 98.0, 103.83,
                                                         55.0, 110.0, 123.47, 130.81, 146.83, 164.81, 92.5)
R2_CHORDS = {
    "C2+G2+C3": (C2, G2, C3), "A1+E2+A2": (A1, E2, A2), "E2+G2": (E2, G2), "E2+G#2": (E2, GS2), "E1+B1": (E1, B1),
    "B1+F#2+B2": (B1, FS2, B2), "E2+F2": (E2, F2), "D2+A2+D3": (D2, A2, D3), "E2+B2+E3": (E2, B2, E3),
    "E2 x2 +3c": (E2, E2 * 2 ** (3 / 1200)), "A2 x2 +9c": (A2, A2 * 2 ** (9 / 1200)), "E1 x2 +12c": (E1, E1 * 2 ** (12 / 1200)),
    "E2+A2+D3+G3+B3+E4": (E2, A2, D3, 196.0, 246.94, 329.63), "B0+F#1": (30.87, 46.25), "C2+Eb2": (C2, 77.78),
    "A1+C2": (A1, C2),
}


def hand_stopped(f0, under, fall_ms, sr, hold, release=30.0, mode=1, block=512, channels=1, level=-20.0):
    """A held note stopped by the hand, falling to a hiss floor `under` dB below it within
    `fall_ms`; threshold 8 dB over the floor (close level 2 dB over it). -> (close ms, ms after the
    end of the fall). Close time: to gain <= -20 dB, from one window before the last sample at which
    the sidechain reading (numpy twin of the engine's, 12 ms window) is at or over the close level:
    the moment the level itself was last there."""
    secs, at = 1.0, 0.5
    s = string(f0, secs, sr, level_db=0.0, t60=30.0, seed=4, click_db=None)
    s *= fx.amp(level - float(fx.db(np.max(reading(s, sr)[int((at - 0.03) * sr):int(at * sr)]))))
    s = hand_stop(s, sr, at, fall_ms / (under / 8.686))
    nz = hiss(secs, sr, -40.0, seed=3)
    nz *= fx.amp(level - under - peak_db(nz, sr, 0.25))
    x = s + nz
    thr = round(level - under + 8.0, 2)
    keys = {"mode": mode, "threshold": thr, "hold": hold, "release": release}
    xx = x if channels == 1 else np.vstack([x, x])
    g = fx.db(fx.mono(fx.run(EFFECT, xx, keys, [f"--block={block}", "--trace"], sr)[1]))
    i0 = int(at * sr)
    hit = np.nonzero(g[i0:] <= -20.0)[0]
    if not len(hit):
        return float("inf"), float("inf")
    end = i0 + hit[0]
    lev = reading(x, sr)
    above = np.nonzero(lev[i0:end] >= fx.amp(thr - 6.0))[0]
    ref = i0 + (above[-1] if len(above) else 0) - int(round(WINDOW_MS * 1e-3 * sr))
    return (end - ref) / sr * 1e3, (end - i0) / sr * 1e3 - fall_ms


def two_tone(f0, df, trough_db, sr, thr, tau=2.5, secs=8.0):
    """Two tones df Hz apart whose beat troughs are trough_db deep, the louder 20 dB over thr at
    the start, decaying (tau) through it: the cleanest form of a beat, trough depth as stated."""
    t = np.arange(int(secs * sr)) / sr
    r = 10 ** (-trough_db / 20)
    b = (1 - r) / (1 + r)
    x = np.exp(-t / tau) * (np.sin(2 * np.pi * f0 * t) + b * np.sin(2 * np.pi * (f0 + df) * t + 1.0)) * (1 - np.exp(-t / 0.002))
    return x * fx.amp(thr + 20.0) / (1 + b) + hiss(secs, sr, -96.0, seed=2)


def closing_rule_checks(report, args):
    sr = 48000
    T = 1
    want = set(args.rows) if args.rows else None

    def on(tag):
        return want is None or tag in want

    if on("A"):
        # A1: a stopped note closes on time whatever the floor (version 2: 418-430 ms on an 8-12 dB floor).
        cells = [(f0, u, ms) for f0 in (41.2, 82.41, 196.0, 1000.0) for u in (8, 12, 20, 40, 60, 90) for ms in (5, 15, 30)]
        res = parallel(lambda c: hand_stopped(c[0], c[1], c[2], sr, TIGHT["hold"]), cells)
        bound = WINDOW_MS + TIGHT["hold"] + TIGHT["release"] / 3.0 + 3.0
        (w, end), c = max(zip(res, cells), key=lambda rc: rc[0][0])
        report.add("A1 stop close", "Tight, floor 8-90 dB under, fall 5-30 ms", w <= bound,
                   f"worst {w:.1f} ms after the level leaves the close level ({c[0]:g} Hz, floor -{c[1]}, fall {c[2]} ms; "
                   f"limit 12 + hold + release/3 + 3 = {bound:.0f}); worst after the end of the fall "
                   f"{max(r[1] for r in res):.1f} ms")
        cells = [(f0, u, s, b, ch) for f0 in (82.41, 1000.0) for u in (12, 40, 90) for s in (44100, 96000, 192000)
                 for b in (1, 37, 512) for ch in (1, 2)]
        res = parallel(lambda c: hand_stopped(c[0], c[1], 15, c[2], TIGHT["hold"], block=c[3], channels=c[4])[0], cells)
        (w, c) = max(zip(res, cells))
        report.add("A1 stop close", "44.1/96/192 kHz, blocks 1/37/512, mono+stereo", w <= bound,
                   f"worst {w:.1f} ms ({c[0]:g} Hz floor -{c[1]} {c[2]} Hz block {c[3]} {c[4]} ch; limit {bound:.0f})")

        # A2: the hold knob moves the wait one for one, at every floor depth.
        holds = (0.0, 6.0, 20.0, 50.0, 100.0, 250.0)
        cells = [(f0, u, h) for f0 in (82.41, 1000.0) for u in (8, 12, 20, 40, 90) for h in holds]
        res = dict(zip(cells, parallel(lambda c: hand_stopped(c[0], c[1], 15, sr, c[2])[0], cells)))
        errs = {(f0, u, h): abs(res[(f0, u, h)] - res[(f0, u, 0.0)] - h)
                for f0 in (82.41, 1000.0) for u in (8, 12, 20, 40, 90) for h in holds[1:]}
        over = [k for k, e in errs.items() if e > max(0.1 * k[2], 3.0)]
        dev = max(errs.values())
        where = "; ".join(f"{k[0]:g} Hz floor -{k[1]} hold {k[2]:g}: {errs[k]:.1f}" for k in over[:3])
        report.add("A2 hold one for one", "hold 0-250, floor 8-90 dB under", not where,
                   f"largest |wait(hold) - wait(0) - hold| {dev:.1f} ms (limit 10 % or 3 ms)"
                   + (f"; over the limit: {where}" if where else ""))

        # A3: palm mutes over a floor 12-40 dB under the notes (g9_tight_floor.py).
        def chug_case(c):
            under, gap = c
            period, n = (75 + gap) * 1e-3, 10
            secs = n * period + 0.4
            notes = place(secs, sr, [(0.1 + k * period, hand_stop(string(82.41, 0.125, sr, level_db=-6, t60=1.2, seed=20 + k),
                                                                   sr, 0.075, 2.5)) for k in range(n)])
            nz = hiss(secs, sr, -40.0, seed=8)
            nz *= fx.amp(-6 - under - peak_db(nz, sr, 0.25))
            x = notes + nz
            thr = round(-6 - under + 8, 2)
            out, tr = fx.run(EFFECT, x, dict(TIGHT, threshold=thr), ["--trace"], sr)
            g, y = fx.mono(tr), fx.mono(out)
            lev = reading(x, sr)
            w = int(0.010 * sr)
            closed, front, judged = 0, 0.0, 0
            for k in range(2, n):
                o = int(round((0.1 + k * period) * sr))
                closed += fx.db(np.sqrt(np.mean(g[o - w:o] ** 2))) < -20.0
                if fx.db(np.max(lev[o:o + 2 * w])) >= thr + 12.0:   # nearer the threshold the pick lands before the opening (no lookahead)
                    judged += 1
                    front = min(front, 10 * np.log10(np.sum(y[o:o + w] ** 2) / np.sum(x[o:o + w] ** 2)))
            return closed, n - 2, front, judged
        cells = [(u, gap) for u in (12, 18, 22, 26, 30, 40) for gap in (50, 75, 100, 150, 250)]
        res = parallel(chug_case, cells)
        shut = sum(r[0] for r in res)
        total = sum(r[1] for r in res)
        front = min(r[2] for r in res)
        judged = sum(r[3] for r in res)
        bad = [f"-{u} dB {gap} ms: {r[0]}/{r[1]}" for (u, gap), r in zip(cells, res) if r[0] < r[1]]
        if args.verbose:
            for c, r in zip(cells, res):
                print("   ", c, r)
        report.add("A3 palm mutes", "floor 12-40 dB under, gaps 50-250 ms, thr floor+8", not bad and front >= -1.0,
                   f"{shut} of {total} gaps closed" + (f" ({'; '.join(bad[:4])})" if bad else "")
                   + f"; first 10 ms of the {judged} notes 12 dB or more over the threshold (X15): "
                   f"worst {front:.2f} dB (limit -1)")

        # A4: soft or decayed notes stopped by the hand (g2_hang.py: version 2 stayed open 418-430 ms).
        def hang(c):
            f0, over = c
            secs, at = 3.0, 1.5
            nz = hiss(secs, sr, -72.0, seed=21)
            npk = peak_db(nz, sr, 0.5)
            s = string(f0, secs, sr, level_db=0.0, t60=30.0, seed=4, click_db=None)
            s *= fx.amp(npk + over - float(fx.db(np.max(reading(s, sr)[int((at - 0.03) * sr):int(at * sr)]))))
            x = hand_stop(s, sr, at, 4.0) + nz
            g = fx.db(fx.mono(fx.run(EFFECT, x, dict(TIGHT, threshold=round(npk + 8, 2)), ["--trace"], sr)[1]))
            hit = np.nonzero(g[int(at * sr):] <= -20.0)[0]
            return hit[0] / sr * 1e3 if len(hit) else float("inf")
        cells = [(f0, over) for f0 in (82.41, 196.0) for over in (8, 12, 16, 20, 24, 32, 40)]
        res = parallel(hang, cells)
        report.add("A4 no hang", "notes 8-40 dB over a hiss floor, hand stop", max(res) <= 60.0,
                   f"closed {min(res):.1f} to {max(res):.1f} ms after the stop begins (limit 60; version 2: 418-430)")

    if on("B"):
        def r2_chord(c):
            name, s, thr, t60, mode, hold = c
            x = strum(R2_CHORDS[name], 5.0, s, level_db=-8, t60=t60, seed=5, strum_ms=6) + hiss(5.0, s, -92, seed=2)
            g = fx.mono(fx.run(EFFECT, x, dict(TIGHT if mode == 1 else SMOOTH, threshold=thr, hold=hold), ["--trace"], s)[1])
            return classify(x, g, s, thr, fx.amp(-6.0))
        cells = [(n, sr, thr, t60, 1, 6.0) for n in R2_CHORDS for thr in range(-30, -70, -5) for t60 in (2.5, 6.0)]
        for n in ("C2+G2+C3", "A1+E2+A2", "E2+G2", "E1+B1", "E2+F2", "B0+F#1", "A2 x2 +9c"):
            for thr in (-35, -45, -55):
                cells += [(n, s, thr, 4.0, 1, 6.0) for s in (44100, 96000, 192000)]
                cells += [(n, sr, thr, 4.0, 2, 10.0), (n, sr, thr, 4.0, 1, 0.0), (n, sr, thr, 4.0, 1, 250.0)]
        res = parallel(r2_chord, cells)
        chat = [(c, r[0]) for c, r in zip(cells, res) if r[0]]
        conf = [(c, r[1]) for c, r in zip(cells, res) if r[1]]
        report.add("B1 chords (verifier 2)", f"{len(cells)} cases: 16 chords x thr x rates x holds", not chat,
                   f"{len(chat)} cases chatter (a -6 dB dropout after a slow fall)"
                   + (": " + "; ".join(f"{c[0]} thr {c[2]} hold {c[5]:g}" for c, _ in chat[:4]) if chat else "")
                   + f"; {len(conf)} with dropouts at stop-fast troughs (C), deepest "
                   + (f"{min(d[2] for _, ds in conf for d in ds):.1f} dB" if conf else "-"))
        info(report, "B1 conflict cases", "fast troughs, A's timing (C)",
             "; ".join(f"{c[0]} thr {c[2]} t60 {c[3]:g} {c[1]} Hz mode {c[4]} hold {c[5]:g}: {len(ds)} x, to "
                       f"{min(d[2] for d in ds):.0f} dB, {max(d[1] for d in ds):.0f} ms" for c, ds in conf[:12])
             + (f" ... ({len(conf)} in all)" if len(conf) > 12 else ""))

        # B2: a quiet decaying chord behind a louder note (g3_chatter2.py, g3b_sweep.py).
        def behind(c):
            name, thr, loud, quiet, gap, hold, mode, alone = c
            loud_note = hand_stop(strum((82.41, 123.47, 164.81), 0.5, sr, level_db=loud, t60=3.0, seed=9), sr, 0.12, 5.0)
            q = strum(R2_CHORDS[name], 5.0 - gap, sr, level_db=quiet, t60=5.0, seed=5, click_db=None, strum_ms=6)
            items = [(0.05 + gap, q)] if alone else [(0.05, loud_note), (0.05 + gap, q)]
            x = place(5.0, sr, items) + hiss(5.0, sr, -95, seed=2)
            g = fx.mono(fx.run(EFFECT, x, dict(TIGHT if mode == 1 else SMOOTH, threshold=thr, hold=hold), ["--trace"], sr)[1])
            i0 = int((0.05 + gap + 0.05) * sr)
            ch, cf = classify(x, g, sr, thr, fx.amp(-6.0))
            return [d for d in ch if d[0] * sr >= i0], [d for d in cf if d[0] * sr >= i0]
        cells = []
        for n in ("C2+G2+C3", "A1+E2+A2", "E2+G2", "E2+G#2", "E1+B1", "E2+F2", "E2 x2 +3c", "A2 x2 +9c", "D2+A2+D3", "C2+Eb2"):
            for quiet in (-26, -32):
                cells += [(n, quiet - m, -3, quiet, 0.25, 6.0, 1) for m in (6, 10, 14, 18)]
        for n in ("E2+G2", "C2+G2+C3", "A2 x2 +9c"):
            for thr in (-38, -42):
                cells += [(n, thr, -3, -28, 0.25, 6.0, 2), (n, thr, -3, -28, 0.25, 20.0, 1), (n, thr, -3, -28, 0.25, 50.0, 1),
                          (n, thr, -12, -28, 0.25, 6.0, 1)]
        res = parallel(behind, [c + (False,) for c in cells])
        ref = parallel(behind, [c + (True,) for c in cells])
        worse = [c for c, r, a in zip(cells, res, ref) if len(r[0]) + len(r[1]) > len(a[0]) + len(a[1])]
        chat = [c for c, r in zip(cells, res) if r[0]]
        conf = sum(1 for r in res if r[1])
        report.add("B2 behind a louder note", f"{len(cells)} cases (g3), loud -3/-12 dBFS 0.25 s before", not chat and not worse,
                   f"{len(chat)} chatter, {len(worse)} worse than the same chord alone; {conf} with stop-fast-trough dropouts (C)")
        sweep = []
        for n in ("E2+G2", "A1+E2+A2", "E2+G#2", "C2+Eb2"):
            for diff in (12, 20, 28):
                for gap in (0.1, 0.25, 0.6):
                    sweep += [(n, -30 - m, -30 + diff, -30, gap, 6.0, 1) for m in (6, 14)]
        res = parallel(behind, [c + (False,) for c in sweep])
        ref = parallel(behind, [c + (True,) for c in sweep])
        worse = [c for c, r, a in zip(sweep, res, ref) if len(r[0]) + len(r[1]) > len(a[0]) + len(a[1])]
        report.add("B2 behind a louder note", f"{len(sweep)} cases (g3b): +12..28 dB, 0.1-0.6 s", not worse,
                   f"{len(worse)} cases with more dropouts than the same chord alone (version 2: 22 of 104 in g3)")

    if on("C"):
        # C: two strings beating, troughs 6-20 dB at 0.5-8 Hz, decaying through the threshold. Slow
        # troughs must be ridden (B); troughs that fall through the band as fast as a stop are closed
        # on A's timing and must cost a partial dip, never a cut to mute.
        cells = [(f0, df, tr, thr) for f0 in (110.0, 196.0) for df in (0.5, 1.0, 2.0, 4.0, 8.0) for tr in (6, 12, 20)
                 for thr in (-40, -55)]

        def beat(c):
            f0, df, tr, thr = c
            x = two_tone(f0, df, tr, sr, thr)
            g = fx.mono(fx.run(EFFECT, x, dict(TIGHT, threshold=thr), ["--trace"], sr)[1])
            return classify(x, g, sr, thr, fx.amp(-6.0))
        res = parallel(beat, cells)
        chat = [c for c, r in zip(cells, res) if r[0]]
        if args.verbose:
            for c, r in zip(cells, res):
                if r[0] or r[1]:
                    print("   ", c, "chatter", [tuple(round(v, 1) for v in d) for d in r[0]], "conflict",
                          [tuple(round(v, 1) for v in d) for d in r[1]])
        deepest = min((d[2] for r in res for d in r[1]), default=0.0)
        report.add("C beats", "two tones 110/196 Hz, beats 0.5-8 Hz, troughs 6-20 dB", not chat and deepest > -60.0,
                   f"{len(chat)} cases chatter after a slow fall"
                   + (" (" + "; ".join(f"{c[0]:g} Hz +{c[1]:g} Hz {c[2]} dB thr {c[3]}" for c in chat[:4]) + ")" if chat else "")
                   + f"; stop-fast troughs: deepest dip {deepest:.1f} dB (limit: above -60, not mute)")
        table = {}
        for (f0, df, tr, thr), (_, cf) in zip(cells, res):
            key = (df, tr)
            depth = min((d[2] for d in cf), default=0.0)
            length = max((d[1] for d in cf), default=0.0)
            old = table.get(key, (0.0, 0.0))
            table[key] = (min(old[0], depth), max(old[1], length))
        for tr in (6, 12, 20):
            info(report, "C trade-off", f"trough {tr} dB: rate -> dip, length",
                 " / ".join(f"{df:g} Hz {table[(df, tr)][0]:.0f} dB {table[(df, tr)][1]:.0f} ms" for df in (0.5, 1.0, 2.0, 4.0, 8.0)))

    if on("D"):
        # D1: picks keep their front (g5_attack.py pick cases, 20 and 34 dB over the threshold).
        def pick(c):
            f0, over, s = c
            x = place(0.8, s, [(0.2, string(f0, 0.5, s, level_db=-40 + over, t60=3.0, seed=6, click_db=-12.0))]) \
                + hiss(0.8, s, -80, seed=4)
            y = fx.mono(fx.run(EFFECT, x, dict(TIGHT, threshold=-40), [], s)[0])
            i, m = int(0.2 * s), int(0.001 * s)
            return 10 * np.log10(np.sum(y[i:i + m] ** 2) / np.sum(x[i:i + m] ** 2))
        cells = [(f0, over, sr) for f0 in (41.2, 61.74, 82.41, 196.0, 1000.0) for over in (20, 34)]
        cells += [(82.41, 20, s) for s in (44100, 96000, 192000)]
        res = parallel(pick, cells)
        (w, c) = min(zip(res, cells))
        report.add("D1 pick front", "picks 20/34 dB over thr, 41-1000 Hz, 44.1-192 kHz", w >= -1.5,
                   f"first millisecond worst {w:.2f} dB ({c[0]:g} Hz +{c[1]} dB {c[2]} Hz; limit -1.5)")

        # D2: swells and slow onsets open without a click (g5_attack.py click cases).
        def hf_peak(z, io, s):
            spec = np.fft.rfft(z)
            f = np.fft.rfftfreq(len(z), 1 / s)
            h = np.fft.irfft(spec * (f > 3000.0), len(z))
            return float(np.max(np.abs(h[io - int(0.002 * s):io + int(0.010 * s)])))

        def swell(c):
            f0, thr, ms, kind = c
            t = np.arange(sr) / sr
            s0 = fx.amp(-12) * np.sin(2 * np.pi * f0 * t) if kind == "sine" else \
                string(f0, 1.0, sr, level_db=-12, t60=20, seed=2, click_db=None)
            x = s0 * np.clip((t - 0.2) / (ms * 1e-3), 0, 1) ** 2 + hiss(1.0, sr, -96, seed=3)
            out, tr = fx.run(EFFECT, x, dict(TIGHT, threshold=thr), ["--trace"], sr)
            o = np.nonzero(fx.mono(tr) > 0.5)[0]
            if not len(o):
                return None
            return float(fx.db(hf_peak(fx.mono(out), o[0], sr))) + 12.0, float(fx.db(hf_peak(x, o[0], sr))) + 12.0
        cells = [(f0, thr, ms, k) for f0 in (41.2, 82.41, 110.0, 220.0) for thr in (-30, -40, -50) for ms in (150, 600)
                 for k in ("sine", "string")]
        res = parallel(swell, cells)
        sines = [r[0] for c, r in zip(cells, res) if r and c[3] == "sine"]
        strings = [(r[0], r[1]) for c, r in zip(cells, res) if r and c[3] == "string"]
        worst_str = max(r[0] - max(-50.0, r[1]) for r in strings)
        report.add("D2 opening click", f"{len(sines) + len(strings)} swells 150/600 ms, 41-220 Hz",
                   max(sines) <= -50.0 and worst_str <= 1.0,
                   f"above 3 kHz at the opening, re the note: sines worst {max(sines):.1f} dB (limit -50); strings worst "
                   f"{max(r[0] for r in strings):.1f} dB where the dry string itself reads up to {max(r[1] for r in strings):.1f} "
                   f"(limit: -50 or the dry string's own, +1 dB)")

    if on("E"):
        # E1: Smooth under the threshold never moves faster than 1 dB per 10 ms, however the floor moves.
        def floor_case(c):
            name, s = c
            secs = 4.0
            t = np.arange(int(secs * s)) / s
            w = hiss(secs, s, -64, seed=31)
            r = np.random.default_rng(77)
            crackle = np.zeros(len(t))
            idx = r.integers(0, len(t), int(secs * 6))
            crackle[idx] = r.uniform(0.3, 1.0, len(idx)) * r.choice([-1, 1], len(idx))
            crackle = _biquad(crackle, *_rbj("lp", 4000, s))
            x = {"step +5 dB": w * np.where(t > 2, fx.amp(5), 1.0), "step +8 dB": w * np.where(t > 2, fx.amp(8), 1.0),
                 "step -8 dB": w * np.where(t > 2, fx.amp(-8), 1.0), "wobble +-4 dB 4 Hz": w * fx.amp(4 * np.sin(8 * np.pi * t)),
                 "wobble +-7 dB 4 Hz": w * fx.amp(7 * np.sin(8 * np.pi * t)),
                 "crackle": hiss(secs, s, -70, seed=35) + fx.amp(-48) * crackle / np.max(np.abs(crackle))}[name]
            g = fx.db(np.maximum(fx.mono(fx.run(EFFECT, x, dict(SMOOTH, threshold=-40), ["--block=64", "--trace"], s)[1]),
                                 1e-9))[int(0.1 * s):]
            win = int(round(0.010 * s))
            view = np.lib.stride_tricks.sliding_window_view(g, win)
            return float(np.max(view.max(axis=1) - view.min(axis=1))), float(np.max(np.abs(np.diff(g))))
        names = ("step +5 dB", "step +8 dB", "step -8 dB", "wobble +-4 dB 4 Hz", "wobble +-7 dB 4 Hz", "crackle")
        cells = [(n, s) for n in names for s in (44100, 48000, 96000)]
        res = parallel(floor_case, cells)
        (move, step), c = max(zip(res, cells))
        report.add("E1 Smooth floor slew", "hiss steps / wobble / crackle under thr -40", move <= 1.01,
                   f"largest gain move in 10 ms {move:.2f} dB ({c[0]}, {c[1]} Hz; limit 1 dB = the slew), largest one-sample "
                   f"step {max(r[1] for r in res):.4f} dB (version 2: 3.6 / 12.4 dB on a +5 dB step, 11.3 dB per sample on crackle)")
        x = np.concatenate([hiss(1.0, sr, -64, seed=31), fx.amp(-20) * np.sin(2 * np.pi * 1000 * np.arange(int(0.2 * sr)) / sr)])
        g = fx.db(np.maximum(fx.mono(fx.run(EFFECT, x, dict(SMOOTH, threshold=-40), ["--trace"], sr)[1]), 1e-9))
        t_open = np.nonzero(g[sr:] >= -1.0)[0][0] / sr * 1e3
        report.add("E1 Smooth floor slew", "a note after the floor", t_open <= 1.0,
                   f"gain >= -1 dB {t_open:.2f} ms after a 1 kHz onset 20 dB over the threshold (limit 1 ms)")

    if on("F"):
        # F: what the sidechain filter does to bass, reported for the UI (not enforced).
        def needed(c):
            f0, hpf, onset = c
            t = np.arange(int(1.5 * sr)) / sr
            lo, hi = -10.0, 40.0
            while hi - lo > 0.1:
                mid = (lo + hi) / 2
                if onset == "steady":
                    x = fx.amp(-40 + mid) * np.sin(2 * np.pi * f0 * t) * np.minimum(1.0, t / 0.3)
                    ok = np.max(fx.mono(fx.run(EFFECT, x, dict(TIGHT, threshold=-40, sc_hpf=hpf), ["--trace"], sr)[1])) >= 1.0
                else:
                    x = fx.amp(-40 + mid) * np.sin(2 * np.pi * f0 * (t - 0.2) + np.pi / 2) * (t >= 0.2)
                    ok = np.max(fx.mono(fx.run(EFFECT, x, dict(TIGHT, threshold=-40, sc_hpf=hpf), ["--trace"], sr)[1])[int(0.6 * sr):]) >= 1.0
                lo, hi = (lo, mid) if ok else (mid, hi)
            return hi
        cells = [(f0, h, o) for f0 in (30.87, 41.2, 82.41) for h in (75.0, 40.0, 20.0) for o in ("steady", "abrupt")]
        res = dict(zip(cells, parallel(needed, cells)))
        for h in (75.0, 40.0, 20.0):
            info(report, "F bass, level to open", f"HPF {'off' if h <= 20.5 else f'{h:g} Hz'}, thr -40, pure tone",
                 " / ".join(f"{f0:g} Hz +{res[(f0, h, 'steady')]:.1f} steady, +{res[(f0, h, 'abrupt')]:.1f} abrupt"
                            for f0 in (30.87, 41.2, 82.41)) + " dB over threshold")

        def dull(c):
            f0, hpf = c
            x = string(f0, 6.0, sr, level_db=-8, t60=5.0, seed=4, bright=0.15, pick=0.3) + hiss(6.0, sr, -96, seed=1)
            g = fx.mono(fx.run(EFFECT, x, dict(TIGHT, threshold=-40, sc_hpf=hpf), ["--trace"], sr)[1])
            d = np.nonzero(fx.db(np.maximum(g, 1e-12)) < -6.0)[0]
            d = d[d > int(0.1 * sr)]
            if not len(d):
                return None
            return float(fx.db(np.max(np.abs(x[max(0, d[0] - int(0.03 * sr)):d[0]])))) + 40.0
        cells = [(f0, h) for f0 in (30.87, 41.2, 55.0) for h in (75.0, 40.0, 20.0)]
        res = dict(zip(cells, parallel(dull, cells)))
        info(report, "F bass, decaying dull note", "true peak over thr -40 when it shuts",
             " / ".join(f"{f0:g} Hz: " + ", ".join(f"{'off' if h <= 20.5 else f'{h:g}'} "
                                                   + ("never opens" if res[(f0, h)] is None else f"{res[(f0, h)]:+.1f}")
                                                   for h in (75.0, 40.0, 20.0))
                        for f0 in (30.87, 41.2, 55.0)) + " dB (it shuts after the closing rule's ride: -6 dB less the decay over up to 1 s)")
        t = np.arange(int(4 * sr)) / sr
        humx = fx.amp(-46) * (np.sin(2 * np.pi * 50 * t) + 0.3 * np.sin(2 * np.pi * 100 * t + 1) + 0.2 * np.sin(2 * np.pi * 150 * t + 2))
        parts = []
        for h in (75.0, 40.0, 20.0):
            for mains in (50.0, 60.0):
                hx = humx if mains == 50.0 else fx.amp(-46) * (np.sin(2 * np.pi * 60 * t) + 0.3 * np.sin(2 * np.pi * 120 * t + 1)
                                                               + 0.2 * np.sin(2 * np.pi * 180 * t + 2))
                lev = reading(hx, sr, fc=h) if h > 20.5 else wmax(np.abs(hx), int(0.012 * sr))
                parts.append(f"{'off' if h <= 20.5 else f'{h:g}'} {mains:g} Hz {fx.db(np.max(lev[sr:])) + 46:.1f}")
        info(report, "F hum, reading", "50/60 Hz + 0.3 / 0.2 2nd / 3rd harmonic", " / ".join(parts) + " dB re the fundamental's amplitude")

    if on("G"):
        # G1: range automation is smoothed per sample, not stepped at block rate.
        x = string(82.41, 4.0, sr, level_db=-14, t60=25, seed=8) + hiss(4.0, sr, -70, seed=9)
        worst = 0.0
        for block in (64, 512):
            g = fx.mono(fx.run(EFFECT, x, ["mode=1", "threshold=-5", "range@0.5..3.0=-90..-10"], [f"--block={block}", "--trace"], sr)[1])
            worst = max(worst, float(np.max(np.abs(np.diff(g[int(0.5 * sr):])))))
        report.add("G1 range smoothing", "closed, range ramp -90 -> -10 dB, blocks 64/512", worst <= 0.001,
                   f"largest gain step {worst:.5f} per sample (limit 0.001; version 2: 0.0121 at block 512)")


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
    rows = [a for a in sys.argv if a.startswith("--rows=")]
    for a in rows:
        sys.argv.remove(a)
    verbose = "--verbose" in sys.argv
    if verbose:
        sys.argv.remove("--verbose")
    args = fx.parse_args(__doc__)
    args.rows = rows[-1].split("=", 1)[1].split(",") if rows else None
    args.verbose = verbose
    report = fx.Report(EFFECT)
    grid = Grid()
    if not args.rows:
        fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                         quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
        specific_checks(report, args, grid)
        extra_checks(report, args, grid)
        robustness_checks(report, args, grid)
    elif "X" in args.rows:
        robustness_checks(report, args, grid)
    closing_rule_checks(report, args)
    if show_grid:
        print_grid(grid)
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
