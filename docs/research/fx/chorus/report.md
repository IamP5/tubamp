# Chorus: reference study and improvement spec

## Verification status

Independently verified on 2026-11-11: 20 of 21 claims verified, 1 partly wrong. The full table is in `verification.md`.
Read these corrections before relying on the numbers below. The body of the report is unchanged.

- C20: the cited page gives the Dimension D's chips, single LFO and modes but does not say the second line inverts the LFO. That statement is in a comment reply on another page.
- R1 Wide: the 8 ms delay clamp breaks the constant-cents law below 1.09 Hz at full depth. The mix law is credited to Choral, but Choral's dry measures 1 - mix/2 (-6 dB at full), not unity.
- R2 CE: it assumes the rate stops at 2.86 Hz. At tubamp's 10 Hz the fixed sweep gives about 91 cents, not 29.
- R3 Dimension: the 5 to 8 cents target uses the sine formula, but the design specifies a triangle (3.5 to 5.2 cents), so its first preset fails its own acceptance band. It also leans on C20 and a tagged guess.
- The Summary's "every reference chorus uses a triangle and band-limits the wet" is contradicted by section 3. The author's click method reads up to 1.2 cents high.
- The 20 ms mode-switch fade was read by a helper and re-read by nobody.

Paths in tags are relative to `docs/research/fx/chorus/evidence/` unless they start with `src/` or `juce_dsp/`.
`C` is `/Library/Audio/Plug-Ins/Components`. Static notes written by helper readers are kept as
`static_at5.md`, `static_gr6a.md`, `static_gr6b.md`, `static_vsm.md`, and the literature notes as `literature.md`.
Section 8 lists which helper-read constants the orchestrator re-read or confirmed by measurement.

## 1. Summary

- tubamp ships one sine-modulated delay line shared by both channels, linear interpolation, no wet filter and a depth knob in milliseconds; pitch wobble runs from 2 cents to over 800 cents across the knob ranges and the block adds no stereo width [measured: `tubamp_today.tsv`].
- Every reference chorus is multi-voice or stereo-voiced, uses a triangle LFO for chorus, band-limits the wet path and keeps pitch deviation under about 30 cents [measured: `brigade_chorus.tsv`, `choral_rate_amount.tsv`, `gr6_ensemble.tsv`].
- Choral sets depth in cents and divides the delay swing by the LFO rate, so the rate knob never changes how out of tune it sounds [decompiled: Choral `Choral::handleParameterRate` 0x884ec] [measured: `choral_rate_amount.tsv`].
- No readable reference models BBD clocking, compander or noise for chorus; they reduce the BBD to a delay curve and a low-pass [decompiled: AmpliTube 5 `ChorusCE1::Module::Process` 0xdf9004], and the UADx CE-1 model is level-independent to 0.01 dB [measured: `brigade_level.tsv`].
- Recommendation: add an own chorus engine behind a new `mod_chorus_mode` choice (0 keeps the JUCE path bit-for-bit) with four voicings: Wide, CE, Dimension, Tri.

## 2. tubamp today

Signal flow of `Modulation` with `Type::chorus`:

- The block wraps `juce::dsp::Chorus<float>` [inferred from `src/dsp/FxBlocks.h:124`].
- Centre delay is fixed at 7.0 ms and feedback at 0 in `prepare` [inferred from `src/dsp/FxBlocks.cpp:358-359`].
- The delay is `max(1 ms, 7 ms + 20 ms * lfo * 0.5 * depth)`, so depth 1 means a swing of plus or minus 10 ms [inferred from `juce_dsp/widgets/juce_Chorus.h:124,171-172`].
- At depth above 0.6 the delay hits the 1 ms floor and the sine is clipped flat for part of the cycle [measured: `tubamp_today.tsv`].
- The LFO is a sine (`std::sin`) [inferred from `juce_dsp/widgets/juce_Chorus.cpp:42`].
- One LFO buffer drives every channel, so left and right get the same delay and the block adds no width [inferred from `juce_dsp/widgets/juce_Chorus.h:117-141`].
- The delay line uses linear interpolation [inferred from `juce_dsp/widgets/juce_Chorus.h:158`]. Linear interpolation loses up to 2.0 dB at 10 kHz and 5.1 dB at 15 kHz, varying with the fractional position [measured: `interp_loss.tsv`].
- Dry/wet uses the linear rule: wet gain = mix, dry gain = 1 - mix [inferred from `juce_dsp/widgets/juce_Chorus.cpp:45`]. At the default mix 0.35 the wet sits 5.4 dB under the dry [inferred].
- Depth is smoothed with a 50 ms linear ramp [inferred from `juce_dsp/widgets/juce_Chorus.cpp:128`]. The delay time itself is set per sample [inferred from `juce_dsp/widgets/juce_Chorus.h:141`].
- There is no filter in the wet path [inferred from `juce_dsp/widgets/juce_Chorus.h:128-149`].
- Parameters: `mod_rate` 0.05 to 10 Hz log, default 1 Hz; `mod_depth` 0 to 1, default 0.4; `mod_mix` 0 to 1, default 0.35 [inferred from `src/Parameters.cpp:102-104`]. Three instances exist (`mod`, `mod2`, `mod3`) [inferred from `src/Parameters.cpp:178-190`].
- The block runs on the stereo chain block [inferred from `src/PluginProcessor.cpp:1405-1415`].

Resulting pitch deviation (sine LFO, peak ratio `1 +- 2*pi*rate*D`) [measured: `tubamp_today.tsv`]:

| rate | depth | delay range | peak pitch |
| --- | --- | --- | --- |
| 0.05 Hz | 0.4 | 3 to 11 ms | 2 cents |
| 0.5 Hz | 0.4 | 3 to 11 ms | 22 cents |
| 1 Hz (default) | 0.4 (default) | 3 to 11 ms | 43 / 44 cents |
| 3 Hz | 0.4 | 3 to 11 ms | 126 / 136 cents |
| 10 Hz | 0.4 | 3 to 11 ms | 388 / 501 cents |
| 1 Hz | 1.0 | 1 to 17 ms, clipped | 105 / 112 cents |

The default setting already detunes by 43 cents, more than any reference chorus measured here [measured: `tubamp_today.tsv`, `brigade_chorus.tsv`].

