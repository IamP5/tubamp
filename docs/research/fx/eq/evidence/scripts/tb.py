#!/usr/bin/env python3
"""Black-box of the installed tubamp AU (aufx Tamp Tuba): the amp block's tone stack, which is the
same ToneStackEQ class as the EQ block (src/PluginProcessor.cpp:1241-1246). A fresh instance runs the amp
block alone with no model loaded (chain::defaultOrder), so the tone stack is the only filter that changes.
   python3 tb.py flat|fr|noise
Renders: auhost render aufx Tamp Tuba in.wav out.wav --warmup 0.5 --set <Amp EQ On>=0|1
         --set <Amp EQ Bass>=v --set <Amp EQ Middle>=v --set <Amp EQ Treble>=v   (ids in tubamp_params.txt)
The 'Amp EQ On'=0 render is the reference."""
import os, subprocess, sys
import numpy as np
RE = os.environ["RE"]; RS = os.environ["RE_STATE"]; sys.path.insert(0, RE)
import sig
W = os.path.join(RS, "work/eq/ours")
ids = {}
os.makedirs(W, exist_ok=True)
if not os.path.exists(os.path.join(W, "tubamp_params.txt")):
    with open(os.path.join(W, "tubamp_params.txt"), "w") as fh:
        subprocess.run([os.path.join(RS, "bin/auhost"), "params", "aufx", "Tamp", "Tuba"], stdout=fh, stderr=subprocess.STDOUT, timeout=120)
for line in open(os.path.join(W, "tubamp_params.txt")):
    p = line.rstrip("\n").split("\t")
    if len(p) > 4 and p[0].isdigit(): ids[p[1]] = p[0]
OFF = [n for n in ids if n.endswith(" On") and n not in ("EQ On",)]
def render(inp, out, eq_on, bass=0.5, mid=0.5, treble=0.5):
    cmd = [os.path.join(RS, "bin/auhost"), "render", "aufx", "Tamp", "Tuba", inp, out, "--warmup", "0.5"]
    cmd += ["--set", f"{ids['Amp EQ On']}={1 if eq_on else 0}", "--set", f"{ids['Amp EQ Bass']}={bass}", "--set", f"{ids['Amp EQ Middle']}={mid}", "--set", f"{ids['Amp EQ Treble']}={treble}"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    if r.returncode: raise RuntimeError(r.stderr[-400:])
def noise(sr, secs=4, db=-12):
    p = os.path.join(W, f"noise_{sr}.wav")
    if not os.path.exists(p):
        rng = np.random.default_rng(1); x = rng.uniform(-1, 1, int(sr * secs)).astype(np.float32) * sig.amp(db); sig.write_wav(p, x, sr)
    return p
def rd(p):
    y, _ = sig.read_wav(p); return y[0]
def dbrms(x): return 10 * np.log10(np.mean(x * x) + 1e-300)
def flat():
    print("# Amp EQ on with Bass=Middle=Treble=5 (flat) against Amp EQ off; white noise -12 dBFS peak")
    print("fs\tsamples_differing\ttotal\tdiff_rms_dBFS\tdiff_rms_rel_signal_dB\tdiff_peak")
    for sr in (44100, 48000, 96000, 192000):
        i = noise(sr); render(i, os.path.join(W, "t_off.wav"), False); render(i, os.path.join(W, "t_on.wav"), True)
        a = rd(os.path.join(W, "t_off.wav")); b = rd(os.path.join(W, "t_on.wav")); n = min(len(a), len(b)); d = b[:n] - a[:n]
        print(f"{sr}\t{int(np.count_nonzero(d))}\t{n}\t{dbrms(d):.1f}\t{dbrms(d) - dbrms(a[:n]):.1f}\t{np.max(np.abs(d)):.3g}")
def fr():
    print("# magnitude of the EQ block: FFT ratio (EQ on at the setting) / (EQ off), noise input, 48 kHz and 44.1 kHz; knob values 0..10 as normalised 0..1")
    grid = [30, 50, 80, 150, 300, 425, 600, 1000, 1800, 3000, 5000, 10000, 15000, 20000]
    print("fs\tbass\tmiddle\ttreble\t" + "\t".join(map(str, grid)))
    for sr in (48000, 44100):
        i = noise(sr); render(i, os.path.join(W, "t_off.wav"), False); a = rd(os.path.join(W, "t_off.wav"))
        for b, m, t in ((1, .5, .5), (0, .5, .5), (.5, 1, .5), (.5, 0, .5), (.5, .5, 1), (.5, .5, 0), (1, 1, 1), (0, 0, 0)):
            render(i, os.path.join(W, "t_on.wav"), True, b, m, t); y = rd(os.path.join(W, "t_on.wav")); n = min(len(a), len(y))
            X = np.fft.rfft(a[:n]); Y = np.fft.rfft(y[:n]); f = np.fft.rfftfreq(n, 1 / sr); hd = 20 * np.log10(np.abs(Y) / (np.abs(X) + 1e-30) + 1e-30)
            sm = lambda q: float(np.mean(hd[(f > q * 0.97) & (f < q * 1.03)]))
            print(f"{sr}\t{b*10:g}\t{m*10:g}\t{t*10:g}\t" + "\t".join(f"{sm(q):.2f}" for q in grid))
def nz():
    print("# roundoff of the tone stack: sine in, least-squares fit of sin/cos at the input frequency removed from the steady-state output;")
    print("# residual RMS in dB relative to the fitted fundamental. 'off' shows the floor of the method and of the rest of the plugin.")
    print("fs\tin_Hz\tin_dBFS\tsetting\tresidual_dBc")
    for sr in (44100, 96000, 192000):
        for fin, lev in ((82.4, -20), (1000.0, -20)):
            n = sr * 4; t = np.arange(n) / sr; x = (sig.amp(lev) * np.sin(2 * np.pi * fin * t)).astype(np.float32)
            i = os.path.join(W, "s.wav"); sig.write_wav(i, x, sr)
            for lab, on, b in (("off", False, .5), ("on, flat", True, .5), ("on, bass=6 (+4 dB)", True, .6), ("on, bass=10 (+20 dB)", True, 1.0), ("on, bass=0 (-20 dB)", True, 0.0)):
                render(i, os.path.join(W, "t_on.wav"), on, b); y = rd(os.path.join(W, "t_on.wav"))[sr: sr * 3]; tt = np.arange(len(y)) / sr
                M = np.stack([np.sin(2 * np.pi * fin * tt), np.cos(2 * np.pi * fin * tt), np.ones(len(y))], 1); c, *_ = np.linalg.lstsq(M, y, rcond=None); r = y - M @ c
                print(f"{sr}\t{fin}\t{lev}\t{lab}\t{dbrms(r) - dbrms(M[:, :2] @ c[:2]):.1f}")
{"flat": flat, "fr": fr, "noise": nz}[sys.argv[1]]()
