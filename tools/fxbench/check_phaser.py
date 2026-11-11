#!/usr/bin/env python3
"""Acceptance for the phaser engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.6.6). Prints a PASS/FAIL table; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_phaser.py [--quick] [--types 1,2]

P2 imports the numeric reference docs/research/fx/phaser/prototype/phaser_ref.py; P3 reads
evidence/fit/spec_check.txt; P4 uses evidence/fit/trajectory.py + itrack.py against
evidence/fit/trajectories.tsv. P9-P12 (not in §4.6.6; added with the sample-rate, sync and
re-enable fixes): rate independence against each voicing's own 48 kHz render, song-position
jumps, the processor's off/reset/on sequence, non-finite and huge input. P13-P18 (round-2
fixes): pitch deviation of a held note against Guitar Rig 6 (evidence/fit/motion.tsv), waveform
null of a guitar-like take against Guitar Rig 6 (evidence/fit/gr6_takes/), song-position jumps at every
block size and rate, the phase lock under host jitter and tempo changes, the level of every model
and Color switch, and loudness against bypass (with the full-scale peak). The P13/P14 references
are rendered by evidence/fit/reftakes.py. --quick runs P1 at blocks 37/512, P2 at 48 kHz, P4 at
the noon rates only and P9 at 44.1/48/96 kHz. Temporary WAVs go to check-tmp/ next to the
tubamp_fxbench build.
"""
import importlib.util
import math
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxcheck as fx  # noqa: E402

EFFECT = "phaser"
TYPE_KEY = "model"
TYPES = {0: "Classic", 1: "Phase 90", 2: "Small Stone"}
# Float keys at their documented limits, for the finite-output test.
RANGES = {"rate": (0.01, 20.0), "depth": (0.0, 1.0), "mix": (0.0, 1.0)}
# Percent of real time per instance (§2.10); None = Classic as today (<= 1.2 x legacy).
BUDGETS = {0: None, 1: 0.4, 2: 0.5}
VARIANTS = {1: [{"color": 0}, {"color": 1}], 2: [{"color": 0}, {"color": 1}]}
NULL_KEYS = [{}, {"rate": 10, "depth": 1, "mix": 1}, {"rate": 0.05, "depth": 0.1, "mix": 0.5}]

STUDY = fx.REPO / "docs" / "research" / "fx" / "phaser"
FIT = STUDY / "evidence" / "fit"
VOICINGS = {(1, 0): "Phase 90 script", (1, 1): "Phase 90 block", (2, 0): "Small Stone off", (2, 1): "Small Stone on"}
WORKERS = 3


def use_build_tmp():
    """fxcheck's temporary WAVs go to the system temp dir by default; keep them in the build tree."""
    exe = Path(fx.binary()).resolve()
    build = next((p.parent for p in exe.parents if p.name == "tubamp_fxbench_artefacts"), exe.parent)
    tmp = build / "check-tmp"
    tmp.mkdir(exist_ok=True)
    tempfile.tempdir = str(tmp)


use_build_tmp()   # at import, so the worker processes use it too


