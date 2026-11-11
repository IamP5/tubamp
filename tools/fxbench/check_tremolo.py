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
"""
import sys
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

def ref_smooth(x, sr, rate, depth, wave, shape, width):
    n = x.shape[1]
    s = 0.1 + 0.8 * f32(shape)
    e = 1.0 - 0.5 * f32(width)
    out = []
    for c, start in enumerate((0.0, 0.5 * f32(width))):
        w = proto.w_morph(proto.phase(f32(rate), n, sr, start), f32(wave), s)
        g = proto.onepole(proto.apply_depth(w ** e, f32(depth)), proto.SLEW_HZ, sr=sr)
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
    n = x.shape[1]
    env = envelope_follower(np.abs(0.5 * (x[0] + x[1])), sr)
    e = exponent
    out = []
    for c, start in enumerate((0.0, 0.5 * f32(width))):
        u = 0.5 + 0.5 * np.cos(2 * np.pi * proto.phase(f32(rate), n, sr, start))
        g = proto.bias_gain(u ** e, f32(depth), env, knee_dbfs=-24.0 + 24.0 * f32(shape))
        out.append(x[c] * proto.onepole(g, proto.SLEW_HZ, sr=sr))
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
        gl = proto.onepole(norm * np.sqrt(1 - d * u), proto.SLEW_HZ, sr=sr)
        gh = proto.onepole(norm * np.sqrt(1 - d * (1 - u)), proto.SLEW_HZ, sr=sr)
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
    detail = f"worst {results[wc]:.1f} dBc at {wc[0]} Hz, phase {np.degrees(wc[1]):.0f} deg"
    ok = results[wc] <= -58.0
    if not tag:
        wd = max(results, key=results.get)
        detail += f"; dense sweep ({len(results)} carriers) worst {results[wd]:.1f} dBc at {wd[0]} Hz, " \
                  f"phase {np.degrees(wd[1]):.1f} deg"
        ok &= results[wd] <= -58.0
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
    # its peaks. Smooth at rate 0.01 Hz sits at its dip (c ~ 0); Optical at rate 0.01 Hz
    # is dark (c = 1, dry). Smooth -> Optical shows the fade-out alone; Optical -> Smooth
    # shows nothing until the fade-in, so its end measures the whole switch.
    def gain_curve(a, b, ts, blk):
        keys = [f"voice={a}", f"voice@{ts / sr!r}={b}", "rate=0.01", "depth=1", "width=0", "wave=0"]
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
