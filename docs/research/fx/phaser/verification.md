# Phaser study: independent verification

Verifier's scratch (scripts, tracks, no WAVs kept): `$RE_STATE/work/phaser-verify/`.
My tracker is `mt.py` there: interleaved sparse multitones (2 % or 2.5 % log grid, 16 or 18 renders), each tone heterodyned with a Blackman-Harris window, hop 1.3 ms, notch refined on |H|^2.
It reads the prototype's own quasi-static truth to 0.3 % (449.4 to 1556.5 Hz computed, 450.9 to 1553.3 Hz tracked).
Tolerances I accepted: 2 % on a frequency, 2.5 % on a period taken from fewer than three cycles, 0.5 dB on a level.
Guitar Rig 6 renders are bit-exact from run to run, so the renders can be combined.
AmpliTube 5 was not loaded. Hopper was not used; all code reading is from `disasm.py`.

## Part 1. Claims

| id | verdict | what I checked (command or address) | what I saw | note |
| --- | --- | --- | --- | --- |
| C01 | VERIFIED | `juce_Phaser.h:105-186,205,209`, `juce_Phaser.cpp:42,51`, `src/dsp/FxBlocks.cpp:359-360,396-399` | `numStages = 6`, `maxUpdateCounter = 4`, `std::sin`, `DryWetMixingRule::linear`, centre 600, feedback 0. | Read the process loop, not only the grep. |
| C02 | VERIFIED | `--floats 0x2bdf1c8 4`; `onSetSampleRate` 0x2a8c8c-0x2a8e1c; `onInit` 0x2a8b2c; `processBuffer` 0x2a981c-0x2a9850 | Pool is -417070, 12652.6, -37689.5, -1112860. Immediates 49157.8 and 1179910. All scaled by 2*pi/fs, -1 added to the constant terms. Lanes 2,3 hold the cubic, lanes 0,1 the line. `fcmge` against 3.1929 picks the line when V >= threshold. | f(3.0604) = 891 Hz, f(3.145) = 1872 Hz, line at 3.2296 = 3173 Hz. Branches meet at 3.1929 (2713 against 2709 Hz). |
| C03 | VERIFIED | `--floats 0x2be05b0 8`; `onInit` 0x2a8b18-0x2a8b44; `processBuffer` 0x2a9874-0x2a990c | [0x310] = 1.2030, 1.2030, -0.2030, -0.2030; [0x320] = -0.2030, -0.2030, 1.2030, 1.2030. Code forms a*[0x310] + [0x320], uses lanes 0,1 as the feed-forward and lanes 2,3 as the state coefficient: y2 = d*y1 + s2, s2 = e*y1 - a*y2. | Algebra gives 1.2030*AP - 0.2030. DC gain 1, HF gain -1.406. |
| C04 | VERIFIED | `setParamColor` 0x2a9050; 0x1391b8; strings at 0x424b771 and 0x424b5f7; `onControlRate` 0x2a9590; loop 0x2a98e8-0x2a9924 | -0.95*c when [0x98] is even, +0.95*c when odd, stored to the ramp target of the feedback lane. Defaults: id "n" = 4, id "c" = 0.4217. [0x98] = Notches - 2 and is the count of plain stages after stage 2, so Notches = total stages. | Traced the loop myself: stage 1 plain, stage 2 modified, F = fb*y_last from the previous sample added to the stage-2 output, then [0x98] plain stages. |
| C05 | VERIFIED | `convertInput` 0x2a8a2c-0x2a8aa8 | 1/(30 - 230x) below 0.1, 1/(8.9667 - 19.6667x) below 0.4, else 1/(1.765 - 1.6625x). | My measured periods: 7.000 s at 0.1, 0.9337 s at 0.5, 0.5181 s at 0.75, 0.1025 s at 1.0. All match the law. |
| C06 | VERIFIED | `onSetSampleRate` 0x2a8ca4-0x2a8d28; `onInit` 0x2a8af0; `onResume` 0x2a8e5c; `processBuffer` 0x2a97cc-0x2a97f0 | 22.91 rad/s one-pole written to lanes 2,3 (control). 1.862087 written to lanes 0,1 of the gain vector that multiplies the input. | 22.91 rad/s = 3.646 Hz. |
| C07 | NOT VERIFIED | Own render of the default state; author's `ptrack.py`; my `mt.py` | ptrack on my render reproduces 444 to 1544 Hz, 0.9363 s. But its track jitters +-5 % frame to frame while the notch moves, and 444 is one outlier frame (t = 1.945 s). At the turning point ptrack itself reads 464 to 465 Hz. My tracker: notch 1 464.5 to 1521.6 Hz, notch 2 2134 to 6951 Hz, period 0.9337 s. | Rate 0.1 half holds: 423.9 to 1589.2 Hz and 1947 to 7256 Hz, period 7.000 s. The default-rate minimum is 4.6 % off. |
| C08 | VERIFIED | Own renders at 44.1, 48, 96 kHz with `mt.py` | 48 kHz: 464.5 to 1521.6 / 2134.4 to 6950.8 Hz. 96 kHz: 464.5 to 1521.6 / 2134.3 to 6950.8 Hz. 44.1 kHz: 465.5 to 1535.3 / 2138.2 to 7003.1 Hz. | Invariance holds to 0.1 Hz. The 44.1 kHz offset is +0.9 % at the top, +0.2 % at the bottom, not 1.2 %. The absolute maxima are 1522 and 6951 Hz, not 1544 and 7059. |
| C09 | NOT VERIFIED | Recheck command on my own state; `sig.py stats`; raw sample read; scaling null test | Harmonics reproduce: H2 -211 dB, H3 -158 dB, H5 -156 dB, H7 -153 dB, H9 -147 dB. The level does not: `sig.py thd` reads -0.63 dB for a -3 dBFS sine, so its "2.85" is not dBFS. The output peak is +0.48 dBFS (1.057 linear), unclipped. | Linearity holds. Null test under sweep, input peak +9.8 dBFS: out(8x) - 8*out(x) is -118 dB. Only the +2.85 dBFS figure is wrong. |
| C10 | VERIFIED | `setParamColor` 0x2d96a8-0x2d97b0; `setParamColorStrength` 0x2d97f0 | 307 or 93 Hz, 8500 or 2960 Hz selected by the bool. c = color * [0x94] * [0x90], `fmax` -0.85, `fmin` 0.85. Centre sqrt(f1*f2), exponent log(sqrt(f2/f1))/3.4657. | Init strength 0.74 at pool 0x2bdf310, N = 2 at 0x2bdf308. |
| C11 | VERIFIED | `updateBaseCoefficients` 0x2d90d8-0x2d92e0; `--doubles 0x37fcc60 1`; `processBuffer` 0x2d9fac-0x2d9fc0, 0x2da210-0x2da2c8 | powf(abs(c), 0.5/N). Angle (i + 0.5 if c < 0)*pi/N into sincos. 0.13333 and 0.33333 used as x*(1 + x^2*(1/3 + x^2*2/15)). One `fdiv.2d` per section pass, loop count [0x9c]. | I saw every listed item. I did not re-derive that the sections equal 1 - c*A^2N; that reading is the author's. |
| C12 | NOT VERIFIED | Own render, `mt.py` with 4096 and 512 windows, `shape.py` | Notch 1 149.9 to 3775 Hz (not 3891). Period 3.1623 s. Notch depth -12.1 dB (not -13.5). Peak +1.9 dB (not +2.5). | The shape half holds: log-f triangle R2 0.993, linear-f 0.85; an RC triangle with kappa 0.70 fits at 0.9998. |
| C13 | VERIFIED | 1 kHz sine, envelope autocorrelation, four states | 0.0564, 0.3168, 1.7783, 10.0006 Hz at 0.25, 0.5, 0.75, 1.0. | The law 0.01*1000^knob is exact. The claimed 0.0575 and 1.79 are short-take readings, 2 % and 0.7 % off. |
| C14 | VERIFIED | `Process` 0xd8986c-0xd89a7c | 1.195402, -2.364943, -2.758621, 0.9996375, 190.4655, 24000, 5e-8, 10000, 1.367816, 0.6321838. sqrt is applied to the LFO-derived value only. No `bl` or `blr` in the function. | Static only. |
| C15 | VERIFIED | `Process` 0xd89938-0xd89964; `SetSampleRate` 0xd89bd4-0xd89ca0 | Sign flip on phase against duty, into a one-pole with omega = rate * 0.1135332 * 2*pi, bilinear. | The fmax 0.1 / min 10 clamp sits on the tempo-sync branch only. The free rate is copied unclamped; its 0.1 to 10 range is in `DefineParameters` 0x1788d10, 0x1788d20. |
| C16 | VERIFIED | `Color` 0xca31dc-0xca3300; `--floats 0x239e1d8 3` | On: 0.581, 0.73, 0.04847 + 9.2015*pow(knob, 4.5384). Off: 0.56, 0.67, 0.07112 + 13.4289*pow(knob, 4.5894). | The rate branch is skipped when the sync flag [0x178] is set. |
| C17 | VERIFIED | `~/Library/Logs/DiagnosticReports/auhost-2026-11-11-010831.ips` | EXC_BAD_ACCESS at 0x0. Top frames: `SharedPluginData::SharedPluginData()`, `SingletonHolder::get()`, `AudioProcessor::AudioProcessor()`, `JuceAU::JuceAU`. | I read the crash report. I did not run the recheck, as instructed. |
| C18 | VERIFIED | Opened the PDF, pages 3 to 6 | Figure 4: R15 runs from B (stage-4 output) to the junction of R6 and R7. Text: R15 "introduces a feedback in the allpass cascade"; the 1974 version has no R15; section 5 puts R15 in the second allpass module. A and B meet through R18 and R16 at the base of Q5. | See Part 3 for what the paper does not say. |
| C19 | NOT VERIFIED | `audition.py` rerun; `mt.py` on prototype and Guitar Rig 6; closed-form response of the block equation | The script reprints the table exactly. The prototype side is right (450.9 to 1553.3 Hz, 0.9337 s; 149.8 to 3734 Hz). The Guitar Rig side is not: 464.5 to 1521.6 Hz, not 444 to 1544; 149.9 to 3775 Hz, not 149 to 3891. Resonance is +3.79 dB for the prototype and +3.49 dB for Guitar Rig 6, not +4.74 and +4.40. | The 4.74 dB is a tracker artefact. The block equation's own peak is +3.79 dB at every break frequency. |
| C20 | VERIFIED | Own static and sweep renders, snapshot by `mt.py snap`, prototype impulse response at constant fc (`fitc20.py`) | Static: 0.33 dB rms at fc 610 Hz (0.30 at 615), worst point 0.96 dB. Sweep bottom 0.49 dB at fc 105. Sweep top 0.75 dB at fc 3100 (0.55 at 3000). 100 Hz to 8 kHz, 2.5 % log grid. | The recheck command only redoes arithmetic on stored columns (0.28, 0.71, 1.63). Outside the band the fit is worse: 1.7 to 2.8 dB rms above 8 kHz, about +2 dB below 100 Hz. |

