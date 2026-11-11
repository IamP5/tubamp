"""Slopes of tubamp's two cut filters from the impulse response with a unit-impulse IR (run after t1.py in $RE_STATE/work/cab/ours)."""
import numpy as np
h = np.load("h_delta_defcuts.npy"); n = 1 << 18; H = np.fft.rfft(h[:32768], n); f = np.fft.rfftfreq(n, 1 / 48000)
g = lambda x: 20 * np.log10(abs(H[np.argmin(abs(f - x))]))
print("freq_hz\tgain_db_re_1k   (cab_lowcut 80 Hz, cab_highcut 8000 Hz, session 48 kHz)")
for x in (20, 40, 80, 160, 1000, 4000, 8000, 16000, 22000): print("%d\t%.2f" % (x, g(x) - g(1000)))
