#!/usr/bin/env python3
"""Shared harness for the engine acceptance scripts (docs/FX-ENGINES.md §2.11, §5.3).

numpy only, safe under `python3 -I`. The check_<effect>.py scripts import this module:

    import fxcheck as fx
    out, trace = fx.run("comp", fx.burst(1000, -10, 0.5, 48000), {"type": 1}, ["--trace=gain"])

Binary: $FXBENCH, else build/tubamp_fxbench_artefacts/Release/tubamp_fxbench under the
repo root (a per-effect build in build-fx/<effect>/ is found through $FXBENCH; see
tools/fxbench/README.md).

Signals are float64 arrays shaped [channels, frames]; a 1-D array is treated as mono.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True  # keep __pycache__ out of tools/re and tools/fxbench
HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "tools" / "re"))
import sig  # noqa: E402  (tools/re/sig.py: WAV I/O, dB helpers, Welch transfer function)

read_wav = sig.read_wav
write_wav = sig.write_wav
db = sig.db
amp = sig.amp

RATES = [22050, 44100, 48000, 96000, 192000]
BLOCKS = [1, 37, 512]
NULL_RATES = [44100, 48000, 96000]
NULL_BLOCKS = [512, 37]


# ---------------------------------------------------------------------------- binary

def binary():
    env = os.environ.get("FXBENCH")
    path = Path(env) if env else REPO / "build" / "tubamp_fxbench_artefacts" / "Release" / "tubamp_fxbench"
    if not path.exists():
        sys.exit(f"tubamp_fxbench not found at {path} (build it, or set FXBENCH)")
    return str(path)


class Result:
    """What one tubamp_fxbench invocation produced."""

    def __init__(self, out, trace, stdout, code, sr, meters):
        self.out, self.trace, self.stdout, self.code, self.sr, self.meters = out, trace, stdout, code, sr, meters

    def number(self, pattern):
        m = re.search(pattern, self.stdout)
        return float(m.group(1)) if m else None


def _tokens(keys):
    if keys is None:
        return []
    if isinstance(keys, dict):
        return [f"{k}={v!r}" if isinstance(v, float) else f"{k}={v}" for k, v in keys.items()]
    return list(keys)


def invoke(effect, inp, keys=None, opts=None, sr=48000):
    """Runs tubamp_fxbench once. `inp` is an array, a WAV path or a "gen:..." spec; `keys`
    a dict or a list of timeline tokens ("bass@0.5..1.0=5..10"); `opts` a list such as
    ["--block=37", "--trace=gain"]. A --trace / --meters option without a path gets a
    temporary one. Exit status 2 (usage or I/O) raises; 0 and 1 are returned."""
    opts = list(opts or [])
    with tempfile.TemporaryDirectory(prefix="fxcheck-") as tmp:
        tmp = Path(tmp)
        if isinstance(inp, str) and inp.startswith("gen:"):
            src = inp
            if not any(o.startswith("--sr") for o in opts):
                opts.append(f"--sr={sr}")
        elif isinstance(inp, (str, Path)):
            src = str(inp)
        else:
            src = str(tmp / "in.wav")
            write_wav(src, np.atleast_2d(inp), sr)
        out_path = tmp / "out.wav"
        trace_path = meters_path = None
        final = []
        for o in opts:
            if o.startswith("--trace"):
                trace_path = tmp / "trace.wav"
                o = f"--trace={trace_path}"
            elif o.startswith("--meters"):
                meters_path = tmp / "meters.tsv"
                o = f"--meters={meters_path}"
            final.append(o)
        cmd = [binary(), effect, src, str(out_path)] + _tokens(keys) + final
        done = subprocess.run(cmd, capture_output=True, text=True)
        if done.returncode == 2 or not out_path.exists():
            raise RuntimeError(f"{' '.join(cmd)}\n{done.stdout}{done.stderr}")
        out, rate = read_wav(out_path)
        trace = read_wav(trace_path)[0] if trace_path and trace_path.exists() else None
        meters = meters_path.read_text().splitlines() if meters_path and meters_path.exists() else None
        return Result(out, trace, done.stdout + done.stderr, done.returncode, rate, meters)


def run(effect, inp, keys=None, opts=None, sr=48000):
    """-> (out, trace): the contract's short form of invoke()."""
    r = invoke(effect, inp, keys, opts, sr)
    return r.out, r.trace


# ---------------------------------------------------------------------------- signals

def _secs(secs, sr):
    return np.arange(int(round(secs * sr))) / sr


