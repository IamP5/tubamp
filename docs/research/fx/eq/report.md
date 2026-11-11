# EQ and tone controls: reference study and improvement spec

## Verification status

Independently verified on 2026-11-11: 18 of 20 claims verified, 2 not. The full table is in `verification.md`.
Read these corrections before relying on the numbers below. The body of the report is unchanged.

- Confirmed with the cause demonstrated: flat is not a pass-through on arm64. A float32 model with the binary's fused multiply-add pattern gives -99.4 / -89.3 / -80.2 dB against the measured -99.5 / -89.5 / -80.3 dB. Without fusing it is bit-exact except at 44.1 kHz (-125 dB). The -70 dBc at bass 10 is float state rounding.
- C03 and C04: Solid EQ's 20 ms ramps and per-sample redesign exist in its code, but the shipped plugin turns parameter smoothing off in its constructor. A gain step lands within 1 ms.
- Solid EQ does not skip flat bands. They get identity coefficients and still run.
- R1 still follows from our own click finding (-62.5 dBFS, a model result, not measured on the plugin), but "both NI products ramp" is false for Solid EQ and unmeasured for Guitar Rig. Bit-identical static renders are untested.
- R2's "as Solid EQ does" is wrong, and cheaper fixes for the flat residual are not weighed.
- The G1 formula is not in Vicanek's equations 13 to 18.

Binaries: SE = `Solid EQ`, GR = `Guitar Rig 6`, AT = `AmpliTube 5`, TB = our own built `tubamp`
(`build/tubamp_artefacts/Release/AU/tubamp.component`). The first three live under
`/Library/Audio/Plug-Ins/Components/<name>.component/Contents/MacOS/`. Evidence files are in
`evidence/`; reproducer scripts are in `evidence/scripts/`. Work files (WAVs, raw dumps, helper
notes) are in `$RE_STATE/work/eq/`. "Half-gain Q" means centre frequency divided by the width
between the two points at half the peak gain in dB, which is the cookbook definition of Q for a bell
[literature: https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html].

## 1. Summary

