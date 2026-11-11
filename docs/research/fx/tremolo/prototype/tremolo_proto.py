#!/usr/bin/env python3
"""Numpy reference of tubamp's tremolo today and of the proposed voicings.

    python3 tremolo_proto.py            print every table
    python3 tremolo_proto.py legacy     one section: legacy | refs | shapes | optical | chop |
                                        bias | harmonic | stereo | depth

Nothing here is lifted from a vendor binary. The two "refs" models re-state, as equations, what
the report documents for Guitar Rig 6 (pulse into a two-rate one-pole) so their predictions can be
compared with the measured tables in ../evidence/. All constants of the proposed voicings are ours:
they come from circuit values, datasheets or are fitted to our own measurements (see report.md).
"""
import sys
import numpy as np

SR = 48000.0


# ----------------------------------------------------------------------------------------------
# helpers
# ----------------------------------------------------------------------------------------------
def phase(rate_hz, n, sr=SR, start=0.0):
    return (start + rate_hz * np.arange(n) / sr) % 1.0


def onepole(x, fc_up, fc_down=None, sr=SR, y0=None):
    """One-pole follower y += a*(x-y). Separate cutoffs while rising and falling (in y)."""
    fc_down = fc_up if fc_down is None else fc_down
    au = 1.0 - np.exp(-2 * np.pi * fc_up / sr)
    ad = 1.0 - np.exp(-2 * np.pi * fc_down / sr)
    y = np.empty_like(x)
    s = x[0] if y0 is None else y0
    for i, v in enumerate(x):
        s += (au if v > s else ad) * (v - s)
        y[i] = s
    return y


def db(v):
    return 20 * np.log10(np.maximum(np.abs(v), 1e-12))


def cycle_stats(g, rate_hz, sr=SR):
    """gmax, gmin, depth_db, duty_hi, rise_ms, fall_ms (10-90 % of the swing) of the last cycle."""
    per = int(round(sr / rate_hz))
    c = g[-per:]
    gmax, gmin = c.max(), c.min()
    mid = 0.5 * (gmax + gmin)
    lo, hi = gmin + 0.1 * (gmax - gmin), gmin + 0.9 * (gmax - gmin)
    c2 = np.roll(c, -int(np.argmin(c)))
    imax = int(np.argmax(c2))
    up, dn = c2[:imax + 1], c2[imax:]
    rise = (np.argmax(up >= hi) - np.argmax(up >= lo)) / sr * 1e3
    fall = (np.argmax(dn <= lo) - np.argmax(dn <= hi)) / sr * 1e3
    return gmax, gmin, db(gmax) - db(gmin), float(np.mean(c > mid)), rise, fall


def table(g, rate_hz, points=20, sr=SR):
    per = int(round(sr / rate_hz))
    c = g[-per:]
    c = np.roll(c, -int(np.argmin(c)))
    return np.interp(np.linspace(0, per - 1, points), np.arange(per), c)


def row(label, g, rate):
    gmax, gmin, d, duty, rise, fall = cycle_stats(g, rate)
    print(f"{label}\t{rate:.3f}\t{gmax:.4f}\t{gmin:.4f}\t{d:.2f}\t{duty:.3f}\t{rise:.2f}\t{fall:.2f}")


HEAD = "label\trate_hz\tgmax\tgmin\tdepth_db\tduty_hi\trise_ms\tfall_ms"


# ----------------------------------------------------------------------------------------------
# 0. tubamp today (src/dsp/FxBlocks.cpp:411-450), bit-for-bit in structure
# ----------------------------------------------------------------------------------------------
def legacy_gain(rate, depth, mix, n, sr=SR):
    ph = phase(rate, n, sr)
    lfo = 0.5 + 0.5 * np.sin(2 * np.pi * ph)
    mod = 1.0 - depth * (1.0 - lfo)
    return np.sqrt(1.0 - mix) + np.sqrt(mix) * mod      # total gain applied to every channel


