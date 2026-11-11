"""Test IR files for the tubamp black-box (run in $RE_STATE/work/cab/ours after tonex4.py rates).
First: $RE_STATE/bin/auhost state aufx Tamp Tuba base.aupreset   (default state that ours.make_state patches)."""
import ours, numpy as np
ours.wav("ir_delta48.wav", np.r_[1.0, np.zeros(63)], 48000)
h44 = np.load("../bb/txh_vir_44100.npy")[14:14 + 2048].copy()   # measured TONEX VIR response, used only as a realistic test IR
h44 /= np.max(np.abs(h44))
ours.wav("ir_cab44.wav", h44, 44100)
h48 = ours.resample_ideal(h44, 44100, 48000)
ours.wav("ir_cab48.wav", h48, 48000)
ours.wav("ir_cab96.wav", ours.resample_ideal(h44, 44100, 96000), 96000)
ours.wav("ir_cab48_predelay.wav", np.r_[np.zeros(240), h48], 48000)
ours.wav("ir_cab48_quiet.wav", h48 * 10 ** (-40 / 20), 48000)
ours.wav("ir_stereo_delta.wav", np.stack([np.r_[1.0, np.zeros(63)], np.r_[0.5, np.zeros(63)]]), 48000)
raw = np.load("../bb/txh_vir_44100.npy")[14:14 + 2048]
t = np.arange(2048) / 44100
hb = raw + 0.05 * np.exp(-t * 900) * (np.sin(2 * np.pi * 9000 * t) + np.sin(2 * np.pi * 14000 * t))   # brighter variant
hb /= np.max(np.abs(hb))
ours.wav("ir_bright44.wav", hb, 44100)
ours.wav("ir_bright48.wav", ours.resample_ideal(hb, 44100, 48000), 48000)
ours.wav("ir_bright96.wav", ours.resample_ideal(hb, 44100, 96000), 96000)
rng = np.random.default_rng(1)
for secs in (0.05, 0.2, 0.5, 1.0, 2.0):
    n = int(48000 * secs); tt = np.arange(n) / 48000
    x = rng.standard_normal(n) * np.exp(-tt * 6.9 / secs); x[0] = 1; x /= np.max(np.abs(x))
    ours.wav(f"ir_noise_{secs}.wav", x, 48000)