- Our three-band is correct in shape and does not need decramping: its highest band is a 1.8 kHz shelf and the worst deviation from the analog curve is 0.04 dB at 44.1 kHz [measured: evidence/ours_cramp.tsv].
- It has two real defects. Coefficients are swapped once per block with no smoothing, which leaves clicks at -62 dBFS under a -12 dBFS note while a knob moves [measured: evidence/ours_zipper.tsv]. The single-precision filter is not transparent: at flat settings 99.7 % of samples differ from the input, and the residual is -99 / -89 / -81 dB at 44.1 / 96 / 192 kHz [measured: evidence/tubamp_flat.tsv].
- The best linear reference, Solid EQ, runs double-precision direct form I, designs its bells with Orfanidis' prescribed-Nyquist-gain method, ramps parameters over 20 ms and skips bands that are flat [decompiled: SE NI::DSP::CSL::Biquad::setPEQ 0xed280; SE NI::DSP::CSL::G_EQ::onSetSampleRate 0xef930].
- Analog-modelled EQs (Pultec, Neve, amp tone stacks) are built as circuit state-space models, not as stacked shelves [decompiled: AT IK::KIS::FX::TR5::Pultec::Module::CalculateSystemL 0xfefa30; GR FMVToneStack<float>::updateSystemMatrices 0x5b398c].
- Proposal: smooth the knobs now; add `eq_type` with Classic (today's code, bit for bit), Parametric (same three bands on a double-precision SVF core plus sweepable mid, low cut, high cut, tilt, level), Stack (passive Fender/Marshall/Vox network) and Graphic (seven octave bands).

## 2. tubamp today

Source: `src/dsp/FxBlocks.h:29-53`, `src/dsp/FxBlocks.cpp:20-24,128-197`, `src/Parameters.cpp:95-97,169-176,214-216`,
`src/PluginProcessor.cpp:1241-1246,1392-1401`, and JUCE `juce_dsp/processors/juce_IIRFilter.cpp:208-275`,
`juce_IIRFilter_Impl.h:41-56,138-162`.

| aspect | what we ship | where |
| --- | --- | --- |
| structure | three second-order sections in series: bass, middle, treble. Each is a `juce::dsp::IIR::Filter<float>` inside a `ProcessorDuplicator` | `FxBlocks.h:46-49`, `FxBlocks.cpp:188-194` |
| filter form | transposed direct form II, float coefficients, float state; `snapToZero` on the state once per block | `juce_IIRFilter_Impl.h:138-162` |
| coefficient maths | Audio EQ Cookbook low shelf, peak, high shelf, computed in float and divided by a0 in float | `juce_IIRFilter.cpp:208-275`, `juce_IIRFilter_Impl.h:41-56` |
| bass | low shelf 150 Hz, Q 0.707, 4 dB per knob unit, so -20..+20 dB | `FxBlocks.cpp:20-24,161-167` |
| middle | peak 425 Hz, 3 dB per unit (-15..+15 dB), Q 0.7 when boosting and 1.5 when cutting | `FxBlocks.cpp:169-177` |
| treble | high shelf 1800 Hz, Q 0.707, 2 dB per unit (-10..+10 dB) | `FxBlocks.cpp:179-185` |
| parameters | `eq_bass`, `eq_mid`, `eq_treble` (and `eq2_*`, `eq3_*`, `amp_eq_*`), 0..10, step 0.1, default 5 | `Parameters.cpp:95-97,169-176,214-216` |
| update policy | `setParameters` runs once per audio block, compares each knob with a cached value and writes new coefficients at once. No ramp, no crossfade | `PluginProcessor.cpp:1243,1397`, `FxBlocks.cpp:155-186` |
| users | three EQ block instances and the amp block's tone stack (mono, before stereo expansion) | `PluginProcessor.h:477-479`, `PluginProcessor.cpp:1241-1246` |
| stereo | one coefficient set, one state per channel | `FxBlocks.h:46-49` |
| latency, oversampling, colour | none | `FxBlocks.cpp:188-194` |

Measured on the installed AU through the amp block's tone stack, which is the same class
(a fresh instance runs the amp block alone, `src/dsp/ChainOrder.h:298-301`):

- Response matches the constants: bass 10 gives +19.5 dB at 50 Hz and +10.1 dB at 150 Hz; middle 10 gives +15.0 dB at 425 Hz; treble 10 gives +5.0 dB at 1.8 kHz and +10.0 dB at 10 kHz [measured: evidence/tubamp_fr.tsv].
- Middle is not symmetric by design: at 300 Hz knob 10 gives +11.4 dB and knob 0 gives -7.1 dB [measured: evidence/tubamp_fr.tsv].
- All three at 10 gives +20.1 dB at 50 Hz and +13.5 dB at 300 Hz; the bands add in dB because they are in series [measured: evidence/tubamp_fr.tsv].

Behaviour the brief asked about:

- **Cramping at 44.1 kHz.** The treble shelf differs from its analog prototype by at most 0.04 dB between 20 Hz and 20 kHz at 44.1 kHz, the middle peak by 0.03 dB [measured: evidence/ours_cramp.tsv]. The shelf sits a decade below Nyquist, so bilinear warping has nothing to act on [inferred]. The same cookbook maths at 8 kHz would be off by 2.6 dB (bell, Q 1, +12 dB) or 0.8 dB (shelf) [measured: evidence/ours_cramp.tsv]. Cramping matters for bands we add, not for the bands we have.
- **Clicks when a knob moves.** In a model of the shipped code, a bass sweep from 5 to 10 in 0.5 s under a -12 dBFS 100 Hz sine leaves components above 1 kHz that peak at -62.5 dBFS with 512-sample blocks and -66.7 dBFS with 64-sample blocks [measured: evidence/ours_zipper.tsv]. Each block boundary is a 0.4 dB step in low-frequency gain [inferred: `Parameters.cpp:95`, `FxBlocks.cpp:23`].
- **Flat is not a pass-through.** With all three knobs at 5 the tone stack changes 175929 of 176400 samples at 44.1 kHz. The difference is -99.5 dB below the signal at 44.1 kHz, -89.4 dB at 96 kHz and -80.6 dB at 192 kHz [measured: evidence/tubamp_flat.tsv]. The cause is fused multiply-add: the built filter loop uses `fmadd` [decompiled: TB juce::dsp::IIR::Filter<float>::processInternal 0xe5d5c, instructions 0xe60c4..0xe60dc], and the same C++ compiled with `-ffp-contract=off` is bit-exact at flat on bass and middle while the default build is not [measured: evidence/ours_flat.tsv].
- **Low shelf at high sample rates.** Coefficient rounding moves the bass curve by at most 0.04 dB at 96 kHz and 0.10 dB at 192 kHz [measured: evidence/ours_prec.tsv]. State rounding is the larger effect: with bass at 10 and an 82.4 Hz sine the residual is -89.0 dBc at 44.1 kHz, -77.7 dBc at 96 kHz and -69.6 dBc at 192 kHz on the installed plugin [measured: evidence/tubamp_noise.tsv]. The model predicts -88.6 / -77.4 / -69.4 dBc for the same cases, and -117 / -122 / -102 dBc for a float TPT SVF with the same response [measured: evidence/ours_prec.tsv].

## 3. Reference implementations

### 3.1 Solid EQ (Native Instruments, SSL 4000 E/G model)

DSP class `NI::DSP::CSL::G_EQ`. Annotated excerpts: `evidence/solid_eq_decompiled.txt`.

- **Signal flow.** Second-order sections in series per channel (high-pass, LF, LMF, HMF, HF, low-pass), then one output gain [decompiled: SE G_EQ::processAudio<Modes 12> 0xfd9a4]. Series is confirmed: LMF and HMF boosted together equal the dB sum of each alone to 0.0000 dB [measured: evidence/solid_eq_interact.tsv].
- **Topology and precision.** Direct form I, coefficients and state in double, input converted from float per sample and back at the output [decompiled: SE G_EQ::processAudio<Modes 12> 0xfdc00..0xfdc7c].
- **Flat bands cost nothing.** A 6-bit mode word (five band-active bits and one "parameters moving" bit) selects one of 64 specialised loops [decompiled: SE G_EQ::processAudio 0xf4018]. `setPEQ` and the shelf setters return identity coefficients when gain or frequency is zero [decompiled: SE Biquad::setPEQ 0xed2b0; SE Biquad::setLowShelfEQ 0xed098]. With defaults the plugin is flat to 0.0000 dB [measured: `evidence/scripts/solid_m.py base`].
- **Bell design.** `setPEQ(f, dB, Q)` is Orfanidis' design: it computes G1, the gain of the analog bell at the Nyquist frequency, from `(w0^2 - pi^2)^2` and the bandwidth, then W2, the warped bandwidth, C, D, A, B and the five coefficients [decompiled: SE Biquad::setPEQ 0xed280, G1 at 0xed3e0]. The sequence is the published one [literature: https://www.vicanek.de/articles/BiquadFits.pdf, eqs. 13-16]. Nine of the bell stages use it and one uses a variant, `setPEQ2` [decompiled: SE G_EQ::calcFilters 0xeff3c].
- **Shelf design.** Cookbook shelves with a slope parameter, in double, no Nyquist correction [decompiled: SE Biquad::setLowShelfEQ 0xed080; SE Biquad::setHighShelfEQ 0xed180].
- **E and G are data, not code.** Each knob goes through 11-point splines; there is one spline set per model (`..._E`, `..._G`), cross-coupled ones (`HMF_Q_G_Curve`: Q as a function of gain) and separate ones per sample rate for the top end (`HF_Q_F_Curve_44_E`, `LP1_Freq_Curve_44/_48/_96/_192`) [decompiled: SE symbols at 0x63e220..0x640480; SE G_EQ::calcFilters 0xeff3c calls Spline<11>::evaluate 82 times]. We do not take these tables.
- **G-series shelves are composites.** The LF shelf path calls one shelf and two bells, the HF path one shelf and one bell [decompiled: SE G_EQ::calcFilters call sites 0xf1a28, 0xf1bac, 0xf1dbc; 0xf2348, 0xf2498] [inferred: the grouping follows the spline names LShelf1..3 and HShelf1..2]. Measured result: a G LF boost of +20 dB dips to -1.2 dB at 2 kHz and a G HF boost dips to -1.5 dB at 500 Hz, while E shelves are monotonic [measured: evidence/solid_eq_shelf.tsv].
- **Ranges.** Gain about +-20 dB on every band (+-23.6 dB for the HF bell) [measured: evidence/solid_eq_bands.tsv]. Bell centres: LF 40 to 650 Hz, LMF 220 Hz to 2.2 kHz, HMF 0.7 to 6.9 kHz, HF 1.5 to 21.6 kHz [measured: evidence/solid_eq_ranges.tsv]. High-pass 18 dB/oct, low-pass about 12 dB/oct [measured: evidence/solid_eq_ranges.tsv].
- **Q against gain.** E mode holds half-gain Q nearly constant: 0.48 at +0.8 dB and 0.43 at +20 dB (Q knob centred). G mode widens small moves: 0.29 up to +6 dB, 0.32 at +11 dB, 0.43 at +20 dB [measured: evidence/solid_eq_q.tsv]. G also gives more gain for the same knob travel: 6.1 dB against 4.6 dB at 0.7 [measured: evidence/solid_eq_q.tsv].
- **Symmetry.** Boost and cut are mirror images: residual 0.000 dB at the extremes, 0.055 dB at most at quarter travel [measured: evidence/solid_eq_sym.tsv].
- **Cramping.** An HF bell peaking at 16 kHz with +22.6 dB still has +9.8 dB at 0.495 fs at 44.1 kHz; a cookbook bell has exactly 0 dB at Nyquist [measured: evidence/solid_eq_cramp.tsv]. At 20 kHz the 44.1 kHz and 96 kHz renders differ by 1.3 dB [measured: evidence/solid_eq_cramp.tsv]. The shelves reach full gain at Nyquist at every rate and differ by up to 1.3 dB at 20 kHz between 44.1 and 96 kHz [measured: evidence/solid_eq_cramp.tsv].
- **Smoothing.** A ramp length of `max(1, round(0.02 * fs / blockSize))` control ticks is set at sample-rate change, constant 0.02 at 0xefb18 [decompiled: SE G_EQ::onSetSampleRate 0xef930]. While a ramp runs, the audio loop advances 16 parameter values by their increments every sample and calls `calcFilters` for each one that is moving [decompiled: SE G_EQ::processAudio<Modes 44> 0x10f7d8..0x10fadc].
- **Colour, oversampling, latency.** No nonlinearity in the audio loop [decompiled: SE G_EQ::processAudio<Modes 12> 0xfd9a4]. THD is below -150 dB at +12 dBFS in and with +20 dB boosts [measured: evidence/solid_eq_thd.tsv]. Reported latency 0 [measured: evidence/solid_eq_params.txt].

### 3.2 Guitar Rig 6

Excerpts: `evidence/gr6_decompiled.txt`. Read by a helper, spot-checked by the orchestrator at the addresses marked "checked", then confirmed headless (`evidence/gr6_eq_blackbox.tsv`).

- **Parametric EQ (`NI::DSP::EQ_Param`, two bands).** Gain +-20 dB, frequency 20 Hz to 20 kHz on a pitch (log) taper, bandwidth knob 0.1..6 mapped through `B = 10^(b/20)` [decompiled: GR EQ_Param::setGain_1 0x26c9f8; EQ_Param::setFreq_1 0x26dfc0; EQ_Param::setBandwidth_1 0x26e1fc]. Measured: +20.00 and -20.00 dB at the ends, +10.00 dB at three-quarter travel, centre 633 Hz at mid travel, half-gain Q from 0.67 to about 22 [measured: evidence/gr6_eq_blackbox.tsv].
- The design uses `w = pi * f / fs` directly where the bilinear transform needs `tan(pi f / fs)`: no pre-warping and no Nyquist correction [decompiled: GR EQ_Param::setFilter 0x27988c, constant 6.2831855 at 0x279918, divide by 2 fs at 0x279948; checked]. The centre therefore lands at `(fs/pi) * atan(pi f / fs)` [inferred]. Measured: a band set to 20 kHz peaks at 17707 Hz at 96 kHz (predicted 17709) and a band set to 10 kHz at 9691 Hz (predicted 9686) [measured: evidence/gr6_eq_blackbox.tsv].
- At 44.1 and 48 kHz the plugin reports 16 samples of latency, rolls off steeply above 0.45 fs, and the same band peaks at 17388 Hz and 17707 Hz, which is the prediction for twice the host rate: the rack runs at 2 fs at those rates and at fs at 96 kHz [measured: evidence/gr6_eq_blackbox.tsv] [inferred].
- Above 96 kHz the frequency is floored at 25 Hz and above 192 kHz at 50 Hz [decompiled: GR EQ_Param::setFilter 0x2798cc..0x27990c; checked]. That is a guard against low-frequency precision loss in a float filter [inferred].
- Float, transposed direct form II normalised to b0 = 1, NEON, bands in series; the output is `dry + mix * (gain * y - dry)` [decompiled: GR EQ_Param::processBuffer 0x26fdf0..0x26ff14].
- Smoothing: w, k, G, volume and mix ramp linearly over 20 ms and the coefficients are rebuilt every sample while the ramp runs [decompiled: GR EQ_Param::onSetSampleRate 0x27983c; EQ_Param::processBuffer 0x26fdf0].
- **Shelving EQ (`EQ_Shelf`).** Two first-order shelves (low 40 Hz to 2 kHz, high 200 Hz to 10 kHz, +-20 dB) merged into one biquad, mapped with `c = fs/pi`, no pre-warp; here the coefficients themselves are ramped over 20 ms [decompiled: GR EQ_Shelf::setShelfFilter 0x27a600; EQ_Shelf::onControlRate 0x27ad20].
- **Graphic EQ (`EQ_Graphic`).** Eight bands in series, centres spread evenly in pitch between a movable minimum (26 to 622 Hz) and maximum (1.1 to 14.9 kHz), bandwidth equal to the band spacing, gain range adjustable from 1 to 30 dB, constant Q, no ripple compensation [decompiled: GR EQ_Graphic::setFilters 0x2779c8, 1/7 constant at 0x2779d4 checked; EQ_Graphic::setFilter 0x277fe4]. Measured at defaults: centres 129 Hz to 4.04 kHz, +15.5 dB per band, half-gain Q about 2; all eight at maximum give +25.6 dB with 7.5 dB of ripple [measured: evidence/gr6_eq_blackbox.tsv].
- **Tone EQ (`CustomEQ`).** A low-pass and a high-pass one-pole whose outputs are mixed with gains `2L(1 - tone)` and `2L * tone`: a tilt built from two one-poles [decompiled: GR CustomEQ::setParamTone 0x273c9c; CustomEQ::processBuffer 0x2740b8].
- **Amp tone stacks.** One amp ("Tortilla") holds a real network: 266 kOhm, 1.08 MOhm, 25.8 kOhm, 33 kOhm, 500 pF and two 22 nF, solved as a three-state state-space system with the trapezoidal rule [decompiled: GR constants 0x37ece54..0x37ece6c checked; FMVToneStack<float>::updateSystemMatrices 0x5b398c]. Those values are a Marshall-type stack [inferred]. Another (`RigJCM`) is heuristic: bass moves a one-pole corner from 73 to 23 Hz, and all three setters rebuild shared mixing gains, so the knobs interact without any circuit [decompiled: GR RigJCM::setParamBass 0x2cc488; RigJCM::setParamMid 0x2cc6a8; RigJCM::setParamTreble 0x2cc750].
- No oversampling inside any of these classes [decompiled: GR EQ_Param::processBuffer 0x26fdf0; EQ_Graphic::processBuffer 0x278c30]; the 2x rate above is applied to the whole rack.

### 3.3 AmpliTube 5

Excerpts: `evidence/at5_decompiled.txt`. Read by a helper; tables, the crossfade and the Pultec constants spot-checked. Static only: the plugin crashes under `auhost`.

- The `IK::Comp::*` MAFProfile constructors only wire parameters; the EQ DSP is in `IK::KIS::FX::*` built on `IK::KIS::TK::Filter` cookbook makers and `TK::StateSpace` / `TK::KMethod` solvers [decompiled: AT StompGraphicEq::MAFProfile 0xa7229c; TK::Filter::MakeRBJPeaking 0x10328d8].
- **Graphic EQs.** One module serves the 7-band stomp (100, 200, 400, 800, 1600, 3200, 6400 Hz), the 10-band stomp (31 Hz to 16 kHz in octaves) and the 31-band rack (ISO third octaves) [decompiled: AT float tables 0x26af59c, 0x26af5b8, 0x26af520; checked]. Q is 1 for the octave units and 4 for the third-octave unit [decompiled: AT StompModule_7Bands 0xcec4cc; RackModule_31Bands 0xcebd64].
- Each band is a cookbook bell with the cookbook bandwidth correction `Qd = 1 / (2 sinh(asinh(1/(2Q)) * w0 / sin w0))`; nothing prescribes the Nyquist gain [decompiled: AT GraphicEq::BandFilter::SetQ 0xceb5b4; checked].
- Bands in series, transposed direct form II, float; a band gain change recomputes coefficients at once with no ramp [decompiled: AT DynamicWrapper<StompModule_10Bands>::ProcessBuffer 0x13dfb78; GraphicEq SetBandBoost 0xcec3bc].
- **MXR six-band model.** Five cookbook bells at 100..1600 Hz with Q 1.41, a fixed and a variable first-order shelf, and a one-pole low-pass at min(0.45 fs, 3150 Hz) [decompiled: AT MXR6BandsEQ::Module 0xed8084, 0xed8230].
- **Rack parametric.** Three bands per channel, eight filter types, each through `FilterOrder2XFade`, which moves every coefficient linearly from old to new, one step of 1/N per sample [decompiled: AT ParametricEQ::Rack::Process 0xf54c28; TK::Filter::FilterOrder2XFade::Process 0x1033848, blend at 0x10338e0..0x1033938 checked].
- **Parametric EQ 2.** Frequency `20 + 19980 * p^4.35`, gain `30p - 15` dB, Q `8 p^3` clamped to 0.01..100 [decompiled: AT ATPostParametricEQ::UpdateParameters 0xfa902c].
- **Generic biquad.** Direct form I with double coefficients and float state [decompiled: AT AudioEngine::BiquadFilter::ProcessBuffer 0x1147ea0].
- **Pultec EQP-1A model.** Component tables (capacitors and inductors per switch position) feed one closed-form five-state continuous model, bilinear-transformed in double; boost and attenuation are one network [decompiled: AT float table 0x3118654 checked; Pultec::Module::CalculateSystemL 0xfefa30, tail call at 0xff06c4]. The process is oversample up, state-space filter, a degree-7 polynomial clamped at +-6.31, oversample down, smoothed output gain [decompiled: AT Pultec::Module::Process 0xff1498, polynomial at 0xff198c..0xff1a28 checked]. The oversampling factor is 1, 2, 4, 8 or 16 [decompiled: AT Pultec::Module::SetSampleRate 0xfef6d4].
- **Neve 1081 model.** Shelf and presence sections are nonlinear K-method solvers with a tabulated amplifier curve; the cut filters are linear state-space sections; the unit is oversampled [decompiled: AT Neve1081::Module::UpdateLow 0xfea160; UpdateHiPresence 0xfeb2f8; FillCurve<KMethodBA338ReferenceFunctor> 0xfe9c28].
- **Marshall tone stack.** An eight-state analytic network, bilinear-transformed in double, with pot taper `0.27 p` below half travel and `p^2.889` above [decompiled: AT UpdateToneEQ 0xd3533c].

### 3.4 SPL Free Ranger (Plugin Alliance, black-box)

Tables: `evidence/freeranger_bands.tsv`.

- Four bells labelled 40, 150, 1k8, 16k; peaks measured at 38 Hz, 141 Hz, 891 Hz and 16.2 kHz with +12.7, +13.2, +11.6 and +14.4 dB at full travel [measured: evidence/freeranger_bands.tsv].
- Strongly proportional Q: the 40 Hz band is 5.1 octaves wide at +0.3 dB and 1.3 octaves at +12.7 dB [measured: evidence/freeranger_bands.tsv].
- Cut is the reciprocal of boost to 0.003 dB [measured: evidence/freeranger_bands.tsv].
- Two adjacent bands at maximum sum to 14.2 to 15.1 dB, less than the dB sum by 2 to 3.7 dB: the bands are not independent series filters [measured: evidence/freeranger_bands.tsv].
- The 16 kHz band at 44.1 kHz matches the 96 kHz render within 0.35 dB to 18 kHz and still has +9.8 dB at Nyquist: not a cookbook bell [measured: evidence/freeranger_bands.tsv; rechecked in evidence/recheck.tsv].
- Flat to 6e-6 dB at defaults, linear (THD below -137 dB at 1 kHz), latency 0 [measured: evidence/freeranger_bands.tsv].

### 3.5 elysia niveau filter (Plugin Alliance, black-box)

Tables: `evidence/niveau_bands.tsv`.

- One tilt control and one frequency control with a x10 switch. The magnitude fits `|H|^2 = (a^2 + b^2 x^2)/(1 + x^2)`, x = f/fc, with a + b = 2: a first-order low-pass weighted a plus a first-order high-pass weighted b [measured: evidence/niveau_bands.tsv]. At the extremes it becomes 2 x low-pass or 2 x high-pass, so the far band falls at 6 dB/oct without limit while the near band sits at +6 dB [measured: evidence/niveau_bands.tsv].
- Corner 40 Hz to 3.4 kHz, ten times that with the switch [measured: evidence/niveau_bands.tsv].
- Rate-independent within 0.2 dB to 18 kHz; a fixed droop of -0.3 dB at 10 kHz and -1.5 dB at 20 kHz; 16 samples of delay at every rate; linear [measured: evidence/niveau_bands.tsv].

### 3.6 bx_console Focusrite SC (Plugin Alliance, black-box)

Tables: `evidence/bxconsole_bands.tsv`, `evidence/bxconsole_bells.tsv`, `evidence/bxconsole_cramp_colour.tsv`.

- Four bands: LF and HF shelves with six switched frequencies each (33 to 330 Hz, 3.3 to 18 kHz), two bells (40 Hz to 1.2 kHz and 600 Hz to 18 kHz with x3 switches), Q 0.7 to 3; third-order high-pass and low-pass [measured: evidence/bxconsole_bands.tsv].
- Constant Q: half-gain Q stays within 0.47..0.53 from +2.5 to +14.5 dB [measured: evidence/bxconsole_bells.tsv].
- Plain bilinear at the top: the HMF bell set to 18 kHz peaks near 14 kHz at 44.1 kHz and falls to +5.0 dB at 20 kHz, against +14.1 dB at 20 kHz in the 96 kHz render [measured: evidence/bxconsole_cramp_colour.tsv; rechecked in evidence/recheck.tsv].
- Colour sits before the EQ: third-harmonic distortion below about 200 Hz that rises steeply with level (-67 dB at 100 Hz and 0 dBFS, -22 dB at 50 Hz), and a noise floor of -125 dBFS with silent input [measured: evidence/bxconsole_cramp_colour.tsv].

### 3.7 UADx (PACE-protected, black-box only)

Tables: `evidence/uad_eqp1a.tsv`, `evidence/uad_neve1073.tsv`, `evidence/uad_meq5.tsv`, `evidence/uad_hlf3c.tsv`, `evidence/uad_cramping.tsv`, `evidence/uad_colour.tsv`.

- **Pultec EQP-1A.** Low boost reaches +16 dB and low attenuation -20 dB at every frequency setting; only the corner moves, and the attenuation corner sits higher than the boost corner (half-gain point 246 Hz against 144 Hz at the 60 Hz setting) [measured: evidence/uad_eqp1a.tsv].
- With boost and attenuation both at 10 on the 60 Hz setting the curve is +15.2 dB at 20 Hz, +12.1 dB at 60 Hz, 0 dB near 270 Hz, -6.5 dB at 500 Hz and -1 dB at 2 kHz. Boost alone gives +13.3 dB at 60 Hz and cut alone -18.2 dB, so the pair is not a dB sum: it is one interacting network [measured: evidence/recheck.tsv; evidence/uad_eqp1a.tsv].
- High boost peak depends on the bandwidth knob, not the frequency: +19.9 dB sharp, +13.6 dB mid, +10.9 dB broad [measured: evidence/uad_eqp1a.tsv].
- **Neve 1073.** High shelf +18.7 / -19.4 dB; mid bell about +18 dB with half-gain Q that rises with gain (0.33 at +7 dB, 0.54 at maximum, 360 Hz setting) and with frequency (0.54 at 360 Hz, 1.03 at 7.2 kHz); high-pass 18 dB/oct [measured: evidence/uad_neve1073.tsv].
- **MEQ-5.** Peaks of +10.6 and +8.8 dB and a dip of -10.8 dB; three bands together depart from the dB sum by at most 1.6 dB [measured: evidence/uad_meq5.tsv].
- **HLF-3C.** 18 dB/oct low and high cuts, independent of each other within 0.4 dB [measured: evidence/uad_hlf3c.tsv].
- **No cramping.** The EQP-1A 16 kHz boost and the 1073 high shelf are the same at 44.1, 48 and 96 kHz within 0.2 dB up to 20 kHz [measured: evidence/uad_cramping.tsv; rechecked in evidence/recheck.tsv]. All four units delay the signal by 86 samples at 48 kHz while reporting zero latency, which fits an internal resampler [measured: evidence/uad_eqp1a.tsv] [inferred].
- **Colour.** EQP-1A flat: THD 0.0016 % at -18 dBFS and 0.032 % at 0 dBFS at 1 kHz, second harmonic leading at low level and third above -6 dBFS; with a +9 dB low boost, 0.43 % at 100 Hz and 0 dBFS [measured: evidence/uad_colour.tsv]. HLF-3C is linear [measured: evidence/uad_colour.tsv].
- **Gaps.** API Vision Channel Strip and Hitsville EQ list parameters but never return from `auhost render` [measured: evidence/uad_gaps.tsv].

## 4. Measurements

**Table 1. tubamp three-band: deviation from the analog prototype, dB** [measured: evidence/ours_cramp.tsv]

| case | fs | 5 kHz | 10 kHz | 15 kHz | 20 kHz | max 20 Hz..20 kHz |
| --- | --- | --- | --- | --- | --- | --- |
| treble shelf 1.8 kHz, +10 dB (shipped) | 44.1 k | 0.03 | 0.01 | 0.00 | 0.00 | 0.04 |
| middle peak 425 Hz, +15 dB (shipped) | 44.1 k | -0.03 | -0.03 | -0.03 | -0.02 | 0.03 |
| cookbook bell 8 kHz, Q 1, +12 dB (not shipped) | 44.1 k | -0.82 | -1.18 | -2.52 | -2.41 | 2.58 |
| cookbook bell 8 kHz, Q 1, +12 dB (not shipped) | 96 k | -0.17 | -0.21 | -0.52 | -0.60 | 0.60 |
| cookbook shelf 8 kHz, +10 dB (not shipped) | 44.1 k | -0.33 | 0.56 | 0.70 | 0.30 | 0.83 |

**Table 2. tubamp tone stack: residual, dB relative to the signal** [measured: evidence/tubamp_flat.tsv, evidence/tubamp_noise.tsv]

| setting | input | 44.1 kHz | 96 kHz | 192 kHz |
| --- | --- | --- | --- | --- |
| flat (5/5/5), on against off | white noise | -99.5 | -89.4 | -80.6 |
| flat | 82.4 Hz sine | -99.0 | -88.6 | -79.2 |
| bass 6 (+4 dB) | 82.4 Hz sine | -96.1 | -85.5 | -77.1 |
| bass 10 (+20 dB) | 82.4 Hz sine | -89.0 | -77.7 | -69.6 |
| bass 0 (-20 dB) | 82.4 Hz sine | -89.7 | -80.1 | -70.7 |
| tone stack off (method floor) | 82.4 Hz sine | -136.2 | -134.3 | -132.4 |

**Table 3. Zipper noise in a model of the shipped code and of candidate fixes** [measured: evidence/ours_zipper.tsv].
Bass 5 to 10 in 0.5 s, 48 kHz, -12 dBFS sine; content above 10x the sine frequency (100 Hz case) or above 4 kHz (1 kHz case).

| variant | 100 Hz: peak dBFS | 100 Hz: RMS dBFS | 1 kHz: peak dBFS |
| --- | --- | --- | --- |
| shipped, 512-sample blocks | -62.5 | -86.1 | -77.8 |
| shipped, 64-sample blocks | -66.7 | -93.0 | -83.3 |
| shipped biquad, 10 ms one-pole on the knob, coefficients every 16 samples | -91.7 | -108.9 | -104.6 |
| shipped biquad, two 5 ms one-poles, every 16 samples | -93.4 | -109.4 | -106.7 |
| shipped biquad, 10 ms one-pole, every sample | -102.9 | -119.3 | -128.8 |
| TPT SVF retuned in steps every 16 samples | -56.8 | -72.8 | -83.1 |
| TPT SVF, coefficients interpolated per sample, 10 ms one-pole | -67.1 | -95.1 | -111.0 |
| TPT SVF, coefficients interpolated per sample, two 5 ms one-poles | -93.9 | -118.6 | -126.5 |

The SVF rows carry a design lesson: its output mix applies gain changes directly, so a stepped or kinked gain trajectory shows up in the output, while the biquad's recursion low-passes it [inferred].

**Table 4. Solid EQ, HMF bell, half-gain Q against gain (Q knob centred)** [measured: evidence/solid_eq_q.tsv]

| gain knob | E gain dB | E Q | G gain dB | G Q |
| --- | --- | --- | --- | --- |
| 0.55 | 0.84 | 0.48 | 0.82 | 0.29 |
| 0.7 | 4.55 | 0.48 | 6.07 | 0.29 |
| 0.8 | 9.10 | 0.47 | 11.12 | 0.32 |
| 0.9 | 16.98 | 0.44 | 17.69 | 0.38 |
| 1.0 | 20.22 | 0.43 | 20.22 | 0.43 |

**Table 5. Response near 20 kHz at 44.1 kHz and 96 kHz, top band at full boost, dB**

| unit and band | fs | 10 k | 14 k | 16 k | 18 k | 20 k | verdict | evidence |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Solid EQ HF bell (peak 16 kHz) | 44.1 k | 9.65 | 19.21 | 22.63 | 15.69 | 11.32 | decramped, +9.8 dB at Nyquist | evidence/solid_eq_cramp.tsv |
| Solid EQ HF bell (peak 16 kHz) | 96 k | 8.81 | 19.16 | 22.71 | 16.66 | 12.66 | | evidence/solid_eq_cramp.tsv |
| SPL Free Ranger 16k | 44.1 k | 5.12 | 11.96 | 14.37 | 12.93 | 10.72 | decramped or oversampled | evidence/recheck.tsv |
| SPL Free Ranger 16k | 96 k | 5.06 | 12.05 | 14.22 | 13.26 | 11.39 | | evidence/recheck.tsv |
| UADx EQP-1A 16 kHz sharp | 44.1 k | 6.87 | 16.81 | 18.92 | 13.67 | 10.09 | rate-independent | evidence/recheck.tsv |
| UADx EQP-1A 16 kHz sharp | 96 k | 6.84 | 16.67 | 19.06 | 13.88 | 10.27 | | evidence/recheck.tsv |
| bx_console HMF x3 at 18 kHz | 44.1 k | 12.15 | 14.27 | 12.98 | 9.87 | 4.96 | plain bilinear | evidence/recheck.tsv |
| bx_console HMF x3 at 18 kHz | 96 k | 11.03 | 13.47 | 14.09 | 14.31 | 14.14 | | evidence/recheck.tsv |

**Table 6. UADx Pultec EQP-1A, 60 Hz setting, 48 kHz, dB relative to flat** [measured: evidence/recheck.tsv]

| setting | 20 | 60 | 100 | 200 | 300 | 500 | 800 | 1 k | 2 k |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| boost 10 | 15.98 | 13.27 | 10.51 | 6.17 | 3.90 | 1.85 | 0.81 | 0.52 | 0.11 |
| attenuation 10 | -19.99 | -18.23 | -15.99 | -11.61 | -8.73 | -5.34 | -2.89 | -2.04 | -0.55 |
| both 10 | 15.18 | 12.13 | 8.92 | 2.95 | -1.51 | -6.51 | -4.82 | -3.42 | -0.95 |

**Table 7. Guitar Rig 6 EQ Parametric, band 1 at +20 dB: where the peak lands** [measured: evidence/gr6_eq_blackbox.tsv]

| host fs | knob | nominal Hz (code) | measured peak Hz | predicted without pre-warp |
| --- | --- | --- | --- | --- |
| 96 k | 0.9 | 10024 | 9691 | 9686 |
| 96 k | 1.0 | 20000 | 17707 | 17709 |
| 48 k | 1.0 | 20000 | 17707 | 17709 (at 96 k internal) |
| 44.1 k | 1.0 | 20000 | 17388 | 17378 (at 88.2 k internal) |

**Table 8. Candidate designs for tubamp (our own numerics)**

| check | result | evidence |
| --- | --- | --- |
| TPT SVF bell / low shelf / high shelf against the cookbook biquad | identical, 0.00000 dB closed form, at most 0.0005 dB from a time-domain run | evidence/design_svf.tsv |
| Orfanidis bell run as SVF parameters against the Orfanidis biquad | identical, 0.000000 dB | evidence/design_orfsvf.tsv |
| bell 100 Hz..8 kHz, +-15 dB, 44.1 kHz, worst error against analog: Q 0.5 / 0.7 / 1 / 2 / 8 | decramped 1.91 / 0.94 / 0.52 / 0.33 / 0.07 dB; cookbook 6.38 / 4.80 / 3.29 / 1.88 / 1.46 dB | evidence/design_orfsvf.tsv |
| same at Q 0.3 | decramped 12.75 dB, cookbook 8.16 dB: the method fails when the band edge passes Nyquist | evidence/design_orfsvf.tsv |
| Yeh closed-form tone stack against an independent nodal solve | 0.000000 dB at interior knob settings | evidence/design_stack.tsv |
| tone stack at noon: dip frequency and depth | tweed 773 Hz / 9.7 dB, blackface 409 Hz / 15.9 dB, Marshall 731 Hz / 6.3 dB, Vox 664 Hz / 18.4 dB | evidence/design_stack.tsv |
| tone stack, bilinear third order at 44.1 kHz, worst error to 10 kHz over a 5x5x5 knob grid | 1.6 dB (0.3 dB at 96 kHz) | evidence/design_stack.tsv |
| seven octave bells, all at +12 dB, Q 1.0 / 1.41 / 2.0 | peak 22.4 / 18.3 / 15.6 dB, ripple 5.5 / 5.7 / 7.0 dB | evidence/design_graphic.tsv |
| first-order tilt, pivot 700 Hz, +-6 dB, 44.1 kHz | worst error against analog 0.03 dB | evidence/design_tilt.tsv |

## 5. Industry practice

What the references agree on:

1. **Linear EQs are series cascades of second-order sections with cookbook-class maths.** Solid EQ shelves, every AmpliTube graphic and parametric band and the Guitar Rig bands are bilinear designs of the textbook analog bell and shelf [decompiled: SE Biquad::setLowShelfEQ 0xed080; AT TK::Filter::MakeRBJPeaking 0x10328d8; GR EQ_Param::setFilter 0x27988c]. What differs is precision, Nyquist handling and smoothing.
2. **Precision goes to double where it matters.** Solid EQ keeps coefficients and state in double [decompiled: SE G_EQ::processAudio<Modes 12> 0xfdc00]. AmpliTube's generic biquad keeps double coefficients [decompiled: AT AudioEngine::BiquadFilter::ProcessBuffer 0x1147ea0]. Guitar Rig stays in float and instead refuses low frequencies at high sample rates [decompiled: GR EQ_Param::setFilter 0x2798cc]. Direct forms lose precision as the cutoff falls relative to the sample rate, and the trapezoidal SVF keeps its behaviour under modulation [literature: https://www.discodsp.net/VAFilterDesign_2.1.2.pdf].
3. **Top-end accuracy splits the field.** Solid EQ prescribes the Nyquist gain [decompiled: SE Biquad::setPEQ 0xed3e0]. Free Ranger, niveau and the UADx units are rate-independent to 18 kHz or better [measured: evidence/recheck.tsv; evidence/niveau_bands.tsv]. Guitar Rig runs the rack at twice the host rate at 44.1 and 48 kHz and does not even pre-warp [measured: evidence/gr6_eq_blackbox.tsv]. The AmpliTube stomp EQs and bx_console are plain bilinear [decompiled: AT GraphicEq::BandFilter::SetQ 0xceb5b4] [measured: evidence/recheck.tsv]. The three published fixes are Orfanidis' prescribed Nyquist gain, Vicanek's matched designs, and oversampling; Vicanek notes that oversampling works at a high CPU cost [literature: https://www.vicanek.de/articles/BiquadFits.pdf]. The Orfanidis bell needs its centre at least half a bandwidth below Nyquist [literature: https://www.vicanek.de/articles/BiquadFits.pdf, section 3.3].
4. **Knob moves are smoothed in the better units.** Native Instruments ramps parameters over 20 ms and redesigns per sample [decompiled: SE G_EQ::onSetSampleRate 0xef930; GR EQ_Param::onSetSampleRate 0x27983c]. AmpliTube's rack parametric crossfades coefficients per sample [decompiled: AT FilterOrder2XFade::Process 0x1033848]. AmpliTube's stomp graphic EQ does neither [decompiled: AT GraphicEq SetBandBoost 0xcec3bc].
5. **Flat means flat.** Solid EQ drops flat bands from the loop [decompiled: SE G_EQ::processAudio 0xf4018], and Free Ranger is flat to 6e-6 dB at defaults [measured: evidence/freeranger_bands.tsv].
6. **Q against gain is a voicing choice.** Constant Q: SSL E model, bx_console, Guitar Rig, all graphic EQs read [measured: evidence/solid_eq_q.tsv; evidence/bxconsole_bells.tsv; evidence/gr6_eq_blackbox.tsv] [decompiled: AT StompModule_7Bands 0xcec4cc]. Proportional Q (wide for small moves, narrower for large): SSL G model, Free Ranger, Neve 1073 [measured: evidence/solid_eq_q.tsv; evidence/freeranger_bands.tsv; evidence/uad_neve1073.tsv]. Rane describes the two families the same way [literature: https://www.ranecommercial.com/legacy/note122.html].
7. **Graphic EQs are constant-Q bells in series, no ripple correction.** Q 1 for octave bands in AmpliTube, 1.41 in its MXR model, bandwidth equal to spacing in Guitar Rig [decompiled: AT StompModule_7Bands 0xcec4cc; AT MXR6BandsEQ::Module 0xed8084; GR EQ_Graphic::setFilters 0x2779c8]. Guitar Rig's eight bands at maximum ripple by 7.5 dB [measured: evidence/gr6_eq_blackbox.tsv]. Adjacent constant-Q bands boosted together ripple [literature: https://www.ranecommercial.com/legacy/note101.html].
8. **Analog character is modelled as circuits.** Pultec and Neve models and amp tone stacks are state-space networks built from component values, with oversampling and a nonlinearity only where the hardware has an amplifier stage [decompiled: AT Pultec::Module::Process 0xff1498; AT Neve1081::Module::UpdateLow 0xfea160; GR FMVToneStack<float>::updateSystemMatrices 0x5b398c]. Plain EQs are strictly linear [measured: evidence/solid_eq_thd.tsv; evidence/freeranger_bands.tsv].
9. **The passive amp tone stack is a third-order network with interacting controls.** Yeh and Smith give its exact transfer function and bilinear discretisation, verified against SPICE [literature: https://ccrma.stanford.edu/~dtyeh/papers/yeh06_dafx.pdf]. Its controls cannot boost the mids, bass swings about 20 dB at 81 Hz and treble about 26 dB at 6 kHz [literature: https://duncanamps.com/technical/tonestack.html].
10. **The Pultec low band is one network.** Boost and attenuation share a selector but have different corners, so using both gives a shelf with a dip above it [measured: evidence/recheck.tsv] [literature: https://accessanalog.com/pultec-eqp1a-passive-eq-explained-2/].

What a guitarist needs from an EQ block next to a neural amp:

- Before the amp, EQ sets how the amp distorts: a low cut near 80 to 150 Hz tightens palm mutes and a mid bell at 700 Hz to 1.2 kHz is the "mid push" of an overdrive pedal [guess]. After the amp, EQ is tone shaping: a high cut removes fizz, a narrow cut removes a resonance, a graphic pedal reshapes mids [guess].
- The pedal graphic EQ convention is octave bands from 100 Hz upward; AmpliTube models seven bands 100 Hz to 6.4 kHz and six bands 100 Hz to 3.2 kHz with +-18 dB [decompiled: AT float table 0x26af59c] [literature: https://www.zzounds.com/a--3994346/item--MXRM109S].
- Amp-style players expect bass, middle and treble that interact, with a mid scoop at noon; that behaviour comes only from the passive network [literature: https://ccrma.stanford.edu/~dtyeh/papers/yeh06_dafx.pdf].

## 6. Gap analysis

| aspect | ours | references | consequence for the player |
| --- | --- | --- | --- |
| knob smoothing | none; coefficients step once per block | 20 ms parameter ramp with per-sample redesign (Solid EQ, Guitar Rig), per-sample coefficient crossfade (AmpliTube rack) | audible clicks and "zipper" when turning or automating a knob under a sustained note |
| numeric precision | float TDF-II; residual -99 dB (44.1 kHz) to -70 dB (192 kHz, bass 10) | double state (Solid EQ); SVF form in the literature | a noise-like residual that a high-gain amp after the EQ raises; worst at high sample rates |
| flat setting | not a pass-through | flat bands skipped (Solid EQ), exact at defaults (Free Ranger) | inserting an EQ block changes the signal before any knob is touched |
| cramping | none measurable for our three bands | matters above about 4 kHz; fixed in Solid EQ, Free Ranger, UADx | nothing today; any high bell we add would sound dull and narrow at 44.1 kHz without a fix |
| band set | fixed bass shelf, fixed 425 Hz mid, fixed 1.8 kHz shelf | sweepable mids with Q (all channel EQs), 18 dB/oct or 12 dB/oct cut filters (Solid EQ, 1073, HLF-3C) | cannot move the mid, cannot tighten lows before the amp or remove fizz after it |
| amp-style stack | three independent bands | circuit tone stacks (Guitar Rig, AmpliTube) | knobs do not interact and there is no mid scoop at noon; it does not feel like an amp |
| graphic EQ | none | 7, 10, 31 bands (AmpliTube), 8 bands (Guitar Rig) | no pedal-style mid shaping |
| tilt | none | niveau (first-order tilt), Guitar Rig Tone EQ | no one-knob brighter/darker control |
| Q law | fixed; asymmetric on middle (0.7 boost, 1.5 cut) | constant or proportional, selectable in Solid EQ | acceptable; our asymmetry already follows the "boost wide, cut narrow" habit |
| output level | none | output gain on every reference | level jumps when EQ is engaged with large boosts |
| colour | none | none in linear EQs; only in hardware models | no gap: tubamp has drive and amp blocks for colour |
| latency | 0 | 0 for linear EQs; 16 samples (niveau, Guitar Rig below 50 kHz), 86 samples (UADx) | no gap |

## 7. Recommendations

Ranked by audible benefit against cost. Parameter ids are appended; `eq_bass`, `eq_mid`, `eq_treble` and their
`eq2_`, `eq3_`, `amp_eq_` forms are untouched. "Classic" below always means today's code path.

### R1. Smooth the knobs (all types, including Classic)

- **What.** Ramp each knob value and redesign coefficients at a short control interval while it moves.
- **Evidence.** Shipped clicks peak at -62.5 dBFS; the same biquad with a smoothed knob and a 16-sample redesign interval reaches -93.4 dBFS [measured: evidence/ours_zipper.tsv]. Both Native Instruments products ramp over 20 ms [decompiled: SE G_EQ::onSetSampleRate 0xef930; GR EQ_Param::onSetSampleRate 0x27983c].
- **Design.**

```
per band: target v_t (read once per block), smoother states s1, s2
a = 1 - exp(-1 / (0.005 * fs))                     // two cascaded 5 ms one-poles
prepare(), reset(), first call after either: s1 = s2 = v_t; coeffs = design(v_t)
per sample:  s1 += a * (v_t - s1);  s2 += a * (s1 - s2)
every 16th sample, if |s2 - v_t| > 1e-4:  coeffs = design(s2)
             else if moving:              s1 = s2 = v_t; coeffs = design(v_t); moving = false
```

  `design()` is the existing factory call with the existing constants, so the settled coefficients are the ones we compute today. The two-pole smoother has no slope jump, which the SVF core of R2 needs (Table 3).
- **Parameters.** None.
- **Cost.** One design (a sin, a cos, a sqrt, a pow) per moving band per 16 samples; nothing when static. Latency 0.
- **Compatibility.** A session whose knobs do not move renders bit-identically, because the smoother starts at the target and the static coefficients are unchanged. Only transitions differ.
- **Proof.** `ours zip` plus `zip_an.py` on the new code: peak above 1 kHz at or below -90 dBFS for the 100 Hz case (shipped: -62.5). Null test of a static render against the current build: zero differing samples.

### R2. New core and `eq_type`; type 1 "Parametric"

- **What.** A double-precision trapezoidal SVF section used by every new type, and a type switch. Parametric is the three shipped bands with the same constants plus a sweepable middle, low cut, high cut, tilt and output level.
- **Evidence.** Residual of the shipped filter: Table 2. A float SVF with the same response is at least 25 dB cleaner in the model [measured: evidence/ours_prec.tsv]; Solid EQ uses double state [decompiled: SE G_EQ::processAudio<Modes 12> 0xfdc00]. The SVF mix forms reproduce the cookbook responses exactly [measured: evidence/design_svf.tsv]. Sweepable mids and cut filters are on every channel EQ studied (3.1, 3.6, 3.7). The SVF equations are Simper's [literature: https://cytomic.com/files/dsp/SvfLinearTrapOptimised2.pdf].
- **Design.**

```
section state: double ic1, ic2;  parameters: g, k, m0, m1, m2
tick(x):  a1 = 1 / (1 + g*(g + k));  a2 = g*a1;  a3 = g*a2
          v3 = x - ic2;  v1 = a1*ic1 + a2*v3;  v2 = ic2 + a2*ic1 + a3*v3
          ic1 = 2*v1 - ic1;  ic2 = 2*v2 - ic2
          return m0*x + m1*v1 + m2*v2
A = 10^(dB/40),  t = tan(pi*f/fs)
bell:        g = t,          k = 1/(Q*A),  m = (1, k*(A*A - 1), 0)
low shelf:   g = t/sqrt(A),  k = 1/Q,      m = (1, k*(A - 1), A*A - 1)
high shelf:  g = t*sqrt(A),  k = 1/Q,      m = (A*A, k*(1 - A)*A, 1 - A*A)
low cut:     g = t,  k = sqrt(2),  m = (1, -k, -1)      // 12 dB/oct Butterworth high-pass
high cut:    g = t,  k = sqrt(2),  m = (0, 0, 1)        // 12 dB/oct Butterworth low-pass
tilt (first order, pivot fp = 700 Hz, T = 10^(tilt_dB/20), gp = tan(pi*fp/fs)):
             y = ((T + gp)*x + (gp - T)*x1 - (T*gp - 1)*y1) / (1 + T*gp)
```

  Chain: low cut, bass shelf (150 Hz, Q 0.707, 4 dB/unit), middle bell (`eq_mid_freq`, `eq_mid_q`, 3 dB/unit), treble shelf (1.8 kHz, Q 0.707, 2 dB/unit), tilt, high cut, level. Control values go through the R1 smoother; g, k and m are computed every 16 samples and linearly interpolated per sample (Table 3, last row). A section whose gain is 0 dB, or a cut filter parked at its end stop, is skipped once its ramp has finished, as Solid EQ does. With m = (1, 0, 0) the section returns its input sample unchanged, so flat is an exact pass-through even before the skip [measured: evidence/design_svf.tsv].
  Our constants: 700 Hz pivot is the geometric centre of 80 Hz to 6 kHz; +-6 dB tilt matches the range of the tilt unit measured [measured: evidence/niveau_bands.tsv]; 12 dB/oct cuts match the gentlest reference low-pass [measured: evidence/solid_eq_ranges.tsv]; Q range 0.5 to 8 is the range over which R3 is valid.
- **Parameters** (instance 1; `eq2_`, `eq3_` likewise).

| id | range | default | note |
| --- | --- | --- | --- |
| `eq_type` | Classic, Parametric, Stack, Graphic | Parametric on a fresh instance; Classic when restoring state that has no `eq_type` | same restore pin as the reverb and delay engines |
| `eq_low_cut` | 20..400 Hz, log | 20 (off at the end stop) | |
| `eq_high_cut` | 2000..20000 Hz, log | 20000 (off at the end stop) | |
| `eq_mid_freq` | 100..8000 Hz, log | 425 | |
| `eq_mid_q` | 0.5..8, log | 0.7 | symmetric in boost and cut |
| `eq_tilt` | -6..+6 dB | 0 | |
| `eq_level` | -12..+12 dB | 0 | all new types |

- **Cost.** Six sections of about 12 floating-point operations per sample and channel in the steady state, fewer when sections are skipped; under 0.1 % of a core at 48 kHz stereo [inferred]. Latency 0.
- **Compatibility.** Classic is the current code bit for bit. Parametric at defaults has the Classic response in boost; in cut its middle uses Q 0.7 where Classic uses 1.5. Sessions saved before this change load as Classic.
- **Proof.** (a) Flat null: Parametric at defaults against bypass, zero differing samples at 44.1, 96 and 192 kHz (Classic: 99.7 % differ). (b) `tb.py noise` equivalent: residual at bass 10, 82.4 Hz, 192 kHz at or below -120 dBc (Classic: -69.6). (c) Response at bass/middle/treble 10 within 0.05 dB of `evidence/tubamp_fr.tsv`. (d) Zipper as in R1, at or below -90 dBFS.

### R3. Decramped bell above 2 kHz

- **What.** When the middle bell (R2) or a graphic band (R5) is centred above 2 kHz, design it with Orfanidis' method and run it on the same SVF section.
- **Evidence.** Solid EQ does this for every bell [decompiled: SE Biquad::setPEQ 0xed280] and keeps +9.8 dB at Nyquist where a cookbook bell has 0 dB [measured: evidence/solid_eq_cramp.tsv]. Over our proposed range at 44.1 kHz the worst error falls from 4.8 dB to 0.9 dB at Q 0.7 and from 3.3 dB to 0.5 dB at Q 1 [measured: evidence/design_orfsvf.tsv].
- **Design.** With G0 = 1, G = 10^(dB/20), GB = sqrt(G), w0 = 2 pi f/fs, Dw = w0/Q [literature: https://www.vicanek.de/articles/BiquadFits.pdf, eqs. 13-18]:

```
F = |G^2 - GB^2|;  G00 = |G^2 - 1|;  F00 = |GB^2 - 1|
G1 = sqrt(((w0^2 - pi^2)^2 + G^2 * F00 * pi^2 * Dw^2 / F) / ((w0^2 - pi^2)^2 + F00 * pi^2 * Dw^2 / F))
G01 = |G^2 - G1|;  G11 = |G^2 - G1^2|;  F01 = |GB^2 - G1|;  F11 = |GB^2 - G1^2|
W2 = sqrt(G11 / G00) * tan(w0/2)^2
DW = (1 + sqrt(F00 / F11) * W2) * tan(Dw/2)
C = F11*DW^2 - 2*W2*(F01 - sqrt(F00*F11));  D = 2*W2*(G01 - sqrt(G00*G11))
A = sqrt((C + D) / F);  B = sqrt((G^2*C + GB^2*D) / F)
SVF parameters:  g = sqrt(W2),  k = A/g,  m0 = G1,  m1 = (B - G1*A)/g,  m2 = 1 - G1
```

  The last line is ours: in the bilinear s-plane the design is `(G1 s^2 + B s + W2)/(s^2 + A s + W2)`, which the SVF realises directly. It equals the Orfanidis biquad to 0.000000 dB [measured: evidence/design_orfsvf.tsv]. Below 2 kHz use the plain bell of R2; at 3.2 kHz and Q 1.4 the two differ by 0.3 dB at most at 44.1 kHz [measured: evidence/design_decramp.tsv], so crossfade the parameter sets between 1.5 and 2.5 kHz [guess: the width of the blend is untested]. The Q floor of 0.5 keeps the band edge below Nyquist; at Q 0.3 the method is worse than the cookbook bell [measured: evidence/design_orfsvf.tsv].
- **Parameters.** None.
- **Cost.** Design only: about 10 square roots and 2 tangents per update. No per-sample cost. Latency 0.
- **Compatibility.** New types only.
- **Proof.** Middle at 8 kHz, Q 1, +12 dB at 44.1 kHz: error against the analog bell at or below 0.5 dB from 20 Hz to 20 kHz (cookbook: 2.6 dB), and gain at Nyquist above +1.5 dB (cookbook: 0).

### R4. Type 2 "Stack": passive tone stack (EQ block and amp block)

- **What.** The Fender/Marshall/Vox three-knob network as a third-order filter, with four voicings. `eq_bass`, `eq_mid`, `eq_treble` drive the three pots.
- **Evidence.** Both amp suites model the stack as a circuit [decompiled: GR FMVToneStack<float>::updateSystemMatrices 0x5b398c; AT UpdateToneEQ 0xd3533c]. The transfer function is published and exact [literature: https://ccrma.stanford.edu/~dtyeh/papers/yeh06_dafx.pdf]; our transcription matches an independent nodal solve to 0.000000 dB [measured: evidence/design_stack.tsv]. The helper's first transcription of b3 had an extra term and was 26 dB off at one setting; the version in `evidence/scripts/design.py` (`yeh_coeffs`) is the checked one.
- **Design.** H(s) = (b1 s + b2 s^2 + b3 s^3)/(1 + a1 s + a2 s^2 + a3 s^3) with the coefficient expressions of Yeh and Smith in t (treble), m (middle), l (bass), R1..R4, C1..C3; bilinear with c = 2 fs gives B0..B3, A0..A3 [literature: https://ccrma.stanford.edu/~dtyeh/papers/yeh06_dafx.pdf, eqs. 1-2]. Run as one third-order transposed direct form II section in double. Knob mapping, ours: t = treble/10, m = middle/10, l = taper(bass/10) with `taper(x) = (b^x - 1)/(b - 1)`, b = (1/0.3 - 1)^2, which puts 30 % of the track at half rotation [literature: https://duncanamps.com/technical/tonestack.html]. Smoothing as R1 with coefficients recomputed every 16 samples.

| voicing | R1 treble | R2 bass | R3 mid | R4 slope | C1 | C2 | C3 | makeup dB | source |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Tweed | 250 k | 1 M | 25 k | 56 k | 250 pF | 20 nF | 20 nF | +8.9 | [literature: https://ccrma.stanford.edu/~dtyeh/papers/yeh06_dafx.pdf] |
| Blackface | 250 k | 250 k | 10 k | 100 k | 250 pF | 100 nF | 47 nF | +15.1 | [literature: https://robrobinette.com/How_The_TMB_Tone_Stack_Works.htm] |
| British | 220 k | 1 M | 22 k | 33 k | 470 pF | 22 nF | 22 nF | +6.4 | [literature: https://sources.debian.org/src/caps/0.9.23-1/ToneStack.cc/] |
| Top Boost | 1 M | 1 M | 10 k | 100 k | 50 pF | 22 nF | 22 nF | +13.6 | [literature: https://sources.debian.org/src/caps/0.9.23-1/ToneStack.cc/] |

  Makeup is ours: minus the mean level of the noon response over 80 Hz to 5 kHz on a log axis, so switching the type in does not drop the level [measured: evidence/design_stack.tsv]. The stack has no flat setting; noon is a mid scoop of 6 to 18 dB depending on voicing [measured: evidence/design_stack.tsv].
- **Parameters.** `eq_voice` (Tweed, Blackface, British, Top Boost; default British); for the amp block `amp_eq_type` (Classic, Stack; default Classic) and `amp_eq_voice` (same list).
- **Cost.** Seven multiplies per sample and channel; a coefficient update is about 120 multiplies. Latency 0.
- **Compatibility.** New type; Classic unchanged. The amp block defaults to Classic.
- **Proof.** Response against `design.py stack` (the analog curve) within 1.6 dB to 10 kHz at 44.1 kHz over the 5x5x5 knob grid and within 0.3 dB at 96 kHz [measured: evidence/design_stack.tsv]. Interaction present: British voicing, middle 0 to 10 moves the dip from 852 Hz to 603 Hz [measured: evidence/design_stack.tsv]. Zipper during a bass sweep at or below -85 dBFS; if the third-order direct form cannot reach that, switch to a three-state trapezoidal state-space form as in Guitar Rig [decompiled: GR FMVToneStack<float>::updateSystemMatrices 0x5b398c].

### R5. Type 3 "Graphic": seven octave bands

- **What.** Seven bells at 100, 200, 400, 800, 1600, 3200, 6400 Hz, +-12 dB each, in series, plus `eq_level`.
- **Evidence.** The band set is the pedal convention that AmpliTube models [decompiled: AT float table 0x26af59c]. Series constant-Q bells are what both suites ship [decompiled: AT DynamicWrapper<StompModule_10Bands>::ProcessBuffer 0x13dfb78; GR EQ_Graphic::setFilters 0x2779c8].
- **Design.** R2 bell sections with Q = sqrt(2). That Q is ours: it makes the half-gain bandwidth one octave, equal to the band spacing, since Q = 1/(2 sinh(ln2/2 * BW)) [literature: https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html]. With all bands at +12 dB the sum peaks at 18.3 dB with 5.7 dB of ripple, and one band at +12 dB adds 2.5 dB an octave away; Q 1 would peak at 22.4 dB [measured: evidence/design_graphic.tsv]. The 3.2 and 6.4 kHz bands use R3: at 44.1 kHz the 6.4 kHz band is then within 0.26 dB of analog instead of 1.2 dB [measured: evidence/design_decramp.tsv]. Bands at 0 dB are skipped.
- **Parameters.** `eq_g100`, `eq_g200`, `eq_g400`, `eq_g800`, `eq_g1k6`, `eq_g3k2`, `eq_g6k4`: -12..+12 dB, default 0.
- **Cost.** At most seven sections. Latency 0.
- **Compatibility.** New type.
- **Proof.** Single band at +12 dB: peak 12.0 +-0.1 dB at the nominal centre, half-gain width 1.0 +-0.05 octave. All bands at 0 dB: bit-exact pass-through.

### R6. Optional: proportional Q for the Parametric middle

- **What.** A choice between constant Q and a law that widens small moves, `Q_eff = Q * (0.65 + 0.35 * |gain_dB| / 15)`.
- **Evidence.** The SSL G model's Q at small gains is 0.67 of its Q at full gain (0.29 against 0.43), and the Neve 1073 behaves the same way [measured: evidence/solid_eq_q.tsv; evidence/uad_neve1073.tsv]. The constant 0.65 is ours, set from that ratio.
- **Parameters.** `eq_q_law` (Constant, Proportional; default Constant).
- **Cost.** None. **Proof.** Half-gain Q at +3 dB is 0.72 +-0.03 of the Q at +15 dB. Low priority: a voicing nicety, not a fix.

### Not recommended

- **Oversampling the EQ.** Only the nonlinear hardware models oversample [decompiled: AT Pultec::Module::SetSampleRate 0xfef6d4]; R3 removes the cramping error at design time for no run-time cost.
- **Saturation, transformer or noise modelling.** The linear references are linear to below -137 dB [measured: evidence/solid_eq_thd.tsv; evidence/freeranger_bands.tsv]. tubamp has drive and amp blocks for colour.
- **A Pultec-style interacting low band.** It needs a five-state network to do properly [decompiled: AT Pultec::Module::CalculateSystemL 0xfefa30]; it is a studio tool and the dip it produces (Table 6) can be dialled with the Parametric type.
- **Graphic-EQ ripple correction** (interaction-matrix gain solving). It makes sliders exact for all-band moves [literature: https://acris.aalto.fi/ws/portalfiles/portal/40860405/V_lim_ki_Neurally_Controlle_Graphic_Equalizer.08809819_1.pdf]; no reference in this study does it, and players expect pedal behaviour.

## 8. Open questions and unverified items

- **Classic at flat.** The brief asked for fixes that keep flat settings bit-identical. Today's flat output is not the input (Table 2), so "bit-identical to today" and "transparent" are different targets. R1 keeps Classic bit-identical; R2 makes the new type transparent. Skipping a Classic band whose knob is exactly 5 would make Classic transparent too, at the price of changing old sessions by a -99 dB residual. That is an owner decision; we did not assume it.
- The zipper numbers in Table 3 come from a C++ model of our code (`evidence/scripts/ours.cpp`), not from the plugin: `auhost` cannot move a parameter during a render. The model's static behaviour matches the plugin (flat residual and roundoff, section 2), so the transient numbers are trusted but not directly confirmed.
- The third-order direct form of R4 was checked for response only. Its behaviour under knob movement is untested; the proof step names the fallback.
- The blend of plain and decramped bells around 2 kHz (R3) is a guess.
- The Blackface voicing uses the Bassman topology formula with blackface values. The real circuit wires the middle control as a plain variable resistor, so the curve is close but not exact [inferred].
- Pot tapers of real amps vary by maker and year; the 30 % law is one published convention [literature: https://duncanamps.com/technical/tonestack.html]. AmpliTube uses a different one [decompiled: AT UpdateToneEQ 0xd3533c].
- UADx API Vision Channel Strip and Hitsville EQ hang in `auhost render`; nothing was measured on them [measured: evidence/uad_gaps.tsv]. API proportional-Q behaviour is therefore taken from literature only [literature: https://www.waves.com/api-550-or-api-560-the-differences-explained].
- AmpliTube 5 is static-only (it crashes under `auhost`), so none of its EQ claims has a measurement behind it.
- Guitar Rig 6 became measurable headless late in the study. Only EQ Parametric and EQ Graphic were measured; EQ Shelving, Custom EQ and the amp tone stacks were not.
- Not read: Guitar Rig `Peavey5150::setToneStack` and `SoldanoToneStack<double>`, AmpliTube `RackEqPG`, the AmpliTube oversampler filters (so no latency figure for its Pultec), Solid EQ `setPEQ2` in full, and the contents of the Solid EQ spline tables (by intent).
- Orfanidis' paper itself could not be opened (404 and paywall); the equations are from Vicanek's summary and agree with the Solid EQ code path instruction by instruction [decompiled: SE Biquad::setPEQ 0xed280].
- The helper's literature digest contained unverified items (matched one-pole shelf formulas, the 1/w0^2 noise rule, SSL band ranges). None of them is used in a recommendation.
- Plugin Alliance binaries were not disassembled; all three units are described from measurement only.
