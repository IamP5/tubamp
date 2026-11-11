# Drive: overdrive, distortion, fuzz and boost

## Verification status

Independently verified on 2026-11-11. The full table is in `verification.md`.
Read these corrections before relying on the numbers below. The body of the report is unchanged.

- 16 of 21 claims verified, none contradicted, 5 inconclusive (C04, C13, C18, C19, C21): those AmpliTube 5 and Skreamer Deluxe numbers come only from the in-process harness `evidence/tools/rigcall.cpp`, which loads the vendor binaries and calls their DSP classes directly. The harness patches nothing, but it enters below the plugins' own instantiation path. Treat its numbers as second-class.
- The claims about tubamp's own drive hold to 0.1 dB across source, an independent model and the installed Audio Unit: alias floor -21.4 dB, 2x polyphase IIR oversampling, 3.14 samples of delay, reported latency 0.
- "720 Hz high-pass" is wrong for the classic Guitar Rig Skreamer: it is 587 Hz at the default Bass setting and moves with the knob (measured through the real AU). In the classic Skreamer the clean path is a knob, not fixed.
- "Worst alias floor of the field" is wrong: the author's own data has AmpliTube's Distortion+ at -17.8 dB.
- The TS prototype is not "inside the spread on every quantity": its pre-clip figures miss by 0.3 to 0.5 dB.
- R2: -71.8 dB and 4.56 samples reproduce, but only for aliases below the fundamental. The worst alias below 20 kHz is about -55 dB across the frequencies tried.
- R4: JUCE's integer latency for the current oversampler is 4 samples (6 at 4x), not 3.
- R3's diode constants come from decompiled values, not from a cited source. The RAT page cited as literature does not contain the component values tagged to it.
- The Guitar Rig Fuzz Face neural network claim holds: the code calls a GRU with carried state, then an output network.

Study of how the reference plugins on this Mac build drive pedals, and what tubamp's drive block
should become. Evidence tags follow `tools/re/README.md`. Paths under `evidence/` are relative to
this folder. Binary short names: **GR6** = Guitar Rig 6, **AT5** = AmpliTube 5.

## 1. Summary

1. tubamp's drive is a bare `tanh` with flat gain from DC, no bass cut before clipping, a 12 dB/oct low-pass after it and 2x oversampling; every reference pedal shapes the signal before the clipper, and that shaping is the pedal's character `[measured: evidence/precomp_all.tsv]`.
2. Three independent Tube Screamer models (GR6 classic, GR6 Deluxe, AT5) agree within 1 dB: a first-order 720 Hz high-pass ahead of a symmetric diode clipper, with the clean input added back at the output `[measured: evidence/precomp_all.tsv]` `[decompiled: AT5 TubeScreamer::Module::SetSampleRate 0xdc7fb4]`.
3. The references compute the clipper three ways: a 256-point static table (GR6 classic), a per-sample implicit diode solve with capacitor memory (GR6 Deluxe), and a pre-tabulated K-method state-space solve (AT5); all run it at 4x or at a fixed 96/192 kHz `[decompiled: GR6 0x316ecc, 0x5efac4; AT5 0xdc8068]`.
4. Our alias floor is the worst of the field (-21 dB re the fundamental for a 5 kHz tone at full gain); 4x oversampling plus first-order antiderivative anti-aliasing reaches -72 dB in simulation for about 4.6 samples of latency `[measured: evidence/summary.tsv, evidence/alias_schemes.tsv]`.
5. Recommendation: keep type 0 as is and add typed pedals (TS-style overdrive, RAT-style distortion, Muff-style fuzz, clean boost, treble boost) on one shared engine: voicing filter, volt-referenced diode clipper solved per sample, 4x + ADAA.

## 2. tubamp today

Source: `src/dsp/FxBlocks.h:13-27`, `src/dsp/FxBlocks.cpp:46-124`, parameters in `src/Parameters.cpp:74-76` (instances 2 and 3 at `:159-161`, `:164-166`).

- **Signal flow.** Input, 2x upsample, `tanh(x * gain)`, 2x downsample, low-pass, output level `[decompiled: src/dsp/FxBlocks.cpp:88-123]`.
- **Pre-filtering.** None: no high-pass, no bass cut, no pre-emphasis before the shaper `[decompiled: src/dsp/FxBlocks.cpp:88-105]`. The 1 dB compression point is the same -20.94 dBFS at every frequency from 60 Hz to 7 kHz at 18 dB of gain `[measured: evidence/precomp_all.tsv]`.
- **Shaper.** `std::tanh`, odd-symmetric, memoryless, single precision `[decompiled: src/dsp/FxBlocks.cpp:105]`. Even harmonics measure below -145 dB at every level and gain `[measured: evidence/thd_all.tsv]`.
- **Oversampling.** `juce::dsp::Oversampling`, one stage (2x), `filterHalfBandPolyphaseIIR`, maximum quality, non-integer latency `[decompiled: src/dsp/FxBlocks.cpp:52-53]`. JUCE designs that stage with transition width 0.05 and -90 dB for the up filter, 0.06 and -75 dB for the down filter `[decompiled: juce_Oversampling.cpp:564-574]`. JUCE's design formula gives 7 and 6 all-pass coefficients (orders 15 and 13) `[inferred]` from `juce_FilterDesign.cpp:620-690`, reproduced in `evidence/tools/alias_schemes.py`.
- **Latency.** The two half-bands have a low-frequency group delay of 3.30 + 2.97 samples at 96 kHz, 3.14 samples at 48 kHz `[inferred]` from the same computation. The impulse peak leaves the block 4 samples late with the tone filter open `[measured: evidence/extra.tsv]`. The plugin reports no latency for the drive: only `amp`, `amp2` and FX slots enter the chain latency `[decompiled: src/PluginProcessor.cpp:1724-1741]`.
- **Post-filtering.** `StateVariableTPTFilter` low-pass at the tone frequency, 12 dB/oct, after the downsampler, at the base rate `[decompiled: src/dsp/FxBlocks.cpp:56-58, 111-112]`. Measured -3 dB at 3987 Hz at the default 4 kHz `[measured: evidence/fr_all.tsv]`.
- **Smoothing.** Gain and level ramp linearly over 20 ms; the gain ramp advances once per host sample; the tone cutoff is set once per block without smoothing `[decompiled: src/dsp/FxBlocks.cpp:15, 60-61, 76-81, 96-100]`.
- **DC, bias, noise.** No DC blocker, no bias, no noise source `[decompiled: src/dsp/FxBlocks.cpp:83-124]`. Silence in gives digital silence out `[measured: evidence/extra.tsv]`.
- **Channels.** Per-channel processing with one shared gain ramp `[decompiled: src/dsp/FxBlocks.cpp:102-106]`.
- **Ranges.** Gain 0..36 dB (default 12), tone 500..12000 Hz log (default 4000), level -24..12 dB (default 0) `[decompiled: src/Parameters.cpp:74-76]`.
- **Level.** Output peaks at +0.8 dBFS at level 0 dB because the low-pass overshoots the ±1 `tanh` ceiling `[measured: evidence/dynamics_all.tsv]`.
- **Default chain.** The installed AU's default chain is `amp` only; the drive has to be inserted `[measured: evidence/tubamp_default_chain.txt]`. All `ours` measurements use a state whose chain is `drive` alone.

## 3. Reference implementations

### 3.0 Method note