def sec_legacy():
    print("# tubamp today: total gain = sqrt(1-mix) + sqrt(mix)*(1 - depth*(1-lfo)), lfo = 0.5+0.5*sin")
    print("depth\tmix\tgain_max\tgain_min\tpeak_db\tdepth_db")
    for depth, mix in [(0.4, 0.35), (1.0, 0.35), (0.4, 0.5), (1.0, 0.5), (0.4, 1.0), (0.75, 1.0), (1.0, 1.0), (1.0, 0.0)]:
        g = legacy_gain(4.0, depth, mix, int(SR))
        print(f"{depth}\t{mix}\t{g.max():.4f}\t{g.min():.4f}\t{db(g.max()):+.2f}\t{db(g.max()) - db(max(g.min(), 1e-9)):.2f}")
    print("# default patch (depth 0.4, mix 0.35) is the first row: 1.6 dB of tremolo riding on a +2.9 dB boost")


# ----------------------------------------------------------------------------------------------
# reference re-statements (for comparison with ../evidence/*.tsv)
# ----------------------------------------------------------------------------------------------
def gr6_tremolo(rate, intens, up, down, width, n, sr=SR):
    """Guitar Rig 6 Tremolo as documented in report.md 3.1 (pulse -> two-rate one-pole)."""
    ph = 2 * phase(rate, n, sr) - 1                       # [-1, 1)
    target = np.where(ph - (1.6 * width - 0.8) >= 0, 1.0, 1.0 - intens)
    fu = min(rate * 2 ** ((60 - 95 * up) / 12), 150.0)
    fd = min(rate * 2 ** ((60 - 95 * down) / 12), 150.0)
    au, ad = 2 * np.pi * fu / sr, 2 * np.pi * fd / sr     # the binary uses w*T, not 1-exp(-wT)
    y = np.empty(n); s = 0.0
    for i, t in enumerate(target):
        s += (au if t >= 1.0 else ad) * (t - s)
        y[i] = s
    return y


def gr6_twang(rate, intens, n, sr=SR):
    """Guitar Rig 6 Twang Reverb 'vibrato' as documented in report.md 3.2 (4x oversampled there)."""
    ph = phase(rate, n, sr)
    lit = ph > 0.8
    target = 1.0 - 0.88 * intens * lit
    a_lit, a_dark = 2 * np.pi * 29.14 / sr, 2 * np.pi * 10.30 / sr
    y = np.empty(n); s = 1.0
    for i in range(n):
        s += (a_lit if lit[i] else a_dark) * (target[i] - s)
        y[i] = s
    return y


def sec_refs():
    print("# Guitar Rig 6 Tremolo model vs measurement (evidence/gr6_tremolo_measure.tsv)")
    print(HEAD)
    n = int(4 * SR)
    for label, kw in [("init Rt=.5 Int=.5 Up=.5 Dn=.5", dict(rate=2.0439, intens=0.5, up=0.5, down=0.5, width=0.5)),
                      ("Up=0 Dn=0 Int=1", dict(rate=2.0439, intens=1, up=0, down=0, width=0.5)),
                      ("Up=.25", dict(rate=2.0439, intens=1, up=0.25, down=0, width=0.5)),
                      ("Up=.75", dict(rate=2.0439, intens=1, up=0.75, down=0, width=0.5)),
                      ("Wdt=1", dict(rate=2.0439, intens=1, up=0, down=0, width=1.0)),
                      ("Rt=1 (16.35 Hz)", dict(rate=16.3516, intens=1, up=0, down=0, width=0.5))]:
        row(label, gr6_tremolo(n=n, **kw), kw["rate"])
    print("# Guitar Rig 6 Twang Reverb model vs measurement (evidence/gr6_twang_measure.tsv): cycle at 6.872 Hz, Int=1")
    g = gr6_twang(6.872, 1.0, int(3 * SR))
    print(HEAD); row("twang ViInt=1 ViSpd=.5", g, 6.872)
    print("phase\tgain"); [print(f"{i / 20:.2f}\t{v:.4f}") for i, v in enumerate(table(g, 6.872))]


# ----------------------------------------------------------------------------------------------
# proposed building blocks
# ----------------------------------------------------------------------------------------------
def skew(ph, s):
    """Piecewise-linear phase warp: the half-cycle point moves to phase s (0.1..0.9)."""
    return np.where(ph < s, 0.5 * ph / s, 0.5 + 0.5 * (ph - s) / (1.0 - s))