def stereo(x):
    return np.vstack([x, x])


def sine(freq, level_db, secs, sr, phase=0.0):
    """Peak level in dBFS."""
    return amp(level_db) * np.sin(2 * np.pi * freq * _secs(secs, sr) + phase)


def sweep(f0, f1, secs, level_db, sr):
    """Exponential sine sweep with 10 ms fades."""
    t = _secs(secs, sr)
    k = np.log(f1 / f0)
    return sig.fade(amp(level_db) * np.sin(2 * np.pi * f0 * secs / k * (np.exp(t / secs * k) - 1.0)), sr, 10.0)


def burst(freq, level_db, length, sr, pre=0.2, post=1.0, fade_ms=1.0, phase=0.0):
    """Silence, a tone of `length` s ending in a raised-cosine fade of fade_ms, silence.
    Returns (signal, index of the end of the fade)."""
    n = int(round(length * sr))
    tone = amp(level_db) * np.sin(2 * np.pi * freq * np.arange(n) / sr + phase)
    nf = max(1, int(round(fade_ms * 1e-3 * sr)))
    tone[-nf:] *= 0.5 + 0.5 * np.cos(np.pi * np.arange(1, nf + 1) / nf)
    a, b = int(round(pre * sr)), int(round(post * sr))
    return np.concatenate([np.zeros(a), tone, np.zeros(b)]), a + n


def white(secs, level_db, sr, seed=12345):
    """Gaussian white noise at an RMS level, fixed seed."""
    x = np.random.default_rng(seed).standard_normal(int(round(secs * sr)))
    return x / np.sqrt(np.mean(x ** 2)) * amp(level_db)


def pink(secs, level_db, sr, seed=12345):
    """Pink noise (1/f by spectral shaping of seeded white noise) at an RMS level."""
    n = int(round(secs * sr))
    spec = np.fft.rfft(np.random.default_rng(seed).standard_normal(n))
    f = np.fft.rfftfreq(n, 1.0 / sr)
    spec[1:] /= np.sqrt(f[1:])
    spec[0] = 0.0
    x = np.fft.irfft(spec, n)
    return x / np.sqrt(np.mean(x ** 2)) * amp(level_db)


def chug(sr, secs=3.0, note=0.075, period=0.15, f0=82.41, level_db=-6.0, noise_db=-66.0, seed=7):
    """Palm-muted low E: f0 + harmonics 1/0.6/0.4/0.25 for `note` s every `period` s, 2 ms
    fades, over white noise at noise_db RMS (gate/evidence/bb/gen.py's chug)."""
    t = _secs(secs, sr)
    tone = sum(a * np.sin(2 * np.pi * f0 * h * t) for h, a in zip((1, 2, 3, 4), (1, 0.6, 0.4, 0.25)))
    tone *= amp(level_db) / np.max(np.abs(tone))
    gate = np.zeros_like(t)
    nn, nf = int(round(note * sr)), max(1, int(round(0.002 * sr)))
    ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(nf) / nf)
    env = np.ones(nn)
    env[:nf], env[-nf:] = ramp, ramp[::-1]
    for start in range(0, len(t) - nn, int(round(period * sr))):
        gate[start:start + nn] = env
    return tone * gate + white(secs, noise_db, sr, seed)


def hover(sr, freq=1000.0, level_db=-40.0, dev_db=3.0, rate=3.0, secs=6.0):
    """A tone whose level wobbles +-dev_db around level_db at `rate` Hz."""
    t = _secs(secs, sr)
    return amp(level_db + dev_db * np.sin(2 * np.pi * rate * t)) * np.sin(2 * np.pi * freq * t)


def step(sr, freq, db_before, db_after, t_step, secs):
    """A tone that steps from db_before to db_after at t_step (phase continuous)."""
    t = _secs(secs, sr)
    return np.where(t < t_step, amp(db_before), amp(db_after)) * np.sin(2 * np.pi * freq * t)


def extreme(sr, secs=20.0):
    """The §2.11 finite-output torture input: full-scale white noise, a +-1 square at
    20 Hz, a 1e-30 tail, then zeros (a quarter each)."""
    q = int(round(secs * sr / 4))
    rng = np.random.default_rng(99)
    noise = rng.uniform(-1.0, 1.0, q)
    square = np.where(np.sin(2 * np.pi * 20.0 * np.arange(q) / sr) >= 0, 1.0, -1.0)
    return np.concatenate([noise, square, np.full(q, 1e-30), np.zeros(q)])