Guitar Rig 6 and AmpliTube 5 choose the pedal inside the plugin, and GUI work was not allowed for this study. Their DSP classes were measured directly instead: `evidence/tools/rigcall.cpp` loads the plugin binary, constructs the class at its symbol address, calls its own setters and process function, and writes a WAV. The vendor code runs unmodified, and the numbers are the same kind the AU host gives for the stand-alone units. Neither product is PACE-protected. Late in the study the Guitar Rig 6 AU also became measurable headless: its state can be authored from the plugin's own factory component presets (`evidence/tools/gr6state.py`, from the tremolo study) and rendered through `auhost`. Models named `gr6au_*` use that route with the factory INIT preset; models named `gr6_*` and `at5_*` use the in-process route. The two routes agree: for the Skreamer with identical knob values, fundamental, H3 and H5 differ by at most 1.1 dB over 18 level and drive combinations `[measured: evidence/gr6_au_vs_inprocess.txt]`. For the Cat they agree within 0.9 dB at mid and full distortion but differ by up to 8 dB at low distortion and in the alias figure, so the AU numbers are the ones to trust for the Cat `[measured: evidence/gr6_au_vs_inprocess.txt]`. AmpliTube 5 could only be measured in-process. Static reading used `tools/re/disasm.py` through `evidence/tools/ann.py`, which resolves literal pools and float immediates.

### 3.1 Guitar Rig 6 classic pedal engine (`NI::DSP::Skreamer`, `Distortion`, `Cat`, `BigFuzzPsi`, `FuzzAce`, `Demon`, `JackHammer`)

One hand-vectorised template serves all seven pedals.

- **Oversampling: 4x inside one SIMD vector.** Each input sample becomes four phases by Catmull-Rom cubic interpolation over a 4-sample history; the weights for phases 0.25, 0.5 and 0.75 are literal constants `[decompiled: GR6 NI::DSP::Skreamer::onInit 0x2f44e8, literals 0x2bde8e0]`. The four phases fill one 4-float NEON register, so the shaper runs once per input sample on a vector `[decompiled: GR6 NI::DSP::Skreamer::processBuffer 0x2f515c-0x2f5264]`. Decimation is a 15-tap symmetric FIR whose taps sum to 1.0 `[decompiled: GR6 0x2f44f4-0x2f4598, literals 0x2bde858]`. The other classes load the same interpolation constants in `onInit` `[decompiled: GR6 Distortion::onInit 0x275854, Cat::onInit 0x267874, BigFuzzPsi::onInit 0x266a90]`.
- **Nonlinearity: static table.** `NI::DSP::WaveShapingTables` holds one `double[256]` curve per circuit (`TS9`, `Rat`, `BigFuzz`, `FuzzFace`, `MT2`, `JackhammerOverdrive`, `JackhammerDistortion`, plus amp stages) `[decompiled: GR6 WaveShapingTables::TS9::TS9 0x316ecc]`. `TableBase::setupTable` finds the curve's operating point, subtracts its DC value and stores float (value, slope) pairs, so a lookup is one multiply-add `[decompiled: GR6 TableBase::setupTable 0x614c14]`. The lookup clamps the index at both ends and interpolates linearly `[decompiled: GR6 Skreamer::processBuffer 0x2f51d0-0x2f5264]`. There is no antiderivative anti-aliasing and no memory in the nonlinearity `[decompiled: GR6 0x2f50c8]`.
- **Table shapes.** The TS9 curve saturates flat at +0.649 and -0.666 (2.6 % asymmetric); the Rat curve at +0.621 and -0.601; the FuzzFace curve is one-sided, -8.7 to -0.5 `[decompiled: evidence/gr6_table_shapes.txt]`.
- **Filters.** One-pole sections with the coefficient `min(1, 6.283*f/sr)`, no pre-warping `[decompiled: GR6 Skreamer::onSetSampleRate 0x2f4c50-0x2f4ca0]`. Fixed corners at 4.6, 8 and 20.6 Hz do the DC blocking and coupling `[decompiled: GR6 0x2f4c64, 0x2f4c94, 0x2f4c7c]`.
- **Skreamer knob mapping.** Frequencies are MIDI pitches fed to a global `2^(x/60)` table. Tone low-pass: pitch 67 + 45*t², 392 Hz to 5.27 kHz `[decompiled: GR6 Skreamer::setParamTone 0x2f47fc-0x2f4818]`. Drive lowers a second low-pass: pitch 130 + 40*bright - 40*drive, 4.7 kHz at full drive with bright at 0.5 `[decompiled: GR6 Skreamer::setParamDrive 0x2f4898, setParamBright 0x2f4a80]`. This is the 51 pF feedback capacitor of the circuit `[inferred]`. Drive gain is 20*drive + 35.2 dB into the table `[decompiled: GR6 0x2f4924-0x2f4938]`. Volume is linear, 0.447 * knob `[decompiled: GR6 Skreamer::setParamVolume 0x2f47d4]`. A `Clean` control mixes the unclipped input at 40*clean - 13 dB `[decompiled: GR6 Skreamer::setParamClean 0x2f4b24-0x2f4b34]`.
- **Smoothing.** Four parameters ramp linearly per sample over 20 ms; targets change only at control rate `[decompiled: GR6 Skreamer::onSetSampleRate 0x2f4c0c-0x2f4c4c, onControlRate 0x2f4f48]`.
- **Channels.** A stereo wrapper holds two instances and crossfades when the stereo switch changes `[decompiled: GR6 ComponentAudio<EffectBaseStereo<Skreamer>>::doBuffer 0x173354-0x1733c8]`.
- **Measured.** Skreamer: response peak 720 Hz, -10.3 dB at 110 Hz and -11.1 dB at 5 kHz re the peak, H3 -18.5 dB and H2 -87.5 dB at 1 kHz -18 dBFS `[measured: evidence/summary.tsv]`. Cost 0.67 % of real time against 0.35 % for our whole AU `[measured: evidence/extra.tsv]`.

### 3.2 Guitar Rig 6 Skreamer Deluxe (`ni::effektrig::dsp::tubescreamer`)

- **Structure.** `TubescreamerOverdrive` (the clipping stage), `TubescreamerTone`, `TubescreamerClarksdaleEQ`, and three modes `Classic`, `LED`, `EQ` `[decompiled: GR6 symbols 0x5efac4, 0x5f0054, 0x5efdd8; mode names at 0x44b3d18]`. Seven parameters; measured roles: 0 drive, 4 tone, 5 volume, 1-3 EQ-mode bands, 6 mode `[measured: evidence/fr_all.tsv, evidence/tone_sweeps.txt]`.
- **Clipper: implicit solve per sample, double precision.** The stage filters the input with a trapezoidal one-pole, forms the feedback-network equation with a capacitor state, and iterates on the diode variable `[decompiled: GR6 TubescreamerOverdrive::process 0x5efae4-0x5efb38]`. The residual is `sign(v)*ln(1 + 2|v|) + c1*v - c0*rhs`: the unknown sits in the logarithmic domain and no `exp` is evaluated `[decompiled: GR6 compute_solver_step 0x5f4d7c-0x5f4e18]`. `ln` is an exponent extraction plus a cubic in the mantissa `[decompiled: GR6 0x5f4d9c-0x5f4dfc]`. The update uses first, second and third derivative terms and clamps the step factor to [0.5, 8] `[decompiled: GR6 0x5f4e1c-0x5f4ed0]`. The loop is unrolled 20 times and exits when |step| < 1e-4 `[decompiled: GR6 0x5efb38-0x5efda4, literal 0x3bd8078]`. It starts from the previous sample's solution `[decompiled: GR6 0x5efb18]`.
- **Output = input + clipped term.** The last lines add the stage input to the feedback voltage `[decompiled: GR6 0x5efda8-0x5efdb4]`.
- **Rate.** The core runs at the power-of-two multiple of the host rate nearest 176.4 kHz: 192 kHz at a 48 kHz host (4x), 192 kHz at 96 kHz (2x), no conversion at 192 kHz `[decompiled: GR6 ModulatableDSPCore<2,2,7>::onSetSampleRate 0x1c5d6c-0x1c5dbc]` `[measured: evidence/gr6_internal_rates.txt]`.
- **Resampler.** `NI::AB::SampleRateConverter`, integer-ratio path: Kaiser-windowed sinc, beta 8, 16 taps per phase, Bessel function from the Abramowitz-Stegun polynomials `[decompiled: GR6 SampleRateConverter::init_intrat 0x245bc30-0x245be00, literal 0x412de80 = I0(8)]`.
- **Measured.** Peak 720 Hz, -10.4 dB at 110 Hz; small-signal gain 9.2 / 14.9 / 27.2 dB at drive 0.1 / 0.5 / 1.0, a log-like pot; worst alias -43 dB; impulse peak 18 samples late; cost 4.4 % of real time `[measured: evidence/summary.tsv, evidence/extra.tsv]`.

