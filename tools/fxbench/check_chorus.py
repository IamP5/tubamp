#!/usr/bin/env python3
"""Acceptance for the chorus engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.5.7). Prints a PASS/FAIL table; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_chorus.py [--quick] [--types 1,2] [--measure]

--measure also prints the characterisation tables that are not pass/fail: peak pitch
deviation per mode, rate and depth; interpolation quality; stereo width and mono
compatibility; the dry/wet law; zipper noise under automation; the delay-range limits.

Pitch is the instantaneous frequency of a steady 1 kHz sine through one soloed voice at
mix 1 (analytic signal by FFT Hilbert), 95th percentile of |deviation| over >= 3 LFO
cycles (§4.5.7; the study's click method reads up to 1.2 cents high, V:12). Delay
trajectories come from the same analytic phase: d(t) = -(phase(t) - 2 pi f0 t) / (2 pi f0),
made absolute against a depth-0 render whose delay is the mode's base delay.
"""
import sys
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxcheck as fx  # noqa: E402

EFFECT = "chorus"
TYPE_KEY = "mode"
TYPES = {0: "Classic", 1: "Wide", 2: "CE", 3: "Dimension", 4: "Tri"}
# Float keys at their documented limits, for the finite-output test.
RANGES = {"rate": (0.01, 20.0), "depth": (0.0, 1.0), "mix": (0.0, 1.0), "width": (0.0, 1.0), "tone": (0.0, 1.0)}
# Percent of real time per instance (§2.10); None = Classic as today (<= 1.2 x legacy).
BUDGETS = {0: None, 1: 0.5, 2: 0.25, 3: 0.25, 4: 0.4}
VARIANTS = {}
NULL_KEYS = [{}, {"rate": 10, "depth": 1, "mix": 1}, {"rate": 0.05, "depth": 0.1, "mix": 0.5}]

SR = 48000
PROBE_HZ = 1000.0
WIDE, CE, DIM, TRI = 1, 2, 3, 4
# Per-mode starting points (Chorus.h kChorus*Default, §4.5.2).
START = {1: dict(rate=1.0, depth=0.4, mix=0.35), 2: dict(rate=0.8, depth=0.7, mix=0.5),
         3: dict(rate=0.25, depth=0.8, mix=0.5), 4: dict(rate=0.6, depth=0.5, mix=0.5)}
BASE_S = {1: 0.006, 2: 0.00295, 3: 0.0085, 4: 0.007}   # voice 0's base delay (§4.5.5)
D_MAX = 0.020
CE_SWEEP, CE_BEND, CE_TOP = 0.0027, 0.1, 2.86
DIM_SWING_PP, DIM_TOP = 0.005, 0.5
# Wet L/R correlation implied by each mode's voice routing: Wide, CE and Dimension feed the
# two sides from different delays (uncorrelated on band noise); Tri shares voice 2.
WET_CORR = {1: 0.0, 2: 0.0, 3: 0.0, 4: 0.5}
SKIP_S, TAIL_S = 0.3, 0.2


# ---------------------------------------------------------------------------- probes

def run(keys, inp, sr=SR, secs=None, ch=2, block=512):
    opts = [f"--channels={ch}", f"--block={block}"]
    if secs is not None:
        opts.append(f"--seconds={secs}")
    return fx.invoke(EFFECT, inp, keys, opts, sr).out


def probe(keys, secs, sr=SR, freq=PROBE_HZ, ch=2, block=512):
    """A steady sine through the engine; -> channel 0 (a soloed voice is on both)."""
    return fx.mono(run(keys, f"gen:sine:{freq}:-12", sr, secs, ch, block))


def secs_for(rate, cycles=3.0, minimum=1.0):
    return SKIP_S + max(cycles / rate, minimum) + TAIL_S + 0.05


def analytic_phase(y, sr):
    z = fx.analytic(y)
    a, b = int(SKIP_S * sr), len(z) - int(TAIL_S * sr)
    return np.arange(a, b) / sr, np.unwrap(np.angle(z))[a:b]


def hop_mean(x, hop):
    n = len(x) // hop
    return x[:n * hop].reshape(n, hop).mean(axis=1)


def cents_track(y, sr, freq=PROBE_HZ, hop_ms=2.0):
    """-> (times, cents) of the instantaneous frequency, averaged per hop."""
    t, ph = analytic_phase(y, sr)
    inst = np.diff(ph) * sr / (2 * np.pi)
    hop = max(1, int(hop_ms * 1e-3 * sr))
    return hop_mean(t[1:], hop), 1200.0 * np.log2(np.maximum(hop_mean(inst, hop), 1e-9) / freq)


def delay_track(y, sr, freq=PROBE_HZ, hop_ms=1.0):
    """-> (times, delay seconds up to a constant)."""
    t, ph = analytic_phase(y, sr)
    raw = -(ph - 2 * np.pi * freq * t) / (2 * np.pi * freq)
    hop = max(1, int(hop_ms * 1e-3 * sr))
    return hop_mean(t, hop), hop_mean(raw, hop)


def whole_cycles(t, x, f):
    n = int(np.floor((t[-1] - t[0]) * f))
    keep = t <= t[0] + n / f
    return t[keep], x[keep]


def fund_phase_deg(t, d, f):
    """Phase phi of the fundamental of d(t), written as -cos(2 pi f t + phi): an LFO
    shape u = shape(f t + offset) with u = 0 at phase 0 gives phi = 360 * offset."""
    t, d = whole_cycles(t, d, f)
    z = np.sum((d - d.mean()) * np.exp(-2j * np.pi * f * t))
    return float(np.degrees(np.angle(-z)))


def wrap_deg(x):
    return (x + 180.0) % 360.0 - 180.0


def pitch_p95(mode, rate, depth, sr=SR, solo=0, ch=2, extra=None):
    keys = dict(mode=mode, rate=rate, depth=depth, mix=1, solo=solo, **(extra or {}))
    _, c = cents_track(probe(keys, secs_for(rate), sr, ch=ch), sr)
    return float(np.percentile(np.abs(c), 95))


def ce_sweep(rate, sr=SR):
    """CE at depth 1: (min ms, max ms, crest) of voice 0's delay trajectory."""
    secs = secs_for(rate, 3.0, 2.0)
    keys = dict(mode=CE, rate=rate, depth=1, mix=1, solo=0)
    t, raw = delay_track(probe(keys, secs, sr), sr)
    _, ref = delay_track(probe(dict(keys, depth=0), secs, sr), sr)
    shift = raw - ref.mean()
    # The trajectory's mean sits within 0.05 ms of the base delay, well inside one probe period.
    period = 1.0 / PROBE_HZ
    shift -= period * np.round(np.mean(shift) / period)
    t, d = whole_cycles(t, BASE_S[CE] + shift, rate)
    dev = d - d.mean()
    return 1e3 * d.min(), 1e3 * d.max(), float(np.max(np.abs(dev)) / np.sqrt(np.mean(dev ** 2)))


def band_noise(secs, sr, lo=150.0, hi=4000.0, level_db=-18.0, seed=7):
    """Mono band-limited noise (FFT brick wall), the study's width probe."""
    n = int(round(secs * sr))
    spec = np.fft.rfft(np.random.default_rng(seed).standard_normal(n))
    f = np.fft.rfftfreq(n, 1.0 / sr)
    spec[(f < lo) | (f > hi)] = 0.0
    x = np.fft.irfft(spec, n)
    return x / np.sqrt(np.mean(x ** 2)) * fx.amp(level_db)


