# Guitar Rig 6 — NI::DSP::NoiseGate (decompiled)

Binary: /Library/Audio/Plug-Ins/Components/Guitar Rig 6.component/Contents/MacOS/Guitar Rig 6 (arm64)
Recheck: `python3 $RE/disasm.py "$GR" --dump '^NI::DSP::NoiseGate::'`

Shared helper used by every setter: `lin(dB) = table[dB*9.9658 + 1200]`, a 2401-entry
(value, slope) table at 0x47db080 (BSS, filled at run time). 9.9658 = 60/6.0206, so the
table is 2^((i-1200)/60) = 10^(dB/20) over +-120 dB [inferred from the index scaling].

| item | formula (p = normalized 0..1) | where |
| --- | --- | --- |
| threshold | T = lin(120p - 120), compared against L^2 + R^2 (a power) | setParamThreshold 0x29b09c (0x42f00000 = 120, 0xc2f00000 = -120) |
| threshold display | 60p - 63 dB, "%+5.1fdB" (= amplitude of a mono signal fed to both channels) | noisegate_threshold 0x13290c (0x42700000 = 60, 0xc27c0000 = -63) |
| attack | t = lin(80p - 80) s = 0.1 ms .. 1 s; N = round(t*sr); linear ramp, inc = (1-g)/N | setParamAttack 0x29acdc; display x1000 "%5.1fms" 0x132744 |
| hold | t = lin(49.54p - 20) s = 0.1 s .. 30 s, counted in control-rate ticks | setParamHold 0x29adb0 (0x42462b71 = 49.54); display "%.2g s" 0x13282c |
| release | coef = max(0, 1 - lin(60 - 80p)/sr) per sample: one-pole decay, tau = 1 ms .. 10 s | setParamRelease 0x29ae58 (0x44e0be4e = 1797.95 = 1200 + 60 dB) ; display lin(80p)*0.001 s 0x132664 |
| learn window | round(0.5 * sr / ctrlDiv) control ticks = 0.5 s | onSetSampleRate 0x29aaa4..0x29aac8 |

## processBuffer 0x29b30c (per sample)

```
p  = L*L + R*R                      ; 0x29b394..0x29b3a4  instantaneous, no envelope filter
d  = p - T                          ; 0x29b3a8
if prev_d * d < 0:                  ; 0x29b3b0..0x29b3bc  sign change = threshold crossing
    if d <= 0:  hold = holdTicks    ; 0x29b348  falling crossing re-arms the hold timer
    elif learning: T = 2*p          ; 0x29b3d0  learn: threshold follows peaks, +3 dB
    else:
        if state == CLOSED: state = ATTACK; n = attackSamples; A = 1; B = (1-g)*attackInc
        hold = 0xffffffff           ; 0x29b41c  timer disarmed while above threshold
prev_d = d
if n == 0 and state == ATTACK: A,B = 1,0; g = 1; state = OPEN    ; 0x29b3dc
g = g*A + B ; outL = g*L ; outR = g*R ; n -= 1
```

onControlRate 0x29b270: `if hold == 0: state = CLOSED; A = releaseCoef; B = 1e-12` then
`hold -= 1`. The learn counter runs down in the same function; at zero it clears the
learn flag and raises a "threshold changed" flag for the UI.

Consequences: single threshold (no hysteresis), no detector smoothing, no range (closes
to silence), no sidechain filter, no lookahead. Chatter is prevented only by hold: the
gate stays open as long as some sample exceeds the threshold at least once per hold
time (minimum 100 ms). Stereo link is the sum of squares.

Learn (setParamLearn 0x29b1e8): T = lin(-120 dB), gate forced closed, 0.5 s window,
T := 2 * max(L^2+R^2) (0x29b3d0). getParamThreshold 0x29b1ac returns
p = 0.086859*ln(T) + 1.2 = dB20(T)/100 + 1.2, which is not the inverse of the setter
(p = dB20(T)/120 + 1). Fed back through the setter the learned threshold lands
0.2*a + 12 dB above the +3 dB point (a = amplitude dB, e.g. +4 dB at -40 dBFS) [inferred].

Lamp: displayGateLamp 0x29b244 returns 1 - g (0 when open).
