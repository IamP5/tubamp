# AmpliTube 5 — gate and noise-reduction modules (decompiled)

Binary: /Library/Audio/Plug-Ins/Components/AmpliTube 5.component/Contents/MacOS/AmpliTube 5 (arm64)
Recheck any block: `python3 $RE/disasm.py "$AT" --addr 0x<address>`

## 1. "Noise Gate" stomp — IK::KIS::FX::AT3::NoiseGate::Stomp
The class has no out-of-line members; its loop is inlined in
IK::MAF::AT3::NoiseGate::ProcessBuffer 0x1793718 (and the DynamicWrapper copy 0x1793e7c).

```
a    = |x|                                             0x17937d4
env  = a            if env < a      (instant attack)   0x17937dc
       env*envRel   otherwise       (exp release)      0x17937d8
if env >  thr*kOpen : n = 0                            0x17937e4
if env <= thr*kClose and n < hold : n += 1             0x17937ec..0x1793800
if n >= hold : g = max(g*relCoef, floor)               0x17937b0  closing, exponential, floored
else         : g = g + attCoef*(1 - g)                 0x179380c  opening, one-pole
y = g*x
state meter = (g < 0.5) ? 1 : 0                        0x1793824  parameter "GateState" (MAFProfile 0xaa79d8)
```
Setters (OnSetParameter 0x179385c): thr = 10^(dB/20) 0x17939cc; attCoef = 1 - exp(-2/(t*fs))
0x1793a9c; hold samples = t*fs 0x1793b64; relCoef = exp(-2/(t*fs)) 0x1793c40;
floor = 10^(dB/20), 0 when dB <= -100 0x1793c68. envRel = exp(-2/(0.02*fs)) (factory
SetSampleRate 0x14436f4; 0.02 s default at 0x1443968). So every "time" is 2 time constants.

Defaults of the DSP struct (Preallocate 0x1443904..0x1443974): kOpen = 1.0, kClose = 1.0
(literal 0x318a520 = {0, 0.001, 1, 1}), floor 0.001 (-60 dB), detector release 0.02 s.
OnSetParameter never writes kOpen/kClose, so the shipped stomp has no hysteresis even
though the loop supports it.

Parameter declarations in DefineParameters 0x17930d8, in order (min, max, default):
(-100, 0, -50) 0x1793104; (0.5, 2, 1) 0x17931a8; (50, 200, 100) 0x1793220;
(20, 1500, 200) 0x17932ac; (-100, -20, -60) 0x1793338. Matched to threshold dB,
attack ms, hold ms, release ms, depth dB by the order of the setter branches [inferred].

## 2. MXR Smart Gate model — IK::KIS::FX::AT3::MXRSmartGate::Module
Process 0xd8ac78, three modes selected by the int at +0xc.

Detector (all modes): env = one-pole of |x| with separate attack and release
coefficients (0xd8adac..0xd8add0). coef = 0.1^(1000/(ms*fs)); attack 7 ms, release
200 ms (Initialize 0xd8a970, 0xd8a9b0). Release is a parameter "TrimRelease"
(OnSetParameter 0x178ac0c).
Level: L = env * sens * trig * (HiTriggerRange ? 0.0446684 : 1) with sens = 125.893
(+42 dB, mode 0, 0xd8acc0) or 237.137 (+47.5 dB, modes 1/2, 0xd8ae9c);
trig = 10^((30 - 30*p^1.1681)/20) (0x178ab0c).

Mode 0 (full gate): gain = curve(L), a 333-point table built in Initialize 0xd8aaf8 from
11 breakpoints (level dB -> gain dB): (-25,-30) (-20,-30) (-14,-30) (-9,-28) (-6,-25)
(-5,-22) (-4,-18) (-3,-14) (-2,-9) (-1,-3) (0,0) (literals 0x317c890, 0x31867e0,
0x317c898). Below -25 dB the gain stays at -30 dB: the model never mutes.
Plus a level-indexed RBJ high shelf at 1500 Hz, slope 0.5 (SetSampleRate 0xd8b558),
re-selected every 4 samples (0xd8ae34).
Modes 1 and 2 (hiss / mid): no broadband gain change. Only a level-indexed RBJ high
shelf, 1500 Hz (mode 1, 0xd8b2e4) or 3000 Hz (mode 2, 0xd8b2f4), slope 0.4, gain from 8
breakpoints (level dB -> shelf dB): (-11,-22) (-8,-20) (-6,-19) (-4,-18) (-3,-17)
(-2,-15) (-1,-7) (0,0) (literal 0x317c8a8..). So the whole transition is 11 dB wide and
the deepest cut is a 22 dB treble shelf.
No hysteresis, no hold, no lookahead; chatter is avoided by the 200 ms release and the
soft curve.

## 3. Rocktron Hush IIB model — IK::KIS::FX::AT4::RocktronHushIIB::Module::Process 0xef13cc
Also what the "Stargate" stomp runs: IK::MAF::AT3::StarGate::ProcessBuffer calls it at 0x184c560.
Four stages:
1. feedback compressor: g = -1/max(K*env(y), gmin), y = g*x (two fixed-point passes,
   0xef13f4..0xef1474); followers 16.03 ms attack, 50.66 or 506.57 ms release
   (Initialize 0xef0fb8, 0xef103c, 0xef1048; switched in StarGate::OnSetParameter 0x184c7d4).
2. two one-pole sidechain filters into a control buffer (0xef14a0, 0xef14ec).
3. a state-space rectifier/integrator with +-11.2 V rail clamps (0xef1534..0xef1674),
   offset by the threshold knob: c = v - (11.3 - 2.3*p) (0xef167c..0xef16a0).
4. feed-forward expander y = -K2*env(v)*v (0xef1730..0xef1778), then two cascaded
   bilinear one-pole low-passes with fc = 6402.27*c + 72789.8 Hz recomputed per sample
   (0xef178c..0xef1834): a 12 dB/oct sliding low-pass from about 1 kHz (c = -11.2) to open.
Because the compressor gain is capped at 1/gmin while the expander keeps going, the pair
is unity above the knee and a 1:2 downward expander below it [inferred from the two gain
laws]. Stages 2-3 were not decoded to component values.