def w_sine(ph):                      # 1 = loud, 0 = dip; starts at the dip
    return 0.5 - 0.5 * np.cos(2 * np.pi * ph)


def w_tri(ph):
    return 1.0 - np.abs(2.0 * ph - 1.0)


def w_morph(ph, wave, shape=0.5):
    """The 'Smooth' voice's LFO, 1 = loud. wave 0 = sine, 0.5 = triangle, 1 = pulse, crossfaded.
    shape (0.1..0.9) is the rise/fall split of the sine and triangle (their crest sits at phase = shape)
    and the duty cycle of the pulse, which is centred on that crest."""
    w0 = skew(ph, shape)
    tri = w_tri(w0)
    if wave <= 0.5:
        a = 2 * wave
        return (1 - a) * w_sine(w0) + a * tri
    a = 2 * wave - 1
    pulse = (((ph - 0.5 * shape) % 1.0) < shape).astype(float)
    return (1 - a) * tri + a * pulse


def w_pulse(ph, duty_hi):
    return (ph < duty_hi).astype(float)


def floor_of(depth):
    """Depth law of the new voices: floor gain = (1-depth)^2  (0.5 -> -12 dB, 0.75 -> -24 dB, 1 -> silence)."""
    return (1.0 - depth) ** 2


def apply_depth(w, depth, makeup_db=3.0):
    f = floor_of(depth)
    return (f + (1.0 - f) * w) * 10 ** (makeup_db * depth / 20)


SLEW_HZ = 120.0     # anti-click one-pole on every voice's gain (tau 1.3 ms, 10-90 % in 2.9 ms)


def voice_smooth(rate, depth, n, wave=0.0, shape=0.5, start=0.0):
    w = w_morph(phase(rate, n, start=start), wave, shape)
    return onepole(apply_depth(w, depth), SLEW_HZ)


# optical: lamp pulse -> LDR with fast "lit" and slow "dark" response
OPT_LIT_FRACTION = 0.25      # lamp lit for a quarter of the cycle
OPT_TAU_LIT_MS = 5.0         # gain falls with this time constant while the lamp is lit
OPT_TAU_DARK_MS = 20.0       # gain recovers with this one while it is dark
OPT_FLOOR = 0.10             # -20 dB at full depth


def voice_optical(rate, depth, n, start=0.0):
    ph = phase(rate, n, start=start)
    lit = ph >= 1.0 - OPT_LIT_FRACTION
    target = 1.0 - depth * (1.0 - OPT_FLOOR) * lit
    f_lit, f_dark = 1e3 / (2 * np.pi * OPT_TAU_LIT_MS), 1e3 / (2 * np.pi * OPT_TAU_DARK_MS)
    return onepole(target, fc_up=f_dark, fc_down=f_lit, y0=1.0)


def sec_shapes():
    n = int(2 * SR); rate = 5.0
    print("# proposed 'Smooth' voice: wave 0 sine, 0.5 triangle, 1 pulse; shape = rise/fall split, duty of the pulse.")
    print("# depth 1, no make-up, 120 Hz one-pole on the gain")
    print(HEAD)
    cases = [("sine", dict(wave=0.0)), ("triangle", dict(wave=0.5)), ("wave .75", dict(wave=0.75)), ("pulse (wave 1)", dict(wave=1.0)),
             ("sine shape .2", dict(wave=0.0, shape=0.2)), ("sine shape .8", dict(wave=0.0, shape=0.8)),
             ("pulse shape .25", dict(wave=1.0, shape=0.25)), ("pulse shape .75", dict(wave=1.0, shape=0.75))]
    for label, kw in cases:
        row(label, onepole(w_morph(phase(rate, n), **kw), SLEW_HZ), rate)
    print("# one cycle, 20 points from the minimum")
    cols = {label: table(onepole(w_morph(phase(rate, n), **kw), SLEW_HZ), rate) for label, kw in cases[:5]}
    print("phase\t" + "\t".join(cols)); [print(f"{i / 20:.2f}\t" + "\t".join(f"{cols[k][i]:.4f}" for k in cols)) for i in range(20)]