def music(sr, channels=2):
    """gen:music (docs/FX-ENGINES.md §5.3.5), the numpy twin of fxbench::fillMusic in
    tools/fxbench/FxBench.h. Same algorithm and constants, but not guaranteed bit-equal
    (libm and FMA contraction differ): when exactness matters pass "gen:music" to run()."""
    total = int(round(6.0 * sr))
    notes = [(0.0, 2.0, [82.41]), (0.6, 2.0, [110.0]), (1.2, 2.0, [196.0]), (2.0, 3.0, [82.41, 110.0, 196.0])]
    harmonics = [1.0, 0.6, 0.4, 0.25]
    i = np.arange(total)
    sig2 = np.zeros((2, total))
    for ch in range(2):
        t = i / sr - (0.003 if ch == 1 else 0.0)
        for start, end, freqs in notes:
            tau = t - start
            live = (tau >= 0) & (t < end)
            env = np.where(tau < 0.005, tau / 0.005, np.exp(-(tau - 0.005) / 0.4)) * np.minimum(1.0, (end - t) / 0.005)
            for f in freqs:
                for h, a in enumerate(harmonics):
                    sig2[ch] += np.where(live, a * np.sin(2 * np.pi * f * (h + 1) * tau) * env, 0.0)
    peak = np.max(np.abs(sig2))
    sig2 *= amp(-6.0) / peak if peak > 0 else 0.0
    frame = lambda s: int(round(s * sr))  # noqa: E731
    noise_amp = amp(-30.0) * np.sqrt(3.0)
    x = 12345
    for lo, hi in ((frame(2.0), frame(3.0)), (frame(4.0), frame(4.5))):
        for k in range(lo, hi):
            for ch in range(2):
                x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
                sig2[ch, k] += noise_amp * (x / 4294967296.0 * 2.0 - 1.0)
    s0, s1, nf = frame(3.0), frame(4.0), max(1, frame(0.005))
    k = np.arange(s1 - s0)
    fade = np.minimum(1.0, np.minimum(k + 1.0, (s1 - s0) - k) / nf)
    sig2[:, s0:s1] += amp(-20.0) * fade * np.sin(2 * np.pi * 1000.0 * k / sr)
    return sig2[:channels].astype(np.float32).astype(np.float64)


# ---------------------------------------------------------------------------- analyses

def mono(x, ch=0):
    x = np.atleast_2d(x)
    return x[min(ch, x.shape[0] - 1)]


def fundamental_residual(y, sr, f0, start=None, stop=None):
    """Least-squares fit of DC + the fundamental (cos, sin at f0) over [start, stop) s;
    returns residual RMS / fundamental RMS in percent. Counts every non-fundamental
    component, harmonic or not (unlike harmonic THD)."""
    y = mono(y)
    a = 0 if start is None else int(start * sr)
    b = len(y) if stop is None else int(stop * sr)
    seg = y[a:b]
    t = np.arange(a, a + len(seg)) / sr
    basis = np.vstack([np.ones_like(t), np.cos(2 * np.pi * f0 * t), np.sin(2 * np.pi * f0 * t)]).T
    coef, *_ = np.linalg.lstsq(basis, seg, rcond=None)
    resid = seg - basis @ coef
    fund_rms = np.hypot(coef[1], coef[2]) / np.sqrt(2.0)
    return 100.0 * np.sqrt(np.mean(resid ** 2)) / max(fund_rms, 1e-30)


def _spectrum(y, sr):
    y = mono(y)
    win = np.blackman(len(y))
    spec = np.abs(np.fft.rfft(y * win)) * 2.0 / np.sqrt(len(y) * np.sum(win ** 2))
    return np.fft.rfftfreq(len(y), 1.0 / sr), spec


def alias_spectrum(y, sr, f0, harmonics=40, lobe=4):
    """Splits a steady tone's spectrum into the true harmonics k*f0 below Nyquist and
    the aliases: images of harmonics above Nyquist folded back to |k*f0 - m*sr|.
    Returns dict(fund_db, harmonic_db={k: dB re fundamental}, worst_alias_db,
    alias_sum_db, inharmonic_db = everything else re fundamental)."""
    freqs, spec = _spectrum(y, sr)
    df = freqs[1]

    def level(f):
        i = int(round(f / df))
        return float(np.sqrt(np.sum(spec[max(0, i - lobe):i + lobe + 1] ** 2))), i

    fund, _ = level(f0)
    used = np.zeros(len(spec), bool)
    harm, aliases = {}, []
    for k in range(1, harmonics + 1):
        f = k * f0
        folded = abs(f - round(f / sr) * sr)
        lv, i = level(folded)
        used[max(0, i - lobe):i + lobe + 1] = True
        if f < sr / 2:
            harm[k] = db(lv / max(fund, 1e-30))
        else:
            aliases.append(lv)
    rest = np.sqrt(np.sum(spec[~used][1:] ** 2))
    alias_sum = np.sqrt(np.sum(np.square(aliases))) if aliases else 0.0
    return dict(fund_db=db(fund), harmonic_db=harm,
                worst_alias_db=db(max(aliases) / max(fund, 1e-30)) if aliases else -300.0,
                alias_sum_db=db(alias_sum / max(fund, 1e-30)), inharmonic_db=db(rest / max(fund, 1e-30)))


