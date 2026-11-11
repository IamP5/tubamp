#!/usr/bin/env python3
"""Acceptance for the tremolo engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.7.7). Prints a PASS/FAIL table; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_tremolo.py [--quick] [--types 1,2] [--specific]

--specific skips the common checks (a development aid; the acceptance run is without it).

Rows T1-T17 are the contract's. Rows X1-X6 are measurements the contract asks for in prose
(rate accuracy, depth law across the knob, transparency at depth 0, zipper under
automation, the stereo power and Bias level tables); their pass bars are noted per row.
Envelopes come from the engine's gain trace (--trace) unless a row says otherwise.

The numeric reference for T2 is docs/research/fx/tremolo/prototype/tremolo_proto.py. Its
building blocks are used as they are; where it has no time-domain model (Bias envelope,
Harmonic bands, stereo spread on Optical/Bias/Harmonic) the reference below writes out
§4.7.4-§4.7.5 on top of them. Knob values are fed to it rounded to float32, as the engine
receives them. Bias and Harmonic take no loudness exponent under spread (coordinator
decision, deviating from §4.7.4): their summed power is then constant at 180 degrees.
Every gain smoother starts at unity after prepare() or reset() (y0=1 in the reference): the
first block glides from the input into the tremolo instead of stepping to the dip.

Rows M1, R1, S1, T5 (table), X7 and N1 were added after the independent verification
(.audit/fx-verify/mod.md): mono and Width, re-enable after bypass, song-position jumps,
pulse splatter against the carrier frequency, depth 0 after automation, NaN and Inf.

Rows Y1-Y8 and Z1 were added after the second verification (.audit/fx-verify/r2-mod.md):
sync transitions at every host block size (N4), sync under host jitter and tempo changes,
voice switches against both voices' trajectories (N5), the start after a reset (N10), pulse
splatter against the edge time (N9), CPU at every width (N15), huge finite samples (N14),
Bias and DC (N11), and real-time safety during switches. The Smooth and Bias references of T2
start at the voice's crest (Tremolo.h reset()); Smooth's pulse has the engine's raised-cosine
edges, Bias's detector its 10 Hz AC coupling. T16's fade-length probe holds the LFO near phase
0 by a song position, since a reset now starts each voice at its crest.
"""
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxcheck as fx  # noqa: E402
import numpy as np  # noqa: E402

sys.path.insert(0, str(fx.REPO / "docs" / "research" / "fx" / "tremolo" / "prototype"))
import tremolo_proto as proto  # noqa: E402

EFFECT = "tremolo"
TYPE_KEY = "voice"
TYPES = {0: "Classic", 1: "Smooth", 2: "Optical", 3: "Bias", 4: "Harmonic"}
# Float keys at their documented limits, for the finite-output test.
RANGES = {"rate": (0.01, 20.0), "depth": (0.0, 1.0), "mix": (0.0, 1.0), "width": (0.0, 1.0), "tone": (0.0, 1.0),
          "shape": (0.0, 1.0), "wave": (0.0, 1.0)}
# Percent of real time per instance (§2.10); None = Classic as today (<= 1.2 x legacy).
BUDGETS = {0: None, 1: 0.10, 2: 0.10, 3: 0.10, 4: 0.15}
VARIANTS = {}
NULL_KEYS = [{}, {"rate": 10, "depth": 1, "mix": 1}, {"rate": 0.05, "depth": 0.1, "mix": 0.5}]
# Classic null under automation: rate, depth and mix steps and a rate ramp (§4.7.3).
NULL_STEPS = ["rate@1=10", "depth@2=1", "mix@2.5=1", "rate@3..4=0.05..7", "depth@4=0.1", "mix@5=0.5"]

LEVEL = -18.0          # §4.7.7: steady 1 kHz sine at -18 dBFS, 4 Hz unless stated
# T5 splatter at the contract's carriers and in the dense sweep: was -58 dBc (one-pole edges,
# worst -59.1); the raised-cosine edges measure -89.8 worst at 44.1, 48 and 96 kHz, plus 1 dB.
SPLATTER_CONTRACT = -88.8
CARRIER = 1000.0


# ---------------------------------------------------------------------------- helpers

def f32(v):
    return float(np.float32(v))


def as_f32(x):
    return np.asarray(x, dtype=np.float32).astype(np.float64)


def render(inp, keys, sr=48000, block=512, extra=()):
    """-> (out [ch, n], trace [2, n]); the input is rounded to float32 like the WAV it travels in."""
    r = fx.invoke(EFFECT, inp, keys, [f"--block={block}", "--trace"] + list(extra), sr)
    if r.trace is None:
        raise RuntimeError(r.stdout)
    return r.out, np.atleast_2d(r.trace)


def tone_in(sr, secs=2.0, freq=CARRIER, level=LEVEL, phase=0.0, channels=2):
    x = as_f32(fx.sine(freq, level, secs, sr, phase))
    return np.vstack([x] * channels)


def rel_db(err, ref):
    return float(fx.db(np.sqrt(np.mean(np.square(err))) / max(np.sqrt(np.mean(np.square(ref))), 1e-30)))


def cycle(g, rate, sr, cycles=1):
    per = int(round(sr * cycles / rate))
    return g[-per:]


def stats(g, rate, sr):
    """proto.cycle_stats on the last cycle: gmax, gmin, depth_db, duty_hi, rise_ms, fall_ms."""
    return proto.cycle_stats(np.asarray(g, float), rate, sr)


def power_ripple(tr, rate, sr):
    p = cycle(tr[0], rate, sr) ** 2 + cycle(tr[1], rate, sr) ** 2
    return float(10 * np.log10(p.max() / max(p.min(), 1e-30)))


def phase_deg(g, rate, sr):
    """Phase of the LFO fundamental in a whole number of cycles, degrees."""
    c = cycle(g, rate, sr, cycles=4)
    t = np.arange(len(c)) / sr
    return float(np.degrees(np.angle(np.sum((c - c.mean()) * np.exp(-2j * np.pi * rate * t)))))


def splatter_dbc(y, sr, carrier):
    """tremolo_proto.sec_chop's measure: energy 2-5 kHz above the carrier (3-6 kHz for
    1 kHz) over the strongest bin, Hann window."""
    lo, hi = carrier + 2000.0, carrier + 5000.0
    y = fx.mono(y)
    spec = np.abs(np.fft.rfft(y * np.hanning(len(y))))
    freqs = np.fft.rfftfreq(len(y), 1.0 / sr)
    band = np.sqrt(np.sum(spec[(freqs >= lo) & (freqs < hi)] ** 2))
    return float(fx.db(band / spec.max()))


def sliding_rms_db(y, sr, ms):
    y = fx.mono(y)
    n = max(1, int(round(sr * ms / 1000.0)))
    c = np.concatenate([[0.0], np.cumsum(y * y)])
    rms = np.sqrt(np.maximum(c[n:] - c[:-n], 0.0) / n)
    return fx.db(np.maximum(rms, 1e-12)) + 3.01


# ---------------------------------------------------------------------------- reference (T2)

PULSE_EDGE_S = 0.001   # Tremolo.cpp kPulseEdgeSeconds


def w_morph_edged(ph, wave, s, rate):
    """proto.w_morph with the engine's pulse: raised-cosine edges kPulseEdgeSeconds long,
    centred on the hard edges, in place of the hard steps."""
    if wave <= 0.5:
        return proto.w_morph(ph, wave, s)
    tri = proto.w_tri(proto.skew(ph, s))
    q = (ph - 0.5 * s) % 1.0
    e = PULSE_EDGE_S * rate
    h = 0.5 * e
    pulse = np.where(q < s, 1.0, 0.0)
    rc = lambda v: 0.5 - 0.5 * np.cos(np.pi * v)  # noqa: E731
    m = q < h
    pulse[m] = rc(0.5 + q[m] / e)
    m = q >= 1.0 - h
    pulse[m] = rc(0.5 + (q[m] - 1.0) / e)
    m = (q >= s - h) & (q < s + h)
    pulse[m] = 1.0 - rc(0.5 + (q[m] - s) / e)
    a = 2 * wave - 1
    return (1 - a) * tri + a * pulse


def ref_smooth(x, sr, rate, depth, wave, shape, width):
    """Smooth starts at its crest, phase s (Tremolo.h reset())."""
    n = x.shape[1]
    s = 0.1 + 0.8 * f32(shape)
    e = 1.0 - 0.5 * f32(width)
    out = []
    for c, start in enumerate((s, s + 0.5 * f32(width))):
        w = w_morph_edged(proto.phase(f32(rate), n, sr, start), f32(wave), s, f32(rate))
        g = proto.onepole(proto.apply_depth(w ** e, f32(depth)), proto.SLEW_HZ, sr=sr, y0=1.0)
        out.append(x[c] * g)
    return np.vstack(out)


def ref_optical(x, sr, rate, depth, width):
    assert sr == proto.SR
    n = x.shape[1]
    return np.vstack([x[c] * proto.voice_optical(f32(rate), f32(depth), n, start=start)
                      for c, start in enumerate((0.0, 0.5 * f32(width)))])


def envelope_follower(level, sr, attack=0.005, release=0.150):
    att, rel = 1 - np.exp(-1 / (attack * sr)), 1 - np.exp(-1 / (release * sr))
    env, e = np.empty(len(level)), 0.0
    for i, v in enumerate(level):
        e += (att if v > e else rel) * (v - e)
        env[i] = e
    return env