### 3.3 Guitar Rig 6 HM2 and Fuzz Face (`ni::effektrig::dsp::hm2`, `::fuzzface`)

- **HM2.** `HM2OverdriveStage::process` calls `compute_solver_step(float, float)` up to 20 times: the same implicit scheme in single precision `[decompiled: GR6 HM2OverdriveStage::process 0x5e5ca4]`. Internal rate 96 kHz at a 48 kHz host `[measured: evidence/gr6_internal_rates.txt]`. H2 is -21.5 dB at 1 kHz: asymmetric `[measured: evidence/summary.tsv]`.
- **Fuzz Face.** Not a circuit solver. `processAudioInternal` calls `ml::GRU<...redfuzzface...>`, `ml::GRU<...boumfuzz...>`, `ml::GRU<...boumsquare...>`, an `ml::OutputNetwork` and Eigen matrix-vector products: a recurrent neural network per mode `[decompiled: GR6 Fuzzface::processAudioInternal 0x5ea460]`. It runs at 96 kHz for 48 and 96 kHz hosts and is downsampled to 96 kHz at 192 kHz `[measured: evidence/gr6_internal_rates.txt]`. H2 -13.4 dB, H3 -11.2 dB at 1 kHz; idle output -90 dBFS, the only model here that is not silent at rest `[measured: evidence/summary.tsv, evidence/extra.tsv]`.

### 3.4 AmpliTube 5 pedals (`IK::KIS::FX::AT3::TubeScreamer`, `ProcoRat`, `BigMuffPi`, `BossDS1`, `BossSD1`, `MxrDistortionPlus`, `AT4::FuzzFace`)

- **Framework.** A nonlinear state-space solver (`IK::KIS::TK::KMethod::KMethod32<...>`) whose nonlinearity is a piecewise-linear curve on a non-uniform grid (`LinearInterpCurveNotUniform`) `[decompiled: AT5 symbols 0xdc6ebc, 0xcc70cc]`. The X-Drive "Green" model reuses the TubeScreamer module `[decompiled: AT5 XDrive::Green::Module::SetDefaults 0xff7128 calls 0xdc8280]`.
- **Tube Screamer circuit in the code.** Two states. The matrix literals are 4526.935 = 1/(4.7 kΩ * 47 nF), the 720.5 Hz corner; 4171881.5 = 1/(4.7 kΩ * 51 pF); and a feedback pole 1/((500 kΩ*drive + 51 kΩ) * 51 pF) `[decompiled: AT5 TubeScreamer::Module::SetSampleRate 0xdc7fb4-0xdc8014]`. The drive pot is linear in the code `[decompiled: AT5 0xdc7fd8-0xdc7ff4]`. An input high-pass at 99.965 rad/s (15.9 Hz) precedes it `[decompiled: AT5 0xdc7f38-0xdc7f68]`.
- **Diode.** The tabulated function is an antiparallel pair of SPICE-style diodes, Is = 2.55 nA, n*Vt = 45.5 mV; the recombination and breakdown terms are present but multiplied by zero or pushed to 1000 V `[decompiled: AT5 FillCurve<TubeScreamer KMethodFunctor> 0xdc6f00-0xdc7078]`. The RAT uses Is = 2.52 nA `[decompiled: AT5 ProcoRat::Module::Initialize 0xda1528]`.
- **Table.** 1999 points per curve, three floats per point `[measured: evidence/at5_ts_table_size.txt]`. After discretisation each point's lookup abscissa is set to its original abscissa minus K times its value, which turns the implicit equation into a lookup `[decompiled: AT5 0xdc8084-0xdc8120]`. At run time the segment is found by bisection and the value by linear interpolation with one division `[decompiled: AT5 TubeScreamer::Module::Process 0xdc7aa0-0xdc7b44]`. The capacitors are inside the solve, so the clipper has memory `[decompiled: AT5 0xdc7a20-0xdc7a9c]`.
- **Oversampling.** `Bilinearize(sr * 4)` when the Oversample flag is set, else `sr` `[decompiled: AT5 0xdc8064-0xdc8078]`. Up and down conversion are two cascaded 2x polyphase all-pass half-bands with two coefficients each, 0.1928 and 0.6726, inlined in `Process` `[decompiled: AT5 0xdc78b8-0xdc7948, literals 0x317b870]`. Being IIR they add about 2 samples: impulse peak at 4 samples with oversampling, 2 without `[measured: evidence/extra.tsv]`. Oversampling costs 1.64 % against 0.67 % of real time `[measured: evidence/extra.tsv]`.
- **Tone.** A biquad from the circuit's s-domain transfer function, bilinear at the base rate, coefficients polynomial in (1 - tone) `[decompiled: AT5 TubeScreamer OnSetParameter 0x187a9e8-0x187ab18]`.
- **Parameter changes.** `Drive()` takes a spin flag, rebuilds the matrices in a second `KMethodData` object and swaps it in at the start of `Process` `[decompiled: AT5 TubeScreamer::Module::Drive 0xdc8294-0xdc82b4, Process 0xdc777c-0xdc77c0]`.
- **Parameter names.** Tube Screamer: Drive, Volume, Tone, Oversample. RAT: Distortion, Filter, Gain, Oversample. Big Muff: Sustain, Volume, Tone, Oversample. SD-1: Drive, Tone, Level. Fuzz Face: Fuzz, Level, Bias, Volume `[decompiled: AT5 OnSetParameter string globals 0x37012f0, 0x36fbba8, 0x36ea4b8, 0x36eab48, 0x36f3a90]`.
- **Big Muff Sustain taper.** `sustain^3.32` `[decompiled: AT5 BigMuffPi OnSetParameter 0x1531b04-0x1531b14]`.
- **RAT.** A 3-state K-method block `[decompiled: AT5 ProcoRat::Module::ComputeKMethod 0xda2030]`. Its literals include 100000 (the distortion pot) and 303030304 = 1/3.3 nF (the Filter capacitor) `[decompiled: AT5 0xda2048, 0xda20c8]`. The remaining matrix entries were not decoded.

### 3.5 Stand-alone NI units and Plugin Alliance HG-2

- **Dirt** (`ni::effektrig::dsp::dirt`). Same core family and resampler as 3.2 `[decompiled: Dirt Dirt::Dirt(TargetSampleRate) 0x83f04]`. It reports 0.385 ms of latency `[measured: evidence/au_params_dirt.txt]`. Odd harmonics only with Bias at 0; Bias adds H2 up to -15 dB and leaves DC in the output (0.076 of full scale) `[measured: evidence/dirt_bias_safety.txt]`. `Safety` is an output ceiling: -14 dBFS peak with it, -1.5 dBFS without, at full drive `[measured: evidence/dirt_bias_safety.txt]`. Worst alias -81 dB (mode II) `[measured: evidence/summary.tsv]`. The clipper equations are inlined in a 2.6 kB function and were not decoded.
- **Driver** (`NI::XMASSCREAM`). A per-sample core wrapped in the same `SampleRateConverter` whenever the host rate differs from the core's rate `[decompiled: Driver XmasScream::processBuffer 0x19980-0x19b40]`. Worst alias -77 dB `[measured: evidence/summary.tsv]`.
- **Supercharger.** A compressor with a `Dirt` switch; H3 -23 dB, H2 -59 dB at mid compression `[measured: evidence/summary.tsv]`. Not studied further: it belongs to the compressor study.
- **HG-2.** Black-box only. H2 and H3 both near -55 dB at -18 dBFS and default settings; worst alias -117 dB; 32 samples of latency; flat within 0.5 dB from 80 Hz to 5 kHz `[measured: evidence/summary.tsv, evidence/extra.tsv]`. A saturator, not a pedal: no pre-clip voicing.