Counts: 16 VERIFIED, 4 NOT VERIFIED, 0 INCONCLUSIVE.

Points the brief asked about:

- PhaserMXR stages. Four by default. Stage 1 is `y = a*x + s; s = x - a*y`. Stage 2 has the two modified coefficients. Stages 3 and 4 are plain. The loop at 0x2a98f0 adds stages when Notches is above 4.
- Phase 90 feedback in Guitar Rig 6. `fb*y_last` is stored to lanes 2,3 of [0x2f0] at 0x2a968c and added to the stage-2 output at 0x2a98e4 on the next sample. Gain -0.95*Color = -0.4006 at the default. The loop holds stages 3 and 4 and one sample of delay.
- StonedPhaser against RigPhaserBT. `calculateFilterCoeffs` 0x697eac makes the same four calls in the same order and loads the same seven double constants as 0x2d90b4. `processAudio` 0x69830c has the same two series constants and one `fdiv.2d` in a loop. Same algorithm, checked at that level only. The rack component (`PhaserBT`) uses RigPhaserBT.
- Small Stone stages. N = 2 sections. Measured: exactly two notches, ratio 5.46 to 5.70.
- Linearity. No call in either Guitar Rig process loop or in the AmpliTube Phase 90 loop. Phaser Nine nulls to -118 dB. Stoned Phaser leaves a residual of -73 to -75 dB that scales with level, sits below 300 Hz, is sign-symmetric and repeatable. It is not harmonic distortion (H2 -126 dB, H3 -120 dB). I could not name its cause.

