# TONE3000 — NoiseGate (decompiled)

Binary: /Library/Audio/Plug-Ins/Components/TONE3000.component/Contents/MacOS/TONE3000 (arm64)
The plugin has no dsp::noise_gate symbols (`grep -c noise_gate $RE_STATE/symbols/TONE3000.txt` = 0).
Its gate is its own class: `NoiseGate::processChannel(NoiseGate::Channel&, float*, int) const` at 0x46d28,
called in place on each input channel from TONE3000Processor::processBlock at 0x42d1c.
Recheck: `python3 $RE/disasm.py "$T3" --addr 0x46d28` and `--dump 'TONE3000Processor::prepareToPlay'` (0x3dc94..0x3ddc4).

processChannel, per sample (G = gate object, C = channel state):
```
hp   = 2nd-order TPT state-variable high-pass(x)            0x46d8c..0x46dc0
lp   = lp + cLP*(hp - lp)                                   0x46dc4..0x46dd4
a    = |lp| ; env += (a > env ? cEnvAtt : cEnvRel)*(a - env) 0x46dd8..0x46df0
state CLOSED: if env >= open : state = OPEN, target = 1     0x46e0c..0x46e14, 0x46d58
              else target = max((env/open)^3, 1e-4)         0x46e58..0x46e6c
state OPEN  : if env <  close: state = HOLD, n = holdSamples 0x46e1c..0x46e30
state HOLD  : if env >= close: state = OPEN                 0x46e38..0x46e40
              elif --n <= 0  : state = CLOSED               0x46e44..0x46e50
g   += (target > g ? cOpen : cClose)*(target - g)           0x46d60..0x46d78
y    = x*g                                                  0x46d7c
```
Constants (prepareToPlay; coefficient = 1 - exp(-1/(sr*t))):
| constant | value | where |
| --- | --- | --- |
| envelope attack | 0.2 ms | 0x3dcb4 (0.0002) |
| envelope release | 25 ms | 0x3dcd8 (0.025) |
| gain open | 0.2 ms (same coefficient) | 0x3dd20 |
| gain close | 100 ms | 0x3dcf4 (0.1) |
| hold | 50 ms = int(sr*0.05) samples | 0x3dd28..0x3dd40 |
| sidechain high-pass | 80 Hz, k = 1.41421 (Butterworth), g = tan(pi*80/sr) | 0x3dd44..0x3dd94 |
| sidechain low-pass | 5 kHz one-pole, 1 - exp(-2*pi*5000/sr) | 0x3dd98..0x3ddc4 |
| open threshold | 10^(thr/20) | processBlock 0x42c78..0x42c8c |
| close threshold | 10^((thr - 5)/20): 5 dB hysteresis | 0x42c90..0x42cc4 |
| closed gain | max((env/open)^3, 1e-4): cubic law, -80 dB floor | 0x46e58..0x46e6c, 0x46d40 |
Stubs: 0x77243c = expf, 0x772cf4 = tanf, 0x772148 = __exp10f (otool -Iv).
