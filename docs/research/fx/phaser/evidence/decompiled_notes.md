# Decompiled constants and structure, by address

Setup for every command:

```bash
export RE_STATE=<shared state dir>; export RE=<repo>/tools/re
GR="/Library/Audio/Plug-Ins/Components/Guitar Rig 6.component/Contents/MacOS/Guitar Rig 6"
AT="/Library/Audio/Plug-Ins/Components/AmpliTube 5.component/Contents/MacOS/AmpliTube 5"
```

Float immediates appear as a `mov`/`movk` pair; the IEEE-754 value is given beside each.

## 1. Guitar Rig 6, NI::DSP::PhaserMXR ("Phaser Nine")

| What | Address | Value |
| --- | --- | --- |
| Rate knob to period, three linear segments | `convertInput` 0x2a89b0 | T = 30 − 230x (x < 0.1); 8.9667 − 19.6667x (x < 0.4); 1.765 − 1.6625x. 30 s to 0.1025 s |
| Sync range | ctor 0x2a8834 (`setFrequencyRange`) | 0.03333 Hz to 9.7561 Hz |
| Depth | `setParamDepth` 0x2a8f78 | d = 0.1692 · depth; LFO amplitude 2d |
| Frequency | `setParamFrequency` 0x2a8fd8 | V0 = 3.145 + 0.3 · (freq − 0.5); sweep is V0 − d … V0 + d |
| Color | `setParamColor` 0x2a9050 | feedback = ∓0.95 · color (sign by stage-count parity) |
| Default Color | `OrangePhaserAU::setDefaultParameterValues` 0x1391b8 | 0.4217; default stage count 4 |
| Control-voltage lowpass | `onSetSampleRate` 0x2a8c10 | one-pole, 22.91 rad/s (3.65 Hz), bilinear with prewarp |
| Audio lowpass | same | one-pole, 103328.5 rad/s (16.4 kHz) |
| Unused shelf | same, state 0x2a0 | 9424.78 rad/s, gain 1.7378; computed, output never read in `processBuffer` |
| Voltage to frequency | pool 0x2bdf1c8, `onSetSampleRate` | f = 49157.8V³ − 417070V² + 1179910V − 1112860 (V < 3.1929), else 12652.6V − 37689.5 |
| Allpass coefficient | `processBuffer` 0x2a9610 | a = 2πf/fs − 1, no prewarp |
| Stage 2 coefficients | pool 0x2be05b0 | d = 1.2030a − 0.2030, e = 1.2030 − 0.2030a, so H2 = 1.2030·AP − 0.2030 |
| Input gain | pool 0x2bdf1dc, `onResume` 0x2a8e5c | 1.862087 on both channels |
| Parameter ramp | `onSetSampleRate` 0x2a8c4c | 0.02 s linear ramps on eight values |

Per-sample flow read from `processBuffer` 0x2a9610 (four float lanes: L, R audio and L, R control voltage):

```
phase += inc (wrap to [-1,1));  tri = |phase|            ; R uses phase + rotate
V  = LP3.65Hz( V0 - d + 2d*tri )
x' = LP16.4kHz( 1.862 * in )
a  = f(V)*2*pi/fs - 1
y1 = AP_a(x')                                            ; s = x - a*y ; y = a*x + s
y2 = (1.2030a - 0.2030)*y1 + s2 ; s2 = (1.2030 - 0.2030a)*y1 - a*y2
u  = y2 + F ;  y3 = AP_a(u) ; y4 = AP_a(y3) ; (extra stages if Notches > 4)
F  = fb * y4                                             ; used on the next sample: unit delay
out = dry*x' + wet*y4                                    ; dry = 1 - mix, wet = +-mix
```

Recheck: `python3 $RE/disasm.py "$GR" --addr 0x2a9610`, `--floats 0x2bdf1c0 8`, `--floats 0x2be05b0 16`.
The transcription is `tools/gr6_mxr.py` and `tools/gr6_mxr_static.py` (analysis only, not for shipping).

## 2. Guitar Rig 6, NI::DSP::RigPhaserBT (rack "Stoned Phaser"); NI::DSP::StonedPhaser is the same algorithm

