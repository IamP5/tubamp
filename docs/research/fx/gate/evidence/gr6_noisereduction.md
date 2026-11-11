# Guitar Rig 6 — NI::DSP::NoiseReduction (decompiled)

Recheck: `python3 $RE/disasm.py "$GR" --dump '^NI::DSP::NoiseReduction::'`
Offsets are from the effect object (x19 in processBuffer 0x29c1a0).

## Controls
| item | formula (p = 0..1) | where |
| --- | --- | --- |
| threshold | thr_dB = 90p - 80 (-80 .. +10 dB); invThr = lin(80 - 90p) | setThresholds 0x29bc9c (0x42b40000 = 90, 0x42a00000 = 80) |
| release | T = 800 ms * lin(24p - 12) = 200 ms .. 3.2 s; coef = lin(-75000/(T*sr)) per sample = 75 dB of decay per T | onSetSampleRate 0x29bb54..0x29bc6c (0x44480000 = 800, 0xc7927c00 = -75000) |
| hiss filter | hissThr(log10) = (thr_dB + 40*h)/20 | 0x29bd40..0x29bd6c (0x42200000 = 40) |
| learn | 0.5 s window; thr := 4 * max(|L|,|R|) (+12 dB over the noise peak), p = (dB + 80)/90 | setParamLearn 0x29c138, processBuffer 0x29c2c8..0x29c33c (fmov #4.0 at 0x29c28c) |

## Fixed coefficients (NoiseReduction_Core::endInit 0x29b490)
| value | meaning | where |
| --- | --- | --- |
| 1960.35/sr = 2*pi*312/sr | one-pole low-pass subtracted from each channel before detection: detector high-pass at 312 Hz | 0x29b53c (0x44f50b33) |
| 12566.4/sr = 2*pi*2000/sr | three cascaded one-pole low-pass subtractions on the mid signal: hiss detector high-pass at 2 kHz | literal 0x2bdea5c |
| 6.28319/sr | audio low-pass coefficient = min(1, 2*pi*fc/sr) | literal 0x2bdea58 |
| exp(-2.2222/sr) | release of the cutoff control, tau = 0.45 s | 0x29b4b8 (0xc00e38da), 1.4427 = 1/ln2 at 0x29b4c8 |
| 0.002 per sample | attack slew of the envelope: 0 to 1 in 500 samples | 0x29c27c / 0x29c6a0 (0x3b03126f) |
| 9.9, 0.1 | release exponent = 0.1 + 9.9*(env - peak) | 0x29c244, 0x29c24c |
| 9500, 1000 | fc = 1000 + 9500*v Hz, v in (.., 2] | 0x29c628, 0x29c638 |

## processBuffer 0x29c1a0 (per sample), own notation
```
hp      = x - lp312(x)                       per channel            0x29c470..0x29c4a0
n       = min(1, max(|hpL|,|hpR|) / thr)                             0x29c4a4, 0x29c67c
peak    = max(n, peak * coef)                 instant attack, exp release   0x29c50c, 0x29c680
env     = env * coef^(0.1 + 9.9*d)            program-dependent release     0x29c4b0..0x29c508
env    += clamp(peak - env, 0, 0.002)         linear attack slew            0x29c694..0x29c6ac
d       = env - peak                                                         0x29c6c0
g       = env            if env >= 0.5        (2:1 below threshold)         0x29c6d8
          2*env^2        if 0.25 <= env < 0.5 (3:1)                         0x29c700
          8*env^3        if env < 0.25        (4:1)                         0x29c6f0
          0              if env <= 1e-10
if hiss > 0:
    m   = 0.5*(L+R); h3 = m minus three 2 kHz one-pole low-passes           0x29c530..0x29c584
    r   = log10|h3| - hissThr                                               0x29c588..0x29c5e4
    v   = max(min(r,0) + 2, v * exp(-2.2222/sr))                            0x29c604..0x29c61c
    fc  = 1000 + 9500*v ; a = min(1, 2*pi*fc/sr)
    L,R = one-pole low-pass(L,R; a)                                         0x29c644..0x29c678
out = g * (L,R)
```
getGain 0x29c11c returns 1 - g for the meter.

No hysteresis, no hold, no lookahead. Chatter is prevented by the peak-hold release and
the slew-limited envelope. The detector high-pass keeps mains hum (50/60/100/120 Hz) out
of the decision. Stereo link: max of the two channels.
