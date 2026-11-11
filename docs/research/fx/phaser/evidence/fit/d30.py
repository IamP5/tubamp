#!/usr/bin/env python3
"""d30.py: where and how large the frozen response differs between sample rates (FX-ENGINES D-30).

    python3 d30.py > d30.txt

Closed form, like rate_limit.py: a frozen voicing is linear and time-invariant, and its rendered
response is spec_check.py's equations at the prototype's coefficient-table row for the sweep position
(make-up included; it cancels). For 44.1 and 48 kHz against 192 kHz: |H| in dB, 20 Hz to 10 kHz on a
1 % grid, sweep positions -1, -0.5, 0, 0.5, 1 (Phase 90: the smoothed LFO u; Small Stone: v), where
the lower rate's response is within 20 dB of its peak (check_phaser.py "P9 rate: frozen response"
uses the 48 kHz response as the mask; at 192 kHz the difference against 48 kHz is the same number).
Printed per voicing and rate: the largest difference with its frequency and sweep position, the
largest inside the top 3 dB of that frozen response, and the band (in which the difference exceeds
half the largest) per position.
"""
import math

import numpy as np

import spec_check as S

P = S.P
F = 20.0 * 1.01 ** np.arange(int(math.log(10000.0 / 20.0) / math.log(1.01)) + 1)
POS = (-1.0, -0.5, 0.0, 0.5, 1.0)
NAMES = {"Phase 90 script": "script", "Phase 90 block": "block", "Small Stone Color off": "off", "Small Stone Color on": "on"}


def response_db(name, voicing, flag, w, fs):
    table = P.coeff_table(NAMES[name], fs)
    t = (w + 1.0) * 0.5 * P.TABLE_N
    i = min(int(t), P.TABLE_N - 1); fr = t - i
    G = table[i] + fr * (table[i + 1] - table[i])
    g = tuple(G / (1.0 - G))
    H = S.H_p90(F, g, fs, flag) if voicing == "phase90" else S.H_ss(F, g, fs, flag)
    return 20 * np.log10(np.abs(H))


def main():
    print("# d30.py: frozen |H| at 44.1 / 48 kHz minus 192 kHz, 20 Hz-10 kHz where the lower rate's response is within")
    print("# 20 dB of its peak; sweep positions -1/-0.5/0/0.5/1. 'top 3 dB': inside 3 dB of that frozen response's peak.")
    print("voicing\tfs_hz\tworst_db\tat_hz\tat_w\ttop3_worst_db\ttop3_at_hz\ttop3_at_w\tband_hz_per_w")
    for name, voicing, flag in S.VOICINGS:
        hi = {w: response_db(name, voicing, flag, w, 192000.0) for w in POS}
        for fs in (44100.0, 48000.0):
            worst, top3, bands = (0.0, 0, 0), (0.0, 0, 0), []
            for w in POS:
                lo = response_db(name, voicing, flag, w, fs)
                d = lo - hi[w]
                mask = lo >= lo.max() - 20.0
                k = int(np.argmax(np.abs(d) * mask))
                if abs(d[k]) > abs(worst[0]): worst = (d[k], F[k], w)
                m3 = lo >= lo.max() - 3.0
                k3 = int(np.argmax(np.abs(d) * m3))
                if abs(d[k3]) > abs(top3[0]): top3 = (d[k3], F[k3], w)
                big = np.nonzero(mask & (np.abs(d) >= 0.5 * abs(d[k])))[0]
                bands.append(f"{w:+g}:{F[big[0]]:.0f}-{F[big[-1]]:.0f}" if len(big) else f"{w:+g}:-")
            print(f"{name}\t{fs:.0f}\t{worst[0]:+.2f}\t{worst[1]:.0f}\t{worst[2]:+g}\t{top3[0]:+.2f}\t{top3[1]:.0f}\t{top3[2]:+g}\t{' '.join(bands)}")


if __name__ == "__main__":
    main()