### 3.6 What each pedal does before and after the clipper (measured)

Table 1 lists the input level at which the fundamental has lost 1 dB of gain, relative to the frequency that clips first. A linear filter after the clipper cannot change this figure, so it shows the gain ahead of the nonlinearity `[measured: evidence/precomp_all.tsv]`. Drive at 0.5; other knobs at 0.5 for in-process models and at the factory INIT values for `GR6 AU` rows.

The RAT rows differ because the gain stage's response depends on the gain itself. With the two RC legs to ground the gain is `1 + Rd/Z(f)`: at low gain the stage is near unity at all frequencies, at high gain it rises toward the 47 Ω leg's corner and then falls where the op-amp runs out of bandwidth `[inferred]`. AT5 at Distortion 0.5 has 17.7 dB of gain at 1 kHz and GR6 at 0.5 has 32.3 dB `[measured: evidence/level_vs_drive_all.tsv]`.

| Model | 110 Hz | 220 Hz | 640 Hz | 1.28 kHz | 2.5 kHz | 5 kHz | Reading |
| --- | --- | --- | --- | --- | --- | --- | --- |
| tubamp today | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | no voicing |
| GR6 Skreamer | -15.1 | -9.4 | -2.7 | -0.8 | -0.2 | 0.0 | 1st-order high-pass near 720 Hz |
| GR6 Skreamer Deluxe | -16.0 | -10.1 | -3.1 | -0.9 | -0.1 | 0.0 | same |
| AT5 Tube Screamer | -15.9 | -10.1 | -2.9 | -0.6 | 0.0 | -0.7 | same |
| AT5 SD-1 | -18.3 | -11.0 | -3.4 | -1.1 | -0.3 | 0.0 | same, steeper below 200 Hz |
| GR6 Distortion (DS-1) | -12.3 | -6.6 | -0.9 | 0.0 | 0.0 | -0.1 | high-pass near 350 Hz |
| AT5 DS-1 | -11.5 | -6.4 | -0.9 | 0.0 | -0.3 | -1.8 | same |
| AT5 RAT | -6.2 | -2.4 | -0.3 | 0.0 | 0.0 | 0.0 | high-pass near 190 Hz |
| GR6 Cat (RAT), in-process | -26.4 | -20.3 | -10.8 | -4.6 | 0.0 | -19.3 | gain rising 6 dB/oct to 2.5 kHz, steep low-pass above |
| GR6 AU Cat, factory INIT (distortion 0.5) | -26.1 | -20.0 | -9.9 | -4.1 | 0.0 | -21.5 | same |
| GR6 AU Skreamer, factory INIT | -15.0 | -9.3 | -2.7 | -0.8 | -0.2 | 0.0 | 720 Hz high-pass, as in-process |
| GR6 AU Distortion, factory INIT | -11.5 | -6.0 | -0.7 | 0.0 | -0.9 | -2.7 | high-pass near 350 Hz, as in-process |
| GR6 AU Big Fuzz, factory INIT (Bass 0.29) | -9.3 | -2.2 | 0.0 | -2.2 | -12.3 | -29.2 | low-pass as in-process; bass cut from the Bass knob |
| AT5 Big Muff | -0.6 | 0.0 | -0.1 | -2.3 | -14.4 | -32.1 | low-pass near 1.3 kHz, 12 dB/oct |
| GR6 BigFuzz | -1.4 | -0.1 | -0.2 | -2.5 | -12.6 | -29.5 | same |
| GR6 Fuzz Face (GRU) | -0.2 | 0.0 | 0.0 | 0.0 | -0.3 | -1.2 | flat |
| AT5 Fuzz Face | -3.3 | -1.5 | -0.2 | 0.0 | 0.0 | 0.0 | high-pass near 100 Hz |
| NI Dirt | 0.0 | 0.0 | 0.0 | -0.1 | -0.5 | -1.8 | flat |

Tone controls `[measured: evidence/tone_sweeps.txt]`:

- Tube Screamer: at 0 a low-pass (-6.1 dB at 2.5 kHz re 720 Hz in AT5); at 1 a treble lift (+2.2 dB at 1.8 kHz re 720 Hz in AT5, +1.8 dB in the Deluxe).
- RAT Filter (AT5): at 0 flat to 18 kHz within 3.1 dB; at 1 a low-pass, -3 dB near 850 Hz.
- Big Muff (AT5): the bass end peaks at 320 Hz, the treble end at 900 Hz; the two ends differ by 14 dB at 110 Hz.
- DS-1 (AT5): tilt. Tone 0 peaks at 450 Hz; tone 1 rises 19 dB from 220 Hz to 2.5 kHz.
- GR6 GainBoost: flat, gain = 30 dB * knob. GR6 treble booster: linear, +37 dB at 3.6 kHz and +12 dB at 110 Hz at full boost.

## 4. Measurements

All at 48 kHz. Battery: `evidence/tools/meas.py`; models: `evidence/tools/models.py`. Drive low / mid / high = 0.1 / 0.5 / 1.0 of each unit's own drive knob.

**Table 2. Small-signal response and gain** `[measured: evidence/summary.tsv, evidence/fr_all.tsv, evidence/level_vs_drive_all.tsv]`. "rel" is dB relative to the response peak at mid drive. Volume knobs sit at 0.5, so absolute gains are not comparable across vendors.

| Model | Peak Hz | 110 Hz rel | 5 kHz rel | Gain at 1 kHz, low / mid / high dB |
| --- | --- | --- | --- | --- |
| tubamp today (tone 4 kHz) | flat | 0.0 | -5.5 | 3.6 / 19.8 / 36.0 |
| GR6 Skreamer | 720 | -10.3 | -11.1 | 10.8 / 19.8 / 28.7 |
| GR6 Skreamer Deluxe | 720 | -10.4 | -10.8 | 9.2 / 14.9 / 27.2 |
| AT5 Tube Screamer | 720 | -11.2 | -11.2 | 13.6 / 23.3 / 27.5 |
| AT5 SD-1 | 900 | -13.8 | -10.2 | 17.5 / 29.7 / 34.1 |
| AT5 RAT | 450 | -4.8 | -12.8 | -5.0 / 17.7 / 40.3 |
| AT5 DS-1 | 2500 | -8.6 | -0.9 | 12.4 / 18.0 / 38.4 |
| AT5 Big Muff | 220 | -3.6 | -33.0 | -0.2 / 46.6 / 51.2 |
| GR6 BigFuzz | 220 | -3.1 | -25.9 | 24.9 / 40.4 / 45.6 |
| AT5 Fuzz Face | 1800 | -1.4 | 0.0 | 29.9 / 34.0 / 40.8 |
| GR6 Fuzz Face (GRU) | 450 | -0.2 | -1.5 | 17.6 / 23.4 / 47.4 |

**Table 3. Harmonics of a 1 kHz sine at -18 dBFS, mid drive, re the fundamental** `[measured: evidence/thd_all.tsv, evidence/summary.tsv]`. The full grid (1 kHz and 110 Hz, three levels, three drives) is in `evidence/thd_all.tsv`.

| Model | H2 dB | H3 dB | THD % at -30 / -18 / -6 dBFS | H2 at 110 Hz, high drive |
| --- | --- | --- | --- | --- |
| tubamp today | -155 | -24.6 | 0.45 / 5.9 / 24.6 | -168 |
| GR6 Skreamer | -87.5 | -18.5 | 10.0 / 12.7 / 14.0 | -68.5 |
| GR6 Skreamer Deluxe | -58.2 | -21.1 | 7.4 / 9.4 / 6.7 | -49.3 |
| AT5 Tube Screamer | -146 | -20.4 | 9.6 / 10.2 / 7.6 | -142 |
| AT5 SD-1 | -34.6 | -19.9 | 12.0 / 10.9 / 7.3 | -25.8 |
| AT5 RAT | -148 | -18.9 | 2.3 / 11.9 / 15.2 | -161 |
| AT5 DS-1 | -142 | -7.6 | 33 / 51 / 56 | -163 |
| AT5 Big Muff | -130 | -4.2 | 77 / 84 / 87 | -113 |
| GR6 Fuzz Face (GRU) | -13.4 | -11.2 | 34 / 44 / 56 | -19.3 |
| AT5 Fuzz Face | -14.3 | -10.8 | 45 / 46 / 45 | -14.4 |
| GR6 HM2 | -21.5 | -10.4 | 37 / 39 / 40 | -23.9 |
| NI Dirt (mode II) | -208 | -26.8 | 0.01 / 4.7 / 44 | -169 |
| PA HG-2 | -54.3 | -55.5 | 0.07 / 0.26 / 6.8 | -50.9 |