def ref_bias(x, sr, rate, depth, shape, width, exponent=1.0):
    """Bias starts at its crest, phase 0.5 (Tremolo.h reset()); its detector is AC-coupled at
    10 Hz (one-pole low-pass subtracted) ahead of the follower."""
    n = x.shape[1]
    m = 0.5 * (x[0] + x[1])
    a = 1 - np.exp(-2 * np.pi * 10.0 / sr)
    hp, low = np.empty(n), 0.0
    for i, v in enumerate(m):
        low += a * (v - low)
        hp[i] = v - low
    env = envelope_follower(np.abs(hp), sr)
    e = exponent
    out = []
    for c, start in enumerate((0.5, 0.5 + 0.5 * f32(width))):
        u = 0.5 + 0.5 * np.cos(2 * np.pi * proto.phase(f32(rate), n, sr, start))
        g = proto.bias_gain(u ** e, f32(depth), env, knee_dbfs=-24.0 + 24.0 * f32(shape))
        out.append(x[c] * proto.onepole(g, proto.SLEW_HZ, sr=sr, y0=1.0))
    return np.vstack(out)


def ref_harmonic(x, sr, rate, depth, tone, width, exponent=1.0):
    n = x.shape[1]
    fc = min(300.0 * 2 ** (2 * (f32(tone) - 0.5)), 0.49 * sr)
    g = np.tan(np.pi * fc / sr)
    G = g / (1 + g)
    d = f32(depth)
    norm = np.sqrt(2 / (2 - d))
    e = exponent
    out = []
    for c, start in enumerate((0.0, 0.5 * f32(width))):
        u = (0.5 - 0.5 * np.cos(2 * np.pi * proto.phase(f32(rate), n, sr, start))) ** e
        gl = proto.onepole(norm * np.sqrt(1 - d * u), proto.SLEW_HZ, sr=sr, y0=1.0)
        gh = proto.onepole(norm * np.sqrt(1 - d * (1 - u)), proto.SLEW_HZ, sr=sr, y0=1.0)
        low, s = np.empty(n), 0.0
        for i, v in enumerate(x[c]):
            w = (v - s) * G
            low[i] = w + s
            s = low[i] + w
        out.append(gl * low + gh * (x[c] - low))
    return np.vstack(out)


# ---------------------------------------------------------------------------- rows

def t1_null_steps(report):
    ok, detail = fx.classic_null(EFFECT, NULL_STEPS, blocks=[1, 37, 512])
    report.add("T1", "Classic null, rate/depth/mix steps", ok, detail or "0 differing samples, 44.1/48/96 kHz, blocks 1/37/512")


def t2_parity(report):
    sr = 48000
    x = tone_in(sr)
    worst = {}
    for width in (0.0, 1.0):
        cases = []
        for wave in (0.0, 0.5, 1.0):
            for shape in (0.2, 0.5, 0.8):
                k = dict(voice=1, rate=4.0, depth=0.75, wave=wave, shape=shape, width=width)
                cases.append(("Smooth", k, ref_smooth(x, sr, 4.0, 0.75, wave, shape, width), x))
        k = dict(voice=2, rate=5.0, depth=1.0, width=width)
        cases.append(("Optical", k, ref_optical(x, sr, 5.0, 1.0, width), x))
        k = dict(voice=3, rate=5.0, depth=0.75, shape=0.5, width=width)
        cases.append(("Bias", k, ref_bias(x, sr, 5.0, 0.75, 0.5, width), x))
        hx = as_f32(np.vstack([fx.sine(82.0, LEVEL, 2.0, sr) + fx.sine(2000.0, LEVEL, 2.0, sr)] * 2))
        k = dict(voice=4, rate=4.0, depth=0.8, tone=0.5, width=width)
        cases.append(("Harmonic", k, ref_harmonic(hx, sr, 4.0, 0.8, 0.5, width), hx))
        for name, k, ref, inp in cases:
            out, _ = render(inp, k, sr)
            d = rel_db(out - ref, ref)
            key = f"{name} spread {int(180 * width)}"
            if key not in worst or d > worst[key][0]:
                worst[key] = (d, k)
    for key, (d, k) in worst.items():
        extra = f" (worst at wave {k['wave']} shape {k['shape']})" if "wave" in k else ""
        report.add("T2", f"parity {key}", d <= -80.0, f"rms diff {d:.1f} dB re output{extra}")


def t3_t4_smooth(report, sr=48000, tag=""):
    _, tr = render(tone_in(sr), dict(voice=1, rate=4.0, depth=1.0, wave=0.0, width=0.0), sr)
    gmax, gmin, _, duty, _, _ = stats(tr[0], 4.0, sr)
    crest = fx.db(gmax)
    floor = fx.db(max(gmin, 1e-12) / gmax)
    ok = abs(crest - 3.0) <= 0.2 and floor <= -60.0 and abs(duty - 0.5) <= 0.01
    report.add("T3" + tag, f"Smooth depth 1 sine{' ' + str(sr) if tag else ''}", ok,
               f"crest {crest:+.2f} dB, floor {floor:.1f} dB re crest, loud {duty:.3f}")
    if tag:
        return
    _, tr = render(tone_in(sr), dict(voice=1, rate=4.0, depth=0.5, wave=0.0, width=0.0), sr)
    gmax, gmin, _, _, _, _ = stats(tr[0], 4.0, sr)
    floor = fx.db(gmin / gmax)
    report.add("T4", "Smooth depth 0.5", abs(floor + 12.0) <= 0.5,
               f"floor {floor:.2f} dB re crest, crest {fx.db(gmax):+.2f} dB")


def t5_pulse(report, sr=48000, tag=""):
    rows = []
    ok = True
    for shape in (0.5, 0.25, 0.75):
        s = 0.1 + 0.8 * shape
        _, tr = render(tone_in(sr), dict(voice=1, rate=4.0, depth=1.0, wave=1.0, shape=shape, width=0.0), sr)
        _, _, _, duty, rise, fall = stats(tr[0], 4.0, sr)
        ok &= abs(rise - 2.9) <= 0.5 and abs(fall - 2.9) <= 0.5 and abs(duty - s) <= 0.01
        rows.append(f"s {s:.1f}: rise {rise:.2f} fall {fall:.2f} ms, loud {duty:.3f}")
    report.add("T5" + tag, f"pulse edges and duty{' ' + str(sr) if tag else ''}", ok, "; ".join(rows))

    # Splatter at the contract's carriers (8 phases incl. cosine at 1 kHz, 997, 1001.5 Hz),
    # then a dense sweep of the carrier phase against the gate edge and off-grid carriers.
    keys = dict(voice=1, rate=4.0, depth=1.0, wave=1.0, shape=0.5, width=0.0)
    contract = [(1000.0, k * np.pi / 4) for k in range(8)] + [(997.0, 0.0), (1001.5, 0.0)]
    dense = [(1000.0, k * np.pi / 32) for k in range(64)]
    dense += [(f, k * np.pi / 4) for f in (997.0, 1001.5, 1003.3, 999.1, 1250.7) for k in range(8)]
    results = {}
    for f, ph in contract + ([] if tag else dense):
        if (f, ph) in results:
            continue
        out, _ = render(tone_in(sr, freq=f, phase=ph, channels=1), keys, sr)
        results[(f, ph)] = splatter_dbc(out, sr, f)
    wc = max(contract, key=lambda c: results[c])
    detail = f"worst {results[wc]:.1f} dBc at {wc[0]} Hz, phase {np.degrees(wc[1]):.0f} deg (bar {SPLATTER_CONTRACT})"
    ok = results[wc] <= SPLATTER_CONTRACT
    if not tag:
        wd = max(results, key=results.get)
        detail += f"; dense sweep ({len(results)} carriers) worst {results[wd]:.1f} dBc at {wd[0]} Hz, " \
                  f"phase {np.degrees(wd[1]):.1f} deg"
        ok &= results[wd] <= SPLATTER_CONTRACT
    report.add("T5" + tag, f"pulse splatter carrier+2..5 kHz{' ' + str(sr) if tag else ''}", ok, detail)
    return results


def t6_depth_step(report):
    sr, block = 48000, 512
    worst = 0.0
    for k in range(8):
        t = (90 + 11 * k) * block / sr
        _, tr = render(tone_in(sr), ["voice=1", "rate=4", "wave=0", "width=0", "depth=0", f"depth@{t!r}=1"], sr, block)
        worst = max(worst, float(np.max(np.abs(np.diff(tr[0])))))
    report.add("T6", "depth step 0 -> 1 between blocks", worst <= 0.005,
               f"largest per-sample gain step {worst:.5f} (8 step phases)")


def t7_optical(report, sr=48000, tag=""):
    _, tr = render(tone_in(sr), dict(voice=2, rate=5.0, depth=1.0, width=0.0), sr)
    gmax, gmin, _, duty, rise, fall = stats(tr[0], 5.0, sr)
    floor = fx.db(gmin)
    ok = abs(fall - 11.0) <= 1.5 and abs(rise - 44.0) <= 5.0 and abs(duty - 0.70) <= 0.03 and abs(floor + 20.0) <= 1.0
    detail = f"fall {fall:.2f} ms, rise {rise:.2f} ms, loud {duty:.3f}, floor {floor:.2f} dB, crest {gmax:.4f}"
    if not tag:
        envs = []
        for level in (-36.0, -18.0, -6.0):
            out, _ = render(tone_in(sr, level=level), dict(voice=2, rate=5.0, depth=1.0, width=0.0), sr)
            env = fx.envelope(out[0], sr, analytic_mag=True)[int(0.1 * sr):-int(0.1 * sr)]
            envs.append(fx.db(np.maximum(env, 1e-12)) - level)
        spread = float(max(np.max(np.abs(envs[0] - envs[1])), np.max(np.abs(envs[2] - envs[1]))))
        ok &= spread <= 0.1
        detail += f"; envelope -36/-18/-6 dBFS within {spread:.4f} dB"
    report.add("T7" + tag, f"Optical 5 Hz depth 1{' ' + str(sr) if tag else ''}", ok, detail)