def load_proto():
    spec = importlib.util.spec_from_file_location("phaser_ref", STUDY / "prototype" / "phaser_ref.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_trajectory():
    if str(FIT) not in sys.path:
        sys.path.insert(0, str(FIT))
    import trajectory  # noqa: E402  (imports itrack and track from the same directory)
    return trajectory


def f32(v):
    """The engine receives rates as float: give the reference the same value."""
    return float(np.float32(v))


def keys_for(model, color, **extra):
    return dict({"model": model, "color": color}, **extra)


# ---------------------------------------------------------------------------- P1

def p1_classic_null(report, quick):
    blocks = [37, 512] if quick else [1, 37, 512]
    for settings in [{}, {"color": 0}] + NULL_KEYS[1:]:
        ok, detail = fx.classic_null(EFFECT, settings, blocks=blocks)
        scope = "44.1/48/96 kHz, blocks " + "/".join(map(str, blocks)) + "".join(f" {k}={v}" for k, v in settings.items())
        report.add("P1 classic null", scope, ok, detail or "0 differing samples")


# ---------------------------------------------------------------------------- P2

def parity_input(sr):
    """4 s of pink noise at -18 dBFS plus an impulse train (0.5 every 0.1 s), different per channel."""
    n = 4 * sr
    x = np.vstack([fx.pink(4.0, -18.0, sr, seed=1), fx.pink(4.0, -18.0, sr, seed=2)])
    x[0, ::sr // 10] += 0.5
    x[1, sr // 20::sr // 10] += 0.5
    return x[:, :n].astype(np.float32).astype(np.float64)


def parity_job(args):
    model, color, rate, sr = args
    proto = load_proto()
    x = parity_input(sr)
    out = fx.invoke(EFFECT, x, keys_for(model, color, rate=rate), ["--block=37"], sr).out
    voicing = "phase90" if model == 1 else "smallstone"
    ref = np.vstack([proto.render(x[ch], sr, voicing, rate=f32(rate), block=bool(color), color=bool(color))
                     for ch in range(2)])
    d = out - ref
    rel = 20 * math.log10(max(np.sqrt(np.mean(d ** 2)), 1e-300) / np.sqrt(np.mean(ref ** 2)))
    return model, color, rate, sr, rel, float(np.max(np.abs(d)))


def p2_parity(report, quick):
    rates = {1: [0.143, 1.071, 3.72, 9.76], 2: [0.1585, 0.316, 1.778]}
    srs = [48000] if quick else [44100, 48000, 88200, 96000, 192000]
    jobs = [(m, c, r, sr) for m in (1, 2) for c in (0, 1) for r in rates[m] for sr in srs]
    with ProcessPoolExecutor(WORKERS) as pool:
        res = list(pool.map(parity_job, jobs))
    for (m, c), name in VOICINGS.items():
        rows = [r for r in res if r[0] == m and r[1] == c]
        worst_rel = max(r[4] for r in rows)
        worst_abs = max(r[5] for r in rows)
        report.add("P2 parity", f"{name}, {len(rows)} rate x fs", worst_rel <= -80.0 and worst_abs <= 1e-4,
                   f"worst rms {worst_rel:.1f} dB, max |diff| {worst_abs:.2e} (limits -80 dB, 1e-4)")


# ---------------------------------------------------------------------------- P3

SPEC_NAMES = {"Phase 90 script": (1, 0), "Phase 90 block": (1, 1), "Small Stone Color off": (2, 0),
              "Small Stone Color on": (2, 1)}


def read_spec_check():
    rows = []
    for line in (FIT / "spec_check.txt").read_text().splitlines():
        if line.startswith("#") or line.startswith("voicing\t"):
            continue
        c = line.split("\t")
        rows.append(dict(name=c[0], fs=int(c[1]), end=c[2], fc=float(c[3]), n1=c[4], n1db=c[5], n2=c[7], n2db=c[8],
                         peak=c[11], dc=c[13]))
    return rows


def analyse_response(h, fs):
    """spec_check.py's analyse() on a measured impulse response: notches = local minima of |H|
    on a 0.1 % log grid (20 Hz to min(20 kHz, 0.49 fs)) with 3 dB prominence, refined by golden
    section on the exact DTFT; highest |H| on the grid; |H| at 1 Hz."""
    n = np.arange(len(h))
    nfft = 1 << int(math.ceil(math.log2(8 * len(h))))
    spec = np.abs(np.fft.rfft(h, nfft))

    def exact(f):
        return abs(np.dot(h, np.exp(-2j * np.pi * f * n / fs)))

    f = 20.0 * 1.001 ** np.arange(int(math.log(min(20000.0, 0.49 * fs) / 20.0) / math.log(1.001)))
    db = 20 * np.log10(np.maximum(spec[np.rint(f * nfft / fs).astype(int)], 1e-300))
    notches = []
    for i in range(1, len(f) - 1):
        if not (db[i] <= db[i - 1] and db[i] < db[i + 1]):
            continue
        j = i - 1
        while j > 0 and db[j - 1] >= db[j]:
            j -= 1
        k = i + 1
        while k < len(f) - 1 and db[k + 1] >= db[k]:
            k += 1
        if min(db[j], db[k]) - db[i] < 3.0:
            continue
        a, b = math.log(f[i - 1]), math.log(f[i + 1])
        gr = (math.sqrt(5) - 1) / 2
        c, d = b - gr * (b - a), a + gr * (b - a)
        for _ in range(60):
            if exact(math.exp(c)) < exact(math.exp(d)):
                b, d = d, c
                c = b - gr * (b - a)
            else:
                a, c = c, d
                d = a + gr * (b - a)
        x = 0.5 * (a + b)
        notches.append((math.exp(x), 20 * math.log10(max(exact(math.exp(x)), 1e-300))))
    k = int(np.argmax(db))
    peak = max(exact(f[j]) for j in range(max(0, k - 2), min(len(f), k + 3)))
    return notches, 20 * math.log10(peak), 20 * math.log10(exact(1.0))


def frozen_job(row):
    model, color = SPEC_NAMES[row["name"]]
    fs = row["fs"]
    x = np.zeros(int(1.5 * fs))
    x[0] = 1.0
    h = fx.invoke(EFFECT, x, keys_for(model, color, freeze_u=-1 if row["end"] == "lo" else 1), ["--block=512"], fs).out[0]
    notches, peak, dc = analyse_response(h, fs)
    if notches and notches[0][0] / row["fc"] > 1.5:
        notches = [(float("nan"), float("nan"))] + notches
    tail = float(np.sum(h[-len(h) // 8:] ** 2) / np.sum(h ** 2))
    return row, notches, peak, dc, tail


def compare_frozen(spec, got, kind):
    """-> (ok, error or None, text). kind "f": frequency (+-0.1 %), "l": level (+-0.05 dB)."""
    if spec == "nan":
        return math.isnan(got), None, "none" if math.isnan(got) else f"{got:.2f} (want none)"
    if spec.startswith("<"):
        return got < float(spec[1:]), None, f"{got:.1f} dB (want {spec})"
    if kind == "f":
        err = 100 * (got / float(spec) - 1)
        return abs(err) <= 0.1, err, f"{err:+.3f} %"
    err = got - float(spec)
    return abs(err) <= 0.05, err, f"{err:+.3f} dB"


def p3_frozen(report):
    with ProcessPoolExecutor(WORKERS) as pool:
        res = list(pool.map(frozen_job, read_spec_check()))
    summary = {}
    for row, notches, peak, dc, tail in res:
        nan = (float("nan"), float("nan"))
        n1 = notches[0] if notches else nan
        n2 = notches[1] if len(notches) > 1 else nan
        checks = [(row["n1"], n1[0], "f"), (row["n1db"], n1[1], "l"), (row["n2"], n2[0], "f"),
                  (row["n2db"], n2[1], "l"), (row["peak"], peak, "l"), (row["dc"], dc, "l")]
        results = [compare_frozen(*c) for c in checks]
        s = summary.setdefault(row["name"], dict(ok=True, f=0.0, l=0.0, bad=[]))
        ok = all(r[0] for r in results) and tail < 1e-12
        s["ok"] &= ok
        for (_, _, kind), (_, err, _) in zip(checks, results):
            if err is not None:
                s[kind] = max(s[kind], abs(err))
        if not ok:
            s["bad"].append(f"{row['fs']} {row['end']}: " + ", ".join(r[2] for r in results) + f", tail {tail:.1e}")
    rates = "/".join(f"{r / 1000:g}" for r in sorted({row["fs"] for row, *_ in res}))
    for name, s in summary.items():
        detail = f"worst frequency {s['f']:.3f} %, worst level {s['l']:.3f} dB, {rates} kHz x lo/hi"
        report.add("P3 frozen fc", name, s["ok"], detail + ("; " + "; ".join(s["bad"]) if s["bad"] else ""))


# ---------------------------------------------------------------------------- P4 + P5 (audio)

def trajectory_job(tag):
    TJ = load_trajectory()
    T = TJ.T
    comp, ov, secs, P, M, lo, gain, kw = TJ.TAKES[tag]
    ref = TJ.read_tsv(str(FIT / "trajectories.tsv"))[tag]
    rate = 1.0 / ref["period"]
    model, color = (1, 1) if kw["voicing"] == "phase90" else (2, int(kw.get("color", False)))
    x, pos = T.probe(int(secs * TJ.FS), P, M, TJ.AMP)
    ys = []
    for j in range(0, M, 2):
        out = fx.invoke(EFFECT, x[j:j + 2], keys_for(model, color, rate=rate), ["--block=512"], TJ.FS).out
        ys += [out[0], out[1]]
    t, n1, per, _ = TJ.track_take(ys, pos, P, lo, 0.0)
    _, f1, _ = TJ.fold(t, n1, ref["period"])
    rms, worst, _, _ = TJ.traj_error(ref["f1"], f1)
    return tag, rms, worst, per, 1.0 / f32(rate)


def p4_trajectory(report, quick):
    TJ = load_trajectory()
    tags = list(TJ.read_tsv(str(FIT / "trajectories.tsv")))
    if quick:
        tags = [t for t in tags if t.endswith("p050") or t.endswith("r050")]
    with ProcessPoolExecutor(WORKERS) as pool:
        res = list(pool.map(trajectory_job, tags))
    groups = {"p9_block": ("Phase 90 block", 0.8, 1.4), "ss_off": ("Small Stone off", 2.0, None),
              "ss_on": ("Small Stone on", 1.2, None)}
    for prefix, (name, rms_max, peak_max) in groups.items():
        rows = [r for r in res if r[0].startswith(prefix)]
        if not rows:
            continue
        rms = max(r[1] for r in rows)
        worst = max(abs(r[2]) for r in rows)
        ok = rms <= rms_max and (peak_max is None or worst <= peak_max)
        per_rate = ", ".join(f"{r[0].rsplit('_', 1)[1]} {r[1]:.2f}/{r[2]:+.2f}" for r in rows)
        limit = f"<= {rms_max} % rms" + (f", <= {peak_max} % max" if peak_max else "")
        report.add("P4 trajectory vs GR6", f"{name}, {len(rows)} rates", ok,
                   f"worst {rms:.2f} % rms, {worst:.2f} % max ({limit}); rms/max per take: {per_rate}")
    worst_period = max(abs(100 * (r[3] / r[4] - 1)) for r in res)
    report.add("P5 rate (audio)", f"tracked from the P4 renders, {len(res)} takes", worst_period < 0.01,
               f"worst period error {worst_period:.4f} % (limit 0.01 %)")


# ---------------------------------------------------------------------------- P5

def unwrap_phase(p):
    d = np.diff(p)
    return np.concatenate([[p[0]], p[0] + np.cumsum(d + (d < -0.5))])


def p5_rate(report):
    worst, where = 0.0, ""
    for model in (1, 2):
        for rate in (0.01, 0.143, 1.071, 3.72, 9.76, 20.0):
            r = fx.invoke(EFFECT, "gen:silence", keys_for(model, 1, rate=rate),
                          ["--block=1", "--channels=1", "--seconds=4", "--trace"], 48000)
            ph = unwrap_phase(r.trace[0].astype(np.float64))
            slope = np.polyfit(np.arange(len(ph)), ph, 1)[0]
            err = 100 * abs((1.0 / (slope * 48000)) * f32(rate) - 1)
            if err >= worst:
                worst, where = err, f"model {model} at {rate} Hz"
    report.add("P5 rate (LFO)", "Phase 90, Small Stone; 0.01-20 Hz", worst < 0.01,
               f"worst period error {worst:.5f} % ({where}; limit 0.01 %)")

    # CH13: 120 BPM, 1/4 note = one cycle per beat (2 Hz); the song starts at beat 0.3, so
    # sync_phase = frac(0.3 + 2 t) and the sweep bottom (phase 0) belongs on t = (k - 0.3) / 2.
    sr, secs, ppq0 = 48000, 4.0, 0.3
    beats = [(k - ppq0) / 2.0 for k in range(1, int(2 * secs) + 2)]
    tokens = ["rate=2.0", f"sync_phase@0..{beats[0]}={ppq0}..1.0"]
    tokens += [f"sync_phase@{t0}..{t0 + 0.5}=0..1" for t0 in beats]
    for model in (1, 2):
        for bs in (64, 512):
            r = fx.invoke(EFFECT, "gen:silence", tokens + [f"model={model}", "color=1"],
                          [f"--block={bs}", "--channels=1", f"--seconds={secs}", "--trace"], sr)
            ends = np.arange(bs - 1, int(secs * sr), bs)
            ph = r.trace[0][ends].astype(np.float64)
            want = (ppq0 + 2.0 * ends / sr) % 1.0
            dt = np.max(np.abs(((ph - want + 0.5) % 1.0 - 0.5) / 2.0))
            period = 1.0 / (np.polyfit(ends / sr, unwrap_phase(ph), 1)[0])
            ok = abs(period / 0.5 - 1) <= 1e-3 and dt <= 1e-3
            report.add("P5 sync (CH13)", f"{TYPES[model]}, 120 BPM 1/4, block {bs}", ok,
                       f"period {period:.6f} s (0.5 +- 0.1 %), phase-0 timing error {1e3 * dt:.4f} ms (<= 1 ms)")


# ---------------------------------------------------------------------------- P6

def p6_stability(report):
    for model, name in ((1, "Phase 90 block"), (2, "Small Stone on")):
        worst, ok = 0.0, True
        for sr in (22050, 48000, 192000):
            rng = np.random.default_rng(5)
            x = rng.uniform(-1.0, 1.0, (2, 30 * sr))
            out = fx.invoke(EFFECT, x, keys_for(model, 1, rate=10.0), ["--block=512"], sr).out
            finite = bool(np.all(np.isfinite(out)))
            peak = float(np.max(np.abs(out))) if finite else float("inf")
            worst = max(worst, peak)
            ok &= finite and peak < 4.0
        report.add("P6 stability", f"{name}, 10 Hz, 30 s full-scale noise", ok,
                   f"finite, max |out| {worst:.3f} (< 4) at 22.05/48/192 kHz")


# ---------------------------------------------------------------------------- P7

def chord(sr, secs):
    """A 3:4:5 major triad on a fundamental of one period per n = round(sr / 1000) samples
    (about 3, 4, 5 kHz, two harmonics each), right channel 3 ms later. Every partial repeats
    every n samples, so the steady chord's level over any n-sample (~1 ms) window is the same
    and adjacent windows show a level step at full size, which a lower chord (whose 1 ms
    windows hold different parts of its waveform) cannot. -> (signal, n)."""
    n = int(round(sr / 1000.0))
    f1 = sr / n
    t = np.arange(int(secs * sr)) / sr
    sig = np.zeros((2, len(t)))
    for ch, lag in ((0, 0.0), (1, 0.003)):
        for k in (3, 4, 5):
            for h in (1, 2):
                sig[ch] += np.sin(2 * np.pi * k * f1 * h * (t - lag)) / h
    return (sig * fx.amp(-12.0) / np.max(np.abs(sig))).astype(np.float32).astype(np.float64), n


def env_db(y, n):
    """Level in dB over every window of n samples (window i starts at sample i)."""
    c = np.concatenate([[0.0], np.cumsum(y.astype(np.float64) ** 2)])
    return 10 * np.log10(np.maximum((c[n:] - c[:-n]) / n, 1e-20))


def largest_step(y, lo, hi, n):
    """Largest level change between an n-sample window and the next, over windows starting in [lo, hi)."""
    return max(float(np.max(np.abs(e[lo + n:hi + n] - e[lo:hi]))) for e in (env_db(y[0], n), env_db(y[1], n)))


def switch_case(x, n, sr, bs, before, events, after, t_event=1.0):
    """-> (step dB, unswitched step dB, fade ms, unheard ms) or None if nothing switched.
    The incoming model starts from reset at the first block at or after t_event and runs unheard
    for a while; the fade starts at the first sample that differs from the unswitched render and
    ends where the output joins a render of the incoming model started fresh at that block."""
    out = fx.invoke(EFFECT, x, fx._tokens(before) + events, [f"--block={bs}"], sr).out
    a = fx.invoke(EFFECT, x, before, [f"--block={bs}"], sr).out
    diff = np.nonzero(np.any(out != a, axis=0))[0]
    if len(diff) == 0:
        return None
    t0 = int(diff[0])
    t_s = int(math.ceil(t_event * sr / bs) * bs)
    w = fx.invoke(EFFECT, x[:, t_s:], after, [f"--block={bs}"], sr).out
    late = np.nonzero(np.any(np.abs(out[:, t_s:] - w) > 1e-6, axis=0))[0]
    t_end = t_s + (int(late[-1]) + 1 if len(late) else 0)
    lo, hi = t0 - 5 * n, t_end + 5 * n
    ms = lambda k: 1e3 * k / sr  # noqa: E731
    return largest_step(out, lo, hi, n), largest_step(a, lo, hi, n), ms(t_end - t0), ms(t0 - t_s)


def p7_switch(report):
    classic = {"rate": 1.0, "depth": 0.5, "mix": 0.5}
    cases = [("Phase 90 script -> block", {"model": 1, "color": 0}, ["color@1.0=1"], {"model": 1, "color": 1}),
             ("Phase 90 block -> Small Stone on", {"model": 1, "color": 1}, ["model@1.0=2"], {"model": 2, "color": 1}),
             ("Small Stone off -> on", {"model": 2, "color": 0}, ["color@1.0=1"], {"model": 2, "color": 1}),
             ("Small Stone on -> Classic", dict(classic, model=2, color=1), ["model@1.0=0"], dict(classic, model=0, color=1)),
             ("Classic -> Phase 90 block", dict(classic, model=0, color=1), ["model@1.0=1"], dict(classic, model=1, color=1))]
    settings = [(48000, 64), (48000, 512), (44100, 512), (44100, 37), (96000, 384)]
    for name, before, events, after in cases:
        ok, worst_step, worst_base, fades, detail = True, 0.0, 0.0, [], ""
        for sr, bs in settings:
            x, n = chord(sr, 2.0)
            r = switch_case(x, n, sr, bs, before, events, after)
            if r is None:
                ok, detail = False, f"{sr} Hz block {bs}: no switch happened"
                continue
            step, base, fade, unheard = r
            worst_step, worst_base = max(worst_step, step), max(worst_base, base)
            fades.append(fade)
            if step > 3.0 or abs(fade - 40.0) > 2.0:
                ok = False
                detail += f" FAIL at {sr} Hz block {bs}: step {step:.2f} dB, fade {fade:.2f} ms;"
        report.add("P7 switch", name, ok,
                   f"largest 1 ms level step {worst_step:.2f} dB (<= 3; unswitched {worst_base:.2f}), fade "
                   f"{min(fades):.2f}-{max(fades):.2f} ms (40 +- 2), after {unheard:.1f} ms unheard, at 48k/64, 48k/512, "
                   "44.1k/512, 44.1k/37, 96k/384." + detail)
    sr, bs = 48000, 512
    x, _ = chord(sr, 2.0)
    out = fx.invoke(EFFECT, x, fx._tokens(dict(classic, model=0, color=1)) + ["color@1.0=0"], [f"--block={bs}"], sr).out
    ref = fx.invoke(EFFECT, x, dict(classic, model=0, color=1), [f"--block={bs}"], sr).out
    report.add("P7 switch", "Classic color toggle (ignored)", np.array_equal(out, ref),
               "bit-identical to no toggle" if np.array_equal(out, ref) else "output changed")


# ---------------------------------------------------------------------------- P8

def p8_independence(report):
    for (m, c), name in VOICINGS.items():
        base = fx.invoke(EFFECT, "gen:music", keys_for(m, c), ["--block=512"], 48000).out
        ok, bad = True, ""
        variants = [{"depth": 0, "mix": 0}, {"depth": 1, "mix": 1}, {"depth": 0.13, "mix": 0.91},
                    ["depth@0..6=0..1", "mix@0..6=1..0"]]
        for v in variants:
            tokens = fx._tokens(keys_for(m, c)) + (fx._tokens(v) if isinstance(v, dict) else v)
            out = fx.invoke(EFFECT, "gen:music", tokens, ["--block=512"], 48000).out
            if not np.array_equal(out, base):
                ok, bad = False, f"differs at {v}"
        report.add("P8 depth/mix ignored", name, ok, bad or "bit-identical for 4 depth/mix settings incl. ramps")


# ---------------------------------------------------------------------------- P9 (sample rate)

RATE_SRS = [44100, 48000, 88200, 96000, 192000]
FROZEN_W = (-1.0, -0.5, 0.0, 0.5, 1.0)
NOON = {1: 1.071, 2: 0.3162}


def frozen_ir(model, color, w, sr):
    x = np.zeros(int(1.5 * sr))
    x[0] = 1.0
    return fx.invoke(EFFECT, x, keys_for(model, color, freeze_u=w), ["--block=512"], sr).out[0]


def response_db(h, sr, f):
    n = np.arange(len(h))
    return np.array([20 * math.log10(max(abs(np.dot(h, np.exp(-2j * np.pi * fk * n / sr))), 1e-300)) for fk in f])


def rate_frozen_job(args):
    model, color, w, sr = args
    h = frozen_ir(model, color, w, sr)
    notches = [n for n in analyse_response(h, sr)[0] if n[0] <= 10000.0]
    f = 20.0 * 1.01 ** np.arange(int(math.log(10000.0 / 20.0) / math.log(1.01)) + 1)
    return args, notches, response_db(h, sr, f)


def rate_trajectory_job(args):
    """Notch-1 trajectory at the noon rate tracked like P4 (itrack), the probe's period scaled with
    the sample rate; -> (args, bin centres, f1 per bin)."""
    model, color, sr = args
    TJ = load_trajectory()
    T = TJ.T
    tag = {1: "p9_block_p050", 2: "ss_on_r050" if color else "ss_off_r050"}[model]
    _, _, secs, P, M, lo, _, _ = TJ.TAKES[tag]
    ps = int(round(P * sr / TJ.FS))
    x, pos = T.probe(int(secs * sr), ps, M, TJ.AMP, start=int(0.1 * sr))
    ys = []
    for j in range(0, M, 2):
        out = fx.invoke(EFFECT, x[j:j + 2], keys_for(model, color, rate=NOON[model]), ["--block=512"], sr).out
        ys += [out[0], out[1]]
    ts, f, H, _, segs = T.snapshots(ys, pos, ps, TJ.AMP, fs=sr)
    tr = T.track(ts, f, H, segs, TJ.AMP, lo, 12000.0, fs=sr)
    r = T.summarise(tr, lo, 12000.0)
    _, f1, _ = TJ.fold(ts, r["n1"], 1.0 / f32(NOON[model]))
    return args, f1


def p9_rate_independence(report, quick):
    """A voicing sounds the same at every sample rate: its notch-1 trajectory, its frozen notches and
    its frozen response against its own 48 kHz render."""
    srs = [44100, 48000, 96000] if quick else RATE_SRS
    TJ = load_trajectory()
    with ProcessPoolExecutor(WORKERS) as pool:
        traj = list(pool.map(rate_trajectory_job, [(m, c, sr) for (m, c) in VOICINGS for sr in srs]))
        frozen = list(pool.map(rate_frozen_job, [(m, c, w, sr) for (m, c) in VOICINGS for w in FROZEN_W for sr in srs]))
    for (m, c), name in VOICINGS.items():
        base = next(f1 for (mm, cc, sr), f1 in traj if (mm, cc, sr) == (m, c, 48000))
        errs = [(sr, TJ.traj_error(base, f1)) for (mm, cc, sr), f1 in traj if (mm, cc) == (m, c) and sr != 48000]
        worst = max(e[0] for _, e in errs)
        report.add("P9 rate: trajectory", f"{name}, noon rate", worst <= 0.3,
                   f"worst {worst:.3f} % rms against its 48 kHz trajectory (<= 0.3 %); rms/max: "
                   + ", ".join(f"{sr / 1000:g}k {e[0]:.3f}/{e[1]:+.3f}" for sr, e in errs))
        nworst, dworst, nwhere, dwhere, missing = 0.0, 0.0, "", "", []
        for w in FROZEN_W:
            ref = next(r for r in frozen if r[0] == (m, c, w, 48000))
            for (mm, cc, ww, sr), notches, db in frozen:
                if (mm, cc, ww) != (m, c, w) or sr == 48000:
                    continue
                if len(notches) != len(ref[1]):
                    missing.append(f"w {w:+.1f} {sr / 1000:g}k: {len(notches)} notches, 48k {len(ref[1])}")
                for (fa, _), (fb, _) in zip(notches, ref[1]):
                    e = 100 * abs(fa / fb - 1)
                    if e >= nworst:
                        nworst, nwhere = e, f"w {w:+.1f} {sr / 1000:g}k {fa:.1f} vs {fb:.1f} Hz"
                mask = ref[2] >= ref[2].max() - 20.0
                d = float(np.max(np.abs(db - ref[2])[mask]))
                if d >= dworst:
                    dworst, dwhere = d, f"w {w:+.1f} {sr / 1000:g}k"
        report.add("P9 rate: frozen notches", name, nworst <= 0.2 and not missing,
                   f"worst {nworst:.3f} % (<= 0.2 %) at {nwhere}; notches <= 10 kHz, sweep at "
                   + "/".join(f"{w:+g}" for w in FROZEN_W) + ("; " + "; ".join(missing) if missing else ""))
        # 1.8 dB is the structure's limit, not a target: four first-order stages at 44.1/48 kHz lag
        # the analog cascade by 4.5 to 7.3 degrees near 10 kHz (evidence/fit/rate_limit.txt, closed
        # form 1.06 to 1.69 dB). Closing it needs a wet-path correction that lifts the band above
        # 10 kHz by 3 to 6 dB, which was declined (FX-ENGINES D-30).
        report.add("P9 rate: frozen response", name, dworst <= 1.8,
                   f"worst {dworst:.2f} dB (<= 1.8 dB, the structure's limit) at {dwhere}; |H| 20 Hz-10 kHz where "
                   "the 48 kHz response is within 20 dB of its peak")


# ---------------------------------------------------------------------------- P10 (sync jumps)

def hp_residual(y, sr, fc=2000.0, taps=1023, beta=14.0):
    """The verifier's click measure (build-verify/mod/h/clicklib.py): |y| through a Kaiser FIR highpass."""
    n = np.arange(taps) - (taps - 1) / 2
    h = -np.sinc(2 * fc / sr * n) * 2 * fc / sr
    h[(taps - 1) // 2] += 1.0
    return np.abs(np.convolve(y, h * np.kaiser(taps, beta), mode="same"))


def jump_job(args):
    model, color, start, jump, sr, bs = args
    rate, secs, te = 1.0, 2.0, 1.0
    t = np.arange(int(secs * sr)) / sr
    x = (fx.amp(-12.0) * np.sin(2 * np.pi * 220.0 * t)).astype(np.float32).astype(np.float64)
    te_blk = math.ceil(te * sr / bs) * bs / sr
    p_end = start + rate * te_blk
    tokens = [f"model={model}", f"color={color}", f"rate={rate}",
              f"sync_phase@0..{te_blk}={start}..{p_end}",
              f"sync_phase@{te_blk}..{secs}={p_end + jump}..{p_end + jump + rate * (secs - te_blk)}"]
    y = fx.invoke(EFFECT, x[None, :], tokens, [f"--block={bs}"], sr).out[0]
    r = hp_residual(y, sr)
    ev = (t >= te_blk - 0.005) & (t <= te_blk + 0.12)
    st = (t > 0.3) & (t < te_blk - 0.05)
    return args, 20 * math.log10(max(r[ev].max(), 1e-12)), 20 * math.log10(max(r[st].max(), 1e-12))


def p10_sync_jumps(report, quick):
    """A song-position jump (transport start, locate, loop) is honoured, but the sweep glides to the
    new phase's position: a 220 Hz tone at -12 dBFS ticks no louder than -60 dBFS above 2 kHz."""
    starts = (0.0, 0.25, 0.5, 0.75)          # sweep bottom, rising, top, falling
    jumps = (0.05, 0.25, 0.5, -0.3)
    sr, bs = 48000, 64
    jobs = [(m, c, a, j, sr, bs) for (m, c) in VOICINGS for a in starts for j in jumps]
    with ProcessPoolExecutor(WORKERS) as pool:
        res = list(pool.map(jump_job, jobs))
    for (m, c), name in VOICINGS.items():
        rows = [r for r in res if r[0][:2] == (m, c)]
        worst = max(rows, key=lambda r: r[1])
        ok = worst[1] <= -60.0
        report.add("P10 sync jump", name, ok,
                   f"worst tick {worst[1]:.1f} dBFS above 2 kHz (<= -60; steady {max(r[2] for r in rows):.1f}) at "
                   f"phase {worst[0][2]} jump {worst[0][3]:+}; {len(rows)} jumps: phases "
                   + "/".join(map(str, starts)) + " x jumps " + "/".join(f"{j:+}" for j in jumps))


# ---------------------------------------------------------------------------- P11 (re-enable)

def reenable_job(args):
    """Drives the engine as the processor will: a loud signal, then no process() calls for a second
    (block off), then reset() and processing again (block back on)."""
    model, color, silent, sr, bs = args
    secs = 4.0
    t = np.arange(int(secs * sr)) / sr
    x = fx.amp(-6.0) * np.sin(2 * np.pi * 110.0 * t) + fx.pink(secs, -20.0, sr, seed=31)[: len(t)]
    if silent:
        x[int(1.2 * sr):] = 0.0
    x = np.vstack([x, x]).astype(np.float32).astype(np.float64)
    keys = keys_for(model, color, reset_on_enable=1)
    out = fx.invoke(EFFECT, x, fx._tokens(keys) + ["on@1.0=0", "on@2.0=1"], [f"--block={bs}"], sr).out
    k = int(math.ceil(2.0 * sr / bs) * bs)
    if silent:
        return args, float(np.max(np.abs(out[:, k:])))
    fresh = fx.invoke(EFFECT, x[:, k:], keys, [f"--block={bs}"], sr).out
    return args, float(np.max(np.abs(out[:, k:] - fresh)))


def p11_reenable(report, quick):
    jobs = [(m, c, silent, sr, bs) for (m, c) in VOICINGS for silent in (True, False)
            for sr, bs in ((48000, 64), (44100, 512))]
    with ProcessPoolExecutor(WORKERS) as pool:
        res = list(pool.map(reenable_job, jobs))
    for (m, c), name in VOICINGS.items():
        silent = max(r[1] for r in res if r[0][:3] == (m, c, True))
        live = max(r[1] for r in res if r[0][:3] == (m, c, False))
        db = lambda v: "0 (exact)" if v == 0.0 else f"{20 * math.log10(v):.1f} dBFS"  # noqa: E731
        report.add("P11 re-enable", name, max(silent, live) <= fx.amp(-60.0),
                   f"after 1 s off and reset(): silent input peaks at {db(silent)}, a continuing signal differs from "
                   f"a fresh engine's output by {db(live)} (both <= -60 dBFS); 48k/64, 44.1k/512")


# ---------------------------------------------------------------------------- P12 (non-finite input)

def p12_nonfinite(report):
    sr = 48000
    base = np.vstack([fx.pink(3.0, -18.0, sr, seed=41), fx.pink(3.0, -18.0, sr, seed=42)])
    bad = base.copy()
    bad[0, sr] = np.nan
    bad[1, int(1.5 * sr)] = np.inf
    bad[:, int(1.7 * sr)] = -np.inf
    bad[:, 2 * sr:2 * sr + 64] = np.nan
    clean = np.where(np.isfinite(bad), bad, 0.0)
    for model, color, name in [(0, 1, "Classic")] + [(m, c, n) for (m, c), n in VOICINGS.items()]:
        ok, detail = True, ""
        for bs in (64, 512):
            keys = keys_for(model, color, rate=3.0)
            y = fx.invoke(EFFECT, bad, keys, [f"--block={bs}"], sr).out
            ref = fx.invoke(EFFECT, clean, keys, [f"--block={bs}"], sr).out
            if not np.all(np.isfinite(y)):
                ok, detail = False, f"block {bs}: {int(np.sum(~np.isfinite(y)))} non-finite output samples"
            elif not np.array_equal(y, ref):
                ok, detail = False, f"block {bs}: differs from the render with zeros in place of the bad samples"
        report.add("P12 non-finite input", name, ok,
                   detail or "NaN, +Inf, -Inf samples and a 64-sample NaN burst: output finite and bit-identical to "
                   "the input with zeros in their place (nothing latched), blocks 64/512")
    # Finite samples no instrument produces: the pair +3e38, -3e38 latched Classic to NaN (round-2 N14).
    huge = base.copy()
    huge[:, sr:sr + 2] = [[3e38, -3e38], [3e38, -3e38]]
    huge[0, int(1.5 * sr)] = 2e6
    huge[1, int(1.7 * sr)] = -1.5e6
    clean = np.where(np.abs(huge) < 1e6, huge, 0.0)
    for model, color, name in [(0, 1, "Classic")] + [(m, c, n) for (m, c), n in VOICINGS.items()]:
        ok, detail = True, ""
        for bs in (64, 512):
            keys = keys_for(model, color, rate=3.0)
            y = fx.invoke(EFFECT, huge, keys, [f"--block={bs}"], sr).out
            ref = fx.invoke(EFFECT, clean, keys, [f"--block={bs}"], sr).out
            if not np.all(np.isfinite(y)):
                ok, detail = False, f"block {bs}: {int(np.sum(~np.isfinite(y)))} non-finite output samples"
            elif not np.array_equal(y, ref):
                ok, detail = False, f"block {bs}: differs from the render with zeros in place of the huge samples"
        report.add("P12 huge input", name, ok,
                   detail or "+3e38/-3e38 pair, 2e6, -1.5e6: output finite and bit-identical to the input with zeros "
                   "in their place, blocks 64/512")


# ---------------------------------------------------------------------------- P13, P14 (motion against GR6)

def load_fit_module(name):
    if str(FIT) not in sys.path:
        sys.path.insert(0, str(FIT))
    return importlib.import_module(name)


def motion_job(args):
    tag, model, color, rate, note = args
    R = load_fit_module("refmeasure")
    x = R.sine(note, 10 * 48000, 48000, -12.0)
    y = fx.invoke(EFFECT, x[None, :], keys_for(model, color, rate=rate), ["--block=64"], 48000).out[0]
    return tag, R.cents(y, note, 48000)


def p13_motion(report):
    """Pitch deviation of a held 196 Hz note, ours against Guitar Rig 6 (evidence/fit/motion.tsv): the
    part of a phaser's sound that a magnitude fit cannot see (round-2 N2)."""
    ref = load_fit_module("reftakes").read_motion(str(FIT / "motion.tsv"))
    jobs = [(t, r["model"], r["color"], r["rate"], r["note"]) for t, r in ref.items()]
    with ProcessPoolExecutor(WORKERS) as pool:
        res = dict(pool.map(motion_job, jobs))
    for (m, c), name in VOICINGS.items():
        tags = [t for t, r in ref.items() if (r["model"], r["color"]) == (m, c)]
        if not tags:
            continue
        errs = [(t, 100 * (res[t][0] / ref[t]["p99"] - 1)) for t in tags]
        worst = max(abs(e) for _, e in errs)
        report.add("P13 pitch vs GR6", f"{name}, 196 Hz, {len(tags)} rates", worst <= 15.0,
                   f"worst {worst:.1f} % (<= 15 %) of Guitar Rig 6's p99; ours/GR6 p99 cents: "
                   + ", ".join(f"{ref[t]['rate']:.3g} Hz {res[t][0]:.1f}/{ref[t]['p99']:.1f}" for t in tags))


NULL_REF = {"p9_block_p050": (1, 1, 1.0 / 0.93375), "p9_block_p090": (1, 1, 1.0 / 0.26875),
            "ss_off_r050": (2, 0, 0.01 * 1000 ** 0.5), "ss_off_r075": (2, 0, 0.01 * 1000 ** 0.75),
            "ss_on_r050": (2, 1, 0.01 * 1000 ** 0.5), "ss_on_r075": (2, 1, 0.01 * 1000 ** 0.75)}
NULL_BARS = {(1, 1): -25.9, (2, 0): -24.0, (2, 1): -29.2}


def null_job(tag):
    """Waveform null of one guitar-like take against Guitar Rig 6 (evidence/fit/gr6_takes): our LFO
    started at the phase that nulls best (Guitar Rig starts near 0), Guitar Rig's latency 16 or 17
    samples, one least-squares gain."""
    R = load_fit_module("refmeasure")
    model, color, rate = NULL_REF[tag]
    sr, secs = 48000, 8.0
    ref = load_fit_module("gr6").read_wav(str(FIT / "gr6_takes" / f"{tag}.wav"))[0][0]
    gtr = R.guitar(int(secs * sr), sr, seed=7)

    def ours(ph):
        p0 = ph % 1.0
        tokens = fx._tokens(keys_for(model, color, rate=rate)) + [f"sync_phase@0..{secs}={p0}..{p0 + rate * secs}"]
        return fx.invoke(EFFECT, gtr[None, :], tokens, ["--block=64"], sr).out[0]
    best = (0.0, 0.0, 16)
    for ph in np.linspace(-0.004, 0.008, 25):
        y = ours(ph)
        for lat in (16, 17):
            r = R.null(y, ref, sr, lat)[0]
            if r < best[0]:
                best = (r, ph, lat)
    for width in (0.0005, 0.0001):
        for ph in best[1] + np.linspace(-width, width, 9):
            r = R.null(ours(ph), ref, sr, best[2])[0]
            if r < best[0]:
                best = (r, ph, best[2])
    return tag, best


def p14_null(report):
    with ProcessPoolExecutor(WORKERS) as pool:
        res = dict(pool.map(null_job, list(NULL_REF)))
    for (m, c), bar in NULL_BARS.items():
        tags = [t for t, v in NULL_REF.items() if v[:2] == (m, c)]
        worst = max(res[t][0] for t in tags)
        report.add("P14 null vs GR6", f"{VOICINGS[(m, c)]}, guitar take", worst <= bar,
                   f"worst {worst:.1f} dB (<= {bar}) re Guitar Rig 6's output; per rate: "
                   + ", ".join(f"{NULL_REF[t][2]:.3g} Hz {res[t][0]:.1f} dB" for t in tags))


# ---------------------------------------------------------------------------- P15, P16 (sync)

def wrap_half(x):
    return x - np.floor(x + 0.5)


def tick_db(y, sr, lo, hi):
    """Largest |hp_residual| over samples [lo, hi), the filter run over a margin either side."""
    m = 1024
    a, b = max(0, lo - m), min(len(y), hi + m)
    r = hp_residual(y[a:b], sr)[lo - a:hi - a]
    return 20 * math.log10(max(float(r.max()), 1e-12))


def sync_tokens(model, color, start, jump, te_blk, secs, rate=1.0):
    """The song position puts the LFO at `start` when the jump happens (te_blk, a block boundary),
    whatever the block size, then `jump` cycles further on."""
    p0 = (start - rate * te_blk) % 1.0
    return [f"model={model}", f"color={color}", f"rate={rate}", f"sync_phase@0..{te_blk}={p0}..{p0 + rate * te_blk}",
            f"sync_phase@{te_blk}..{secs}={start + jump}..{start + jump + rate * (secs - te_blk)}"]


JUMP_TONES = ((220.0, 82.41), (329.63, 146.83))


def jump_grid_job(args):
    """Tick of one jump: two probe tones at -12 dBFS (one per channel), worst channel."""
    model, color, start, jump, sr, bs, tones = args
    secs, te = 1.4, 0.7
    te_blk = math.ceil(te * sr / bs) * bs / sr
    t = np.arange(int(secs * sr)) / sr
    x = np.vstack([fx.amp(-12.0) * np.sin(2 * np.pi * f * t) for f in tones]).astype(np.float32).astype(np.float64)
    y = fx.invoke(EFFECT, x, sync_tokens(model, color, start, jump, te_blk, secs), [f"--block={bs}"], sr).out
    k = int(round(te_blk * sr))
    return args, max(tick_db(y[ch], sr, k - int(0.005 * sr), k + int(0.2 * sr)) for ch in range(2))


def retarget_job(args):
    """A second jump while the first glide runs (+0.5, then +0.25 about 15 ms later): the glide restarts
    from where the sweep is. Two tones, worst channel."""
    model, color, start, sr, bs, tones = args
    secs, te = 1.4, 0.7
    te_blk = math.ceil(te * sr / bs) * bs / sr
    te2 = te_blk + math.ceil(0.015 * sr / bs) * bs / sr
    t = np.arange(int(secs * sr)) / sr
    x = np.vstack([fx.amp(-12.0) * np.sin(2 * np.pi * f * t) for f in tones]).astype(np.float32).astype(np.float64)
    p0 = (start - te_blk) % 1.0
    tokens = [f"model={model}", f"color={color}", "rate=1.0", f"sync_phase@0..{te_blk}={p0}..{p0 + te_blk}",
              f"sync_phase@{te_blk}..{te2}={start + 0.5}..{start + 0.5 + te2 - te_blk}",
              f"sync_phase@{te2}..{secs}={start + 0.75 + te2 - te_blk}..{start + 0.75 + secs - te_blk}"]
    y = fx.invoke(EFFECT, x, tokens, [f"--block={bs}"], sr).out
    k = int(round(te_blk * sr))
    return args, max(tick_db(y[ch], sr, k - int(0.005 * sr), k + int(0.2 * sr)) for ch in range(2))


def settle_job(args):
    """Time from a jump until the output is within -40 dB of the same voicing running at the new phase
    all along (10 ms windows, re the input: a 220 Hz tone at -12 dBFS)."""
    model, color, start, jump, sr, bs = args
    secs, te = 1.6, 0.6
    te_blk = math.ceil(te * sr / bs) * bs / sr
    t = np.arange(int(secs * sr)) / sr
    x = (fx.amp(-12.0) * np.sin(2 * np.pi * 220.0 * t)).astype(np.float32).astype(np.float64)[None, :]
    y = fx.invoke(EFFECT, x, sync_tokens(model, color, start, jump, te_blk, secs), [f"--block={bs}"], sr).out[0]
    p_new = (start + jump - te_blk) % 1.0
    ref = fx.invoke(EFFECT, x, [f"model={model}", f"color={color}", "rate=1.0", f"sync_phase@0..{secs}={p_new}..{p_new + secs}"],
                    [f"--block={bs}"], sr).out[0]
    n = int(0.01 * sr)
    k = int(round(te_blk * sr))
    e = env_db(y[k:] - ref[k:], n) - env_db(x[0, k:], n)
    above = np.nonzero(e > -40.0)[0]
    return args, 1e3 * ((int(above[-1]) + 1) if len(above) else 0) / sr


def p15_sync_jumps(report, quick):
    """Song-position jumps at every host block size and sample rate: one glide of fixed length."""
    starts, jumps = (0.0, 0.25, 0.5, 0.75), (0.1, 0.25, 0.5)
    blocks = (16, 64, 256, 512, 1024, 4096)
    jobs = [(m, c, a, j, 48000, bs, tones) for (m, c) in VOICINGS for a in starts for j in jumps for bs in blocks
            for tones in JUMP_TONES]
    jobs += [(m, c, a, j, sr, bs, tones) for (m, c) in VOICINGS for a in (0.0, 0.5) for j in jumps
             for sr in (44100, 96000, 192000) for bs in (16, 512, 4096) for tones in JUMP_TONES]
    settle = [(m, c, a, j, sr, bs) for (m, c) in VOICINGS for (a, j) in ((0.0, 0.5), (0.25, 0.25))
              for sr in (48000, 192000) for bs in blocks]
    retarget = [(m, c, a, 48000, bs, tones) for (m, c) in VOICINGS for a in (0.0, 0.5) for bs in (16, 64, 256, 512)
                for tones in JUMP_TONES]
    with ProcessPoolExecutor(WORKERS) as pool:
        ticks = list(pool.map(jump_grid_job, jobs))
        times = list(pool.map(settle_job, settle))
        twice = list(pool.map(retarget_job, retarget))
    for (m, c), name in VOICINGS.items():
        rows = [r for r in ticks if r[0][:2] == (m, c)]
        worst = max(rows, key=lambda r: r[1])
        a = worst[0]
        report.add("P15 sync jump tick", name, worst[1] <= -60.0,
                   f"worst {worst[1]:.1f} dBFS above 2 kHz (<= -60) at phase {a[2]} jump +{a[3]} {a[4] / 1000:g}k block {a[5]} "
                   f"{a[6][0]:g}/{a[6][1]:g} Hz; {len(rows)} renders: phases 0/0.25/0.5/0.75 x jumps 0.1/0.25/0.5 x blocks "
                   "16-4096 at 48k, phases 0/0.5 at 44.1/96/192k x blocks 16/512/4096; tones 82/147/220/330 Hz at -12 dBFS")
        rows = [r for r in twice if r[0][:2] == (m, c)]
        worst = max(rows, key=lambda r: r[1])
        report.add("P15 sync jump twice", name, worst[1] <= -60.0,
                   f"worst {worst[1]:.1f} dBFS above 2 kHz (<= -60) at phase {worst[0][2]} block {worst[0][4]}; +0.5 then +0.25 "
                   "about 15 ms later, phases 0/0.5, blocks 16/64/256/512 at 48k, tones 82/147/220/330 Hz")
        for (s0, j) in ((0.0, 0.5), (0.25, 0.25)):
            ts = [(r[0][4], r[0][5], r[1]) for r in times if r[0][:4] == (m, c, s0, j)]
            vals = [v for _, _, v in ts]
            ok = max(vals) <= 1.2 * min(vals)
            report.add("P15 sync jump settle", f"{name}, phase {s0} jump +{j}", ok,
                       f"to within -40 dB of the new phase in {min(vals):.1f}-{max(vals):.1f} ms (spread <= 20 %) over blocks "
                       "16-4096 at 48/192k: " + ", ".join(f"{sr / 1000:g}k/{bs} {v:.1f}" for sr, bs, v in ts))


def lock_job(args):
    """A host that is not sample-exact: jittered song position, tempo steps of 1 BPM, a tempo ramp.
    -> (args, jumps glided, tick dBFS, largest phase error after 1 s in cycles)."""
    model, color, sr, bs, kind, note = args
    secs = 4.0
    t = np.arange(int(secs * sr)) / sr
    x = (fx.amp(-12.0) * np.sin(2 * np.pi * 220.0 * t)).astype(np.float32).astype(np.float64)[None, :]
    base = [f"model={model}", f"color={color}", f"host_note={note}", "host_ppq=0.3"]
    if kind.startswith("jit"):
        prog = ["host_bpm=120"]
        tokens, ref_tokens = base + prog + [f"host_jitter={kind[3:]}"], base + prog
    elif kind == "step":
        prog = [f"host_bpm@{0.25 * k}={120 + k}" for k in range(16)]
        tokens = base + prog
        ref_tokens = [f"model={model}", f"color={color}", f"sync_phase={0.3 * note % 1.0}", f"sync_phase@{0.5 * bs / sr}=-1"] \
            + [f"rate@{0.25 * k}={(120 + k) / 60.0 * note}" for k in range(16)]
    else:
        tokens = base + [f"host_bpm@0..{secs}=100..140"]
        ref_tokens = [f"model={model}", f"color={color}", f"sync_phase={0.3 * note % 1.0}", f"sync_phase@{0.5 * bs / sr}=-1",
                      f"rate@0..{secs}={100 / 60.0 * note}..{140 / 60.0 * note}"]
    r = fx.invoke(EFFECT, x, tokens, [f"--block={bs}", "--trace"], sr)
    ref = fx.invoke(EFFECT, x, ref_tokens, [f"--block={bs}"], sr).out[0]
    ends = np.arange(bs - 1, len(t), bs)
    err = np.abs(wrap_half(r.trace[0][ends].astype(np.float64) - r.trace[2][ends].astype(np.float64)))
    late = ends / sr > 1.0
    lo, hi = int(0.3 * sr), len(t) - int(0.05 * sr)
    tick = tick_db(r.out[0] - ref, sr, lo, hi)
    return args, int(round(float(r.trace[1][-1]))), tick, float(err[late].max())


def p16_sync_lock(report):
    kinds = (("jit2", 1), ("jit8", 1), ("jit8", 4), ("step", 1), ("ramp", 1), ("ramp", 4))
    settings = ((44100, 64), (44100, 4096), (96000, 512), (192000, 16))
    jobs = [(m, c, sr, bs, k, note) for (m, c) in VOICINGS for sr, bs in settings for k, note in kinds]
    with ProcessPoolExecutor(WORKERS) as pool:
        res = list(pool.map(lock_job, jobs))
    # The tick is the high-passed difference from the same voicing following an exact host (jitter) or
    # running free at the same rates (steps, ramp). The 1/16-note ramp is left out of it: a free-running
    # LFO drifts from a ramping host by design, and at 8 Hz the Color-on sweep's own content above 2 kHz
    # (-75 dBFS) moves with any LFO offset; it is checked for glides and phase error.
    for (m, c), name in VOICINGS.items():
        rows = [r for r in res if r[0][:2] == (m, c)]
        glides = sum(r[1] for r in rows)
        tick = max((r for r in rows if r[0][4:6] != ("ramp", 4)), key=lambda r: r[2])
        err = max(rows, key=lambda r: r[3])
        ok = glides == 0 and tick[2] <= -80.0 and err[3] < 0.01
        report.add("P16 sync lock", name, ok,
                   f"{glides} glides (0), worst tick {tick[2]:.1f} dBFS (<= -80; {tick[0][4]} 1/{4 * tick[0][5]} "
                   f"{tick[0][2] / 1000:g}k/{tick[0][3]}), phase error after 1 s {err[3]:.2e} cycle (< 0.01; {err[0][4]} "
                   f"1/{4 * err[0][5]} {err[0][2] / 1000:g}k/{err[0][3]}); song position jittered +-2/+-8 samples per block "
                   "(1/4, 1/16 notes), tempo stepped 120-135 BPM in 1 BPM steps (1/4), ramped 100-140 BPM over 4 s (1/4, 1/16); "
                   "44.1k/64, 44.1k/4096, 96k/512, 192k/16")


# ---------------------------------------------------------------------------- P17 (switch level)

SWITCH_VOICES = {"Classic": (0, 1), "script": (1, 0), "block": (1, 1), "Stone off": (2, 0), "Stone on": (2, 1)}


def switch_signal(kind, sr, secs):
    t = np.arange(int(secs * sr)) / sr
    if kind == "guitar":
        g = load_fit_module("refmeasure").guitar(len(t), sr, seed=11, gap=0.25)
        return np.vstack([g, g])
    s = (fx.amp(-12.0) * np.sin(2 * np.pi * float(kind) * t)).astype(np.float32).astype(np.float64)
    return np.vstack([s, s])


def frame_db(y, lo, hi, n, hop):
    """Level in dB of n-sample windows starting every hop samples in [lo, hi), summed over channels."""
    p = np.sum(y.astype(np.float64) ** 2, axis=0)
    c = np.concatenate([[0.0], np.cumsum(p)])
    starts = np.arange(lo, hi, hop)
    return 10 * np.log10(np.maximum((c[starts + n] - c[starts]) / n, 1e-20))


def switch_level_job(args):
    """Level of a switch against both voicings' own levels, frame by frame: the outgoing one rendered
    without the switch, the incoming one started fresh at the switch block (as the switch starts it)."""
    a, b, kind, ts = args
    sr, bs, secs = 48000, 128, 2.0
    x = switch_signal(kind, sr, secs)
    before = keys_for(*SWITCH_VOICES[a], rate=1.0)
    after = keys_for(*SWITCH_VOICES[b], rate=1.0)
    events = [f"model@{ts}={after['model']}", f"color@{ts}={after['color']}"]
    out = fx.invoke(EFFECT, x, fx._tokens(before) + events, [f"--block={bs}"], sr).out
    ya = fx.invoke(EFFECT, x, before, [f"--block={bs}"], sr).out
    t_s = int(math.ceil(ts * sr / bs) * bs)
    yb = np.zeros_like(out)
    yb[:, t_s:] = fx.invoke(EFFECT, x[:, t_s:], after, [f"--block={bs}"], sr).out
    n, hop = int(0.02 * sr), int(0.0025 * sr)
    lo, hi = t_s, t_s + int(0.09 * sr)
    lo_out, la, lb = (frame_db(y, lo, hi, n, hop) for y in (out, ya, yb))
    up = float(np.max(lo_out - np.maximum(la, lb)))
    down = float(np.max(np.minimum(la, lb) - lo_out))
    click = None
    if kind != "guitar":
        click = max(tick_db(out[ch], sr, t_s - int(0.005 * sr), t_s + int(0.1 * sr)) for ch in range(2))
    return args, up, down, click


def p17_switch_level(report):
    """Every ordered pair of the five voicings, held notes and a guitar-like take, two switch times."""
    kinds = ("82.41", "196.0", "440.0", "guitar")
    jobs = [(a, b, k, ts) for a in SWITCH_VOICES for b in SWITCH_VOICES if a != b for k in kinds for ts in (1.0, 1.37)]
    with ProcessPoolExecutor(WORKERS) as pool:
        res = list(pool.map(switch_level_job, jobs))
    up = max(res, key=lambda r: r[1])
    down = max(res, key=lambda r: r[2])
    click = max((r for r in res if r[3] is not None), key=lambda r: r[3])
    ok = up[1] <= 1.5 and down[2] <= 3.0 and click[3] <= -60.0
    where = lambda r: f"{r[0][0]} -> {r[0][1]}, {r[0][2]}, {r[0][3]} s"  # noqa: E731
    report.add("P17 switch level", f"20 ordered pairs x 4 signals x 2 times", ok,
               f"at most {up[1]:+.2f} dB above the higher steady level (<= 1.5; {where(up)}), {down[2]:.2f} dB below the "
               f"lower (<= 3; {where(down)}), click {click[3]:.1f} dBFS (<= -60; {where(click)}); 20 ms frames over 90 ms "
               "from the switch; held notes 82/196/440 Hz at -12 dBFS and a guitar-like take")


def switch_rt(report):
    """--rt-check, --reset-repeat and --bench while models and Color switch every 50 ms. The sequence
    starts on Phase 90: a run that starts on Classic fails --reset-repeat in the legacy code too."""
    events = []
    keys = list(SWITCH_VOICES.values())[1:] + [SWITCH_VOICES["Classic"]]
    for k in range(60):
        m, c = keys[k % len(keys)]
        events += [f"model@{0.05 * k}={m}", f"color@{0.05 * k}={c}"]
    ok, detail = True, ""
    for sr, bs in ((48000, 64), (44100, 37), (96000, 512), (192000, 4096)):
        for opt in ("--rt-check", "--reset-repeat"):
            r = fx.invoke(EFFECT, "gen:music", ["rate=1.0"] + events, [f"--block={bs}", opt], sr)
            if r.code != 0:
                ok, detail = False, f"{opt} {sr} Hz block {bs}: {r.stdout.strip()}"
    cpu = fx._bench(EFFECT, "gen:noise:pink:-18", ["rate=1.0"] + events)
    report.add("switch rt safety", "model/Color every 50 ms", ok and cpu <= 0.5,
               detail or f"no allocation, reset-repeat identical at 48k/64, 44.1k/37, 96k/512, 192k/4096; cpu {cpu:.4f} % "
               "(<= 0.5 %, the Small Stone budget)")


# ---------------------------------------------------------------------------- P18 (level)

def k_weight_db(x, sr):
    """Ungated BS.1770 loudness of a whole mono take, in dB: the K-weighting (pre-filter shelf and
    RLB high-pass, the standard's 48 kHz biquads) applied to the take's power spectrum."""
    def biquad(b, a, f):
        z = np.exp(-2j * np.pi * f / sr)
        return (b[0] + b[1] * z + b[2] * z * z) / (a[0] + a[1] * z + a[2] * z * z)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / sr)
    h = biquad((1.53512485958697, -2.69169618940638, 1.19839281085285), (1.0, -1.69065929318241, 0.73248077421585), f) \
        * biquad((1.0, -2.0, 1.0), (1.0, -1.99004745483398, 0.99007225036621), f)
    return 10 * math.log10(np.sum(np.abs(X * h) ** 2) / len(x) ** 2)


LEVEL_TAKES = {"mixed": dict(seed=7), "low riff": dict(seed=8, notes=[82.41, 98.0, 110.0, 123.47, 82.41, 146.83], gap=0.3),
               "high": dict(seed=9, notes=[329.63, 392.0, 440.0, 493.88, 587.33, 659.25, 783.99], gap=0.35)}


def level_job(args):
    model, color, take = args
    R = load_fit_module("refmeasure")
    sr = 48000
    x = R.guitar(12 * sr, sr, **LEVEL_TAKES[take])
    y = fx.invoke(EFFECT, np.vstack([x, x]), keys_for(model, color, rate=f32(kPhaserRate(model))), ["--block=64"], sr).out[0]
    s = slice(sr, None)
    return args, k_weight_db(y[s], sr) - k_weight_db(x[s], sr)


def kPhaserRate(model):
    return {0: 1.0, 1: 1.07, 2: 0.32}[model]


def headroom_job(args):
    model, color, kind, rate = args
    sr = 48000
    t = np.arange(8 * sr) / sr
    if kind == "guitar":
        x = load_fit_module("refmeasure").guitar(len(t), sr, seed=5, peak_db=0.0)
    elif kind.startswith("sq"):
        x = np.sign(np.sin(2 * np.pi * float(kind[2:]) * t))
    else:
        x = np.sin(2 * np.pi * float(kind) * t)
    y = fx.invoke(EFFECT, np.vstack([x, x]), keys_for(model, color, rate=rate), ["--block=64"], sr).out
    return args, float(np.max(np.abs(y)))


def p18_level(report):
    """Make-up rule (voicings.md, Level): unity K-weighted loudness against bypass, averaged over a
    mixed, a low and a high-register guitar-like take, at the voicing's starting rate. Also the peak a
    full-scale input reaches (squares, sines at the resonance region, a 0 dBFS take, starting rate and
    10 Hz), which the processor's documentation must warn about."""
    jobs = [(m, c, k) for (m, c) in VOICINGS for k in LEVEL_TAKES]
    peaks = [(m, c, k, r) for (m, c) in VOICINGS for k in ("sq41.2", "sq82.4", "sq110", "97.3", "440", "1318.5", "3300", "guitar")
             for r in (kPhaserRate(m), 10.0)]
    with ProcessPoolExecutor(WORKERS) as pool:
        res = list(pool.map(level_job, jobs))
        pk = list(pool.map(headroom_job, peaks))
    for (m, c), name in VOICINGS.items():
        vals = [(a[2], v) for a, v in res if a[:2] == (m, c)]
        mean = float(np.mean([v for _, v in vals]))
        worst = max((r for r in pk if r[0][:2] == (m, c)), key=lambda r: r[1])
        report.add("P18 level vs bypass", name, abs(mean) <= 0.25,
                   f"register mean {mean:+.2f} dB (|.| <= 0.25), K-weighted, " + ", ".join(f"{k} {v:+.2f}" for k, v in vals)
                   + f"; full-scale peak {worst[1]:.2f} ({20 * math.log10(worst[1]):+.1f} dB, {worst[0][2]} at {worst[0][3]:g} Hz)")


# ---------------------------------------------------------------------------- extra

def out_of_range(report):
    """setParameters clamps anything: out-of-range and non-finite keys still give finite output."""
    bad = []
    for keys in ({"model": 7}, {"model": -3}, {"model": 1, "rate": 1e9}, {"model": 2, "rate": -4},
                 {"model": 1, "rate": "nan"}, {"model": 2, "sync_phase": "nan"}, {"model": 1, "sync_phase": 1e12},
                 {"model": 0, "rate": "nan", "depth": "inf", "mix": "-inf"}, {"model": 2, "color": 1, "rate": "inf"}):
        tokens = [f"{k}={v}" for k, v in keys.items()]
        out = fx.invoke(EFFECT, "gen:music", tokens, ["--block=128"], 48000).out
        if not np.all(np.isfinite(out)) or np.max(np.abs(out)) >= 1e3:
            bad.append(str(keys))
    report.add("finite (bad keys)", "model/rate/sync/depth/mix out of range", not bad, "; ".join(bad) or "finite")


def specific_checks(report, args):
    only = args.types
    p1_classic_null(report, args.quick)
    if only is None or 1 in only or 2 in only:
        p2_parity(report, args.quick)
        p3_frozen(report)
        p4_trajectory(report, args.quick)
        p5_rate(report)
        p6_stability(report)
        p7_switch(report)
        p8_independence(report)
        p9_rate_independence(report, args.quick)
        p10_sync_jumps(report, args.quick)
        p11_reenable(report, args.quick)
        p13_motion(report)
        p14_null(report)
        p15_sync_jumps(report, args.quick)
        p16_sync_lock(report)
        p17_switch_level(report)
        switch_rt(report)
        p18_level(report)
        out_of_range(report)
    p12_nonfinite(report)


def main():
    args = fx.parse_args(__doc__)
    report = fx.Report(EFFECT)
    specific_checks(report, args)
    fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                     quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