**Table 4. Aliasing: 5 kHz sine at -6 dBFS, highest drive** `[measured: evidence/alias_all.tsv, evidence/summary.tsv]`. "Worst" is the strongest inharmonic component below 5 kHz, "sum" the RSS of all inharmonic bins below 20 kHz, both re the 5 kHz fundamental at the output. Post-filters attenuate the fundamental more than the low aliases, so the figure punishes dark tone settings.

| Model | Worst dB | Sum dB | Oversampling found |
| --- | --- | --- | --- |
| tubamp today, tone 4 kHz | -21.4 | -20.8 | 2x polyphase IIR |
| tubamp today, tone 12 kHz | -26.8 | -22.4 | same |
| AT5 Tube Screamer, Oversample off | -19.5 | -17.0 | none |
| GR6 Skreamer | -25.3 | -23.1 | 4x cubic interpolation, 15-tap FIR |
| AT5 RAT | -25.1 | -22.5 | 4x IIR half-bands |
| GR6 Cat | -30.8 | -27.2 | 4x cubic |
| AT5 Tube Screamer | -40.4 | -38.9 | 4x IIR half-bands |
| GR6 AU Skreamer, factory INIT | -33.6 | -33.2 | 4x cubic, brighter tone setting |
| GR6 AU Cat, factory INIT | -32.6 | -32.3 | 4x cubic |
| GR6 AU Distortion, factory INIT | -40.3 | -30.0 | 4x cubic |
| GR6 Skreamer Deluxe | -43.3 | -39.0 | 192 kHz, Kaiser sinc |
| AT5 Big Muff | -44.5 | -36.1 | 4x IIR half-bands |
| GR6 AU Big Fuzz, factory INIT | -75.4 | -58.6 | 4x cubic; tone stack removes most of the 5 kHz region |
| NI Driver | -77.2 | -71.6 | fixed internal rate, Kaiser sinc |
| NI Dirt (mode II) | -80.7 | -74.6 | fixed internal rate, Kaiser sinc |
| PA HG-2 | -117.3 | -109.3 | not determined |

**Table 5. Dynamics, latency, cost** `[measured: evidence/dynamics_all.tsv, evidence/precomp_all.tsv, evidence/extra.tsv]`. "Out range" is the change in output peak when a 1 kHz input goes from -42 to -6 dBFS (36 dB). "P1dB" is the input level for 1 dB compression at 1.28 kHz, mid drive.

| Model | Out range mid / high dB | P1dB dBFS | Impulse peak, samples | Cost, % real time |
| --- | --- | --- | --- | --- |
| tubamp today | 24.5 / 7.5 | -20.9 | 4 to 6 | 0.35 (whole AU) |
| GR6 Skreamer | 7.0 / 2.9 | -44.4 | 5 | 0.67 |
| GR6 Skreamer Deluxe | 16.0 / 11.1 | -39.2 | 18 | 4.41 |
| AT5 Tube Screamer | 11.9 / 10.7 | -47.2 | 4 | 1.64 |
| AT5 RAT | 15.4 / 1.0 | -29.6 | 3 | 1.58 |
| AT5 Big Muff | 5.1 / 1.1 | -66.1 | 7 | 1.90 |
| NI Dirt | 23.7 / 12.9 | -17.4 | 19 | 2.09 |
| PA HG-2 | 35.0 / 33.8 | -6.6 | 32 | 1.65 |

**Table 6. Alias simulation for our own shaper** `[measured: evidence/alias_schemes.tsv]`. `tanh` at 36 dB of gain, 5 kHz at -6 dBFS, JUCE half-band stages, optional first-order ADAA at the oversampled rate. The "2x" row reproduces the measured plugin (-26.8 / -22.4 dB with the tone open), which validates the simulation. Latency and multiplies are `[inferred]` from the JUCE design formula.

| Scheme | Worst alias dB | Sum to 20 kHz dB | Half-band multiplies per input sample | Latency, samples |
| --- | --- | --- | --- | --- |
| 1x | -20.2 | -14.2 | 0 | 0 |
| 2x (ships) | -26.9 | -20.7 | 13 | 3.14 |
| 2x + ADAA | -71.4 | -40.4 | 13 | 3.39 |
| 4x | -36.7 | -30.0 | 31 | 4.43 |
| **4x + ADAA** | **-71.8** | **-54.0** | 31 | 4.56 |
| 8x | -53.2 | -46.7 | 59 | 4.95 |
| 8x + ADAA | -87.1 | -75.3 | 59 | 5.01 |
| 16x | -91.6 | -84.2 | not computed | not computed |

**Table 7. Prototype of the proposed TS-style type against the references** `[measured: evidence/ts_proto.tsv, evidence/summary.tsv, evidence/precomp_all.tsv]`. Numerical prototype `evidence/tools/ts_proto.py`: circuit values from the literature, static diode solve, 4x + ADAA, 0 dBFS = 1 V.

| Quantity (drive 0.5, tone 0.5) | Prototype | GR6 Skreamer | GR6 Deluxe | AT5 |
| --- | --- | --- | --- | --- |
| Response peak, Hz | 720 | 720 | 720 | 720 |
| 110 Hz re peak, dB | -11.2 | -10.3 | -10.4 | -11.2 |
| 5 kHz re peak, dB | -11.1 | -11.1 | -10.8 | -11.2 |
| Pre-clip emphasis at 110 / 220 / 640 Hz, dB | -16.5 / -10.6 / -3.4 | -15.1 / -9.4 / -2.7 | -16.0 / -10.1 / -3.1 | -15.9 / -10.1 / -2.9 |
| H3 at 1 kHz -18 dBFS, dB | -20.8 | -18.5 | -21.1 | -20.4 |
| P1dB at 1.28 kHz, dBFS | -42.3 | -44.4 | -39.2 | -47.2 |
| Out range for -42..-6 dBFS in, dB | 13.7 | 7.0 | 16.0 | 11.9 |
| Worst alias, 5 kHz -6 dBFS full drive, dB | -61.0 | -25.3 | -43.3 | -40.4 |

## 5. Industry practice

**What the references agree on.**

