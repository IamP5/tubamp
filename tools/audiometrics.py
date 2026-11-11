#!/usr/bin/env python3
"""Metrics for reverb impulse responses and delay echo trains. numpy only.

The companion of tubamp_fxrender (tools/FxRender.cpp): render an engine, then measure
it. The definitions are the ones docs/REVERB-DELAY.md section 5 quotes.

  audiometrics.py reverb  ir.wav            [--json out.json] [--dry-ms 0]
  audiometrics.py delay   burst_render.wav  [--json out.json] [--tone HZ]
  audiometrics.py mod     sine_render.wav --tone HZ [--skip 2.0] [--json out.json]

reverb: stimulus is a unit impulse at t=0 (stim=impulse), wet only. Reports onset, decay
        time (EDT/T20/T30) broadband and per octave, the echo-density profile and mixing
        time, the late tail's ringing index, and L/R correlation and spectral centroid
        over time.
delay:  stimulus is one short burst at t=0 (stim=burst:1000:20). Reports each repeat's
        time, level per channel, spectral centroid and, with --tone, its pitch.
mod:    stimulus is a steady sine (stim=sine:1000). Reports the pitch and amplitude
        wobble of the output and the dominant wobble rates.
"""
import json
import struct
import sys

import numpy as np


# ----------------------------------------------------------------------------- io
def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", "not a RIFF/WAVE file"
    pos, fmt, raw = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, sr, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE and len(body) >= 26:
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, ch, sr, bits)
        elif cid == b"data":
            raw = body
        pos += 8 + size + (size & 1)
    tag, ch, sr, bits = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(raw, dtype="<f4").astype(np.float64)
    elif tag == 3 and bits == 64:
        x = np.frombuffer(raw, dtype="<f8").astype(np.float64)
    elif tag == 1 and bits == 16:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif tag == 1 and bits == 24:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        x = v.astype(np.float64) / 8388608.0
    elif tag == 1 and bits == 32:
        x = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0
    else:
        raise ValueError(f"unsupported wav format tag={tag} bits={bits}")
    x = x[: (len(x) // ch) * ch].reshape(-1, ch).T
    return sr, x


def db(x, floor=-200.0):
    return float(max(floor, 10.0 * np.log10(max(float(x), 1e-30))))


# ------------------------------------------------------------------------ filters
def bandpass_fft(x, sr, lo, hi):
    """Zero-phase band-pass with half-octave raised-cosine skirts."""
    # Pad both ends: a zero-phase filter rings acausally, and without padding the
    # onset's pre-ring wraps circularly onto the quiet tail and flattens the decay.
    pad = int(sr)
    n0 = len(x)
    x = np.concatenate([np.zeros(pad), x, np.zeros(pad)])
    n = len(x)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1.0 / sr)
    g = np.zeros_like(f)
    with np.errstate(divide="ignore"):
        lf = np.log2(np.maximum(f, 1e-9))
    l0, l1 = np.log2(lo), np.log2(hi)
    w = 0.25  # skirt half-width in octaves
    g[(lf >= l0 + w) & (lf <= l1 - w)] = 1.0
    m = (lf > l0 - w) & (lf < l0 + w)
    g[m] = 0.5 - 0.5 * np.cos(np.pi * (lf[m] - (l0 - w)) / (2 * w))
    m = (lf > l1 - w) & (lf < l1 + w)
    g[m] = 0.5 + 0.5 * np.cos(np.pi * (lf[m] - (l1 - w)) / (2 * w))
    return np.fft.irfft(X * g, n)[pad:pad + n0]


def analytic(x):
    n = len(x)
    X = np.fft.fft(x)
    h = np.zeros(n)
    h[0] = 1
    if n % 2 == 0:
        h[n // 2] = 1
        h[1:n // 2] = 2
    else:
        h[1:(n + 1) // 2] = 2
    return np.fft.ifft(X * h)


# ------------------------------------------------------------------- reverb metrics
def schroeder(x):
    e = np.cumsum((x ** 2)[::-1])[::-1]
    return 10.0 * np.log10(np.maximum(e / max(e[0], 1e-30), 1e-30))


def decay_time(edc_db, sr, hi, lo):
    """RT60 extrapolated from a line fit of the EDC between `hi` and `lo` dB."""
    idx = np.where((edc_db <= hi) & (edc_db >= lo))[0]
    if len(idx) < 16:
        return None
    t = idx / sr
    slope, _ = np.polyfit(t, edc_db[idx], 1)
    if slope >= -1e-6:
        return None
    return float(-60.0 / slope)


def noise_floor_trim(x, sr):
    """Truncate where the smoothed energy envelope hits its floor (Lundeby-lite)."""
    win = max(1, int(0.02 * sr))
    n = (len(x) // win) * win
    if n == 0:
        return x
    env = (x[:n] ** 2).reshape(-1, win).mean(axis=1)
    peak = env.max()
    if peak <= 0:
        return x
    tail = env[-max(4, len(env) // 10):].mean()
    thresh = max(tail * 4.0, peak * 1e-12)
    above = np.where(env > thresh)[0]
    if len(above) == 0:
        return x
    return x[: min(len(x), (above[-1] + 1) * win)]


def echo_density(x, sr, win_ms=20.0):
    """Abel & Huang normalised echo density profile. Returns (times, ned)."""
    w = int(win_ms * 1e-3 * sr) | 1
    hann = np.hanning(w)
    hann /= hann.sum()
    x2 = x ** 2
    sd = np.sqrt(np.maximum(np.convolve(x2, hann, mode="same"), 1e-30))
    outside = (np.abs(x) > sd).astype(np.float64)
    ned = np.convolve(outside, hann, mode="same") / 0.3173
    t = np.arange(len(x)) / sr
    return t, ned


def spectral_centroid(x, sr):
    X = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1.0 / sr)
    s = X.sum()
    return float((f * X).sum() / s) if s > 0 else 0.0


def ringing_index(x, sr):
    """Peakiness of the late-tail spectrum: mean over 1/3-octave bands (200 Hz-8 kHz) of
    (max bin power / median bin power) in dB. Smooth noise-like tails sit ~10-13 dB
    (Rayleigh statistics); metallic/modal tails are much higher."""
    X = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1.0 / sr)
    vals, fc = [], 200.0
    while fc < 8000.0:
        m = (f >= fc / 2 ** (1 / 6)) & (f < fc * 2 ** (1 / 6))
        if m.sum() >= 16:
            vals.append(10 * np.log10(X[m].max() / max(np.median(X[m]), 1e-30)))
        fc *= 2 ** (1 / 3)
    return float(np.mean(vals)) if vals else None


def corr(a, b):
    d = np.sqrt((a ** 2).sum() * (b ** 2).sum())
    return float((a * b).sum() / d) if d > 0 else 0.0


def reverb_metrics(path, dry_ms=0.0):
    sr, x = read_wav(path)
    L = x[0]
    R = x[1] if x.shape[0] > 1 else x[0]
    skip = int(dry_ms * 1e-3 * sr)
    L, R = L[skip:].copy(), R[skip:].copy()
    mono = 0.5 * (L + R)
    energy_sig = np.sqrt(0.5 * (L ** 2 + R ** 2))
    peak = energy_sig.max()
    out = {"file": path, "sr": sr, "length_s": len(L) / sr, "peak": float(peak)}
    if peak < 1e-9:
        out["silent"] = True
        return out
    onset = int(np.argmax(energy_sig > peak * 10 ** (-40 / 20)))
    out["onset_ms"] = 1000.0 * onset / sr
    out["peak_time_ms"] = 1000.0 * int(np.argmax(energy_sig)) / sr
    out["energy_db"] = db((L ** 2).sum() + (R ** 2).sum())
    Lt, Rt = L[onset:], R[onset:]
    # Use the louder-energy channel sum for decay (sum of both channel energies).
    both = noise_floor_trim(np.sqrt(Lt ** 2 + Rt ** 2), sr)
    n = len(both)
    Lt, Rt = Lt[:n], Rt[:n]
    out["usable_s"] = n / sr

    def rt_of(sigL, sigR):
        e = np.cumsum((sigL ** 2 + sigR ** 2)[::-1])[::-1]
        edc = 10.0 * np.log10(np.maximum(e / max(e[0], 1e-30), 1e-30))
        return {
            "edt": (lambda v: None if v is None else v)(decay_time(edc, sr, 0.0, -10.0)),
            "t20": decay_time(edc, sr, -5.0, -25.0),
            "t30": decay_time(edc, sr, -5.0, -35.0),
            "t_late": decay_time(edc, sr, -25.0, -45.0),
        }

    out["broadband"] = rt_of(Lt, Rt)
    bands = {}
    for fc in (63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000):
        if fc * 1.42 >= sr / 2:
            continue
        bl = bandpass_fft(Lt, sr, fc / np.sqrt(2), fc * np.sqrt(2))
        br = bandpass_fft(Rt, sr, fc / np.sqrt(2), fc * np.sqrt(2))
        r = rt_of(bl, br)
        r["energy_db"] = db((bl ** 2).sum() + (br ** 2).sum())
        bands[str(fc)] = r
    out["bands"] = bands

    # Echo density on the mono-ish left channel after onset.
    t, ned = echo_density(Lt, sr)
    prof = {}
    for ms in (10, 20, 30, 50, 80, 120, 200, 400, 800):
        i = int(ms * 1e-3 * sr)
        if i < len(ned):
            prof[str(ms)] = float(ned[i])
    out["echo_density"] = prof
    i0 = int(0.005 * sr)
    idx = np.where(ned[i0:] >= 0.9)[0]
    out["mixing_time_ms"] = float(1000.0 * (idx[0] + i0) / sr) if len(idx) else None

    # Spectral evolution + stereo correlation in windows.
    wins = {}
    for name, a, b in (("0-80ms", 0.0, 0.08), ("80-300ms", 0.08, 0.3), ("300-1000ms", 0.3, 1.0),
                       ("1-2s", 1.0, 2.0), ("2-4s", 2.0, 4.0)):
        i, j = int(a * sr), int(b * sr)
        if j > n:
            continue
        wins[name] = {
            "centroid_hz": spectral_centroid(0.5 * (Lt[i:j] + Rt[i:j]) if abs(corr(Lt[i:j], Rt[i:j])) > 0.5 else Lt[i:j], sr),
            "lr_corr": corr(Lt[i:j], Rt[i:j]),
            "level_db": db(((Lt[i:j] ** 2).mean() + (Rt[i:j] ** 2).mean()) / 2),
        }
    out["windows"] = wins

    # Late-tail smoothness (ringing) on a window starting at 30% of the usable length.
    i = int(0.3 * n)
    j = min(n, i + int(1.0 * sr))
    if j - i > int(0.25 * sr):
        out["ringing_db"] = ringing_index(Lt[i:j], sr)

    # Early reflections: sparse-peak summary of the first 100 ms.
    e = np.abs(Lt[: int(0.1 * sr)])
    if len(e):
        thr = e.max() * 10 ** (-20 / 20)
        pk = [k for k in range(1, len(e) - 1) if e[k] > thr and e[k] >= e[k - 1] and e[k] >= e[k + 1]]
        out["early_peaks_count_100ms"] = len(pk)
        out["early_first_peaks_ms"] = [round(1000.0 * k / sr, 2) for k in pk[:12]]
    return out


# -------------------------------------------------------------------- delay metrics
def delay_metrics(path, tone=None):
    sr, x = read_wav(path)
    L = x[0]
    R = x[1] if x.shape[0] > 1 else x[0]
    env_win = max(1, int(0.004 * sr))
    k = np.ones(env_win) / env_win
    envL = np.sqrt(np.convolve(L ** 2, k, mode="same"))
    envR = np.sqrt(np.convolve(R ** 2, k, mode="same"))
    env = np.sqrt(0.5 * (envL ** 2 + envR ** 2))
    peak = env.max()
    out = {"file": path, "sr": sr, "length_s": len(L) / sr, "peak": float(peak)}
    if peak < 1e-9:
        out["silent"] = True
        return out
    # Peak picking with a minimum separation of 15 ms and -50 dB floor.
    thr = peak * 10 ** (-50 / 20)
    sep = int(0.015 * sr)
    cand = np.where((env[1:-1] > thr) & (env[1:-1] >= env[:-2]) & (env[1:-1] >= env[2:]))[0] + 1
    peaks = []
    for c in cand[np.argsort(-env[cand])]:
        if all(abs(c - p) > sep for p in peaks):
            peaks.append(int(c))
        if len(peaks) >= 40:
            break
    peaks.sort()
    echoes = []
    half = int(0.012 * sr)
    for p in peaks:
        a, b = max(0, p - half), min(len(L), p + half)
        segL, segR = L[a:b], R[a:b]
        seg = segL if (segL ** 2).sum() >= (segR ** 2).sum() else segR
        e = {
            "t_ms": round(1000.0 * p / sr, 2),
            "level_db": round(20 * np.log10(max(env[p], 1e-12)), 2),
            "L_db": round(20 * np.log10(max(envL[p], 1e-12)), 2),
            "R_db": round(20 * np.log10(max(envR[p], 1e-12)), 2),
            "centroid_hz": round(spectral_centroid(seg, sr), 1),
        }
        if tone:
            z = analytic(bandpass_fft(seg, sr, tone / 1.5, tone * 1.5))
            ph = np.unwrap(np.angle(z))
            w = np.abs(z) ** 2
            inst = np.diff(ph) * sr / (2 * np.pi)
            if w[1:].sum() > 0:
                fest = float((inst * w[1:]).sum() / w[1:].sum())
                e["freq_hz"] = round(fest, 2)
                e["cents"] = round(1200 * np.log2(max(fest, 1e-6) / tone), 2)
        echoes.append(e)
    out["echoes"] = echoes
    if len(echoes) >= 3:
        ts = np.array([e["t_ms"] for e in echoes])
        lv = np.array([e["level_db"] for e in echoes])
        d = np.diff(ts)
        out["interval_ms_median"] = float(np.median(d))
        out["interval_ms_std"] = float(np.std(d))
        out["level_drop_db_per_repeat"] = [round(float(v), 2) for v in np.diff(lv)[:12]]
        out["centroid_ratio_per_repeat"] = [
            round(echoes[i + 1]["centroid_hz"] / max(echoes[i]["centroid_hz"], 1e-6), 3)
            for i in range(min(len(echoes) - 1, 12))
        ]
    return out


# ---------------------------------------------------------------- modulation metrics
def mod_metrics(path, tone, skip=2.0):
    sr, x = read_wav(path)
    out = {"file": path, "sr": sr, "tone": tone}
    for name, ch in (("L", x[0]), ("R", x[1] if x.shape[0] > 1 else x[0])):
        y = ch[int(skip * sr):]
        if len(y) < sr or np.abs(y).max() < 1e-9:
            out[name] = {"silent": True}
            continue
        z = analytic(bandpass_fft(y, sr, tone / 1.3, tone * 1.3))
        edge = int(0.1 * sr)
        z = z[edge:-edge]
        amp = np.abs(z)
        inst = np.diff(np.unwrap(np.angle(z))) * sr / (2 * np.pi)
        # Smooth instantaneous frequency to 100 Hz bandwidth to drop numerical fuzz.
        w = max(1, int(sr / 200))
        inst = np.convolve(inst, np.ones(w) / w, mode="valid")
        cents = 1200 * np.log2(np.maximum(inst, 1e-6) / np.median(inst))
        res = {
            "pitch_dev_cents_rms": float(np.sqrt(np.mean(cents ** 2))),
            "pitch_dev_cents_pp": float(np.percentile(cents, 99.5) - np.percentile(cents, 0.5)),
            "amp_mod_db_pp": float(20 * np.log10(np.percentile(amp, 99.5) / max(np.percentile(amp, 0.5), 1e-12))),
        }
        # Dominant wobble rate from the spectrum of the pitch-deviation signal.
        c = cents - cents.mean()
        dec = max(1, int(sr / 400))
        c = c[::dec]
        C = np.abs(np.fft.rfft(c * np.hanning(len(c))))
        f = np.fft.rfftfreq(len(c), dec / sr)
        m = (f > 0.05) & (f < 40)
        if m.any() and C[m].max() > 0:
            order = np.argsort(-C[m])[:5]
            res["wobble_rates_hz"] = [round(float(f[m][i]), 3) for i in order]
        out[name] = res
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    mode, path = argv[1], argv[2]
    opts = {argv[i]: argv[i + 1] for i in range(3, len(argv) - 1, 2)}
    if mode == "reverb":
        res = reverb_metrics(path, float(opts.get("--dry-ms", 0)))
    elif mode == "delay":
        res = delay_metrics(path, float(opts["--tone"]) if "--tone" in opts else None)
    elif mode == "mod":
        res = mod_metrics(path, float(opts["--tone"]), float(opts.get("--skip", 2.0)))
    else:
        print(__doc__)
        return 2
    text = json.dumps(res, indent=1)
    if "--json" in opts:
        with open(opts["--json"], "w") as f:
            f.write(text)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