def t8_t12_stereo(report):
    sr = 48000
    x = tone_in(sr)
    worst = 0.0
    for v in (1, 2, 3, 4):
        out, _ = render(x, dict(voice=v, rate=4.0, depth=1.0, width=0.0), sr)
        worst = max(worst, float(np.max(np.abs(out[0] - out[1]))))
    report.add("T8", "spread 0, voices 1-4", worst == 0.0, f"max |L - R| {worst:.3g}")

    _, tr = render(x, dict(voice=1, rate=4.0, depth=1.0, wave=0.0, width=1.0), sr)
    corr = fx.correlation(cycle(tr[0], 4.0, sr, 4), cycle(tr[1], 4.0, sr, 4))
    ripple = power_ripple(tr, 4.0, sr)
    report.add("T9", "Smooth sine spread 180 depth 1", corr < -0.9 and ripple <= 0.2,
               f"corr {corr:+.3f}, summed power ripple {ripple:.3f} dB")
    floors = [fx.db(cycle(g, 4.0, sr).min() / cycle(g, 4.0, sr).max()) for g in tr]
    report.add("T12", "stereo floor spread 180 depth 1", max(floors) <= -36.0,
               f"floor L {floors[0]:.1f} / R {floors[1]:.1f} dB re crest")

    rows = []
    ok = True
    for d in (0.25, 0.5, 0.75):
        _, tr = render(x, dict(voice=1, rate=4.0, depth=d, wave=0.0, width=1.0), sr)
        r = power_ripple(tr, 4.0, sr)
        ok &= r <= 0.7
        rows.append(f"{d}: {r:.2f} dB")
    report.add("T10", "spread 180, depth 0.25/0.5/0.75", ok, "summed power ripple " + ", ".join(rows))

    _, tr = render(x, dict(voice=1, rate=4.0, depth=1.0, wave=0.0, width=0.5), sr)
    lead = (phase_deg(tr[1], 4.0, sr) - phase_deg(tr[0], 4.0, sr) + 180.0) % 360.0 - 180.0
    swing = power_ripple(tr, 4.0, sr)
    report.add("T11", "spread 90", abs(lead - 90.0) <= 2.0, f"right leads by {lead:.2f} deg; power swing {swing:.1f} dB")


def t13_sync(report, sr=48000, tag=""):
    # 120 BPM, 1/8: the dispatcher sends 4 Hz and frac(ppq / 0.5) per block. The song
    # starts mid-beat (ppq 0.37) so the lock has something to correct.
    secs, p0 = 6.0, 0.37
    keys = ["voice=1", "rate=4", "depth=1", "wave=0", "width=0",
            f"sync_phase@0..{secs!r}={2 * p0!r}..{2 * p0 + 4 * secs!r}"]
    _, tr = render(tone_in(sr, secs=secs), keys, sr)
    g = tr[0]
    grid = [(0.5 * k - p0) / 2.0 for k in range(1, 100)]
    grid = [t for t in grid if 0.1 < t < secs - 0.1]
    errs, mins = [], []
    for t in grid:
        a, b = int((t - 0.06) * sr), int((t + 0.06) * sr)
        i = a + int(np.argmin(g[a:b]))
        mins.append(i / sr)
        errs.append(abs(i / sr - t) * 1e3)
    rate = (len(mins) - 1) / (mins[-1] - mins[0])
    err_pct = abs(rate - 4.0) / 4.0 * 100
    ok = err_pct <= 0.1 and max(errs) <= 2.0
    report.add("T13" + tag, f"sync 120 BPM 1/8{' ' + str(sr) if tag else ''}", ok,
               f"{rate:.4f} Hz ({err_pct:.3f} %), dips within {max(errs):.2f} ms of the eighths")


def t14_bias(report):
    sr = 48000
    depths = {}
    for level in (-40.0, -18.0, -6.0):
        _, tr = render(tone_in(sr, freq=440.0, level=level), dict(voice=3, rate=5.0, depth=0.75, shape=0.5, width=0.0), sr)
        c = cycle(tr[0], 5.0, sr, 4)
        depths[level] = float(fx.db(c.max() / c.min()))
    ok = abs(depths[-40.0] - 5.5) <= 0.5 and abs(depths[-6.0] - 1.25) <= 0.4
    report.add("T14", "Bias 440 Hz depth 0.75 knee -12", ok,
               "depth " + ", ".join(f"{d:.2f} dB at {lv:.0f} dBFS" for lv, d in depths.items()))
    return depths


def t15_harmonic(report):
    sr = 48000
    x = as_f32(fx.white(4.0, -18.0, sr))
    out, _ = render(np.vstack([x, x]), dict(voice=4, rate=4.0, depth=0.0, width=0.0), sr)
    null = rel_db(out[0] - x, x)

    envs, swings = [], []
    for f in (82.0, 2000.0):
        out, _ = render(tone_in(sr, secs=3.0, freq=f), dict(voice=4, rate=4.0, depth=1.0, width=0.0), sr)
        env = fx.envelope(out[0], sr, analytic_mag=True)
        n = int(round(sr / f)) * 2
        env = np.convolve(env, np.ones(n) / n, mode="same")[int(1.0 * sr):int(2.75 * sr)]
        envs.append(env)
        swings.append(float(fx.db(env.max() / env.min())))
    corr = fx.correlation(envs[0], envs[1])

    def ripple(lo, hi):
        # 60 s of band-limited pink noise, power ratio out/in folded onto 32 LFO-phase bins.
        # Pink noise is heavy at the bottom of its band, so even 240 cycles leave a few
        # tenths of a dB of jitter per bin; max - min of the raw bins would read that as
        # ripple. The voice's power ratio is linear in u, i.e. a sinusoid in LFO phase,
        # so the ripple is read from a least-squares sinusoid through the bins (the raw
        # max - min is reported beside it).
        secs = 60.0
        n = int(secs * sr)
        spec = np.fft.rfft(fx.pink(secs, -18.0, sr))
        freqs = np.fft.rfftfreq(n, 1.0 / sr)
        spec[(freqs < lo) | (freqs > hi)] = 0.0
        p = np.fft.irfft(spec, n)
        p = as_f32(p / np.sqrt(np.mean(p ** 2)) * fx.amp(-18.0))
        out, _ = render(np.vstack([p, p]), dict(voice=4, rate=4.0, depth=1.0, width=0.0), sr)
        skip = int(0.5 * sr)
        bins = (np.floor((np.arange(n) / sr * 4.0 % 1.0) * 32).astype(int))[skip:]
        ratio = np.bincount(bins, out[0][skip:] ** 2, 32) / np.bincount(bins, p[skip:] ** 2, 32)
        ph = 2 * np.pi * (np.arange(32) + 0.5) / 32
        basis = np.vstack([np.ones(32), np.cos(ph), np.sin(ph)]).T
        c, *_ = np.linalg.lstsq(basis, ratio, rcond=None)
        swing = np.hypot(c[1], c[2])
        raw = 10 * np.log10(ratio.max() / ratio.min())
        return float(10 * np.log10((c[0] + swing) / (c[0] - swing))), float(raw)

    pink_ripple, pink_raw = ripple(80.0, 5000.0)
    full_ripple, _ = ripple(20.0, 20000.0)
    ok = null <= -100.0 and min(swings) >= 10.0 and corr < -0.9 and pink_ripple <= 3.5
    report.add("T15", "Harmonic", ok,
               f"depth 0 null {null:.1f} dB; depth 1 swing 82 Hz {swings[0]:.1f} dB, 2 kHz {swings[1]:.1f} dB, "
               f"corr {corr:+.3f}; pink 80 Hz-5 kHz ripple {pink_ripple:.2f} dB (raw bins {pink_raw:.2f}; "
               f"20 Hz-20 kHz {full_ripple:.2f} dB)")


def t16_switch(report):
    sr, block = 48000, 512
    t_switch = 94 * block / sr
    # (a) mid-note switches between every voice and the next at moderate settings.
    worst, where = 0.0, ""
    for a, b in ((0, 1), (1, 2), (2, 3), (3, 4), (4, 0), (1, 4)):
        keys = [f"voice={a}", f"voice@{t_switch!r}={b}", "rate=4", "depth=0.5", "mix=0.35", "width=0", "wave=0"]
        out, _ = render(tone_in(sr), keys, sr, block)
        env = sliding_rms_db(out[0], sr, 1.0)
        step = np.abs(env[int(round(sr * 0.001)):] - env[:-int(round(sr * 0.001))])
        lo = int((t_switch - 0.1) * sr)
        s = float(step[lo:lo + int(0.3 * sr)].max())
        if s > worst:
            worst, where = s, f"{TYPES[a]} -> {TYPES[b]}"
    report.add("T16", "voice switch mid-note", worst <= 3.0, f"largest 1 ms level step {worst:.2f} dB ({where})")

    # (b) fade length, from the per-sample gain c = y / x read where the carrier is near
    # its peaks. At rate 0.01 Hz with the LFO held near phase 0 by a song position (a reset
    # starts each voice at its crest, Tremolo.h), Smooth's pulse sits in its gap (c = 0) and
    # Optical is dark (c = 1). The switch is one crossfade, so either direction measures it
    # whole.
    def gain_curve(a, b, ts, blk):
        keys = [f"voice={a}", f"voice@{ts / sr!r}={b}", "rate=0.01", "depth=1", "width=0", "wave=1",
                "sync_phase@0..2=0..0.02"]
        x = tone_in(sr)
        out, _ = render(x, keys, sr, blk)
        valid = np.nonzero(np.abs(x[0]) >= 0.7 * np.max(np.abs(x[0])))[0]
        return valid, out[0][valid] / x[0][valid]

    rows, ok = [], True
    for blk in (64, 512):
        ts = int(round(t_switch * sr / blk)) * blk
        valid, c = gain_curve(1, 2, ts, blk)
        fade_out = (valid[np.nonzero(c < 0.999)[0][-1]] - ts) / sr * 1e3
        valid, c = gain_curve(2, 1, ts, blk)
        floor = np.median(c[valid > ts + int(0.1 * sr)])
        total = (valid[np.nonzero(c > floor + 0.001)[0][-1]] - ts) / sr * 1e3
        ok &= abs(total - 40.0) <= 2.0
        rows.append(f"block {blk}: out {fade_out:.1f} ms, whole switch {total:.1f} ms")
    report.add("T16", "voice switch fade length", ok, ", ".join(rows))


