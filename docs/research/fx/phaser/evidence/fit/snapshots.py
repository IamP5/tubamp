#!/usr/bin/env python3
"""snapshots.py: complex frequency responses of Guitar Rig 6 along its sweep -> snapshots.tsv.

    export AUHOST=... GR6_STATES=...      (gr6.py)
    python3 snapshots.py > snapshots.tsv

An impulse train (0.5 every S samples from sample 4096) is rendered through Guitar Rig 6 at 48 kHz.
Snapshot k is the output from impulse k on, S samples, the last quarter faded with a raised cosine,
divided by the impulse height; its DTFT is taken at FREQS (40 points, log-spaced, 50 Hz to 6 kHz:
below Guitar Rig's own top-end roll-off). The response keeps Guitar Rig's latency (fit.py fits it
as a pure delay) and, for Phaser Nine, its x1.862 make-up. Time = the impulse's time in seconds;
Guitar Rig's LFO starts at phase 0 with the render (fit.py measures the offset, about 1e-4 cycle).

Takes (tag, component, overrides, seconds, spacing S, every how many snapshots are kept):
"""
import math
import sys

import numpy as np

import gr6

SR = 48000
FREQS = 50.0 * (6000.0 / 50.0) ** (np.arange(40) / 39.0)
TAKES = [
    ("ss_off_r040", "ss", dict(Rt=0.4), 8.0, 2048, 2),
    ("ss_off_r050", "ss", dict(Rt=0.5), 8.0, 2048, 2),
    ("ss_off_r075", "ss", dict(Rt=0.75), 4.0, 2048, 1),
    ("ss_on_r040", "ss", dict(Rt=0.4, Cl=1), 10.0, 8192, 1),
    ("ss_on_r050", "ss", dict(Rt=0.5, Cl=1), 10.0, 8192, 1),
    ("p9_block_p010", "p9", dict(p=0.1), 10.0, 4096, 2),
]
__doc__ += "".join(f"  {t[0]}: {t[1]} {t[2]}, {t[3]} s, S {t[4]}, every {t[5]}\n" for t in TAKES)


def snapshot_spectra(y, pos, S, height=0.5):
    w = np.ones(S); q = S // 4; w[-q:] = 0.5 + 0.5 * np.cos(np.pi * np.arange(q) / q)
    E = np.exp(-2j * np.pi * np.outer(FREQS, np.arange(S)) / SR)
    return np.array([E @ (y[p:p + S] * w) / height for p in pos])


def main():
    print("# snapshots.py: Guitar Rig 6 complex snapshots along the sweep, 48 kHz (see the script header).")
    print("# Columns: tag, rate_hz, time_s, then re and im of H at each frequency of the freqs row.")
    print("freqs\t\t\t" + "\t".join(f"{f:.2f}" for f in FREQS))
    for tag, comp, over, secs, S, keep in TAKES:
        n = int(secs * SR)
        x = np.zeros(n); pos = np.arange(4096, n - S, S); x[pos] = 0.5
        rate = gr6.ss_rate(over["Rt"]) if comp == "ss" else gr6.p9_rate(over["p"])
        y = gr6.render([x], gr6.state(comp, **over))[0]
        H = snapshot_spectra(y, pos, S)
        for k in range(0, len(pos), keep):
            vals = "\t".join(f"{v.real:.6g}\t{v.imag:.6g}" for v in H[k])
            print(f"{tag}\t{rate:.6f}\t{pos[k] / SR:.6f}\t{vals}")
        sys.stdout.flush()


def read(path):
    """-> freqs, {tag: (rate, times, H[k, f])}"""
    freqs, takes = None, {}
    for line in open(path):
        if line.startswith("#"):
            continue
        c = line.rstrip("\n").split("\t")
        if c[0] == "freqs":
            freqs = np.array([float(v) for v in c[3:]]); continue
        v = np.array([float(x) for x in c[3:]])
        t = takes.setdefault(c[0], [float(c[1]), [], []])
        t[1].append(float(c[2])); t[2].append(v[0::2] + 1j * v[1::2])
    return freqs, {k: (r, np.array(ts), np.array(h)) for k, (r, ts, h) in takes.items()}


if __name__ == "__main__":
    main()