## Part 2. Prototype and voicing spec

### First-notch trajectory, my method

| System | Notch 1 min (Hz) | Notch 1 max (Hz) | LFO period (s) | Notch 2 (Hz) | Peak (dB) |
| --- | --- | --- | --- | --- | --- |
| Guitar Rig 6 Phaser Nine, defaults | 464.5 | 1521.6 | 0.9337 | 2134 to 6951 | +8.89 (+3.49 over the 5.40 make-up) |
| Prototype Phase 90 block, 1.071 Hz | 450.9 | 1553.3 | 0.9337 | 2044 to 6652 | +3.76 |
| Prototype error | -2.9 % | +2.1 % | 0.0 % | -4.2 %, -4.3 % | +0.3 dB |
| Guitar Rig 6 Stoned Phaser, Color off, Rate 0.5 | 149.9 | 3775 | 3.1623 | 819 to about 17300 | +1.91 |
| Prototype Small Stone, 0.3167 Hz | 149.8 | 3734 | 3.1576 | 872 to about 14700 | +1.93 |
| Prototype error | -0.1 % | -1.1 % | -0.15 % (rate set 0.15 % high) | +6.5 % at the bottom | 0.0 dB |

- Phase 90 block: the "within 1 %" claim is refuted. The error is -2.9 % and +2.1 % at 1.071 Hz.
- The error grows with rate. Rate 0.1: -0.9 % and +1.2 % (423.9 to 1589.2 against 420.2 to 1608.1 Hz). Rate 0.75: -5.0 % and +3.1 % (504.5 to 1460.6 against 479.1 to 1505.2 Hz). Rate 1.0: -5.6 % and +7.7 % (739.4 to 1103.3 against 698.1 to 1188.2 Hz).
- Cause: the 6.0 Hz gate smoothing was fitted to ptrack extremes (444 to 1544, 476 to 1481, 712 to 1157 Hz) that are outlier frames. Guitar Rig 6 narrows more than the prototype does.
- Within one cycle at 1.071 Hz the two tracks differ by 3.6 % rms and up to +6.7 % after the best time shift. The prototype rises faster (0.60 of the log range at mid-rise against 0.53).
- Small Stone, Color off: the "within 3 %" claim is confirmed. Trajectory difference 2.2 % rms, worst -4.3 % at mid-rise. Notch depth -12.0 against -12.1 dB.
- Small Stone, Color on (the report gives no Guitar Rig number). Where both notches resolve: notch 1 140.6 to 1279.8 Hz in Guitar Rig 6, 148.4 to 1306.9 Hz in the prototype; notch 2 708.9 to 7045 Hz against 766.7 to 6786 Hz (+8.2 %, -3.7 %). Notch depths agree within 0.3 dB over the sweep. In the low half of the sweep the prototype's maximum is a +7.2 dB plateau at 20 kHz; Guitar Rig 6 peaks at 160 to 480 Hz there.
- Color-on static response: 0.33 dB rms, confirmed (table row C20).
- Prototype script voicing: 413.2 to 1424.4 Hz, notches -43 dB, peak 0.0 dB.