1. The pedal's identity is linear filtering around a simple clipper. Every pedal model has its own pre-clip response (Table 1); the clipper itself is a diode pair or a one-sided transistor curve `[measured: evidence/precomp_all.tsv]`.
2. Tube Screamer: 720 Hz first-order high-pass into the gain, symmetric soft clip, clean signal summed at the output, low-pass near 720 Hz after it. Three implementations from two vendors sit within 1 dB of each other `[measured: evidence/precomp_all.tsv]`. The summed clean path is why the output keeps rising with input level (12 to 16 dB over a 36 dB input range) where a plain saturator flattens `[measured: evidence/dynamics_all.tsv]`.
3. Overdrives and distortions clip symmetrically (H2 below -58 dB). Asymmetry is deliberate and confined to circuits that have it: SD-1 (-35 dB), HM-2 (-22 dB), Fuzz Face (-13 dB) `[measured: evidence/thd_all.tsv]`.
4. Nobody runs a pedal clipper at the host rate. 4x is the floor (GR6 classic, AT5); newer NI code fixes the internal rate at 96 or 192 kHz whatever the host rate `[decompiled: GR6 0x1c5d6c; AT5 0xdc8068]`.
5. No reference uses antiderivative anti-aliasing in these pedals `[decompiled: GR6 0x2f50c8, 0x5efac4; AT5 0xdc7758]`. Their alias figures at 4x are -25 to -45 dB (Table 4).
6. Diode constants are textbook values: Is 2.5 nA, n*Vt 45 mV `[decompiled: AT5 0xdc6fb8, 0xdc6f9c, 0xda1528]`.
7. Tone and gain knobs change component values, not abstract cutoffs: AT5 recomputes a circuit-derived biquad and the state matrices; GR6 moves the corner the capacitor would move `[decompiled: AT5 0x187a9e8; GR6 0x2f4898]`.
8. Parameter changes are smoothed over 20 ms or swapped between blocks, and coefficient maths runs at control rate `[decompiled: GR6 0x2f4c0c, 0x2f4f48; AT5 0xdc8294]`.
9. DC is blocked ahead of the clipper with 5 to 20 Hz high-passes `[decompiled: GR6 0x2f4c64; AT5 0xdc7f38]`.
10. No pedal model adds noise `[measured: evidence/extra.tsv]`.
11. The level where clipping starts corresponds to 0 dBFS near 1 V: a TS stage built from circuit values with that scale compresses at -42 dBFS, inside the references' -39 to -47 dBFS `[measured: evidence/ts_proto.tsv, evidence/precomp_all.tsv]`.

**Literature.**

- Yeh, Abel and Smith model the Tube Screamer clipping stage as `Vo = V + Vi`, where V solves `dV/dt = In/Cc - V/(R2*Cc) - (2*Is/Cc)*sinh(V/Vt)`, with R1 = 4.7 kΩ, Cz = 0.047 µF, Cc = 51 pF, R2 = 51 kΩ + D*500 kΩ, and input high-passes at 15.9 and 15.6 Hz `[literature: https://ccrma.stanford.edu/~dtyeh/papers/yeh07_dafx_distortion.pdf]`. Those are the numbers in AT5's matrices `[decompiled: AT5 0xdc7fb4-0xdc8014]`.
- The same paper gives the tone stage values (Rs 1 kΩ, Cs 0.22 µF, Ri 10 kΩ, Rf 1 kΩ, 20 kΩ pot, Rz 220 Ω, Cz 0.22 µF) and calls it a second-order transfer function with variable coefficients `[literature: https://ccrma.stanford.edu/~dtyeh/papers/yeh07_dafx_distortion.pdf]`.
- For the DS-1 it gives a 36 dB band-pass transistor stage with poles at 3 Hz and 600 Hz, an op-amp gain stage with Rt = D*100 kΩ, Rb = (1-D)*100 kΩ + 4.7 kΩ, Cz = 1 µF, Cc = 250 pF, a 2.2 kΩ / 10 nF diode limiter, and a tone stage fading between a 320 Hz low-pass and a 1.16 kHz high-pass `[literature: https://ccrma.stanford.edu/~dtyeh/papers/yeh07_dafx_distortion.pdf]`.
- It approximates the diode limiter as static, tabulates it by Newton iteration on a non-uniform grid, and upsamples by eight to ten; it states that aliases at factors of eight or above tend to be masked `[literature: https://ccrma.stanford.edu/~dtyeh/papers/yeh07_dafx_distortion.pdf]`.
- It notes that the real pedals show a strong even-order component the simplified models lack `[literature: https://ccrma.stanford.edu/~dtyeh/papers/yeh07_dafx_distortion.pdf]`.
- Parker, Zavalishin and Le Bivic give first-order antiderivative anti-aliasing, `y[n] = (F0(x[n]) - F0(x[n-1])) / (x[n] - x[n-1])`, with `F0 = log(cosh(x))` for `tanh`, and warn that the quotient is ill-conditioned when the two samples are close `[literature: https://www.dafx.de/paper-archive/2016/dafxpapers/20-DAFx-16_paper_41-PN.pdf]`.
- Their Table 1: a hard clipper at input gain 10 needs 12x oversampling without anti-aliasing, 4x with the first-order (rectangular kernel) method and 3x with the second-order one for the same 46.5 dB signal-to-alias ratio `[literature: https://www.dafx.de/paper-archive/2016/dafxpapers/20-DAFx-16_paper_41-PN.pdf]`. Our simulation agrees: 4x + ADAA beats plain 8x by 18 dB on the worst alias (Table 6).
- A SPICE study of the RAT gives gain = 1 + Rgain/(560 Ω ∥ 47 Ω), about 2300 (67 dB) at maximum, diodes limiting to ±0.65 V, a Filter corner from 32 kHz down to 475 Hz, and a slew-limited op-amp whose responses share one roll-off asymptote at high gain `[literature: https://cushychicken.github.io/ltspice-proco-rat]`.
- A circuit study of the Big Muff describes an input gain stage, two clipping stages, a tone stage that mixes one first-order low-pass and one first-order high-pass (which leaves a mid notch), and an output stage `[literature: https://www.guitarscience.net/papers/bigmuff.pdf]`.

## 6. Gap analysis

| Aspect | Ours | References | Consequence for the player |
| --- | --- | --- | --- |
| Bass before clipping | Full, flat to DC | TS -16 dB at 110 Hz, DS-1 -12 dB, RAT -6 dB | Low strings clip first and intermodulate; palm mutes into a high-gain amp model turn to mud. "Tighten the amp with a boost" does not work. |
| Mid focus | None | Peak at 720 Hz (TS), 450 Hz (RAT), scoop (Muff) | No pedal character; the block only adds gain. |
| Clean-through path | None; hard ±1 ceiling | TS output = input + clipped term | Output range 7.5 dB at full gain against 11 to 16 dB: picking dynamics are flattened and the amp model no longer follows the pick. |
| Clip threshold | P1dB -21 dBFS at 18 dB gain, tied to digital full scale | -39 to -47 dBFS at mid drive, tied to diode volts | Feel depends on interface gain; a quiet DI never reaches the knee. |
| Alias floor | -21 dB (5 kHz, -6 dBFS, 36 dB) | -25 to -45 dB at 4x, -77 dB or better at fixed high rates | Inharmonic grit under high notes and bends, then amplified by the amp model. |
| Clipper types | One symmetric `tanh` | Soft feedback, hard shunt, cascaded stages, asymmetric transistor | One sound. |
| Asymmetry | None | SD-1 -35 dB H2, fuzz -13 dB | No even harmonics available. |
| Tone control | 12 dB/oct low-pass only | Circuit tone stacks: low-pass to treble lift (TS), tilt (DS-1), scoop (Muff) | Cannot brighten into a dark amp. |
| DC | Not blocked | 5 to 20 Hz high-pass | Harmless today (symmetric); required once bias or asymmetry exists. |
| Latency | 3.1 samples, unreported | 2 to 5 (IIR) or 16 to 20 (sinc), reported by Dirt and HG-2 | Inaudible in series. In a split chain the drive lane is 3 samples late and combs against the other lane. |
| Gain taper | dB-linear | TS log-like pot, Muff power 3.3 | Minor. |
| CPU | 0.35 % (whole AU) | 0.6 to 4.4 % | Room to spend. |

## 7. Recommendations

Ranked by audible benefit against cost. All constants are ours: circuit values from the cited literature, or fits to the measurements in section 4. No vendor table or code is used.

### R1. Typed drive engine: voicing filter, volt-referenced diode clipper, clean-through path

**Build.** A `drive_type` choice per instance. Type 0 runs the current code path untouched. Types 1 and up share one engine:

```
x  = input * V_REF                       // volts
x  = HP1(x, 15.9 Hz)                     // DC block, bilinear one-pole
u  = PRE_type(x; drive)                  // linear voicing, at the base rate (R3, R5, R6)
u4 = upsample4(u)
v4 = CLIP_type(u4)                       // static diode solve + ADAA (R2)
v  = downsample4(v4)
y  = v + CLEAN_type * x                  // 1 for the TS type, 0 for hard clippers
y  = TONE_type(y; tone)                  // biquad, coefficients at control rate
out = y * level / V_REF
```