def swept_response(x, y, sr, points=48, fmin=20.0, fmax=20000.0):
    """[(freq, gain_db, phase_deg)] from any broadband in/out pair (sweep or noise)."""
    freqs, h, sxx = sig.welch_tf(mono(x), mono(y), sr)
    usable = sxx > sxx.max() * 1e-8
    rows = []
    for f in np.geomspace(fmin, min(fmax, sr / 2 * 0.95), points):
        i = int(np.argmin(np.abs(freqs - f)))
        if usable[i]:
            rows.append((float(freqs[i]), float(db(h[i])), float(np.degrees(np.angle(h[i])))))
    return rows


def settle_times(trace, sr, start, fractions=(0.5, 0.63, 0.9), final=None):
    """Seconds after `start` (s) for the trace to cover each fraction of its change from
    its value at start to `final` (default: its last value). Works on dB or linear."""
    tr = mono(trace)
    i0 = int(round(start * sr))
    v0 = tr[i0]
    v1 = tr[-1] if final is None else final
    out = {}
    for frac in fractions:
        target = v0 + frac * (v1 - v0)
        seg = tr[i0:]
        hit = np.nonzero(seg >= target)[0] if v1 >= v0 else np.nonzero(seg <= target)[0]
        out[frac] = hit[0] / sr if len(hit) else None
    return out


def first_crossing(trace, sr, start, threshold, below=True):
    """Seconds after `start` (s) to the first sample at or below (above) threshold."""
    tr = mono(trace)[int(round(start * sr)):]
    hit = np.nonzero(tr <= threshold)[0] if below else np.nonzero(tr >= threshold)[0]
    return hit[0] / sr if len(hit) else None


