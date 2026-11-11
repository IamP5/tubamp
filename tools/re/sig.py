#!/usr/bin/env python3
"""Test signals and basic analyses for black-box effect measurement (numpy only).

Generate (48 kHz float32 mono unless --sr / --stereo):
    sig.py gen sine   out.wav --freq 1000 --db -18 --secs 2
    sig.py gen sweep  out.wav --f0 20 --f1 20000 --db -18 --secs 10     exponential sine sweep
    sig.py gen impulse out.wav --db -6 --secs 2
    sig.py gen noise  out.wav --db -18 --secs 10                         white, fixed seed
    sig.py gen steps  out.wav --freq 1000 --levels -60:0:3 --hold 0.5    stepped-level sine
    sig.py gen burst  out.wav --freq 1000 --lo -30 --hi -6 --on 0.5 --off 1.5 --reps 3
    sig.py gen silence out.wav --secs 2

Analyse:
    sig.py fr    in.wav out.wav [--points 48]     magnitude response from any broadband pair
    sig.py thd   out.wav --freq 1000              harmonic levels and THD of a steady sine
                                                  (counts harmonics only: inharmonic sidebands
                                                  from gain modulation are not in the figure)
    sig.py curve in.wav out.wav --hold 0.5        static in/out level curve from `steps`
    sig.py env   out.wav [--ms 1] [--freq 1000]   level envelope, for attack/release timing
    sig.py notches in.wav out.wav [--frame 4096 --hop 1024]  notch/peak tracks over time
    sig.py stats out.wav                          peak, RMS, DC, channel correlation

Analyses print TSV. They read channel 0 unless --ch is given.
"""
import argparse
import struct
import sys

import numpy as np


def write_wav(path, data, sr):
    data = np.atleast_2d(np.asarray(data, dtype=np.float32))
    if data.shape[0] > data.shape[1]:
        data = data.T
    channels, frames = data.shape
    body = data.T.astype("<f4").tobytes()
    header = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 3, channels, int(sr), int(sr) * channels * 4, channels * 4, 32)
    with open(path, "wb") as fh:
        fh.write(header + b"data" + struct.pack("<I", len(body)) + body)


