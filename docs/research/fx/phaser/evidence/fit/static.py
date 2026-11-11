#!/usr/bin/env python3
"""static.py: frozen-LFO responses of a swept phaser with a periodic multisine (numpy only).

At Rate 0 Guitar Rig 6's Stoned Phaser runs at 0.01 Hz (period 100 s), so one period of a
periodic probe (8192 samples, 0.17 s) sees the break frequency move by at most 0.9 % over the
full stock range and 0.45 % over the narrowed windows used for the static frames. Every
8192-sample window of the output is then one period of a (nearly) time-invariant response:
H(f_k, t) = Y_k / X_k at each probe bin, for any frame hop.

    static.py gen <probe.wav> [--secs 212]
        245 tones: every bin from 41 Hz to 290 Hz, then a 1.02 log grid to 16 kHz, period 8192,
        Schroeder phases, -42 dBFS each.
    static.py frames <probe.wav> <out.wav> [--hop 2048 --skip 1] --dump <file.npz>
        |H| per frame on the probe bins, frame centre times.
    static.py track <probe.wav> <out.wav>     quasi-static notch trajectory, one summary line in
        the column order of targets.tsv (n1/n2 min/max, period, depth, peak). The |H| frames are
        averaged over 5 frames, resampled to a 1.01 log grid and passed to track.summarise (3 dB, 60 Hz-12 kHz),
        so notch identity and extremes follow the same rules as the multitone tracker.
    static.py snap <probe.wav> <out.wav> --t T [--avg 1]   |H| in dB on STATIC_HZ at time T (s).
"""
import argparse, math, os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import track  # noqa: E402

FS = 48000; N = 8192
STATIC_HZ = np.round(50 * 1.1 ** np.arange(56), 1)     # 50 Hz .. 9.9 kHz, 10 % steps


def bins():
    k = list(range(7, 50))                               # 41 .. 287 Hz, every bin
    f = k[-1] * FS / N
    while f < 16000:
        f *= 1.02; b = int(round(f * N / FS))
        if b > k[-1]: k.append(b)
    return np.array(k)


def gen(path, secs):
    k = bins(); n = np.arange(N)
    ph = -np.pi * np.arange(len(k)) ** 2 / len(k)       # Schroeder phases: low crest factor
    amp = 10 ** (-42 / 20)
    one = sum(amp * np.cos(2 * np.pi * kk * n / N + p) for kk, p in zip(k, ph))
    x = np.tile(one, int(math.ceil(secs * FS / N)))[:int(secs * FS)]
    track.write_wav(path, x)
    return len(k), 20 * np.log10(np.abs(one).max())


def frames(probe, out, hop=2048, skip=1.0, ch=0):
    x = track.read_wav(probe)[0][0]; y = track.read_wav(out)[0]; y = y[min(ch, y.shape[0] - 1)]
    k = bins(); X = np.fft.rfft(x[:N])[k]
    starts = np.arange(int(skip * FS), min(len(x), len(y)) - N, hop)
    H = np.empty((len(starts), len(k)))
    for i, s in enumerate(starts):
        H[i] = np.abs(np.fft.rfft(y[s:s + N])[k] / X)
    return k * FS / N, (starts + N / 2) / FS, H


def to_log_grid(f, H, step=1.01, lo=50.0, hi=16000.0):
    g = lo * step ** np.arange(int(math.log(hi / lo) / math.log(step)) + 1)
    lp = np.log(f); P = np.array([np.interp(np.log(g), lp, h * h) for h in H])
    return g, np.sqrt(P)


def summary(f, t, H, lo=60, hi=12000, avg=5):
    """Quasi-static trajectory. |H|^2 is averaged over `avg` consecutive frames first (0.2 s at the
    default hop): a moving response leaks a little between probe bins, and the average removes the
    frame-to-frame ripple that otherwise makes the notch finder jump."""
    if avg > 1:
        k = np.ones(avg) / avg
        H = np.sqrt(np.apply_along_axis(lambda c: np.convolve(c, k, mode="same"), 0, H ** 2))[avg // 2:len(t) - avg // 2]
        t = t[avg // 2:len(t) - avg // 2]
    g, Hg = to_log_grid(f, H)
    r = track.summarise(t, g, 1.01, Hg, lo, hi, 3.0, 0.0)
    return r


def snap(f, t, H, T, avg=1):
    i = int(np.argmin(np.abs(t - T))); h = np.mean(H[max(0, i - avg // 2):i + avg // 2 + 1] ** 2, axis=0) ** 0.5
    return np.interp(np.log(STATIC_HZ), np.log(f), 20 * np.log10(np.maximum(h, 1e-9))), t[i]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen"); g.add_argument("probe"); g.add_argument("--secs", type=float, default=212)
    for name in ("frames", "track", "snap"):
        p = sub.add_parser(name); p.add_argument("probe"); p.add_argument("out")
        p.add_argument("--hop", type=int, default=2048); p.add_argument("--skip", type=float, default=1.0)
        if name == "frames": p.add_argument("--dump", required=True)
        if name == "snap": p.add_argument("--t", type=float, required=True); p.add_argument("--avg", type=int, default=1)
    a = ap.parse_args()
    if a.cmd == "gen":
        n, pk = gen(a.probe, a.secs); print(f"{n} tones, period {N}, peak {pk:.1f} dBFS"); return
    f, t, H = frames(a.probe, a.out, a.hop, a.skip)
    if a.cmd == "frames":
        np.savez_compressed(a.dump, f=f, t=t, H=H); print(f"{len(t)} frames x {len(f)} bins -> {a.dump}")
    elif a.cmd == "track":
        print(track.HEADER); print(track.fmt_summary(os.path.basename(a.out), summary(f, t, H)))
    else:
        d, tt = snap(f, t, H, a.t, a.avg); print(f"# t={tt:.3f}"); print("\n".join(f"{x}\t{v:.2f}" for x, v in zip(STATIC_HZ, d)))


if __name__ == "__main__":
    main()