def sec_depth():
    print("# depth law of the new voices: floor = (1-depth)^2, peak make-up +3 dB * depth")
    print("depth\tfloor\tfloor_db\tpeak_db\tmean_power_db(sine)")
    for d in (0, 0.25, 0.5, 0.75, 0.9, 1.0):
        g = apply_depth(w_sine(phase(4.0, int(SR))), d)
        print(f"{d}\t{floor_of(d):.4f}\t{db(floor_of(d)):.1f}\t{db(g.max()):+.2f}\t{10 * np.log10(np.mean(g ** 2)):+.2f}")


def sec_optical():
    n = int(2 * SR)
    print("# proposed 'Optical' voice (lit 25 % of the cycle, tau 5 ms lit / 20 ms dark, floor -20 dB)")
    print(HEAD)
    for rate in (3.0, 5.0, 8.0, 12.0):
        row(f"depth 1 @ {rate} Hz", voice_optical(rate, 1.0, n), rate)
    for d in (0.25, 0.5, 0.75):
        row(f"depth {d} @ 5 Hz", voice_optical(5.0, d, n), 5.0)
    print("phase\tgain (5 Hz, depth 1)"); [print(f"{i / 20:.2f}\t{v:.4f}") for i, v in enumerate(table(voice_optical(5.0, 1.0, n), 5.0))]


def sec_chop():
    n = int(2 * SR)
    print("# 'Smooth' voice as a chopper: wave 1, depth 1 (floor 0), +3 dB make-up, 120 Hz one-pole on the gain")
    print(HEAD)
    for rate, width in [(4.0, 0.5), (4.0, 0.25), (4.0, 0.75), (16.0, 0.5), (20.0, 0.5)]:
        row(f"shape {width} @ {rate} Hz", voice_smooth(rate, 1.0, n, wave=1.0, shape=width), rate)
    # click check: spectrum of a 1 kHz sine gated by the smoothed pulse vs an unsmoothed one
    t = np.arange(n) / SR; car = np.sin(2 * np.pi * 1000 * t)
    raw = w_morph(phase(4.0, n), 1.0, 0.5)
    for label, g in [("unsmoothed pulse", raw), ("120 Hz one-pole", onepole(raw, SLEW_HZ))]:
        X = np.abs(np.fft.rfft(car * g * np.hanning(n))); fr = np.fft.rfftfreq(n, 1 / SR)
        ref = X.max()
        band = lambda lo, hi: db(np.sqrt(np.sum(X[(fr >= lo) & (fr < hi)] ** 2)) / ref)
        print(f"# splatter of a 1 kHz carrier, {label}: 3-6 kHz {band(3000, 6000):.1f} dBc, 6-12 kHz {band(6000, 12000):.1f} dBc, 12-24 kHz {band(12000, 24000):.1f} dBc")
    # zipper check: depth stepped 0 -> 1 with and without the 20 ms ramp
    ph = phase(4.0, n); half = n // 2 + 777
    d_step = np.where(np.arange(n) < half, 0.0, 1.0)
    d_ramp = np.clip((np.arange(n) - half) / (0.020 * SR), 0.0, 1.0)
    for label, d in [("depth stepped", d_step), ("depth ramped over 20 ms", d_ramp)]:
        f = (1.0 - d) ** 2
        g = (f + (1.0 - f) * w_sine(ph)) * 10 ** (3.0 * d / 20)
        print(f"# largest sample-to-sample gain step, {label}, before the 120 Hz one-pole: {np.max(np.abs(np.diff(g))):.5f}; after it: {np.max(np.abs(np.diff(onepole(g, SLEW_HZ)))):.5f}")


# ----------------------------------------------------------------------------------------------
# bias voice: level-dependent depth from a push-pull pair of 3/2-power devices
# ----------------------------------------------------------------------------------------------
def pair(v, b):
    """Push-pull pair, each side i = max(0, v + b)^1.5 (Child-Langmuir); b = idle bias (conduction offset)."""
    return np.maximum(v + b, 0.0) ** 1.5 - np.maximum(-v + b, 0.0) ** 1.5