## 3. Reference implementations

### 3.1 Native Instruments Choral (primary)

Static source: `$C/Choral.component/Contents/MacOS/Choral`, class `ni::effektrig::dsp::choral::Choral`. Hopper was not available; everything is from `disasm.py`.

Structure

- Seven `ChorusVoice<22050>` objects, processed in double precision with left and right as the two lanes of one SIMD vector [decompiled: Choral `Choral::processAudioInternal` 0x86068, seven calls to 0x8731c at 0x8664c-0x8669c].
- Both lanes of a voice share one delay time; stereo comes from panning voices, not from per-channel LFOs [decompiled: Choral `ChorusVoice<22050>::process` 0x8731c, one scalar delay at 0x87558].
- Voices are used in pairs. The Voices knob fades pairs 2 and 3 in with a square-root law and voice 7 has output gain 0 [decompiled: Choral lambda `$_4` 0x8a054].
- Width rotates each pair to opposite sides by plus or minus `width * 45` degrees, scaled by 1, 5/7 and 3/7 for pairs 1 to 3 [decompiled: Choral `Choral::handleParameterWidth` 0x88670]. At 100 % and mono input voice 1 lands on the left and voice 2 on the right [measured: `choral_modes.tsv`].

Voice

- Wet pre-filter: a 2-pole Butterworth state-variable filter, output `LP*(1-t) + HP*t` [decompiled: Choral `ChorusVoice<22050>::process` 0x87338-0x8739c; coefficients with `sqrt(2)` damping at `Choral::updateModeDependentValues` 0x86f1c-0x86f68].
- Delay read: 4-point third-order Hermite (Catmull-Rom) [decompiled: Choral `ChorusVoice<22050>::process` 0x8755c-0x875ec, excerpt in `choral_hermite.txt`].
- LFO 1 is a triangle `1 - |2 - 4p|` from a phase accumulator [decompiled: Choral `lfo_shaper::triangle_shaper` 0x88e80, bound in the voice constructor at 0x88c30].
- LFO 2 is a sine, enabled per voice, running at a multiple of the voice's LFO 1 rate [decompiled: Choral `lfo_shaper::sine_shaper` 0x88e9c bound at 0x88c6c; rate product at `Choral::handleParameterRate` 0x885b4-0x885dc].
- The unipolar modulator is `m = 0.25*(1+l)*(2 - (1-l)*k)`: linear for `k = 0`, squared for `k = 1` [decompiled: Choral `ChorusVoice<22050>::process` 0x87508-0x8752c]. This bends the triangle the way a BBD clock oscillator does [inferred].
- The modulator passes a one-pole low-pass before it scales the delay, per sample [decompiled: Choral `ChorusVoice<22050>::process` 0x87530-0x87558].
- `delay = base + depth * m` in samples, computed every sample [decompiled: Choral `ChorusVoice<22050>::process` 0x87554-0x8755c].

Parameter laws

- Rate: `0.25 + 7.4 * p^3` Hz, so 0.25 to 7.65 Hz [decompiled: Choral `Choral::handleParameterRate` 0x88514-0x88538, constant 7.4 at 0x88524].
- Amount: `a = 0.01 + 0.99 * p^2`, target `30 * a` cents [decompiled: Choral lambda `$_1` 0x898f4-0x89958].
- Depth: `depth_samples = fs * (2^(30a/1200) - 1) / (2 * f_voice)` [decompiled: Choral `Choral::handleParameterRate` 0x885e0-0x8860c]. A triangle of swing `D` at rate `f` has slope `2 f D`, so this holds the pitch offset at the Amount value for any rate [inferred].
- Measured: Amount 50 % gives 7.9, 8.4 and 8.4 cents at 0.25, 1 and 3 Hz; Amount 100 % gives 31 to 32 cents at all three rates [measured: `choral_rate_amount.tsv`].
- Delay knob: multiplies every base delay by `0.1 + 5 * p` [decompiled: Choral `Choral::updateModeDependentValues` 0x86f88-0x86fa4].
- Mix: `wet = p`, `dry = 1 - p`, linear [decompiled: Choral lambda `$_9` 0x8a7bc]. The wet bus already contains the voice input, so Mix 100 % is dry and delayed signal at equal gain [decompiled: Choral `Choral::processAudioInternal` 0x863cc-0x863d4] [measured: `choral_modes.tsv`, renders at Mix 100 % still show the undelayed tap].
- Invert flips the delayed signal and the feedback sign [decompiled: Choral lambda `$_8` 0x8a6cc].
- Feedback: gain `g = 0.999 * p^(1/4)`; each voice gets `g^(delay_k/delay_1)` so all voices decay in the same time; input is scaled by `(1 - g^4)^(1/4)` [decompiled: Choral lambda `$_6` 0x8a28c-0x8a3b8].
- Scatter mixes the seven voice outputs through a circulant matrix before feedback [decompiled: Choral lambda `$_7` 0x8a574; matrix products at `Choral::processAudioInternal` 0x861ec-0x863a8].

Modes (voice constants) [decompiled: Choral `Choral::setModeSmoothingTargetValues` 0x87a70, summarised in `choral_mode_table.tsv`]

| mode | base delay per pair | LFO phases in a pair | rate ratios | second LFO | wet low-pass |
| --- | --- | --- | --- | --- | --- |
| Synth | 3, 7, 11 ms | 0 / 180 deg | 1, 1.43, 1.72 | almost none | 5, 5, 10 kHz |
| Ensemble | 7, 7, 13 ms | 0/180, 120/300, 240/60 deg | 1 | sine at about 4.1 x rate, full amount | 4, 6, 10 kHz |
| Dimension | 11 ms | 0/180, 180/0, 90/270 deg | 1 | none | 12 kHz; pair 2 high-passed at 150 Hz |
| Universal | 7, 11, 13 ms | 0/0, 0/90, 0/90 deg | 1, 1.43, 1.72 | pairs 2, 3 | 20 kHz |

Measured at the defaults (0.25 Hz, Amount 50 %, first pair) [measured: `choral_modes.tsv`]:

