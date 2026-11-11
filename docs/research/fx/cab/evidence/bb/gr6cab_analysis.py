"""Delay, polarity and blend checks on the Guitar Rig 6 responses saved by gr6cab.py (run in $RE_STATE/work/cab/gr6bb)."""
import numpy as np
L = lambda n: np.load(f"h_{n}.npy")[0][:8192]
def xc(a, b, n=16384, up=16):
    X = np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)); Xp = np.zeros(n * up // 2 + 1, complex); Xp[: len(X)] = X
    x = np.fft.irfft(Xp, n * up); k = int(np.argmax(np.abs(x))); s = np.sign(x[k])
    if k > n * up // 2: k -= n * up
    return k / up, s
ref = L("crp_s1")
for ph in ("0.25", "0.5", "1"):
    d, s = xc(L(f"crp_s1_ph{ph}"), ref); print("Control Room Pro, Phase 1 = %s: delay %.2f samples at 48 kHz (0.04 m / 343.2 m/s * 48000 * phase = %.2f)" % (ph, d, 0.04 / 343.2 * 48000 * float(ph)))
d, s = xc(L("crp_s1_inv"), ref); print("PhaseInv 1 = 1: delay %.2f samples, polarity %+d" % (d, s))
off = L("mc_off"); e = off ** 2; print("empty rack (component off): energy centroid %.1f samples, peak index %d" % (np.sum(np.arange(len(off)) * e) / np.sum(e), int(np.argmax(np.abs(off)))))
for n in ("crp_s1", "mc_c0_48000", "mc_cab0", "cr_init"):
    h = L(n); print("%s: peak index %d (empty rack 16)" % (n, int(np.argmax(np.abs(h)))))
a, b, m = L("mc_c0_48000"), L("mc_c1"), L("mc_c0.5")
A = np.stack([a, b], 1); c, *_ = np.linalg.lstsq(A, m, rcond=None); r = m - A @ c
print("Matched Cabinet crossfade 0.5 = %.3f * (c=0) + %.3f * (c=1), residual %.1f dB" % (c[0], c[1], 10 * np.log10(np.sum(r ** 2) / np.sum(m ** 2))))