def describing_gain(A, b, n=4096):
    th = 2 * np.pi * np.arange(n) / n
    y = pair(A * np.sin(th), b)
    return 2 * np.mean(y * np.sin(th)) / A        # fundamental gain


BIAS_C = 0.20            # fitted below: tremolo depth of the pair ~ that of gain^2 = b + BIAS_C*A
BIAS_KNEE_DBFS = -12.0   # ours: envelope level at which BIAS_C*A equals the idle bias


def bias_gain(u, depth, env, knee_dbfs=BIAS_KNEE_DBFS):
    """Tremolo gain of the 'Bias' voice. u in [0,1] is the LFO (1 = dip), env the signal envelope
    (linear peak, 1.0 = full scale), q = env / knee.
    Small signals: gain = sqrt(1 - depth*u), the pair's small-signal gain goes with sqrt(bias).
    Large signals: gain -> 1, a pair driven far past its bias no longer cares where the bias sits."""
    q = env / 10 ** (knee_dbfs / 20)
    return np.sqrt(((1.0 - depth * u) + q) / (1.0 + q))


def sec_bias():
    print("# push-pull 3/2-law pair, numerical: tremolo depth (dB) between bias 1 and bias 0.25 versus sine amplitude A,")
    print("# against the closed form 10*log10((1 + c*A)/(0.25 + c*A)), c = %.2f" % BIAS_C)
    print("A/bias\texact_db\tclosed_db")
    for A in (0.05, 0.5, 2, 5, 12.5, 25, 50, 100):
        ex = db(describing_gain(A, 1.0)) - db(describing_gain(A, 0.25))
        print(f"{A}\t{ex:.2f}\t{10 * np.log10((1 + BIAS_C * A) / (0.25 + BIAS_C * A)):.2f}")
    print(f"# 'Bias' voice: tremolo depth (dB, crest to trough) versus input level, knee {BIAS_KNEE_DBFS} dBFS")
    print("level_dbfs\tdepth=0.5\tdepth=0.75\tdepth=0.9\tdepth=1.0")
    for lv in (-60, -40, -30, -24, -18, -12, -6, 0):
        env = 10 ** (lv / 20)
        print(f"{lv}\t" + "\t".join(f"{-db(bias_gain(1.0, d, env)):.2f}" for d in (0.5, 0.75, 0.9, 1.0)))
    print("# one cycle of the gain at depth 0.9 for a quiet (-40 dBFS) and a loud (-6 dBFS) note")
    ph = np.arange(20) / 20; u = 0.5 + 0.5 * np.cos(2 * np.pi * ph)
    print("phase\tquiet\tloud"); [print(f"{ph[i]:.2f}\t{bias_gain(u[i], 0.9, 0.01):.4f}\t{bias_gain(u[i], 0.9, 0.5):.4f}") for i in range(20)]
    print("# exact pair, harmonic content at the crest (b = 1) and the trough (b = 0.25), A = 0.5: H3, H5 re H1 dB")
    for b in (1.0, 0.25):
        th = 2 * np.pi * np.arange(4096) / 4096; y = pair(0.5 * np.sin(th), b); X = np.abs(np.fft.rfft(y))
        print(f"b={b}\tH3 {db(X[3] / X[1]):.1f}\tH5 {db(X[5] / X[1]):.1f}")


# ----------------------------------------------------------------------------------------------
# harmonic voice: duck the low band and the high band in antiphase
# ----------------------------------------------------------------------------------------------
HARM_LP_HZ = 145.0      # circuit: 220 k with 5 nF
HARM_HP_HZ = 640.0      # circuit: 1 M with 250 pF
HARM_XOVER_HZ = 300.0   # ours: geometric mean of the two circuit corners (304.6 Hz), rounded


def harm_circuit(f, u):
    """The circuit's topology: LP145 and HP640 (both first order), antiphase gains 1-u and u, summed."""
    s = 1j * f
    return (1.0 - u) / (1.0 + s / HARM_LP_HZ) + u * (s / HARM_HP_HZ) / (1.0 + s / HARM_HP_HZ)