def spectra(x, y, sr, nfft=16384):
    """Long-term power spectra (Hann, 50 % overlap): -> (freqs, Sxx, Syy)."""
    win = np.hanning(nfft)
    sxx = np.zeros(nfft // 2 + 1)
    syy = np.zeros(nfft // 2 + 1)
    for start in range(0, len(x) - nfft + 1, nfft // 2):
        sxx += np.abs(np.fft.rfft(x[start:start + nfft] * win)) ** 2
        syy += np.abs(np.fft.rfft(y[start:start + nfft] * win)) ** 2
    return np.fft.rfftfreq(nfft, 1.0 / sr), sxx, syy


def band_db(freqs, sxx, syy, fc, half_oct):
    m = (freqs >= fc * 2 ** -half_oct) & (freqs <= fc * 2 ** half_oct)
    return float(10 * np.log10(np.sum(syy[m]) / max(np.sum(sxx[m]), 1e-30)))


def wet_response(keys, sr=SR, secs=8.0):
    x = fx.white(secs, -18.0, sr, seed=3)
    y = fx.mono(run(dict(keys, mix=1, solo=0), fx.stereo(x), sr))
    a = int(0.5 * sr)
    return spectra(x[a:], y[a:], sr)


def minus3_point(freqs, sxx, syy, sr):
    """-3 dB corner of the wet low-pass: first 1/24-octave band above 1 kHz that is 3 dB
    under the passband (the loudest band between 250 Hz and 1 kHz)."""
    ref = max(band_db(freqs, sxx, syy, f, 1 / 48) for f in np.geomspace(250, 1000, 24))
    grid = np.geomspace(1000.0, 0.47 * sr, 600)
    lv = np.array([band_db(freqs, sxx, syy, f, 1 / 48) for f in grid]) - ref
    below = np.nonzero(lv <= -3.0)[0]
    if len(below) == 0 or below[0] == 0:
        return None
    i = below[0]
    w = (-3.0 - lv[i - 1]) / (lv[i] - lv[i - 1])
    return float(np.exp(np.log(grid[i - 1]) + w * (np.log(grid[i]) - np.log(grid[i - 1]))))


def hp_residual(y, sr, fc=2000.0, order=8):
    """Zero-phase 8th-order Butterworth-magnitude high-pass by FFT."""
    spec = np.fft.rfft(y)
    f = np.fft.rfftfreq(len(y), 1.0 / sr)
    h = np.zeros_like(f)
    h[1:] = 1.0 / np.sqrt(1.0 + (fc / f[1:]) ** (2 * order))
    return np.fft.irfft(spec * h, len(y))


def width_stats(mode, keys, sr=SR, secs=8.0):
    """Mono band noise in, -> (L/R correlation, mono loss dB = (L+R)/2 against L)."""
    x = band_noise(secs, sr)
    out = run(dict(keys, mode=mode), fx.stereo(x), sr)
    a = int(0.5 * sr)
    left, right = out[0, a:], out[1, a:]
    mid = 0.5 * (left + right)
    rms = lambda v: np.sqrt(np.mean(v ** 2))  # noqa: E731
    return fx.correlation(left, right), float(fx.db(rms(mid) / rms(left))), float(fx.db(rms(right) / rms(left)))


def predicted_mono_loss(mode, mix, width):
    """(L+R)/2 against L, in dB, for dry d and wet w = the mix law, unit-power wet sides of
    correlation rho that are uncorrelated with the dry, and the width's side gain:
    L = d x + w (M + width S), mono = d x + w M."""
    d, w, rho = min(1.0, 2.0 - 2.0 * mix), min(1.0, 2.0 * mix), WET_CORR[mode]
    mid = d * d + w * w * (1 + rho) / 2
    side = w * w * width * width * (1 - rho) / 2
    return float(10 * np.log10(mid / (mid + side)))


def mono_bound(mode, keys, sr=SR):
    """-> (ok, measured loss, predicted loss): the loss may not exceed the decorrelated-wet
    prediction by more than 0.1 dB."""
    _, loss, _ = width_stats(mode, keys, sr)
    pred = predicted_mono_loss(mode, keys["mix"], keys["width"])
    return loss >= pred - 0.1, loss, pred


# ---------------------------------------------------------------------------- model values

def cents(ratio_minus_one):
    return 1200.0 * np.log2(1.0 + ratio_minus_one)


def model_cents(mode, rate, depth):
    if mode == WIDE:
        k = 2 ** (30 * depth * depth / 1200) - 1
        return cents(2 * rate * min(k / (2 * rate), D_MAX))
    if mode == CE:
        return cents(2 * rate * CE_SWEEP * depth * min(1.0, CE_TOP / rate) * (1 + CE_BEND))
    if mode == DIM:
        return cents(2 * rate * DIM_SWING_PP * depth * min(1.0, DIM_TOP / rate))
    k = 2 ** (20 * depth * depth / 1200) - 1
    return cents(np.pi * rate * min(k / (np.pi * rate), D_MAX)) + 3.0   # slow + the 3-cent shimmer, at most


# ---------------------------------------------------------------------------- acceptance rows

def ch2(sr=SR):
    got = [pitch_p95(WIDE, r, 0.5, sr) for r in (0.25, 1.0, 3.0)]
    return all(abs(c - 7.5) <= 1.5 for c in got), "cents p95 at 0.25/1/3 Hz: " + " / ".join(f"{c:.2f}" for c in got)


def ch3():
    got = {r: pitch_p95(WIDE, r, 1.0) for r in (0.25, 0.5, 1.0, 3.0)}
    limit = model_cents(WIDE, 0.25, 1.0)
    ok = all(abs(got[r] - 30.0) <= 2.0 for r in (0.5, 1.0, 3.0)) and abs(got[0.25] - limit) <= 1.5
    return ok, ("cents p95 at 0.5/1/3 Hz: " + " / ".join(f"{got[r]:.2f}" for r in (0.5, 1.0, 3.0))
                + f"; 0.25 Hz {got[0.25]:.2f} (limit {limit:.2f})")


def ch4():
    secs = secs_for(1.0, 4.0)
    keys = dict(mode=WIDE, rate=1.0, depth=0.5, mix=1)
    phases = []
    for solo in (0, 1):
        t, d = delay_track(probe(dict(keys, solo=solo), secs), SR)
        phases.append(fund_phase_deg(t, d, 1.0))
    dphi = wrap_deg(phases[1] - phases[0])
    c1, _, _ = width_stats(WIDE, dict(START[WIDE], width=1.0, mix=0.5))
    c0, _, _ = width_stats(WIDE, dict(START[WIDE], width=0.0, mix=0.5))
    # Mono sum: decorrelated wet sides lose a predictable amount in (L+R)/2 (coordinator
    # decision: a property, bounded here); checked at mix 0.5 (dry = wet) and the start mix.
    ok50, loss50, pred50 = mono_bound(WIDE, dict(START[WIDE], width=1.0, mix=0.5))
    ok35, loss35, pred35 = mono_bound(WIDE, dict(START[WIDE], width=1.0))
    ok = abs(dphi - 90.0) <= 10.0 and c1 < 0.93 and c0 >= 0.9995 and ok50 and ok35
    return ok, (f"R-L {dphi:+.1f} deg; corr {c1:.3f} at width 1, {c0:.4f} at width 0; mono sum "
                f"{loss50:+.2f} dB (bound {pred50:+.2f}) at mix 0.5, {loss35:+.2f} (bound {pred35:+.2f}) at mix 0.35")


def ch5(sr=SR):
    rows, ok = [], True
    for r in (0.43, 1.847, 2.86):
        lo, hi, crest = ce_sweep(r, sr)
        ok &= abs(lo - 1.6) <= 0.15 and abs(hi - 4.3) <= 0.15 and 1.6 <= crest <= 1.8
        rows.append(f"{r:g} Hz {lo:.3f}-{hi:.3f} ms crest {crest:.2f}")
    return ok, "; ".join(rows)


def ch6(sr=SR):
    slow = pitch_p95(CE, 0.43, 1.0, sr)
    top = pitch_p95(CE, 2.86, 1.0, sr)
    fast = {r: pitch_p95(CE, r, 1.0, sr) for r in (3.0, 4.0, 5.0, 7.0, 10.0, 14.0, 20.0)}
    worst = max(fast.values())
    ok = 4.0 <= slow <= 6.0 and 25.0 <= top <= 31.0 and max(worst, top, slow) <= 31.0
    return ok, (f"{slow:.2f} cents at 0.43 Hz; {top:.2f} at 2.86 Hz; max {worst:.2f} over 3-20 Hz "
                f"({', '.join(f'{r:g}:{c:.1f}' for r, c in fast.items())})")


def ch7():
    path = Path(__file__).resolve().parents[2] / "docs/research/fx/chorus/evidence/brigade_fr.tsv"
    ref, section = [], False
    for line in path.read_text().splitlines():
        if line.startswith("# freq_hz"):
            section = True
            continue
        if section and line and not line.startswith("#"):
            f, _, wet = (float(v) for v in line.split("\t"))
            if 100.0 <= f <= 8000.0:
                ref.append((f, wet))
    freqs, sxx, syy = wet_response(dict(mode=CE, rate=1.847, depth=1.0))
    ours = np.array([band_db(freqs, sxx, syy, f, 1 / 6) for f, _ in ref])
    theirs = np.array([w for _, w in ref])
    diff = ours - theirs
    diff -= diff.mean()
    i = int(np.argmax(np.abs(diff)))
    return bool(np.max(np.abs(diff)) <= 2.0), (f"max |diff| {np.max(np.abs(diff)):.2f} dB at {ref[i][0]:.0f} Hz "
                                                f"over {len(ref)} bands 100 Hz-8 kHz")


def ch8(sr=SR):
    secs = secs_for(0.25, 3.0)
    keys = dict(mode=DIM, rate=0.25, depth=0.8, mix=1)
    phases = []
    for solo in (0, 1):
        t, d = delay_track(probe(dict(keys, solo=solo), secs, sr), sr)
        phases.append(fund_phase_deg(t, d, 0.25))
    dphi = wrap_deg(phases[1] - phases[0])
    a = pitch_p95(DIM, 0.25, 0.8, sr)
    b = pitch_p95(DIM, 0.5, 0.6, sr)
    mono_ok, loss, pred = mono_bound(DIM, dict(START[DIM], width=1.0), sr)
    ok = abs(abs(dphi) - 180.0) <= 10.0 and 3.0 <= a <= 4.0 and 4.7 <= b <= 5.7 and mono_ok
    return ok, (f"R-L {dphi:+.1f} deg; {a:.2f} cents at 0.25 Hz/0.8; {b:.2f} at 0.5 Hz/0.6; "
                f"mono sum {loss:+.2f} dB (bound {pred:+.2f})")


def ch8_rates():
    """Dimension's detune stays at its 0.5 Hz value at every faster rate, sync range included."""
    ref = pitch_p95(DIM, 0.5, 1.0)
    got = {r: pitch_p95(DIM, r, 1.0) for r in (1.0, 2.0, 3.0, 5.0, 10.0, 16.0, 20.0)}
    worst = max(got.values())
    return worst <= ref + 0.2, (f"depth 1: {ref:.2f} cents at 0.5 Hz; max {worst:.2f} over 1-20 Hz "
                                f"({', '.join(f'{r:g}:{c:.2f}' for r, c in got.items())})")


def ch_width_remedy():
    """The mono-sum remedy: at width 0.8 or below the loss is within 1 dB at mix 0.5."""
    rows, ok = [], True
    for mode in (WIDE, CE, DIM, TRI):
        for width in (0.8, 0.5, 0.0):
            _, loss, _ = width_stats(mode, dict(START[mode], mix=0.5, width=width))
            ok &= abs(loss) <= 1.0
            rows.append(f"{TYPES[mode]} w{width:g} {loss:+.2f}")
    return ok, "mono sum dB at mix 0.5: " + ", ".join(rows)


def ch9():
    rate, depth = START[TRI]["rate"], START[TRI]["depth"]
    # Three taps: an impulse every 50 ms through the full wet (mix 1); a tap is a peak of
    # at least a quarter of the window's largest, 0.15 ms or more from the next.
    period, secs = int(0.05 * SR), 6.0
    x = np.zeros(int(secs * SR))
    x[period::period] = 0.5
    out = run(dict(mode=TRI, rate=rate, depth=depth, mix=1, width=1), fx.stereo(x))
    lo, hi, sep = int(0.004 * SR), int(0.016 * SR), 0.15e-3 * SR
    counts = []
    for k in range(period * 4, len(x) - period, period):
        taps = []
        for chan in out:
            seg = np.abs(chan[k + lo:k + hi])
            peaks = [i for i in range(1, len(seg) - 1) if seg[i] >= seg[i - 1] and seg[i] > seg[i + 1]
                     and seg[i] >= 0.25 * seg.max()]
            taps += peaks
        merged = []
        for p in sorted(taps):
            if not merged or p - merged[-1] > sep:
                merged.append(p)
        counts.append(len(merged))
    three = float(np.mean(np.array(counts) == 3))
    # Slow phases and the 6 Hz shimmer, from each soloed voice.
    secs_p = secs_for(rate, 4.0)
    phases = []
    for solo in (0, 1, 2):
        t, d = delay_track(probe(dict(mode=TRI, rate=rate, depth=depth, mix=1, solo=solo), secs_p), SR)
        phases.append(fund_phase_deg(t, d, rate))
    gaps = [wrap_deg(phases[1] - phases[0]), wrap_deg(phases[2] - phases[1])]
    t, c = cents_track(probe(dict(mode=TRI, rate=rate, depth=depth, mix=1, solo=0), secs_p), SR)
    basis = [np.ones_like(t)]
    for f in (rate, 2 * rate, 6.0):
        basis += [np.cos(2 * np.pi * f * t), np.sin(2 * np.pi * f * t)]
    coef, *_ = np.linalg.lstsq(np.vstack(basis).T, c, rcond=None)
    fast = float(np.hypot(coef[5], coef[6]))
    ok = three >= 0.5 and all(abs(g - 120.0) <= 15.0 for g in gaps) and 2.0 <= fast <= 4.0
    return ok, (f"3 distinct taps in {100 * three:.0f} % of impulses; slow gaps {gaps[0]:+.1f}/{gaps[1]:+.1f} deg; "
                f"6 Hz component {fast:.2f} cents peak")


def ch10():
    rows, ok = [], True
    for mode in (WIDE, CE, DIM, TRI):
        corner = {}
        for tone in (0.0, 0.5, 1.0):
            corner[tone] = minus3_point(*wet_response(dict(mode=mode, depth=0, tone=tone)), SR)
        if None in corner.values():
            ok = False
            rows.append(f"{TYPES[mode]} no -3 dB point")
            continue
        down = np.log2(corner[0.0] / corner[0.5])
        up = np.log2(corner[1.0] / corner[0.5])
        ok &= abs(down + 1.0) <= 0.1 and abs(up - 1.0) <= 0.1
        rows.append(f"{TYPES[mode]} {corner[0.5] / 1e3:.2f} kHz {down:+.2f}/{up:+.2f} oct")
    return ok, "; ".join(rows)


def ch11():
    rows, ok = [], True
    for mode in (WIDE, CE, DIM, TRI):
        nxt = mode % 4 + 1
        keys = [f"mode={mode}", f"mode@1={nxt}", f"mode@2={mode}", "mode@3=0", f"mode@4={mode}",
                "rate@5=5", "rate@6=0.2", "depth@7=1", "depth@8=0.1", "rate@9=10", "depth@10=0.6"]
        out = run(keys, "gen:sine:220:-12", SR, 11.5)
        worst = -300.0
        for chan in out:
            r = hp_residual(chan, SR)[int(0.5 * SR):int(11.3 * SR)]
            worst = max(worst, float(fx.db(np.max(np.abs(r)))))
        ok &= worst <= -60.0
        rows.append(f"{TYPES[mode]} {worst:.1f}")
    return ok, "dBFS peak after 2 kHz HP: " + ", ".join(rows)


def ch12():
    secs = 4.0
    y = probe(dict(mode=WIDE, rate=1.0, depth=0.3, tone=1.0, mix=1, solo=0), secs, freq=10000.0)
    env = np.abs(fx.analytic(y))[int(0.5 * SR):int((secs - 0.3) * SR)]
    env = hop_mean(env, int(0.0005 * SR))
    ripple = float(fx.db(env.max() / env.min()))
    return ripple <= 0.7, f"10 kHz amplitude ripple {ripple:.2f} dB (Catmull-Rom 0.53, linear 2.0)"


def ch13():
    # 120 BPM, 1/4: one LFO cycle per beat, 0.5 s. The host is 0.3 beats into the bar at
    # t = 0, so a free-running LFO (phase 0 at t = 0) would be 0.15 s off the grid.
    secs, ppq0, f = 10.0, 0.3, 2.0
    keys = [f"mode={WIDE}", "rate=2", "depth=0.5", "mix=1", "solo=0", f"sync_phase@0..{secs}={ppq0}..{ppq0 + f * secs}"]
    out = fx.mono(run(keys, f"gen:sine:{PROBE_HZ}:-12", SR, secs))
    t, d = delay_track(out, SR)
    seg_t, seg_phi = [], []
    for s0 in np.arange(t[0], t[-1] - 1.0, 1.0):
        m = (t >= s0) & (t < s0 + 1.0)
        seg_t.append(s0 + 0.5)
        seg_phi.append(fund_phase_deg(t[m], d[m], f))
    unwrapped = np.degrees(np.unwrap(np.radians(seg_phi)))
    slope = np.polyfit(seg_t, unwrapped, 1)[0]            # deg/s of drift against 2 Hz
    period_err = -slope / (360.0 * f)                     # relative error of the period
    phi = fund_phase_deg(t, d, f)
    err_cycles = wrap_deg(phi - 360.0 * ppq0) / 360.0
    err_ms = 1e3 * err_cycles / f
    ok = abs(period_err) <= 1e-3 and abs(err_ms) <= 1.0
    return ok, f"period {1e3 / f * (1 + period_err):.3f} ms ({100 * period_err:+.4f} %); phase 0 lands {err_ms:+.3f} ms from the beat"


def ch14():
    rows, ok = [], True
    for sr in (44100, 96000):
        for name, fn in (("CH2", ch2), ("CH5", ch5), ("CH6", ch6), ("CH8", ch8)):
            good, detail = fn(sr)
            ok &= good
            rows.append((sr, name, good, detail))
    return ok, rows


def ch_mono():
    """A mono instance: its output equals the mean of the stereo outputs for the same mono
    input, and it still carries the moving wet. Dimension is the exception: its two lines
    move in antiphase, so their mean has no detune (CH19), and a mono instance plays line A
    alone, which is the stereo left channel."""
    rows, ok = [], True
    x = band_noise(4.0, SR)
    for mode in (WIDE, CE, DIM, TRI):
        keys = dict(START[mode], mode=mode)
        st = run(keys, fx.stereo(x), SR)
        mo = fx.mono(run(keys, x[None, :], SR, ch=1))
        want, name = (st[0], "left") if mode == DIM else (0.5 * (st[0] + st[1]), "mid")
        err = float(np.max(np.abs(mo - want)))
        wet = float(fx.db(np.sqrt(np.mean((mo - x) ** 2)) / np.sqrt(np.mean(x ** 2))))
        ok &= err <= 1e-6 and wet > -20.0
        rows.append(f"{TYPES[mode]} |mono-{name}| {err:.1e}, wet {wet:+.1f} dB")
    return ok, "; ".join(rows)


# ---------------------------------------------------------------------------- transitions
# A voice's pitch is the slope of its delay, so any transition is judged on the
# instantaneous frequency of a steady sine through one soloed voice: at every moment where
# the voice is sounding (analytic amplitude >= half its steady value; inside a dip the
# frequency of a silent path is undefined, and the dip is reported), |deviation| may not
# exceed the largest steady-state peak of the settings involved by more than 5 cents, and
# it must be back inside the new settings' own peak + 5 cents 250 ms after the change.

TR_CARRIERS = (997.3, 3301.7)
TR_TOL, TR_SETTLE = 5.0, 0.25
TR_BLOCK = 64
VOICES = {WIDE: (0, 2), CE: (0,), DIM: (0,), TRI: (0,)}
TRI_FAST = 2 ** (3 / 1200) - 1


def voice_slope(mode, voice, rate, depth):
    """Peak delay slope (s/s) of a voice in steady state at (rate, depth) (§4.5.5)."""
    rate = min(max(rate, 0.01), 20.0)
    if mode == WIDE:
        f = rate * (np.sqrt(2.0) if voice in (2, 3) else 1.0)
        return min(2 ** (30 * depth * depth / 1200) - 1, 2 * f * D_MAX)
    if mode == CE:
        return 2 * (1 + CE_BEND) * rate * CE_SWEEP * depth * min(1.0, CE_TOP / rate)
    if mode == DIM:
        return 2 * rate * DIM_SWING_PP * depth * min(1.0, DIM_TOP / rate)
    return min(2 ** (20 * depth * depth / 1200) - 1, np.pi * rate * D_MAX) + TRI_FAST


def slope_cents(s):
    """The larger side of a delay slope s: a rising delay bends down by -1200 log2(1 - s)."""
    return float(-1200.0 * np.log2(1.0 - s))


def if_cents(y, sr, f0, smooth_ms=2.0):
    """-> (cents per sample, analytic amplitude, valid mask). Valid: amplitude >= half the
    median, kept clear of the 2 ms average's reach into an amplitude null."""
    z = fx.analytic(y)
    amp_ = np.abs(z)
    inst = np.diff(np.unwrap(np.angle(z))) * sr / (2 * np.pi)
    k = max(1, int(smooth_ms * 1e-3 * sr))
    inst = np.convolve(inst, np.ones(k) / k, "same")
    cents_ = 1200.0 * np.log2(np.maximum(inst, 1e-3) / f0)
    low = amp_[1:] < 0.5 * np.median(amp_)
    guard = np.convolve(low.astype(float), np.ones(2 * k + 1), "same") > 0
    return cents_, amp_[1:], ~guard


def transition(mode, voice, f0, keys, events, secs, sr=SR, ch=2, block=TR_BLOCK):
    """One soloed voice through a timeline; -> (output, cents, valid, index of the first
    analysed sample)."""
    tokens = [f"{k}={v}" for k, v in dict(keys, mode=mode, mix=1, solo=voice).items()] + list(events)
    y = fx.mono(run(tokens, f"gen:sine:{f0}:-12", sr, secs, ch, block))
    c, _, valid = if_cents(y, sr, f0)
    a, b = int(0.4 * sr), len(c) - int(0.2 * sr)
    return y, c[a:b], valid[a:b], a


_STEADY = {}


def steady_peak(mode, voice, f0, rate, depth, sr):
    """Largest |deviation| of 2.5 s of steady state (the interpolation adds up to a cent
    to the closed form at 22.05 kHz and 3.3 kHz)."""
    key = (mode, voice, f0, round(rate, 6), round(depth, 6), sr)
    if key not in _STEADY:
        y = fx.mono(run([f"mode={mode}", f"rate={rate}", f"depth={depth}", "mix=1", f"solo={voice}"],
                        f"gen:sine:{f0}:-12", sr, 2.9, 2, 512))
        c, _, valid = if_cents(y, sr, f0)
        a, b = int(0.4 * sr), len(c) - int(0.2 * sr)
        _STEADY[key] = float(np.max(np.abs(c[a:b][valid[a:b]])))
    return _STEADY[key]


def tr_case(mode, start, events, t0, t_end, secs, path, sr=SR, ch=2, block=TR_BLOCK):
    """Runs a transition at every carrier and solo voice; path = the (rate, depth) settings
    the change passes through. The steady-state peak of those settings is the larger of the
    closed form and a measurement of the first, last and steepest of them. -> (ok, worst
    excess over that peak, settle s, dip ms, max). Times are moved to the block start where
    the timeline takes effect."""
    worst_excess, worst_settle, dip_ms, worst_abs = -1e9, 0.0, 0.0, 0.0
    ok = True
    t_end = np.ceil(t_end * sr / block - 1e-9) * block / sr
    for voice in VOICES[mode]:
        model = [slope_cents(voice_slope(mode, voice, r, d)) for r, d in path]
        steep = path[int(np.argmax(model))]
        for f0 in TR_CARRIERS:
            measured = {p: steady_peak(mode, voice, f0, p[0], p[1], sr) for p in {path[0], path[-1], steep}}
            bound = max(max(model), max(measured.values())) + TR_TOL
            after = max(model[-1], measured[path[-1]]) + TR_TOL
            _, c, valid, a = transition(mode, voice, f0, start, events, secs, sr, ch, block)
            t = (np.arange(len(c)) + a) / sr
            seg = (t >= t0 - 0.002) & valid
            dev = np.abs(c[seg])
            peak = float(dev.max()) if len(dev) else 0.0
            worst_abs = max(worst_abs, peak)
            worst_excess = max(worst_excess, peak - (bound - TR_TOL))
            over = np.nonzero((np.abs(c) > after) & valid & (t >= t_end))[0]
            settle = float(t[over[-1]] - t_end) if len(over) else 0.0
            worst_settle = max(worst_settle, settle)
            dip_ms = max(dip_ms, 1e3 * float(np.sum(~valid & (t >= t0 - 0.002))) / sr)
            ok &= peak <= bound and settle <= TR_SETTLE
    return ok, worst_excess, worst_settle, dip_ms, worst_abs


def tr_scenarios():
    """(label, start keys, timeline, t0, t_end, path of (rate, depth))."""
    T = 1.0
    out = []
    for r0, r1, d in ((0.05, 10.0, 1.0), (10.0, 0.05, 1.0), (0.05, 20.0, 1.0), (20.0, 0.05, 1.0),
                      (1.0, 10.0, 1.0), (0.25, 5.0, 0.4), (10.0, 1.0, 0.4)):
        out.append((f"rate {r0:g}->{r1:g} d{d:g}", dict(rate=r0, depth=d), [f"rate@{T}={r1}"], T, T, [(r0, d), (r1, d)]))
    for r, d0, d1 in ((0.25, 0.0, 1.0), (0.25, 1.0, 0.0), (1.0, 0.4, 1.0), (10.0, 0.0, 1.0)):
        out.append((f"depth {d0:g}->{d1:g} r{r:g}", dict(rate=r, depth=d0), [f"depth@{T}={d1}"], T, T, [(r, d0), (r, d1)]))
    out.append(("width 1->0, tone 0->1", dict(rate=1.0, depth=1.0, width=1.0, tone=0.0),
                [f"width@{T}=0", f"tone@{T + 0.3}=1"], T, T + 0.3, [(1.0, 1.0)]))
    out.append(("rate ramp 0.05->10 in 0.25 s", dict(rate=0.05, depth=1.0), [f"rate@{T}..{T + 0.25}=0.05..10"],
                T, T + 0.25, [(0.05, 1.0), (10.0, 1.0)]))
    out.append(("rate ramp 10->0.05 in 1 s", dict(rate=10.0, depth=1.0), [f"rate@{T}..{T + 1.0}=10..0.05"],
                T, T + 1.0, [(10.0, 1.0), (0.05, 1.0)]))
    out.append(("depth ramp 0->1 in 0.2 s r0.5", dict(rate=0.5, depth=0.0), [f"depth@{T}..{T + 0.2}=0..1"],
                T, T + 0.2, [(0.5, 0.0), (0.5, 1.0)]))
    rng = np.random.default_rng(5)
    for gap in (0.025, 0.06):
        rates = np.exp(rng.uniform(np.log(0.05), np.log(10.0), 16))
        depths = rng.uniform(0.2, 1.0, 16)
        ev = []
        for i, (r, d) in enumerate(zip(rates, depths)):
            ev += [f"rate@{T + i * gap:.4f}={r:.4f}", f"depth@{T + i * gap:.4f}={d:.4f}"]
        path = [(1.0, 0.6)] + list(zip(rates, depths))
        out.append((f"16 random rate+depth steps {1e3 * gap:g} ms apart", dict(rate=1.0, depth=0.6), ev,
                    T, T + 15 * gap, path))
    return out


def ch15():
    rows, ok = [], True
    for mode in (WIDE, CE, DIM, TRI):
        worst = (-1e9, "")
        settle = (0.0, "")
        dip = 0.0
        for label, start, events, t0, t_end, path in tr_scenarios():
            good, excess, st, dip_ms, _ = tr_case(mode, start, events, t0, t_end, t_end + 0.8, path)
            ok &= good
            if excess > worst[0]:
                worst = (excess, label)
            if st > settle[0]:
                settle = (st, label)
            dip = max(dip, dip_ms)
            if not good:
                rows.append(f"{TYPES[mode]} FAIL {label}: {excess:+.1f} c over, settle {1e3 * st:.0f} ms")
        rows.append(f"{TYPES[mode]} worst {worst[0]:+.1f} c over the settings' peak ({worst[1]}), "
                    f"settle {1e3 * settle[0]:.0f} ms ({settle[1]}), dip <= {dip:.0f} ms")
    return ok, "; ".join(rows)


def ch15_rates():
    """CH15's hardest steps at 22.05 and 192 kHz, blocks 1, 512 and 4096, mono and stereo."""
    pick = ("rate 0.05->10 d1", "rate 20->0.05 d1", "depth 0->1 r0.25", "16 random rate+depth steps 25 ms apart")
    cases = [c for c in tr_scenarios() if c[0] in pick]
    fails, worst, settle = [], -1e9, 0.0
    for sr in (22050, 192000):
        for block in (1, 512, 4096):
            for ch in (1, 2):
                for mode in (WIDE, CE, DIM, TRI):
                    for label, start, events, t0, t_end, path in cases:
                        good, excess, st, _, _ = tr_case(mode, start, events, t0, t_end, t_end + 0.9, path, sr, ch, block)
                        worst, settle = max(worst, excess), max(settle, st)
                        if not good:
                            fails.append(f"{TYPES[mode]} {label} {sr}/{block}/{ch}ch: {excess:+.1f} c, {1e3 * st:.0f} ms")
    detail = f"worst {worst:+.1f} c over the settings' peak, settle {1e3 * settle:.0f} ms"
    return not fails, detail + ("" if not fails else "; FAIL " + "; ".join(fails[:4]))


SYNC_CASES = {WIDE: (2.0, 1.0), CE: (1.0, 0.7), DIM: (0.5, 0.8), TRI: (1.0, 0.5)}


def sync_events(f, kind, T=1.3, secs=3.2):
    """Timeline tokens of a synced LFO at f Hz with one discontinuity at T."""
    if kind == "start":       # free-running until play at T, from bar start
        return [f"sync_phase@{T}..{secs}={0.0}..{f * (secs - T)}"], [(f,)]
    if kind == "locate":      # +0.5 cycle
        return [f"sync_phase@0..{T}=0..{f * T}", f"sync_phase@{T}..{secs}={f * T + 0.5}..{f * secs + 0.5}"], [(f,)]
    if kind == "loop":        # back 0.37 cycle
        return [f"sync_phase@0..{T}=0..{f * T}", f"sync_phase@{T}..{secs}={f * T - 0.37}..{f * secs - 0.37}"], [(f,)]
    # note value x4: the dispatcher's phase is song position times cycles per beat
    g = 4.0 * f
    return ([f"rate@{T}={g}", f"sync_phase@0..{T}=0..{f * T}", f"sync_phase@{T}..{secs}={g * T}..{g * secs}"],
            [(f,), (g,)])


def ch16():
    """Transport start, locate, loop and note-value jumps of a synced LFO: pitch bound and
    settle as CH15, the LFO on the new phase afterwards (within 2 % of a cycle), and no
    click (220 Hz -12 dBFS, mix 0.5 and 1, 2 kHz HP <= -60 dBFS)."""
    rows, ok = [], True
    T = 1.3
    for mode in (WIDE, CE, DIM, TRI):
        f, depth = SYNC_CASES[mode]
        secs = T + 0.55 + max(2.0 / f, 1.0)
        worst, settle, click, lock = -1e9, 0.0, -300.0, 0.0
        for kind in ("start", "locate", "loop", "note"):
            events, rates = sync_events(f, kind, T, secs)
            start = dict(rate=f, depth=depth)
            path = [(r[0], depth) for r in rates]
            good, excess, st, _, _ = tr_case(mode, start, events, T, T, secs, path)
            ok &= good
            worst, settle = max(worst, excess), max(settle, st)
            if not good:
                rows.append(f"{TYPES[mode]} FAIL {kind}: {excess:+.1f} c over, settle {1e3 * st:.0f} ms")
            # The LFO sits on the requested phase afterwards (voice 0's delay fundamental).
            g = rates[-1][0]
            y = fx.mono(run([f"mode={mode}", f"rate={f}", f"depth={depth}", "mix=1", "solo=0"] + events,
                            f"gen:sine:{PROBE_HZ}:-12", SR, secs, 2, TR_BLOCK))
            t, d = delay_track(y, SR)
            m = (t >= T + 0.3) & (t <= secs - 0.25)
            phi = fund_phase_deg(t[m], d[m], g)
            want = sync_phase_at(events, t[m][0]) * 360.0
            # fund_phase_deg reads phase at t = 0 of its own window: refer it to the window start.
            err = wrap_deg(phi + 360.0 * g * t[m][0] - want) / 360.0
            lock = max(lock, abs(err))
            ok &= abs(err) <= 0.02
            for mix in (0.5, 1.0):
                out = run([f"mode={mode}", f"rate={f}", f"depth={depth}", f"mix={mix}"] + events,
                          "gen:sine:220:-12", SR, secs, 2, TR_BLOCK)
                for chan in out:
                    r = hp_residual(chan, SR)[int(0.5 * SR):int((secs - 0.2) * SR)]
                    click = max(click, float(fx.db(np.max(np.abs(r)))))
        ok &= click <= -60.0
        rows.append(f"{TYPES[mode]} worst {worst:+.1f} c over, settle {1e3 * settle:.0f} ms, "
                    f"phase error {100 * lock:.2f} % of a cycle, click {click:.1f} dBFS")
    return ok, "; ".join(rows)


def sync_phase_at(events, t):
    """The sync phase a timeline asks for at time t (linear ramps only)."""
    value = None
    for e in events:
        key, rest = e.split("@", 1)
        if key != "sync_phase":
            continue
        span, vals = rest.split("=")
        t0, t1 = (float(v) for v in span.split(".."))
        v0, v1 = (float(v) for v in vals.split(".."))
        if t0 <= t <= t1:
            value = v0 + (v1 - v0) * (t - t0) / (t1 - t0)
    return value


def loud_then_silence(sr, secs, ch):
    n = int(secs * sr)
    t = np.arange(n) / sr
    x = fx.amp(-6.0) * np.sin(2 * np.pi * 110.0 * t) + fx.pink(secs, -20.0, sr, seed=31)[:n]
    x[int(1.2 * sr):] = 0.0
    return np.vstack([x] * ch)


def ch17():
    """The processor's bypass: process a loud signal, stop calling for a second, reset(),
    process again. With silent input nothing may come out (no replay); under a sustained
    220 Hz -12 dBFS sine the dry stays continuous across the restart and the wet fades in
    (2 kHz HP <= -60 dBFS), mix 0.35 and 0.5, mono and stereo."""
    rows, ok = [], True
    secs = 3.5
    for mode in (WIDE, CE, DIM, TRI):
        replay, click = -300.0, -300.0
        for ch in (1, 2):
            keys = [f"mode={mode}", "mix=1", "on@1.0=0", "on@2.2=1", "reset@2.2=1"]
            out = run(keys, loud_then_silence(SR, secs, ch), SR, None, ch, TR_BLOCK)
            tail = out[:, int(2.2 * SR):]
            replay = max(replay, float(fx.db(max(np.max(np.abs(tail)), 1e-30))))
            for mix in (0.35, 0.5):
                keys = [f"mode={mode}", f"mix={mix}", "on@1.0=0", "on@2.0=1", "reset@2.0=1"]
                out = run(keys, "gen:sine:220:-12", SR, secs, ch, TR_BLOCK)
                for chan in out:
                    # From well after the switch-off, which is the processor's hard bypass.
                    r = hp_residual(chan, SR)[int(1.5 * SR):int((secs - 0.2) * SR)]
                    click = max(click, float(fx.db(np.max(np.abs(r)))))
        ok &= replay <= -120.0 and click <= -60.0
        rows.append(f"{TYPES[mode]} replay {replay:.1f} dBFS, restart {click:.1f} dBFS")
    return ok, "; ".join(rows)


def ch21():
    """What a type switch into chorus fades in: the engine's own output from reset() on
    (zeros before it), a sustained 220 Hz -12 dBFS sine, 2 kHz HP <= -60 dBFS: at mix 1 as
    is, at mix 0.5 under the dispatcher's 20 ms linear fade (the dry starts at once)."""
    rows, ok = [], True
    T, secs = 1.0, 2.0
    for mode in (WIDE, CE, DIM, TRI):
        worst = -300.0
        for ch in (1, 2):
            for mix in (1.0, 0.5):
                out = run([f"mode={mode}", f"mix={mix}", f"reset@{T}=1"], "gen:sine:220:-12", SR, secs, ch, TR_BLOCK)
                y = out.copy()
                y[:, :int(T * SR)] = 0.0
                if mix < 1.0:
                    ramp = np.clip((np.arange(y.shape[1]) - T * SR) / (0.02 * SR), 0.0, 1.0)
                    y *= ramp
                for chan in y:
                    r = hp_residual(chan, SR)[int(0.5 * SR):int((secs - 0.2) * SR)]
                    worst = max(worst, float(fx.db(np.max(np.abs(r)))))
        ok &= worst <= -60.0
        rows.append(f"{TYPES[mode]} {worst:.1f} dBFS")
    return ok, "onset after reset, 2 kHz HP peak: " + ", ".join(rows)


def chord(secs, sr, level_db=-12.0, seed=11):
    """A sustained E major guitar chord, six notes with six harmonics each."""
    t = np.arange(int(secs * sr)) / sr
    rng = np.random.default_rng(seed)
    x = np.zeros_like(t)
    for f in (82.41, 123.47, 164.81, 207.65, 246.94, 329.63):
        for h in range(1, 7):
            x += np.sin(2 * np.pi * f * h * t + rng.uniform(0, 2 * np.pi)) / h
    return x / np.max(np.abs(x)) * fx.amp(level_db)


def frame_db(y, sr, win=0.02, hop=0.005):
    n, h = int(win * sr), int(hop * sr)
    starts = np.arange(0, y.shape[-1] - n, h)
    p = np.array([np.mean(y[..., s:s + n] ** 2) for s in starts])
    return (starts + n / 2) / sr, 10 * np.log10(np.maximum(p, 1e-30))


def ch18():
    """Mode switches between the new modes hold the level. Under a sustained chord the
    20 ms level, averaged over six switches each way (at different LFO phases: one switch
    alone mostly shows where the comb happens to sit, up to 4 dB on this chord), stays
    within 1.5 dB of the span between the levels before and after, every ordered pair, mix
    0.35 / 0.5 / 1; a 220 Hz -12 dBFS sine through the same switches shows no click (2 kHz
    HP <= -60 dBFS). Rate and depth are the outgoing mode's starting point."""
    gap, count = 0.6, 12
    secs = 1.0 + gap * count + 0.4
    x = fx.stereo(chord(secs, SR))
    worst_lv, worst_click, where = -9.0, -300.0, ""
    ok = True
    for mix in (0.35, 0.5, 1.0):
        for a, b in ((WIDE, CE), (WIDE, DIM), (WIDE, TRI), (CE, DIM), (CE, TRI), (DIM, TRI)):
            times = [1.0 + gap * k for k in range(count)]
            keys = ([f"mode={a}", f"mix={mix}"] + [f"{k}={v}" for k, v in START[a].items() if k != "mix"]
                    + [f"mode@{t:.3f}={b if k % 2 == 0 else a}" for k, t in enumerate(times)])
            t, lv = frame_db(run(keys, x, SR, None, 2, TR_BLOCK), SR)
            for first, (frm, to) in enumerate(((a, b), (b, a))):
                rel = np.arange(-0.1, 0.3, 0.005)
                power = np.zeros_like(rel)
                for t0 in times[first::2]:
                    power += np.interp(t0 + rel, t, 10 ** (lv / 10))
                trace = 10 * np.log10(power / len(times[first::2]))
                before = 10 * np.log10(np.mean(10 ** (trace[rel < -0.03] / 10)))
                after = 10 * np.log10(np.mean(10 ** (trace[rel > 0.2] / 10)))
                lo, hi = min(before, after) - 1.5, max(before, after) + 1.5
                excess = float(max(np.max(trace - hi), np.max(lo - trace))) + 1.5
                if excess > worst_lv:
                    worst_lv, where = excess, f"{TYPES[frm]}->{TYPES[to]} mix {mix:g}"
                ok &= excess <= 1.5
            out = run(keys, "gen:sine:220:-12", SR, secs, 2, TR_BLOCK)
            for chan in out:
                r = hp_residual(chan, SR)[int(0.5 * SR):int((secs - 0.2) * SR)]
                worst_click = max(worst_click, float(fx.db(np.max(np.abs(r)))))
    ok &= worst_click <= -60.0
    return ok, (f"largest distance from the before/after span: {worst_lv:+.2f} dB ({where}); "
                f"click {worst_click:.1f} dBFS")


def ifp95(y, sr, f0):
    """|IF| p95 in cents where the (possibly multi-voice) wet is not in an amplitude null."""
    c, amp_, _ = if_cents(y, sr, f0, smooth_ms=3.0)
    lo, hi = int(1.0 * sr), len(c) - int(0.3 * sr)
    a = amp_[lo:hi]
    keep = a > 0.2 * np.median(a)
    return float(np.percentile(np.abs(c[lo:hi])[keep], 95))


def comb_swing(y, x, sr, nfft=4096):
    """Median over third-octave bands 200 Hz - 5 kHz of how far each band's level against
    the input moves over time (95th - 5th percentile, dB, 85 ms frames)."""
    edges = 200 * 2 ** (np.arange(0, 15) / 3.0)
    f = np.fft.rfftfreq(nfft, 1 / sr)
    win = np.hanning(nfft)
    starts = range(int(1.0 * sr), len(y) - nfft, nfft // 2)
    S = np.array([np.abs(np.fft.rfft(y[s:s + nfft] * win)) ** 2 for s in starts])
    X = np.array([np.abs(np.fft.rfft(x[s:s + nfft] * win)) ** 2 for s in starts])
    out = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        m = (f >= lo) & (f < hi)
        r = 10 * np.log10(S[:, m].sum(1) / X[:, m].sum(1))
        out.append(np.percentile(r, 95) - np.percentile(r, 5))
    return float(np.median(out))


def ch19():
    """A mono instance is still a chorus: pitch deviation (|IF| p95 at mix 1, 220 / 997 /
    3302 Hz) at least half the stereo instance's left channel, and a moving comb (pink
    noise at the mode's starting mix) at least half as deep."""
    rows, ok = [], True
    for mode in (WIDE, CE, DIM, TRI):
        st = START[mode]
        secs = 1.3 + max(2.0 / st["rate"], 3.0)
        ratios = []
        for f0 in (220.5, 997.3, 3301.7):
            keys = dict(mode=mode, rate=st["rate"], depth=st["depth"], mix=1)
            x = fx.sine(f0, -12, secs, SR)
            s = ifp95(run(keys, fx.stereo(x), SR)[0], SR, f0)
            m = ifp95(run(keys, x[None, :], SR, ch=1)[0], SR, f0)
            ratios.append((m, s))
        x = fx.pink(10.0, -18.0, SR, seed=6)
        keys = dict(START[mode], mode=mode)
        cs = comb_swing(run(keys, fx.stereo(x), SR)[0], x, SR)
        cm = comb_swing(run(keys, x[None, :], SR, ch=1)[0], x, SR)
        good = all(m >= 0.5 * s for m, s in ratios) and cm >= 0.5 * cs
        ok &= good
        rows.append(f"{TYPES[mode]} detune mono/stereo " + " ".join(f"{m:.2f}/{s:.2f}" for m, s in ratios)
                    + f" c, comb {cm:.1f}/{cs:.1f} dB")
    return ok, "; ".join(rows)


def ch20():
    """Non-finite input: one NaN, +Inf or -Inf sample (and a run of 100 NaNs) behaves exactly
    like a zero sample, bit for bit, in every mode, mono and stereo, blocks 37 and 512; one
    1e30 sample gives finite output that is back on the clean render 0.2 s later."""
    rows, ok = [], True
    n = int(1.5 * SR)
    base = np.vstack([fx.pink(1.5, -18.0, SR, seed=21)[:n], fx.pink(1.5, -18.0, SR, seed=22)[:n]])
    for mode in TYPES:
        bad_runs, huge = 0, -300.0
        for ch in (1, 2):
            for bs in (37, 512):
                keys = [f"mode={mode}"] + ([f"{k}={v}" for k, v in START[mode].items()] if mode else [])
                opts = dict(sr=SR, secs=None, ch=ch, block=bs)
                clean = base[:ch].copy()
                zeroed = clean.copy()
                zeroed[0, SR // 2] = 0.0
                zeroed[0, SR // 2 + 1000:SR // 2 + 1100] = 0.0
                ref = run(keys, zeroed, **opts)
                for bad in (np.nan, np.inf, -np.inf):
                    x = clean.copy()
                    x[0, SR // 2] = bad
                    x[0, SR // 2 + 1000:SR // 2 + 1100] = np.nan
                    y = run(keys, x, **opts)
                    if not np.all(np.isfinite(y)) or not np.array_equal(y, ref):
                        bad_runs += 1
                x = clean.copy()
                x[0, SR // 2] = 1e30
                y = run(keys, x, **opts)
                c = run(keys, clean, **opts)
                tail = slice(int(0.7 * SR), None)
                if not np.all(np.isfinite(y)):
                    bad_runs += 1
                else:
                    huge = max(huge, float(fx.db(np.max(np.abs(y[:, tail] - c[:, tail])) + 1e-30)))
        good = bad_runs == 0 and huge <= -100.0
        ok &= good
        rows.append(f"{TYPES[mode]} {bad_runs} bad, 1e30 residue {huge:.0f} dB")
    return ok, "; ".join(rows)


TORTURE = ["rate@0.3=10", "depth@0.35=0.1", "mode@0.4=3", "rate@0.45=0.05", "mode@0.47=2", "mode@0.5=0",
           "sync_phase@0.6..1.0=0.3..2.3", "rate@0.6=5", "sync_phase@1.0..1.4=0.9..2.9", "mode@0.8=4", "on@1.1=0",
           "on@1.2=1", "reset@1.2=1", "mode@1.25=1", "depth@1.3=1", "mode@1.32=4", "mode@1.35=1", "rate@1.4=20"]


def ch22():
    """--rt-check and --reset-repeat through switches, dips, sync jumps, bypass and reset,
    22.05-192 kHz, blocks 1 / 37 / 512 / 4096, mono and stereo; output finite."""
    fails = []
    for sr in fx.RATES:
        for ch in (1, 2):
            for bs in (1, 37, 512, 4096):
                for opt in ("--rt-check", "--reset-repeat"):
                    r = fx.invoke(EFFECT, "gen:music", ["mode=1"] + TORTURE,
                                  [f"--block={bs}", f"--channels={ch}", "--seconds=2", opt], sr)
                    if r.code != 0 or not np.all(np.isfinite(r.out)):
                        fails.append(f"{sr}/{ch}ch/{bs} {opt}")
    return not fails, "all clean" if not fails else "; ".join(fails[:6])


def ch23():
    """CPU while a switch runs two modes: modes alternating every 60 ms, so one is always
    filling or fading; budget = the two modes' budgets added."""
    rows, ok = [], True
    classic = 1.2 * fx.invoke(EFFECT, "gen:noise:pink:-18", ["mode=0"], ["--bench", "--legacy"],
                              48000).number(r"([0-9.]+) % of real time")
    for a, b in ((WIDE, TRI), (CE, DIM), (WIDE, CE), (DIM, TRI), (WIDE, 0)):
        tokens = [f"mode={a}"] + [f"mode@{0.06 * i:.2f}={(b if i % 2 else a)}" for i in range(1, 167)]
        pct = fx.invoke(EFFECT, "gen:noise:pink:-18", tokens, ["--bench"], 48000).number(r"([0-9.]+) % of real time")
        budget = BUDGETS[a] + (BUDGETS[b] if BUDGETS[b] is not None else classic)
        ok &= pct <= budget
        rows.append(f"{TYPES[a]}+{TYPES[b]} {pct:.3f} % (budget {budget:.3g})")
    return ok, "; ".join(rows)


def specific_checks(report, args):
    only = args.types

    def want(*modes):
        return only is None or any(m in only for m in modes)

    if want(0):
        ok, detail = fx.classic_null(EFFECT, ["rate@1=5", "depth@2=1", "mix@3=0.8", "rate@4=0.05", "depth@4.5=0.1"],
                                     blocks=[512, 37, 1])
        report.add("CH1", "Classic null, steps, blocks 1/37/512", ok, detail or "0 differing samples")
    rows = [("CH2", "Wide pitch, depth 0.5", ch2, WIDE), ("CH3", "Wide pitch, depth 1", ch3, WIDE),
            ("CH4", "Wide stereo", ch4, WIDE), ("CH5", "CE sweep, depth 1", ch5, CE), ("CH6", "CE pitch", ch6, CE),
            ("CH7", "CE wet vs brigade_fr.tsv", ch7, CE), ("CH8", "Dimension", ch8, DIM),
            ("CH8-rates", "Dimension detune bound 0.5-20 Hz", ch8_rates, DIM),
            ("CH4/8-width", "mono-sum remedy, width <= 0.8", ch_width_remedy, WIDE), ("CH9", "Tri", ch9, TRI),
            ("CH10", "Tone corner shift", ch10, WIDE), ("CH11", "Clicks (mode, rate, depth)", ch11, WIDE),
            ("CH12", "Interpolation ripple", ch12, WIDE), ("CH13", "Sync 120 BPM 1/4", ch13, WIDE),
            ("CH-mono", "mono instance", ch_mono, WIDE),
            ("CH15", "transients: rate, depth, width, tone", ch15, WIDE),
            ("CH15-rates", "CH15 at 22.05/192 kHz, blocks, mono", ch15_rates, WIDE),
            ("CH16", "sync: start, locate, loop, note", ch16, WIDE), ("CH17", "bypass, reset: replay, restart", ch17, WIDE),
            ("CH18", "mode switch: level, click", ch18, WIDE), ("CH19", "mono: detune and comb", ch19, WIDE),
            ("CH20", "non-finite input", ch20, WIDE), ("CH21", "onset after reset", ch21, WIDE),
            ("CH22", "rt-check, reset-repeat under switches", ch22, WIDE), ("CH23", "cpu during a switch", ch23, WIDE)]
    for name, scope, fn, mode in rows:
        if want(mode):
            ok, detail = fn()
            report.add(name, scope, ok, detail)
    if want(WIDE, CE, DIM):
        _, rows14 = ch14()
        for sr, name, good, detail in rows14:
            report.add("CH14", f"{name} at {sr} Hz", good, detail)


# ---------------------------------------------------------------------------- --measure

MEASURE_RATES = [0.05, 0.1, 0.25, 0.44, 0.5, 1.0, 2.0, 2.86, 3.0, 5.0, 10.0, 16.0, 20.0]
MEASURE_DEPTHS = [0.25, 0.5, 0.75, 1.0]


def measure():
    print("\n== Peak pitch deviation, cents (p95 of |dev|, 1 kHz probe, voice 0, mix 1, 48 kHz); model in brackets")
    for mode in (WIDE, CE, DIM, TRI):
        print(f"\n{TYPES[mode]}")
        print("rate Hz  " + "".join(f"depth {d:<14}" for d in MEASURE_DEPTHS))
        for r in MEASURE_RATES:
            cells = []
            for d in MEASURE_DEPTHS:
                got = pitch_p95(mode, r, d, ch=1)
                cells.append(f"{got:6.2f} [{model_cents(mode, r, d):5.2f}]  ")
            print(f"{r:<8g} " + "".join(cells), flush=True)

    print("\n== Interpolation: soloed voice, mix 1, tone 1, rate 1 Hz, 48 kHz")
    print("mode       depth  freq   mean dB  ripple dB  AM sidebands dB re carrier")
    for mode in (WIDE, CE, DIM, TRI):
        for depth in (0.3, 1.0):
            for f in (5000.0, 10000.0, 15000.0):
                y = probe(dict(mode=mode, rate=1.0, depth=depth, tone=1.0, mix=1, solo=0), 4.0, freq=f)
                env = hop_mean(np.abs(fx.analytic(y))[int(0.5 * SR):int(3.7 * SR)], int(0.0005 * SR))
                mean_db = float(fx.db(env.mean() / fx.amp(-12)))
                ripple = float(fx.db(env.max() / env.min()))
                am = float(10 * np.log10(np.var(env) / env.mean() ** 2 / 2 + 1e-30))
                print(f"{TYPES[mode]:<10} {depth:<6g} {f / 1e3:4.0f}k  {mean_db:7.2f}  {ripple:9.2f}  {am:9.1f}")

    print("\n== Stereo width and mono compatibility (band noise 150 Hz-4 kHz, each mode's starting point, width 1)")
    print("mode       mix   corr L/R  mono sum re L dB  R re L dB  mono-sum comb depth dB (median 85 ms frame)")
    for mode in (WIDE, CE, DIM, TRI):
        for mix in sorted({START[mode]["mix"], 0.5, 1.0}):
            keys = dict(START[mode], mix=mix, width=1.0)
            corr, loss, rl = width_stats(mode, keys)
            x = band_noise(6.0, SR)
            out = run(dict(keys, mode=mode), fx.stereo(x), SR)
            mid = 0.5 * (out[0] + out[1])
            depths = []
            nfft = 4096
            win = np.hanning(nfft)
            freqs = np.fft.rfftfreq(nfft, 1.0 / SR)
            band = (freqs >= 250) & (freqs <= 3500)
            for s in range(int(0.5 * SR), len(x) - nfft, nfft):
                rx = np.abs(np.fft.rfft(x[s:s + nfft] * win)) ** 2
                ry = np.abs(np.fft.rfft(mid[s:s + nfft] * win)) ** 2
                ratio = np.convolve(ry, np.ones(9), "same") / np.maximum(np.convolve(rx, np.ones(9), "same"), 1e-30)
                lv = 10 * np.log10(ratio[band])
                depths.append(np.median(lv) - lv.min())
            print(f"{TYPES[mode]:<10} {mix:<5g} {corr:8.3f}  {loss:+15.2f}  {rl:+9.2f}  {np.median(depths):8.1f}")

    print("\n== Dry/wet law: output level re input, band noise, L channel, starting rate/depth, width 1")
    mixes = [0.0, 0.25, 0.5, 0.75, 1.0]
    print("mode       " + "".join(f"mix {m:<6g}" for m in mixes))
    x = band_noise(4.0, SR)
    for mode in (WIDE, CE, DIM, TRI):
        cells = []
        for m in mixes:
            out = run(dict(START[mode], mode=mode, mix=m), fx.stereo(x), SR)
            a = int(0.5 * SR)
            cells.append(f"{fx.db(np.sqrt(np.mean(out[0, a:] ** 2)) / np.sqrt(np.mean(x[a:] ** 2))):+8.2f}  ")
        print(f"{TYPES[mode]:<10} " + "".join(cells))

    print("\n== Zipper under automation (1 kHz -12 dBFS; automation reaches setParameters once per block)")
    print("   a) mix 0->1, width 1->0, tone 0->1 ramps with the LFO still (depth 0): any line at k*fs/block is zipper")
    print("mode       block  largest k*fs/block line dB re carrier")
    for mode in (WIDE, CE, DIM, TRI):
        keys = [f"mode={mode}", "depth=0", "mix@0.5..3.5=0..1", "width@3.5..6.5=1..0", "tone@6.5..9.5=0..1"]
        for block in (64, 512):
            out = run(keys, f"gen:sine:{PROBE_HZ}:-12", SR, 10.0, block=block)
            line = max(fx.zipper_lines(chan, SR, block, PROBE_HZ) for chan in out)
            print(f"{TYPES[mode]:<10} {block:<6d} {line:10.1f}")
    print("   b) rate 0.2->5 Hz then depth 0->1 ramps: peak after a 4 kHz HP, against the same settings held still")
    print("mode       block  automated dBFS  still (rate 5, depth 1) dBFS")
    for mode in (WIDE, CE, DIM, TRI):
        keys = [f"mode={mode}", "rate@0.5..3.5=0.2..5", "depth@3.5..6.5=0..1"]
        still = run([f"mode={mode}", "rate=5", "depth=1"], f"gen:sine:{PROBE_HZ}:-12", SR, 7.0)
        base = max(float(fx.db(np.max(np.abs(hp_residual(chan, SR, 4000.0)[int(0.5 * SR):int(6.8 * SR)]))))
                   for chan in still)
        for block in (64, 512, 2048):
            out = run(keys, f"gen:sine:{PROBE_HZ}:-12", SR, 7.0, block=block)
            hp = max(float(fx.db(np.max(np.abs(hp_residual(chan, SR, 4000.0)[int(0.5 * SR):int(6.8 * SR)]))))
                     for chan in out)
            print(f"{TYPES[mode]:<10} {block:<6d} {hp:12.1f}  {base:12.1f}")

    print("\n== Delay-range limits: 25 Hz probe, delay trajectory of the longest voice, made absolute at depth 0")
    print("mode       sr      rate   depth  voice  min ms  max ms  max pitch step between 1 ms hops (cents)")
    cases = [(WIDE, 192000, 0.05, 1.0, 3), (WIDE, 22050, 0.05, 1.0, 3), (WIDE, 48000, 0.01, 1.0, 3),
             (WIDE, 48000, 20.0, 1.0, 3), (TRI, 192000, 0.05, 1.0, 0), (TRI, 22050, 0.01, 1.0, 0),
             (CE, 192000, 0.43, 1.0, 0), (CE, 22050, 20.0, 1.0, 0), (DIM, 192000, 0.25, 1.0, 1)]
    base = {WIDE: 0.010, TRI: 0.007, CE: 0.00295, DIM: 0.0085}
    for mode, sr, rate, depth, voice in cases:
        secs = min(secs_for(rate, 1.2, 1.0), 125.0)
        keys = dict(mode=mode, rate=rate, depth=depth, mix=1, solo=voice)
        f0 = 25.0
        t, raw = delay_track(probe(keys, secs, sr, freq=f0, ch=1), sr, f0)
        _, ref = delay_track(probe(dict(keys, depth=0), 2.0, sr, freq=f0, ch=1), sr, f0)
        d = base[mode] + ((raw - ref.mean() + 0.005) % (1 / f0)) - 0.005   # shift kept in [-5, 35) ms
        _, c = cents_track(probe(keys, secs, sr, ch=1), sr)
        step = float(np.max(np.abs(np.diff(c)))) if len(c) > 1 else 0.0
        print(f"{TYPES[mode]:<10} {sr:<7d} {rate:<6g} {depth:<6g} {voice:<6d} {1e3 * d.min():6.2f}  {1e3 * d.max():6.2f}  {step:8.2f}")


def main():
    want_measure = "--measure" in sys.argv
    if want_measure:
        sys.argv.remove("--measure")
    args = fx.parse_args(__doc__)
    report = fx.Report(EFFECT)
    fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                     quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
    specific_checks(report, args)
    status = report.finish()
    if want_measure:
        measure()
    return status


if __name__ == "__main__":
    sys.exit(main())