def x1_rates(report):
    sr = 48000
    rows, worst = [], 0.0
    for rate, secs in ((0.1, 21.0), (0.5, 6.0), (2.0, 6.0), (4.0, 6.0), (10.0, 6.0), (20.0, 6.0)):
        _, tr = render(tone_in(sr, secs=secs), dict(voice=1, rate=rate, depth=1.0, wave=0.0, width=0.0), sr)
        g = tr[0] - np.mean(tr[0])
        up = np.nonzero((g[:-1] < 0) & (g[1:] >= 0))[0]
        t = (up + (-g[up]) / (g[up + 1] - g[up])) / sr
        measured = (len(t) - 1) / (t[-1] - t[0])
        err = abs(measured - rate) / rate * 100
        worst = max(worst, err)
        rows.append(f"{rate:g}: {measured:.5f}")
    report.add("X1", "rate accuracy 0.1-20 Hz (bar 0.1 %)", worst <= 0.1, f"worst {worst:.4f} %; Hz " + ", ".join(rows))


def x2_depth_law(report):
    """Depth across the knob, Smooth sine: the trace against tremolo_proto.voice_smooth (the
    law (1 - d)^2 with +3 dB * d make-up, through the 120 Hz smoother, which lifts the
    deepest floors a little above the bare law) within the verifier's 0.15 dB."""
    sr = 48000
    n = 2 * sr
    worst = 0.0
    rows = []
    for d in (0.1, 0.25, 0.5, 0.75, 0.9, 0.97):
        _, tr = render(tone_in(sr), dict(voice=1, rate=4.0, depth=d, wave=0.0, width=0.0), sr)
        gmax, gmin, _, _, _, _ = stats(tr[0], 4.0, sr)
        rmax, rmin, _, _, _, _ = stats(proto.voice_smooth(4.0, f32(d), n), 4.0, sr)
        floor, crest = fx.db(gmin / gmax), fx.db(gmax)
        worst = max(worst, abs(floor - fx.db(rmin / rmax)), abs(crest - fx.db(rmax)))
        rows.append(f"{d}: {floor:.2f}/{crest:+.2f} (law {fx.db((1 - d) ** 2):.1f}/{3 * d:+.2f})")
    report.add("X2", "depth law across the knob (bar 0.15 dB)", worst <= 0.15,
               f"floor re crest / crest dB {', '.join(rows)}; worst error against the reference {worst:.4f} dB")


def x3_transparent(report):
    sr = 48000
    x = as_f32(fx.music(sr))
    rows, ok = [], True
    for v in (1, 2, 3, 4):
        out, _ = render(x, dict(voice=v, depth=0.0, rate=4.0, width=1.0, wave=0.5), sr)
        diff = float(np.max(np.abs(out - x)))
        d = rel_db(out - x, x) if diff > 0 else -np.inf
        ok &= d <= -100.0
        rows.append(f"{TYPES[v]} {'bit-exact' if diff == 0 else f'{d:.0f} dB'}")
    report.add("X3", "depth 0 transparent (bar -100 dB)", ok, ", ".join(rows))


def x4_zipper(report):
    """Block-rate zipper. Automation reaches the engine once per block, so a knob swept
    across its range in 1 s arrives as a staircase at fs / 512. The gain trace of that
    render minus the same sweep delivered every sample (block 1) is the staircase's
    residue; its lines at k * fs / 512 are the AM sidebands it puts around any note, in dBc
    (a gain ripple of amplitude r gives two sidebands of r / 2). Bar -60 dBc [choice]."""
    sr, block = 48000, 512
    autos = {"depth": ("depth", "depth@0.5..1.5=0..1"), "rate": ("rate", "rate@0.5..1.5=1..10"),
             "wave": ("wave", "wave@0.5..1.5=0..1"), "shape": ("shape", "shape@0.5..1.5=0.1..0.9")}
    # Rate reaches every voice through the same phase counter, so it is measured where the
    # LFO is smooth. Optical's lamp switches hard: a staircase and a ramp of rate move its
    # edges by a fraction of a sample, and that edge shift, not zipper, fills the residue.
    reads = {1: ("depth", "rate", "wave", "shape"), 2: ("depth",), 3: ("depth", "rate", "shape"),
             4: ("depth", "rate")}
    a, b = int(0.6 * sr), int(1.5 * sr)
    t = np.arange(b - a) / sr
    win = np.hanning(b - a)
    rows, ok = [], True
    for v, names in reads.items():
        worst, which = -300.0, ""
        for name in names:
            base = {"voice": v, "depth": 0.75, "rate": 4, "width": 0, "wave": 0, "shape": 0.5}
            key, auto = autos[name]
            base.pop(key)
            keys = [f"{k}={val}" for k, val in base.items()] + [auto]
            _, ga = render(tone_in(sr), keys, sr, block)
            _, gb = render(tone_in(sr), keys, sr, 1)
            d = (ga[0] - gb[0])[a:b]
            level = np.mean(np.abs(gb[0][a:b]))
            for k in range(1, 9):
                amp = 2 * np.abs(np.sum(d * win * np.exp(-2j * np.pi * k * sr / block * t))) / np.sum(win)
                z = float(fx.db(amp / 2 / max(level, 1e-9)))
                if z > worst:
                    worst, which = z, name
        ok &= worst <= -60.0
        rows.append(f"{TYPES[v]} {worst:.0f} dBc ({which})")
    report.add("X4", "zipper under automation (bar -60 dBc)", ok, ", ".join(rows))


def summed_ripple(y, sr, rate, ms):
    """Ripple of L^2 + R^2 over the last LFO cycle, from sliding RMS over `ms` (a whole
    number of periods of every tone in the input)."""
    p = 10 ** (sliding_rms_db(y[0], sr, ms) / 10) + 10 ** (sliding_rms_db(y[1], sr, ms) / 10)
    c = cycle(p, rate, sr)
    return float(10 * np.log10(c.max() / c.min()))


def x5_stereo(report):
    """What each voice guarantees under spread.

    Smooth (loudness law w^(1 - spread/360), §4.7.4): T9-T12. The table below is printed.
    Bias and Harmonic (no exponent): L^2 + R^2 constant at 180 degrees at every depth and
    level (Bias: both channels share one envelope; Harmonic: band powers linear in u, for
    any spectrum). Bar 0.2 dB, T9's. Optical: the lamp is lit a quarter of the cycle, so
    no power guarantee; its figure is reported."""
    sr = 48000
    x = tone_in(sr)
    widths = (0.0, 0.25, 0.5, 0.75, 1.0)
    print("\nstereo summed-power ripple L^2+R^2 (dB), Smooth sine 4 Hz; rows depth, columns spread deg")
    print("depth\t" + "\t".join(str(int(180 * w)) for w in widths))
    table = {}
    for d in (0.25, 0.5, 0.75, 1.0):
        vals = []
        for w in widths:
            _, tr = render(x, dict(voice=1, rate=4.0, depth=d, wave=0.0, width=w), sr)
            vals.append(power_ripple(tr, 4.0, sr))
        table[d] = vals
        print(f"{d}\t" + "\t".join(f"{v:.2f}" for v in vals))
    ok = table[1.0][-1] <= 0.2 and all(table[d][-1] <= 0.7 for d in (0.25, 0.5, 0.75))
    report.add("X5", "Smooth stereo power table (T9/T10 bars)", ok,
               f"180 deg: {', '.join(f'd{d} {table[d][-1]:.2f}' for d in table)}; 90 deg d1 {table[1.0][2]:.1f} dB")

    rows, worst = [], 0.0
    for level in (-40.0, -18.0, -6.0):
        for d in (0.5, 0.75, 1.0):
            out, _ = render(tone_in(sr, level=level), dict(voice=3, rate=4.0, depth=d, width=1.0), sr)
            r = summed_ripple(out, sr, 4.0, 1.0)
            worst = max(worst, r)
            rows.append(f"{level:.0f}/{d}: {r:.3f}")
    report.add("X5", "Bias spread 180 summed power (bar 0.2 dB)", worst <= 0.2,
               f"worst {worst:.3f} dB; dBFS/depth " + ", ".join(rows))

    rows, worst = [], 0.0
    mix = as_f32(np.vstack([fx.sine(100.0, LEVEL, 2.0, sr) + fx.sine(2000.0, LEVEL, 2.0, sr)] * 2))
    for name, inp in (("1 kHz", x), ("100 Hz + 2 kHz", mix)):
        for d in (0.5, 0.8, 1.0):
            out, _ = render(inp, dict(voice=4, rate=4.0, depth=d, width=1.0), sr)
            r = summed_ripple(out, sr, 4.0, 10.0)
            worst = max(worst, r)
            rows.append(f"{name} d{d}: {r:.3f}")
    report.add("X5", "Harmonic spread 180 summed power (bar 0.2 dB)", worst <= 0.2,
               f"worst {worst:.3f} dB; " + ", ".join(rows))

    out, _ = render(x, dict(voice=2, rate=4.0, depth=1.0, width=1.0), sr)
    print(f"Optical spread 180 depth 1 summed-power ripple (no guarantee): {summed_ripple(out, sr, 4.0, 1.0):.2f} dB")


def x6_shapes(report):
    """Envelope shape per voice at its starting point (rate, depth from Tremolo.h), 1 kHz."""
    sr = 48000
    print("\nenvelope per voice at its starting point (gain trace, last cycle)")
    print("voice\trate\tdepth\tcrest_db\tfloor_db_re_crest\tloud\trise_ms\tfall_ms")
    starts = {1: (4.0, 0.6), 2: (5.0, 0.7), 3: (5.0, 0.75), 4: (4.0, 0.8)}
    for v, (rate, depth) in starts.items():
        _, tr = render(tone_in(sr), dict(voice=v, rate=rate, depth=depth, width=0.0), sr)
        gmax, gmin, _, duty, rise, fall = stats(tr[0], rate, sr)
        print(f"{TYPES[v]}\t{rate}\t{depth}\t{fx.db(gmax):+.2f}\t{fx.db(gmin / gmax):.2f}\t{duty:.3f}\t{rise:.2f}\t{fall:.2f}")
    for wave in (0.5, 1.0):
        _, tr = render(tone_in(sr), dict(voice=1, rate=4.0, depth=1.0, wave=wave, width=0.0), sr)
        gmax, gmin, _, duty, rise, fall = stats(tr[0], 4.0, sr)
        print(f"Smooth w{wave}\t4.0\t1.0\t{fx.db(gmax):+.2f}\t{fx.db(max(gmin, 1e-9) / gmax):.2f}\t{duty:.3f}\t"
              f"{rise:.2f}\t{fall:.2f}")