def harm_ours(f, u, depth, fc=HARM_XOVER_HZ):
    """Proposed: one complementary first-order split (L = LP, H = x - L), power-law antiphase gains,
    loudness normalised for a spectrum with equal power in both bands. Transparent at depth 0."""
    s = 1j * f
    L = 1.0 / (1.0 + s / fc); H = 1.0 - L
    return np.sqrt(2.0 / (2.0 - depth)) * (np.sqrt(1.0 - depth * u) * L + np.sqrt(1.0 - depth * (1.0 - u)) * H)


def sec_harmonic():
    fs = (60, 82, 145, 220, 300, 440, 640, 1000, 2000, 4000, 8000)
    print("# circuit topology (LP 145 Hz and HP 640 Hz, antiphase): magnitude in dB")
    print("u\t" + "\t".join(str(f) for f in fs))
    for u in (0.0, 0.5, 1.0):
        print(f"{u}\t" + "\t".join(f"{db(harm_circuit(f, u)):+.1f}" for f in fs))
    print("# proposed 'Harmonic' voice (complementary split at %g Hz, power-law gains), depth 1: magnitude in dB" % HARM_XOVER_HZ)
    print("u\t" + "\t".join(str(f) for f in fs))
    for u in (0.0, 0.25, 0.5, 0.75, 1.0):
        print(f"{u}\t" + "\t".join(f"{db(harm_ours(f, u, 1.0)):+.1f}" for f in fs))
    print("# proposed: swing per frequency (dB, u=0 minus u=1) for depth 0.5 / 0.75 / 1; depth 0 is exactly 0 dB everywhere")
    for d in (0.0, 0.5, 0.75, 1.0):
        print(f"depth {d}\t" + "\t".join(f"{db(harm_ours(f, 0, d)) - db(harm_ours(f, 1, d)):+.1f}" for f in fs))
    f = np.logspace(np.log10(80), np.log10(5000), 400)
    print("# power average over 80 Hz..5 kHz (flat per octave), depth 1: proposed / circuit topology")
    for u in (0.0, 0.25, 0.5, 0.75, 1.0):
        po = 10 * np.log10(np.mean(np.abs(harm_ours(f, u, 1.0)) ** 2)); pc = 10 * np.log10(np.mean(np.abs(harm_circuit(f, u)) ** 2))
        print(f"u={u}\t{po:+.2f} dB\t{pc:+.2f} dB")


# ----------------------------------------------------------------------------------------------
# stereo spread
# ----------------------------------------------------------------------------------------------
def sec_stereo():
    n = int(SR); rate = 4.0
    print("# spread: right LFO leads by spread degrees. Sum of channel powers over a cycle (dB), depth 1, no make-up")
    print("law\tspread\tpower_min_db\tpower_max_db\tcorr(gL,gR)")
    for law in ("linear", "proposed"):
        for sp in (0, 90, 180):
            wl = w_sine(phase(rate, n)); wr = w_sine(phase(rate, n, start=sp / 360.0))
            e = 1.0 if law == "linear" else 1.0 - sp / 360.0        # proposed: exponent slides from 1 to 0.5
            gl, gr = wl ** e, wr ** e
            p = gl ** 2 + gr ** 2
            print(f"{law}\t{sp}\t{10 * np.log10(max(p.min(), 1e-12) / 2):+.2f}\t{10 * np.log10(p.max() / 2):+.2f}\t{np.corrcoef(gl, gr)[0, 1]:+.3f}")
    print("# proposed law: shape raised to the power (1 - spread/360). At 180 degrees it is a square root and the summed power is constant.")


SECTIONS = dict(legacy=sec_legacy, refs=sec_refs, depth=sec_depth, shapes=sec_shapes, optical=sec_optical,
                chop=sec_chop, bias=sec_bias, harmonic=sec_harmonic, stereo=sec_stereo)

if __name__ == "__main__":
    want = sys.argv[1:] or list(SECTIONS)
    for name in want:
        print(f"\n== {name} ==")
        SECTIONS[name]()
