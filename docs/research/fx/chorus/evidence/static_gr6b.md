# Guitar Rig 6: five modulated-delay effects (static reading)

Binary: `/Library/Audio/Plug-Ins/Components/Guitar Rig 6.component/Contents/MacOS/Guitar Rig 6` (arm64 slice).
All commands assume:

```
export RE_STATE=/private/tmp/claude-501/-Users-tuba-Dev-projects-tubamp--claude-worktrees-tubamp-effects-reverse-engineer-90c7f7/cab46780-7fe6-4ba5-87cd-2d4971b7e79d/scratchpad/re
export RE=/Users/tuba/Dev/projects/tubamp/.claude/worktrees/tubamp-effects-reverse-engineer-90c7f7/tools/re
B="/Library/Audio/Plug-Ins/Components/Guitar Rig 6.component/Contents/MacOS/Guitar Rig 6"
```

Dumps in this directory: `hammond.s`, `hammond2.s`, `scanner.s`, `lambdas.s`, `vv.s`, `fm.s`, `ftc.s`, `st.s`.
Immediate constants loaded with `mov/movk` are decoded from the instruction at the named address
(`python3 $RE/disasm.py "$B" --addr <function>` shows them). Pool constants name their pool address.
Hopper was not used: the daemon queue was saturated (other agents' jobs of 800-4000 targets), so all
reading is from `disasm.py`. Stub names were resolved with `otool -Iv "$B"`
(0x2755ecc `__exp10f`, 0x27563d0 `exp`, 0x27563e8 `exp2f`, 0x2756928 `log2f`).

---

## 1. `ni::effektrig::dsp::hammondvibratochorus::HammondVibratoChorus`

This is not a delay-line-with-LFO chorus. It is a circuit model: a wave-digital-filter LC ladder
(the Hammond "line box") whose node voltages are picked off by a 16-position rotary scanner model.

### Signal flow (per sample) [decompiled: Guitar Rig 6 HammondVibratoChorus::processAudioInternal 0x5e0fe0]

1. A gain `g` (one-pole smoothed, state 0x3f0, target 0x3f4) multiplies both inputs; mono = 0.5*(gL*inL + gR*inR) (0x5e1110-0x5e1124).
2. Two 2nd-order state-variable filters run on the mono signal: SVF1 low-pass output (field 0x408) feeds the ladder; SVF2 high-pass output (field 0x430) bypasses the ladder (0x5e1128-0x5e11d4).
3. Mode index (field 0x3e8) <= 2 runs `HammondVibratoChorusLadderCircuit<18u>::process` (0x5e122c); mode >= 3 runs `LadderCircuit<36u>::process` (0x5e1a84) (branch at 0x5e11dc). The ladder writes all node voltages into an `Eigen::Array<float,19>` or `<float,37>` on the stack.
4. `HammondVibratoChorusScanner<float>::process` (0x5e18c0 / 0x5e2258) returns a (L, R) pair.
5. Output: `outX = g*mix*(scanX + 0.5*HP2) + (1-mix)*g*inX`, mix = smoothed param 6 (field 0x54) (0x5e1060-0x5e10ac).

Annotated core of step 5:
```
5e106c ldr s0,[x21,#0x430] ; HP2 output (mono high band)
5e1070 fmul s0,s0,s9      ; *0.5
5e1074 ldr s1,[x21,#0x54] ; mix (param 6)
5e1078 fmul s2,s10,s1     ; g*mix
5e107c fsub s1,s8,s1      ; 1-mix
5e1080 fmul s3,s12,s1     ; (1-mix)*g*inL  (s12 = g*inL)
5e1084 ldr s4,[x9,x25]    ; scanner L output
5e1088 fadd s4,s0,s4      ; scan + 0.5*HP2
5e108c fmul s4,s2,s4      ; *g*mix
5e1090 fadd s3,s3,s4      ; + dry
```

### Band split [decompiled: Guitar Rig 6 HammondVibratoChorus::onSetSampleRateInternal 0x5e258c]
- Both SVFs are tuned to 7100 Hz (immediate 0x45dde000 at 0x5e25d0) with w = pi*fc/fs (pi = 0x40490fdb at 0x5e259c), g = w + w^3/3 + 2w^5/15 (Taylor tan; 0x3eaaaaab = 1/3 at 0x5e2600, 0x3e088889 = 2/15 at 0x5e25f0), damping 2R = 1.41421 (0x3fb504f3 at 0x5e2644) so Q = 0.707 Butterworth.
- 7.1 kHz equals the ladder cutoff 1/(pi*sqrt(LC)) = 7118 Hz for L = 0.5 H, C = 4 nF [inferred]. So the design keeps content above the line's cutoff by routing it around the ladder, at 0.5 gain, which equals the 1/2 divider of a matched line [inferred].

### Ladder (the delay structure)
- WDF: capacitor port resistance T/(2C) (`fadd s3,s3,s3; fdiv s3,s1,s3` with s1 = T at 0x5e2b4c-0x5e2b54), parallel adaptors 1/(1/Rc + 1/R) and series adaptors R + R_L, plus an Eigen scattering-matrix assignment for the tap network [decompiled: Guitar Rig 6 LadderCircuit<18u>::redoPortResistancesAndScatteringMatrices 0x5e2ac0].
- Inductor port resistance is the field at ladder+0 (= fs, stored at 0x5e25b4), i.e. 2L/T = fs, so L = 0.5 H for every section [inferred from 0x5e2b30 `fadd s3,s3,s2`, s2 = fs].
- Shunt capacitors: 4 nF (immediate 0x3189705f at 0x5e080c) with 1 nF (0x3089705f, 0x5e0844) at the end section [decompiled: Guitar Rig 6 HammondVibratoChorus::HammondVibratoChorus 0x5e0528].
- Source resistance 11 kOhm (0x462be000 at 0x5e0794, stored ladder+0x18) and termination 15 kOhm (0x466a6000 at 0x5e07a0, stored ladder+0x94) [decompiled: ctor 0x5e0528]. Z0 = sqrt(L/C) = 11.18 kOhm, so the line is driven almost matched [inferred].
- A resistor array of 27k, 56k, 39k, 33k, 18k, 12k, 68k, 150k, 150k, 180k Ohm sits in the pool (`--floats 0x37f4470 10`) and 180 kOhm (0x482fc800 at 0x5e07e8) is written into the ladder; these are the tap/pickup network resistors [decompiled: ctor 0x5e0528; role inferred].
- Per-section delay sqrt(LC) = 44.7 us; 18 sections = 0.80 ms, 36 sections = 1.61 ms of low-frequency group delay [inferred].

### Scanner [decompiled: Guitar Rig 6 HammondVibratoChorusScanner<float>::process<Array<float,19>> 0x5e18c0]
- Phase: `ph += 2*pi*f*T` (2pi = 0x40c90fdb at 0x5e18c4), wrapped at 2pi; second phase = ph + offset (field scanner+0x164), wrapped (0x5e18c0-0x5e1930).
- Position = ph/(2pi)*16; integer part i, fraction a; next = i+1 with 15 -> 0 wrap (0x5e1934-0x5e197c). So 16 stator positions per revolution.
- Output = (1-a)*V[tap(i)] + a*V[tap(i+1)]: linear crossfade between adjacent stator plates (0x5e1a14-0x5e1a74).
- tap(i) = modeTable[mode][posTable[i]]. posTable (16 ints at scanner+0x114) is a triangle 0..8..1, so one vibrato cycle per revolution, rising then falling through 9 plate groups [decompiled: ctor stores, reconstructed by `python3 $RE_STATE/work/chorus/gr6b/ctor_tables.py $RE $RE_STATE/work/chorus/gr6b/hammond.s`].
- modeTable: 6 modes x 9 ints (scanner+0x3c). Each row runs from node 0 (line input) to a deepest node: mode I -> node 8, II -> 12, III -> 18 (18-section line); IV -> 24, V -> 30, VI -> 36 (36-section line). Spacing within a row widens toward the middle of the sweep and is non-uniform (not reproduced here; see `scanner_tables.txt`).
- Peak delay swing therefore: I 0.36 ms, II 0.54, III 0.80, IV 1.07, V 1.34, VI 1.61 ms [inferred: node count x 44.7 us].
- Implied mean pitch deviation at the default 6 Hz: about 7, 11, 16, 22, 27, 33 cents for I..VI (1200*log2(1+2*D*f)); the piecewise-linear tap spacing makes the instantaneous deviation larger on steep segments [inferred].
- Stereo: if the stereo switch is on (scanner+0x160), R uses the offset phase; else R = L (`fcsel s1,s0,s1,eq` at 0x5e1a7c).

### Parameters (normalized 0..1) [decompiled: lambdas $_0..$_5 0x5e3cb4-0x5e42b4, getParameterString 0x5e0b34]
- Names (from VintageVibratoAU::registerParameters 0x1b230c strings): Stereo, Blend, Mode, Rate, Spread, Color, Width, Mix.
- Rate: f = 6 * 2^(5x - 3) Hz, range 0.75-24 Hz, exponential (lambda $_2 0x5e3fbc: 5.0, -3.0, `exp2f`, 6.0); default x = 0.6 -> 6.0 Hz (`--floats 0x37f4990 4` gives 0, 0, 0.6, 0) [decompiled].
- Mode: index = clamp(int(6x), 0, 5); labels "I".."VI" built from 'I'/'V' chars in getParameterString (0x5e0c44-0x5e0ca8) [decompiled].
- Width: stereo phase offset = x*pi, 0-180 deg (lambda $_4 0x5e41c0); default 0.25 = 45 deg (`--floats 0x37f4498 2`) [decompiled].
- Spread (bool): stereo scanner on when x >= 0.5 (lambda $_3 0x5e40d0) [decompiled].
- Mix: linear dry/wet crossfade, default 1.0 (full vibrato) (`mov w8,#0x3f800000` at 0x5e0fd4) [decompiled]. Chorus is mix < 1: dry at (1-mix), wet at mix. No equal-power law [decompiled: 0x5e1074-0x5e10a8].
- Color: 3 modes, clamp(int(3x),0,2). Mode 0: termination fixed at 15 kOhm. Mode 1: R = 15k * 10^(4.5*(0.3667*(1-blend)) - 2.25) = 84 Ohm .. 3.8 kOhm. Mode 2: exponent argument 0.6333 + 0.3667*blend -> 59 kOhm .. 2.67 MOhm (0x3ebbbba5, 0x3f22221d, 4.5, -2.25, `__exp10f`, 15000 at 0x5e2720-0x5e2788) [decompiled: handleColorModeParameter 0x5e26b4]. Changing the termination mismatches the line: reflections color the response and change how much the far taps carry [inferred].
- Blend: clamped to [1e-6, 1] (0x358637bd at 0x5e3cdc) and written to ladder+8 for both ladders, then the port resistances are recomputed; inside `redo` a resistance R18 * blend^2 is formed (0x5e2cfc-0x5e2d08) [decompiled]. Its circuit role (probably the chorus-blend pot that mixes the line input into the scanner pickup, as on the organ) is [guess].

### Smoothing and click avoidance
- Parameter smoothing: `ModulatableDSPCore<2,2,7>::processParameterValueSmoother` is called every sample (0x5e10c0) [decompiled].
- Mode change: if running, the mode lambda sets the gain target to 0 (0x5e3ec8); processControlRate waits until the smoothed gain reaches 0, then switches ladder/mode and sets the target back to 1 (0x5e2450-0x5e24c4) [decompiled]. Gain smoother: one-pole, coefficient exp(ln(0.05)/(0.01*fs)) i.e. 95 % in 10 ms (`--doubles 0x37fcc30 1` = -2.9957; `exp` call at 0x5e2690), snap threshold 0.001 (0x3a83126f at 0x5e265c) [decompiled].
- Latency 0 (getInternalLatency 0x5e2aa0 returns 0) [decompiled].
- No feedback path exists [decompiled: processAudioInternal has none].

---

## 2. `NI::DSP::VintageVibrato`

`VintageVibrato` is a stereo wrapper around `HammondVibratoChorus`; it has no DSP of its own.
- `EffectBaseStereo<VintageVibrato>::EffectBaseStereo` constructs two `HammondVibratoChorus` cores (calls to 0x5e0b30 at 0x1c52e8 and 0x1c5340) [decompiled: Guitar Rig 6 0x1c5288].
- `VintageVibratoAU::doBuffer` runs core 1 on channels 0/1 via `IDSPCoreToEffectBaseAdapter<HammondVibratoChorus>::processBuffer` (0x1b2900), core 2 on the next channel pair (0x1b2928), then applies a linearly ramped output gain (0x1b2940-0x1b2978) [decompiled: Guitar Rig 6 0x1b2884]. When the flag at +0x6700 is set it memcpy's channels before and after processing (mono handling) [decompiled; exact channel direction not determined].
- Parameter setters `setParameter` idx 0 (Blend), 2 (Rate), 5 (Width), 6 (Mix), `setMode`, `setColor`, `setSpread` are adaptors that forward to the core (symbols 0x1c69c0-0x1c7148) [decompiled].
- `onControlRate` calls `HammondVibratoChorus::processControlRate` for both cores and runs the bypass ramp (vector of 4 gains approaching targets over a fixed number of blocks) [decompiled: Guitar Rig 6 0x1c97fc].
- So all section-1 findings apply; "Vintage Vibrato" in the GR6 UI is this scanner model [inferred].

---

## 3. `NI::DSP::FlangeMistress` (Electric Mistress style)

### Per-sample loop [decompiled: Guitar Rig 6 FlangeMistress::processBuffer 0x280fc4]
4-lane NEON; lanes 0/1 carry L/R delay times, lanes 2/3 carry L/R samples.
- LFO: phase in [-1,1), `ph += inc` (field 0x160), wrap -2 (0x281244-0x281264). Lane 0 = ph + 0x168, lane 1 = lane0 + rotate (0x88), each wrapped; LFO = (|p| - 0.5)*depth(0x188): a triangle in +-0.5*depth (`fabs.2s`, `movi v6 = -0.5` at 0x280ff4, 0x28104c-0x281054).
- Delay target = base(0x210) + LFO, smoothed by a one-pole (state 0x220, coef 0x230) before reading (0x28107c-0x281088).
- Read: integer part and fraction, two adjacent samples, weights (1-frac, frac) (`v1` = {1,1,0,0} from `--floats 0x2be0120 4`): linear interpolation (0x2810b0-0x281154) [decompiled].
- Buffer write = in + fb*(y + 1.5119*(y - LP(y))): a high-shelf/emphasis on the feedback path; LP is one-pole state 0x228/0x22c (0x3fc1857f = 1.5119 at 0x280fd0; 0x281078-0x281098) [decompiled]. Feedback sign positive [decompiled: no negation in the path].
- Output = (dry*in + wet*y)*outGain; dry, wet, fb, outGain are a 4-lane vector ramped per sample (0x1b0 += 0x1c0, 0x2811d0) [decompiled].
- No anti-alias/reconstruction filters, no saturation, no noise, no compander in the loop [decompiled: processBuffer contains no calls and no such arithmetic]. The BBD character exists only in the delay-time law below.

### Constants [decompiled: Guitar Rig 6 FlangeMistress::onSetSampleRate 0x2805b0, setParamDepth 0x280aa8]
- Delay law: tau = 0.5 * 512e-6 * ((x*a + b)*6.6902 - 9.1847) seconds; immediates 6.6902 (0x40d6161e), -9.1847 (0xc112f488), 0.000512 (0x3a0637bd). The form N/(2*f_clk) with N = 512 matches a 512-stage BBD whose clock period (us) is a linear function of the knob [inferred].
- Static ("filter matrix") mode, a = 4.248 (0x4087ef9e), b = 1.644 (0x3fd26e98): tau = 0.46 ms .. 7.74 ms; LFO depth 0 (0x280b20) [decompiled].
- Sweep mode, a = 1.784 (0x3fe45a1d), b = 2.42 (0x401ae148): centre 1.79 .. 4.85 ms; depth = 2*(centre - 0.2178 ms) (0xb96454bd = -0.0002178 s at 0x280b90), so the sweep always bottoms at 0.218 ms and peaks at 2*centre - 0.218 = 3.37 .. 9.48 ms [decompiled + arithmetic].
- Buffer: 2 << int(log2(fs*0.00774)) samples (0x3bfd9eee = 0.00774 s, `log2f`), 512 at 48 kHz [decompiled].
- Delay-time smoother: one-pole, coef 2*pi*2.3/fs (0x40c90e56 = 6.283, 0x40133333 = 2.3 at 0x280680-0x2806a0) [decompiled]. This glides the delay when Depth/Color/static change and slightly rounds the triangle corners only at slow rates [inferred].
- Feedback emphasis LP: one-pole at 146.8 Hz (0x4312d517 at 0x2806a8) [decompiled]. y + 1.512*(y - LP) = 2.51*HP-ish boost above ~150 Hz: thins low end in the regenerated signal [inferred].
- Color = feedback: fb = 0.3*color (0x3e99999a at 0x280a04), so regeneration never exceeds 0.3 before the emphasis [decompiled: setParamColor 0x280958]. For color > 0.5 an output trim is looked up from a runtime-filled table (BSS at 0x47db080, 2400 entries, index (15 - 30c)*9.966 + 1200) [decompiled]; it is a dB-to-gain table giving 0 dB at c = 0.5 falling to about -15 dB at c = 1 [inferred].
- DryWet: dry = (1-m)*1.2589, wet = +-m*1.2589 (+2 dB, 0x3fa12478), sign flipped by Invert (0x280904, 0x280d4c) [decompiled]. Linear law.
- Rotate (stereo LFO offset, phase span 2 = 360 deg): offset = 2r + (r < 0.5 ? 1 : -1): r = 0 -> 180 deg, r -> 0.5 -> 360/0 deg, r = 1 -> 180 deg (0x280d28) [decompiled].
- Rate: forwarded to a shared LFO/tempo-sync helper at +0x98 (`setParamRate` -> 0x60d464, `setParamSync` -> 0x60d3b0); its law was not read here (covered by the LFOHelper study) [decompiled: 0x280a90].
- Parameter ramps: control block = 20 ms (0x3ca3d70a = 0.02 at 0x2805cc); the 4 gains ramp linearly over that period (onControlRate 0x280e44) [decompiled].

---

## 4. `NI::DSP::FlangerTC` (TC Electronic chorus/flanger style)

### Per-sample loop [decompiled: Guitar Rig 6 FlangerTC::processBuffer 0x281918]
- LFO: a counter (0xf4) advances by rate*(1/fs); every 0.5 it wraps and flips a sign (0xf8). The sign (a +-1 square wave) plus an offset (0x98) is scaled (0xf0) and passed through two cascaded one-pole low-passes (states 0xd0, 0xd4, shared coef 0xd8) (0x281960-0x2819a8). So the LFO is an RC-filtered square, like an analog TC LFO [decompiled].
- LFO filter coef = 2*pi/fs * min(rate, 3.28 Hz) in mode 0, min(rate, 2 Hz) otherwise (0x4051eb85 = 3.28 at 0x281530) [decompiled: updateMode 0x2813c0]. At low rates the corner tracks the rate (rounded square ~ triangle-ish); at high rates the cap makes the wave smaller and more sinusoidal [inferred].
- Delay = (1 + lfo) * D * fs, clamped to [1, max] (0x2819f8-0x281a14). D = 4.4 ms in mode 0 (0x3b902de1), 1.39 ms in modes 1/2 (0x3ab630aa) [decompiled: updateMode 0x2813cc-0x2813e8].
- Read: 4-point, 3rd-order Lagrange interpolation, coefficients (-2a - 3b + 6c - d)/6, (3a - 6b + 3c)/6, (d - a + 3b - 3c)/6 (0x281a88-0x281af0) [decompiled].
- Input: mono (L+R)*0.5*fade + 1e-20 denormal guard (0x1e3ce508 at 0x281940) [decompiled]. Pre-filter one-pole LP at 4 kHz, DC gain 0.54 (0x46c4597c = 2*pi*4000, 0x3f0a3d71 = 0.54 at 0x2816c0) [decompiled: onSetSampleRate 0x2815f8].
- Feedback path: fb*norm*(x' + interp), then a one-pole high-pass at 472 Hz (coef 2*pi*472/fs, 0x43ec0000 at 0x281700) before the buffer write (0x281b14-0x281b3c) [decompiled]. A 141 Hz one-pole (0x430d0000 at 0x281710) is updated in the loop but its output is not used in the signal path (0x281b00-0x281b10) [decompiled].
- Intensity per mode: mode 0 -> 0.9*I, mode 1 -> 1.85*I, mode 2 -> 0.64*I written to different lanes (feed-forward vs feedback); normalisation 1/(1 + 0.04*k) (0x3f666666, 0x3feccccd, 0x3f23d70a, 0x3d23d70a) [decompiled: setIntensity 0x282114]. Input gain 0.5 (0.425 in mode 2, 0x3ed9999a) [decompiled]. Which mode is chorus/flanger/pitch is [guess]: mode 0 with 4.4 ms reads as chorus.
- Stereo: setStereo writes 2.0 or 0 to a lane (0x2820a8); outR = (1-f^2)*dry + f*(y + (x' - y)*k120), outL = (1-f^2)*dry + f*y, so R gets a different blend of the pre-delay signal and the delay output (0x281b40-0x281b70) [decompiled; the role of 2.0 not traced].
- Mode change: setMode zeroes the fade lane (0x168) and defers updateMode, so the wet path fades out, switches, fades in; output crossfade dry->processed with (1 - f^2) / f weights (0x2820ec, 0x281b44-0x281b6c) [decompiled].
- Width: w^2 * modeDepth[mode], table {1.2, 2.8, 4.5} (`--floats 0x2c46ddc 3`) -> LFO amplitude (0xf0) [decompiled: setWidth 0x281590].
- Rate law: setLFORate uses 440, 12, 1/12, 1236.4, 1381.9 and `pow` — a note/semitone style law not decoded [decompiled: 0x281e90; meaning not determined].

---

## 5. `NI::DSP::StereoTouch`

StereoTouch is not a short-delay chorus. It is a micro-pitch-shift widener applied above a crossover,
followed by fixed delays and an M/S spread control.

### Per-sample loop [decompiled: Guitar Rig 6 StereoTouch::processBuffer 0x301ecc]
- Each input channel goes through a state-variable filter (L states 0x18c/0x194, R 0x19c/0x1a4) at the Split frequency (0x302340-0x302398) [decompiled].
- The shifter input is (HP_L + HP_R + BP_L + BP_R) * gains: only the band above Split, summed to mono, is widened (0x3023b0-0x3023cc) [decompiled]. The low band is never shifted [inferred].
- Shifter: one circular buffer of N = floor(fs*512/44100) samples (0x472c4400 = 44100, 0x44000000 = 512 at 0x301938), two read heads N/2 apart, crossfaded by a Hann table 0.5 - 0.5*cos(2*pi*k/N) (`--doubles 0x2bdfdd0 1` = 2pi; 0x301a38-0x301a58) [decompiled: onSetSampleRate 0x301880]. N = 11.6 ms.
- Two shifter voices run on the same buffer: read pointers step by r and 1/r per sample (0x17c, 0x174) (0x30209c, 0x3023e0) [decompiled]. One output goes to the L delay line, the other to R, so L and R are detuned in opposite directions [decompiled; which side goes up is not determined].
- Reads use 4-point cubic Hermite (Catmull-Rom): c1 = 0.5(x2 - x0), c2 = x0 - 2.5x1 + 2x2 - 0.5x3, c3 = 0.5(x3 - x0) + 1.5(x1 - x2) (0x301fa0-0x301fe8) [decompiled].
- Delay lines of 40 ms (0x3d23d70a = 0.04 at 0x301920); taps at B*0.5*0.5 = 10 ms for L and B*0.5 = 20 ms for R (field 0x144 = 0.5 from `--floats 0x37fcd70 2`; tap math 0x301a5c-0x301ae0), crossfaded by a smoothed lane (0xb0 = 1.0) (0x302200-0x302228) [decompiled]. The 10/20 ms offsets decorrelate L/R further (Haas region) [inferred].
- Output: wet X' = 0.5*(tap + previous tap) (2-tap average); then with sp = spread lane, m = mix:
  `outL = (1-m)*inL + m*(0.5*(1+sp)*L' + 0.5*(1-sp)*R')`, `outR = (1-m)*inR + m*(0.5*(1+sp)*R' + 0.5*(1-sp)*L')` (0x30225c-0x302300) [decompiled].
- Mono sum: outL + outR = (1-m)(inL+inR) + m(L' + R'); Spread cancels in the sum, so Spread never changes the mono sum. The wet L' + R' is the sum of the up- and down-shifted copies delayed 10 and 20 ms, which comb-filters slightly in mono (detune makes the combs move, so it is a slow chorus-like wash, not a static notch) [inferred].

### Parameters [decompiled: setters 0x301b80-0x301c6c]
- Split: f = 100 + 900*x Hz (0x44610000 = 900, 0x42c80000 = 100), w = 2*pi*f/fs (0x301c1c) [decompiled].
- Drift: d' = 0.15 + 0.85*x (0x3e19999a, 0x3f59999a); detune = 4*(0.4*d')^3 semitones (0x3ecccccd, pow 3, *4), r = 2^(detune/12) (`--doubles 0x37fd080 1` = 1.0594631) [decompiled: setParamDrift 0x301b98]. Range 0.09 cents (x=0) .. 4.9 cents (x=0.5) .. 25.6 cents (x=1), cubic law [inferred arithmetic].
- Spread: sp = 2*(0.01 + 0.99*x) = 0.02..2 (0x3c23d70a, 0x3f7d70a4) [decompiled: setParamSpread 0x301c6c]. sp = 1 passes L'/R' unchanged; sp < 1 narrows toward mono; sp > 1 adds anti-phase crosstalk (wider than the source) [inferred].
- Mix: linear (0x301b80) [decompiled]. Control block 20 ms, parameters ramp linearly [decompiled: onSetSampleRate 0x3018b0].
- SVF damping field 0x184 was not located; filter Q is not determined.

---

## Cross-cutting observations for a chorus design
- Linear interpolation (Mistress), 3rd-order Lagrange (TC), cubic Hermite (StereoTouch), WDF ladder with linear plate crossfade (Hammond) all appear; the modern NI code chose Lagrange/Hermite where modulation is wide [decompiled per section].
- Every effect fades or ramps on mode change: gain-to-zero-then-switch (Hammond, TC) or 20 ms linear ramps of gains (Mistress, StereoTouch) [decompiled].
- Delay times are smoothed with one-pole filters (Mistress 2.3 Hz, TC LFO 2-3.28 Hz cap), not jumped [decompiled].
- None of these five uses BBD filtering, companding or noise; the Mistress BBD flavour is only the N/(2 f_clk) delay law [decompiled].

## Not determined
- Hammond "Blend" circuit role; exact default of Mode/Color/Stereo.
- VintageVibrato mono-copy channel direction; AU-side default values (read from AU members at +0x2a0).
- FlangeMistress Rate law (shared helper at 0x60d464).
- FlangerTC mode names, rate law, role of the stereo 2.0 lane.
- StereoTouch shift direction per side, SVF Q.