def analytic(y):
    """Analytic signal by FFT (numpy Hilbert transform)."""
    y = mono(y)
    n = len(y)
    spec = np.fft.fft(y)
    h = np.zeros(n)
    h[0] = 1.0
    if n % 2 == 0:
        h[n // 2] = 1.0
        h[1:n // 2] = 2.0
    else:
        h[1:(n + 1) // 2] = 2.0
    return np.fft.ifft(spec * h)


def pitch_track(y, sr, hop_ms=5.0):
    """Instantaneous frequency of a (near) single-tone signal, averaged per hop:
    returns (times_s, freqs_hz)."""
    z = analytic(y)
    inst = np.diff(np.unwrap(np.angle(z))) * sr / (2 * np.pi)
    hop = max(1, int(hop_ms * 1e-3 * sr))
    frames = len(inst) // hop
    f = inst[:frames * hop].reshape(frames, hop).mean(axis=1)
    return (np.arange(frames) + 0.5) * hop / sr, f


def envelope(y, sr, ms=1.0, analytic_mag=False):
    """Level envelope: RMS per `ms` window (dBFS, sine-calibrated) or, with analytic_mag,
    the per-sample analytic magnitude (linear)."""
    if analytic_mag:
        return np.abs(analytic(y))
    y = mono(y)
    n = max(1, int(sr * ms / 1000.0))
    frames = len(y) // n
    rms = np.sqrt(np.mean(y[:frames * n].reshape(frames, n) ** 2, axis=1))
    return db(rms) + 3.01


def correlation(a, b):
    a, b = mono(a), mono(b)
    a, b = a - a.mean(), b - b.mean()
    return float(np.sum(a * b) / max(np.sqrt(np.sum(a * a) * np.sum(b * b)), 1e-30))


def zipper_lines(y, sr, block, carrier_hz, skip_s=0.5, lobe=4):
    """Largest spectral line at a multiple of sr/block (the parameter update rate),
    in dB relative to the carrier, ignoring the carrier's own lobe."""
    y = mono(y)[int(skip_s * sr):]
    freqs, spec = _spectrum(y, sr)
    df = freqs[1]
    ic = int(round(carrier_hz / df))
    carrier = np.sqrt(np.sum(spec[max(0, ic - lobe):ic + lobe + 1] ** 2))
    worst = -300.0
    k = 1
    while k * sr / block < sr / 2:
        for f in (k * sr / block, abs(carrier_hz - k * sr / block), carrier_hz + k * sr / block):
            i = int(round(f / df))
            if abs(i - ic) <= lobe or i >= len(spec):
                continue
            worst = max(worst, db(np.sqrt(np.sum(spec[max(0, i - lobe):i + lobe + 1] ** 2)) / max(carrier, 1e-30)))
        k += 1
    return worst


def zipper_metric(y, sr, ms=2.0):
    """Short-time roughness: the largest RMS, in dB re the signal's RMS, of the
    second difference over `ms` windows (steps in a gain show up as spikes here)."""
    y = mono(y)
    d2 = np.diff(y, 2)
    n = max(1, int(sr * ms / 1000.0))
    frames = len(d2) // n
    st = np.sqrt(np.mean(d2[:frames * n].reshape(frames, n) ** 2, axis=1))
    return float(db(st.max() / max(np.sqrt(np.mean(y ** 2)), 1e-30)))


# ---------------------------------------------------------------------------- report

class Report:
    """Collects rows and prints one PASS/FAIL table. finish() -> exit status."""

    def __init__(self, effect):
        self.effect, self.rows = effect, []

    def add(self, check, scope, ok, detail=""):
        status = "PASS" if ok else "FAIL"
        self.rows.append((check, scope, status, detail))
        print(f"  {status}  {check:<22} {scope:<34} {detail}", flush=True)
        return ok

    def todo(self, check, detail):
        self.rows.append((check, "-", "TODO", detail))
        print(f"  TODO  {check:<22} {'-':<34} {detail}", flush=True)

    def finish(self):
        fails = sum(1 for r in self.rows if r[2] == "FAIL")
        todo = sum(1 for r in self.rows if r[2] == "TODO")
        print(f"\n{self.effect}: {len(self.rows)} checks, {fails} FAIL, {todo} TODO")
        print(f"{'check':<24}{'scope':<36}{'result':<8}detail")
        for check, scope, status, detail in self.rows:
            print(f"{check:<24}{scope:<36}{status:<8}{detail}")
        return 1 if fails else 0


def parse_args(description=""):
    ap = argparse.ArgumentParser(description=description)
    ap.add_argument("--quick", action="store_true",
                    help="48 kHz stereo, blocks 37 and 512 only (the full matrix is the acceptance run)")
    ap.add_argument("--types", type=lambda s: [int(v) for v in s.split(",")], default=None,
                    help="comma-separated type indices to check (default: all)")
    return ap.parse_args()


# ---------------------------------------------------------------------------- §2.11

def _bench(effect, inp, keys, extra=(), sr=48000):
    r = invoke(effect, inp, keys, ["--bench"] + list(extra), sr)
    return r.number(r"([0-9.]+) % of real time")


def common_checks(effect, types, ranges=None, budgets=None, variants=None, report=None, quick=False,
                  type_key="type", null_keys=None, only=None):
    """The checks every engine passes first (docs/FX-ENGINES.md §2.11).

    types     {index: name}; index 0 is Classic.
    ranges    {key: (min, max)} of the float keys, for the finite-output torture test.
    budgets   {index: percent of real time} (--bench); None for an index means "Classic
              as today": at most 1.2 x the legacy reference's own figure.
    variants  {index: [dict of extra keys, ...]} sub-choices to run each check under
              (e.g. the phaser's color, the EQ Stack's voices); default one empty dict.
    type_key  the effect's type key ("type", "mode", "model", "voice").
    null_keys [dict, ...] settings the Classic null runs at (default: the defaults only).
    only      type indices to check (default: all).
    """
    report = report or Report(effect)
    ranges, budgets, variants = ranges or {}, budgets or {}, variants or {}
    rates = [48000] if quick else RATES
    chans = [2] if quick else [1, 2]
    blocks = [37, 512] if quick else BLOCKS

    for t, name in types.items():
        if only is not None and t not in only:
            continue
        for extra in variants.get(t, [{}]):
            keys = {type_key: t, **extra}
            tag = name + ("".join(f" {k}={v}" for k, v in extra.items()))
            new_type = t != 0

            finite_ok, finite_detail = True, ""
            silence_ok, silence_detail = True, ""
            reset_ok, reset_detail = True, ""
            rt_ok, rt_detail = True, ""
            for sr in rates:
                x = extreme(sr, 4.0 if quick else 20.0)
                for ch in chans:
                    inp = np.vstack([x] * ch)
                    zeros = np.zeros((ch, 2 * sr))
                    for bs in blocks:
                        where = f"{sr} Hz {ch} ch block {bs}"
                        for end in (0, 1):
                            k = dict(keys, **{key: lim[end] for key, lim in ranges.items()})
                            out = invoke(effect, inp, k, [f"--block={bs}", f"--channels={ch}"], sr).out
                            if not np.all(np.isfinite(out)) or np.max(np.abs(out)) >= 1e3:
                                finite_ok, finite_detail = False, f"{where}, keys at {'max' if end else 'min'}"
                        out = invoke(effect, zeros, keys, [f"--block={bs}"], sr).out
                        if np.max(np.abs(out)) >= 1e-9:
                            silence_ok, silence_detail = False, f"{where}: max {np.max(np.abs(out)):.3g}"
                        r = invoke(effect, "gen:music", keys, [f"--block={bs}", f"--channels={ch}", "--reset-repeat"], sr)
                        if r.code != 0:
                            if t == 0 and invoke(effect, "gen:music", keys, [f"--block={bs}", f"--channels={ch}",
                                                                             "--reset-repeat", "--legacy"], sr).code != 0:
                                reset_detail = "the frozen legacy reset is not deterministic either"
                            else:
                                reset_ok, reset_detail = False, f"{where}: {r.stdout.strip()}"
                        r = invoke(effect, "gen:music", keys, [f"--block={bs}", f"--channels={ch}", "--rt-check"], sr)
                        if r.code != 0:
                            rt_ok, rt_detail = False, f"{where}: {r.stdout.strip()}"
            report.add("finite", tag, finite_ok, finite_detail)
            report.add("silence", tag, silence_ok, silence_detail)
            report.add("reset determinism", tag, reset_ok, reset_detail)
            report.add("rt safety", tag, rt_ok, rt_detail)

            if new_type:
                ok, detail = True, ""
                for sr in rates:
                    for ch in chans:
                        ref = invoke(effect, "gen:music", keys, ["--block=512", f"--channels={ch}"], sr).out
                        for bs in (1, 37):
                            out = invoke(effect, "gen:music", keys, [f"--block={bs}", f"--channels={ch}"], sr).out
                            err = float(np.max(np.abs(out - ref)))
                            if err > 1e-6:
                                ok, detail = False, f"{sr} Hz {ch} ch block {bs}: max |diff| {err:.3g}"
                report.add("block independence", tag, ok, detail)
            else:
                for settings in null_keys or [{}]:
                    ok, detail = classic_null(effect, dict(extra, **settings))
                    scope = tag + "".join(f" {k}={v}" for k, v in settings.items())
                    report.add("classic null", scope, ok, detail)

            decay = np.hstack([music(48000)[:, :48000], np.zeros((2, 30 * 48000))])
            ftz = _bench(effect, decay, keys)
            noftz = _bench(effect, decay, keys, ["--no-ftz"])
            if new_type:
                report.add("denormals", tag, noftz <= 1.5 * max(ftz, 1e-4),
                           f"no-FTZ {noftz:.4f} % vs FTZ {ftz:.4f} %")
            else:
                report.add("denormals", tag, True, f"Classic runs under the processor's FTZ (no-FTZ {noftz:.4f} %)")

            cpu = _bench(effect, "gen:noise:pink:-18", keys)
            budget = budgets.get(t)
            if budget is None:
                ref = _bench(effect, "gen:noise:pink:-18", keys, ["--legacy"])
                report.add("cpu", tag, cpu <= 1.2 * ref + 1e-3, f"{cpu:.4f} % (legacy {ref:.4f} %, limit 1.2x)")
            else:
                report.add("cpu", tag, cpu <= budget, f"{cpu:.4f} % (budget {budget} %)")
    return report


def classic_null(effect, keys=None, opts=None, rates=NULL_RATES, blocks=NULL_BLOCKS, inp="gen:music"):
    """--classic-null over rates x blocks; -> (ok, detail)."""
    for sr in rates:
        for bs in blocks:
            r = invoke(effect, inp, keys, [f"--block={bs}", "--classic-null"] + list(opts or []), sr)
            if r.code != 0:
                return False, f"{sr} Hz block {bs}: {r.stdout.strip()}"
    return True, ""