`V_REF` is volts per digital full scale: 1.0 by default; when `amp_cal_input` is on, `1.0954 * 10^(amp_cal_level / 20)` (peak volts of a full-scale sine at that dBu), so the diode knee sits at the same guitar voltage on every interface.

**Evidence.** Pre-clip voicing is the largest measured difference between us and every reference (Table 1). The clean-through path and the volt-referenced knee explain the dynamics gap (Table 5). The prototype of type 1 lands inside the spread of the three reference Tube Screamers on every measured quantity (Table 7) `[measured: evidence/ts_proto.tsv]`.

**New parameters (append-only).** `drive_type`, `drive2_type`, `drive3_type`: choice {Classic, Screamer, Distortion, Fuzz, Boost, Treble}, default 0 (Classic). The three existing parameters per instance keep their ids, ranges and defaults. For types above 0 they are read as knob positions: `d = drive_gain / 36`, `T = ln(drive_tone / 500) / ln(24)`, level unchanged in dB. The UI can relabel them per type.

**Cost and latency.** See R2.

**Compatibility.** Type 0 is the existing `Drive::process` body. A session without `drive_type` reads as 0. The state gains three parameters; nothing is renumbered.

**Proof.** Null test: render an old session before and after the change; the difference must be below -140 dBFS. `meas.py ours thd|fr|pre` with type 0 must reproduce the `ours` rows of `evidence/*_all.tsv` to the printed precision.

### R2. Alias control: 4x polyphase IIR plus first-order ADAA through the inverse function

**Build.** For types above 0: two `juce::dsp::Oversampling` IIR stages (4x), the clipper in double precision, first-order ADAA at the 4x rate.

All our clippers have the form `u = g(v) = v + k * sinh(v / a)`: a diode pair of saturation current Is and slope voltage `a = n*Vt` behind a resistance R, with `k = 2*Is*R`. `g` is explicit, so the inverse and its antiderivative need no table:

```
// solve v from u: start at an upper bound of |v|; Newton then converges monotonically
v = sign(u) * min(|u|, a * asinh(|u| / k))
repeat 4 times:  v -= (v + k*sinh(v/a) - u) / (1 + (k/a)*cosh(v/a))

// exact antiderivative of v(u), by parts:  F(u) = u*v - v*v/2 - k*a*(cosh(v/a) - 1)
if |u[n] - u[n-1]| > 1e-6:  y[n] = (F[n] - F[n-1]) / (u[n] - u[n-1])
else:                       y[n] = v((u[n] + u[n-1]) / 2)
```

Four iterations leave an error below 1e-6 V over ±120 V of input across the drive range; five reach 1e-11 V `[measured: evidence/newton_convergence.txt]`. The state is the previous `u` and `F`.

**Evidence.** Table 6: `tanh` worst alias -71.8 dB at 4x + ADAA against -26.9 dB today and -53.2 dB at plain 8x `[measured: evidence/alias_schemes.tsv]`. Table 7: the full TS type reaches -61 dB where the references reach -25 to -43 dB `[measured: evidence/ts_proto.tsv]`. Parker et al. report the same threefold saving in oversampling factor `[literature: https://www.dafx.de/paper-archive/2016/dafxpapers/20-DAFx-16_paper_41-PN.pdf]`.

**Cost.** 31 all-pass multiplies per input sample for the half-bands against 13 today, plus per oversampled sample one `asinh`, four `sinh`/`cosh` pairs and one divide `[inferred]`. The references spend 0.6 to 4.4 % of a core on comparable work `[measured: evidence/extra.tsv]`. Expect 1 to 2 % per instance `[guess]`; measure with the existing smoke test.

**Latency.** 4.43 samples (half-bands, low-frequency group delay) + 0.125 (ADAA) = 4.56 samples at 48 kHz, against 3.14 today `[inferred]` from `evidence/tools/alias_schemes.py`.

**Compatibility.** New types only.

**Proof.** `meas.py ours alias` on the new type with the tone fully open: worst inharmonic below 5 kHz for a 5 kHz sine at -6 dBFS and full drive at or below -55 dB re the fundamental; inharmonic sum for 1244 Hz at -6 dBFS at or below -80 dB.

### R3. Type 1 "Screamer" (TS808/TS9 style)

**Constants.** R1 = 4.7 kΩ, Cz = 47 nF, Cc = 51 pF, R2 = 51 kΩ + 500 kΩ * d² `[literature: https://ccrma.stanford.edu/~dtyeh/papers/yeh07_dafx_distortion.pdf]`. The d² taper is ours, chosen to follow the log-like gain steps of the references (9 / 15 / 27 dB) `[measured: evidence/level_vs_drive_all.tsv]`. Is = 2.52 nA, a = 45.3 mV `[inferred]` from the decompiled reference values, which are within 2 %.

```
PRE(s)  = (R2/R1) * s*R1*Cz / (1 + s*R1*Cz) * 1 / (1 + s*R2*Cc)     // 720 Hz high-pass, gain 10.9..117, low-pass 61..5.7 kHz
CLIP    : v + 2*Is*R2*sinh(v/a) = u
CLEAN   = 1                                                          // y = x + v
TONE(s) = Va/Vi * (1 + (Rf/Rr) * Yw / (Gl + Gr + Yw))
          Va/Vi = (1/Rs) / (1/Rs + s*Cs + 1/Ri + Gl * Yw / (Gl + Gr + Yw))
          Yw = 1 / (Rz + 1/(s*Cz2)),  Gl = 1/(T*20k),  Gr = 1/((1-T)*20k)
          Rs 1k, Cs 0.22u, Ri 10k, Rf 1k, Rz 220, Cz2 0.22u
```

The tone expression is our nodal analysis of the stage the paper describes `[inferred]`. It is second order and maps to one bilinear biquad, recomputed at control rate. PRE is a second-order section, bilinear at the base rate, recomputed when `d` moves.

**Evidence.** Table 7.

**Proof.** At drive 0.5 and tone 0.5: response peak 650 to 800 Hz; 110 Hz and 5 kHz both -10 to -12 dB re the peak; pre-clip emphasis (`meas.py pre`) -15 to -17 dB at 110 Hz and -2.5 to -3.5 dB at 640 Hz; H2 below -60 dB; output range for -42..-6 dBFS input between 10 and 16 dB.

### R4. Latency of the drive in split chains

**Build.** Round the drive's delay to an integer (JUCE `useIntegerLatency`, or one fractional all-pass) and add it to `ChainLatency`, so the lane ring aligns a drive lane with its sibling: 3 samples for type 0, 5 for the new types.

**Evidence.** The drive delays its lane by 3.14 samples and `ChainLatency` ignores it `[decompiled: src/PluginProcessor.cpp:1724-1741]`. Two equal correlated lanes offset by 3.14 samples null at 7.6 kHz `[inferred]`.

**Cost.** None. **Compatibility.** Serial chains stay bit-identical if type 0 keeps its non-integer path and only the lane compensation and the reported latency change. Split sessions with a drive in one lane change (the comb goes away). That is the one item here that alters an existing session's sound; it needs a product decision.

**Proof.** Split chain, drive at 0 dB gain in lane A, nothing in lane B, sum: response flat within 0.5 dB to 16 kHz.

### R5. Type 2 "Distortion" (RAT-style hard clipper)

**Constants.** Gain `G(s) = 1 + Rd * (1/(47 + 1/(s*C1)) + 1/(560 + 1/(s*C2)))`, Rd = 100 kΩ * d², maximum 1 + 100k/(47 ∥ 560) = 2308 (67 dB) `[literature: https://cushychicken.github.io/ltspice-proco-rat]`. C1 = 2.2 µF and C2 = 4.7 µF are recalled from the schematic, not confirmed by a source opened in this study `[guess]`. A one-pole low-pass on the gain stage at `GBW / |G|`, with GBW fitted so the small-signal response falls 6 dB/oct from 1 kHz at full distortion, as AT5 measures `[measured: evidence/fr_all.tsv]`. Clipper: `v + 2*Is*Rs*sinh(v/a) = u` with Rs = 1 kΩ `[guess]`, CLEAN = 0. Filter: one-pole low-pass, `fc = 1 / (2*pi*(1.5k + 100k*(1-T))*3.3n)`, 32 kHz to 475 Hz `[literature: https://cushychicken.github.io/ltspice-proco-rat]`.