| mode | delay range | swing | pitch (95th pct) |
| --- | --- | --- | --- |
| Synth | 3.09 to 11.68 ms | 8.59 ms | 8.9 cents |
| Ensemble | 8.4 to 17.8 ms | 9.4 ms | 19.1 cents (second LFO adds) |
| Dimension | 11.04 to 19.73 ms | 8.69 ms | 8.3 cents |
| Universal | 7.02 to 15.75 ms | 8.73 ms | 7.8 cents |

Wet response: Synth is 3.0 dB down at 5 kHz and 14.6 dB down at 10 kHz, which fits the 5 kHz 2-pole filter in the table [measured: `choral_wet_fr.tsv`].

Smoothing

- Ten parameters ramp linearly per sample, and each one that is moving calls its recompute lambda every sample [decompiled: Choral `ModulatableDSPCore<2u,2u,10u>::processParameterValueSmoother` 0x86738].
- A mode change does not fade or reset: 63 internal targets are smoothed per sample and the voice constants are recomputed while the flag at `this+0x515` is set [decompiled: Choral `Choral::processAudioInternal` 0x866fc-0x86710 calling `SmoothingToolbox<63>::onProcessSample` 0x86a08 and `updateModeDependentValues` 0x86e10].

### 3.2 Guitar Rig 6

`RigEnsemble` (the "Ensemble" chorus, a CE-1 type) [decompiled: Guitar Rig 6, addresses in `static_gr6a.md`]

- One delay line per channel, one tap, 2-point linear interpolation, no feedback [decompiled: Guitar Rig 6 `RigEnsemble::processBuffer` 0x2c4418-0x2c44bc].
- Triangle LFO from a phase accumulator; the right LFO leads by up to 90 degrees with the Stereo control [decompiled: Guitar Rig 6 `RigEnsemble::processBuffer` 0x2c43b0-0x2c4404, `setParamStereo` 0x292004]. Measured: 46 degrees at Stereo 0.5 and 92 degrees at Stereo 1 [measured: `gr6_ensemble.tsv`].
- Chorus mode: the sweep is fixed at 2.6 ms peak to peak (constant 0.0026 at 0x2933d0) and the Intensity knob sets the LFO rate `0.1 + 2.9*I` Hz (constants at 0x2932e4-0x2932f8) [decompiled: Guitar Rig 6 `RigEnsemble::setParamMode` 0x293358, `setParamChorusIntens` 0x2932d4]. Measured: swing 2.60 ms at 1.55 Hz and 2.58 ms at 3.01 Hz, pitch 14.3 and 28.0 cents, pitch trace square (crest 1.05) [measured: `gr6_ensemble.tsv`].
- The Delay knob moves the sweep centre; the measured wet tap spans 0.40 to 2.97 ms at Delay 0 and 3.78 to 6.36 ms at Delay 1 [measured: `gr6_ensemble.tsv`].
- Vibrato mode: rate 3.24 to 11.56 Hz, and the delay-time signal is low-passed at 1.62 Hz (constant 0x3fcfa787 at 0x293380), which turns the triangle into a near sine [decompiled: Guitar Rig 6 `RigEnsemble::setParamMode` 0x293358]. Measured at the defaults: 6.1 Hz, 11 to 12 cents, delay crest 1.39 [measured: `gr6_ensemble.tsv`].
- Tone: input 26 Hz high-pass and 10.5 kHz low-pass; the delay feed adds 4.19 kHz and 14.9 kHz low-passes; a Treble one-pole follows the delay [decompiled: Guitar Rig 6 `RigEnsemble`, `static_gr6a.md`].
- Mix is a linear crossfade with the wet side boosted by 4.07 (12.2 dB) [decompiled: Guitar Rig 6 `RigEnsemble::processBuffer` 0x2c4578-0x2c45a4].
- No BBD clock, compander or noise model; `BBDSaturator` and the `DelayLineVariable*` helpers are not called by it [decompiled: Guitar Rig 6 `RigEnsemble::processBuffer`, no calls in the loop].