# ---------------------------------------------------------------------------- rows after verification

VOICE_START = {1: (4.0, 0.6), 2: (5.0, 0.7), 3: (5.0, 0.75), 4: (4.0, 0.8)}   # Tremolo.h rate, depth


def m1_mono(report):
    """A mono block has nothing to spread: Width must not change a sample, in any voice, and
    Smooth at depth 1 reaches the floor of stereo at width 0 (T3's)."""
    sr = 48000
    x = as_f32(fx.music(sr))[:1]
    rows, ok = [], True
    for v in TYPES:
        outs = [render(x, dict(voice=v, rate=4.0, depth=0.8, wave=0.3, width=w), sr)[0] for w in (0.0, 0.37, 1.0)]
        diff = max(float(np.max(np.abs(o - outs[0]))) for o in outs[1:])
        ok &= diff == 0.0
        rows.append(f"{TYPES[v]} {diff:.3g}")
    report.add("M1", "mono: width 0/0.37/1 identical, every voice", ok, "max |diff| " + ", ".join(rows))

    keys = dict(voice=1, rate=4.0, depth=1.0, wave=0.0)
    _, mono = render(tone_in(sr, channels=1), dict(keys, width=1.0), sr)
    _, st = render(tone_in(sr), dict(keys, width=0.0), sr)
    floors = []
    for g in (mono[0], st[0]):
        gmax, gmin, _, _, _, _ = stats(g, 4.0, sr)
        floors.append(fx.db(max(gmin, 1e-12) / gmax))
    ok = floors[0] <= -60.0 and abs(floors[0] - floors[1]) <= 0.01
    report.add("M1", "mono Smooth depth 1 at width 1: floor of stereo width 0", ok,
               f"mono {floors[0]:.1f} dB, stereo width 0 {floors[1]:.1f} dB re crest")


def r1_reenable(report):
    """The block switched off and on as the processor does it: setParameters() and process()
    stop while bypassed, and on the way back the processor calls reset() on voices 1-4
    (the adapter calls it between setParameters() and process()).
    (a) A loud note, bypass at 1.0 s, input silent from 1.2 s, on again at 2.2 s: nothing
        of the old note may come back (bar -140 dBFS).
    (b) Input that keeps playing, with rate and depth moved while bypassed: from the
        re-enable sample on, the output equals a freshly prepared engine's on the same
        input, bit for bit (no stale filter, envelope, phase or knob ramp).
    (c) The gain across the re-entry, unity while bypassed, steps by no more than the
        120 Hz gain smoother's coefficient (a full-swing pulse edge) per sample."""
    sr, block = 48000, 512
    n = int(3.5 * sr)
    t_off = int(np.ceil(1.0 * sr / block)) * block
    t_on = int(np.ceil(2.2 * sr / block)) * block
    note = fx.sine(110.0, -6.0, 3.5, sr, 0.7) + fx.pink(3.5, -20.0, sr, seed=31)
    loud = note.copy()
    loud[int(1.2 * sr):] = 0.0
    loud = as_f32(np.vstack([loud, loud]))
    playing = as_f32(np.vstack([note, fx.pink(3.5, -20.0, sr, seed=32) + note]))
    bound = 1.0 - np.exp(-2 * np.pi * 120.0 / sr)
    rows, ok = [], True
    for v, (rate, depth) in VOICE_START.items():
        onoff = [f"on@{t_off / sr!r}=0", f"on@{t_on / sr!r}=1"]
        out, _ = render(loud, [f"voice={v}", f"rate={rate}", f"depth={depth}", "width=0"] + onoff, sr, block)
        stale = float(fx.db(max(float(np.max(np.abs(out[:, t_on:]))), 1e-30)))

        after = [f"voice={v}", "rate=6", "depth=0.9", "width=0.5", "wave=0.5"]
        keys = [f"voice={v}", "width=0.5", "wave=0.5", "rate=2", "depth=0.3", f"rate@{t_off / sr!r}=6",
                f"depth@{t_off / sr!r}=0.9"] + onoff
        out, tr = render(playing, keys, sr, block)
        fresh, _ = render(playing[:, t_on:], after, sr, block)
        same = int(np.sum(out[:, t_on:] != fresh))
        g = tr[0].copy()
        g[t_off:t_on] = 1.0
        step = float(np.max(np.abs(np.diff(g[t_on - block:t_on + block]))))
        ok &= stale <= -140.0 and same == 0 and step <= bound
        rows.append(f"{TYPES[v]} {stale:.0f} dBFS / {same} / {step:.4f}")
    report.add("R1", "re-enable: stale peak / samples differing from fresh / gain step", ok,
               f"{', '.join(rows)} (bars -140 dBFS, 0, {bound:.4f})")


def s1_sync_jump(report):
    """A song-position jump (transport start, locate, loop) mid-cycle: honoured, without a
    click. -12 dBFS low tones, each voice at its starting point and at depth 1, jumps of
    0.1-0.75 cycle at six points of the cycle. The click is the peak of the output above
    2 kHz (clicklib's 1023-tap Kaiser high-pass) from 5 ms before the jump to 120 ms after.
    Bar -60 dBFS, or what the voice itself puts above 2 kHz in that window without a jump
    (Optical's lamp edges: the larger of the renders that stay on the old phase and that
    start on the new one), whichever is higher."""
    sr, block, secs = 48000, 512, 2.0
    taps = np.arange(1023) - 511
    hp = -np.sinc(2 * 2000.0 / sr * taps) * 2 * 2000.0 / sr
    hp[511] += 1.0
    hp *= np.kaiser(1023, 14.0)

    def above_2k(y, a, b):
        return float(fx.db(max(float(np.max(np.abs(np.convolve(y, hp, mode="same")[a:b]))), 1e-12)))

    def sync(offset, rate, te=None, jump=0.0):
        if te is None:
            return [f"sync_phase@0..{secs!r}={offset!r}..{offset + rate * secs!r}"]
        b = rate * te + jump
        return [f"sync_phase@0..{te!r}=0..{rate * te!r}", f"sync_phase@{te!r}..{secs!r}={b!r}..{b + rate * (secs - te)!r}"]

    jumps = (0.1, 0.25, 0.5, 0.75)
    rows, ok = [], True
    for v, (rate, start_depth) in VOICE_START.items():
        worst = (-999.0, -999.0, "")
        for depth in (start_depth, 1.0):
            base = [f"voice={v}", f"rate={rate}", f"depth={depth}", "width=0", "wave=0"]
            for f0 in (82.4, 220.0):
                x = as_f32(fx.sine(f0, -12.0, secs, sr, 0.7))[None, :]
                refs = {j: render(x, base + sync(j, rate), sr, block)[0][0] for j in (0.0,) + jumps}
                for jump in jumps:
                    for k in range(6):
                        te = round((1.2 + k / (6 * rate)) * sr / block) * block / sr
                        y = render(x, base + sync(0.0, rate, te, jump), sr, block)[0][0]
                        a, b = int((te - 0.005) * sr), int((te + 0.12) * sr)
                        click = above_2k(y, a, b)
                        natural = max(above_2k(refs[0.0], a, b), above_2k(refs[jump], a, b))
                        if click - max(-60.0, natural) > worst[0] - max(-60.0, worst[1]):
                            worst = (click, natural, f"depth {depth}, {f0} Hz, jump {jump}, at {k}/6 cycle")
        ok &= worst[0] <= max(-60.0, worst[1])
        rows.append(f"{TYPES[v]} {worst[0]:.1f} (own {worst[1]:.1f}; {worst[2]})")
    report.add("S1", "sync phase jump click above 2 kHz (dBFS)", ok, "; ".join(rows))


# Pulse splatter bound against the carrier, from the measured worst case (16 carrier phases,
# carriers on the 4 Hz grid so the phase at every edge is the same) plus 1 dB. The edge itself
# does not change with the carrier: the measure's band (carrier + 2..5 kHz) also catches the
# lower sideband of the edge, which for a low carrier lands at nearly the same offsets and adds
# coherently at the worst phase. Tightened after the raised-cosine edges (Y5): measured worst
# -85.8 dBc from 40 Hz, -87.0 from 82 Hz, -88.6 from 200 Hz, -90.1 from 500 Hz (the one-pole
# edge alone measured -55.8 / -56.1 / -56.9 / -58.4 there, under bars -55.5 / -55.8 / -56.4 / -58.0).
SPLATTER_BOUNDS = ((40.0, -84.8), (82.0, -86.0), (200.0, -87.6), (500.0, -89.1))   # (from Hz, bar dBc)


def t5_splatter_table(report):
    sr = 48000
    keys = dict(voice=1, rate=4.0, depth=1.0, wave=1.0, shape=0.5, width=0.0)
    carriers = (40.0, 48.0, 64.0, 84.0, 112.0, 148.0, 196.0, 248.0, 332.0, 440.0, 660.0, 1000.0, 1320.0, 2000.0)
    print("\npulse splatter, Smooth wave 1 depth 1 4 Hz, worst of 16 carrier phases (dBc)")
    print("carrier_hz\tworst\tbar")
    rows, ok = [], True
    for f in carriers:
        worst = max(splatter_dbc(render(tone_in(sr, freq=f, phase=k * np.pi / 8, channels=1), keys, sr)[0], sr, f)
                    for k in range(16))
        bar = [b for lo, b in SPLATTER_BOUNDS if f >= lo][-1]
        ok &= worst <= bar
        print(f"{f:g}\t{worst:.1f}\t{bar}")
        rows.append(f"{f:g} {worst:.1f}")
    bars = ", ".join(f">= {lo:g} Hz {b}" for lo, b in SPLATTER_BOUNDS)
    report.add("T5", "pulse splatter against the carrier 40 Hz-2 kHz", ok, f"dBc {', '.join(rows)} (bars {bars})")


