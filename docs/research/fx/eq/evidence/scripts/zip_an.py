#!/usr/bin/env python3
"""Zipper analysis of `./ours zip .` outputs. Bass knob swept 5 -> 10 in 0.5 s (t = 0.5..1.0 s) at 48 kHz while a
-12 dBFS sine plays. A click-free filter leaves only slow amplitude change, so everything far from the sine is zipper.
Metric: brick-wall high-pass (FFT) at 10x the sine frequency for the 100 Hz input, at 4 kHz for the 1 kHz input;
peak and RMS of what remains during the sweep, dBFS (200 ms raised-cosine fades at both ends before the FFT). The static tail (1.5..2.0 s) gives the floor.
Variants (see ours.cpp zip()): shipped512/shipped64 = as shipped, coefficients swapped once per 512/64-sample block, knob quantised to 0.1;
tdf2_smoothed = shipped biquad, knob through a 10 ms one-pole, coefficients every 16 samples; tdf2_persample = same, every sample;
tdf2_smoothed_2pole = two cascaded 5 ms one-poles, every 16 samples; svf_smoothed = TPT SVF retuned every 16 samples (stepped);
svf_interp = SVF, g and mix coefficients linearly interpolated per sample, 10 ms one-pole; svf_interp_2pole = same with two 5 ms one-poles."""
import numpy as np, sys, os
os.chdir(os.path.join(os.environ["RE_STATE"], "work/eq/ours"))
fs = 48000
print("input_Hz\tvariant\tsweep_peak_dBFS\tsweep_rms_dBFS\tstatic_rms_dBFS")
for fin, fc in ((100, 1000), (1000, 4000)):
    for v in ("shipped512", "shipped64", "tdf2_smoothed", "tdf2_persample", "tdf2_smoothed_2pole", "svf_smoothed", "svf_interp", "svf_interp_2pole"):
        y = np.fromfile(f"zip_{v}_{fin}.f32", dtype=np.float32).astype(np.float64)
        w = np.ones(len(y)); n = int(0.2 * fs); w[:n] = 0.5 - 0.5 * np.cos(np.pi * np.arange(n) / n); w[-n:] = w[:n][::-1]; y = y * w
        Y = np.fft.rfft(y); f = np.fft.rfftfreq(len(y), 1 / fs); Y[f < fc] = 0; r = np.fft.irfft(Y, len(y))
        a = r[int(0.45 * fs):int(1.1 * fs)]; b = r[int(1.5 * fs):int(1.9 * fs)]
        print(f"{fin}\t{v}\t{20*np.log10(np.max(np.abs(a))+1e-30):.1f}\t{10*np.log10(np.mean(a*a)+1e-30):.1f}\t{10*np.log10(np.mean(b*b)+1e-30):.1f}")
