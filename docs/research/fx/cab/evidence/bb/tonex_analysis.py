"""Analyses on the TONEX impulse responses saved by tonex4.py (txh_*.npy). Run in $RE_STATE/work/cab/bb."""
import numpy as np
L = lambda n: np.load(f"txh_{n}.npy")[:4096]
h0, h1, h5, h25 = L("blend0_m1"), L("blend1_m2"), L("blend05"), L("blend025")
def fit(h, a, b):
    A = np.stack([a, b], 1); c, *_ = np.linalg.lstsq(A, h, rcond=None); r = h - A @ c
    return c, 10 * np.log10(np.sum(r ** 2) / np.sum(h ** 2))
c, e = fit(h5, h0, h1); print("MIC BLEND 0.5  = %.4f * mic1 + %.4f * mic2, residual %.1f dB" % (c[0], c[1], e))
c, e = fit(h25, h0, h1); print("MIC BLEND 0.25 = %.4f * mic1 + %.4f * mic2, residual %.1f dB" % (c[0], c[1], e))
def xc_delay(a, b, n=8192, up=16):
    X = np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)); Xp = np.zeros(n * up // 2 + 1, complex); Xp[: len(X)] = X
    x = np.fft.irfft(Xp, n * up); k = int(np.argmax(np.abs(x)))
    if k > n * up // 2: k -= n * up
    return k / up
print("delay of mic1 (414, Z=0) against mic2 (57, Z=1), cross-correlation peak: %.2f samples at 48 kHz" % xc_delay(h0, h1))
for z in ("0", "0.5", "1"): print("MIC 1 Z = %s: delay against Z = 0: %.2f samples" % (z, xc_delay(L(f"m1z_{z}"), L("m1z_0"))))
for x in ("0", "0.25", "0.5", "0.75", "1"): print("MIC 1 X = %s: delay against X = 0: %.2f samples" % (x, xc_delay(L(f"m1x_{x}"), L("m1x_0"))))
def minphase(h, n=16384):
    H = np.abs(np.fft.fft(h, n)) + 1e-9; c = np.fft.ifft(np.log(H)).real
    w = np.zeros(n); w[0] = 1; w[1:n // 2] = 2; w[n // 2] = 1
    return np.fft.ifft(np.exp(np.fft.fft(c * w))).real
h = L("m1z_0"); on = int(np.argmax(np.abs(h) > np.max(np.abs(h)) * 0.01)); hm = minphase(h)
frac = lambda x, k: np.sum(x[:k] ** 2) / np.sum(x ** 2)
print("energy in the first k samples after onset, measured IR against its minimum-phase equivalent:")
for k in (16, 32, 64, 128, 256): print("  k=%d  measured %.3f  minimum-phase %.3f" % (k, frac(h[on:], k), frac(hm, k)))
D = np.fft.rfft(np.load("txh_res_1.npy")[:8192], 1 << 17) / np.fft.rfft(np.load("txh_res_0.npy")[:8192], 1 << 17); f = np.fft.rfftfreq(1 << 17, 1 / 48000)
m = (f > 40) & (f < 400); k = int(np.argmax(np.abs(D[m]))); g = 20 * np.log10(np.abs(D[m])); fm = f[m]
print("CAB RESONANCE 1 against 0: low peak %.1f dB at %.0f Hz, half-gain band %.0f..%.0f Hz" % (g[k], fm[k], fm[:k][g[:k] < g[k] / 2][-1], fm[k:][g[k:] < g[k] / 2][0]))