`ChorusFlangerTCSCF` (TC Stereo Chorus Flanger model, inside Replika's FX unit) [decompiled: Guitar Rig 6, `static_gr6a.md`]

- One mono delay, 4-tap cubic Lagrange read [decompiled: Guitar Rig 6 `ChorusFlangerTCSCF::processAudio` 0x66eb34-0x66ebf0].
- LFO is a square wave through two one-pole low-passes whose corner follows the rate (capped at 3.28 Hz chorus, 2 Hz otherwise) [decompiled: Guitar Rig 6 `ChorusFlangerTCSCF::processAudio` 0x66ea58-0x66ea98].
- Delay is `base * (1 + lfo + offset)` with base 4.4 ms (chorus) or 1.39 ms; depth has a square taper [decompiled: Guitar Rig 6 `ChorusFlangerTCSCF::processAudio` 0x66eae8-0x66eb14].
- The right output can carry the wet inverted, which cancels the wet in a mono sum [decompiled: Guitar Rig 6 `ChorusFlangerTCSCF::processAudio` 0x66ebf4-0x66ec78].
- A mode switch fades the output out over about 20 ms, switches, and fades back in [decompiled: Guitar Rig 6 `ChorusFlangerTCSCF::applyTargetMode` 0x66e5ec].

Relatives [decompiled: Guitar Rig 6, `static_gr6b.md`]

- `hammondvibratochorus` is a wave-digital LC ladder (18 or 36 sections, about 44.7 microseconds each) scanned at 16 positions with linear crossfade; chorus is the vibrato mixed linearly with dry [decompiled: Guitar Rig 6 `HammondVibratoChorusScanner::process` 0x5e18c0].
- `FlangeMistress` uses a triangle LFO, a 2.3 Hz one-pole on the delay time, linear interpolation and a delay law that fits a 512-stage BBD, with no BBD filters or noise [decompiled: Guitar Rig 6 `FlangeMistress::processBuffer` 0x280fc4, `setParamDepth` 0x280aa8].
- `StereoTouch` widens with opposite micro pitch shifts (up to 25.6 cents) applied only above a split frequency [decompiled: Guitar Rig 6 `StereoTouch::processBuffer` 0x301ecc].

### 3.3 AmpliTube 5 (static only; it does not load in `auhost`)

[decompiled: AmpliTube 5, addresses in `static_at5.md`]

- All chorus delay reads are 2-point linear interpolation [decompiled: AmpliTube 5 `DelayLineVariable` 0x10e2330-0x10e236c, `ChorusCE1::Module::Process` 0xdf90e4-0xdf90f4].
- No AmpliTube chorus has feedback, clock-tracking BBD filters, compander or noise [decompiled: AmpliTube 5, `static_at5.md` comparison table].
- `RackAnalogChorus` is `KIS::FX::AT4::ChorusCE1`. Chorus mode: triangle LFO at `1 + 2*Intensity` Hz with a fixed control swing; vibrato mode: sine at 3.4 to 12.5 Hz whose depth shrinks as the rate rises [decompiled: AmpliTube 5 `AnalogChorus::OnSetParameter` 0x152a400-0x152a694].
- The CE-1 control voltage goes through a 9-point voltage-to-delay table spanning 1.138 to 2.943 ms [decompiled: AmpliTube 5 `ChorusCE1::gcV2TCurve` 0x30c6a2c].
- CE-1 outputs: one is 100 % wet and the other is dry unless StereoMode is on [decompiled: AmpliTube 5 `ChorusCE1::Stereo::Process` 0xdf93a0].
- `RackFulltoneChorus` is a tri-chorus: three delay lines, a three-phase sine LFO 120 degrees apart, two more LFOs summed in, left = voice 1 + 2, right = voice 2 + 3, wet inverted against dry [decompiled: AmpliTube 5 `FullToneTERC::Module::Process` 0xe2da68].
- The generic chorus graph (`StompChorus`, `RackDigitalChorus`, Fender Triangle) is LFO, scale/offset, one or two linear-interpolated delay lines with antiphase LFOs, and a wet/dry sum [decompiled: AmpliTube 5 `ChorusStereo::DefineParameters` 0x10ddf20].
- `StompMXRStereoChorus` shrinks the delay swing as the rate rises and boosts the wet with a 200 Hz shelf and a 6.5 kHz bell [decompiled: AmpliTube 5 `MXRStereoChorus::Module::SetSampleRate` 0xf8d228-0xf8d568].

### 3.4 ValhallaSpaceModulator

[decompiled: ValhallaSpaceModulator, `static_vsm.md`]

- Every mode reads its delay lines with a first-order allpass interpolator [decompiled: ValhallaSpaceModulator `ProcessBlockUp` 0x3c5c0-0x3c644]. The wet tap is flat within 0.4 dB from 1 to 15 kHz in the median [measured: `valhalla_flair.tsv`].
- LFOs are a 32-bit phase accumulator into 256-entry tables with linear interpolation [decompiled: ValhallaSpaceModulator `ProcessBlockUp` 0x3c2e8-0x3c3dc].
- Ensemble modes use four lines with sine LFOs at 0/90/180/270 degrees [decompiled: ValhallaSpaceModulator `processBlock` 0x193f8].
- Wet/dry is equal power, `sin` and `cos` of `mix*pi/2` [decompiled: ValhallaSpaceModulator `CalcParameter` 0x3b69c-0x3b6d4].
- Depth and manual are smoothed per sample by a 300 ms one-pole [decompiled: ValhallaSpaceModulator `SetSampleRate` 0x3c0ac-0x3c0c4].
- The feedback loop has a 10 Hz high-pass and `0.5*tanh(2v)` [decompiled: ValhallaSpaceModulator `ProcessBlockUp` 0x3c6b8-0x3c718].

### 3.5 Flair (secondary)

- Four pitch-tuned comb voices at 2x oversampling with a cubic delay read: a harmonic flanger, not a chorus [decompiled: Flair `setVoiceDelayTimes` 0x8c6b8-0x8c80c, `onSetSampleRate` 0x8edcc-0x8ee60]. At the defaults one channel shows the dry tap and one comb tap 4.12 ms later [measured: `valhalla_flair.tsv`]. It adds nothing to the chorus design beyond another NI cubic read [inferred].

### 3.6 UADx Brigade Chorus (CE-1 model, black-box only)

- Chorus mode: one wet tap sweeping 1.70 to 4.38 ms (2.67 ms swing, centre 3.04 ms) with a triangle; the Intensity knob changes only the LFO rate, 0.43 to 2.86 Hz [measured: `brigade_chorus.tsv`].
- Pitch offset in chorus mode: 4.5 cents at the slowest setting, 19 cents at the default, 29 cents at the fastest; the pitch trace is close to a square wave (crest 1.14 to 1.29) [measured: `brigade_chorus.tsv`].
- Vibrato mode: sine, 3.4 to 12.4 Hz, swing 0.22 to 3.16 ms, 4 to 75 cents; the swing shrinks as the rate rises [measured: `brigade_chorus.tsv`].
- Classic mode: the left output is dry only and the right output is wet only [measured: `brigade_taps.txt`]. Dual mode: both outputs carry dry and wet, and the right LFO is 89 to 95 degrees from the left [measured: `brigade_chorus.tsv`].
- Wet path: about 6 dB hotter than dry at 1 kHz, 3 dB down near 130 Hz and near 4.5 kHz, 13 dB down at 8.9 kHz, 22 dB down at 12.3 kHz [measured: `brigade_fr.tsv`]. Dry path: flat, 3 dB down near 45 Hz [measured: `brigade_fr.tsv`].
- Gain on both outputs is constant to 0.01 dB from minus 40 to 0 dBFS input, and silence in gives digital silence out, so at the default Level there is no audible compander, saturation or noise [measured: `brigade_level.tsv`].

### 3.7 UADx Studio D Chorus (Dimension D model)

- Not measured. `auhost render` blocked in a modal alert raised by the plugin (`juce::OSXMessageBox` in a `sample` of the hung process) and the process was killed [inferred, no evidence file]. Dimension numbers below come from literature and from Choral's Dimension mode.

## 4. Measurements

Method. The main probe is an impulse train (one click every 50 ms or 10 ms). Each click shows every delay tap of every voice, so delay against time, pitch offset (`1200*log2(1 - d(delay)/dt)`), LFO rate, shape and left/right phase come from one take. The tool is `cx.py` (commands `gen clicks`, `trk`, `lr`, `tapfr`, `width`, `spec`); `calc.py` holds the closed-form numbers. Every evidence file lists its commands in its header.

| what | result | file |
| --- | --- | --- |
| Choral voice delays per mode | base 3.09 / 7.0 / 11.04 ms; swing 8.6 to 9.8 ms at 0.25 Hz, Amount 50 % | `choral_modes.tsv` |
| Choral pitch against rate | Amount 50 %: 7.9, 8.4, 8.4 cents at 0.25, 1, 3 Hz | `choral_rate_amount.tsv` |
| Choral wet response | Synth: 3.0 dB down at 5 kHz; Universal: 0.6 dB spread at 10 kHz, 2.6 dB at 15 kHz | `choral_wet_fr.tsv` |
| Choral width, band noise | Width 100 %: correlation 0.88 to 0.92, mono sum 0.2 dB under one channel | `choral_width.tsv` |
| Brigade chorus sweep | 1.70 to 4.38 ms, triangle, 0.43 to 2.86 Hz, 4.5 to 29 cents | `brigade_chorus.tsv` |
| Brigade vibrato | sine, 3.4 to 12.4 Hz, 4 to 75 cents | `brigade_chorus.tsv` |
| Brigade stereo | Classic: dry left, wet right. Dual: quadrature | `brigade_taps.txt`, `brigade_chorus.tsv` |
| Brigade wet and dry response | wet band 130 Hz to 4.5 kHz | `brigade_fr.tsv` |
| Brigade level dependence | none from minus 40 to 0 dBFS; no idle noise | `brigade_level.tsv` |
| GR6 Ensemble sweep | 2.6 ms fixed swing, 1.55 Hz at Intensity 0.5, 3.01 Hz at 1, 14 and 28 cents | `gr6_ensemble.tsv` |
| Valhalla wet tap flatness | median within 0.4 dB, 1 to 15 kHz | `valhalla_flair.tsv` |
| Interpolator loss, closed form | linear 2.0 dB at 10 kHz; Hermite 0.53 dB | `interp_loss.tsv` |
| tubamp today, closed form | 2 to 844 cents across the knob ranges | `tubamp_today.tsv` |

The Hermite prediction (0.53 dB at 10 kHz, 2.53 dB at 15 kHz) matches the Choral Universal measurement (0.59 dB and 2.58 dB) [measured: `interp_loss.tsv`, `choral_wet_fr.tsv`].

Three independent CE-1 models agree on the design: fixed sweep near 2.6 ms, triangle, intensity mapped to rate up to about 3 Hz [measured: `brigade_chorus.tsv`, `gr6_ensemble.tsv`] [decompiled: AmpliTube 5 `AnalogChorus::OnSetParameter` 0x152a400].

## 5. Industry practice

What the references agree on

- Chorus uses a triangle LFO, so the pitch offset is a constant plus or minus value that flips, not a continuous sweep [measured: `brigade_chorus.tsv`, `gr6_ensemble.tsv`] [decompiled: Choral `lfo_shaper::triangle_shaper` 0x88e80]. Vibrato uses a sine [measured: `brigade_chorus.tsv`].
- Triangle corners are rounded by a low-pass on the delay-time signal [decompiled: Choral `ChorusVoice<22050>::process` 0x87530; Guitar Rig 6 `FlangeMistress::processBuffer` 0x280fc4].
- Pitch deviation stays small: 4.5 to 29 cents for the CE-1 model, 0.3 to 30 cents for Choral, 5 to 8 cents for the Dimension D [measured: `brigade_chorus.tsv`] [decompiled: Choral lambda `$_1` 0x898f4] [literature: http://www.florian-anwander.de/roland_string_choruses/].
- Depth is tied to rate. Choral divides the swing by the rate; the CE-1 models fix the swing and let the rate set the detune; the MXR and CE-1 vibrato shrink the swing as the rate rises [decompiled: Choral `Choral::handleParameterRate` 0x884ec; AmpliTube 5 `MXRStereoChorus::Module::SetSampleRate` 0xf8d228] [measured: `brigade_chorus.tsv`].
- Stereo is made with a second voice, 180 degrees (Choral, AmpliTube stereo chorus, Roland) or 90 degrees (UADx Dual, Guitar Rig Ensemble, Valhalla) from the first [measured: `choral_modes.tsv`, `brigade_chorus.tsv`, `gr6_ensemble.tsv`] [decompiled: AmpliTube 5 `ChorusStereo::DefineParameters` 0x10ddf20].
- The wet path is band-limited, between 4 and 12 kHz, and often high-passed at 50 to 150 Hz [measured: `brigade_fr.tsv`, `choral_wet_fr.tsv`] [decompiled: Choral `setModeSmoothingTargetValues` 0x87a70].
- Chorus has no feedback by default [decompiled: AmpliTube 5 `static_at5.md` table; Guitar Rig 6 `RigEnsemble::processBuffer` 0x2c4418].
- Dry and wet meet at equal gain for the full effect [decompiled: Choral `Choral::processAudioInternal` 0x863cc; AmpliTube 5 `ChorusCE1::Stereo::Process` 0xdf93a0].
- Delay time is computed per sample in every reference read [decompiled: Choral `ChorusVoice<22050>::process` 0x87554; Guitar Rig 6 `RigEnsemble::processBuffer` 0x2c4418; AmpliTube 5 `ChorusCE1::Module::Process` 0xdf9004].
- The quality interpolators are cubic (NI Choral, TC model) or allpass (Valhalla); the simpler models use linear [decompiled: Choral 0x8755c; Guitar Rig 6 `ChorusFlangerTCSCF::processAudio` 0x66eb34; ValhallaSpaceModulator `ProcessBlockUp` 0x3c5c0; AmpliTube 5 `DelayLineVariable` 0x10e2330].
- BBD behaviour is reduced to two things: a nonlinear control-to-delay curve and wet band-limiting. No readable reference runs a clocked BBD, a compander or noise in a chorus [decompiled: AmpliTube 5 `ChorusCE1::gcV2TCurve` 0x30c6a2c; Choral 0x87508; Guitar Rig 6 `FlangeMistress::setParamDepth` 0x280aa8].

What the literature says

- BBD delay is `stages / (2 * f_clock)` [literature: http://colinraffel.com/publications/dafx2010practical.pdf].
- Chorus and flanger lines of 1024 stages or fewer usually ignore the compander and the nonlinearity; BBD distortion is about 1 % per 1024 stages [literature: http://colinraffel.com/publications/dafx2010practical.pdf].
- The CE-1 uses a 512-stage MN3002 with a clock period of 5 to 16 microseconds, which gives 1.28 to 4.10 ms [literature: https://www.tonehome.de/app/download/8747046/boss_ce1_chorus.pdf]. The UADx sweep of 1.70 to 4.38 ms is close to that [measured: `brigade_chorus.tsv`].
- Linear modulation of the clock period gives a clean triangular delay; linear modulation of the clock frequency bends it [literature: https://electricdruid.net/weird-wiggles-bbd-clock-modulation/].
- Dimension D: two MN3007 lines, one LFO, inverted modulation on the second line, 80 Hz input high-pass; modes are 10 ms plus or minus 2.0 ms at 0.25 Hz, 7.5 plus or minus 2.5 ms at 0.25 Hz, 7.5 plus or minus 1.5 ms at 0.5 Hz, and the last again with a 10 dB same-side pad [literature: http://www.florian-anwander.de/roland_string_choruses/].
- Juno-60 chorus rates are 0.5, 0.8 and 1 Hz; the Roland RS-202 ensemble pairs 0.66 Hz with 6.25 Hz [literature: http://www.florian-anwander.de/roland_string_choruses/].
- Juno-60 BBD filters are a first-order high-pass plus a fifth-order low-pass on each side, and the minimum clock is about 26 kHz [literature: https://www.dafx.de/paper-archive/2018/papers/DAFx2018_paper_12.pdf].
- Dattorro's standard chorus is blend 1.0, feedforward 0.7071, feedback 0; he recommends quadrature LFOs for stereo and warns that linear interpolation is an audible time-varying low-pass with more than two voices [literature: https://ccrma.stanford.edu/%7Edattorro/EffectDesignPart2.pdf].
- Dattorro's pitch ratio is `1 - width * Omega * T * cos(Omega n T)`, the relation used in section 2 [literature: https://ccrma.stanford.edu/%7Edattorro/EffectDesignPart2.pdf].
- Allpass interpolation has no gain error but its transient error limits it to about one semitone of pitch change [literature: https://ccrma.stanford.edu/%7Edattorro/EffectDesignPart2.pdf].

## 6. Gap analysis

| aspect | ours | references | consequence for the player |
| --- | --- | --- | --- |
| Stereo | same delay on both channels [inferred from `juce_dsp/widgets/juce_Chorus.h:117`] | second voice at 90 or 180 degrees [measured: `brigade_chorus.tsv`, `choral_modes.tsv`] | Ours stays as narrow as the input. References spread the guitar across the speakers, which is most of what players call a good chorus. |
| Pitch wobble | 43 cents at defaults, up to hundreds [measured: `tubamp_today.tsv`] | 5 to 30 cents [measured: `brigade_chorus.tsv`, `choral_rate_amount.tsv`] | Ours sounds seasick as soon as rate or depth is raised. The usable area of the two knobs is small. |
| Depth law | swing in ms, independent of rate [inferred from `juce_dsp/widgets/juce_Chorus.h:124`] | swing tied to rate [decompiled: Choral 0x884ec] | Turning our rate up also turns the detune up. References let the player change speed without retuning depth. |
| LFO shape | sine [inferred from `juce_dsp/widgets/juce_Chorus.cpp:42`] | triangle for chorus [measured: `brigade_chorus.tsv`] | Sine sweeps the pitch continuously (vibrato feel). Triangle holds a steady detune, the classic doubled sound. |
| Voices | 1 | 2 to 6 [decompiled: Choral 0x86068; AmpliTube 5 0xe2da68] | One voice gives a moving comb. Several voices blur the comb into thickness. |
| Wet tone | full band [inferred from `juce_dsp/widgets/juce_Chorus.h:128`] | low-pass 4 to 12 kHz, high-pass 50 to 150 Hz [measured: `brigade_fr.tsv`] | Ours adds comb swirl in the pick attack and the fizz of a driven amp. A darker wet sits behind the dry note. |
| Interpolation | linear, 2 dB flutter at 10 kHz [measured: `interp_loss.tsv`] | Hermite or allpass [decompiled: Choral 0x8755c] | Small: a faint treble flutter on clean tones. It matters more when the wet is the only signal (vibrato). |
| Mix law | linear crossfade, default wet 5.4 dB under dry [inferred from `juce_dsp/widgets/juce_Chorus.cpp:45`] | dry fixed, wet up to equal [decompiled: Choral 0x863cc] | Our knob lowers the dry level as chorus is added. References keep the note level and add the effect. |
| Depth above 0.6 | delay clips at 1 ms [measured: `tubamp_today.tsv`] | no clipping in range [measured: `choral_rate_amount.tsv`] | The top of our depth knob produces a kinked sweep with a flange-like glitch. |
| Voicings | one | CE-1, Dimension, tri-chorus, modern [measured and decompiled in section 3] | Players ask for these by name. |
| Mode switch | not applicable | fade out, switch, fade in, or morph [decompiled: Guitar Rig 6 0x66e5ec; Choral 0x86a08] | Needed once we have voicings. |

## 7. Recommendations

All constants below are ours: derived from published circuit values, from the literature, or fitted to the measurements in section 4. None is copied from a vendor table.

Shared design (used by R1 to R4)

```
voice:
  x_f   = HP1(fc_hp) -> LP2_butterworth(fc_lp * tone)         // wet band-limit, TPT SVF
  write x_f into the delay line
  u     = lfo_shape(phase + phase_offset)                     // unipolar 0..1
  u_s   = onepole(u, fc = max(8 Hz, 20 * rate))               // corner rounding, per sample
  d     = d0 + D * u_s                                        // seconds, per sample
  y     = hermite4(delay line, d * fs)                        // 4-point Catmull-Rom

triangle: u = 1 - |2*frac(p) - 1|          peak slope 2*f*D
sine:     u = 0.5 - 0.5*cos(2*pi*p)        peak slope pi*f*D

depth in cents c:
  triangle: D = (2^(c/1200) - 1) / (2 * f)
  sine:     D = (2^(c/1200) - 1) / (pi * f)
  D = min(D, D_max of the voicing)

mix m (0..1):  dry = min(1, 2 - 2m),  wet = min(1, 2m)       // equal gains at 0.5, vibrato at 1
out_L = dry * in_L + wet * sum(pan_L[k] * y[k]),  same for R
```

- The cents law follows from the pitch ratio `1 - dd/dt` [literature: https://ccrma.stanford.edu/%7Edattorro/EffectDesignPart2.pdf] and is the law Choral uses [decompiled: Choral `Choral::handleParameterRate` 0x885e0].
- The mix law keeps dry at unity up to the midpoint, as the references do [decompiled: Choral `Choral::processAudioInternal` 0x863cc] [literature: https://ccrma.stanford.edu/%7Edattorro/EffectDesignPart2.pdf].
- Hermite costs 0.53 dB at 10 kHz against 2.0 dB for linear [measured: `interp_loss.tsv`].
- A voicing change fades the wet out over 20 ms, swaps constants, clears the lines and fades in [decompiled: Guitar Rig 6 `ChorusFlangerTCSCF::applyTargetMode` 0x66e5ec].
- Rate, depth, tone and width get per-sample smoothing (20 ms); the delay time is never stepped [decompiled: Choral `processParameterValueSmoother` 0x86738].

New parameters (append-only; instances 2 and 3 use the `mod2_` and `mod3_` prefixes like the existing ids)

| id | type | range | default | notes |
| --- | --- | --- | --- | --- |
| `mod_chorus_mode` | choice | Classic, Wide, CE, Dimension, Tri | 0 (Classic) | Order frozen once shipped. Only read when `mod_type` is Chorus. |
| `mod_width` | float | 0 to 1 | 1.0 | 0 collapses the voices to the centre. Ignored by Classic. |
| `mod_tone` | float | 0 to 1 | 0.5 | Wet low-pass = voicing cutoff times `2^(2*(tone-0.5))`. Ignored by Classic. |

What the shared rate/depth/mix cannot express today: the voicing (voice count, LFO shape, delay range), the stereo spread and the wet tone [inferred]. Depth keeps its id and range but means cents (Wide, Tri) or sweep fraction (CE, Dimension) in the new modes [inferred]. A feedback parameter is not proposed: no reference chorus uses feedback by default [decompiled: AmpliTube 5 `static_at5.md` table]. A single sub-mode id shared with phaser and tremolo is not advised, because each type needs its own frozen choice list [inferred].

Compatibility for every item: `mod_chorus_mode` defaults to 0 and mode 0 calls the existing `juce::dsp::Chorus` path unchanged, so a session saved before the change loads the default and renders bit-identical [inferred]. No existing id, range or default changes.

Cost for every item: about 40 floating-point operations per voice per sample (filter, LFO, Hermite) [inferred]. Four voices in stereo should stay far below 1 % of one core at 48 kHz [guess, to be profiled]. Latency is 0 samples. Each delay line needs 25 ms at the maximum sample rate.

### R1. Wide: stereo two-pair chorus with a cents depth law (highest benefit)

- Build: four voices in two pairs. Pair 1: `d0 = 6 ms`. Pair 2: `d0 = 10 ms`, LFO rate times `sqrt(2)`, gain 0.7. Within a pair the right voice LFO is 90 degrees from the left. Triangle LFO. Wet filter: high-pass 60 Hz, low-pass 9 kHz. `D_max = 8 ms`. Depth knob `x` gives `c = 30 * x^2` cents.
- Evidence: delay region 1 to 30 ms with 5 ms nominal, and quadrature stereo [literature: https://ccrma.stanford.edu/%7Edattorro/EffectDesignPart2.pdf]; quadrature in two shipping models [measured: `brigade_chorus.tsv`, `gr6_ensemble.tsv`]; unequal rates per pair [decompiled: Choral `setModeSmoothingTargetValues` 0x87a70, structure only; our ratio is `sqrt(2)`]; 30 cents ceiling with a square taper [decompiled: Choral lambda `$_1` 0x898f4] [measured: `brigade_chorus.tsv`, 29 cents at the CE-1 maximum]; low-pass inside the 4 to 12 kHz range the references use [measured: `choral_wet_fr.tsv`].
- Player benefit: real width, a steady detune that does not change with speed, thicker than one voice.
- Proof: with `cx.py trk`, pitch (95th percentile) at depth 0.5 is 7.5 cents plus or minus 1.5 at 0.25, 1 and 3 Hz. With `cx.py lr`, right minus left phase is 90 plus or minus 10 degrees. With `cx.py width` on the band noise, correlation is below 0.93 at width 1 and the mono sum is within 1 dB of one channel.

### R2. CE: CE-1 style chorus

- Build: one delay line per channel. Triangle. Sweep 1.6 ms to 4.3 ms at depth 1 (centre 2.95 ms, swing 2.7 ms times depth). No cents law: the rate sets the detune, as on the pedal. Right voice LFO at 90 degrees times width. Wet filter: first-order high-pass at 130 Hz; 2-pole low-pass at 4.6 kHz plus one pole at 9 kHz. Shape curve `u' = u * (1 + 0.25 * (u - 1))` bends the triangle slightly toward the short-delay end.
- Evidence: 512 stages at a 5 to 16 microsecond clock period gives 1.28 to 4.10 ms [literature: https://www.tonehome.de/app/download/8747046/boss_ce1_chorus.pdf]; measured model sweep 1.70 to 4.38 ms, rate 0.43 to 2.86 Hz, fixed swing [measured: `brigade_chorus.tsv`]; two more CE-1 models fix the swing and map intensity to rate [measured: `gr6_ensemble.tsv`] [decompiled: AmpliTube 5 `AnalogChorus::OnSetParameter` 0x152a400]; wet band fitted to the measured response [measured: `brigade_fr.tsv`]; curved control-to-delay law [decompiled: AmpliTube 5 `ChorusCE1::gcV2TCurve` 0x30c6a2c; Choral 0x87508]; quadrature dual mode [measured: `brigade_chorus.tsv`].
- Player benefit: the dark, mid-forward shimmer of the classic pedal; slow settings are subtle, fast settings reach about 29 cents.
- Proof: `cx.py trk` at depth 1 shows a 1.6 to 4.3 ms sweep within 0.15 ms, delay crest 1.6 to 1.8, 4 to 6 cents at 0.43 Hz and 25 to 31 cents at 2.86 Hz. `cx.py spec` of the wet path is within 2 dB of `brigade_fr.tsv` from 100 Hz to 8 kHz after level alignment.

### R3. Dimension: Dimension D style widener

- Build: two delay lines fed from the mono sum, one triangle LFO, inverted on the second line. `d0 = 8.5 ms`, swing plus or minus `2.5 ms * depth`. Left = dry + line A, right = dry + line B. Wet filter: high-pass 80 Hz, low-pass 10 kHz. Suggested preset points: rate 0.25 Hz with depth 0.8, and rate 0.5 Hz with depth 0.6.
- Evidence: delay 7.5 to 10 ms, swing 1.5 to 2.5 ms, rates 0.25 and 0.5 Hz, one LFO inverted on the second line, 80 Hz high-pass [literature: http://www.florian-anwander.de/roland_string_choruses/]; antiphase pair measured in Choral's Dimension mode [measured: `choral_modes.tsv`]. The low-pass value is not sourced [guess].
- Player benefit: width with almost no audible wobble (5 to 8 cents), the "always on" studio chorus.
- Proof: `cx.py lr` shows 180 plus or minus 10 degrees; `cx.py trk` shows 5 to 9 cents at the two preset points; mono sum within 1 dB of one channel.

### R4. Tri: three-phase ensemble

- Build: three delay lines, `d0 = 7 ms`. Slow sine LFO at the rate knob with phases 0, 120, 240 degrees and depth from the cents law (`c = 20 * x^2`). Fast sine LFO fixed at 6 Hz, same three phases, fixed 3 cents. Left = voice 1 + voice 2, right = voice 2 + voice 3, each at 0.7. Wet low-pass 6 kHz, high-pass 80 Hz.
- Evidence: three lines at 120 degrees and the shared centre voice [decompiled: AmpliTube 5 `FullToneTERC::Module::Process` 0xe2da68]; three-phase voice pairs with a second faster sine [decompiled: Choral `setModeSmoothingTargetValues` 0x87a70, Ensemble block 0x87bf4]; slow plus fast pair of 0.66 and 6.25 Hz [literature: http://www.florian-anwander.de/roland_string_choruses/].
- Player benefit: the dense 1980s rack chorus; the three phases average out most of the audible pitch movement.
- Proof: `cx.py taps` shows three wet taps; the slow components are 120 plus or minus 15 degrees apart; a 6 Hz component is present at 2 to 4 cents.

### R5. Tone and width controls

- Build: `mod_tone` and `mod_width` as in the table. Width 0 gives both channels the mean of the left and right wet.
- Evidence: Choral exposes Width and per-mode wet cutoffs [decompiled: Choral `handleParameterWidth` 0x88670]; AmpliTube's MXR model exposes wet bass and treble [decompiled: AmpliTube 5 `MXRStereoChorus::Module::SetSampleRate` 0xf8d26c].
- Proof: `cx.py width` correlation is 1.000 at width 0; `cx.py spec` shows the wet 3 dB point moving one octave each way across the tone range.

### R6. Click-free control

- Build: the fade on mode change and the per-sample smoothing described in the shared design.
- Evidence: fade-switch-fade and per-sample ramps in the references [decompiled: Guitar Rig 6 `ChorusFlangerTCSCF::applyTargetMode` 0x66e5ec; Choral `processParameterValueSmoother` 0x86738].
- Proof: switch `mod_chorus_mode` and step rate and depth during a 220 Hz sine at minus 12 dBFS; after a 2 kHz high-pass the output never exceeds minus 60 dBFS.

Not recommended

- A clocked BBD model with compander and noise. The readable references do not build one for chorus [decompiled: AmpliTube 5 `ChorusCE1::Module::Process` 0xdf9004; Guitar Rig 6 `RigEnsemble::processBuffer` 0x2c4418], the protected CE-1 model shows no level dependence or noise [measured: `brigade_level.tsv`], and the literature says short lines usually skip it [literature: http://colinraffel.com/publications/dafx2010practical.pdf].
- Allpass interpolation. It is flat [measured: `valhalla_flair.tsv`] but limited to about a semitone and needs care at fraction 0 [literature: https://ccrma.stanford.edu/%7Edattorro/EffectDesignPart2.pdf]; Hermite is simpler and close enough under a wet low-pass.

## 8. Open questions and unverified items

- UADx Studio D Chorus could not be rendered (modal alert). The Dimension voicing rests on one literature source and on Choral's Dimension mode. The same-side pad of mode 4 and any inverted cross-feed between channels are not confirmed and are left out of R3.
- Boss CE-2 numbers were not found (the ElectroSmash page did not resolve). No CE-2 voicing is proposed.
- The UADx Brigade was measured at the default Level only. Preamp drive at high Level settings was not measured.
- The CE-1 dry-to-wet level ratio for a mono mix is not established; R2 uses equal gain.
- Choral: the LFO smoothing coefficient (0.2 to 0.99 per sample) was not checked for sample-rate scaling, and the phase-glide fields at voice offsets 0xc0 to 0xd0 were not decoded.
- ValhallaSpaceModulator: delay ranges and LFO phases were not measured reliably because several moving taps overlap; only the tap flatness is reported. Its mode order is inferred from help strings.
- AmpliTube 5 is static only. Fulltone tri-chorus LFO depths, the two fixed LFO rates and the filter coefficients were not read.
- Helper-read constants re-read by the orchestrator: AmpliTube `gcV2TCurve` values and the CE-1 rate and vibrato constants; Guitar Rig `RigEnsemble` constants 2.9, 0.1, 0.0026 and 1.6223. The `RigEnsemble` reading is also confirmed by measurement. Other helper claims (Hammond, FlangeMistress, StereoTouch, TC SCF, Valhalla internals, Flair) were not re-read; Valhalla's allpass claim is supported by measurement.
- No listening test was done. The acceptance thresholds are engineering targets.
- CPU cost is an estimate, not a profile.