### Constants: voicings.md against sources and prototype

Every numeric constant in voicings.md appears with the same value in `phaser_ref.py`, except the one in item 6. Items without support, or wrong:

1. Block resonance "+4.7 dB over the script passband" (voicings "Block result", report 4.3 and the R1 acceptance line). The stated equation gives +3.79 dB. Guitar Rig 6 gives +3.49 dB. The DC figure (-1.4 dB) and the notch spacing (4.3 to 4.6) are right.
2. Gate smoothing 6.0 Hz. It points at a measurement file, but the target numbers in that file are biased. It needs a refit against 465 to 1522 Hz at 1.071 Hz and 739 to 1103 Hz at 9.76 Hz.
3. "K fitted ... rms error 2.2 %". Against my numbers at four rates the sweep-end error is 4.1 % rms.
4. R28 = 24 kOhm, beta 0.4167. Tagged [guess], correctly. The cited parts list has no 24 k part; it has a fifth 22 k (R15) and a 56 k (R28). Guitar Rig 6's constants imply 0.4006 to 0.406.
5. "Equal summing resistors R16, R18 [literature: Eichas, figure 4]". The paper gives no resistor values.
6. Color-off dry gain: 0.75 in voicings.md and the prototype, 0.76 in `ss_color_fit.tsv` and report 4.4.
7. "g every 16 samples with linear interpolation ... inaudible" is [inferred] and untested. The prototype is per-sample only.
8. In the prototype but not in the spec: break frequency clamped to 0.49 fs, Vgs/Vp clipped to 0..1, LFO start phase 0, depth scaling capped at 1.2, the mix scaling law.
9. The fit scripts for the Small Stone constants and for K, kappa and the gate were not kept. The fits cannot be rerun from `evidence/`. I checked their results instead.
10. 47 nF, 22 kOhm, 2N5952 and the 5.1 V zener are in the parts list. Their role (allpass capacitor, JFET shunt) is read from the counts, not stated there.