def x7_depth_zero_after(report):
    """Depth automated down to 0 (a ramp, and Harmonic's step 1 -> 0): one second after the
    automation ends the output is the input, bit for bit, as from a fresh reset (X3)."""
    sr = 48000
    x = as_f32(fx.music(sr))
    rows, ok = [], True
    cases = [(v, ["depth=0.75", "depth@1.0..1.5=0.75..0"]) for v in (1, 2, 3, 4)] + [(4, ["depth=1", "depth@1.0=0"])]
    for v, auto in cases:
        out, _ = render(x, [f"voice={v}", "rate=4", "width=1", "wave=0.5"] + auto, sr)
        a = int(2.5 * sr)
        diff = float(np.max(np.abs(out[:, a:] - x[:, a:])))
        ok &= diff == 0.0
        label = f"{TYPES[v]}{' step' if '@1.0=' in auto[1] else ''}"
        rows.append(f"{label} {'bit-exact' if diff == 0 else f'{rel_db(out[:, a:] - x[:, a:], x[:, a:]):.1f} dB'}")
    report.add("X7", "depth automated to 0: transparent after 1 s", ok, ", ".join(rows))


def n1_non_finite(report):
    """Bursts of NaN and Inf in the input, then normal signal, every voice, block 512: a bad
    input sample may only make its own output sample non-finite, and 0.5 s after the last
    one the output is the clean render's (bar -100 dB rms)."""
    sr, n = 48000, 3 * 48000
    clean = as_f32(np.vstack([fx.pink(3.0, -18.0, sr, seed=21), fx.pink(3.0, -18.0, sr, seed=22)]))
    bad = clean.copy()
    bad[0, sr:sr + 37] = np.nan
    bad[:, int(1.5 * sr):int(1.5 * sr) + 100] = np.inf
    bad[1, int(1.7 * sr)] = -np.inf
    bad[0, int(1.8 * sr) + 5] = np.nan
    last = int(1.8 * sr) + 5
    rows, ok = [], True
    for v in TYPES:
        keys = dict(voice=v, depth=0.8, rate=5.0, width=0.5)
        ref, _ = render(clean, keys, sr)
        out, _ = render(bad, keys, sr)
        spread = int(np.sum(~np.isfinite(out) & np.isfinite(bad)))
        a = last + sr // 2
        tail_ok = bool(np.all(np.isfinite(out[:, a:])))
        diff = rel_db(out[:, a:] - ref[:, a:], ref[:, a:]) if tail_ok else np.inf
        if tail_ok and np.max(np.abs(out[:, a:] - ref[:, a:])) == 0.0:
            diff = -np.inf
        ok &= spread == 0 and diff <= -100.0
        rows.append(f"{TYPES[v]} {spread} / {'identical' if diff == -np.inf else f'{diff:.0f} dB'}")
    report.add("N1", "NaN/Inf bursts: extra non-finite samples / 0.5 s later vs clean", ok, ", ".join(rows))


# ---------------------------------------------------------------------------- rows after the second verification
# (.audit/fx-verify/r2-mod.md N4, N5, N9, N10, N11, N14, N15). Y1-Y3 drive the engine through the adapter's
# scripted play head (host_* keys, fx_tremolo.cpp), which resolves rate and sync phase as the processor does.

POOL = 8


def pmap(fn, jobs):
    with ThreadPoolExecutor(POOL) as ex:
        return list(ex.map(fn, jobs))


