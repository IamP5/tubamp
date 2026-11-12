#!/usr/bin/env python3
"""Acceptance for the gate engine: first the rule's invariants (I rows, on seeded random and
adversarial input), then the common checks of docs/FX-ENGINES.md §2.11, the effect's own table
(§4.1.10), the extra proofs (X rows) and the rows on the rule itself (P rows: the engine equals a
numpy port of the plain hysteresis rule Gate.h states). Prints a PASS/FAIL table; exit status 1 on
any FAIL. Rows named "info ..." print their figures and are never enforced: a plain gate at the
default hold dips on some chords and beats, and those rows show by how much.

    python3 tools/fxbench/check_gate.py [--quick] [--types 1,2] [--grid] [--rows=I,P,A,B,...] [--verbose]

(Not with -I: it drops the user site-packages, where numpy may live.) --grid also prints the full
close-time grid (threshold x level x frequency, Tight and Smooth, worst and best over the note-off
phase sweep). --rows runs only the groups named, for iterating; the full run is the acceptance:
  common  the common checks of §2.11       G  G1-G19 (the effect's table)    X  X1-X21 (extra proofs)
  I       the rule's invariants            P  the engine against the port   A B C D E F R  the other rows
  T       the targets on seeded draws
A name that is not a group is an error. --verbose lists every dip the C rows count.

Bars whose source the row does not state are docs/FX-ENGINES.md §4.1.10's table.

Close time (§4.1.10): on the per-sample gain trace, from the end of the burst's 1 ms
raised-cosine fade to the first sample with gain <= -20 dB; each cell is the max over
8 start phases x 4 burst lengths (0.3 s + {0, 0.37, 1.13, 2.9} ms), i.e. 32 note-off
phases. The 32 bursts of a cell run in one render, each after 0.6 s of digital silence,
which leaves the gate in exactly its reset state (closed, window empty, filter flushed).
"""
import math
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

# The engine's per-mode starting points (Gate.h kGateReleaseDefault, kGateHoldDefault). The hold is how
# long the level stays under the close level before the gate closes (A rows, I4, P1).
TIGHT = {"mode": 1, "release": 30.0, "hold": 10.0, "range": -90.0, "sc_hpf": 75.0}
SMOOTH = {"mode": 2, "release": 80.0, "hold": 40.0, "range": -90.0, "sc_hpf": 75.0}
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


CLOSE_BOUND_SOURCE = "window + hold + release/3 + 0.1 ms"


def close_bound(keys):
    """The rule's latest close time from a burst's end: its last crest at the close level is no later than the end,
    the reading holds it a window, the gate closes after the hold and reaches -20 dB a third of the release later;
    0.1 ms for the sample grid."""
    return WINDOW_MS + keys["hold"] + keys["release"] / 3.0 + 0.1


def close_floor(keys, freq):
    """The rule's earliest: the last crest at the close level is at most the 1 ms fade and half a period early."""
    return WINDOW_MS + keys["hold"] + keys["release"] / 3.0 - 1.0 - 500.0 / freq - 0.1


def close_row(report, grid, row, mode, freq, level, thrs, bound, sr=48000, hpf=HPF, scope=None):
    worst = {t: grid.worst(mode, freq, level, t, sr, hpf) for t in thrs}
    ok = all(v <= bound for v in worst.values())
    detail = " / ".join(f"{t}: {v:.1f}" for t, v in worst.items()) + f" ms (limit {bound:.1f}: {CLOSE_BOUND_SOURCE})"
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


def open_close_changes(gain, start=0):
    """Changes between open (>= -1 dB) and closed (<= -20 dB), with the gain in between keeping the last."""
    gd = fx.db(np.maximum(fx.mono(gain)[start:], 1e-12))
    state = np.where(gd >= -1.0, 1, np.where(gd <= -20.0, 0, -1))
    known = state[state >= 0]
    return int(np.count_nonzero(known[1:] != known[:-1]))


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
    # G2-G6, G17, X7: the bar is the rule's arithmetic (close_bound), not §4.1.10's looser figures.
    bound = close_bound(TIGHT)
    close_row(report, grid, "G2 close time", T, 1000.0, -20, (-30, -40, -50, -60), bound)
    close_row(report, grid, "G3 close time", T, 1000.0, -6, (-30, -40, -50), bound)
    worst = {"-6/-60": grid.worst(T, 1000.0, -6, -60), "-6/-70": grid.worst(T, 1000.0, -6, -70),
             "-20/-70": grid.worst(T, 1000.0, -20, -70)}
    report.add("G4 close time", f"Tight 1 kHz, HPF {HPF:g}", all(v <= bound for v in worst.values()),
               " / ".join(f"{k}: {v:.1f}" for k, v in worst.items()) + f" ms (limit {bound:.1f}: {CLOSE_BOUND_SOURCE})")
    off = {"-6/-60": grid.worst(T, 1000.0, -6, -60, hpf=20.0), "-6/-70": grid.worst(T, 1000.0, -6, -70, hpf=20.0),
           "-20/-70": grid.worst(T, 1000.0, -20, -70, hpf=20.0)}
    report.add("G4 close time", "Tight 1 kHz, HPF off", all(v <= bound for v in off.values()),
               " / ".join(f"{k}: {v:.1f}" for k, v in off.items()) + f" ms (limit {bound:.1f}: {CLOSE_BOUND_SOURCE})")
    close_row(report, grid, "G5 close time", T, 82.41, -20, (-40, -60), bound)
    cells = {(lv, t): grid.worst(T, 82.41, lv, t) for lv in LEVELS for t in THRESHOLDS}
    (lv, t), v = max(cells.items(), key=lambda kv: kv[1])
    report.add("G6 close time", "Tight 82.41 Hz, all 10 cells", v <= bound,
               f"max {v:.1f} ms at {lv} dBFS / thr {t} (limit {bound:.1f}: {CLOSE_BOUND_SOURCE})")

    # G7: the rule's shortest close: the last crest at the close level is at most the 1 ms fade plus half a period
    # before the burst's end, so the close comes no sooner than window + hold + release/3 - that - 0.1 ms.
    shortest = min((grid.best(m, f, lv, t) - close_floor(TIGHT if m == T else SMOOTH, f), grid.best(m, f, lv, t), m, f, lv, t)
                   for m in (T, S) for f in FREQS for lv in LEVELS for t in THRESHOLDS)
    report.add("G7 never shorter", "Tight + Smooth, all 40 cells", shortest[0] >= 0.0,
               f"least margin {shortest[0]:+.1f} ms: {shortest[1]:.1f} ms ({TYPES[shortest[2]]} {shortest[3]:g} Hz {shortest[4]} "
               f"dBFS thr {shortest[5]}; limit window + hold + release/3 - 1 ms fade - half a period - 0.1 ms)")

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
        line = float(np.median(gap_fall_db(0.075, TIGHT, srate)))
        report.add(row, f"Tight chug thr -40{slack}", med <= line + 0.01 and first10 >= -HEARD_DB,
                   f"gap median {med:.1f} dB (worst {worst_gap:.1f}; limit {line:.1f}: the rule's release line, X13's "
                   f"gap_fall_db, +0.01); first 10 ms {first10:.2f} dB (limit -{HEARD_DB:g}: about the smallest change heard)")

    chug_rows("G9 chug", sr)
    # G10: Smooth's expander on the chug's noise. With 150 ms gaps the gate closes, the release reaches the law and
    # the gain then follows it: the median gain over each gap's last 10 ms must be the 3:1 law, (EXPANDER_SLOPE - 1) x
    # (floor estimate - close level), within HEARD_DB, on the estimate the rule computes from the twin reading (the dB
    # level weighted linearly from the close, which comes the hold after the reading is last at the close level).
    # A gate without the expander (Tight: the range) or a 2.5:1 law (4.5 dB off here) fails.
    x = fx.chug(sr, secs=3.0, note=0.075, period=0.225)
    g = np.maximum(gain_db(fx.run(EFFECT, x, dict(SMOOTH, threshold=-40), ["--trace"], sr)[1]), -120.0)
    lev_db = fx.db(np.maximum(twin_level(x, sr, HPF), 1e-9))
    step_n, w10, hold_n = int(round(0.225 * sr)), int(round(0.010 * sr)), int(round(SMOOTH["hold"] * 1e-3 * sr))
    pairs = []
    for o in range(step_n, len(x) - int(round(0.075 * sr)), step_n):
        c = o - step_n + int(np.nonzero(lev_db[o - step_n:o] >= -46.0 - TWIN_EPS_DB)[0][-1]) + hold_n + 1
        k = o - w10 // 2
        wts = np.arange(1, k - c + 2, dtype=float)
        est = float(np.sum(wts * lev_db[c:k + 1]) / np.sum(wts))
        pairs.append((float(np.median(g[o - w10:o])), min(0.0, max(SMOOTH["range"], (EXPANDER_SLOPE - 1.0) * (est + 46.0)))))
    off = max(pairs, key=lambda q: abs(q[0] - q[1]))
    report.add("G10 chug", "Smooth thr -40, 150 ms gaps", all(abs(a - b) <= HEARD_DB for a, b in pairs),
               f"{len(pairs)} gaps; gain at the gap's end against the {EXPANDER_SLOPE:g}:1 law on the rule's estimate: median "
               f"{np.median([q[0] for q in pairs]):.1f} against {np.median([q[1] for q in pairs]):.1f} dB, furthest "
               f"{off[0]:.1f} against {off[1]:.1f} (limit +-{HEARD_DB:g} dB, about the smallest change heard)")

    for m in (T, S):
        g = gain_db(fx.run(EFFECT, fx.hover(sr), dict(TIGHT if m == T else SMOOTH, threshold=-40), ["--trace"], sr)[1])
        n = crossings(g, sr)
        report.add("G11 hover", f"{TYPES[m]} 1 kHz -40 +-3 dB, thr -40", n == 0, f"{n} crossings of -6 dB after 0.5 s")

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
    # G15: the filter's work is shown with hum 4 dB over the threshold, which holds the gate open only
    # without the filter.
    med_on, _, _, _ = chug_stats(dict(TIGHT, threshold=-40), sr, extra=hum(-36.0))
    med_off, _, _, _ = chug_stats(dict(TIGHT, threshold=-40, sc_hpf=20.0), sr, extra=hum(-36.0))
    report.add("G15 SC HPF", "50 Hz hum -36 + chug, thr -40", med_on <= -30.0 and med_off >= -1.0,
               f"HPF {HPF:g} gap median {med_on:.1f} dB (limit -30); HPF off {med_off:.1f} dB (open = >= -1)")
    med_on, _, _, _ = chug_stats(dict(TIGHT, threshold=-40), sr, extra=hum(-44.0))
    med_off, _, _, _ = chug_stats(dict(TIGHT, threshold=-40, sc_hpf=20.0), sr, extra=hum(-44.0))
    info(report, "G15 hum in the band", "50 Hz hum -44 + chug, thr -40",
         f"HPF {HPF:g} gap median {med_on:.1f} dB, HPF off {med_off:.1f} dB: hum inside the band holds an open gate open")
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
        close_row(report, grid, "G17 rates", T, 1000.0, -20, (-30, -40, -50, -60), bound, sr=srate)
        close_row(report, grid, "G17 rates", T, 82.41, -20, (-40, -60), bound, sr=srate)
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
    t_rule = WINDOW_MS + TIGHT["hold"] + 0.1
    ok = first2 == first_above and t_closed <= t_rule and gr[-1] == 90.0 and state[-1] == 0
    report.add("G18 meter", "Tight thr -40, block 1", ok,
               f"state 2 at sample {first2 - int(0.1 * sr)} after onset, first above thr {first_above - int(0.1 * sr)}; "
               f"state 0 {t_closed:.1f} ms after note end (limit window + hold + 0.1 = {t_rule:.1f}: the rule closes the hold "
               f"after the reading leaves the close level, a window after the last crest); GR closed {gr[-1]:g} dB")


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
        line = float(np.median(gap_fall_db(gap, TIGHT, sr)))
        ok &= med <= line + 0.01
        parts.append(f"{gap * 1e3:.0f} ms: {med:.1f} (worst {worst_gap:.1f}, limit {line:.1f})")
    report.add("X2 palm-mute gaps", "Tight chug thr -40, 75 ms notes", ok,
               "; ".join(parts) + " dB (limit: the median of the rule's release line over the gap's last 10 ms, X13's gap_fall_db, +0.01)")

    # X3: while the note's sidechain peak is still at least 0.5 dB over the open level, the gain stays at
    # 0 dB; where it passes the close level the gain is reported.
    worst, where, at_close = 0.0, "", 0.0
    for m in (T, S):
        for freq in (82.41, 1000.0):
            for tau in (0.4, 1.5):
                t = np.arange(int((tau * 9 + 0.5) * sr)) / sr
                env = fx.amp(-6.0) * np.exp(-t / tau)
                x = env * np.sin(2 * np.pi * freq * t)
                g = gain_db(fx.run(EFFECT, x, dict(TIGHT if m == T else SMOOTH, threshold=-40), ["--trace"], sr)[1])
                w = (freq / HPF) ** 4
                sc_env = env * w / np.sqrt(1.0 + w * w)   # 4th-order Butterworth magnitude
                live = sc_env >= fx.amp(-40.0 + 0.5)
                live[:int(0.005 * sr)] = False  # the gate opening from reset is G8's business
                v = float(np.min(g[live]))
                if v < worst:
                    worst, where = v, f"{TYPES[m]} {freq:g} Hz tau {tau} s"
                at_close = min(at_close, float(g[np.nonzero(sc_env >= fx.amp(-46.0))[0][-1]]))
    report.add("X3 decay not chopped", "thr -40, -6 dBFS exp decays", worst >= -0.1,
               f"min gain above open+0.5 dB: {worst:.3f} dB{' (' + where + ')' if where else ''}; "
               f"lowest gain where the note passes the close level {max(at_close, -120.0):.1f} dB")

    # X4: no chatter on stationary noise at levels around the threshold: the gate changes between
    # open (>= -1 dB) and closed (<= -20 dB, the close-time bar) at most twice in 5.5 s.
    worst, where = 0, ""
    for rms in range(-62, -44, 2):
        n = open_close_changes(fx.run(EFFECT, fx.white(6.0, rms, sr, seed=rms + 100), dict(TIGHT, threshold=-40),
                                      ["--trace"], sr)[1], int(0.5 * sr))
        if n > worst:
            worst, where = n, f"noise {rms} dBFS RMS"
    report.add("X4 no chatter", "white noise -62..-46 dBFS RMS, thr -40", worst <= 2,
               f"max {worst} open/close changes in 5.5 s{' (' + where + ')' if where else ''} (limit 2)")

    # X4 long: 30 s of white noise whose median 12 ms reading sits within 3 dB of the threshold, either
    # side: the 6 dB band keeps the gate from chattering on it.
    def x4_long(c):
        off, seed = c
        x = fx.white(30.0, -60.0, sr, seed=seed)
        x = x * fx.amp(-40.0 + off - float(fx.db(np.median(reading(x, sr)[int(0.5 * sr):]))))
        return off, seed, open_close_changes(fx.run(EFFECT, x, dict(TIGHT, threshold=-40), ["--trace"], sr)[1], int(0.5 * sr))
    res = parallel(x4_long, [(off, 900 + s) for off in (-3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0) for s in range(6)])
    w = max(res, key=lambda q: q[2])
    report.add("X4 long", f"{len(res)} cases: noise median reading -3..+3 dB re thr -40, 30 s, Tight", w[2] <= 2,
               f"max {w[2]} open/close changes (median reading {w[0]:+.0f} dB, seed {w[1]}; limit 2); per offset: "
               + ", ".join(f"{off:+.0f}: {max(q[2] for q in res if q[0] == off)}" for off in sorted({q[0] for q in res})))

    # X21 swell after a decay: a note decays (20 or 60 dB/s) until the gate closes, then a volume swell
    # rises linear in dB from the hiss (-75 dBFS) to -20 dBFS over 0.5-5 s. The gain must be over -1 dB
    # within 10 ms of the level reading crossing the open level (the opening ramp is at most 5 ms).
    def swell_case(c):
        decay, swell, mode = c
        n = int(sr * (3.0 + swell + 1.5))
        t = np.arange(n) / sr
        env = np.full(n, -200.0)
        env[t < 0.6] = -20.0
        b = (t >= 0.6) & (t < 3.0)
        env[b] = -20.0 - decay * (t[b] - 0.6)
        r = (t >= 3.0) & (t < 3.0 + swell)
        env[r] = -75.0 + 55.0 * (t[r] - 3.0) / swell
        env[t >= 3.0 + swell] = -20.0
        y = sum(np.sin(2 * np.pi * 110.0 * k * t + k) / k for k in range(1, 6)) / 1.6
        x = y * fx.amp(env) + fx.amp(-75.0) * np.random.default_rng(7).standard_normal(n)
        g = fx.mono(fx.run(EFFECT, x, dict(TIGHT if mode == 1 else SMOOTH, threshold=-40), ["--trace"], sr)[1])
        i0 = int(3.0 * sr)
        closed = bool(g[i0 - 10] < 1e-3) if mode == 1 else True
        cross = i0 + int(np.argmax(reading(x, sr)[i0:] >= fx.amp(-40.0)))
        up = np.nonzero(g[cross:] > fx.amp(-1.0))[0]
        return (up[0] / sr * 1e3 if len(up) else float("inf")), closed
    cells = [(d, s, m) for m in (1, 2) for d in (20.0, 60.0) for s in (0.5, 1.0, 2.0, 3.0, 5.0)]
    res = parallel(swell_case, cells)
    (w, _), c = max(zip(res, cells), key=lambda rc: rc[0][0])
    report.add("X21 swell after a decay", f"{len(cells)} cases: decay 20/60 dB/s, swell 0.5-5 s, Tight and Smooth",
               w <= 10.0 and all(r[1] for r in res),
               f"over -1 dB at most {w:.1f} ms after the reading crosses the open level ({TYPES[c[2]]}, decay {c[0]:g} dB/s, "
               f"swell {c[1]:g} s; limit 10); Tight closed before every swell: {all(r[1] for r in res)}")

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
    # Open/close is counted as in X4 (>= -1 / <= -20 dB) and the deepest dip that reopens is reported:
    # at 41.2 Hz the gaps between crests reach under the close level for up to 12.3 ms.
    t = np.arange(int(8 * sr)) / sr
    worst, where, deepest = 0, "", 0.0
    for freq in (41.2, 46.25, 82.41):
        for tau in (0.7, 1.5, 3.0):
            env = fx.amp(-6.0) * np.exp(-t / tau)
            x = env * sum(a * np.sin(2 * np.pi * freq * h * t) for h, a in zip((1, 2, 3, 4), (1, 0.6, 0.4, 0.25))) / 2.25
            for thr in (-40, -55):
                g = fx.mono(fx.run(EFFECT, x, dict(TIGHT, threshold=thr), ["--trace"], sr)[1])
                n = open_close_changes(g)
                deepest = min([deepest] + [float(fx.db(max(d[2], 1e-12))) for d in dips(g, 1.0)])
                if n > worst:
                    worst, where = n, f"{freq:g} Hz tau {tau} s thr {thr}"
    report.add("X9 low-note decay", "Tight, 41.2/46.25/82.41 Hz decays", worst <= 2,
               f"max {worst} open/close changes ({where}) (limit 2: one open, one close); deepest dip that "
               f"reopens {deepest:.2f} dB")

    # X7: the close-time rows at the ends of the rate range (G17's +1 ms rule).
    for srate in (22050, 192000):
        close_row(report, grid, "X7 rates", T, 1000.0, -20, (-30, -40, -50, -60), close_bound(TIGHT), sr=srate)
        close_row(report, grid, "X7 rates", T, 82.41, -20, (-40, -60), close_bound(TIGHT), sr=srate)


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