Supported as tagged: sweep ends 0.787 and 0.252 (slow sweep within 1.2 %), kappa 0.36 (= pi * 0.1135332, the AmpliTube LFO), Small Stone ranges, kappa 0.70 (best of the shapes I tried, mid-rise 0.576), c 0.80, 365 Hz, 279 Hz, 0.33, 0.125, 1.33, 0.50.

### Audition WAVs

- Ten files plus `cli_check.wav` exist in `$RE_STATE/work/phaser/audition/`. 48 kHz float mono, noise 10 s, riff 7.6 s. All ten are byte-identical to my rerun of `audition.py`.
- None is silent. Noise files are -14 to -20 dBFS rms. Riff files are quiet: -36 to -39 dBFS rms, peaks -8 to -13 dBFS.
- `smallstone-color_noise.wav` peaks at +1.19 dBFS, five samples above full scale. It is not clipped in the float file but will clip on fixed-point playback. The other files stay below 0 dBFS.
- All four processed noise files show a moving notch: the deepest 1/6-octave dip travels 442 to 1561 Hz (block), 145 Hz upward (Small Stone), and so on.

### Classic bit-identical?

Yes by construction, if built as written. Both new parameters are choices with default 0, `mod_type` keeps its three entries, and model 0 runs the existing `juce::dsp::Phaser` call path. Gaps to close:

- R5 has no compatibility line and cites "ours holds for four samples". Applied to the Classic path it would change old sessions. It must be limited to the new models.
- The new parameters need a new AU version hint (existing ones use 1 and 2). The report does not say so.
- `Modulation::setParameters` resets engines on a type change only. A model change needs the same reset for the new state; that does not touch Classic.

## Part 3

### Citations opened