def hp_2k(y, sr):
    """|y| high-passed at 2 kHz: S1's Kaiser windowed sinc, its length scaled with the rate."""
    taps = int(1023 * sr / 48000) | 1
    k = np.arange(taps) - taps // 2
    h = -np.sinc(2 * 2000.0 / sr * k) * 2 * 2000.0 / sr
    h[taps // 2] += 1.0
    h *= np.kaiser(taps, 14.0)
    n = len(y) + taps - 1
    m = 1 << int(np.ceil(np.log2(n)))
    z = np.fft.irfft(np.fft.rfft(y, m) * np.fft.rfft(h, m), m)[taps // 2:taps // 2 + len(y)]
    return np.abs(z)


def peak_db(a, i0, i1):
    return float(fx.db(max(float(np.max(a[max(0, i0):i1])), 1e-12)))


SYNC_BASE = ["host_bpm=120", "host_beats=1", "width=0", "wave=0", "rate=3.3"]
SYNC_EVENTS = {
    # name: (constant keys, keys at the event time T)
    "engage": (["host_sync=0"], lambda T: [f"host_sync@{T!r}=1"]),
    "release": (["host_sync=1"], lambda T: [f"host_sync@{T!r}=0"]),
    "note 1/4->1/16": (["host_sync=1"], lambda T: [f"host_beats@{T!r}=0.25"]),
    "locate +0.37": (["host_sync=1"], lambda T: [f"host_ppq@{T!r}={2 * T + 0.37!r}"]),
    "loop -1.5 cycles": (["host_sync=1"], lambda T: [f"host_ppq@{T!r}={2 * T - 1.5!r}"]),
    "transport start": (["host_sync=1", "host_play=0"], lambda T: [f"host_play@{T!r}=1", f"host_ppq@{T!r}=2.73"]),
}


def optical_edges(sr, level, depth, rates, f0=110.0):
    """Optical's own lamp edges: the peak above 2 kHz of a steady take, worst of eight carrier
    phases (an edge's figure depends on where in the carrier's cycle it lands) and of the
    given LFO rates."""
    def one(job):
        rate, k = job
        x = as_f32(fx.sine(f0, level, 1.6, sr, k * np.pi / 4))[None, :]
        y = render(x, ["voice=2", f"depth={depth}", f"rate={rate}", "width=0"], sr, 512)[0][0]
        return peak_db(hp_2k(y, sr), int(0.3 * sr), int(1.57 * sr))
    return max(pmap(one, [(r, k) for r in rates for k in range(8)]))


def y1_sync_transitions(report):
    """N4: sync engaged and released, note value changed, locate, loop and transport start while
    a 110 Hz note plays, every voice at depth 1, host blocks 16-4096 at 48 kHz and 64/512/4096
    at 44.1, 96 and 192 kHz, two event instants each. Click = peak above 2 kHz from 5 ms before
    the event to 250 ms after. Bar -70 dBFS at -12 dBFS input, or what the voice itself puts
    above 2 kHz in the same take outside the event, whichever is higher; for Optical also its
    steady lamp edges at the worst carrier phase (optical_edges, at the take's 2, 3.3 and 8 Hz).
    The 0 dBFS takes (48 kHz, blocks 64 and 256) carry the same bar 12 dB up."""
    configs = [(48000, b) for b in (16, 64, 128, 256, 512, 1024, 4096)]
    configs += [(sr, b) for sr in (44100, 96000, 192000) for b in (64, 512, 4096)]
    jobs = [(v, ev, T, sr, b, -12.0) for v in (1, 2, 3, 4) for ev in SYNC_EVENTS for T in (1.0133, 1.2771)
            for sr, b in configs]
    jobs += [(v, ev, T, 48000, b, 0.0) for v in (1, 2, 3, 4) for ev in SYNC_EVENTS for T in (1.0133, 1.2771)
             for b in (64, 256)]

    def one(job):
        v, ev, T, sr, b, level = job
        const, at = SYNC_EVENTS[ev]
        secs = T + 0.55
        x = as_f32(fx.sine(110.0, level, secs, sr, 0.7))[None, :]
        keys = [f"voice={v}", "depth=1"] + SYNC_BASE + const + at(T)
        y = render(x, keys, sr, b)[0][0]
        h = hp_2k(y, sr)
        a, e = int((T - 0.005) * sr), int(secs * sr) - int(0.03 * sr)
        click = peak_db(h, a, int((T + 0.25) * sr))
        own = max(peak_db(h, int(0.3 * sr), a - int(0.05 * sr)), peak_db(h, int((T + 0.3) * sr), e))
        return job, click, own

    res = pmap(one, jobs)
    edges = {(sr, level): optical_edges(sr, level, 1.0, (2.0, 3.3, 8.0)) for sr, level in {(j[3], j[5]) for j in jobs}}
    res = [(j, click, max(own, edges[(j[3], j[5])]) if j[0] == 2 else own) for j, click, own in res]
    rows, ok = [], True
    for v in (1, 2, 3, 4):
        for level in (-12.0, 0.0):
            rr = [r for r in res if r[0][0] == v and r[0][5] == level]
            bar = -70.0 + (level + 12.0)
            w = max(rr, key=lambda r: r[1] - max(bar, r[2]))
            ok &= all(r[1] <= max(bar, r[2]) for r in rr)
            rows.append(f"{TYPES[v]} {level:.0f} dBFS {w[1]:.1f} (own {w[2]:.1f}; {w[0][1]}, "
                        f"{w[0][3] // 1000} kHz block {w[0][4]})")
    report.add("Y1", "sync transitions click above 2 kHz (dBFS)", ok, "; ".join(rows) + f" ({len(res)} takes)")


def meter_rows(meters):
    a = np.array([[float(c) for c in m.split("\t")] for m in meters])
    return a[:, 0], a[:, 1], a[:, 2]


def y2_sync_tracking(report):
    """The other half of N4 (and the chorus's N1): an imperfect host. Song position jittered by
    +-2 and +-8 samples per block, tempo stepped 1 BPM at a time and ramped 100 -> 140 BPM, at
    1/32 and 1/4, host blocks 16-4096 and 22.05-192 kHz: zero song-position jumps, and from one
    second on the LFO within 0.01 cycle of the true (unjittered) host phase."""
    steps = [f"host_bpm@{0.1 * k!r}={100 + k}" for k in range(1, 41)]
    cases = {"jitter 2": ["host_bpm=140", "host_jitter=2"], "jitter 8": ["host_bpm=140", "host_jitter=8"],
             "1 BPM steps": ["host_bpm=100"] + steps, "ramp 100-140": ["host_bpm@0..4=100..140"]}
    configs = [(48000, b) for b in (16, 64, 128, 256, 512, 1024, 4096)]
    configs += [(22050, 4096), (44100, 64), (44100, 4096), (96000, 512), (192000, 64), (192000, 4096)]
    jobs = [(c, beats, sr, b) for c in cases for beats in (0.125, 1.0) for sr, b in configs]

    def one(job):
        c, beats, sr, b = job
        keys = ["voice=1", "depth=1", "host_sync=1", f"host_beats={beats}"] + cases[c]
        r = fx.invoke(EFFECT, "gen:silence", keys, [f"--block={b}", "--meters", "--seconds=5", f"--sr={sr}"], sr)
        start, jumps, err = meter_rows(r.meters)
        late = start >= sr
        return job, int(jumps[-1] - jumps[0]), float(np.max(np.abs(err[late])))

    res = pmap(one, jobs)
    rows, ok = [], True
    for c in cases:
        rr = [r for r in res if r[0][0] == c]
        jumps = sum(r[1] for r in rr)
        w = max(rr, key=lambda r: r[2])
        ok &= jumps == 0 and w[2] < 0.01
        rows.append(f"{c}: {jumps} jumps, error {w[2]:.4f} (1/{int(4 / w[0][1])}, {w[0][2] // 1000} kHz block {w[0][3]})")
    report.add("Y2", "sync under host jitter and tempo changes", ok, "; ".join(rows) + f" ({len(res)} takes)")


def frames_db(g, sr, ms=5.0):
    n, hop = int(sr * ms / 1000), sr // 1000
    c = np.concatenate([[0.0], np.cumsum(g * g)])
    i = np.arange(0, len(g) - n, hop)
    return i, np.maximum(10 * np.log10(np.maximum((c[i + n] - c[i]) / n, 1e-30)), -40.0)


def y3_voice_switch(report):
    """N5: every ordered pair of the five voices at depth 0.6 and 1, switched at four LFO phases.
    The play head pins every voice's LFO to one song position (voices 1-4 lock to it; Classic
    runs from its own reset phase, which the position matches), so each voice alone gives its own
    trajectory. Level from a quadrature probe (2917 Hz, -12 dBFS, width 0) in 5 ms frames, floored
    at -40 dB: from 5 ms before the switch to 100 ms after it must stay within 1 dB of the span of
    the two trajectories. Click: peak above 2 kHz of a 110 Hz -12 dBFS note over the same window,
    bar -62 dBFS or the two voices' own figure in that window, whichever is higher; with Optical
    on either side also its steady lamp edges at the worst carrier phase (optical_edges)."""
    sr, block, rate = 48000, 64, 4.0
    secs = 1.5
    base = ["host_sync=1", "host_bpm=240", "host_beats=1", "host_ppq=0.25", "width=0", "wave=0", "mix=0.35"]
    t = np.arange(int(secs * sr)) / sr
    amp = fx.amp(-12.0)
    quad = as_f32(np.vstack([amp * np.sin(2 * np.pi * 2917.0 * t), amp * np.cos(2 * np.pi * 2917.0 * t)]))
    note = as_f32(fx.sine(110.0, -12.0, secs, sr, 0.7))[None, :]
    instants = [1.0 + k / (4 * rate) + 0.0007 for k in range(4)]

    def gain(y):
        return np.sqrt(y[0] ** 2 + y[1] ** 2) / amp

    def ref(job):
        v, d = job
        keys = [f"voice={v}", f"depth={d}"] + base
        return job, gain(render(quad, keys, sr, block)[0]), hp_2k(render(note, keys, sr, block)[0][0], sr)

    refs = {r[0]: r[1:] for r in pmap(ref, [(v, d) for v in TYPES for d in (0.6, 1.0)])}

    def one(job):
        a, b, d, T = job
        keys = [f"voice={a}", f"voice@{T!r}={b}", f"depth={d}"] + base
        g = gain(render(quad, keys, sr, block)[0])
        h = hp_2k(render(note, keys, sr, block)[0][0], sr)
        i, L = frames_db(g, sr)
        _, La = frames_db(refs[(a, d)][0], sr)
        _, Lb = frames_db(refs[(b, d)][0], sr)
        t0 = int(np.ceil(T * sr / block)) * block
        m = (i >= t0 - sr // 200) & (i < t0 + sr // 10)
        out = float(np.max(np.maximum(L - np.maximum(La, Lb), np.minimum(La, Lb) - L)[m]))
        a0, a1 = t0 - sr // 200, t0 + sr // 10
        click = peak_db(h, a0, a1)
        own = max(peak_db(refs[(a, d)][1], a0, a1), peak_db(refs[(b, d)][1], a0, a1))
        return job, out, click, own

    res = pmap(one, [(a, b, d, T) for a in TYPES for b in TYPES if a != b for d in (0.6, 1.0) for T in instants])
    edges = {d: optical_edges(sr, -12.0, d, (rate,)) for d in (0.6, 1.0)}
    res = [(j, out, click, max(own, edges[j[2]]) if 2 in j[:2] else own) for j, out, click, own in res]
    wl = max(res, key=lambda r: r[1])
    wc = max(res, key=lambda r: r[2] - max(-62.0, r[3]))
    ok = all(r[1] <= 1.0 and r[2] <= max(-62.0, r[3]) for r in res)
    pairs = {}
    for r in res:
        pairs[(r[0][0], r[0][1])] = max(pairs.get((r[0][0], r[0][1]), -99.0), r[1])
    nxt = sorted(pairs.items(), key=lambda kv: -kv[1])[1:3]
    report.add("Y3", "voice switch: level outside both trajectories / click", ok,
               f"worst {wl[1]:.2f} dB ({TYPES[wl[0][0]]} -> {TYPES[wl[0][1]]}, depth {wl[0][2]}); click {wc[2]:.1f} dBFS "
               f"(own {wc[3]:.1f}; {TYPES[wc[0][0]]} -> {TYPES[wc[0][1]]}); next pairs "
               + ", ".join(f"{TYPES[a]}->{TYPES[b]} {v:.1f}" for (a, b), v in nxt) + f" ({len(res)} switches)")


def y4_switch_on(report):
    """N10: switched on (reset) without sync, the LFO starts where the voice's gain is at its
    crest: 20 ms later the gain is within 1 dB of the voice's steady crest (gain trace; Harmonic:
    the low band), voices 1-4 at their starting rates, depth 0.6 and 1."""
    sr, block = 48000, 64
    rows, ok = [], True
    for v, (rate, _) in VOICE_START.items():
        for d in (0.6, 1.0):
            keys = [f"voice={v}", f"rate={rate}", f"depth={d}", "width=0", "wave=0", "on@0.5=0", "on@1.0=1"]
            _, tr = render(tone_in(sr, secs=2.0), keys, sr, block)
            t_on = int(np.ceil(1.0 * sr / block)) * block
            g = tr[0]
            crest = float(np.max(g[t_on + sr // 2:t_on + sr // 2 + int(sr / rate)]))
            at = float(fx.db(g[t_on + int(0.02 * sr)] / crest))
            ok &= at >= -1.0
            rows.append(f"{TYPES[v]} d{d} {at:+.1f}")
    report.add("Y4", "switch-on: gain 20 ms after re-enable re crest (dB)", ok, ", ".join(rows))


def hf_dbc(y, sr, skip):
    y = y[skip:]
    spec = np.abs(np.fft.rfft(y * np.hanning(len(y))))
    freqs = np.fft.rfftfreq(len(y), 1.0 / sr)
    return float(fx.db(np.sqrt(np.sum(spec[freqs >= 2000.0] ** 2)) / spec.max()))


def y5_pulse_edges(report):
    """N9: pulse and square splatter against the edge time. A 110 Hz note at -12 dBFS through
    Smooth wave 1, depth 1, rates 1-20 Hz, duty 0.1-0.9, the LFO pinned to a song position so
    its phase is known. The engine's 10-90 % edge time at each rate is read from its gain trace
    at duty 0.5 (a full swing). The comparison is today's edge at that same time: the hard pulse
    on the same phase through a one-pole whose 10-90 % time is the measured one (y0 = 1), applied
    to the same note. Energy above 2 kHz re the note (Hann, after the first 0.25 s). Bar: at
    least 8 dB less than today's edge, every case."""
    sr = 48000
    print("\npulse edge: rate, duty, engine 10-90 % ms (duty 0.5), energy above 2 kHz dBc: engine, "
          "one-pole edge of that time, difference")
    rates, shapes = (1.0, 2.0, 4.0, 10.0, 20.0), (0.0, 0.25, 0.5, 0.75, 1.0)

    def one(job):
        rate, shape = job
        secs = 4.0 if rate < 3 else 2.0
        x = as_f32(fx.sine(110.0, -12.0, secs, sr, 0.3))[None, :]
        keys = ["voice=1", f"rate={rate}", "depth=1", "wave=1", f"shape={shape}", "width=0",
                f"sync_phase@0..{secs!r}=0..{rate * secs!r}"]
        out, tr = render(x, keys, sr)
        _, _, _, _, rise, fall = stats(tr[0], rate, sr)
        return job, 0.5 * (rise + fall), x[0], out[0]

    res = {r[0]: r[1:] for r in pmap(one, [(r, s) for r in rates for s in shapes])}
    ok, worst, edges = True, None, []
    for rate in rates:
        edge = res[(rate, 0.5)][0]
        edges.append(edge)
        a = 1.0 - np.exp(-np.log(9.0) / (edge * 1e-3 * sr))
        for shape in shapes:
            _, x, out = res[(rate, shape)]
            s = 0.1 + 0.8 * f32(shape)
            ph = rate * np.arange(len(x)) / sr
            hard = (((ph - 0.5 * s) % 1.0) < s).astype(float)
            today, z = np.empty_like(hard), 1.0
            for i, v in enumerate(hard):
                z += a * (v - z)
                today[i] = z
            eng, old = hf_dbc(out, sr, sr // 4), hf_dbc(x * today, sr, sr // 4)
            print(f"{rate:g}\t{s:.1f}\t{edge:.2f}\t{eng:.1f}\t{old:.1f}\t{old - eng:+.1f}")
            ok &= old - eng >= 8.0
            if worst is None or old - eng < worst[0]:
                worst = (old - eng, rate, s)
    report.add("Y5", "pulse splatter vs today's edge at the same 10-90 % time", ok,
               f"least improvement {worst[0]:.1f} dB (rate {worst[1]:g} Hz, duty {worst[2]:.1f}); "
               f"edge 10-90 % {min(edges):.2f}-{max(edges):.2f} ms")


def y6_cpu_width(report):
    """N15: every voice inside its budget at every width (48 kHz, block 64, stereo pink), Smooth
    also at wave 0.7; the figure with a song-position jump fade running all the time is shown.
    Each figure is the least of three --bench runs (each already the least of five), so a busy
    machine does not read as engine cost."""
    rows, ok = [], True
    jobs = [(v, w, wave) for v in (1, 2, 3, 4) for w in (0.0, 0.25, 0.5, 0.75, 1.0)
            for wave in ((0.0, 0.7) if v == 1 else (0.0,))]
    for v, w, wave in jobs:
        cpu = min(fx._bench(EFFECT, "gen:noise:pink:-18", dict(voice=v, rate=5.0, depth=0.8, width=w, wave=wave))
                  for _ in range(3))
        ok &= cpu is not None and cpu <= BUDGETS[v]
        rows.append((v, w, wave, cpu))
    fade = fx._bench(EFFECT, "gen:noise:pink:-18",
                     ["voice=1", "rate=5", "depth=0.8", "width=0.5", "sync_phase@0..10=0..3777.7"])
    worst = {v: max(r[3] for r in rows if r[0] == v) for v in (1, 2, 3, 4)}
    report.add("Y6", "CPU at widths 0-1 (% real time, worst)", ok,
               ", ".join(f"{TYPES[v]} {c:.3f}/{BUDGETS[v]}" for v, c in worst.items())
               + f"; Smooth w0.5 with a jump fade always on {fade:.3f}")


def y7_huge(report):
    """N14: one sample of 3e38 (and the pair 3e38, -3e38; and 1e7) at eight points of a 4 Hz cycle,
    in every voice: the output is finite and 0.5 s after the last one equals the clean render
    (bar -100 dB rms). Classic with a sample of
    9.9e5 (below the limit) is the legacy code's output bit for bit."""
    sr = 48000
    clean = as_f32(np.vstack([fx.pink(2.0, -18.0, sr, seed=41), fx.pink(2.0, -18.0, sr, seed=42)]))
    k = sr // 2
    rows, ok = [], True
    for v in TYPES:
        keys = dict(voice=v, depth=1.0, rate=4.0, width=0.5)
        ref, _ = render(clean, keys, sr)
        worst = "finite, identical"
        for burst in ((3e38,), (3e38, -3e38), (1e7,)):
            bad = clean.copy()
            for at in range(8):
                for j, val in enumerate(burst):
                    bad[:, k + at * sr // 32 + j] = val
            out, _ = render(bad, keys, sr)
            fin = bool(np.all(np.isfinite(out)))
            a = k + sr // 4 + sr // 2
            diff = rel_db(out[:, a:] - ref[:, a:], ref[:, a:]) if fin else np.inf
            if fin and np.max(np.abs(out[:, a:] - ref[:, a:])) == 0.0:
                diff = -np.inf
            ok &= fin and diff <= -100.0
            if not fin or diff > -np.inf:
                worst = f"{burst}: {'non-finite' if not fin else f'{diff:.0f} dB'}"
        rows.append(f"{TYPES[v]} {worst}")
    x = clean.copy()
    x[:, k] = 9.9e5
    a = fx.invoke(EFFECT, x, dict(voice=0), ["--block=512"], sr).out
    b = fx.invoke(EFFECT, x, dict(voice=0), ["--block=512", "--legacy"], sr).out
    same = int(np.sum(a != b))
    ok &= same == 0
    report.add("Y7", "huge finite samples / Classic below 1e6", ok,
               ", ".join(rows) + f"; Classic 9.9e5: {same} samples differ from legacy")


def y8_bias_dc(report):
    """N11: a DC offset on the input must not change Bias's depth (an AC-coupled amp never lets
    DC reach its bias): -40 dBFS 997 Hz, depth 1, shape 0.5, DC 0 / 0.01 / 0.1 / -0.1: depth
    within 0.5 dB of the DC-free take. The onset response is reported (the voice: Tremolo.h)."""
    sr = 48000
    depths = {}
    for dc in (0.0, 0.01, 0.1, -0.1):
        x = as_f32(fx.sine(997.0, -40.0, 3.0, sr) + dc)[None, :]
        _, tr = render(x, dict(voice=3, rate=5.0, depth=1.0, shape=0.5, width=0.0), sr)
        c = tr[0][2 * sr:]
        depths[dc] = float(fx.db(c.max() / c.min()))
    ok = all(abs(d - depths[0.0]) <= 0.5 for d in depths.values())
    steady = as_f32(fx.sine(196.0, -6.0, 3.0, sr))[None, :]
    onset = steady.copy()
    i0 = 2 * sr
    onset[:, :i0] = 0.0
    keys = ["voice=3", "rate=5", "depth=0.75", "shape=0.5", "width=0", "sync_phase@0..3=0..15"]
    _, g1 = render(onset, keys, sr, 64)
    _, g2 = render(steady, keys, sr, 64)
    dip = float(np.min(fx.db(g1[0][i0:i0 + sr // 20] / g2[0][i0:i0 + sr // 20])))
    report.add("Y8", "Bias depth with DC on the input (dB)", ok,
               ", ".join(f"DC {dc:+g}: {d:.2f}" for dc, d in depths.items()) + f"; onset on the dip (voice): {dip:+.1f} dB")


def z1_switch_robust(report):
    """--rt-check, --reset-repeat and finite output during voice switches, sync events and a
    bypass, every voice in turn, 22.05-192 kHz, blocks 1-4096, mono and stereo (gen:music); the
    same timeline under --bench."""
    timeline = ["voice=1", "rate=5", "depth=0.9", "width=0.5", "wave=0.6", "host_sync=0", "host_bpm=133",
                "voice@0.5=2", "voice@0.53=3", "voice@0.9=4", "voice@1.2=0", "voice@1.5=1", "voice@1.51=0",
                "host_sync@1.7=1", "voice@1.8=3", "host_beats@2.1=0.25", "host_ppq@2.4=1.37", "voice@2.45=4",
                "on@2.6=0", "on@2.7=1", "host_sync@2.9=0", "voice@2.95=2"]
    jobs = [(sr, b, ch) for sr in (22050, 44100, 96000, 192000) for b in (1, 37, 512, 4096) for ch in (1, 2)]

    def one(job):
        sr, b, ch = job
        opts = [f"--block={b}", f"--channels={ch}", "--seconds=3.2", f"--sr={sr}"]
        rt = fx.invoke(EFFECT, "gen:music", timeline, opts + ["--rt-check"], sr)
        rr = fx.invoke(EFFECT, "gen:music", timeline, opts + ["--reset-repeat"], sr)
        return job, rt.code, rr.code, bool(np.all(np.isfinite(rt.out)))

    res = pmap(one, jobs)
    bad = [r for r in res if r[1] != 0 or r[2] != 0 or not r[3]]
    cpu = fx._bench(EFFECT, "gen:noise:pink:-18", timeline)
    report.add("Z1", "rt-check / reset-repeat / finite during switches", not bad,
               f"{len(res)} runs, {len(bad)} failing" + (f" (first {bad[0]})" if bad else "")
               + f"; bench {cpu:.3f} % real time")


def specific_checks(report, args):
    only = args.types
    if only is None or 0 in only:
        t1_null_steps(report)
    if only is not None and not set(only) & {1, 2, 3, 4}:
        return
    t2_parity(report)
    t3_t4_smooth(report)
    t5_pulse(report)
    t6_depth_step(report)
    t7_optical(report)
    t8_t12_stereo(report)
    t13_sync(report)
    t14_bias(report)
    t15_harmonic(report)
    t16_switch(report)
    for sr in (44100, 96000):
        t3_t4_smooth(report, sr, " (T17)")
        t5_pulse(report, sr, " (T17)")
        t7_optical(report, sr, " (T17)")
        t13_sync(report, sr, " (T17)")
    x1_rates(report)
    x2_depth_law(report)
    x3_transparent(report)
    x4_zipper(report)
    x5_stereo(report)
    x6_shapes(report)
    m1_mono(report)
    r1_reenable(report)
    s1_sync_jump(report)
    t5_splatter_table(report)
    x7_depth_zero_after(report)
    n1_non_finite(report)
    y1_sync_transitions(report)
    y2_sync_tracking(report)
    y3_voice_switch(report)
    y4_switch_on(report)
    y5_pulse_edges(report)
    y6_cpu_width(report)
    y7_huge(report)
    y8_bias_dc(report)
    z1_switch_robust(report)


def main():
    # --specific (development aid): skip the common checks.
    specific_only = "--specific" in sys.argv
    if specific_only:
        sys.argv.remove("--specific")
    args = fx.parse_args(__doc__)
    report = fx.Report(EFFECT)
    if not specific_only:
        fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                         quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
    specific_checks(report, args)
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