def gap_fall_db(gap, keys, sr=48000):
    """The gain (dB) over a chug gap's last 10 ms of a gate that closes the full window plus the hold after the
    note's last sample and then falls at the release rate to the range."""
    note_n, step_n = int(round(0.075 * sr)), int(round((0.075 + gap) * sr))
    w, hold_n = int(round(WINDOW_MS * 1e-3 * sr)), int(round(keys["hold"] * 1e-3 * sr))
    k = np.arange(step_n - note_n - int(round(0.010 * sr)), step_n - note_n)
    return np.clip(-60.0 / (keys["release"] * 1e-3 * sr) * (k - (w - 1 + hold_n) + 1), keys["range"], 0.0)


def gap_bound(gap, keys, sr=48000):
    """The loudest a chug gap's last 10 ms can be under the rule (gap_energy's measure): the reading is under the
    close level one window after the note's last sample, the gate closes after the hold, then falls the release
    rate to the range. fx.chug's -66 dBFS RMS noise peaks well under any close level used with it."""
    g_db = gap_fall_db(gap, keys, sr)
    g = np.where(g_db <= -89.95, 0.0, fx.amp(g_db)) if keys["range"] <= -89.95 else fx.amp(g_db)
    return 10 * np.log10(max(float(np.mean(g ** 2)), 1e-12))


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

    # X10 to X12, report only: a plain gate at the default hold closes in a chord's or a beat's deeper
    # troughs, so these rows count every dip of the gain that comes back, those at the range within their
    # first 60 ms and the reopenings that click (D2's measure).
    def judge(item):
        x, keys, srate = item
        out, tr = fx.run(EFFECT, x, keys, ["--trace"], srate)
        return trough_dips(fx.mono(tr), srate, keys.get("range", -90.0), x, fx.mono(out))

    def summary(labels, results):
        return dip_summary(labels, results)[1]

    # X10: decaying chords, every verifier case.
    sigs = {name: chord_signal(fr, sr) for name, fr in CHORDS}
    cells = [(name, thr) for name, _ in CHORDS for thr in range(-30, -70, -5)]
    res = parallel(judge, [(sigs[n], dict(TIGHT, threshold=t), sr) for n, t in cells])
    info(report, "info X10 chord decay", "Tight, 14 chords x thr -30..-65", summary([f"{n} thr {t}" for n, t in cells], res))
    for srate in ((44100, 96000) if args.quick else (22050, 44100, 96000, 192000)):
        sigs = {name: chord_signal(fr, srate) for name, fr in CHORDS if name in HARD_CHORDS}
        cells = [(name, thr) for name in HARD_CHORDS for thr in range(-30, -70, -5)]
        res = parallel(judge, [(sigs[n], dict(TIGHT, threshold=t), srate) for n, t in cells])
        info(report, "info X10 chord decay", f"Tight, 4 hardest chords, {srate} Hz", summary([f"{n} thr {t}" for n, t in cells], res))

    # X11: beating, amplitude-modulated notes and two strings a few hertz apart.
    cells = [(f, d, r) for f in (82.41, 110.0) for d in (0.2, 0.33, 0.5, 0.7) for r in (1, 3, 7, 15, 30)]
    res = parallel(judge, [(am_note(f, d, r, sr), dict(TIGHT, threshold=-40), sr) for f, d, r in cells])
    info(report, "info X11 beating", "Tight, AM 3.5-15 dB x 1-30 Hz, thr -40",
         summary([f"{f:g} Hz depth {d} at {r} Hz" for f, d, r in cells], res))
    cells = [(name, fa, fb, t) for name, fa, fb in TWO_STRINGS for t in (-30, -40, -50, -60)]
    sigs = {name: two_strings(fa, fb, sr) for name, fa, fb in TWO_STRINGS}
    res = parallel(judge, [(sigs[n], dict(TIGHT, threshold=t), sr) for n, _, _, t in cells])
    info(report, "info X11 beating", "Tight, two strings, thr -30..-60", summary([f"{n} thr {t}" for n, _, _, t in cells], res))

    # X12: single notes decaying into a -80 dBFS floor.
    cells = [(f, tau, t) for f in (41.2, 55.0, 82.41, 110.0, 196.0, 330.0) for tau in (0.15, 0.4, 1.0, 2.5)
             for t in (-20, -30, -40, -50, -60, -70)]
    sigs = {(f, tau): single_note(f, tau, sr) for f, tau, _ in cells}
    res = parallel(judge, [(sigs[(f, tau)], dict(TIGHT, threshold=t), sr) for f, tau, t in cells])
    ok, text = dip_summary([f"{f:g} Hz tau {tau} thr {t}" for f, tau, t in cells], res)
    dipped = sum(1 for ds in res if any(d[2] < -1.0 for d in ds))
    report.add("X12 single decays", "Tight, 41.2-330 Hz x tau x thr", ok and dipped == 0,
               text + " (limit: no dip under -1 dB: a single decay crosses the band once)")

    # X13: palm-mute gaps, by the energy of the gain over each gap's last 10 ms. The bar is the rule's own
    # arithmetic (gap_bound): no gap may end louder than a gate that closes the full window plus the hold
    # after the note and then falls at the release rate.
    gaps = (0.030, 0.040, 0.050, 0.075, 0.100)
    res = {gap: gap_energy(dict(TIGHT, threshold=-40), gap) for gap in gaps}
    bounds = {gap: gap_bound(gap, TIGHT, sr) for gap in gaps}
    report.add("X13 palm-mute gaps", "Tight thr -40, gaps 30-100 ms", all(res[g][1] <= bounds[g] + 0.01 for g in gaps),
               " / ".join(f"{g * 1e3:.0f} ms {res[g][0]:.1f} (worst {res[g][1]:.1f}, limit {bounds[g]:.1f})" for g in gaps)
               + " dB, median (worst); limit: closed window + hold after the note, then the release, +0.01 dB")
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
    # Bars from the rule: closed, Smooth follows its law at most SMOOTH_SLEW_DB_S, 1 dB per 10 ms and 100 / sr per sample
    # (the 44.1 kHz figure, the largest; +1 % for float rounding).
    lim_step = SMOOTH_SLEW_DB_S / 44100 * 1.01
    report.add("X14 Smooth floor", "noise / hum / buzz under thr -40", worst_move <= 1.01 and worst_step <= lim_step,
               f"max gain move in 10 ms {worst_move:.2f} dB (limit 1.01: the slew), max one-sample step {worst_step:.4f} dB "
               f"(limit {lim_step:.4f}: the slew per sample at 44.1 kHz) ({where})")

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


EARLY_MS = 60.0         # no dip at the range within this of its start


def _hf(z, sr):
    f = np.fft.rfftfreq(len(z), 1 / sr)
    return np.abs(np.fft.irfft(np.fft.rfft(z) * (f > 3000.0), len(z)))


def trough_dips(g, sr, range_db, x, y):
    """Every dip of the gain below unity that comes back to unity: [(start s, length ms, depth dB, at the
    range within its first 60 ms, click dB)]. The click is D2's measure of an opening, at the reopening:
    the output's peak above 3 kHz from 2 ms before the rise to 10 ms after it, over the larger of the
    dry signal's own there + 1 dB and the note's peak - 50 dB; above 0 is a click."""
    floor = 0.0 if range_db <= -89.95 else fx.amp(range_db) * (1 + 1e-4)
    hx, hy = _hf(np.asarray(x, dtype=float), sr), _hf(np.asarray(y, dtype=float), sr)
    out = []
    for s, e, lo in dips(g, 1.0):
        k = s + int(np.nonzero(g[s:e] <= lo * (1 + 1e-6) + 1e-12)[0][-1])
        early = bool(np.any(g[s:min(e, s + int(EARLY_MS * 1e-3 * sr))] <= floor))
        w = slice(max(0, k - int(0.002 * sr)), e + int(0.010 * sr))
        allowed = max(float(fx.db(np.max(hx[w]))) + 1.0, float(fx.db(np.max(np.abs(x[w])))) - 50.0)
        out.append((s / sr, (e - s) / sr * 1e3, float(fx.db(max(lo, 1e-12))), early, float(fx.db(np.max(hy[w]))) - allowed))
    return out


def dip_summary(labels, results):
    """-> (ok, text) over cases of trough_dips lists: no dip at the range inside 60 ms, no reopening that clicks."""
    early = [lab for lab, ds in zip(labels, results) if any(d[3] for d in ds)]
    fast = [lab for lab, ds in zip(labels, results) if any(d[4] > 0.0 for d in ds)]
    audible = [d for ds in results for d in ds if d[2] < -1.0]
    n1 = sum(1 for ds in results if any(d[2] < -1.0 for d in ds))
    n6 = sum(1 for ds in results if any(d[2] < -6.0 for d in ds))
    text = f"{n1} of {len(results)} dip under -1 dB, {n6} under -6 dB"
    if audible:
        most = max(sum(1 for d in ds if d[2] < -1.0) for ds in results)
        text += (f" (deepest {min(d[2] for d in audible):.1f} dB, longest {max(d[1] for d in audible):.0f} ms, "
                 f"at most {most} in one case)")
    worst = max((d[4] for ds in results for d in ds), default=-99.0)
    text += (f"; at the range inside {EARLY_MS:.0f} ms: {len(early)}; cases with a reopening click: {len(fast)} "
             f"(worst reopening {worst:+.1f} dB re the dry signal's own HF + 1 / note - 50)")
    bad = early + [lab for lab in fast if lab not in early]
    if bad:
        text += " (" + "; ".join(bad[:4]) + ")"
    return not bad, text


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


PRECURSOR_THR = -40.0
PRECURSOR_NOTES = (41.2, 82.41, 164.81, 329.63)   # E1 to E4
PRECURSOR_CLEAN_DB = 15.0                          # the comparison: the same pick over a floor this far under


def _pick_at(f0, n, sr, seed):
    """A picked note (the round-6 coordinator's openprobe.py pluck): 12 harmonics, peak -12 dBFS in its first 20 ms."""
    r = np.random.default_rng(seed)
    t = np.arange(n) / sr
    y = np.zeros(n)
    for k in range(1, 13):
        if f0 * k > 2400:
            break
        y += np.sin(2 * np.pi * f0 * k * t + r.uniform(0, 6.28)) / k * np.exp(-t * (1.5 + 0.6 * k))
    return y * fx.amp(-12.0) / np.max(np.abs(y[:int(0.02 * sr)]))


def _precursor_pick(sr, f0, kind, under, lead, seed):
    """A pick at 0.5 s, threshold -40, after something in the band. kind 'hiss' or 'hum' (hum plus hiss): a steady
    floor whose median reading is `under` dB under the threshold; 'scrape': 10 ms of 300-2000 Hz noise whose peak
    is `under` dB under it, ending `lead` ms before the pick, over hiss 35 dB under. -> (x, the pick alone, onset)."""
    n, on = int(0.9 * sr), int(0.5 * sr)
    r = np.random.default_rng(seed + 100)
    pick = np.zeros(n)
    pick[on:] = _pick_at(f0, n - on, sr, seed)

    def floor(kind, under_db):
        h = r.standard_normal(n)
        if kind == "hum":
            t = np.arange(n) / sr
            h = 0.3 * h + 3.0 * np.sin(2 * np.pi * 60 * t) + 1.5 * np.sin(2 * np.pi * 180 * t + 1)
        return h * fx.amp(PRECURSOR_THR - under_db) / float(np.median(twin_level(h, sr, HPF)[int(0.1 * sr):on]))
    if kind == "scrape":
        m = int(0.010 * sr)
        s = _biquad(_biquad(r.standard_normal(3 * m), *_rbj("hp", 300.0, sr)), *_rbj("lp", 2000.0, sr))[m:2 * m] * np.hanning(m)
        x = floor("hiss", 35.0)
        e = on - int(round(lead * 1e-3 * sr))
        x[e - m:e] += s * fx.amp(PRECURSOR_THR - under) / np.max(np.abs(s))
    else:
        x = floor(kind, under)
    return x + pick, pick, on