- Eichas et al., DAFx-14 (`dafx14.fau.de/...eichas...pdf`): exists. It says what the report attributes: four stages, the fc formula, R15 from B to the R6/R7 junction, no R15 in the 1974 version, resonance with harmonic distortion, summing at Q5, curved LFO edges with a longer falling edge, matched JFETs on one control voltage, lookup tables, about 70 % of an Intel i5 core. It gives no component values and never names "R28".
- Huovilainen, DAFx-05 (`dafx.de/paper-archive/2005/P_155.pdf`): exists. Eq. 1 is the tanh OTA law, eq. 8 is g = 1 - exp(-2*pi*fc/fs), eq. 9 to 17 are the JFET stage and its fc. Section 2 says the digital copy needs a unit delay and that the difference "can likely be ignored". Its figure 1 feeds back around stages 2 to 4, which is AmpliTube's topology.
- General Guitar Gadgets parts list (`generalguitargadgets.com/pdf/ggg_p90_bom.pdf`): exists. Six 0.047 uF, five 22 k, 10 k op-amp resistors, 2N5952, 5.1 V zener. No 24 k part.
- Not opened by me: the CCRMA page.

### Inferred or guessed items presented as fact

- Summary line 4 and voicings "How close it lands": "within 1 %" for the Phase 90. Refuted above.
- Summary line 2: "the references agree" on the block-logo loop and the Color switch. Section 5 of the same report says NI and IK use different Phase 90 loops, and the Color-on structure is a fit to one plugin.
- voicings "Shared building block": Guitar Rig 6 "needs to run at twice the host rate" carries a [decompiled] tag. It was never read in code (report section 8 says so). My rate test supports it; it is still an inference.
- Report 5 and the gap table: "NI's break frequency runs 918 to 3525 Hz" and "Phase 90 918 - 3525 Hz ... [measured]". Those are tubamp's fitted constants. The decompiled NI law gives 891 to 3173 Hz at stock depth (report 3.1).
- Report 4.3: "+4.7 dB without the unit delay [inferred]". Wrong number, see item 1.
- voicings "The notch-2 gap is the unit delay" is tagged [inferred], but beta also differs (0.4167 against 0.4006), so the cause is open.
- Report 4.1: "slow takes read about 2.5 % short because they hold fewer than three periods". True, and it means the period column is not a measurement of the law. My direct readings match the law to four digits.

### Fences

- Only Guitar Rig 6 and AmpliTube 5 were disassembled. Neither is on the PACE list. Phasis was measured black-box. No uaudio, Soundtoys, Relab, Sonnox, Antares or Lexicon file is in the author's work directory.
- No decompiled listing in the report or evidence is longer than about ten lines.
- The prototype and voicings.md contain no vendor table and no vendor code sequence. Their constants are circuit values or fits.
- Borderline: `evidence/tools/gr6_mxr.py` (58 lines) is a transcription of NI's process loop with NI's six polynomial constants. It is marked analysis-only. It must not reach `src/`.
- Write scope respected: `git status` shows only untracked `docs/research/fx/`, `tools/re/`, `.audit/`.

### Tool problems found

- `ptrack.py` min and max are single-frame extremes of a track that jitters about 5 % while the notch moves. Its peak reading is about 1 dB high on a moving sweep.
- `sig.py thd` level is 2.37 dB above dBFS peak.
- The system disk filled during my renders. I deleted my own scratch WAVs; nothing of the author's was touched.

## Overall judgment

The structure in voicings.md is sound and I could confirm it against the binaries, the Eichas circuit and my own renders: four stages, the stage-2 feedback node, the delay-free loops, the sweep laws, the LFO shapes, linearity, and the Small Stone constants (first notch within 1.1 %, Color-on static response within 0.33 dB rms). tubamp can be built from it. Three things should be corrected first, because the spec's acceptance numbers were taken from a jittery tracker. The Phase 90 reference sweep at noon is 465 to 1522 Hz, not 444 to 1544 Hz, and the prototype is 3 % wide at noon and 6 to 8 % wide at the fastest rate, so the 6 Hz gate smoothing needs a refit. The block resonance is +3.8 dB, not +4.7 dB. The 24 kOhm feedback resistor is a guess that the cited parts list does not support. The Color-on voicing is a fit to one plugin and is 2 to 3 dB off above 8 kHz and below 100 Hz.
