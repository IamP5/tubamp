# Phaser: reverse-engineering report

## Verification status

Independently verified on 2026-11-11, then corrected in two fix rounds. The full table is in `verification.md`.

- 16 of 20 claims verified, 4 not (C07, C09, C12, C19). All four were wrong numbers, not wrong structure. They are corrected in the section "Corrections after verification" at the end of this report and in `claims.tsv`.
- The first prototype missed Guitar Rig 6's Phase 90 sweep by up to 7.9 %, not the 1 % this report first claimed: its targets came from outlier frames of the notch tracker. Two fix rounds refitted the voicings. `voicings.md`, `prototype/phaser_ref.py` and `evidence/fit/` are the current truth; where the body of this report disagrees with them, they win.
- Current accuracy against Guitar Rig 6, from `evidence/fit/compare.txt` after the third fix round (`verification-refit.md` is the independent check of the round-2 voicings; round 3 answers it; the coordinator re-tracked the round-3 prototype with that verifier's own tracker against its stored Guitar Rig takes and got 0.38 to 0.72 % rms and at most 1.24 % at six rates, but no fresh agent has re-verified round 3). Phase 90 block, six rates 0.14 to 9.76 Hz: whole first-notch trajectory over the LFO cycle 0.38 to 0.72 % rms, 1.27 % at worst (round 2: up to 3.87 % rms and 6.5 %); sweep ends within 0.75 %; notch depth within 0.07 dB; resonance −0.06 dB; period exact. Its second notch (not a target) is up to 4.2 % low at the sweep top. Small Stone, Color off, within 1.14 % on the first notch's ends and 1.9 % rms over the trajectory (3.6 % at worst); its second notch sits 6.5 % high at the bottom and 3.7 % high at the top and was left out of the fit. Small Stone, Color on, within 3.03 % on both notches' ends at three rates, except the first-notch minimum at 1.78 Hz (+4.19 %), 1.1 % rms over the trajectory (2.2 % at worst); static response 0.05 to 0.25 dB rms. All at 48 kHz, against Guitar Rig 6 at settings this study chose (Phaser Nine four stages, Color 0.4217; Stoned Phaser Color Strength 0.74), not the components' INIT presets and not a hardware pedal (`voicings.md`, top).
- The Phase 90 script-logo voicing has no measured reference. Guitar Rig's Color 0 keeps its stage-2 gain, so it is not a true script pedal. The script voicing is the block voicing without the feedback path.
- Still guesses: the Phase 90 sweep law's form (its constants are fitted), its 50/50 mix, the 10 kOhm op-amp resistor behind the fitted 25.3 kOhm feedback-resistor equivalent, the 16-sample coefficient update, and the analog meaning of the fitted feedback lag. The Phase 90 LFO curvature is now fitted (a straight triangle); the earlier 0.36 came from an AmpliTube 5 constant and is gone.
- The remaining Color-on gap traces to Guitar Rig running that effect at 96 kHz. Running our stages at 2x would close it; that was not done.
- AmpliTube 5 was read, never measured. No Small Stone schematic could be opened, so the Small Stone constants are fitted to Guitar Rig 6.

Evidence tags and method follow `tools/re/README.md`. Evidence files are in `evidence/`;
the build spec for the two voicings is `voicings.md`; the reference implementation is
`prototype/phaser_ref.py`. "GR6" is Guitar Rig 6, "AT5" is AmpliTube 5.

## 1. Summary

- tubamp ships JUCE's generic six-stage phaser with no feedback, a sine LFO and a 35 % mix; it cannot make a Phase 90 or a Small Stone.
- Both pedals are four equal first-order allpass stages. The Phase 90 block-logo feedback and the Small Stone Color switch are specific loops, and the references agree on them.
- GR6 and AT5 model both pedals as linear filters. The work is in the sweep law, the LFO shape and the feedback loop, not in saturation.
- A numpy prototype of both voicings lands within 1 % (Phase 90) and 3 % (Small Stone) of GR6's first-notch sweep, with our own constants.
- Recommended: two new append-only parameters per mod instance (`mod_phaser_model`, `mod_phaser_color`); model 0 keeps today's sound bit for bit.

## 2. tubamp today

- `Modulation` wraps one `juce::dsp::Phaser<float>` beside the chorus and the tremolo, and passes it rate, depth and mix only [`src/dsp/FxBlocks.h:111-130`, `src/dsp/FxBlocks.cpp:396-400`].
- Centre frequency is fixed at 600 Hz and feedback at 0 in `prepare` [`src/dsp/FxBlocks.cpp:359-360`].
- Parameters are `mod_type` (Chorus, Phaser, Tremolo), `mod_rate` 0.05 to 10 Hz log, default 1 Hz, `mod_depth` 0 to 1, default 0.4, `mod_mix` 0 to 1, default 0.35, repeated for `mod2_*` and `mod3_*` [`src/Parameters.cpp:99-104,178-190`].
- The JUCE class has six stages (`numStages = 6`), each a first-order TPT allpass [`juce_dsp/widgets/juce_Phaser.h:209`, `juce_Phaser.cpp:48`].
- The LFO is a sine, scaled by `depth * 0.5`, added to the normalised centre and mapped to 20 Hz … min(20 kHz, 0.49·fs) on a log scale [`juce_Phaser.cpp:42,149`, `juce_Phaser.h:136-141`].
- The cutoff is updated every fourth sample and held in between (`maxUpdateCounter = 4`) [`juce_Phaser.h:205,164`].
- Feedback is taken from the chain output with a one-sample delay; we set it to 0 [`juce_Phaser.h:158,176`].
- Dry and wet are mixed linearly, `(1 − mix)·dry + mix·wet` [`juce_Phaser.cpp:51`].
- Both channels share one LFO [`juce_Phaser.h:119-128`].
- Computed result at the defaults, 48 kHz: cutoff 151 Hz to 2389 Hz; three notches at 40-640 Hz, 151-2389 Hz and 562-8915 Hz; notch depth −10.5 dB because the mix is 35 % [inferred: evidence/ours_today.tsv].
- At depth 1 the sweep runs 20 Hz to 19 kHz and clips flat at the bottom [inferred: evidence/ours_today.tsv].

## 3. Reference implementations

### 3.1 Guitar Rig 6 "Phaser Nine" (NI::DSP::PhaserMXR), a Phase 90 model

Signal flow, per sample [decompiled: Guitar Rig 6 0x2a9610]; the pseudo-code is in evidence/decompiled_notes.md section 1.

- Input is multiplied by 1.862 (+5.4 dB) and passed through a one-pole lowpass at 16.4 kHz [decompiled: Guitar Rig 6 0x2a8e5c, 0x2a8cd8].
- The LFO is a triangle, `|phase|` with phase in [−1, 1), in double precision; the right channel can be offset by the Rotate parameter [decompiled: Guitar Rig 6 0x2a9744-0x2a97b4].
- The triangle is scaled to a control voltage `V0 − d … V0 + d`, with `V0 = 3.145 + 0.3·(Frequency − 0.5)` and `d = 0.1692·Depth`, then smoothed by a one-pole lowpass at 3.65 Hz [decompiled: Guitar Rig 6 0x2a8fd8, 0x2a8f78, 0x2a8ca4].
- Voltage maps to frequency by a cubic below 3.1929 V and a line above it: 891 Hz at 3.0604 V, 1872 Hz at 3.145 V, 3173 Hz at 3.2296 V [decompiled: Guitar Rig 6 pool 0x2bdf1c8].
- The allpass coefficient is `a = 2πf/fs − 1`, a first-order approximation with no prewarp [decompiled: Guitar Rig 6 0x2a9838].
- Default stage count is 4; the Notches parameter selects 2 to 8 stages [decompiled: Guitar Rig 6 0x1391b8, 0x2a9104].
- Stage 1 is a plain allpass. Stage 2 is `1.2030·AP − 0.2030`: high-frequency gain 1.406, DC gain 1 [decompiled: Guitar Rig 6 pool 0x2be05b0].
- Feedback is taken from the last stage and added before stage 3, with gain `−0.95·Color` and a one-sample delay; default Color is 0.4217 [decompiled: Guitar Rig 6 0x2a9050, 0x1391b8, 0x2a9690].
- Stage 2 plus this loop is the op-amp stage with a feedback resistor into its inverting input: 10 kΩ / 24.6 kΩ gives both the 1.406 and the 0.40 [inferred from the two constants above and Eichas figure 4].
- Output is `dry·x + wet·y4` with dry = 1 − Mix and wet = ±Mix; default Mix 0.5 [decompiled: Guitar Rig 6 0x2a90b4, 0x2a8bcc].
- The Rate knob sets the period, linear in three segments: 30 s at 0, 7 s at 0.1, 1.1 s at 0.4, 0.1025 s at 1 [decompiled: Guitar Rig 6 0x2a89b0].
- Eight parameters are ramped linearly over 20 ms [decompiled: Guitar Rig 6 0x2a8c48, 0x2a936c].
- A high-shelf section (1500 Hz, ×1.74) is computed every sample but its output is never used [decompiled: Guitar Rig 6 0x2a97f4-0x2a9814].
- There is no nonlinear function anywhere in the process loop [decompiled: Guitar Rig 6 0x2a9610].
- The plugin runs this code at twice the host rate at 44.1 and 48 kHz: the measured sweep is the same at 48 and 96 kHz and matches the transcribed code only when the code runs at 96 kHz [inferred from measured: evidence/gr6_phaser_nine_tracks.tsv rows def, sr96, slow; evidence/tools/gr6_mxr_static.py].

### 3.2 Guitar Rig 6 "Stoned Phaser" (NI::DSP::RigPhaserBT), a Small Stone model

`NI::DSP::StonedPhaser` is the same algorithm behind a different interface [decompiled: Guitar Rig 6 0x697eac against 0x2d90b4].

- The Color switch changes three things at once: sweep limits 307 Hz / 8500 Hz off and 93 Hz / 2960 Hz on, feedback from 0 to `strength` (0.74 at init, clamped to ±0.85), and the wet share of the mix [decompiled: Guitar Rig 6 0x2d96a8, 0x2d9eac].
- The sweep is exponential: frequency = √(f1·f2) · 32^x, with 32^x built from a cubic raised to the fourth power [decompiled: Guitar Rig 6 0x2d9774, 0x2da150-0x2da18c].
- There are N = 2 second-order sections, four stages [decompiled: Guitar Rig 6 0x2d9e90, pool 0x2bdf308].
- The feedback loop has no unit delay. The polynomial `1 − c·A^2N` is factored: pole radius `|c|^(1/2N)`, angles `(i + ½[c<0])·π/N`, and each conjugate pair becomes one section [decompiled: Guitar Rig 6 0x2d90d8-0x2d92e0].
- Each section is solved per sample in double precision with one division, using `g = tan(x) ≈ x + x³/3 + 2x⁵/15` [decompiled: Guitar Rig 6 0x2da210-0x2da2c8].
- The input passes a biquad whose analog prototype is built from named resistor values (270 kΩ − 243 kΩ·min(|c|/0.7, 1), 300 kΩ, 100 kΩ, 200 kΩ) and scaled by `1.5·(1 − 0.9|c|)` [decompiled: Guitar Rig 6 0x2d92e4-0x2d9548].
- A second path feeds the previous wet sample, through its own biquad, back to the input with gain −c [decompiled: Guitar Rig 6 0x2d9ea0, 0x2da1ec]. Where that biquad passes, it cancels the analytic feedback, so the effective feedback is band-limited [inferred].
- The Wet knob at 0.5 gives a wet share of `min(1, 0.5 + 0.5|c|)`: 0.5 with Color off, 0.87 at c = 0.74 [decompiled: Guitar Rig 6 0x2d9eac-0x2d9f0c].
- The process loop calls no saturating function [decompiled: Guitar Rig 6 0x2d9f7c].
- The exact biquad coefficients were not decoded; the response was fitted from measurement instead (section 4.4).

### 3.3 AmpliTube 5, MXR Phase 90 (IK::KIS::FX::AT3::MXRPhaser90::Stomp)

The stomp classes (`IK::Comp::StompMXRPhase90::MAFProfile`) only bind parameters; the DSP is a kernel class [decompiled: AmpliTube 5 0xa9f9d0, 0x13d855c].

- The LFO is a ±1 square wave into a one-pole lowpass at 0.1135 × rate, so the shape is the same at every rate: a triangle with exponential edges, peak ±0.1765 [decompiled: AmpliTube 5 0xd89938-0xd89964, 0xd89c44].
- Rate is 0.1 to 10 Hz, with optional tempo sync [decompiled: AmpliTube 5 0x1788d0c, 0xd89bdc].
- Gate voltage is `Vgs = 1.1954·v − 2.3649` volts [decompiled: AmpliTube 5 0xd8986c-0xd89978].
- JFET resistance is `rds = 190.47 Ω / (1 − sqrt(Vgs / −2.7586 V))`, in parallel with 24 kΩ [decompiled: AmpliTube 5 0xd89988-0xd899c4].
- The capacitor is 50 nF and the op-amp resistors 10 kΩ [decompiled: AmpliTube 5 0xd898ac, 0xd898b4].
- The break frequency therefore sweeps 696 Hz to 2077 Hz [inferred: evidence/decompiled_notes.md section 3].
- Stage 1 is a first-order allpass by the bilinear transform without prewarp [decompiled: AmpliTube 5 0xd899d0-0xd89aa0].
- Stages 2 to 4 are one third-order section, `A³/(1 + k·A³)` with k = 0.3678, its six coefficients recomputed in closed form every sample: feedback around three stages with no delay [decompiled: AmpliTube 5 0xd899f0-0xd89b38].
- Output is `1.0·dry + 1.0·wet` [decompiled: AmpliTube 5 0x13d9218, 0xd89b80].
- The signal never enters the JFET law: the model is linear [decompiled: AmpliTube 5 0xd8984c].
- No oversampling; one channel [decompiled: AmpliTube 5 0xd8984c].
- The six fitted values (rate, k, bias, R0, LFO gain, Vp) are registered as module parameters with ranges [decompiled: AmpliTube 5 0x1788ce0], which suggests they were tuned against a unit [inferred].

### 3.4 AmpliTube 5, Small Stone (IK::KIS::FX::AT3::ElectroHarmonixSmallStone::Module)

- Color changes the LFO control range from 0.56-0.67 to 0.581-0.73 [decompiled: AmpliTube 5 0xca31b8].
- Color changes the rate law from `0.071 + 13.43·knob^4.59` Hz to `0.048 + 9.20·knob^4.54` Hz: the same knob position is about 30 % slower with Color on [decompiled: AmpliTube 5 0xca324c, 0xca32d4, pool 0x239e1d8].
- Color selects a different set of filter coefficients [decompiled: AmpliTube 5 0xca2b80].
- Processing is a chain of first-order sections, each run over four sub-samples per input sample: four-times oversampling [decompiled: AmpliTube 5 0xca2560-0xca2594].
- The loop calls no saturating function [decompiled: AmpliTube 5 0xca2390].
- The filter constants in `UpdateFilters` were not decoded.

### 3.5 Phasis (black-box only)

- Parameters: Mode, Notches, Invert, Center, Spread, Feedback, Mix, LFO Sync, Rate, Amount, Mod Mix, Spread Mod Polarity, Stereo [measured: evidence/phasis_params.txt].
- At its defaults it has two notches spaced 5.73 apart, so four equal stages; they sweep 38 Hz to 5.2 kHz and 209 Hz to 21 kHz, more than seven octaves [measured: evidence/phasis_tracks.tsv].
- The sweep is a triangle in log frequency (R² 0.98, against 0.73 for linear frequency), clipped at the bottom [measured: evidence/phasis_tracks.tsv].
- Notches are −31 dB or deeper, so dry and wet are equal [measured: evidence/phasis_tracks.tsv].
- What it adds over a pedal is user control of centre, spread between stages, feedback and stereo offset [measured: evidence/phasis_params.txt]. The code was not read.

### 3.6 Secondary classes (not studied)

AmpliTube also contains `MXRPhaser100`, `FenderPhaser`, `FOXXPhaser`, `UniVibe` and `ST4S::DSP::Phaser` (with an `AnalogLFO` class) kernels [decompiled: AmpliTube 5 symbols at 0xd89f34, 0xccb3b8, 0xe27a44, 0xdc89d4, 0xfa81cc]. They were listed, not read.

### 3.7 The circuits

- Phase 90: four allpass stages, each an op-amp with a JFET in parallel with a fixed resistor at the non-inverting input; `fc = 1/(2π·(Rfix‖Rjfet)·C)` [literature: https://dafx14.fau.de/papers/dafx14_felix_eichas_physical_modeling_of_the_.pdf].
- Dry and allpass output are summed through two resistors at the base of a PNP output transistor [literature: same, figure 4].
- The feedback resistor (R15 in that paper, R28 on the MXR board) goes from the stage-4 output to the R6/R7 junction, the inverting input of the stage-2 op-amp [literature: same, figure 4].
- The 1974 script version has no feedback resistor; adding it gives a resonance and harmonic distortion of the boosted frequencies [literature: same, section 3.1].
- The real LFO has slightly curved edges and a falling edge a little longer than the rising one [literature: same, section 4.3].
- All four JFETs share one gate voltage and must be matched [literature: same, section 4.1].
- The paper's own model is a nonlinear state-space model with JFET lookup tables and used about 70 % of an Intel i5 core [literature: same, section 5].
- JFET stage: `fc = (1/Rp + 2(IDSS/Vp²)(Vg − Vp))/(2πC)`, linear in gate voltage; the drain current has a `−Vds²` term, which is the distortion [literature: http://dafx.de/paper-archive/2005/P_155.pdf, eq. 9-17].
- OTA stage (Small Stone): `I = Ictrl·tanh((V+ − V−)/2Vt)`; a corrected discrete form with `g = 1 − exp(−2π fc/fs)` [literature: same, eq. 1-8].
- A typical analog phaser feeds the unmixed chain output back; a digital copy needs a unit delay, which moves the resonances, and this is often ignored because the feedback is low [literature: same, section 2].
- Parts list: 0.047 µF, 22 kΩ, 2N5952, 5.1 V zener [literature: https://generalguitargadgets.com/pdf/ggg_p90_bom.pdf, opened by the literature helper, not by me].
- Analog allpass `(s − ωb)/(s + ωb)`, digital `(g + z⁻¹)/(1 + g·z⁻¹)`; four sections give two notches [literature: https://ccrma.stanford.edu/~jos/pasp/Phasing_First_Order_Allpass_Filters.html, opened by the helper].
- No primary source for the Small Stone circuit could be opened; ElectroSmash's Phase 90 page did not resolve (DNS) on three attempts.

## 4. Measurements

All at 48 kHz unless stated. Tracks come from a periodic flat-spectrum probe (period 2048 samples) and `evidence/tools/ptrack.py`.

### 4.1 GR6 Phaser Nine: sweep against settings [measured: evidence/gr6_phaser_nine_tracks.tsv]

| Setting | Notch 1 (Hz) | Notch 2 (Hz) | Period (s) | Peak (dB, incl. 5.4 make-up) |
| --- | --- | --- | --- | --- |
| Defaults (Rate 0.5) | 444 – 1544 | 2091 – 7059 | 0.936 | 9.8 |
| Rate 0.05 | 423 – 1596 | 1916 – 7314 | 17.96 | 9.0 |
| Rate 0.1 | 424 – 1593 | 1910 – 7307 | 6.85 | 9.0 |
| Rate 0.25 | 428 – 1588 | 1909 – 7314 | 4.02 | 9.1 |
| Rate 0.4 | 445 – 1545 | 2046 – 7059 | 1.098 | 9.7 |
| Rate 0.75 | 476 – 1481 | 2254 – 6744 | 0.515 | 10.2 |
| Rate 0.9 | 578 – 1387 | 2551 – 6297 | 0.269 | 9.8 |
| Rate 1.0 | 712 – 1157 | 3332 – 5299 | 0.102 | 10.0 |
| Color 0, Rate 0.1 | 372 – 1395 | 2148 – 8150 | 6.85 | 6.3 |
| Color 0.7, Rate 0.1 | 459 – 1727 | 1781 – 6788 | 6.85 | 12.7 |
| Color 1.0, Rate 0.1 | 469 – 1904 | 1612 – 6277 | 6.80 | 27.4 |
| Depth 1, Rate 0.1 | 189 – 2204 | 846 – 10027 | 6.84 | 9.1 |
| Depth 0 | 905 | 4148 | static | 8.9 |
| 44.1 kHz host | 460 – 1562 | 2053 – 7131 | 0.930 | 10.0 |
| 96 kHz host | 462 – 1544 | 2090 – 7059 | 0.936 | 9.8 |

- The decompiled period law predicts 18.5, 7.0, 4.05, 1.10, 0.934, 0.518, 0.269 and 0.1025 s for those knob values; slow takes read about 2.5 % short because they hold fewer than three periods [measured: same file].
- The sweep narrows as the rate rises [measured: same file]; the cause is the 3.65 Hz control lowpass [decompiled: Guitar Rig 6 0x2a8ca4].
- Notch spacing is 5.83 with Color 0 (four equal stages) and 4.58 at the default Color [measured: same file].
- The transcribed code, run at 96 kHz, predicts 413 – 1592 Hz and 1898 – 7257 Hz for the slow sweep [inferred: evidence/tools/gr6_mxr_static.py].

### 4.2 GR6 Stoned Phaser: sweep against settings [measured: evidence/gr6_stoned_phaser_tracks.tsv]

| Setting | Notch 1 (Hz) | Notch 2 (Hz) | Period (s) | Peak (dB) | Notch 1 depth (dB) |
| --- | --- | --- | --- | --- | --- |
| Color off, Rate 0.5 | 149 – 3891 | 775 – 20103 | 3.158 | 2.5 | −13.5 |
| Color off, Rate 0.25 | 149 – 3565 | 793 – 18286 | 17.4 | 2.0 | |
| Color off, Rate 0.75 | (too fast to track) | | 0.559 | 3.7 | |
| Color off, Rate 1.0 | (too fast to track) | | 0.100 | 4.0 | |
| Color on, strength 0.74 | 100 – 1449 | 541 – 7357 | 3.158 | 7.5 | −5.2 |
| Color on, strength 0.4 | 43 – 1304 | 256 – 7452 | 3.158 | 3.7 | |
| Color on, strength 0.85 | 93 – 1861 | 442 – 7448 | 3.158 | 7.8 | |

- Rate is 0.0575, 0.317, 1.79 and 9.97 Hz at knob 0.25, 0.5, 0.75 and 1: `0.01·1000^knob` [measured: same file].
- The log-frequency track rises fast then slows, and falls fast then slows: an RC-shaped triangle [measured: evidence/phasis_tracks.tsv column stoned_phaser].

### 4.3 Static responses [measured: evidence/gr6_snapshots.tsv]

| Capture | DC region | Notch 1 | Peak between notches | Notch 2 |
| --- | --- | --- | --- | --- |
| Phaser Nine, Color 0 | +5.4 dB | 797 Hz, −24 dB | 1945 Hz, +6.2 dB | 4625 Hz, −10 dB |
| Phaser Nine, Color 0.4217 | +4.0 dB | 914 Hz, −23 dB | 1922 Hz, +8.9 dB | 4148 Hz, −14 dB |
| Stoned Phaser, Color off | +1.6 dB | 764 Hz, −12 dB | 2000 Hz, +1.7 dB | 4314 Hz, −12 dB |
| Stoned Phaser, Color on | +4.1 dB at 100 Hz | 361 Hz, −5 dB | 700 Hz, +6.7 dB | 1551 Hz, −15 dB |

- Relative to its make-up gain the block-logo setting sits −1.4 dB at DC and +3.5 dB at the peak [measured: same file]; the circuit equation in voicings.md gives −1.4 dB and +4.7 dB without the unit delay [inferred].

### 4.4 Fit of our Small Stone equations to the captures [measured: evidence/ss_color_fit.tsv]

| Case | Model | rms error |
| --- | --- | --- |
| Color off, static | four stages, dry 0.76, wet 0.50 | 0.25 dB |
| Color on, static | + feedback 0.80 through a 365 Hz highpass, + wet shelf, dry 0.125, wet 1.33 | 0.29 dB (100 Hz – 8 kHz) |
| Color on, sweep bottom (fc 105 Hz) | same constants | 0.68 dB |
| Color on, sweep top (fc 3100 Hz) | same constants | 1.64 dB |

Plain feedback around four, three, two or one stage, without the highpass and the shelf, could not get below 2.6 dB rms [measured: fits run in `$RE_STATE/work/phaser`, not kept as a file].

### 4.5 Distortion [measured: evidence/gr6_thd.tsv]

| Model | Input | H2 | H3 |
| --- | --- | --- | --- |
| Phaser Nine | 220 Hz, −3 dBFS | −211 dB | −158 dB |
| Phaser Nine | 1 kHz, −12 dBFS | −125 dB | −131 dB |
| Stoned Phaser | 220 Hz, −3 dBFS | −126 dB | −120 dB |

Both are linear to measurement accuracy. At −3 dBFS in, Phaser Nine puts out +2.85 dBFS without clipping [measured: same file].

### 4.6 Prototype against GR6 [measured: evidence/prototype_vs_gr6.tsv]

| Voicing | Notch 1 ours / GR6 (Hz) | Notch 2 ours / GR6 (Hz) | Period ours / set (s) | Peak ours / GR6 (dB) |
| --- | --- | --- | --- | --- |
| Phase 90 block, 1.071 Hz | 448 – 1559 / 444 – 1544 | 1905 – 6866 / 2091 – 7059 | 0.931 / 0.934 | 4.74 / 4.40 |
| Phase 90 script, 1.071 Hz | 415 – 1444 / no true script in GR6 | 2374 – 7627 | 0.932 / 0.934 | 0.59 |
| Small Stone, 0.3167 Hz | 148 – 3776 / 149 – 3891 | 870 – 14786 / 775 – 20103 | 3.118 / 3.158 | 2.28 / 2.49 |
| Small Stone Color, 0.3167 Hz | 65 – 1470 / (unreliable) | 368 – 6891 | 3.074 / 3.158 | 7.74 / 7.3 |

- The Small Stone notch-2 maximum is a tracker limit in an 8 s take, not a model difference: the model's own value is 2.414 × 8300 Hz = 20 kHz [inferred].
- Audition files (noise and a plucked-string riff through each voicing) are written to `$RE_STATE/work/phaser/audition/` by `prototype/audition.py` [measured: evidence/prototype_vs_gr6.tsv].

## 5. Industry practice

What NI and IK agree on:

- Four equal first-order allpass stages for both pedals [decompiled: Guitar Rig 6 0x1391b8, pool 0x2bdf308; AmpliTube 5 0xd8984c].
- Dry and wet summed in equal parts for the Phase 90 [decompiled: Guitar Rig 6 0x2a8bcc; AmpliTube 5 0x13d9218].
- No saturation in either pedal model [measured: evidence/gr6_thd.tsv] [decompiled: AmpliTube 5 0xd8984c, 0xca2390].
- The allpass coefficient is recomputed every sample, not at a decimated control rate [decompiled: Guitar Rig 6 0x2a9838, 0x2da210; AmpliTube 5 0xd899d0].
- The LFO is a triangle with rounded or exponential edges, not a sine [decompiled: Guitar Rig 6 0x2a97ac, 0x2a8ca4; AmpliTube 5 0xd89c44].
- Phase 90 speed tops out near 10 Hz [decompiled: Guitar Rig 6 0x2a891c; AmpliTube 5 0x1788d20].
- The Phase 90 sweep is not exponential: both tie frequency to a JFET law of the gate voltage [decompiled: Guitar Rig 6 pool 0x2bdf1c8; AmpliTube 5 0xd899b0].
- The Small Stone Color switch is more than feedback: it also moves the sweep and the rate or mix [decompiled: Guitar Rig 6 0x2d96a8; AmpliTube 5 0xca31b8].
- Where a feedback loop matters, the newer code avoids the unit delay: NI factors the loop into biquads, IK folds three stages and the loop into one cubic [decompiled: Guitar Rig 6 0x2d90b4; AmpliTube 5 0xd899f0].

Where they differ from each other and from the circuit:

- Phase 90 feedback topology. The circuit and NI put the loop around stages 3 and 4, entering at the stage-2 op-amp; IK puts an ideal loop around stages 2 to 4 [literature: Eichas figure 4] [decompiled: Guitar Rig 6 0x2a9690; AmpliTube 5 0xd899f0].
- NI's Phase 90 loop has a one-sample delay [decompiled: Guitar Rig 6 0x2a9690], hidden by running at twice the host rate [inferred from measured: evidence/gr6_phaser_nine_tracks.tsv].
- Sweep range. NI's break frequency runs 918 to 3525 Hz at stock depth, IK's 696 to 2077 Hz [inferred: evidence/tools/gr6_mxr_static.py; evidence/decompiled_notes.md section 3]. Real units differ by bias trim [guess].
- LFO smoothing. NI's fixed 3.65 Hz lowpass shrinks the sweep at high speed; IK's lowpass scales with rate and does not [decompiled: Guitar Rig 6 0x2a8ca4; AmpliTube 5 0xd89c44].
- Level. NI adds 5.4 dB, IK sums dry and wet at unity each (+6 dB in phase) [decompiled: Guitar Rig 6 0x2a8e5c; AmpliTube 5 0x13d9218].
- Neither models the JFET or OTA distortion that the literature describes [literature: Eichas section 3.1; Huovilainen eq. 1, 9].
- Oversampling: NI twice [inferred], IK none for the Phase 90 and four times for the Small Stone [decompiled: AmpliTube 5 0xd8984c, 0xca2560].

Modern design (Phasis): exponential sweep, triangle LFO, full-depth notches, user stage count and feedback [measured: evidence/phasis_tracks.tsv, evidence/phasis_params.txt].

## 6. Gap analysis

| Aspect | Ours | References | Consequence for the player |
| --- | --- | --- | --- |
| Stages | 6, fixed [`juce_Phaser.h:209`] | 4 for both pedals [decompiled: Guitar Rig 6 0x1391b8] | Three notches instead of two: a denser phase, not the pedal sound |
| Mix | 35 % default, linear [`src/Parameters.cpp:104`] | 50/50 [decompiled: Guitar Rig 6 0x2a8bcc] | Notches only −10.5 dB deep at the default; the effect sounds thin |
| Feedback | 0, and JUCE's loop has a unit delay [`src/dsp/FxBlocks.cpp:360`] | block-logo loop, Color loop, delay-free [decompiled: Guitar Rig 6 0x2d90b4] | No resonant peak between the notches; no script/block or Color choice |
| Sweep range | 151 – 2389 Hz cutoff at default depth, up to 20 Hz – 19 kHz [inferred: evidence/ours_today.tsv] | Phase 90 918 – 3525 Hz; Small Stone 355 – 8300 Hz [measured: evidence/prototype_vs_gr6.tsv] | Our default sweeps lower and four octaves wide; a Phase 90 covers under two |
| Sweep law | exponential [`juce_Phaser.h:140`] | JFET law for the Phase 90, exponential for the Small Stone [decompiled: Guitar Rig 6 pool 0x2bdf1c8, 0x2d9774] | The Phase 90's uneven travel, slow at the top and quick at the bottom, is missing |
| LFO shape | sine [`juce_Phaser.cpp:42`] | triangle with RC edges [decompiled: AmpliTube 5 0xd89c44] | A sine dwells at both ends; the pedals keep moving |
| Control rate | every 4 samples, held [`juce_Phaser.h:164`] | every sample [decompiled: Guitar Rig 6 0x2a9838] | Possible faint stepping at fast, deep settings [guess] |
| Voicing choice | none | named pedals | Cannot dial "Phase 90" or "Small Stone" |
| Depth against rate | constant | NI narrows the sweep at high speed [measured: evidence/gr6_phaser_nine_tracks.tsv] | Fast settings stay wide instead of tightening into a warble |
| Saturation | none | none in NI or IK [measured: evidence/gr6_thd.tsv] | No gap against the plugins |

## 7. Recommendations

Ranked by audible benefit against cost. All new parameters are appended; nothing existing changes id, range or default.

### R1. Add the Phase 90 voicing (script and block)

- Evidence: sections 3.1, 3.3, 3.7, 4.1, 4.6.
- Build: `voicings.md`, "Voicing 1". Four TPT allpass stages, JFET-law sweep 918 – 3525 Hz, RC triangle (κ 0.36) through a 6 Hz one-pole, 50/50 mix, block-logo loop solved without a delay.

```
x_n  = centre - half * lowpass6Hz(rc_triangle(phase))        // Vgs/Vp, 0.787 .. 0.252
fc   = 154 + 6773 * (1 - sqrt(x_n))                           // Hz
G    = g/(1+g), g = tan(pi*fc/fs);  a = 1 - 2G
v1   = A1(x);  s2 = (1+b)*v1 - (2+b)*lp2(v1)                  // b = 0 script, 0.4167 block
y4   = (a*a*s2 + a*k3 + k4) / (1 + b*a*a);  u = s2 - b*y4
out  = 0.5*x + 0.5*A4(A3(u))
```

- New parameters: `mod_phaser_model` choice {Classic, Phase 90, Small Stone}, default Classic; `mod_phaser_color` choice {Off, On}, default Off (Phase 90: Off = script, On = block). Same pair as `mod2_phaser_*`, `mod3_phaser_*`.
- CPU: four one-poles, one `tan` (or a polynomial) and one division per sample and channel; well under 0.1 % of a core [guess]. Latency 0.
- Compatibility: Classic runs today's `juce::dsp::Phaser` path untouched; both new parameters default to 0, so old sessions are bit-identical.
- Proof: render the periodic probe through tubamp and run `ptrack.py`. Accept if notch 1 spans 448 – 1559 Hz ±5 % at 1.071 Hz (block), the period is within 1 %, the block resonance is 4.7 ±1 dB above the script passband, script notches are below −25 dB, and the numbers move less than 2 % between 44.1, 48 and 96 kHz. With model Classic the output must null against the current build.

### R2. Add the Small Stone voicing (Color off and on)

- Evidence: sections 3.2, 3.4, 4.2, 4.3, 4.4, 4.6.
- Build: `voicings.md`, "Voicing 2". Exponential sweep 355 – 8300 Hz (off) or 105 – 3100 Hz (on), RC triangle κ 0.70, dry 0.75 / wet 0.50 (off); Color adds feedback 0.80 through a 365 Hz highpass, a wet shelf and dry 0.125 / wet 1.33.

```
fc  = sqrt(lo*hi) * (hi/lo)^(rc_triangle(phase)/2)
xs  = color ? 0.33*x + 0.67*lp279(x) : x
cf  = c * (1 - Gh)                                            // c = 0 or 0.80
y4  = (p^4*(xs - cf*sh) + K) / (1 - cf*p^4);  u = xs + cf*(y4 - sh)
out = dry*x + wet*P4(P3(P2(P1(u))))
```

- New parameters: none beyond R1 (`mod_phaser_model` = Small Stone, `mod_phaser_color` = Color).
- CPU: as R1 plus two one-poles. Latency 0.
- Compatibility: as R1.
- Proof: Color off at 0.3167 Hz: notch 1 148 – 3776 Hz ±5 %, notch depth −12.8 ±1.5 dB, peak +2.3 ±0.5 dB. Color on: static response against `evidence/gr6_snapshots.tsv` column `ss_on_static` within 2 dB rms from 100 Hz to 8 kHz, resonance +7.7 ±1 dB.

### R3. Pin depth and mix in the pedal voicings

- Evidence: each pedal model has one rate control [decompiled: AmpliTube 5 0x1789310 handles only the rate]; our generic defaults (depth 0.4, mix 0.35) are not the stock pedal (section 2).
- Build: when `mod_phaser_model` ≠ Classic, ignore `mod_depth` and `mod_mix`; the UI greys them. `mod_rate` stays in Hz.
- New parameters: none. CPU: none. Compatibility: Classic unchanged.
- Proof: with model Phase 90, sweeping `mod_depth` and `mod_mix` must leave the output bit-identical.
- If the owner later wants them live, the prototype already scales both with 0.5 = stock.

### R4. Solve feedback without a unit delay, and prewarp

- Evidence: JUCE's loop and NI's Phase 90 loop use a one-sample delay; NI then runs at twice the rate and still differs from the delay-free result by up to 9 % in notch 2 (section 4.6); NI's newer code and IK's avoid the delay (section 5).
- Build: the closed forms in R1 and R2. This is part of both voicings, listed separately because it is the reason they need no oversampling.
- New parameters: none. CPU: one division per sample. Compatibility: applies to the new models only.
- Proof: notch frequencies at 44.1 kHz and 96 kHz agree within 2 %; a 30 s render at the fastest rate with Color on stays bounded.

### R5. Sample-rate coefficient update

- Evidence: both references update per sample (section 5); ours holds for four samples (section 2).
- Build: compute `g` per sample with `tan`, or every 16 samples with linear interpolation.
- New parameters: none. CPU: a `tan` per sample is the largest single cost; the interpolated form removes it.
- Proof: a 1 kHz sine through the Phase 90 voicing at 10 Hz shows no spectral lines at multiples of fs/16 above −100 dB.

### R6. Optional later: JFET and OTA colour

- Evidence: the hardware distorts [literature: Eichas section 3.1; Huovilainen eq. 1, 9]; neither commercial model does (section 4.5). Low priority: the references the owner will compare against are clean.
- Build: Phase 90: add the `−Vds²` term as a small even-order term per stage, scaled by level. Small Stone: `tanh` on each stage's input difference as in Huovilainen eq. 6, which breaks the closed-form loop and needs one Newton step or a unit delay.
- New parameters: `mod_phaser_grit` 0 to 1, default 0. CPU: about double [guess]. Compatibility: default 0 leaves R1 and R2 unchanged.
- Proof: THD of a −12 dBFS 220 Hz sine rises from below −100 dB to a chosen target; no target can be set from the plugins, it needs a hardware capture.

### R7. Not recommended now

- A stage-count or stereo-offset control (Phasis, NI's Notches and Rotate): useful for a generic phaser, not for the two pedal tones asked for.
- Changing the Classic defaults (mix 0.35, depth 0.4): it would change old sessions.
- Make-up gain: NI's +5.4 dB is a product choice; tubamp has level controls elsewhere.

### What the shared "mod" block cannot express today

Rate, depth and mix cannot select a voicing, cannot switch feedback (script/block, Color), cannot change the stage count, the sweep law or the LFO shape, and their defaults (depth 0.4, mix 0.35) are wrong for both pedals. Two appended parameters per instance cover it:

| id | Type | Values | Default |
| --- | --- | --- | --- |
| `mod_phaser_model`, `mod2_phaser_model`, `mod3_phaser_model` | choice | Classic, Phase 90, Small Stone | Classic |
| `mod_phaser_color`, `mod2_phaser_color`, `mod3_phaser_color` | choice | Off, On | Off |

They are read only when `mod_type` is Phaser. A new choice parameter is used instead of new `mod_type` entries so that existing automation of `mod_type` keeps its normalised values.

## 8. Open questions and unverified items

- No Small Stone schematic was read. The Color-on structure (highpassed feedback, wet shelf) is a fit to GR6, cross-checked only against the shape of NI's code. A hardware capture or a schematic would settle the component values.
- R28 = 24 kΩ is from web search summaries, not an opened page; NI's constants imply 23.7 to 24.6 kΩ against 10 kΩ. ElectroSmash was unreachable.
- AmpliTube 5 crashes in its constructor under `auhost`; everything about it is from code [measured: evidence/at5_host_crash.txt]. Its Phase 90 sweep (696 – 2077 Hz) is a calculation, not a capture.
- NI's twice-rate processing is inferred from sample-rate invariance, not read in the code.
- The RigPhaserBT input and feedback biquads (0x2d9318 onward) and IK's `UpdateFilters` (0xca2b78) were not decoded.
- The Phase 90 stock sweep follows NI's unit. IK's unit sits about 0.6 to 0.76 times lower. Which is closer to a given real pedal depends on its bias trim.
- GR6's "Color 0" keeps the stage-2 gain of 1.406, so it is not a true script pedal; our script voicing has no direct capture to compare with.
- The prototype's Color-on high-frequency plateau at the bottom of the sweep is about 2 to 3 dB above GR6 beyond 12 kHz.
- Fast-rate tracks (above about 4 Hz for the Small Stone) could not be measured with a 43 ms frame.
- Phasis, `MXRPhaser100`, `FenderPhaser`, `FOXXPhaser`, `UniVibe` and `ST4S::DSP::Phaser` were not read.
- The Hopper pseudo-code request for these functions was still queued when the study ended; all reading is from `disasm.py`.
- Tubamp's own phaser was not rendered (no built plugin in this worktree); section 2 is computed from source.

## Corrections after verification

An independent verification (`verification.md`) found four claims off. They were re-measured
with a multitone tracker that takes sweep extremes per LFO cycle and the median over cycles
(`evidence/fit/track.py`; self-test within 0.07 % on smooth sweeps, `evidence/fit/selftest.txt`).
New targets are in `evidence/fit/targets.tsv`; the prototype comparison is in
`evidence/fit/compare.txt` (before the refit: `evidence/fit/compare_before.txt`). The rest of this
report is left as written; where it quotes the old numbers below, these corrections win.

| Claim | Item | Old value | Corrected value | Source |
| --- | --- | --- | --- | --- |
| C07 | Phaser Nine defaults, notch 1 | 444 – 1544 Hz | 464.1 – 1522.8 Hz | evidence/fit/targets.tsv p9_block_p050 |
| C07 | Phaser Nine defaults, notch 2 | 2091 – 7059 Hz | 2132 – 6956 Hz | same |
| C07 | Phaser Nine defaults, period | 0.936 s | 0.9337 s (the decompiled law) | same |
| C07 | Phaser Nine Rate 0.1, notch 1 / notch 2 / period | 424 – 1593 / 1910 – 7307 Hz / 6.85 s | 423.6 – 1590.2 / 1946 – 7259 Hz / 7.000 s | targets.tsv p9_block_p010 |
| C07 | Phaser Nine Rate 0.75 and 1.0, notch 1 (sections 4.1, voicings) | 476 – 1481, 712 – 1157 Hz | 504.0 – 1461.4, 737.8 – 1105.9 Hz | targets.tsv p9_block_p075, p9_block_p100 |
| C09 | Phaser Nine output level, −3 dBFS 220 Hz sine in | +2.85 dBFS | +0.48 dBFS peak (1.057), unclipped; harmonics unchanged | re-rendered after the fix: `sig.py thd` H1 +0.48 dB, `sig.py stats` peak +0.48 dBFS (state st_d000, sine220_m3.wav); the old reading came from `sig.py thd` before its 2.37 dB level fix |
| C12 | Stoned Phaser Color off, Rate 0.5, notch 1 | 149 – 3891 Hz | 150.5 – 3780.9 Hz | targets.tsv ss_off_r050 |
| C12 | same, period / notch depth / peak | 3.158 s / −13.5 dB / +2.5 dB | 3.1623 s / −12.08 dB / +1.94 dB | same |
| C19 | Prototype Phase 90 block against GR6, notch 1 at 1.071 Hz | 448 – 1559 against 444 – 1544 Hz (+0.8 %, +1.0 %) | before the refit 449.6 – 1556.3 against 464.1 – 1522.8 Hz (−3.1 %, +2.2 %), growing to −7.9 % / +7.8 % at faster rates; after the refit 464.5 – 1526.6 Hz (+0.1 %, +0.3 %), worst −1.40 % (1.29 % after the first fix round, see evidence/fit/compare_before_color.txt) over six rates | compare_before.txt, compare.txt |
| C19 | Block resonance, prototype against GR6 | +4.74 against +4.40 dB (and "+4.7 dB" in 4.3 and R1) | before +3.79 against +3.53 dB; after +3.52 against +3.53 dB. The 4.7 dB was a tracker artefact | compare.txt; closed form in evidence/fit/fit.py |
| C19 | Prototype Small Stone Color off, notch 1 | 148 – 3776 against 149 – 3891 Hz | 148.9 – 3739.4 against 150.5 – 3780.9 Hz (−1.1 %, −1.1 %); unchanged constants | compare.txt ss_off_r050 |

Constants changed in `voicings.md` and `prototype/phaser_ref.py` (all regenerated by `evidence/fit/fit.py`):

| Constant | Old | New | Tag |
| --- | --- | --- | --- |
| Phase 90 gate smoothing | 6.0 Hz | 4.406 Hz | fit |
| Phase 90 JFET law scale R0 (K) | 500 Ω (6.77 kHz) | 539.2 Ω (6.28 kHz) | fit |
| Phase 90 sweep ends x_off, x_on | 0.787, 0.252 | 0.7680, 0.2139 | fit |
| Phase 90 block ratio β | 10 k / 24 k = 0.4167 (24 k guessed) | 0.3957 (R28 equivalent 25.3 kΩ) | fit |
| Small Stone Color-off dry / wet | 0.75 / 0.50 (0.76 in ss_color_fit.tsv) | 0.75 / 0.50, from depth and peak | fit |

Other corrections: "equal summing resistors" is not in Eichas (no values given), so the 50/50
mix is now tagged a guess; R28's value is unsourced. The Small Stone Color-on voicing is
outside 3 % on notch 1's minimum (−4 to −7 %, a detection-threshold reading) and on notch 2's
minimum (+17 %); a range refit cannot fix both, so it is left as an open item (voicings.md,
"Measured accuracy"). The audition set was re-rendered with every file at or below −1 dBFS
(`audition/peaks.txt`).

### Second round: Small Stone Color on, Phase 90 notch depth

**Color-on structure.** `NI::DSP::StonedPhaser` was read for the structure of the Color-on path
(`evidence/stoned_color_structure.md`). Four equal stages with one shared break frequency,
realised as two second-order sections. Feedback is taken from the fourth stage and injected
before the first, with gain `+c` delay-free and `−c` through a filter `BB` (unity at DC) and one
sample of delay, so the net feedback `c·(1 − z⁻¹·BB)` is a second-order-shaped highpass. The
"Color biquads" are one two-pole network with two numerators: `BA` shapes the wet input only
(the dry is the unfiltered input), `BB` sits in the feedback path. Color also selects a lower
sweep-limit pair and a wet share of `min(1, 0.5 + 0.5|c|)`; it leaves the LFO shape and rate
alone. The prototype's earlier model had the same loop but first-order filters (a 365 Hz
highpass and a 279/845 Hz shelf); that is why it fitted one static frame (0.33 dB) and missed
notch 2 at the sweep bottom by 17 %. Section 3.2's "input biquad" is the `BA` branch.

**New measurements** (`evidence/fit/targets.tsv`, `measure.py color`). The three Color-on rows
were re-rendered and came back identical. Added: a Rate-0 (0.01 Hz) quasi-static take through a
periodic multisine (`evidence/fit/static.py`), and six frozen static frames at the LFO turning
points of three Sweep Min/Max windows.

**Prototype.** The Color-on path now uses that topology with our own constants, fitted by
`evidence/fit/fit.py` from `targets.tsv`: c 0.7553, network poles 53.0 / 266.0 Hz, feedback
zero 18.3 Hz, input zeros 16.5 / 885.2 Hz, dry 0.1266, wet 0.4289, sweep 99.0 to 3133.5 Hz.
Static frames 0.05 to 0.25 dB rms (were 0.52 to 0.79 dB on the same frames).

| Color on, rate (Hz) | Notch-1 min | Notch-1 max | Notch-2 min | Notch-2 max | Period | Peak |
| --- | --- | --- | --- | --- | --- | --- |
| 0.159 before / after | −6.57 / +1.09 % | +1.84 / +1.85 % | +17.3 / −0.16 % | −3.8 / −2.98 % | 0.00 / −0.01 % | +0.17 / 0.00 dB |
| 0.316 before / after | −6.73 / +0.82 % | +2.46 / +2.45 % | +15.0 / −1.37 % | −3.8 / −2.98 % | 0.00 / 0.00 % | +0.29 / +0.01 dB |
| 1.778 before / after | −4.18 / +4.19 % | +2.82 / +2.80 % | +16.5 / +1.68 % | −3.9 / −3.03 % | 0.00 / 0.00 % | +0.26 / −0.02 dB |

Two cells stay just outside 3 %, both from how Guitar Rig 6 computes rather than from the
circuit. (1) Notch 2 at the sweep top: Guitar Rig runs the component at twice the session rate
[inferred]: the same structure fits all six frames to 0.01 dB rms at 96 kHz against 0.05 to
0.25 dB at 48 kHz, and 48 kHz first-order allpasses cannot widen the notch spacing at the top;
f_hi splits the error between the two notches. (2) The notch-1 minimum at 1.78 Hz (+4.2 %) is a
detection threshold, and Guitar Rig's factored-section form narrows the swept range at fast
rates where our TPT chain does not (shown on an analysis-only transcription with the same loop
and trajectory). Before/after tables: `evidence/fit/compare_before_color.txt`, `compare.txt`.

**Phase 90 notch depth.** The 3.7 dB gap was not a tracker floor: the tracker reads ideal
synthetic phasers at −55 to −72 dB and the prototype's closed-form −32.6 dB exactly. It was not
a wet/dry imbalance either: binned per frame, Guitar Rig's notch fills in as it rises (−30.3 dB
near 510 Hz to −27.2 dB near 1380 Hz at noon, −29.4 to −28.2 dB over 740 to 1100 Hz at the
fastest rate), and a gain imbalance gives the same depth at every frequency. Phaser Nine's loop
holds a one-sample delay [decompiled: Guitar Rig 6 0x2a968c, 0x2a98e4]; at 96 kHz the
transcription gives −30.6 dB at 412 Hz to −26.7 dB at 1595 Hz, which matches. The fix is one
fitted constant: an 11 818 Hz one-pole lag in the R28 feedback path (`P90_FB_LAG_HZ`). Median
depth is now −28.74 to −28.92 dB against −28.70 to −28.90 dB (within 0.06 dB at every rate).
Notch 1 moved by at most 0.16 % (sweep top) and 0.04 % (sweep bottom), the peak by −0.06 dB,
and notch 2's top rose 1 to 2 % towards Guitar Rig.

Also found: with Color off, Guitar Rig's input branch is a first-order highpass near 44 Hz on
the wet path, which explains its notch ratio of 5.4 (ours 5.83). Color off was not changed.
The audition set was re-rendered (`audition/peaks.txt`).

### Third round: Phase 90 sweep shape, κ, spec errors

An independent check of the round-2 voicings (`verification-refit.md`) confirmed every sweep end
but found the Phase 90 block sweep up to 6.6 % off Guitar Rig 6 between the ends (3.1 to 3.8 % rms
up to 1.9 Hz), a constant taken from a vendor binary (κ = 0.36, from AmpliTube 5), and errors in
`voicings.md`. This round fixed those items and left the Small Stone alone.

**Trajectory measurement.** The whole first-notch path over one LFO period was measured on
Guitar Rig 6's Phaser Nine at six rates (0.143, 0.247, 1.071, 1.930, 3.721, 9.756 Hz) and on the
Stoned Phaser, Color off and on, at three rates, with the verifier's impulse-train tracker
(`evidence/fit/itrack.py`, copied with credit; `evidence/fit/trajectory.py` renders, tracks and
folds into 200 phase bins) [measured: evidence/fit/trajectories.tsv, commands in its header]. The
tracked notch of the four takes compared is identical to the verifier's, snapshot for snapshot. Self-test on our own synthetic phaser: smooth sweeps
within 0.05 % rms and 0.26 % at any point, sharp corners 0.48 % rms at 1.78 Hz
(`evidence/fit/selftest_traj.txt`).

**Sweep law.** At 0.14 Hz Guitar Rig's path is straight in log frequency over the lower 85 % of
its range with a sharp bottom turn and bends to about 0.6 of that slope near the top. A
square-root conductance on a curved triangle cannot follow that. The new law, our own form with
fitted constants: each stage's JFET leg is an exponential channel resistance `R1·e^(−γu)`,
soft-limited from below by a series resistance RS (`R = (RS^m + R_ch^m)^(1/m)`), in parallel with
the 22 kΩ resistor [circuit]; u is the LFO after the gate smoothing. Free constants RS 933.7 Ω,
R1 1786.1 Ω, γ 0.8915, m 3.892, κ −0.002, gate 3.535 Hz, fitted by `evidence/fit/fit.py` to
`trajectories.tsv` with the sweep ends of `targets.tsv` as a penalty; the feedback lag was refitted
to the depth (11 736 Hz, was 11 818). β is unchanged. Laws tried and dropped: square-root refitted
(2.35 % rms worst rate), power law of (1 − Vgs/Vp) (1.57 %), linear JFET conductance with a series
resistor (no physical optimum), exponential with a plain series resistor (1.00 % rms but the top
end +2.9 %). Nothing was taken from Guitar Rig's own frequency mapping.

| Phase 90 block, rate (Hz) | Sweep ends, worst, before / after | Trajectory rms before / after | Trajectory largest before / after | Depth after | Peak after |
| --- | --- | --- | --- | --- | --- |
| 0.143 | +0.46 / −0.51 % | 3.87 / 0.72 % | +6.31 / −1.27 % | −0.05 dB | −0.06 dB |
| 0.247 | +0.55 / +0.27 % | 3.84 / 0.72 % | +6.33 / −1.20 % | −0.04 dB | −0.06 dB |
| 1.071 | +0.10 / +0.44 % | 3.61 / 0.69 % | +6.53 / −1.08 % | −0.06 dB | −0.06 dB |
| 1.930 | −0.29 / +0.62 % | 3.15 / 0.62 % | +5.95 / +1.03 % | −0.06 dB | −0.06 dB |
| 3.720 | −1.40 / −0.75 % | 2.10 / 0.56 % | +4.05 / −1.15 % | −0.06 dB | −0.06 dB |
| 9.756 | +1.08 / +0.47 % | 1.15 / 0.38 % | +2.14 / +0.76 % | −0.07 dB | −0.06 dB |

Sources: `evidence/fit/traj_before_round3.txt`, `compare_before_round3.txt`, `compare.txt`. Every
round-3 target is met (trajectory ≤ 1.5 % rms and ≤ 3 % at any point, ends ≤ 1.5 %, depth ≤ 0.2 dB,
resonance ≤ 0.1 dB, period exact); the ends improved rather than paying for the middle. Notch 2's
top, not a target, moved from −3.4 to −4.2 % at the slowest rate.

**κ.** The Phase 90 LFO curvature is now `[fit: evidence/fit/trajectories.tsv]`: −0.002 (0.008 in
a first pass at the same cost), i.e. a straight triangle; the AmpliTube-derived 0.36 and its
attribution are gone from the prototype and the spec.

**Small Stone.** Same trajectory measure: Color off 1.89 to 1.90 % rms (3.6 % at worst), Color on
1.08 to 1.12 % rms (2.2 %), both below the 3 % that would have triggered a change, so its constants
are unchanged. `fit.py` now re-derives the Color-off range and κ from `trajectories.tsv` as a
check (353.2 / 8544 Hz, κ 0.710; it would reach 0.75 to 0.93 % rms), not applied.

**Spec errors fixed** (`voicings.md`).
- The first notch's quasi-static top was given as 1589 Hz while the equations gave 1609 Hz. The
  numbers in the spec are now printed by `evidence/fit/spec_check.py` (`spec_check.txt`), which
  evaluates the spec's equations for each voicing and checks them against the prototype's impulse
  response (≤ 2e-10 dB). With the new law: Phase 90 block notch 1 419.5 to 1589.6 Hz, notch 2 1924
  to 6977 Hz (fc 930.8 to 3487.3 Hz), script notch 1 386.0 to 1465.6 Hz; Small Stone Color off
  147.1 to 3744.7 Hz; Color on notch 1 to 1329.2 Hz (it fades at the sweep bottom), notch 2 432 to
  6965 Hz; all at 48 kHz.
- "R28" was in no opened source (Eichas calls the feedback resistor R15; the GGG list's R28 is
  56 kΩ). The spec and prototype now say "the block-logo feedback resistor (R15 in Eichas'
  schematic)" and give 25.3 kΩ as a fitted equivalent (with an unsourced 10 kΩ), not a schematic
  value. Section 3.7's "R28 on the MXR board" is superseded by this.
- The top of `voicings.md` now states which Guitar Rig 6 settings are the reference and why
  (Phaser Nine four stages, Color 0.4217; Stoned Phaser Color Strength 0.74), that the components'
  INIT presets differ (seven stages and Color 0.5027; Color Strength 0.40), and that the claim is
  "matches a vendor's model at chosen settings", not a measured pedal.
- Prototype-only details are now in the spec: gate smoothing form and start state, LFO polarity
  and start phase, the lag clamp, the command-line default rates.

Claim C19's prototype numbers are superseded by `compare.txt`. The audition set was re-rendered
(`audition/peaks.txt`; every file under 12 s, loudest −3.69 dBFS).
