# Solid Dynamics — gate/expander half, NI::DSP::CSL::E_Dynamics (decompiled)

Binary: /Library/Audio/Plug-Ins/Components/Solid Dynamics.component/Contents/MacOS/Solid Dynamics (arm64)
Recheck: `python3 $RE/disasm.py "$SD" --dump 'NI::DSP::CSL::E_Dyn_State::updateGEGainFactor'`
S = per-channel E_Dyn_State (pointers at E_Dynamics+0x110 / +0x118). All doubles.

Level (E_Dynamics::evaluate 0xef560..0xef5b0): L = 10*log10(max(10^-7.2, lp(min(x^2,1)))).
The low-pass coefficient is exp(-1.60944/(fs*8.25e-8)) (setSampleRate 0xee5d4), which is
0 at audio rates, so L is the instantaneous sample level in dB with a -72 dB floor.

updateGEGainFactor 0xefa58:
```
E = E*c + L*(1-c),  c = cAtt if L > E else cRel          0xefa68..0xefa94  dB-domain envelope
if expander (S+0x80):
    target = clamp(thr - L, 0, 180)                      0xefaa4..0xefabc  1:2 slope, uses L not E
    c2 = cAtt if target < GR else cRel                   0xefac0..0xefb04
else (gate):
    if thr - hyst > E:  target = 150; hyst = 3.0         0xefad4..0xefaf4  closed: reopen at thr - 3
    else:               target = 0;   hyst = S[0x70] + 3  0xefb0c..0xefb1c  open: close at thr - 3 - S[0x70]
    c2 = cRel if target >= GR else cAtt/1.1              0xefb00, 0xefb2c..0xefb40
GR = min(GR*c2 + target*(1-c2), range)                   0xefb44..0xefb64
gain = 10^(-GR/20)                                       0xefb68
```
S[0x70] = 4.8 dB (constructor literal 0x58e158, stored at 0xedc28). So the gate opens at
thr - 3 dB and closes at thr - 7.8 dB: 4.8 dB of hysteresis. No hold timer.
Closing is a one-pole in the dB domain aimed at 150 dB and clipped at Range, so the
gain falls at a nearly constant dB per second until it hits the range floor.

Parameters (E_Dynamics::setParameter 0xee830):
| param | mapping | where |
| --- | --- | --- |
| Threshold G/E | thr = 68p - 72.5 dB | 0xeec5c..0xeec84 (68.0, -72.5) |
| Mode | p > 0.5 = expander | 0xeec8c..0xeecb0 |
| Atk Mode G/E | fast 0.1 ms, slow 13 ms; cAtt = exp(-1.60944/(t*fs)) (time to 80 %) | 0xeecd0..0xeecf0, 0xeef74..0xeefbc |
| Range | range dB = p * (47.5 - 1.5*q), q = another parameter's value | 0xeedfc..0xeee24 |
| Release G/E | spline-mapped ms (FXParameter<11>), cRel = exp(-1.60944/(t*fs)) | 0xeed30.., setSampleRate 0xee7b0..0xee7f8 |

evaluate takes min(compressor gain, gate gain) as the applied gain (0xef688), not the product.