| What | Address | Value |
| --- | --- | --- |
| Sweep limits by Color | `setParamColor` 0x2d96a8 | off: 307 Hz and 8500 Hz; on: 93 Hz and 2960 Hz |
| Sweep Min/Max knobs | `setParamFreq1` 0x2d985c, `setParamFreq2` 0x2d9920 | multiplier 5^(2x − 1) |
| Sweep law | `setParamColor` 0x2d9774, `processBuffer` 0x2d9f7c | centre √(f1·f2); exponent ln√(f2/f1)/ln 32; f = centre · 32^x by a cubic raised to the fourth power |
| Feedback | `setParamColor`, `setParamColorStrength` 0x2d97f0, `setParamInvert` 0x2d99e4 | c = color · strength · (±1), clamped to ±0.85; init strength 0.74 (pool 0x2bdf310) |
| Sections | `setParamNotches` 0x2d9ad4, init pool 0x2bdf308 | N = 2 second-order sections (four stages) |
| Feedback poles | `updateBaseCoefficients` 0x2d90b4 | r = \|c\|^(1/2N), angles (i + ½[c<0])·π/N: the loop 1 − c·A^2N is factored into N biquads |
| Prewarp | `processBuffer` 0x2da210 | g = x + x³/3 + 2x⁵/15 (0.3333, 0.13333 at 0x2d9fac) |
| Section solve | `processBuffer` 0x2da258 | double precision, one `fdiv` per section and sample (zero-delay form) |
| Mix centre | `onControlRate` 0x2d9eac | wet share at knob 0.5 = min(1, 0.5 + 0.5\|c\|) |
| Input gain | `updateBaseCoefficients` 0x2d92e4 | 1.5 · (1 − 0.9\|c\|) |
| Second path | `onControlRate` 0x2d9ea0, `processBuffer` 0x2da1ec | previous wet sample, filtered by a biquad, times −c, added to the input |
| Network constants | `updateBaseCoefficients` 0x2d9318 | 270 kΩ − 243 kΩ·min(\|c\|/0.7, 1), 300 kΩ, 100 kΩ, 200 kΩ, 5466.37, 2π·60, 2π·50 |

Recheck: `python3 $RE/disasm.py "$GR" --addr 0x2d96a8`, `--addr 0x2d90b4`, `--addr 0x2d9f7c`.

## 3. AmpliTube 5, IK::KIS::FX::AT3::MXRPhaser90::Stomp

Read from `Process` 0xd8984c (scalar, mono, per sample) and `SetSampleRate` 0xd89bb8.

```
sq   = +-1 square at `rate` (0.1 .. 10 Hz, duty 0.5)             ; ctor in Preallocate 0x13d91cc
v    = onepole_lowpass(sq, fc = 0.1135332 * rate)                ; bilinear, 0xd89c44
Vgs  = 1.195402 * v - 2.364943
rds  = 190.4655 / (1 - sqrt(min(Vgs / -2.758621, 0.9996375)))    ; 0 if Vgs >= 0
Rp   = rds * 24000 / (rds + 24000)
K    = Rp * 5e-8 * 2*fs                                          ; bilinear, no prewarp
stage 1:      (K-1 - (K+1) z^-1) / (K+1 - (K-1) z^-1)            ; first-order allpass
stages 2..4:  third-order section, numerator (w-1)^3, denominator (w+1)^3 + k (w-1)^3, k = 0.3678162
out  = 1.0 * in + 1.0 * wet                                      ; gains at +0x90, +0x94
```

The six defaults (0.1 … 10; 0.3678162; −2.364943; 190.4655; 1.195402; −2.758621) are also registered as module parameters
in `IK::MAF::AT3::MXRPhase90::DefineParameters` 0x1788ce0. Derived: v peaks at ±tanh(π·0.1135/2) = ±0.1765, so
Vgs runs −2.576 … −2.154 V, rds 5651 … 1637 Ω, Rp 4574 … 1533 Ω, break frequency 696 … 2077 Hz.
The cubic in K is the bilinear image of A³/(1 + kA³), A = (w − 1)/(w + 1): feedback around three stages, no delay.

Recheck: `python3 $RE/disasm.py "$AT" --addr 0xd8984c | grep -E "0x433e|0xc030|0xc017|0x3f99|0x46bb|0x3faf|0x3f21"`.

## 4. AmpliTube 5, IK::KIS::FX::AT3::ElectroHarmonixSmallStone::Module

| What | Address | Value |
| --- | --- | --- |
| Color off control range | `Color` 0xca3264 | 0.56 … 0.67 (each end trimmable in percent) |
| Color on control range | `Color` 0xca31dc | 0.581 … 0.73 |
| Rate, Color off | `Color` 0xca32d4, exponent pool 0x239e1d8 | 0.07112 + 13.429 · knob^4.589 Hz |
| Rate, Color on | `Color` 0xca324c, exponent pool 0x239e1e0 | 0.04847 + 9.2015 · knob^4.538 Hz |
| Filters | `UpdateFilters` 0xca2b78 | two coefficient branches selected by Color; constants not decoded |
| Process | `Process` 0xca2390 | first-order sections `y = b0·x + s; s = a·y − b1·x`, each run over 4 sub-samples per input sample; no call to a saturating function |

Recheck: `python3 $RE/disasm.py "$AT" --addr 0xca31b8`.