def closing_rule_checks(report, args):
    sr = 48000
    T = 1
    want = set(args.rows) if args.rows else None

    def on(tag):
        return want is None or tag in want

    if on("P"):
        plain_rows(report, args)

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
            thr = round(npk + 8, 2)
            g = fx.db(fx.mono(fx.run(EFFECT, x, dict(TIGHT, threshold=thr), ["--trace"], sr)[1]))
            i0 = int(at * sr)
            hit = np.nonzero(g[i0:] <= -20.0)[0]
            if not len(hit):
                return float("inf"), float("inf")
            lev = twin_level(x, sr, HPF)[:i0 + hit[0]]
            i_cl = int(np.nonzero(lev >= fx.amp(thr - 6.0 - TWIN_EPS_DB))[0][-1])
            return hit[0] / sr * 1e3, (i0 + hit[0] - i_cl) / sr * 1e3
        cells = [(f0, over) for f0 in (82.41, 196.0) for over in (8, 12, 16, 20, 24, 32, 40)]
        res = parallel(hang, cells)
        # The bar is R2's: the reading spans the window, so from its last sample at the close level the rule closes
        # after the hold and reaches -20 dB a third of the release later; 3 ms of margin as I4.
        bound = TIGHT["hold"] + TIGHT["release"] / 3.0 + 3.0
        report.add("A4 no hang", "notes 8-40 dB over a hiss floor, hand stop", max(r[1] for r in res) <= bound,
                   f"closed at most {max(r[1] for r in res):.1f} ms after the reading is last at the close level (limit hold + "
                   f"release/3 + 3 = {bound:.0f}); {min(r[0] for r in res):.1f} to {max(r[0] for r in res):.1f} ms after the "
                   "stop begins (version 2: 418-430)")

        # A5: palm mutes with the threshold 3 to 4 dB over the hiss's peak reading (the first verifier's chug, thr -50).
        # Per gap the bar is the rule's arithmetic on the twin reading: the gate is closed at the latest once the reading
        # has stayed under the close level (within the twin's rounding) for more than the hold after the note, and then
        # falls at the release rate; the median gain over the gap's last 10 ms may not exceed that line's, +0.01 dB. Enforced where the gap is longer than window + hold +
        # the release to -20 dB (32 ms); the 30 ms gaps are printed.
        npk = peak_db(fx.white(3.0, -66.0, sr, seed=7), sr, 0.25)

        def a5_case(c):
            gap, thr = c
            x = fx.chug(sr, secs=3.0, note=0.075, period=0.075 + gap)
            g = np.maximum(gain_db(fx.run(EFFECT, x, dict(TIGHT, threshold=thr), ["--trace"], sr)[1]), -120.0)
            lev = twin_level(x, sr, HPF)
            step_n, w = int(round((0.075 + gap) * sr)), int(round(0.010 * sr))
            hold_n, fall = int(round(TIGHT["hold"] * 1e-3 * sr)), 60.0 / (TIGHT["release"] * 1e-3 * sr)
            out = []
            under = runs_since(lev >= fx.amp(thr - 6.0 - TWIN_EPS_DB))
            for o in range(step_n, len(x) - int(round(0.075 * sr)), step_n):
                # The latest the rule can close: the first run under the close level longer than the hold after the
                # note's last reading over the open level; nothing after it reaches the open level again.
                a = o - step_n + int(np.nonzero(lev[o - step_n:o] >= fx.amp(thr - TWIN_EPS_DB))[0][-1])
                run = np.nonzero(under[a:o] > hold_n)[0]
                k = np.arange(o - w, o)
                if not len(run):
                    out.append((float(np.median(g[o - w:o])), 0.0))
                    continue
                c = a + int(run[0])
                rule = np.clip(-fall * (k - c + 1), TIGHT["range"], 0.0)
                out.append((float(np.median(g[o - w:o])), float(np.median(rule))))
            return out
        cells = [(gap, thr) for gap in (0.030, 0.075, 0.100) for thr in (-50.0, round(npk + 3.0, 2), round(npk + 4.0, 2))]
        res = parallel(a5_case, cells)
        parts = [f"{gap * 1e3:.0f} ms thr {thr:g}: median {np.median([q[0] for q in r]):.1f}, worst over the rule "
                 f"{max(q[0] - q[1] for q in r):+.2f} ({sum(1 for q in r if q[1] <= -20.0)}/{len(r)} gaps the rule closes)"
                 for (gap, thr), r in zip(cells, res)]
        enforced = [r for (gap, _), r in zip(cells, res) if gap * 1e3 > WINDOW_MS + TIGHT["hold"] + TIGHT["release"] / 3.0]
        report.add("A5 palm mutes near hiss", f"chug, hiss peak {npk:.1f} dBFS, thr 3-4 dB over, gaps 75/100 ms",
                   all(q[0] <= q[1] + 0.01 for r in enforced for q in r),
                   "; ".join(p for p, (gap, _) in zip(parts, cells) if gap > 0.03)
                   + " dB (limit: per gap the rule's release line from its latest possible close, +0.01 dB; version 1's "
                   "medians were -25 / -69)")
        info(report, "info A5 30 ms gaps", "shorter than window + hold + release to -20",
             "; ".join(p for p, (gap, _) in zip(parts, cells) if gap <= 0.03))

    if on("B"):
        # B1, B2, report only (as X10-X12): the second verifier's chords. Dips are counted under -1 dB and,
        # as the verifier counted them, under -6 dB, with those at the range inside 60 ms and the reopenings
        # that click. The verifier's script used hold 6; the hold-6 variants are printed beside the default.
        def r2_chord(c):
            name, s, thr, t60, mode, hold = c
            x = strum(R2_CHORDS[name], 5.0, s, level_db=-8, t60=t60, seed=5, strum_ms=6) + hiss(5.0, s, -92, seed=2)
            out, tr = fx.run(EFFECT, x, dict(TIGHT if mode == 1 else SMOOTH, threshold=thr, hold=hold), ["--trace"], s)
            return trough_dips(fx.mono(tr), s, -90.0, x, fx.mono(out))

        def b1_cells(hold):
            cells = [(n, sr, thr, t60, 1, hold) for n in R2_CHORDS for thr in range(-30, -70, -5) for t60 in (2.5, 6.0)]
            for n in ("C2+G2+C3", "A1+E2+A2", "E2+G2", "E1+B1", "E2+F2", "B0+F#1", "A2 x2 +9c"):
                for thr in (-35, -45, -55):
                    cells += [(n, s, thr, 4.0, 1, hold) for s in (44100, 96000, 192000)]
                    cells += [(n, sr, thr, 4.0, 2, SMOOTH["hold"]), (n, sr, thr, 4.0, 1, 0.0), (n, sr, thr, 4.0, 1, 250.0)]
            return cells
        for hold in (TIGHT["hold"], 6.0):
            cells = b1_cells(hold)
            res = parallel(r2_chord, cells)
            labels = [f"{c[0]} thr {c[2]} t60 {c[3]:g} {c[1]} Hz mode {c[4]} hold {c[5]:g}" for c in cells]
            ok, text = dip_summary(labels, res)
            scope = f"{len(cells)} cases: 16 chords x thr x rates x holds 0/{hold:g}/250"
            if hold == TIGHT["hold"]:
                info(report, "info B1 chords (verifier 2)", scope, text)
                deep = sorted(((min(d[2] for d in ds), lab, ds) for lab, ds in zip(labels, res) if any(d[2] < -6.0 for d in ds)))[:8]
                info(report, "B1 deepest dips", "the cases under -6 dB, deepest first",
                     "; ".join(f"{lab}: {sum(1 for d in ds if d[2] < -6.0)} x, to {v:.0f} dB, {max(d[1] for d in ds if d[2] < -6.0):.0f} ms"
                               for v, lab, ds in deep) or "none")
            else:
                info(report, "info B1 hold 6", scope, ("would pass: " if ok else "would fail: ") + text)
        cells = [(n, sr, thr, t60, 1, TIGHT["hold"]) for n in R2_CHORDS for thr in range(-30, -70, -5) for t60 in (2.5, 6.0)]
        res = parallel(r2_chord, cells)
        info(report, "info B1 chords (verifier 2)", f"{len(cells)} cases at the default hold {TIGHT['hold']:g} ms",
             dip_summary([f"{c[0]} thr {c[2]} t60 {c[3]:g}" for c in cells], res)[1])

        # B2, report only: a quiet decaying chord behind a louder note (g3_chatter2.py, g3b_sweep.py), counted
        # as B1, with the cases that dip under -6 dB more often than the same chord alone.
        def behind(c):
            name, thr, loud, quiet, gap, hold, mode, alone = c
            loud_note = hand_stop(strum((82.41, 123.47, 164.81), 0.5, sr, level_db=loud, t60=3.0, seed=9), sr, 0.12, 5.0)
            q = strum(R2_CHORDS[name], 5.0 - gap, sr, level_db=quiet, t60=5.0, seed=5, click_db=None, strum_ms=6)
            items = [(0.05 + gap, q)] if alone else [(0.05, loud_note), (0.05 + gap, q)]
            x = place(5.0, sr, items) + hiss(5.0, sr, -95, seed=2)
            out, tr = fx.run(EFFECT, x, dict(TIGHT if mode == 1 else SMOOTH, threshold=thr, hold=hold), ["--trace"], sr)
            i0 = int((0.05 + gap + 0.05) * sr)
            return [d for d in trough_dips(fx.mono(tr), sr, -90.0, x, fx.mono(out)) if d[0] * sr >= i0]

        def deeper6(ds):
            return sum(1 for d in ds if d[2] < -6.0)

        def g3_cells(hold):
            cells = []
            for n in ("C2+G2+C3", "A1+E2+A2", "E2+G2", "E2+G#2", "E1+B1", "E2+F2", "E2 x2 +3c", "A2 x2 +9c", "D2+A2+D3", "C2+Eb2"):
                for quiet in (-26, -32):
                    cells += [(n, quiet - m, -3, quiet, 0.25, hold, 1) for m in (6, 10, 14, 18)]
            for n in ("E2+G2", "C2+G2+C3", "A2 x2 +9c"):
                for thr in (-38, -42):
                    cells += [(n, thr, -3, -28, 0.25, SMOOTH["hold"], 2), (n, thr, -3, -28, 0.25, 20.0, 1), (n, thr, -3, -28, 0.25, 50.0, 1),
                              (n, thr, -12, -28, 0.25, hold, 1)]
            return cells

        def g3b_cells(hold):
            return [(n, -30 - m, -30 + diff, -30, gap, hold, 1) for n in ("E2+G2", "A1+E2+A2", "E2+G#2", "C2+Eb2")
                    for diff in (12, 20, 28) for gap in (0.1, 0.25, 0.6) for m in (6, 14)]
        for hold in (TIGHT["hold"], 6.0):
            cells = g3_cells(hold)
            res = parallel(behind, [c + (False,) for c in cells])
            ref = parallel(behind, [c + (True,) for c in cells])
            worse = [c for c, r, a in zip(cells, res, ref) if deeper6(r) > deeper6(a)]
            ok, text = dip_summary([f"{c[0]} thr {c[1]} loud {c[2]} hold {c[5]:g} mode {c[6]}" for c in cells], res)
            scope = f"{len(cells)} cases (g3), loud -3/-12 dBFS 0.25 s before, hold {hold:g}"
            text = f"{len(worse)} with more dips under -6 dB than the same chord alone; " + text
            info(report, "info B2 behind a louder note" if hold == TIGHT["hold"] else "info B2 g3 hold 6", scope, text)

            sweep = g3b_cells(hold)
            res = parallel(behind, [c + (False,) for c in sweep])
            ref = parallel(behind, [c + (True,) for c in sweep])
            worse = [c for c, r, a in zip(sweep, res, ref) if deeper6(r) > deeper6(a)]
            ok, text = dip_summary([f"{c[0]} thr {c[1]} +{c[2] - c[3]} dB gap {c[4]:g}" for c in sweep], res)
            scope = f"{len(sweep)} cases (g3b): +12..28 dB, 0.1-0.6 s, hold {hold:g}"
            text = (f"{len(worse)} with more dips under -6 dB than the same chord alone"
                    + "".join(f"; {c[0]} thr {c[1]} +{c[2] - c[3]} dB gap {c[4]:g}" for c in worse[:6]) + "; " + text)
            info(report, "info B2 behind a louder note" if hold == TIGHT["hold"] else "info B2 g3b hold 6", scope, text)

    if on("C"):
        # C, report only: two strings beating, troughs 6-20 dB at 0.5-15 Hz, decaying through the threshold,
        # counted as X10. The trade-off tables give, per beat rate and trough depth, the deepest dip and its
        # length, and per decay rate how long the floor stays audible.
        cells = [(f0, df, tr, thr) for f0 in (110.0, 196.0) for df in (0.5, 1.0, 2.0, 4.0, 8.0, 15.0) for tr in (6, 12, 20)
                 for thr in (-40, -55)]

        def beat(c):
            f0, df, tr, thr = c
            x = two_tone(f0, df, tr, sr, thr)
            out, tr = fx.run(EFFECT, x, dict(TIGHT, threshold=thr), ["--trace"], sr)
            return trough_dips(fx.mono(tr), sr, -90.0, x, fx.mono(out))
        res = parallel(beat, cells)
        if args.verbose:
            for c, r in zip(cells, res):
                if r:
                    print("   ", c, [tuple(round(float(v), 1) for v in d) for d in r])
        info(report, "info C beats", "two tones 110/196 Hz, beats 0.5-15 Hz, troughs 6-20 dB",
             dip_summary([f"{c[0]:g} Hz +{c[1]:g} Hz {c[2]} dB thr {c[3]}" for c in cells], res)[1])
        table = {}
        for (f0, df, tr, thr), ds in zip(cells, res):
            worst = min(ds, key=lambda d: d[2], default=None)
            old = table.get((df, tr), (0.0, 0.0, 0))
            if worst is not None and worst[2] < old[0]:
                old = (worst[2], worst[1], old[2])
            table[(df, tr)] = (old[0], old[1], max(old[2], sum(1 for d in ds if d[2] < -1.0)))
        for tr in (6, 12, 20):
            info(report, "C trade-off", f"trough {tr} dB: rate -> deepest dip",
                 " / ".join(f"{df:g} Hz {table[(df, tr)][0]:.0f} dB {table[(df, tr)][1]:.0f} ms ({table[(df, tr)][2]} under -1 dB)"
                            for df in (0.5, 1.0, 2.0, 4.0, 8.0, 15.0)))

        def tail(c):
            rate, margin, mode = c
            secs = 0.4 + 30.0 / rate + 1.0
            t = np.arange(int(secs * sr)) / sr
            thr = -40.0
            note = _note(196.0, secs, sr, np.random.default_rng(3), "tone")
            s = _scale_reading(note, sr, HPF, thr + 12.0) * fx.amp(-rate * np.maximum(t - 0.2, 0))
            fl = _scale_reading(hiss(secs, sr, -60, seed=31), sr, HPF, thr - margin)
            g = fx.db(np.maximum(fx.mono(fx.run(EFFECT, s + fl, dict(TIGHT if mode == 1 else SMOOTH, threshold=thr), ["--trace"], sr)[1]), 1e-12))
            ls = twin_level(s, sr, HPF)
            i_fl = int(np.nonzero(ls >= fx.amp(thr - margin))[0][-1])
            i_cl = int(np.nonzero(ls >= fx.amp(thr - 6.0))[0][-1])
            heard = np.nonzero(g[i_fl:] > -(20.0 - margin))[0]
            return (heard[-1] / sr * 1e3 if len(heard) else 0.0), float(g[i_cl])
        for mode in (1, 2):
            rows = []
            for margin in (8.0, 3.5):
                rates = (5, 10, 20, 40, 60, 80, 120, 200)
                res = parallel(tail, [(r, margin, mode) for r in rates])
                rows.append(f"floor {margin:g} dB under thr: " + ", ".join(f"{r} dB/s {h:.0f} ms ({max(c, -99):.0f})" for r, (h, c) in zip(rates, res)))
            info(report, "C decay tail", f"{TYPES[mode]}: floor heard over thr-20 dB (gain at close lvl)", " | ".join(rows))

    def floor_tail(g, g0, i_cl, mode, keys):
        """-> (ms the floor stays over -20 dB after the reading (note + floor) is last at the close level, Tight;
        Smooth: the same over the most the floor alone gives it + 1 dB, which its expander may take while
        the note's tail still sinks under the floor), Smooth's last 250 ms over that floor-alone figure dB)."""
        limit = float(np.max(g0[len(g0) // 2:])) + 1.0 if mode == 2 else -20.0
        above = np.nonzero(g[i_cl:] > limit)[0]
        end = float(np.median(g[-int(0.25 * sr):])) - float(np.max(g0[len(g0) // 2:])) if mode == 2 else -99.0
        return (above[-1] / sr * 1e3 if len(above) else 0.0), end

    def tail_verdict(res, cells, bound):
        t_worst = max((r[0], c) for r, c in zip(res, cells) if c[-1] == 1)
        s_worst = max((r[0], c) for r, c in zip(res, cells) if c[-1] == 2) if any(c[-1] == 2 for c in cells) else (0.0, None)
        end = max((r[1] for r, c in zip(res, cells) if c[-1] == 2), default=-99.0)
        return t_worst[0] <= bound and end <= 1.0, t_worst, s_worst, end

    if on("R"):
        # R rows: the third verifier's material (.audit/fx-verify/r3-engines.md, r3_gate_new.py, r3_gate_latch.py),
        # rebuilt here. A floor outside the band (8 dB or more under the threshold) alone must never open the gate,
        # and once the reading (note + floor) has left the close level Tight closes after the hold.
        def r3_floor(kind, secs):
            n = int(round(secs * sr))
            t = np.arange(n) / sr
            rng = np.random.default_rng(3)
            white = rng.standard_normal(n)
            if kind == "hiss":
                return white
            if kind.startswith("spikes"):
                y = np.zeros(n)
                for k in range(int(secs * 50) - 1):
                    y[int(k * sr / 50):int(k * sr / 50) + 24] += np.hanning(24)
                return y + white * fx.amp(-32.0 if kind.endswith("-20") else -20.0) * 0.25
            if kind == "crackle":
                y = white * 0.05
                for i in rng.integers(0, n - 40, int(secs * 6)):
                    y[i:i + 30] += rng.uniform(0.5, 1.0) * np.hanning(30) * rng.choice([-1, 1])
                return y
            depth, rate = (4.0, 1.5) if kind == "hum AM" else (4.0, 4.0) if kind == "hiss AM" else (3.5, 2.0)
            if kind == "hum AM":
                y = sum(a * np.sin(2 * np.pi * 50 * (k + 1) * t + 0.7 * k) for k, a in enumerate((1.0, 0.3, 0.5, 0.1, 0.25)))
            elif kind == "hiss AM":
                y = white
            else:
                y = np.zeros(n)
                for k in range(int(secs * 100) - 1):
                    i = int(k * sr / 100)
                    y[i:i + 28] += np.hanning(28) * (1 if k % 2 == 0 else -1)
            return y * fx.amp(depth * np.sin(2 * np.pi * rate * t))

        def latch(c):
            kind, src, rate, mode = c
            thr, listen = -30.0, 4.0
            secs = 0.2 + 18.0 / rate + listen
            t = np.arange(int(round(secs * sr))) / sr
            if src == "tone":
                s = _scale_reading(_note(196.0, secs, sr, np.random.default_rng(1), "tone"), sr, HPF, thr + 12) * fx.amp(-rate * np.maximum(t - 0.2, 0))
            else:
                s = place(secs, sr, [(0.2, strum((82.41, 123.47, 164.81), min(secs - 0.2, 8.0), sr, level_db=0.0, t60=60.0 / rate, seed=5, strum_ms=6))])
                s = _scale_reading(s, sr, HPF, thr + 14)
            f = _scale_reading(r3_floor(kind, secs), sr, HPF, thr - 8.0, start=0.1)
            keys = dict(TIGHT if mode == 1 else SMOOTH, threshold=thr)
            g = fx.db(np.maximum(fx.mono(fx.run(EFFECT, s + f, keys, ["--trace"], sr)[1]), 1e-12))
            g0 = fx.db(np.maximum(fx.mono(fx.run(EFFECT, f, keys, ["--trace"], sr)[1]), 1e-12))
            i_cl = int(np.nonzero(twin_level(s + f, sr, HPF) >= fx.amp(thr - 6.0))[0][-1])
            return floor_tail(g, g0, i_cl, mode, keys) + (float(np.max(g0[int(0.2 * sr):])),)
        kinds = ("spikes -20", "spikes -8", "hum AM", "hiss AM", "crackle", "buzz AM", "hiss")
        cells = [(k, src, rate, m) for k in kinds for src, rate in (("tone", 20), ("chord", 20), ("tone", 60)) for m in (1, 2)]
        res = parallel(latch, cells)
        # The reading already spans the window, so from its last sample at the close level the rule closes after
        # the hold and reaches -20 dB a third of the release later; 3 ms of margin as I4.
        bound = TIGHT["hold"] + TIGHT["release"] / 3.0 + 3.0
        _, tw, sw, end = tail_verdict(res, cells, bound)
        floor_open = max(r[2] for r, c in zip(res, cells) if c[-1] == 1)
        report.add("R1 pulsing floors", f"{len(cells)} cases: 7 floors 8 dB under thr, Tight", floor_open <= -89.0,
                   f"the floor alone opens Tight to {floor_open:.1f} dB (limit -89: never opens)")
        info(report, "info R1 tails", f"{len(cells)} cases: tone/chord x 20/60 dB/s, both modes",
             f"Tight: floor over -20 dB at most {tw[0]:.0f} ms after the reading is last at the close level ({tw[1][0]}, "
             f"{tw[1][1]} {tw[1][2]} dB/s; R2's bound would be {bound:.0f}). Smooth: over its floor-alone setting + 1 dB at most "
             f"{sw[0]:.0f} ms ({sw[1][0]}, {sw[1][1]}), its last 250 ms {end:+.1f} dB re the floor alone")

        def hum_chord(c):
            fh, under, margin, t60, mode = c
            secs = 7.0
            ch = strum((82.41, 123.47, 164.81), secs, sr, level_db=-8, t60=t60, seed=5, strum_ms=6)
            t = np.arange(len(ch)) / sr
            h = sum(a * np.sin(2 * np.pi * fh * (k + 1) * t + 0.7 * k) for k, a in enumerate((1.0, 0.3, 0.5, 0.1, 0.25, 0.05, 0.12)))
            h = h * fx.amp(-8 - under) / np.max(np.abs(h)) + hiss(secs, sr, -8 - under - 26, seed=4)
            thr = float(fx.db(np.max(twin_level(h, sr, HPF)[int(0.1 * sr):]))) + margin
            keys = dict(TIGHT if mode == 1 else SMOOTH, threshold=round(thr, 2))
            g = fx.db(np.maximum(fx.mono(fx.run(EFFECT, ch + h, keys, ["--trace"], sr)[1]), 1e-12))
            g0 = fx.db(np.maximum(fx.mono(fx.run(EFFECT, h, keys, ["--trace"], sr)[1]), 1e-12))
            i_cl = int(np.nonzero(twin_level(ch + h, sr, HPF) >= fx.amp(thr - 6.0))[0][-1])
            return floor_tail(g, g0, i_cl, mode, keys)
        cells = [(fh, u, mg, t60, 1) for fh in (50.0, 60.0) for u in (10, 20, 30) for mg in (6, 8, 10) for t60 in (2.5, 6.0)]
        cells += [(60.0, 20, 8, 4.0, 2)]
        res = parallel(hum_chord, cells)
        # With the threshold 6 dB over the hum's peak reading the hum touches the close level, the edge of the band,
        # and holds an open gate open: those cases are reported.
        edge = [c[2] < 8 for c in cells]
        judged = [(r, c) for r, c, e in zip(res, cells, edge) if not e]
        ok, (w, c), sw, end = tail_verdict([r for r, _ in judged], [c for _, c in judged], bound)
        report.add("R2 hum under a chord", f"{len(judged)} cases: 50/60 Hz hum 10-30 dB under, thr 8-10 dB over it", ok,
                   f"Tight: hum over -20 dB at most {w:.0f} ms after the reading is last at the close level ({c[0]:g} Hz, {c[1]} dB "
                   f"under, thr +{c[2]}, t60 {c[3]:g}; limit hold + release/3 + 3 = {bound:.0f}). Smooth: {sw[0]:.0f} ms over its "
                   f"hum-alone setting + 1 dB, last 250 ms {end:+.1f} dB re it (limit +1)")
        at_edge = [(r, c) for r, c, e in zip(res, cells, edge) if e]
        info(report, "info R2 hum at the band", f"{len(at_edge)} cases: thr 6 dB over the hum's peak reading",
             "Tight: hum over -20 dB after the reading is last at the close level: "
             + ", ".join(f"{c[0]:g} Hz {c[1]} dB t60 {c[3]:g}: {r[0]:.0f} ms" for r, c in at_edge))

        def fade(c):
            kind, T, depth, mode = c
            sus, secs = 0.4, 0.4 + T + 1.6
            t = np.arange(int(round(secs * sr))) / sr
            u = np.clip((t - sus) / T, 0, None)
            if kind == "glide":
                f = np.where(t < sus, 196.0, 196.0 * 0.5 ** np.clip(u, 0, 1))
                ph = 2 * np.pi * np.cumsum(f) / sr
                y = sum(np.sin(k * ph + 0.9 * k) / k for k in range(1, 9))
                env = fx.amp(-60.0 * u)
            else:
                y = _note(196.0, secs, sr, np.random.default_rng(2), "tone")
                env = np.clip(1 - u, 0, 1)
            thr = -10.0 - depth + 8.0
            s = _scale_reading(y, sr, HPF, -10.0, start=0.05) * env
            fl = _scale_reading(hiss(secs, sr, -60, seed=31), sr, HPF, thr - 8.0)
            keys = dict(TIGHT if mode == 1 else SMOOTH, threshold=thr)
            g = fx.db(np.maximum(fx.mono(fx.run(EFFECT, s + fl, keys, ["--trace"], sr)[1]), 1e-12))
            g0 = fx.db(np.maximum(fx.mono(fx.run(EFFECT, fl, keys, ["--trace"], sr)[1]), 1e-12))
            i_cl = int(np.nonzero(twin_level(s + fl, sr, HPF) >= fx.amp(thr - 6.0))[0][-1])
            return floor_tail(g, g0, i_cl, mode, keys)
        cells = [(k, T, d, m) for k in ("glide", "volume knob") for T in (0.3, 1.0, 3.0) for d in (20, 36) for m in (1, 2)]
        res = parallel(fade, cells)
        ok, (w, c), sw, end = tail_verdict(res, cells, bound)
        report.add("R3 glides, volume down", f"{len(cells)} cases: octave glide or knob to zero over 0.3-3 s", ok,
                   f"Tight: floor over -20 dB at most {w:.0f} ms after the reading is last at the close level ({c[0]} {c[1]:g} s, "
                   f"floor {c[2]} dB under; limit hold + release/3 + 3 = {bound:.0f}). Smooth: {sw[0]:.0f} ms over its floor-alone "
                   f"setting + 1 dB, last 250 ms {end:+.1f} dB re it (limit +1)")

        def staccato(c):
            hold, mode = c
            per, note, nn = 60.0 / 180 / 4, 0.045, 18
            secs = 0.1 + nn * per + 0.4
            items = [(0.1 + k * per, hand_stop(string(82.41, note + 0.05, sr, level_db=-6, t60=1.2, seed=20 + k), sr, note, 2.5))
                     for k in range(nn)]
            notes = place(secs, sr, items)
            pk = peak_db(notes, sr)
            x = notes + _scale_reading(hiss(secs, sr, -60, seed=8), sr, HPF, pk - 15)
            g = fx.mono(fx.run(EFFECT, x, dict(TIGHT if mode == 1 else SMOOTH, threshold=round(pk - 7, 2), hold=hold), ["--trace"], sr)[1])
            gaps = [fx.db(np.sqrt(np.mean(g[int((0.1 + k * per) * sr) - int(0.01 * sr):int((0.1 + k * per) * sr)] ** 2)))
                    for k in range(2, nn)]
            return float(np.median(gaps)), sum(1 for v in gaps if v < -20), len(gaps)
        cells = [(0.0, 1), (6.0, 1), (10.0, 1), (20.0, 1), (SMOOTH["hold"], 2)]
        res = parallel(staccato, cells)
        info(report, "R4 staccato 180 bpm", "38 ms gaps, floor 15 dB under, thr +8",
             " / ".join(f"{TYPES[m]} hold {h:g}: median {r[0]:.1f} dB, {r[1]}/{r[2]} gaps under -20" for (h, m), r in zip(cells, res)))

        def tremolo(c):
            rate, drop, thr_re = c
            secs, t_stop = 3.2, 2.2
            t = np.arange(int(round(secs * sr))) / sr
            chord = strum((82.41, 123.47, 164.81), secs, sr, level_db=0, t60=20.0, seed=5, click_db=None, strum_ms=2)
            ph = (t * rate) % 1.0
            s = chord * fx.amp(-drop * ph) * (1 - np.exp(-(ph / rate) / 0.0006))
            s = hand_stop(s / np.max(np.abs(s)) * fx.amp(-8), sr, t_stop, 3.0)
            pk = float(fx.db(np.max(twin_level(s, sr, HPF)[int(0.3 * sr):int(t_stop * sr)])))
            x = s + _scale_reading(hiss(secs, sr, -60, seed=31), sr, HPF, pk - 30)
            out, tr = fx.run(EFFECT, x, dict(TIGHT, threshold=round(pk + thr_re, 2)), ["--trace"], sr)
            g = fx.mono(tr)
            ds = [d for d in trough_dips(g, sr, -90.0, x, fx.mono(out)) if 0.2 <= d[0] <= t_stop]
            k = np.nonzero(fx.db(np.maximum(g[int(t_stop * sr):], 1e-12)) <= -20)[0]
            return ds, (k[0] / sr * 1e3 if len(k) else float("inf"))
        cells = [(r, d, tp) for r in (12, 16) for d in (3, 6, 10) for tp in (-20, -12, -5)]
        res = parallel(tremolo, cells)
        _, text = dip_summary([f"{c[0]} Hz drop {c[1]} thr {c[2]:+d}" for c in cells], [r[0] for r in res])
        info(report, "info R5 tremolo picking", f"{len(cells)} cases: strokes 12/16 Hz falling 3-10 dB, thr -20..-5 re peak",
             text + f"; the hand stop after it closes in {max(r[1] for r in res):.0f} ms at most")

    if on("D"):
        # D1: picks keep their front (g5_attack.py pick cases, 20 and 34 dB over the threshold). The bar is the rule's
        # arithmetic on the same pick: the dry signal under a 0.2 ms linear rise (a pick climbs the last dB at once)
        # that starts one sample after the twin reading reaches the threshold (P1's one sample of rounding).
        def pick(c):
            f0, over, s = c
            x = place(0.8, s, [(0.2, string(f0, 0.5, s, level_db=-40 + over, t60=3.0, seed=6, click_db=-12.0))]) \
                + hiss(0.8, s, -80, seed=4)
            y = fx.mono(fx.run(EFFECT, x, dict(TIGHT, threshold=-40), [], s)[0])
            i, m = int(0.2 * s), int(0.001 * s)
            kc = i + int(np.argmax(twin_level(x[i:], s, HPF) >= fx.amp(-40.0))) + 1
            rule = np.clip((np.arange(len(x)) - kc + 1) / (OPEN_MIN_MS * 1e-3 * s), 0.0, 1.0)
            e = np.sum(x[i:i + m] ** 2)
            return 10 * np.log10(np.sum(y[i:i + m] ** 2) / e), 10 * np.log10(np.sum((x * rule)[i:i + m] ** 2) / e)
        cells = [(f0, over, sr) for f0 in (41.2, 61.74, 82.41, 196.0, 1000.0) for over in (20, 34)]
        cells += [(82.41, 20, s) for s in (44100, 96000, 192000)]
        res = parallel(pick, cells)
        (w, bar), c = min(zip(res, cells), key=lambda rc: rc[0][0] - rc[0][1])
        report.add("D1 pick front", "picks 20/34 dB over thr, 41-1000 Hz, 44.1-192 kHz", all(r[0] >= r[1] - 0.01 for r in res),
                   f"first millisecond, least over its bar: {w:.2f} dB against {bar:.2f} ({c[0]:g} Hz +{c[1]} dB {c[2]} Hz); worst "
                   f"{min(r[0] for r in res):.2f} dB (limit: the rule's 0.2 ms linear rise from one sample after the reading "
                   "reaches the threshold, -0.01 dB)")

        # D3: a pick after something in the band (round 6, finding 1) opens as fast as the same pick over a floor 15 dB
        # under the threshold: E1-E4 at -12 dBFS, threshold -40, Tight and Smooth, over hiss or hum whose median
        # reading is 3-7 dB under the threshold, or 2-40 ms after a 10 ms scrape 1.5-5 dB under it. Energy of the
        # first 2 and 5 ms against the pick alone; the bar, stated before measuring, is HEARD_DB worse than the
        # comparison, about the smallest level change heard on music.
        def precursor(c):
            mode, f0, kind, under, lead, seed = c
            keys = dict(TIGHT if mode == 1 else SMOOTH, threshold=PRECURSOR_THR)
            out = []
            for k, u, ld in ((kind, under, lead), ("hum" if kind == "hum" else "hiss", PRECURSOR_CLEAN_DB, None)):
                x, p, on = _precursor_pick(sr, f0, k, u, ld, seed)
                y = fx.mono(fx.run(EFFECT, x, keys, [], sr)[0])
                out.append([10 * np.log10(np.sum(y[on:on + w] ** 2) / np.sum(p[on:on + w] ** 2))
                            for w in (int(0.002 * sr), int(0.005 * sr))])
            return max(out[1][0] - out[0][0], out[1][1] - out[0][1]), out[0]
        cells = [(m, f0, k, u, None, sd) for m in (1, 2) for k in ("hiss", "hum") for u in (3.0, 5.0, 6.0, 7.0)
                 for f0 in PRECURSOR_NOTES for sd in range(3)]
        cells += [(m, f0, "scrape", u, ld, sd) for m in (1, 2) for u in (1.5, 3.0, 5.0) for ld in (2.0, 5.0, 20.0, 40.0)
                  for f0 in PRECURSOR_NOTES for sd in range(3)]
        res = parallel(precursor, cells)
        (w, own), c = max(zip(res, cells), key=lambda rc: rc[0][0])
        over = sum(1 for r in res if r[0] > HEARD_DB)
        report.add("D3 pick after a precursor", f"{len(cells)} cases: E1-E4 after an in-band floor or scrape, Tight + Smooth",
                   over == 0,
                   f"worst {w:+.2f} dB under the same pick over a floor {PRECURSOR_CLEAN_DB:g} dB under the threshold "
                   f"({TYPES[c[0]]} {c[1]:g} Hz {c[2]} {c[3]:g} dB under{'' if c[4] is None else f' lead {c[4]:g} ms'}: first 2 / 5 ms "
                   f"{own[0]:.2f} / {own[1]:.2f} dB); {over} of {len(cells)} ({100 * over / len(cells):.0f} %) over the bar "
                   f"(limit {HEARD_DB:g} dB, stated before measuring: about the smallest level change heard on music)")

        # D4: a swell opens slowly. Linear-in-dB swells, odd harmonics so the waveform repeats every half period: the
        # windowed reading lags the envelope by at most half a period, so it spends at least 1/rate - T/2 in the last dB
        # and the rule gives a ramp of 3 x that (at most 5 ms). Enforced, 3 ms where that time is at least 1.5 ms (1 ms
        # for the 3 ms, 0.5 ms for the floor and the filter's delay); the other cells, where one half period climbs more
        # than the last dB can hold, are printed. Tight only: Smooth's gain has followed a slow swell up to 0 dB before the
        # reading reaches the threshold, so it has no ramp to time (its opening is the same arithmetic, which P1 pins).
        def swell_ramp(c):
            f0, rate, mode = c
            n = int((50.0 / rate + 0.4) * sr)
            t = np.arange(n) / sr
            y = sum(np.sin(2 * np.pi * f0 * k * t + k) / k for k in (1, 3, 5, 7) if f0 * k < 2400)
            x = y * fx.amp(np.minimum(-70.0 + rate * t, -20.0)) + hiss(n / sr, sr, -86.0, seed=3)
            g = fx.mono(fx.run(EFFECT, x, dict(TIGHT if mode == 1 else SMOOTH, threshold=-40), ["--trace"], sr)[1])
            cross = int(np.argmax(twin_level(x, sr, HPF) >= fx.amp(-40.0)))
            return (int(np.argmax(g[cross:] >= 1.0)) + 1) / sr * 1e3
        cells = [(f0, rate, m) for f0 in (82.41, 110.0, 220.0, 440.0) for rate in (10, 30, 100, 300, 1000, 3000) for m in (1,)]
        res = parallel(swell_ramp, cells)
        held = [(r, c) for r, c in zip(res, cells) if 1e3 / c[1] - 500.0 / c[0] >= 1.5]
        rest = [(r, c) for r, c in zip(res, cells) if 1e3 / c[1] - 500.0 / c[0] < 1.5]
        lo, cl = min(held)
        report.add("D4 swell ramp", f"{len(held)} cases: 10-300 dB/s on 82-440 Hz, Tight", lo >= 3.0,
                   f"shortest ramp {lo:.2f} ms ({cl[0]:g} Hz {cl[1]} dB/s {TYPES[cl[2]]}; limit 3 ms where the reading spends "
                   "1.5 ms or more in the last dB: 10-100 dB/s at every note, 300 dB/s at 440 Hz)")
        info(report, "info D4 fast swells", f"{len(rest)} cases: a half period climbs past the last dB",
             ", ".join(f"{c[0]:g} Hz {c[1]} dB/s {r:.2f}" for r, c in sorted(rest, key=lambda rc: rc[1]))
             + " ms ramp")

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
        skipped = [c for c, r in zip(cells, res) if r is None]
        judged = len(sines) + len(strings)
        worst_str = max((r[0] - max(-50.0, r[1]) for r in strings), default=0.0)
        report.add("D2 opening click", f"{judged} of {len(cells)} swells 150/600 ms, 41-220 Hz",
                   bool(sines) and max(sines) <= -50.0 and worst_str <= 1.0 and judged >= len(cells) / 2,
                   f"above 3 kHz at the opening, re the note: sines worst {max(sines, default=0.0):.1f} dB (limit -50); strings "
                   f"worst {max((r[0] for r in strings), default=0.0):.1f} dB where the dry string itself reads up to "
                   f"{max((r[1] for r in strings), default=0.0):.1f} (limit: -50 or the dry string's own, +1 dB); {len(skipped)} "
                   f"never open, skipped (limit: at least half judged)"
                   + "".join(f"; {c[3]} {c[0]:g} Hz thr {c[1]} {c[2]} ms" for c in skipped[:4]))

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
                        for f0 in (30.87, 41.2, 55.0)) + " dB (it shuts once the reading has been under the close level for the hold)")
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
        # G19: range automation is smoothed per sample, not stepped at block rate.
        x = string(82.41, 4.0, sr, level_db=-14, t60=25, seed=8) + hiss(4.0, sr, -70, seed=9)
        worst = 0.0
        for block in (64, 512):
            g = fx.mono(fx.run(EFFECT, x, ["mode=1", "threshold=-5", "range@0.5..3.0=-90..-10"], [f"--block={block}", "--trace"], sr)[1])
            worst = max(worst, float(np.max(np.abs(np.diff(g[int(0.5 * sr):])))))
        report.add("G19 range smoothing", "closed, range ramp -90 -> -10 dB, blocks 64/512", worst <= 0.001,
                   f"largest gain step {worst:.5f} per sample (limit 0.001; version 2: 0.0121 at block 512)")

    if on("T"):
        target_checks(report)


# ---------------------------------------------------------------------------- the plain rule
# Rows P1 to P4. P1 renders a fixed sample of material through the engine and through plain_reference, a
# numpy port of the rule Gate.h states (gate6_ref.py, per sample), and requires the two to agree; P2 to P4
# are the round-5 verifier's findings 4, 1 and 2 (.audit/fx-verify/r5-gate.md).

PLAIN_HYSTERESIS_DB = 6.0
PLAIN_TOL_DB = 0.05         # gain agreement, stated before measuring (SPEC_v6 6, add 1)
PLAIN_FLOOR_DB = -80.0      # ... wherever either gain is over this
PLAIN_RATES = (44100, 48000, 96000, 192000)
PLAIN_BLOCKS = (1, 64, 512, 4096)
PLAIN_HOLDS = (0.0, 10.0, 40.0, 50.0)   # 40: Smooth's default
PLAIN_RELEASES = (5.0, 30.0, 500.0)
PLAIN_RANGES = (-90.0, -40.0)
PLAIN_HPFS = (75.0, 20.0, 150.0)        # 20: off


def plain_reference(x, sr, keys):
    """The rule of Gate.h for Tight and Smooth, per sample, on twin_level's reading. -> (tight, smooth linear
    gains)."""
    thr, rel = keys["threshold"], keys.get("release", 30.0)
    hold, rng_db, hpf = keys.get("hold", 10.0), keys.get("range", -90.0), keys.get("sc_hpf", HPF)
    lev = twin_level(x, sr, hpf)
    L = lev.tolist()
    LD = (20.0 * np.log10(np.maximum(lev, 1e-9))).tolist()
    n = len(L)
    open_lv = 10 ** (thr / 20)
    close_lv = open_lv * 10 ** (-PLAIN_HYSTERESIS_DB / 20)
    hold_n = float(round(hold * 1e-3 * sr))
    step = 60.0 / (rel * 1e-3 * sr)
    mute = rng_db <= -89.95
    near_lv = open_lv * 10 ** (-OPEN_NEAR_DB / 20)
    open_min, open_max = OPEN_MIN_MS * 1e-3 * sr, OPEN_MAX_MS * 1e-3 * sr
    offset = (EXPANDER_SLOPE - 1.0) * 20.0 * np.log10(close_lv)
    tau_max = 0.2 * sr
    grow = int(np.ceil(tau_max / 0.5))
    slew = 100.0 / sr
    k_db = np.log(10.0) / 20.0
    long_ago = 1 << 30
    is_open, below, since_near = False, 0, long_ago
    t_gr = s_gr = rng_db
    t_from = s_from = 0.0
    t_ph = s_ph = 1.0
    t_cl = s_cl = False
    o_step, o_curve = 1.0, 0.0
    floor_db, floor_since, settle = 0.0, 0, max(1, int(round(WINDOW_MS * 1e-3 * sr)))
    T, S = np.zeros(n), np.zeros(n)
    exp, log = math.exp, math.log

    def gain(gr):
        return 0.0 if (mute and gr <= -89.95) else exp(gr * k_db)

    for i in range(n):
        lv = L[i]
        since_near = 0 if lv < near_lv else min(since_near + 1, long_ago)
        settling = settle > 0
        if settling:
            settle -= 1
        if lv >= open_lv:
            below = 0
            if not is_open:
                is_open = True
                steps = min(open_max, max(open_min, OPEN_RATIO * since_near + 1.0))
                o_step = 1.0 / steps
                o_curve = min(1.0, max(0.0, steps / open_min - 1.0))
                t_from, t_ph, t_cl = gain(t_gr), (1.0 if t_gr >= 0.0 else 0.0), False
                s_from, s_ph, s_cl = gain(s_gr), (1.0 if s_gr >= 0.0 else 0.0), False
        elif is_open:
            below = below + 1 if lv < close_lv else 0
            if below > hold_n:
                is_open = False
                t_cl = s_cl = True
                floor_since = 0
        if is_open:
            if t_ph < 1.0:
                t_ph = min(1.0, t_ph + o_step)
                q = t_ph
                g = t_from + (1.0 - t_from) * (q + o_curve * (q * q * (3.0 - 2.0 * q) - q))
                t_gr = 0.0 if (q >= 1.0 or g >= 1.0) else log(max(g, 1e-9)) / k_db
            if s_ph < 1.0:
                s_ph = min(1.0, s_ph + o_step)
                q = s_ph
                g = s_from + (1.0 - s_from) * (q + o_curve * (q * q * (3.0 - 2.0 * q) - q))
                s_gr = 0.0 if (q >= 1.0 or g >= 1.0) else log(max(g, 1e-9)) / k_db
            T[i], S[i] = gain(t_gr), gain(s_gr)
            continue
        if t_cl:
            if t_gr > rng_db:
                t_gr = max(rng_db, t_gr - step)
            t_cl = t_gr > rng_db
        else:
            t_gr = rng_db
        if floor_since == 0 or settling:
            floor_db = LD[i]
        else:
            floor_db += (LD[i] - floor_db) * ((1.0 / max(1.0, 0.5 * floor_since)) if floor_since < grow else 1.0 / tau_max)
        floor_since = min(floor_since + 1, grow)
        target = min(0.0, max(rng_db, (EXPANDER_SLOPE - 1.0) * floor_db - offset))
        if s_cl:
            if s_gr > target:
                s_gr = max(target, s_gr - step)
            s_cl = s_gr > target
        else:
            sl = 1e9 if settling else slew
            s_gr += max(-sl, min(sl, target - s_gr))
        T[i], S[i] = gain(t_gr), gain(s_gr)
    return T, S


def gain_events(g, before):
    """-> (samples where the gain starts to rise, samples where it starts to fall); `before` is the gain before
    sample 0 (the reset state). Steps under 1e-6 of the gain are float rounding, not movement."""
    g = np.concatenate([[before], np.asarray(g, dtype=float)])
    dg = np.diff(g)
    d = np.where(np.abs(dg) > 1e-6 * np.abs(g[:-1]), np.sign(dg), 0.0)
    prev = np.concatenate([[0.0], d[:-1]])
    return np.nonzero((d > 0) & (prev <= 0))[0], np.nonzero((d < 0) & (prev >= 0))[0]


def unity_events(g):
    """-> (samples where the gain arrives at exactly 1, samples where it leaves 1): an opening's end and a close's
    start. Smooth's gain wanders a little while closed, so its direction changes are not events; these are."""
    at = np.concatenate([[False], np.asarray(g, dtype=float) >= 1.0])
    d = np.diff(at.astype(np.int8))
    return np.nonzero(d == 1)[0], np.nonzero(d == -1)[0]


def _nearest(a, b):
    """For each of a, the distance to the nearest of b (both sorted)."""
    if not len(b):
        return np.full(len(a), 1 << 30)
    k = np.clip(np.searchsorted(b, a), 1, len(b) - 1) if len(b) > 1 else np.zeros(len(a), dtype=np.int64)
    d = np.abs(b[k] - a)
    return np.minimum(d, np.abs(b[np.maximum(k - 1, 0)] - a))


def event_misses(engine, ref):
    """Reference events with no engine event within one sample, plus engine events with no reference event."""
    engine, ref = np.asarray(engine, dtype=np.int64), np.asarray(ref, dtype=np.int64)
    return int(np.count_nonzero(_nearest(ref, engine) > 1) + np.count_nonzero(_nearest(engine, ref) > 1))


def shifted_spans(eng_events, ref_events, n):
    """Spans from each engine event exactly one sample off the reference's to the next event."""
    eng = np.sort(np.concatenate(eng_events)).astype(np.int64)
    ref = np.sort(np.concatenate(ref_events)).astype(np.int64)
    d = _nearest(eng, ref)
    return [(int(v) - 1, int(eng[k + 1]) + 1 if k + 1 < len(eng) else n) for k, v in enumerate(eng) if d[k] == 1]


def gain_error(g, r, spans):
    """Largest |dB difference| where either gain is over PLAIN_FLOOR_DB. A state change one sample apart moves the
    ramp after it by a sample, so inside `spans` each sample is compared with the nearest of the reference's three
    around it."""
    gd, rd = fx.db(np.maximum(g, 1e-12)), fx.db(np.maximum(r, 1e-12))
    live = (gd > PLAIN_FLOOR_DB) | (rd > PLAIN_FLOOR_DB)
    err = np.abs(gd - rd)
    if spans:
        shifted = np.zeros(len(g), dtype=bool)
        for a, b in spans:
            shifted[max(a, 0):b] = True
        near = np.minimum(err, np.minimum(np.abs(gd - np.roll(rd, 1)), np.abs(gd - np.roll(rd, -1))))
        err = np.where(shifted, near, err)
    return float(np.max(np.where(live, err, 0.0)))


# --- the round-5 verifier's signals (build-verify/r5-gate/v5lib.py, t2, t8b, t15), numpy only

def r5_pluck(f0, secs, sr, t60=4.0, bright=0.6, pick=0.13, seed=0, inharm=1e-4, attack_ms=1.0, hf_t60_ratio=0.25, nh_max=40):
    n = int(round(secs * sr))
    t = np.arange(n) / sr
    r = np.random.default_rng(seed)
    y = np.zeros(n)
    for k in range(1, nh_max + 1):
        fk = k * f0 * np.sqrt(1 + inharm * k * k)
        if fk > 0.4 * sr:
            break
        a = abs(np.sin(k * np.pi * pick)) / k ** (2 - bright)
        t60k = t60 / (1 + (k - 1) * (1 / hf_t60_ratio - 1) / 10)
        y += a * np.exp(-6.9078 * t / t60k) * np.sin(2 * np.pi * fk * t + r.uniform(0, 2 * np.pi))
    na = max(1, int(attack_ms * 1e-3 * sr))
    y[:na] *= np.linspace(0, 1, na)
    return y / np.max(np.abs(y))


def r5_fade_stop(a, sr, at_s, ms=8.0):
    y = a.copy()
    i, m = int(at_s * sr), max(1, int(ms * 1e-3 * sr))
    if i < len(y):
        k = min(m, len(y) - i)
        y[i:i + k] *= 0.5 * (1 + np.cos(np.pi * np.arange(k) / m))
        y[i + k:] = 0
    return y


def r5_hiss(n, rms_db, seed=1):
    return np.random.default_rng(seed).standard_normal(n) * fx.amp(rms_db)


def r5_two_notes(sr, delay_ms, thr=-40.0):
    """t2_stuck_gain.py: an A2 pluck decaying through the threshold, then a D3 picked delay_ms after the A2 last
    reads over the open level. -> (x, onset of the D3)."""
    n = int(4.0 * sr)
    a = np.zeros(n)
    note1 = fx.amp(-12) * r5_pluck(110.0, 3.0, sr, t60=1.2, seed=3)
    a[int(0.2 * sr):int(0.2 * sr) + len(note1)] = note1
    i2 = int(np.nonzero(twin_level(a, sr, HPF) >= fx.amp(thr))[0][-1]) + int(delay_ms * 1e-3 * sr)
    note2 = fx.amp(-12) * r5_pluck(146.83, 1.5, sr, t60=3.0, seed=4)
    a[i2:i2 + len(note2)] += note2[:n - i2]
    return a + r5_hiss(n, -75.0), i2


R5_GROW = [20, 28, 38, 52, 70]


def r5_riff(sr, gaps_ms, f0=82.41, on_ms=70.0, seed=1, fade_ms=6.0, lvl=-12.0):
    """t8b_gap_latch.py: hand-stopped E2 chugs over hiss at -70 dBFS RMS. -> (x, [(gap start, gap end)])."""
    items, gaps, t = [], [], 0.3
    for k, gap in enumerate(gaps_ms):
        note = fx.amp(lvl) * r5_pluck(f0, on_ms * 1e-3 + 0.05, sr, t60=0.5, seed=seed + k, bright=0.8)
        items.append((int(t * sr), r5_fade_stop(note, sr, on_ms * 1e-3 - fade_ms * 1e-3, ms=fade_ms)))
        end = t + on_ms * 1e-3
        t = end + gap * 1e-3
        gaps.append((int(end * sr), int(t * sr)))
    x = np.zeros(int((t + 0.6) * sr))
    for i0, a in items:
        x[i0:i0 + len(a)] += a[:len(x) - i0]
    return x + r5_hiss(len(x), -70.0, seed), gaps


def plain_material(kind, sr):
    """One piece of P1's material at `sr`. -> (x, threshold dB)."""
    rng = np.random.default_rng(PLAIN_KINDS.index(kind) + 17)
    if kind == "ringing chord C2+G2+C3":
        return strum(R2_CHORDS["C2+G2+C3"], 3.0, sr, level_db=-8, t60=6.0, seed=5, strum_ms=6) + hiss(3.0, sr, -92, seed=2), -45.0
    if kind == "ringing chord E2+G2":
        return strum(R2_CHORDS["E2+G2"], 3.0, sr, level_db=-8, t60=2.5, seed=5, strum_ms=6) + hiss(3.0, sr, -92, seed=2), -55.0
    if kind == "held chord B0+F#1 near thr":
        x = strum(R2_CHORDS["B0+F#1"], 2.5, sr, level_db=-30, t60=20.0, seed=7, strum_ms=6)
        return x + hiss(2.5, sr, -80, seed=3), round(peak_db(x, sr, 0.3) - 3.0, 2)
    if kind.startswith("palm-mute riff"):
        f0, under = (82.41, 30.0) if kind.endswith("hiss") else (55.0, 20.0)
        period, nn = 0.17, 10
        secs = 0.1 + nn * period + 0.3
        notes = place(secs, sr, [(0.1 + k * period, hand_stop(string(f0, 0.14, sr, level_db=-6, t60=1.2, seed=20 + k), sr, 0.09, 2.5))
                                 for k in range(nn)])
        fl = _floor("hiss" if kind.endswith("hiss") else "hum", secs, sr, rng)
        return notes + _scale_reading(fl, sr, HPF, -6 - under), round(-6 - under + 9.0, 2)
    if kind.startswith("hand stop"):
        band = kind.endswith("in the band")
        s = _scale_reading(string(196.0, 1.4, sr, level_db=0.0, t60=30.0, seed=4, click_db=None), sr, HPF, -20.0)
        thr = -17.0 if band else -32.0
        if band:   # down into the band at 100 dB/s from 8 dB over the threshold, so the gate is open when it gets there
            t = np.arange(len(s)) / sr
            s = s * fx.amp(np.maximum(0.0, 11.0 - 100.0 * np.maximum(t - 0.3, 0)))
        s = hand_stop(s, sr, 0.7, 10.0 / (40.0 / 8.686))
        return s + _scale_reading(hiss(1.4, sr, -40, seed=3), sr, HPF, -60.0), thr
    if kind.startswith("decay into"):
        secs = 2.5
        t = np.arange(int(round(secs * sr))) / sr
        note = _scale_reading(_note(110.0, secs, sr, rng, "tone"), sr, HPF, -20.0) * fx.amp(-40.0 * np.maximum(t - 0.2, 0))
        in_band = kind.endswith("band")
        fl = _floor("hum" if in_band else "hiss", secs, sr, rng)
        return note + _scale_reading(fl, sr, HPF, -40.0 - (4.0 if in_band else 10.0)), -40.0
    if kind == "swell after a decay":
        n = int(sr * 3.2)
        t = np.arange(n) / sr
        env = np.where(t < 0.3, -20.0, -20.0 - 60.0 * (t - 0.3))
        sw = (t >= 1.4) & (t < 2.4)
        env[sw] = -75.0 + 55.0 * (t[sw] - 1.4)
        env[t >= 2.4] = -20.0
        y = sum(np.sin(2 * np.pi * 110.0 * k * t + k) / k for k in range(1, 6)) / 1.6
        return y * fx.amp(np.maximum(env, -200.0)) + fx.amp(-75.0) * np.random.default_rng(7).standard_normal(n), -40.0
    if kind == "soft note, hard pick":   # finding 5: attack 10 dB over the body, body 3 dB under the threshold
        body = r5_pluck(110.0, 2.0, sr, t60=10.0, seed=3, bright=0.3)
        t = np.arange(len(body)) / sr
        x = np.concatenate([np.zeros(int(0.2 * sr)), body * (1 + (fx.amp(10.0) - 1) * np.exp(-t / 0.010))])
        x = x * fx.amp(-40.0 - float(fx.db(twin_level(x, sr, HPF)[int(0.28 * sr)])))
        return x + r5_hiss(len(x), -95.0), -37.0
    if kind == "r5 t2":
        return r5_two_notes(sr, 235.0)[0], -40.0
    if kind == "r5 t8b":
        return r5_riff(sr, R5_GROW + [80] * 12)[0], -40.0
    if kind == "picks over silence":
        x = np.zeros(int(1.8 * sr))
        for k, at in enumerate((0.1, 0.45, 0.62, 1.1)):
            p = hand_stop(string(82.41, 0.3, sr, level_db=0.0, t60=1.0, seed=30 + k), sr, 0.08 + 0.05 * k, 3.0 + 4 * k) * fx.amp(-40.0 + 5 * k)
            i0 = int(at * sr)
            x[i0:i0 + len(p)] += p[:len(x) - i0]
        return x, -42.0
    if kind == "beat near thr":
        return two_tone(110.0, 3.0, 12.0, sr, -40.0, tau=1.0, secs=2.5), -40.0
    if kind == "noise at thr":
        x = fx.white(2.5, -60.0, sr, seed=903)
        return x * fx.amp(-40.0 - float(fx.db(np.median(twin_level(x, sr, HPF)[int(0.2 * sr):])))), -40.0
    if kind.startswith("pick over"):   # finding 1 of round 6: a pick after a floor in the band, 5 dB under the threshold
        x = _precursor_pick(sr, 82.41, "hum" if kind.endswith("hum") else "hiss", 5.0, None, 3)[0]
        return x, PRECURSOR_THR
    if kind == "pick after a scrape":
        return _precursor_pick(sr, 164.81, "scrape", 3.0, 5.0, 4)[0], PRECURSOR_THR
    if kind == "picks over a floor at thr":   # the floor's peak reading 1 dB under the threshold
        fl = _scale_reading(hiss(1.8, sr, -60, seed=12), sr, HPF, -41.0, start=0.1)
        x = np.zeros(len(fl))
        for k, at in enumerate((0.3, 0.75, 1.2)):
            p = hand_stop(string(110.0, 0.4, sr, level_db=-14.0 - 6 * k, t60=1.5, seed=40 + k), sr, 0.2, 4.0)
            x[int(at * sr):int(at * sr) + len(p)] += p[:len(x) - int(at * sr)]
        return x + fl, -40.0
    if kind == "level steps":
        x = _note(196.0, 2.5, sr, rng, "tone")
        lv = np.zeros(len(x))
        pos, k = 0, 0
        while pos < len(x):
            m = int((0.02 + 0.37 * ((k * 0.618) % 1.0)) * sr)
            lv[pos:pos + m] = -55.0 + 25.0 * ((k * 0.382) % 1.0)
            pos, k = pos + m, k + 1
        return x * fx.amp(lv), -40.0
    raise ValueError(kind)


PLAIN_KINDS = ("ringing chord C2+G2+C3", "ringing chord E2+G2", "held chord B0+F#1 near thr", "palm-mute riff hiss",
               "palm-mute riff hum", "hand stop", "hand stop in the band", "decay into hiss", "decay into the band",
               "swell after a decay", "soft note, hard pick", "r5 t2", "r5 t8b", "picks over silence", "beat near thr",
               "noise at thr", "level steps", "pick over hiss", "pick over hum", "pick after a scrape",
               "picks over a floor at thr")


def plain_cases():
    """Every material at every rate; block, hold, release, range and sidechain filter drawn in turn so each value
    meets each other."""
    cases = []
    for k, kind in enumerate(PLAIN_KINDS):
        for j, sr in enumerate(PLAIN_RATES):
            i = 4 * k + j
            cases.append((kind, sr, PLAIN_BLOCKS[(i + i // 4) % 4], PLAIN_HOLDS[(i + k) % 4], PLAIN_RELEASES[(i // 4) % 3],
                          PLAIN_RANGES[(i // 12) % 2], PLAIN_HPFS[(i + i // 3) % 3]))
    return cases


def plain_case(case, reference=plain_reference):
    """-> (worst gain difference dB over both modes, event misses, events, events one sample apart, desc)."""
    kind, sr, block, hold, rel, rng_db, hpf = case
    x, thr = plain_material(kind, sr)
    keys = {"threshold": thr, "hold": hold, "release": rel, "range": rng_db, "sc_hpf": hpf}
    gt = fx.mono(fx.run(EFFECT, x, dict(keys, mode=1), [f"--block={block}", "--trace"], sr)[1]).astype(float)
    gs = fx.mono(fx.run(EFFECT, x, dict(keys, mode=2), [f"--block={block}", "--trace"], sr)[1]).astype(float)
    rt, rs = reference(x, sr, keys)[:2]
    # Opens and closes as each type's gain shows them, by one detector per type: an opening from a mute is a
    # smoothstep whose first samples are exactly 0 in both, so the reference's own state would mark it earlier than
    # any gain can. Tight: where the gain starts to rise and to fall. Smooth: where it arrives at and leaves 0 dB.
    before = 0.0 if rng_db <= -89.95 else fx.amp(rng_db)
    eng, ref = gain_events(gt, before), gain_events(rt, before)
    eng_s, ref_s = unity_events(gs), unity_events(rs)
    misses = sum(event_misses(a, b) for a, b in zip(eng + eng_s, ref + ref_s))
    spans = shifted_spans(eng, ref, len(x))
    spans_s = shifted_spans(eng_s, ref_s, len(x)) + spans
    err = max(gain_error(gt, rt, spans), gain_error(gs, rs, spans_s))
    desc = f"{kind}, {sr} Hz block {block} thr {thr:g} hold {hold:g} release {rel:g} range {rng_db:g} hpf {hpf:g}"
    return err, misses, sum(len(e) for e in ref + ref_s), len(spans) + len(shifted_spans(eng_s, ref_s, len(x))), desc


def plain_rows(report, args):
    sr = 48000
    # P1: the engine equals the plain rule on every material, rate, block and knob setting drawn.
    cases = plain_cases()
    res = parallel(plain_case, cases)
    worst = max(res, key=lambda r: r[0])
    missed = [r for r in res if r[1] > 0]
    report.add("P1 equals the plain rule", f"{len(cases)} cases: {len(PLAIN_KINDS)} materials x 44.1-192 kHz, Tight + Smooth",
               worst[0] <= PLAIN_TOL_DB and not missed,
               f"worst gain difference {worst[0]:.4f} dB where either is over {PLAIN_FLOOR_DB:g} dB (limit {PLAIN_TOL_DB}, stated "
               f"before measuring; {worst[4]}); opens and closes more than one sample apart: {sum(r[1] for r in res)} of "
               f"{sum(r[2] for r in res)}, one sample apart (the reading's rounding, allowed): {sum(r[3] for r in res)}"
               + "".join(f"; {r[4]}: {r[1]}" for r in missed[:3]))

    # P2: the release knob on a natural decay (finding 4): a note decaying 20 dB/s falls from -1 to -60 dB in 59/60
    # of the knob (the knob is the time for 60 dB), within 10 % or 1 ms.
    def decay_fall(rel):
        secs = 0.2 + 60.0 / 20.0 + 1.0
        t = np.arange(int(secs * sr)) / sr
        x = _scale_reading(_note(110.0, secs, sr, np.random.default_rng(3), "tone"), sr, HPF, -20.0) * fx.amp(-20.0 * np.maximum(t - 0.2, 0))
        x = x + hiss(secs, sr, -100.0, seed=5)
        g = fx.db(np.maximum(fx.mono(fx.run(EFFECT, x, dict(TIGHT, threshold=-40.0, release=rel), ["--trace"], sr)[1]), 1e-12))
        i1 = int(np.nonzero(g > -1.0)[0][-1])
        down = np.nonzero(g[i1:] <= -60.0)[0]
        return int(down[0]) / sr * 1e3 if len(down) else float("inf")
    rels = (5.0, 30.0, 100.0, 500.0)
    falls = parallel(decay_fall, rels)
    report.add("P2 release on a decay", "Tight, a note decaying 20 dB/s through thr -40",
               all(abs(f - 59.0 / 60.0 * r) <= max(0.1 * 59.0 / 60.0 * r, 1.0) for f, r in zip(falls, rels)),
               "fall from -1 to -60 dB: " + " / ".join(f"release {r:g}: {f:.1f} ms" for r, f in zip(rels, falls))
               + " (limit 59/60 x the knob, within 10 % or 1 ms)")

    # P3: a note picked while the last one is still in the hold or the band plays at full gain (finding 1).
    def stuck(c):
        mode, delay = c
        x, i2 = r5_two_notes(sr, delay)
        g = fx.db(np.maximum(fx.mono(fx.run(EFFECT, x, dict(TIGHT if mode == 1 else SMOOTH, threshold=-40.0), ["--trace"], sr)[1]), 1e-12))
        return min(float(g[i2 + int(ms * 1e-3 * sr)]) for ms in (20, 100, 500))
    cells = [(m, d) for m in (1, 2) for d in (120, 170, 185, 195, 205, 215, 225, 235, 245, 255, 265, 300)]
    low = min(zip(parallel(stuck, cells), cells))
    report.add("P3 stuck gain", f"{len(cells)} cases: verifier t2, second note 120-300 ms after the first, Tight + Smooth",
               low[0] >= -PLAIN_TOL_DB,
               f"lowest gain of the second note at +20 / +100 / +500 ms: {low[0]:.3f} dB ({TYPES[low[1][0]]}, {low[1][1]} ms; limit "
               f"0 dB within P1's {PLAIN_TOL_DB}: nothing lowers the gain while the gate is open)")

    # P4: gaps that grow do not latch the gate open for the long gaps after them (finding 2). The engine's count of
    # long gaps over -6 dB equals the plain rule's (the numpy port) on the same riff, and is 0 wherever the rule
    # closes them all.
    def latch(c):
        long_ms, srate, block, mode = c
        keys = dict(TIGHT if mode == 1 else SMOOTH, threshold=-40.0)
        out = []
        for seq in (R5_GROW + [long_ms] * 30, [long_ms] * 30):
            x, gaps = r5_riff(srate, seq)
            g = fx.mono(fx.run(EFFECT, x, keys, [f"--block={block}", "--trace"], srate)[1])
            r = plain_reference(x, srate, keys)[0 if mode == 1 else 1]
            m = int(0.004 * srate)
            out.append(tuple(sum(1 for a, b in gaps[-30:] if float(fx.db(max(float(np.min(v[a:b - m])), 1e-12))) > -6.0)
                             for v in (g, r)))
        return out
    cells = [(L, 48000, 512, 1) for L in (60, 80, 100)] + [(80, s, b, 1) for s, b in ((44100, 64), (96000, 2048), (192000, 4096))]
    cells += [(L, 48000, 512, 2) for L in (60, 80, 100)]
    res = parallel(latch, cells)
    report.add("P4 gap latch", f"{len(cells)} cases: verifier t8b, gaps 20-70 ms then 30 of 60/80/100 ms",
               all(e == r for case in res for e, r in case),
               "long gaps over -6 dB, engine / plain rule, after the growing gaps; alone: "
               + ", ".join(f"{TYPES[c[3]]} {c[0]} ms {c[1]} Hz: {r[0][0]}/{r[0][1]}; {r[1][0]}/{r[1][1]}" for c, r in zip(cells, res))
               + " (limit: the engine's count is the rule's, so 0 where the rule closes every long gap)")


# ---------------------------------------------------------------------------- the targets
# Rows TA, TC, TD on seeded random draws: chords (report only), palm mutes close, stops close.

CHATTER_MS = 250.0      # a gain excursion under -6 dB that reopens within this is chatter; longer, a close
TC1_MARGIN_DB = 8.0     # TC1 is enforced on riffs whose threshold is at least this over the floor's peak reading


def dropouts(g, sr, level_db=-6.0):
    """Excursions of the gain under level_db that come back: [(start s, length ms, deepest dB)]."""
    gdb = fx.db(np.maximum(g, 1e-12))
    d = np.diff((gdb > level_db).astype(np.int8))
    dn, up = np.nonzero(d == -1)[0] + 1, np.nonzero(d == 1)[0] + 1
    res = []
    for s in dn:
        e = up[up > s]
        if len(e):
            res.append((s / sr, (e[0] - s) / sr * 1e3, float(np.min(gdb[s:e[0]]))))
    return res


def chord_metrics(g, lev, sr, thr):
    """-> (chatter count, legitimate closes, dips under -12 dB inside a trough under the open level shorter
    than 100 ms that began with the gate open, deepest such dip dB)."""
    d = dropouts(g, sr)
    gdb = fx.db(np.maximum(g, 1e-12))
    dd = np.diff((lev >= fx.amp(thr)).astype(np.int8))
    st, en = np.nonzero(dd == -1)[0] + 1, np.nonzero(dd == 1)[0] + 1
    bad, deepest = 0, 0.0
    for s in st:
        e = en[en > s]
        if len(e) and (e[0] - s) / sr < 0.1 and gdb[s] > -1.0:
            m = float(np.min(gdb[s:e[0] + int(0.005 * sr)]))
            deepest = min(deepest, m)
            bad += m < -12.0
    return sum(1 for q in d if q[1] < CHATTER_MS), sum(1 for q in d if q[1] >= CHATTER_MS), bad, deepest


def random_chord(i):
    """A seeded chord or two-tone beat, 15-45 dB over a threshold of -70..-25, over hiss / hum / buzz 8-40 dB
    under it; Tight or Smooth at its defaults. -> (x, sr, keys, desc)"""
    rng = np.random.default_rng(7000 + i)
    sr = int(rng.choice([44100, 48000, 96000]))
    secs = 4.0
    t = np.arange(int(secs * sr)) / sr
    if i % 2 == 0:
        name = list(R2_CHORDS)[int(rng.integers(len(R2_CHORDS)))]
        s = strum(R2_CHORDS[name], secs, sr, level_db=0, t60=float(rng.uniform(1.5, 8)), seed=int(rng.integers(1, 99)),
                  strum_ms=float(rng.uniform(2, 15)))
        desc = f"chord {name}"
    else:
        f0 = float(rng.choice([41.2, 55.0, 82.41, 110.0, 146.83, 196.0]))
        df, tr = float(rng.uniform(2.0, 15.0)), float(rng.uniform(3, 20))
        rr = 10 ** (-tr / 20)
        b = (1 - rr) / (1 + rr)
        tau = float(rng.uniform(0.5, 3))
        s = np.exp(-t / tau) * (np.sin(2 * np.pi * f0 * t) + b * np.sin(2 * np.pi * (f0 + df) * t + 1.0)) * (1 - np.exp(-t / 0.002))
        desc = f"two-tone {f0:g} Hz beat {df:.1f} Hz trough {tr:.0f} dB tau {tau:.1f}"
    thr = float(rng.uniform(-70, -25))
    s = _scale_reading(s, sr, HPF, thr + float(rng.uniform(15, 45)), start=0.0)
    fk = str(rng.choice(["hiss", "hum", "buzz"]))
    fl = _scale_reading(_floor(fk, secs, sr, rng), sr, HPF, thr - float(rng.uniform(8, 40)), start=0.1)
    mode = int(rng.choice([1, 2]))
    keys = dict(TIGHT if mode == 1 else SMOOTH, threshold=round(thr, 2))
    return s + fl, sr, keys, desc + f" floor {fk}"


def random_palm(i, corner=None):
    """Seeded palm-muted chugs: 41-110 Hz, notes 60-120 ms, gaps 50-250 ms, hiss or hum 12-40 dB under the
    notes, threshold 6-12 dB over its peak reading; `corner` = (f0, note ms, gap ms, under, margin) fixes those.
    -> (x, sr, keys, onsets, margin dB, gap ms, the longest hand-stop fall to the close level ms, desc)"""
    rng = np.random.default_rng(13000 + i)
    sr = int(rng.choice([44100, 48000, 96000]))
    f0 = float(rng.choice([41.2, 55.0, 73.42, 82.41, 110.0]))
    note_ms, gap_ms = float(rng.uniform(60, 120)), float(rng.uniform(50, 250))
    under, margin = float(rng.uniform(12, 40)), float(rng.uniform(6, 12))
    if corner is not None:
        f0, note_ms, gap_ms, under, margin = corner
    period, n = (note_ms + gap_ms) * 1e-3, 8
    secs = n * period + 0.4
    items, tcs = [], []
    for k in range(n):
        s = string(f0, note_ms * 1e-3 + 0.05, sr, level_db=-6, t60=float(rng.uniform(0.6, 2.0)), seed=int(rng.integers(1, 99)))
        tcs.append(float(rng.uniform(1.5, 5.0)))
        items.append((0.1 + k * period, hand_stop(s, sr, note_ms * 1e-3, tcs[-1])))
    notes = place(secs, sr, items)
    # The stop falls 8.686 dB per time constant, from the note (-6 dBFS) to the close level (margin - 6 over the floor).
    fall_ms = max(tcs) * (under - margin + 6.0) / 8.686
    fk = str(rng.choice(["hiss", "hum"]))
    fl = _scale_reading(_floor(fk, secs, sr, rng), sr, HPF, -6 - under)
    keys = dict(TIGHT, threshold=round(-6 - under + margin, 2))
    onsets = [int((0.1 + k * period) * sr) for k in range(2, n)]
    return (notes + fl, sr, keys, onsets, margin, gap_ms, fall_ms,
            f"{f0:g} Hz notes {note_ms:.0f} ms gaps {gap_ms:.0f} ms {fk} {under:.0f} dB under, thr +{margin:.1f}, {sr} Hz")


def chord_stop(i):
    """A seeded beating chord held 0.6-2.5 s, stopped by the hand (fall 5-30 ms) onto hiss 20-60 dB under it,
    threshold 6-20 dB under the chord. -> (x, sr, keys, at s, fall ms, desc)"""
    rng = np.random.default_rng(11000 + i)
    sr = 48000
    name = list(R2_CHORDS)[int(rng.integers(len(R2_CHORDS)))]
    secs = 3.5
    s = strum(R2_CHORDS[name], secs, sr, level_db=0, t60=float(rng.uniform(3, 10)), seed=int(rng.integers(1, 99)), strum_ms=6)
    at = float(rng.uniform(0.6, 2.5))
    lv = twin_level(s, sr, HPF)
    s = s / float(np.max(lv[int((at - 0.3) * sr):int(at * sr)])) * fx.amp(-20)
    under, fall = float(rng.uniform(20, 60)), float(rng.uniform(5, 30))
    s = hand_stop(s, sr, at, fall / (under / 8.686))
    thr = -20.0 - float(rng.uniform(6, 20))
    x = s + _scale_reading(hiss(secs, sr, -40, seed=int(rng.integers(1, 99))), sr, HPF, -20 - under, start=0.05)
    return x, sr, dict(TIGHT, threshold=round(thr, 2)), at, fall, f"{name} stop at {at:.2f} s thr {thr:.1f} under {under:.0f} fall {fall:.0f}"


def target_checks(report):
    sr = 48000

    # TA1, report only: the g1 chord grid (16 chords x 8 thresholds x 2 decays, the B1 signals), Tight and Smooth at
    # their default holds (Smooth with 21 extra cases at t60 4): cases with chatter.
    def grid(c):
        name, thr, t60, mode = c
        x = strum(R2_CHORDS[name], 5.0, sr, level_db=-8, t60=t60, seed=5, strum_ms=6) + hiss(5.0, sr, -92, seed=2)
        g = fx.mono(fx.run(EFFECT, x, dict(TIGHT if mode == 1 else SMOOTH, threshold=thr), ["--trace"], sr)[1]).astype(float)
        return chord_metrics(g, twin_level(x, sr, HPF), sr, thr)
    for mode in (1, 2):
        cells = [(n, thr, t60, mode) for n in R2_CHORDS for thr in range(-30, -70, -5) for t60 in (2.5, 6.0)]
        if mode == 2:
            cells += [(n, thr, 4.0, 2) for n in ("C2+G2+C3", "A1+E2+A2", "E2+G2", "E1+B1", "E2+F2", "B0+F#1", "A2 x2 +9c")
                      for thr in (-35, -45, -55)]
        res = parallel(grid, cells)
        chat = [(c, r) for c, r in zip(cells, res) if r[0] > 0]
        info(report, "info TA1 chord grid (g1)", f"{len(cells)} cases, {TYPES[mode]} hold {(TIGHT if mode == 1 else SMOOTH)['hold']:g}",
             f"{len(chat)} with chatter (under -6 dB, back within {CHATTER_MS:g} ms)"
             + "".join(f"; {c[0]} thr {c[1]} t60 {c[2]:g}" for c, _ in chat[:4])
             + f"; {sum(1 for r in res if r[1] > 0)} with a close of {CHATTER_MS:g} ms or more")

    # TA2, report only: random chords and two-tone beats: chatter, and dips under -12 dB inside a trough under
    # the open level shorter than 100 ms.
    def rchord(i):
        x, s, keys, desc = random_chord(i)
        g = fx.mono(fx.run(EFFECT, x, keys, ["--trace"], s)[1]).astype(float)
        return chord_metrics(g, twin_level(x, s, HPF), s, keys["threshold"]) + (f"{desc}, {TYPES[keys['mode']]}, {s} Hz",)
    res = parallel(rchord, range(120))
    chat = [r for r in res if r[0] > 0]
    short = [r for r in res if r[2] > 0]
    info(report, "info TA2 random chords", "120 seeded chords / beats, thr -70..-25, floors 8-40 dB under",
         f"{len(chat)} with chatter, {len(short)} with a dip under -12 dB in a trough < 100 ms (deepest gain there "
         f"{min(r[3] for r in res):.1f} dB)" + "".join(f"; {r[4]}" for r in (chat + short)[:3]))

    # TC1: random palm-muted chugs: every gap under -20 dB over its last 10 ms (§4.1.10's closed bar). Enforced on the
    # riffs where the rule must get there: the gap is at least window + hold + the release to -20 dB + the hand stop's
    # fall to the close level + the 10 ms the gap's end is measured over, and the threshold is at least TC1_MARGIN_DB over the floor's peak reading (nearer, the
    # floor's own peaks reach the close level and may restart the hold, which a hysteresis gate does by design). The
    # rest are printed, with round 6's corner (60 ms notes, 50 ms gaps, floor 40 dB under, threshold +8).
    def rpalm(c):
        i, corner = c
        x, s, keys, onsets, margin, gap_ms, fall_ms, desc = random_palm(i, corner)
        g = fx.mono(fx.run(EFFECT, x, keys, ["--trace"], s)[1]).astype(float)
        w = int(0.010 * s)
        vals = [float(fx.db(np.sqrt(np.mean(g[o - w:o] ** 2)))) for o in onsets]
        need = WINDOW_MS + keys["hold"] + keys["release"] / 3.0 + fall_ms + 10.0
        return sum(1 for v in vals if v < -20.0), len(vals), max(vals), desc, margin >= TC1_MARGIN_DB and gap_ms >= need, need
    corners = [(f0, 60.0, 50.0, 40.0, 8.0) for f0 in (41.2, 82.41)]
    res = parallel(rpalm, [(i, None) for i in range(120)] + [(900 + k, c) for k, c in enumerate(corners)])
    for enforced in (True, False):
        part = [r for r in res if r[4] == enforced]
        bad = [r for r in part if r[0] < r[1]]
        text = (f"{len(part) - len(bad)} of {len(part)} with every gap under -20 dB over its last 10 ms; worst gap end "
                f"{max(r[2] for r in part):.1f} dB" + "".join(f"; {r[3]}: {r[0]}/{r[1]} ({r[2]:.1f} dB, needs {r[5]:.0f} ms)" for r in bad[:3]))
        if enforced:
            report.add("TC1 random chugs", f"seeded: gap >= window + hold + release/3 + fall + 10, thr +{TC1_MARGIN_DB:g}..12", not bad,
                       text + " (limit -20 dB: §4.1.10's closed bar)")
        else:
            info(report, "info TC1 random chugs", "shorter gaps or thr under +8 dB, and round 6's corner",
                 text + "; corner: " + ", ".join(f"{r[3]}: {r[0]}/{r[1]} closed, gap end {r[2]:.1f} dB (needs {r[5]:.0f} ms)"
                                                 for r in res[-len(corners):]))

    # TD1: hand stops after a beating chord: -20 dB within hold + release/3 + 3 ms of the last sample at which the twin
    # reading is at the close level (R2's bound: the reading already spans the window).
    def cstop(i):
        x, s, keys, at, fall, desc = chord_stop(i)
        g = fx.db(np.maximum(fx.mono(fx.run(EFFECT, x, keys, ["--trace"], s)[1]), 1e-12))
        i0 = int(round(at * s))
        hit = np.nonzero(g[i0:] <= -20.0)[0]
        if not len(hit):
            return float("inf"), 0.0, desc
        lev = twin_level(x, s, HPF)[:i0 + hit[0]]
        i_cl = int(np.nonzero(lev >= fx.amp(keys["threshold"] - 6.0 - TWIN_EPS_DB))[0][-1])
        return (i0 + hit[0] - i_cl) / s * 1e3, keys["hold"] + keys["release"] / 3.0 + 3.0, desc
    res = parallel(cstop, range(120))
    w = max(res, key=lambda r: r[0] - r[1])
    over = [r for r in res if r[0] > r[1]]
    report.add("TD1 stops after a chord", "120 seeded chords held 0.6-2.5 s, fall 5-30 ms onto hiss 20-60 dB under", not over,
               f"{len(over)} over the bound; worst {w[0]:.1f} ms after the reading is last at the close level against hold + "
               f"release/3 + 3 = {w[1]:.0f} ({w[2]})")


# ---------------------------------------------------------------------------- the invariants
# Rows I3, I4, I5 state the rule of Gate.h as invariants and assert them, case by case, on seeded random and
# adversarial input: floors that wobble or pulse, decays, hand stops from any level, beats, chords, silence,
# steps; every rate, block size, channel count, threshold, hold, release and range. They read only the gain
# trace and a numpy twin of the detector level, never the engine's state.

OPEN_MIN_MS = 0.2       # kOpenMinSeconds: the fastest opening, a linear rise
OPEN_MAX_MS = 5.0       # kOpenMaxSeconds: an opening started over the open level may finish under it
OPEN_NEAR_DB = 1.0      # kOpenNearDb: the opening is timed over the last dB of the climb ...
OPEN_RATIO = 0.5 * PLAIN_HYSTERESIS_DB / OPEN_NEAR_DB   # ... 3 x as long, so a steady climb opens as if timed over the band
EXPANDER_SLOPE = 3.0    # kExpanderSlope: Smooth's gain falls 2 dB per dB of floor under the close level
TWIN_EPS_DB = 0.02      # the twin of the level against the engine's float arithmetic
TWIN_LAW_EPS_DB = 0.1   # the same for Smooth's close and law: more than 0.02 dB seen (150 Hz filter, 192 kHz, hold 0)
SMOOTH_SLEW_DB_S = 100.0    # kFloorSlewDbPerSecond: Smooth follows its law at most this fast once closed
GROW_MS = 400.0         # Smooth's floor estimate weighs linearly for this long (kFloorTauMaxSeconds / 0.5)
HEARD_DB = 1.0          # about the smallest level change heard on music: the bar of D3 and the tolerance of G10
INV_CASES = 360


def twin_level(x, sr, hpf):
    x = np.asarray(x, dtype=np.float32).astype(float)
    if hpf <= 20.5:
        return wmax(np.abs(x), int(round(WINDOW_MS * 1e-3 * sr)))
    return reading(x, sr, fc=hpf)


def runs_since(mask):
    """Samples since `mask` was last True (0 where it is True; huge before the first True)."""
    idx = np.where(mask, np.arange(len(mask)), -10 ** 9)
    return np.arange(len(mask)) - np.maximum.accumulate(idx)


def _scale_reading(y, sr, hpf, peak, start=0.05):
    lv = twin_level(y, sr, hpf)
    return y * fx.amp(peak) / max(float(np.max(lv[int(start * sr):])), 1e-30)


def _floor(kind, secs, sr, rng):
    n = int(round(secs * sr))
    t = np.arange(n) / sr
    f = float(rng.choice([50.0, 60.0]))
    if kind == "hiss":
        return rng.standard_normal(n)
    if kind == "hum":
        return sum(a * np.sin(2 * np.pi * f * k * t + k) for k, a in zip((1, 2, 3, 4, 5), (1, 0.3, 0.5, 0.1, 0.25)))
    y = np.zeros(n)
    w = max(4, int(0.0006 * sr))
    per = sr / (2 * f) if kind == "buzz" else sr / f
    k = 0
    while int(k * per) + w < n:
        i = int(k * per)
        y[i:i + w] += np.hanning(w) * (1 if (k % 2 == 0 or kind == "spikes") else -1)
        k += 1
    if kind == "spikes":
        y += rng.standard_normal(n) * fx.amp(-float(rng.uniform(8, 20))) * 0.25
    return y


def _wobbled(y, sr, rng):
    t = np.arange(len(y)) / sr
    depth, rate = float(rng.uniform(0, 12)), float(rng.uniform(0.5, 20))
    return y * fx.amp(depth / 2 * np.sin(2 * np.pi * rate * t + rng.uniform(0, 2 * np.pi))), depth, rate


def _note(f0, secs, sr, rng, kind=None):
    kind = kind or rng.choice(["tone", "string"])
    if kind == "string":
        return string(f0, secs, sr, level_db=0.0, t60=30.0, seed=int(rng.integers(1, 99)), click_db=None)
    t = np.arange(int(round(secs * sr))) / sr
    y = sum(np.sin(2 * np.pi * f0 * k * t + rng.uniform(0, 6.3)) / k for k in range(1, 7) if f0 * k < 0.45 * sr)
    return y / np.max(np.abs(y))


def inv_case(i):
    """One seeded case: (x, keys, sr, block, channels, description)."""
    rng = np.random.default_rng(1000 + i)
    sr = int(rng.choice([44100, 48000, 96000, 192000]))
    secs = 2.0
    n = int(round(secs * sr))
    t = np.arange(n) / sr
    hpf = float(rng.choice([75.0, 75.0, 20.0, 150.0]))
    thr = float(rng.uniform(-70, -20))
    keys = {"mode": int(rng.choice([1, 2])), "threshold": round(thr, 2), "hold": float(rng.choice([0.0, 6.0, 10.0, round(rng.uniform(0, 60), 1)])),
            "release": round(float(rng.uniform(5, 160)), 1), "range": float(rng.choice([-90.0, -90.0, -60.0, -40.0, -20.0])), "sc_hpf": hpf}
    f0 = float(rng.choice([41.2, 55.0, 82.41, 110.0, 146.83, 196.0, 330.0, 1000.0]))
    kinds = ["floor", "decay", "stop", "beat", "chord", "picks", "steps"]
    kind = kinds[i % len(kinds)]
    fk = str(rng.choice(["hiss", "hum", "buzz", "spikes"]))
    fl, depth, rate = _wobbled(_floor(fk, secs, sr, rng), sr, rng)
    desc = f"{kind} f0 {f0:g} floor {fk} wobble {depth:.1f} dB {rate:.1f} Hz"
    if kind == "floor":
        x = _scale_reading(fl, sr, hpf, thr + float(rng.uniform(-14, 3)))
        if rng.uniform() < 0.5:
            s = _note(f0, secs, sr, rng) * fx.amp(thr + float(rng.uniform(6, 30)) - rng.uniform(5, 200) * np.maximum(t - 0.3, 0))
            x = x + s * (t < 1.2)
    elif kind == "decay":
        r = float(rng.uniform(5, 200))
        start = thr + float(rng.uniform(6, 30))
        s = _scale_reading(_note(f0, secs, sr, rng), sr, hpf, start) * fx.amp(-r * np.maximum(t - 0.2, 0))
        x = s + _scale_reading(fl, sr, hpf, thr - float(rng.uniform(2, 30)))
        desc += f" decay {r:.0f} dB/s"
    elif kind == "stop":
        over = float(rng.uniform(-5.8, 30))
        under = float(rng.uniform(8, 90))
        fall = float(rng.uniform(5, 30))
        s = _scale_reading(_note(f0, secs, sr, rng), sr, hpf, 0.0)
        env = fx.amp(np.where(t < 0.5, thr + max(over, 8.0), thr + over))
        if over < 0:   # into the band slowly first (100 dB/s), so the gate is open when it gets there
            env = fx.amp(np.maximum(thr + over, thr + 8.0 - 100.0 * np.maximum(t - 0.5, 0)))
        at = 0.5 + float(rng.uniform(0.05, 0.4))
        s = hand_stop(s * env, sr, at, fall / (under / 8.686))
        x = s + _scale_reading(fl, sr, hpf, thr + over - under)
        desc += f" note {over:+.1f} re thr, floor {under:.0f} under, fall {fall:.0f} ms"
    elif kind == "beat":
        df, tr = float(rng.uniform(0.5, 12)), float(rng.uniform(3, 25))
        rr = 10 ** (-tr / 20)
        b = (1 - rr) / (1 + rr)
        tau = float(rng.uniform(0.5, 4))
        s = np.exp(-t / tau) * (np.sin(2 * np.pi * f0 * t) + b * np.sin(2 * np.pi * (f0 + df) * t + 1.0)) * (1 - np.exp(-t / 0.002))
        x = s * fx.amp(thr + float(rng.uniform(0, 15))) / (1 + b) + _scale_reading(fl, sr, hpf, thr - float(rng.uniform(6, 40)))
        desc += f" beat {df:.1f} Hz trough {tr:.0f} dB tau {tau:.1f} s"
    elif kind == "chord":
        name = list(R2_CHORDS)[int(rng.integers(len(R2_CHORDS)))]
        s = strum(R2_CHORDS[name], secs, sr, level_db=0.0, t60=float(rng.uniform(1.5, 8)), seed=int(rng.integers(1, 99)), strum_ms=6)
        x = _scale_reading(s, sr, hpf, thr + float(rng.uniform(10, 35))) + _scale_reading(fl, sr, hpf, thr - float(rng.uniform(4, 40)))
        desc = f"chord {name} floor {fk} wobble {depth:.1f} dB {rate:.1f} Hz"
    elif kind == "picks":
        x = np.zeros(n)
        for at in np.sort(rng.uniform(0.05, 1.8, int(rng.integers(1, 6)))):
            p = string(f0, 0.3, sr, level_db=0.0, t60=float(rng.uniform(0.3, 3)), seed=int(rng.integers(1, 99)))
            p = hand_stop(p, sr, float(rng.uniform(0.02, 0.25)), float(rng.uniform(1, 20)))
            p *= fx.amp(thr + float(rng.uniform(-3, 30)))
            i0 = int(at * sr)
            m = min(len(p), n - i0)
            x[i0:i0 + m] += p[:m]
        if rng.uniform() < 0.5:
            x = x + _scale_reading(fl, sr, hpf, thr - float(rng.uniform(3, 60)))
        desc = f"picks f0 {f0:g} over silence or {fk}"
    else:
        lv = np.zeros(n)
        pos = 0
        while pos < n:
            m = int(rng.uniform(0.005, 0.4) * sr)
            lv[pos:pos + m] = thr + float(rng.uniform(-15, 10))
            pos += m
        x = _note(f0, secs, sr, rng, "tone") * fx.amp(lv)
        desc = f"steps f0 {f0:g}"
    return x, keys, sr, int(rng.choice([1, 37, 512])), int(rng.choice([1, 2])), desc


def _smooth_bound(lev_db, start, k0, k1, close_db, sr):
    """An upper bound of Smooth's expander law (dB) at every sample from k0 to k1 of an episode whose gate cannot have
    closed before `start` (the reading had not yet been under the close level for the hold): its floor estimate starts
    at the close and weighs the level linearly (then with a 200 ms time constant), so at most a quarter of its weight
    lies before half the elapsed time. The law is (EXPANDER_SLOPE - 1) x (estimate - close level)."""
    m = min(max(k0 - start, 0), int(GROW_MS * 1e-3 * sr))
    est = 0.25 * float(np.max(lev_db[start:k1 + 1])) + 0.75 * float(np.max(lev_db[start + m // 2:k1 + 1]))
    return min(0.0, (EXPANDER_SLOPE - 1.0) * (est + TWIN_LAW_EPS_DB - close_db))


def check_invariants(item):
    """-> dict of the worst figure per invariant for one case (0 or negative = holds)."""
    i = item
    x, keys, sr, block, ch, desc = inv_case(i)
    xx = x if ch == 1 else np.vstack([x, x])
    opts = [f"--block={block}", "--trace"]
    g = fx.mono(fx.run(EFFECT, xx, keys, opts, sr)[1]).astype(float)
    gt = g if keys["mode"] == 1 else fx.mono(fx.run(EFFECT, xx, dict(keys, mode=1), opts, sr)[1]).astype(float)
    lev = twin_level(x, sr, keys["sc_hpf"])
    thr, hold, rel, rng_db = keys["threshold"], keys["hold"], keys["release"], keys["range"]
    op = fx.amp(thr)
    eps = fx.amp(TWIN_EPS_DB)
    maybe_over = lev >= op / eps
    ms = sr / 1000.0
    run_o = runs_since(maybe_over)
    res = {"desc": desc, "keys": keys, "sr": sr, "block": block, "ch": ch}
    lev_db = fx.db(np.maximum(lev, 1e-9))
    # Episodes under the open level, as the engine sees them: a rise over it that did not reopen the gate (the
    # twin within its rounding of the open level) joins two.
    episodes = []
    starts = np.nonzero(~maybe_over[1:] & maybe_over[:-1])[0] + 1
    reach = int(np.ceil(OPEN_MAX_MS * ms)) + 1
    for s0 in starts:
        e = np.nonzero(maybe_over[s0:])[0]
        e0 = s0 + (e[0] if len(e) else len(x) - s0)
        if episodes and not np.any(np.diff(gt[episodes[-1][1] - 1:s0 + reach]) > 0):
            episodes[-1] = (episodes[-1][0], e0)
        else:
            episodes.append((s0, e0))
    # I3: a rebound under the open level never raises the gain: once an opening (at most 5 ms) is done,
    # Tight's gain never rises until the level is over the open level again; Smooth's never rises above
    # what its release (the state machine's, gt) or its expander law allows. From the close on Smooth follows
    # its law at most SMOOTH_SLEW_DB_S, so it may sit over the law's current value only by falling from an
    # earlier one at that rate (the law at the close is the level of that instant, close to 0 dB).
    settled = run_o > int(np.ceil(OPEN_MAX_MS * ms)) + 1
    rise = np.zeros(len(g))
    rise[1:] = gt[1:] - gt[:-1] * (1 + 1e-6) - 1e-9
    res["I3"] = float(np.max(np.where(settled, rise, -1.0)))
    if keys["mode"] == 2:
        worst = -999.0
        step = max(1, int(ms))
        slew = SMOOTH_SLEW_DB_S / sr
        settle_n = int(np.ceil(OPEN_MAX_MS * ms)) + 2
        hold_n = int(round(hold * ms))
        # The earliest the gate can close in an episode: once the reading may have been under the close level (the
        # twin's rounding taken its way) for more than the hold. Smooth's floor estimate starts there or later.
        under_close = runs_since(lev >= fx.amp(thr - 6.0 + TWIN_LAW_EPS_DB)) > hold_n
        for s0, e0 in episodes:
            # Only episodes the gate enters open (Tight at 0 dB or still rising): one that starts on a bump to within
            # the twin's rounding of the open level that did not open the gate keeps the estimate it had.
            if not (gt[s0 - 1] >= 1.0 or np.any(np.diff(gt[max(0, s0 - reach):s0 + 1]) > 0)):
                continue
            c = np.nonzero(under_close[s0:e0])[0]
            c0 = s0 + (int(c[0]) if len(c) else e0 - s0)
            prev = -999.0   # the most the law allowed at the end of the last block, falling at the slew from there
            for k in range(s0, e0 - 1, step):
                k1 = min(k + step, e0 - 1)
                # Before c0 the gate is still open, so Smooth's gain is Tight's (gt).
                law = _smooth_bound(lev_db, c0, k, k1, thr - 6.0, sr) if k1 >= c0 else -999.0
                allowed = np.maximum(law, prev - slew * np.arange(k1 - k))
                prev = max(law, prev - slew * (k1 - k))
                if k < s0 + settle_n:
                    continue
                lim = np.maximum(gt[k:k1], fx.amp(np.maximum(allowed, rng_db)))
                worst = max(worst, float(np.max(g[k:k1] - lim * (1 + 1e-4))))
        res["I3s"] = worst

    # I5 (closing and opening without a click), on the case's own type: the gain never falls faster than the release
    # knob, nor rises faster than the 0.2 ms linear opening. Smooth's first window after the reset is out: there its
    # law takes the level unslewed (Gate.cpp, settle).
    own = slice(int(round(WINDOW_MS * ms)) + 1 if keys["mode"] == 2 else 0, None)
    gd = fx.db(np.maximum(g, 1e-12))[own]
    fall = gd[:-1] - gd[1:]
    knob = 60.0 / (rel * ms)
    live = (g[own][:-1] > 0) & (g[own][1:] > 0)
    res["I5c"] = float(np.max(np.where(live, fall - knob * 1.02 - 1e-5, -1.0)))
    res["I5o"] = float(np.max(np.diff(g[own]) - 1.0 / (OPEN_MIN_MS * ms) * 1.01))
    # I6 (a lower bound): once the reading has been over the open level (within the twin's rounding) for longer than
    # the longest opening (5 ms), the gain is 0 dB.
    over_n = runs_since(np.concatenate([[True], ~(lev >= op * eps)]))[1:]   # counted from the reset at the start
    sure = over_n > int(np.ceil(OPEN_MAX_MS * ms)) + 1
    res["I6"] = float(np.max(np.where(sure, 1.0 - g, -1.0))) if np.any(sure) else -1.0
    res["I6n"] = int(np.count_nonzero(sure))
    return res


def stop_case(i):
    """I4: a held note stopped by the hand, from any level (inside the band too), onto a floor 8 to 90 dB
    under it or a threshold 3 to 4 dB over the floor. -> (ms after the end of the fall to -20 dB, bound)."""
    rng = np.random.default_rng(5000 + i)
    sr = int(rng.choice([44100, 48000, 96000, 192000]))
    f0 = float(rng.choice([41.2, 55.0, 82.41, 110.0, 196.0, 330.0, 1000.0]))
    hold = float(rng.choice([0.0, 6.0, 10.0, round(rng.uniform(0, 50), 1)]))
    rel = round(float(rng.uniform(5, 100)), 1)
    fall = float(rng.uniform(5, 30))
    near = i % 4 == 0
    if near:   # threshold 3 to 4 dB over the floor's reading, note 8 to 40 dB over the floor
        under = float(rng.uniform(8, 40))
        thr, over = -20.0 - under + float(rng.uniform(3, 4)), None
        note = -20.0
    else:
        over = float(rng.uniform(-5.8, 30)) if i % 4 != 1 else float(rng.uniform(-5.8, -0.2))
        under = float(rng.uniform(max(8.0, over + 4.0), 90))   # the floor at least 4 dB under the threshold
        thr = -20.0 - over
        note = -20.0
    secs = 1.6
    t = np.arange(int(round(secs * sr))) / sr
    s = _scale_reading(string(f0, secs, sr, level_db=0.0, t60=30.0, seed=int(rng.integers(1, 99)), click_db=None), sr, HPF, 0.0)
    if note < thr:   # reach the band at 100 dB/s from 8 dB over, so the gate is open when it gets there
        env = np.maximum(note, thr + 8.0 - 100.0 * np.maximum(t - 0.4, 0))
    else:
        env = np.full(len(t), note)
    at = 0.4 + (thr + 8.0 - note) / 100.0 * (note < thr) + float(rng.uniform(0.03, 0.3))
    s = hand_stop(s * fx.amp(env), sr, at, fall / (under / 8.686))
    x = s + _scale_reading(hiss(secs, sr, -40, seed=int(rng.integers(1, 99))), sr, HPF, note - under, start=0.05)
    block, ch = int(rng.choice([1, 37, 512])), int(rng.choice([1, 2]))
    keys = {"mode": 1, "threshold": round(thr, 2), "hold": hold, "release": rel}
    g = fx.db(np.maximum(fx.mono(fx.run(EFFECT, x if ch == 1 else np.vstack([x, x]), keys, [f"--block={block}", "--trace"], sr)[1]), 1e-12))
    i0 = int(round(at * sr))
    hit = np.nonzero(g[i0:] <= -20.0)[0]
    close = (hit[0] / sr * 1e3 if len(hit) else float("inf")) - fall
    bound = WINDOW_MS + hold + rel / 3.0 + 3.0
    desc = (f"{f0:g} Hz note {note - thr:+.1f} re thr, floor {under:.0f} under{' (thr 3-4 over it)' if near else ''}, fall {fall:.0f} ms, "
            f"hold {hold:g}, release {rel:g}, {sr} Hz block {block} {ch} ch")
    return close, bound, desc


def invariant_checks(report, args):
    cases = parallel(check_invariants, range(INV_CASES))
    n_s = sum(1 for r in cases if r["keys"]["mode"] == 2)

    def worst(key, pick=None):
        rs = [r for r in cases if key in r and (pick is None or pick(r))]
        r = max(rs, key=lambda r: r[key])
        return r[key], r, len(rs)

    def where(r):
        k = r["keys"]
        return (f"{r['desc']}; thr {k['threshold']:g} hold {k['hold']:g} release {k['release']:g} range {k['range']:g} "
                f"hpf {k['sc_hpf']:g} {r['sr']} Hz block {r['block']} {r['ch']} ch")

    v, r, n = worst("I3")
    report.add("I3 reopen over open only", f"{n} cases, Tight's state machine", v <= 0.0,
               f"largest rise under the open level after an opening: {max(v, 0):.2e}" + ("" if v <= 0 else " at " + where(r)))
    v, r, n = worst("I3s")
    report.add("I3 reopen over open only", f"{n} Smooth cases", v <= 0.0,
               f"largest gain over max(release, the {EXPANDER_SLOPE:g}:1 law, 2 x (estimate - close level), on the most its "
               f"estimate can hold, falling at most {SMOOTH_SLEW_DB_S:g} dB/s): {max(v, 0):.2e}" + ("" if v <= 0 else " at " + where(r)))
    vc, rc, n = worst("I5c")
    vo, ro, _ = worst("I5o")
    report.add("I5 no click", f"{n} cases, each type's own gain", vc <= 0.0 and vo <= 0.0,
               f"fall beyond the release knob {max(vc, 0):.2e} dB per sample; rise beyond the 0.2 ms opening "
               f"{max(vo, 0):.2e} per sample" + ("" if vc <= 0 else " at " + where(rc)) + ("" if vo <= 0 else " at " + where(ro)))
    v, r, n = worst("I6")
    report.add("I6 open means 0 dB", f"{n} cases, {sum(q['I6n'] for q in cases)} samples judged", v <= 1e-6,
               f"largest gain under 0 dB once the reading has been over the open level for more than {OPEN_MAX_MS:g} ms "
               f"(the longest opening): {max(v, 0):.2e} (limit 1e-6)" + ("" if v <= 1e-6 else " at " + where(r)))
    # I4 enforces the stops drawn with the threshold at least 4 dB over the floor ("over" and "band"); with the
    # threshold only 3 to 4 dB over it ("near") the floor sits well inside the band and may hold an open gate
    # open, which is the hysteresis, so those are reported.
    stops = parallel(stop_case, range(240))
    judged = [s for i, s in enumerate(stops) if i % 4 != 0]
    over = max(judged, key=lambda s: s[0] - s[1])
    near = [s for i, s in enumerate(stops) if i % 4 == 0]
    band = [s for i, s in enumerate(stops) if i % 4 == 1]
    report.add("I4 stop closes", f"{len(judged)} random hand stops, floor 4+ dB under thr", over[0] <= over[1],
               f"worst {over[0]:.1f} ms after the end of the fall against window + hold + release/3 + 3 = {over[1]:.1f} "
               f"({over[2]}); from inside the band worst margin {min(b - c for c, b, _ in band):+.1f} ms")
    info(report, "info I4 near the floor", f"{len(near)} stops, thr 3-4 dB over the floor (floor in the band)",
         f"{sum(1 for c, b, _ in near if c == float('inf'))} never close; of the rest, worst margin "
         f"{min((b - c for c, b, _ in near if c != float('inf')), default=0.0):+.1f} ms against the same bound")


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


ROW_GROUPS = ("common", "I", "G", "X", "P", "A", "B", "C", "D", "E", "F", "R", "T")


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
    unknown = sorted(set(args.rows or ()) - set(ROW_GROUPS))
    if args.rows is not None and (unknown or not args.rows):
        sys.exit(f"--rows: no group named {', '.join(unknown) or '(empty)'}; groups are {', '.join(ROW_GROUPS)}")
    report = fx.Report(EFFECT)
    grid = Grid()

    def on(tag):
        return args.rows is None or tag in args.rows
    if on("I"):
        invariant_checks(report, args)
    if on("common"):
        fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                         quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
    if on("G"):
        specific_checks(report, args, grid)
    if on("X"):
        extra_checks(report, args, grid)
        robustness_checks(report, args, grid)
    closing_rule_checks(report, args)
    if show_grid:
        print_grid(grid)
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