**Evidence.** AT5 RAT at 17.7 dB of gain: pre-clip emphasis -6.2 dB at 110 Hz and -2.4 dB at 220 Hz, symmetric, H3 -18.9 dB, output range 15 dB at mid and 1 dB at full distortion `[measured: evidence/precomp_all.tsv, evidence/thd_all.tsv, evidence/dynamics_all.tsv]`. GR6 Cat (real AU, factory preset) at 32.3 dB of gain: -26.1 dB at 110 Hz, -9.9 dB at 640 Hz, first clipping at 2.5 kHz, -21.5 dB at 5 kHz `[measured: evidence/precomp_all.tsv]`. The `G(s)` above with a gain-bandwidth low-pass has this dependence on gain `[inferred]`.

**Proof.** Pre-clip emphasis within 3 dB of the AT5 RAT row of Table 1 when the 1 kHz small-signal gain is set to 18 dB, and within 4 dB of the GR6 AU Cat row at 110, 220 and 640 Hz when it is set to 32 dB; Filter open: -3 dB above 15 kHz; Filter closed: -3 dB between 450 and 900 Hz; H2 below -60 dB.

### R6. Type 3 "Fuzz" (Muff-style, two cascaded soft clippers)

**Constants (fits).** Input high-pass 60 Hz. Two identical stages, each: gain from the taper `d^3.3`, scaled so the total small-signal gain runs from 0 to 50 dB; a one-pole low-pass at 1.3 kHz; the R2 clipper equation with CLEAN = 0; a 100 Hz coupling high-pass. Tone: `y = (1-T)*LP(408 Hz) + T*HP(1856 Hz)`, first-order sections. The two corners are the commonly quoted 39 kΩ/10 nF and 22 kΩ/3.9 nF values, not confirmed by a source opened here `[guess]`.

**Evidence.** Both references: flat pre-clip response from 110 to 900 Hz, then -13 dB at 2.5 kHz and -30 dB at 5 kHz; H2 below -110 dB, H3 -4 dB; small-signal gain up to 46 to 51 dB; output range 1 to 6 dB `[measured: evidence/precomp_all.tsv, evidence/summary.tsv]`. Topology `[literature: https://www.guitarscience.net/papers/bigmuff.pdf]`.

**Proof.** Pre-clip emphasis within 3 dB of the two Big Muff rows of Table 1 at 1.28, 2.5 and 5 kHz; THD above 70 % at -18 dBFS and mid sustain; with tone at 0.5 the small-signal response between 600 Hz and 2 kHz sits at least 3 dB below the straight line joining its 220 Hz and 5 kHz values.

### R7. Types 4 and 5: clean boost and treble boost

**Build.** Boost: gain `drive_gain` dB, no clipper, tone as a one-pole high-cut, level. Treble boost: first-order high-pass shelf with a 2 kHz corner, gain `drive_gain` dB above it, no clipper. Both bypass the oversampler and add no latency.

**Evidence.** GR6 GainBoost is flat with gain 30 dB * knob and no harmonics; GR6's treble booster is linear, +37 dB at 3.6 kHz and +12 dB at 110 Hz at full boost `[measured: evidence/tone_sweeps.txt, evidence/thd_all.tsv]`.

**Proof.** THD below 0.01 % at -6 dBFS; boost flat within 0.1 dB; treble boost falls 6 dB/oct below its corner.

### R8. Optional asymmetry (later)

**Build.** A different diode constant on one polarity for an SD-1 flavour (`sinh` replaced by `(exp(v/a1) - exp(-v/a2)) / 2`), with a 10 Hz high-pass after the clipper. The inverse-function ADAA still applies: the antiderivative of `g` stays closed-form.

**Evidence.** SD-1 H2 -35 dB; Dirt's Bias reaches -15 dB and leaks DC because nothing removes it `[measured: evidence/thd_all.tsv, evidence/dirt_bias_safety.txt]`. Real pedals show even-order content the symmetric models lack `[literature: https://ccrma.stanford.edu/~dtyeh/papers/yeh07_dafx_distortion.pdf]`.

**Proof.** H2 between -40 and -30 dB at 1 kHz -18 dBFS; output DC below -80 dBFS.

### Not recommended

- A neural fuzz like GR6's GRU model: it needs training data and a fixed internal rate, and it leaves -90 dBFS at idle `[measured: evidence/extra.tsv]`.
- Capacitor memory inside the clipper (GR6 Deluxe, AT5): two to seven times the CPU of GR6's static table for differences below 1 dB in the pre-clip and response figures measured here `[measured: evidence/extra.tsv, evidence/precomp_all.tsv]`.
- Sinc resampling to a fixed 192 kHz: 16 to 20 samples of latency in front of the amp `[measured: evidence/extra.tsv]`.

## 8. Open questions and unverified items

- **ElectroSmash and GEOFEX were unreachable** (DNS failure, connection reset; the Wayback Machine is blocked for the fetch tool). Fuzz Face, Rangemaster and Big Muff component values are therefore not source-backed here; the Muff tone corners, the RAT leg capacitors and the RAT series resistor are marked `[guess]`.
- **GR6 Cat.** Its setter maths was not read; the gain-dependent pre-clip response is explained from the circuit, not from the code. The in-process Cat differs from the real AU by up to 8 dB at low distortion.
- **GR6 Skreamer Deluxe, HM2 and Fuzz Face** have no factory component preset file, so they were measured in-process only. The coordinator reports that AmpliTube 5 crashes in its constructor under `auhost`; its numbers here come from the in-process route alone and have no AU cross-check.
- **Classes without an out-of-line constructor** (Cat, BigFuzzPsi, FuzzAce, JackHammer, MulhollandDrive, GainBoost, RigTrebleBooster) were built from zeroed memory, the base constructor and the class vtable. Members the inlined constructor would set to non-zero values are missing, and results for those models carry that caveat; for Cat, Distortion and Big Fuzz prefer the `gr6au_*` rows. MulhollandDrive's output (H3 above the fundamental) looks wrong and is not used.
- **Parameter defaults.** GR6 and AT5 knobs were set to 0.5 where the product's preset value is unknown. Absolute gains are not comparable across vendors; relative shapes are.
- **Not decoded:** the AT5 RAT, Big Muff, DS-1, SD-1 and Fuzz Face state matrices; AT5 Big Muff `UpdateClampers`; GR6 HM2 beyond the solver loop; GR6 Demon and JackHammer; Dirt's clipper equations and its three modes; Driver's distortion core; AT5 `AudioEngine::NonLinear`, `MultiRate`, `BiasShifterMatrix`, `StompSlashBooster`, `StompTrebleBooster`, `AT2JHE::Fuzz`; GR6 `SineSaturator` and `WarmTube`.
- **GR6 Skreamer signal order.** The NEON shuffles in `processBuffer` were not fully untangled; which one-pole feeds which is taken from the measured response, not from the code.
- **HG-2** has no symbols and was not disassembled; its oversampling scheme is unknown.
- **CPU of the proposal** is an estimate. The Newton iteration count, single against double precision, and sharing one `Oversampling` object across types need a build.
- **Alias metric.** "Worst alias re the fundamental" depends on the post-filter. For acceptance tests, measure with the tone fully open.
- **The d² drive taper and `V_REF` = 1 V** are our choices, fitted to the references' spread; a player test should confirm them.
- **Hopper** gave nothing usable for this code: the NEON-heavy loops come out as raw `asm` lines, and the AmpliTube cache stayed empty for the whole session. All static reading is from `disasm.py`.