def read_wav(path):
    """Returns (float64 array shaped [channels, frames], sample rate)."""
    raw = open(path, "rb").read()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        sys.exit(f"{path}: not a RIFF/WAVE file")
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(raw):
        tag, size = raw[pos:pos + 4], struct.unpack("<I", raw[pos + 4:pos + 8])[0]
        chunk = raw[pos + 8:pos + 8 + size]
        if tag == b"fmt ":
            fmt = struct.unpack("<HHIIHH", chunk[:16])
        elif tag == b"data":
            data = chunk
        pos += 8 + size + (size & 1)
    if fmt is None or data is None:
        sys.exit(f"{path}: missing fmt or data chunk")
    code, channels, sr, _rate, _align, bits = fmt
    if code == 0xFFFE:
        code = 3 if bits == 32 else 1
    if code == 3 and bits == 32:
        x = np.frombuffer(data, "<f4").astype(np.float64)
    elif code == 3 and bits == 64:
        x = np.frombuffer(data, "<f8")
    elif code == 1 and bits == 16:
        x = np.frombuffer(data, "<i2") / 32768.0
    elif code == 1 and bits == 24:
        b = np.frombuffer(data, np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = np.where(v >= 1 << 23, v - (1 << 24), v) / 8388608.0
    elif code == 1 and bits == 32:
        x = np.frombuffer(data, "<i4") / 2147483648.0
    else:
        sys.exit(f"{path}: unsupported format code {code}, {bits} bits")
    x = x[:len(x) // channels * channels].reshape(-1, channels).T
    return x, sr


def db(x):
    return 20.0 * np.log10(np.maximum(np.abs(x), 1e-12))


def amp(level_db):
    return 10.0 ** (level_db / 20.0)


def fade(x, sr, ms=5.0):
    n = min(len(x) // 2, int(sr * ms / 1000.0))
    if n > 0:
        ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(n) / n)
        x[:n] *= ramp
        x[-n:] *= ramp[::-1]
    return x


def gen(args):
    sr = args.sr
    t = np.arange(int(args.secs * sr)) / sr
    kind = args.kind
    if kind == "sine":
        x = fade(amp(args.db) * np.sin(2 * np.pi * args.freq * t), sr)
    elif kind == "sweep":
        k = np.log(args.f1 / args.f0)
        phase = 2 * np.pi * args.f0 * args.secs / k * (np.exp(t / args.secs * k) - 1.0)
        x = fade(amp(args.db) * np.sin(phase), sr, 10.0)
    elif kind == "impulse":
        x = np.zeros(len(t))
        x[int(0.1 * sr)] = amp(args.db)
    elif kind == "noise":
        x = np.random.default_rng(1234).standard_normal(len(t))
        x = fade(x / np.sqrt(np.mean(x ** 2)) * amp(args.db), sr)
    elif kind == "silence":
        x = np.zeros(len(t))
    elif kind == "steps":
        lo, hi, step = (float(v) for v in args.levels.split(":"))
        levels = np.arange(lo, hi + 1e-9, step)
        n = int(args.hold * sr)
        tt = np.arange(n * len(levels)) / sr
        x = np.sin(2 * np.pi * args.freq * tt) * np.repeat(amp(levels), n)
    elif kind == "burst":
        on, off = int(args.on * sr), int(args.off * sr)
        env = np.tile(np.concatenate([np.full(off, amp(args.lo)), np.full(on, amp(args.hi))]), args.reps)
        env = np.concatenate([env, np.full(off, amp(args.lo))])
        x = env * np.sin(2 * np.pi * args.freq * np.arange(len(env)) / sr)
    else:
        sys.exit(f"unknown signal {kind}")
    write_wav(args.out, np.vstack([x, x]) if args.stereo else x, sr)
    print(f"{args.out}\t{len(x)} frames\t{sr} Hz", file=sys.stderr)


def load_pair(args):
    a, sr = read_wav(args.inp)
    b, sr2 = read_wav(args.out)
    if sr != sr2:
        sys.exit("sample rates differ")
    n = min(a.shape[1], b.shape[1])
    return a[min(args.ch, a.shape[0] - 1), :n], b[min(args.ch, b.shape[0] - 1), :n], sr


def welch_tf(x, y, sr, nfft=16384):
    """|H(f)| estimate: cross-spectrum over input auto-spectrum, Hann, 75% overlap."""
    hop, win = nfft // 4, np.hanning(nfft)
    sxx = np.zeros(nfft // 2 + 1)
    sxy = np.zeros(nfft // 2 + 1, dtype=complex)
    for start in range(0, len(x) - nfft + 1, hop):
        fx = np.fft.rfft(x[start:start + nfft] * win)
        fy = np.fft.rfft(y[start:start + nfft] * win)
        sxx += np.abs(fx) ** 2
        sxy += np.conj(fx) * fy
    freqs = np.fft.rfftfreq(nfft, 1.0 / sr)
    return freqs, sxy / np.maximum(sxx, 1e-30), sxx


def fr(args):
    x, y, sr = load_pair(args)
    freqs, h, sxx = welch_tf(x, y, sr)
    usable = sxx > sxx.max() * 1e-8
    print("freq_hz\tgain_db\tphase_deg")
    for f in np.geomspace(20.0, min(20000.0, sr / 2 * 0.95), args.points):
        i = int(np.argmin(np.abs(freqs - f)))
        if usable[i]:
            print(f"{freqs[i]:.1f}\t{db(h[i]):.2f}\t{np.degrees(np.angle(h[i])):.1f}")


def thd(args):
    y, sr = read_wav(args.out)
    y = y[min(args.ch, y.shape[0] - 1)]
    y = y[len(y) // 4:len(y) - len(y) // 8]
    win = np.blackman(len(y))
    # Scaled so that the root-sum-square over a component's main lobe is its peak
    # amplitude. Dividing by sum(win)/2 instead reads high by the window's noise
    # bandwidth (2.4 dB for Blackman) once the lobe bins are summed.
    spec = np.abs(np.fft.rfft(y * win)) * 2.0 / np.sqrt(len(y) * np.sum(win ** 2))
    freqs = np.fft.rfftfreq(len(y), 1.0 / sr)

    def level(f):
        i = int(np.argmin(np.abs(freqs - f)))
        return float(np.sqrt(np.sum(spec[max(0, i - 4):i + 5] ** 2)))

    fund = level(args.freq)
    print("harmonic\tfreq_hz\tlevel_db\trel_db")
    total = 0.0
    for k in range(1, 11):
        f = args.freq * k
        if f >= sr / 2:
            break
        lv = level(f)
        if k > 1:
            total += lv ** 2
        print(f"{k}\t{f:.1f}\t{db(lv):.2f}\t{db(lv / max(fund, 1e-12)):.2f}")
    print(f"thd_percent\t{100.0 * np.sqrt(total) / max(fund, 1e-12):.4f}")
    print(f"dc\t{np.mean(y):.6f}")


def curve(args):
    x, y, sr = load_pair(args)
    n = int(args.hold * sr)
    print("in_db\tout_db\tgain_db")
    for start in range(0, len(x) - n + 1, n):
        seg = slice(start + n // 2, start + n - n // 16)
        xin, yout = np.sqrt(np.mean(x[seg] ** 2)), np.sqrt(np.mean(y[seg] ** 2))
        print(f"{db(xin) + 3.01:.2f}\t{db(yout) + 3.01:.2f}\t{db(yout / max(xin, 1e-12)):.2f}")


def env(args):
    y, sr = read_wav(args.out)
    y = y[min(args.ch, y.shape[0] - 1)]
    n = max(1, int(sr * args.ms / 1000.0))
    if args.freq:
        n = max(n, int(round(sr / args.freq)))
    frames = len(y) // n
    rms = np.sqrt(np.mean(y[:frames * n].reshape(frames, n) ** 2, axis=1))
    print("time_s\tlevel_db")
    for i, v in enumerate(rms):
        print(f"{i * n / sr:.5f}\t{db(v) + 3.01:.2f}")


def notches(args):
    x, y, sr = load_pair(args)
    win = np.hanning(args.frame)
    freqs = np.fft.rfftfreq(args.frame, 1.0 / sr)
    band = (freqs >= 60) & (freqs <= 12000)
    print("time_s\tnotch_hz_list\tpeak_hz_list")
    for start in range(0, len(x) - args.frame + 1, args.hop):
        fx = np.abs(np.fft.rfft(x[start:start + args.frame] * win))
        fy = np.abs(np.fft.rfft(y[start:start + args.frame] * win))
        if fx[band].max() < 1e-6:
            continue
        g = db(fy / np.maximum(fx, 1e-9))
        k = np.ones(9) / 9.0
        g = np.convolve(g, k, mode="same")
        idx = np.where(band)[0][1:-1]
        lows = [i for i in idx if g[i] < g[i - 1] and g[i] <= g[i + 1] and g[i] < np.median(g[band]) - args.depth]
        highs = [i for i in idx if g[i] > g[i - 1] and g[i] >= g[i + 1] and g[i] > np.median(g[band]) + args.depth]
        print(f"{start / sr:.4f}\t{','.join(f'{freqs[i]:.0f}' for i in lows)}\t{','.join(f'{freqs[i]:.0f}' for i in highs)}")


def stats(args):
    y, sr = read_wav(args.out)
    print("channel\tpeak_db\trms_db\tdc")
    for ch in range(y.shape[0]):
        print(f"{ch}\t{db(np.max(np.abs(y[ch]))):.2f}\t{db(np.sqrt(np.mean(y[ch] ** 2))):.2f}\t{np.mean(y[ch]):.6f}")
    if y.shape[0] >= 2:
        denom = np.sqrt(np.sum(y[0] ** 2) * np.sum(y[1] ** 2))
        print(f"lr_correlation\t{(np.sum(y[0] * y[1]) / denom) if denom > 0 else 0.0:.4f}")
    print(f"frames\t{y.shape[1]}\nsample_rate\t{sr}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("gen")
    g.add_argument("kind")
    g.add_argument("out")
    g.add_argument("--sr", type=int, default=48000)
    g.add_argument("--secs", type=float, default=2.0)
    g.add_argument("--db", type=float, default=-18.0)
    g.add_argument("--freq", type=float, default=1000.0)
    g.add_argument("--f0", type=float, default=20.0)
    g.add_argument("--f1", type=float, default=20000.0)
    g.add_argument("--levels", default="-60:0:3")
    g.add_argument("--hold", type=float, default=0.5)
    g.add_argument("--lo", type=float, default=-30.0)
    g.add_argument("--hi", type=float, default=-6.0)
    g.add_argument("--on", type=float, default=0.5)
    g.add_argument("--off", type=float, default=1.5)
    g.add_argument("--reps", type=int, default=3)
    g.add_argument("--stereo", action="store_true")
    g.set_defaults(fn=gen)

    def pair(name, fn):
        p = sub.add_parser(name)
        p.add_argument("inp")
        p.add_argument("out")
        p.add_argument("--ch", type=int, default=0)
        p.set_defaults(fn=fn)
        return p

    def single(name, fn):
        p = sub.add_parser(name)
        p.add_argument("out")
        p.add_argument("--ch", type=int, default=0)
        p.set_defaults(fn=fn)
        return p

    pair("fr", fr).add_argument("--points", type=int, default=48)
    single("thd", thd).add_argument("--freq", type=float, default=1000.0)
    pair("curve", curve).add_argument("--hold", type=float, default=0.5)
    e = single("env", env)
    e.add_argument("--ms", type=float, default=1.0)
    e.add_argument("--freq", type=float, default=0.0)
    n = pair("notches", notches)
    n.add_argument("--frame", type=int, default=4096)
    n.add_argument("--hop", type=int, default=1024)
    n.add_argument("--depth", type=float, default=6.0)
    single("stats", stats)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
