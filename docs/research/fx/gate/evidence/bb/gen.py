#!/usr/bin/env python3
"""Gate test signals (48 kHz mono float32). Usage: gen.py <outdir>
decay.wav   1 kHz sine, 0.5 s silence, then level falls linearly in dB -10 -> -100 dBFS(peak) over 9 s (10 dB/s)
rise.wav    1 kHz sine, level rises -100 -> -10 dBFS over 9 s
burst.wav   1 kHz: 1 s at LO, then 3 x (0.3 s at HI, 1.5 s at LO); HI=-20, LO=-70 dBFS peak
burstlf.wav same with 82.41 Hz (low E)
hover.wav   1 kHz at -40 dBFS with +-3 dB 3 Hz sinusoidal (in dB) level wobble, 6 s  (set threshold to -40)
noisehover.wav white noise, RMS wobbling -43..-37 dBFS at 0.5 Hz, 8 s
chug.wav    staccato palm mutes: 82.41 Hz + harmonics, 75 ms long (tau 60 ms, 5 ms mute fade), every 150 ms for 3 s, over white noise at -66 dBFS RMS, then 2 s noise only
sustain.wav one note 196 Hz + harmonics, decay 6 dB/s from -12 dBFS for 10 s, over noise -70 dBFS RMS
"""
import sys, numpy as np
sys.path.insert(0, sys.argv[2] if len(sys.argv) > 2 else '.')
import sig
sr = 48000
out = sys.argv[1]
def w(name, x): sig.write_wav(f'{out}/{name}.wav', x.astype(np.float32), sr)
def sine(f, n): return np.sin(2*np.pi*f*np.arange(n)/sr)
def dbl(d): return 10.0**(np.asarray(d)/20.0)
n = 9*sr
ramp = np.linspace(-10, -100, n)
w('decay', np.concatenate([np.zeros(sr//2), dbl(ramp)*sine(1000, n), np.zeros(sr//2)]))
w('rise', np.concatenate([np.zeros(sr//2), dbl(ramp[::-1])*sine(1000, n), np.zeros(sr//2)]))
def burst(f):
    lev = [np.full(sr, -70.0)]
    for _ in range(3): lev += [np.full(int(0.3*sr), -20.0), np.full(int(1.5*sr), -70.0)]
    lev = np.concatenate(lev)
    return dbl(lev)*sine(f, len(lev))
w('burst', burst(1000)); w('burstlf', burst(82.41))
t = np.arange(6*sr)/sr
w('hover', dbl(-40 + 3*np.sin(2*np.pi*3*t))*sine(1000, len(t)))
rng = np.random.default_rng(1)
t = np.arange(8*sr)/sr
nz = rng.standard_normal(len(t))
w('noisehover', nz*dbl(-40 + 3*np.sin(2*np.pi*0.5*t)))
# chugs: staccato palm mutes, 70 ms of note (tau 60 ms) then hand-muted with a 5 ms fade; 75 ms of noise-only gap
n = 5*sr; x = np.zeros(n); f0 = 82.41
for k in range(20):
    s = int(k*0.150*sr); m = int(0.075*sr); tt = np.arange(m)/sr
    note = sum(a*np.sin(2*np.pi*f0*h*tt) for h, a in ((1,1.0),(2,0.6),(3,0.4),(4,0.25),(6,0.1)))
    e = np.exp(-tt/0.060); f = int(0.005*sr); e[-f:] *= np.linspace(1, 0, f)
    x[s:s+m] += 0.25*note*e
x += rng.standard_normal(n)*dbl(-66)
w('chug', x)
n = 10*sr; tt = np.arange(n)/sr
note = sum(a*np.sin(2*np.pi*196*h*tt) for h, a in ((1,1.0),(2,0.5),(3,0.3),(5,0.1)))
note /= np.max(np.abs(note))
w('sustain', dbl(-12 - 6*tt)*note + rng.standard_normal(n)*dbl(-70))
