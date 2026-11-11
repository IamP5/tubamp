# Cab study: independent verification

Verifier's scratch: `$RE_STATE/work/cab-verify/` (`jucemodel.py` is my numpy model of the JUCE loader, `an_ours.py`, `tx.py` are my re-measurement scripts).
Binaries: GR = Guitar Rig 6, AT = AmpliTube 5, T3 = TONE3000, TX = TONEX. J = `juce_Convolution.cpp`, RAS = `juce_ResamplingAudioSource.cpp` (JUCE 8.0.15).
Tolerances I accepted: 0.1 dB on levels, 0.1 sample on delays, 15 % relative on CPU.

Result: 20 VERIFIED, 0 NOT VERIFIED, 0 INCONCLUSIVE. Several rows pass only with a scope caveat. The caveats matter for the recommendations and are listed below the table.

## Claims

| id | verdict | what I checked | what I saw | note |
| --- | --- | --- | --- | --- |
| C01 | VERIFIED | `src/dsp/FxBlocks.cpp:656-660`, `FxBlocks.h:187`, J:1206-1208, J:740-745. Also disassembled `tubamp::CabSim::loadImpulseResponse` in the loaded AU binary (0xe89b0..0xe89c0). | Source: `Stereo::yes, Trim::yes, 0, Normalise::yes`; member is a default-constructed `Convolution`; default ctor is `Latency{0}`. Binary: the queued load stores 1, 1, 0, 1 for stereo, trim, size, normalise. | Source and measured binary agree on the call. |
| C02 | VERIFIED | J:571-621, J:623-651, J:653-674, J:785-790; RAS:153-166, RAS:210-225. | Trim threshold `decibelsToGain(-80)` on raw sample values, applied before resampling. `0.125f / sqrt(sumSq)` applied after resampling. Linear interpolation between two samples; one 2nd-order Butterworth at `0.5*ratio` (upsampling, after) or `0.5/ratio` (downsampling, before). | |
| C03 | VERIFIED | Re-rendered `cab44` at 44.1/48/96 kHz through the AU, 60 s warm-up. Also my numpy model. | AU: -18.06 / -17.52 / -14.56 dB at exactly 1 kHz, L2 -18.06 each. Model: -18.06 / -17.52 / -14.56. Claim: -18.07 / -17.55 / -14.49. | Differences up to 0.07 dB come from the author reading the nearest FFT bin. The 96 k against 48 k step is 2.97 to 3.06 dB depending on that; theory is 3.01. |
| C04 | VERIFIED (scope caveat) | Re-rendered `bright44`, `bright48`, `bright96` at 48 kHz; model from JUCE source. | AU and model both: -1.83 / -2.51 / -3.72 dB at 12 / 14 / 16 kHz, +0.53 dB at 1 kHz, residual -17.0 dB (model -16.7). | The numbers hold for `ir_bright44.wav` only. That file is a cab response with artificial 9 kHz and 14 kHz bursts added; its 14 kHz content is as loud as its 4 kHz content. On the unmodified cab IR the error is under 0.25 dB below 6 kHz and the residual is -27.8 dB. |
| C05 | VERIFIED | Re-rendered with `--warmup 0.5` at 44.1/48/96 kHz and with two different IR files; model of the default IR (one sample of 1.0 at 44.1 kHz, `Normalise::no`). J:805-815, J:1055-1076. | AU: 0.00 / -2.30 / -7.24 dB at 1 kHz; two IR files give bit-identical output. Model default IR at 48 kHz is one sample of 0.76694 (-2.30 dB); at 96 kHz two samples, -7.24 dB at 1 kHz. AU take equals 0.76694 x the dry response, residual -124 dB. | See (d) below for when this happens and how long it lasts. |
| C06 | VERIFIED | Re-timed 4 cells (best of 2) with the author's states and `noise120.wav`. | 2229 samples: 0.46 % at 32, 0.16 % at 512. 96000 samples: 16.3 % at 32, 0.75 % at 512. Claim: 0.49, 0.14, 16.1, 0.84. | The 0.14 % figure is at the noise floor (a 1-sample IR costs 0.12 %). Other agents were using the machine. |
| C07 | VERIFIED | `disasm.py GR --dump NI::DSP::Convolver::processBuffer`, 0x619fd0..0x61a070. | Per input sample: loop of 32 (`cmp x11,#0x20`) `fmul.4s`/`fadd.4s` into a 32-slot ring (`and #0x1f`), then lane 0 is output and the lanes shift by one. That is a transposed 128-tap FIR. FFT output is added after it. | |
| C08 | VERIFIED | Read 4528 ints at 0x464b598; symbol size from `nm -n`; xref in `setImpulseResponse` 0x615638..0x615668. | 283 rows of 16 ints. Row 25: `1920 1 64 28`. Row 26: `2048 2 64 10 256 5`. Row 283: `67108864 7 ... 1048576 56`. `setImpulseResponse` clamps to the last row's length and scans rows of 0x40 bytes. | Row lengths equal 128 plus the partition sum, which fits the FIR head of C07. |
| C09 | VERIFIED | `ConvolutionBase::onSetSampleRate` 0x2699c0..0x269af8; `onControlRate` 0x26ac8c..0x26af5c. | 0x3ca3d70a = 0.02 loaded at 0x2699e0 and 0x269a8c, times fs, over block size, rounded, stored at +0x2fc with its reciprocal at +0x300. Mode 2: rate x 0.5 at 0x269a40, two `SampleRateConverter::init(...,2,2)`, inner convolvers get the half rate. `onControlRate` ramps with +0x300 across the two convolvers at +0x2a0 and +0x2a8. | |
| C10 | VERIFIED | `ControlRoomCustom8::calcIR` 0xb302c..0xb31ac and 0xb35c0..0xb3600. | 0x3d23d70a = 0.04, 0x43ab999a = 343.2. Delay = phase x fs x 0.04 / 343.2, split into floor and fraction. Polarity `fcsel +1/-1`. L = 2f(1-pan), R = 2f x pan. Then `log(N)` x -8.6859 (0xc10af968) + (-12.0). | The log call is a stub; -8.6859 = -20/ln 10 only makes sense with a natural log. |
| C11 | VERIFIED | Re-rendered `mc_off`, `crp_s1`, `crp_s1_ph1`, `crp_s1_ph0.5`, `crp_s1_inv`, `crp_s1_pan0` with the author's states; my own cross-correlation and a phase-slope fit. | Phase 1: 5.44 samples by cross-correlation, 5.41 by phase slope. PhaseInv: 0.00 samples, polarity -1. Peaks at 20 against 16. Pan 0: L +6.02 dB, R silent. | Code predicts 5.59 samples at full scale. The measured value is 0.15 short and lower settings are not linear, so the parameter is mapped before it reaches `calcIR`. |
| C12 | VERIFIED | `tellAmpImpedance` 0x26b5f8; `RigPlexi::setCabinetImpedance` 0x2db5a0..0x2db650; floats at 0x2c43644; symbol size. | Table is 2700 bytes: 28 rows of 24 floats plus 12 bytes of padding. Row 28 is all zeros with unit gains (index 27 is skipped by `cmp w1,#0x1b`). First poles -6.0475 +- 52.2875, magnitude 52.6. The setter maps roots with constant fs x 0.31831 (= 2fs/2pi), so the table is in Hz. | The report says 27 records, the claim 28. Both are right: 27 real plus one null row. |
| C13 | VERIFIED (static) | `Cabinet::OnSampleFrequencyChange` 0x154a9e0..0x154aa0c, 0x154b760..0x154b770; `Conv::IR::IR` 0x102bc78..0x102bda4. | 0x47ac4400 = 88200.0; `csel` picks 2048 below, 4096 from 88.2 kHz. Call is `IR(ptr, len, 1, 128)`. With the bool set and len < 8192 the ctor copies `min(len, 128)` samples as a head and gives the rest to `UniformSegmentedIR`. | "zeroLatency" is the author's name for the bool. The structure fits it. Not run. |
| C14 | VERIFIED (static) | `UpdateImpedance1Left` tail call 0xf1ad70; floats at 0x3171fe8; symbol size. | Ends in `b StateSpace64<4,1,1>::Bilinearize`. 5356 bytes = 103 x 52. First record: int 1, 16.0, 12.5, 23.32, 20.85, 53.09, ... | Reading the fields as ohm, henry, farad is the author's inference. 1/(2 pi sqrt(0.01852 x 0.000305)) = 67.0 Hz checks out under that reading. |
| C15 | VERIFIED | `prepareBlockModelOffThread` 0x69040..0x690e4, 0x693f0..0x69428. | `x24 = fileRate x 10`. `w26` = IR length at the processing rate; `cmp w26, #48001` selects `Convolution(NonUniform{8192})`, else the default ctor. Load call has `w2=0` (Stereo::no), `w3=1` (Trim::yes), `x4=x24`, `w5=0` (Normalise::no). | The threshold is 48000 samples at any rate, so "1 s" in report 3.3 is true only at 48 kHz. |
| C16 | VERIFIED | `processChainOnBuffer` 0x411a0..0x411d8; string search. | `fmov s0,#-18.0` selected when block type == 1 and the byte at +0x5e0 is 0; times 0.05; then a pow10 stub. UI string "IR pre-trimmed -18 dB" exists. | The trim is skipped when the +0x5e0 flag is set. I did not identify that flag. Block type 1 = IR rests on the UI string. |
| C17 | VERIFIED | Authored fresh TONEX states and re-rendered `blend 0, 1, 0.5, 0.25` (my `tx.py`). | 0.5000/0.5000, residual -79.8 dB. 0.7500/0.2500, -83.2 dB. Cross-correlation delay -1.69 samples. | One mic pair, one position pair, at 48 kHz through TONEX's internal 44.1 kHz resampling. |
| C18 | VERIFIED | Re-rendered Resonance 0, 0.5, 1; Presence 0, 1; Depth 0, 1, amp model off. | Resonance 1 against 0: 10.3 dB at 109 Hz, half-gain band 84..143 Hz, 11.3 dB at 20 kHz. Presence +-6.0 dB at 20 kHz. Depth +-6.0 dB at 80 Hz. | |
| C19 | VERIFIED | WebFetch of both raw GitHub URLs. | `.h`: `const size_t mMaxLength = 8192;`. `.cpp`: `dsp::ResampleCubic<float>(...)`, `pow(10, -18 * 0.05) * 48000 / mSampleRate`, output is `mWeight.dot(input)`. | |
| C20 | VERIFIED (stand-in data) | Ran `cabproto.py norm/align/load`. Recomputed the alignment case with my own code on the same two IRs. Checked the bell by hand from the report's formulas. | Norm: std 1.07 dB, 48 k-referenced gain -1.88 dB at all three rates. Align (mine): 12-sample offset gives worst band -11.51 dB; after cross-correlation -0.10 dB. Bell: Ra 8.97 ohm, Qz 1.118, Qp 2.231, peak 6.0 dB, 3.95 dB at 80 Hz. | The 1.07 dB spread is identical for JUCE's current rule. The two L2 rules differ by a constant, so the new rule does not level-match IRs any better. 25 IRs are 21 distinct ones, all from TONEX. |

## Which binary was measured

- `auhost` loads `~/Library/Audio/Plug-Ins/Components/tubamp.component` (checked with `DYLD_PRINT_LIBRARIES`). arm64 only, 8.3 MB, built 10 Nov 23:40:35, sha1 `f03ffd4a...`.
- It is byte-identical to `/Users/tuba/Dev/projects/tubamp/build/tubamp_artefacts/Release/AU/...`, the main checkout's build. This worktree has no build directory.
- A second, older copy sits in `/Library/Audio/Plug-Ins/Components` (17 Aug, universal). It is not the one loaded.
- The binary contains typed reverb and delay engine symbols that this worktree's source (ca741d8) does not have. It was built from the main checkout before those changes were committed (commits are stamped 11 Nov 00:30). I cannot tie it to a commit.
- For the cab this does not matter. The `CabSim` text in `FxBlocks.cpp` and `FxBlocks.h` is identical between this worktree and main's HEAD. The binary's `CabSim::loadImpulseResponse` passes the same four options. A numpy model written only from the JUCE 8.0.15 source reproduces the AU's output for three IR files with a residual of -96 dB.
- The cab block and the IR are really in the path: the state XML has `chainOrder="cab"` and the `irPath`, a unit-impulse IR comes out at exactly 0.125 x the cut filters, and the output follows the IR file.

## Scrutiny of the tubamp claims

Route used: both, for all four. Installed AU re-rendered, and the independent model `jucemodel.py`.

**(a) 44.1 kHz IR loses 2.5 dB at 14 kHz at another rate.** The mechanism is true and follows from the code: `ResamplingAudioSource` interpolates linearly. Linear interpolation has a sinc-squared response, -2.99 dB at 14 kHz for a 44.1 kHz source. The -2.51 dB in the report is that loss net of a +0.53 dB lift from the re-normalisation. But the number was measured on a synthetic IR with a 14 kHz burst added at the level of its midrange. On the unmodified cab IR the 14 kHz region is 64 dB down, the error below 6 kHz is at most 0.25 dB and the time-domain residual is -27.8 dB. The report's Summary and gap table state the 2.5 dB without saying the file was doctored, and conclude that a 44.1 kHz IR "sounds duller". That audible claim is not demonstrated for ordinary cab IRs.

**(b) 3.1 dB louder at 96 kHz than at 48 kHz.** True; the exact figure is 3.0 dB (2.97 measured at 1 kHz, 3.01 in theory). It follows from normalising `sum h^2` after resampling without a rate term. Between 44.1 and 48 kHz the step is 0.54 dB.

**(c) A quiet IR file changes the response.** True: trim compares raw sample values with 1e-4, so a file 40 dB down loses 932 of its 2222 samples. The effect is small. +0.57 dB at 1 kHz is a narrow-band reading in a notch. In third-octave bands from 80 Hz to 6 kHz the change is between -0.06 and +0.22 dB. The trimmed tail holds energy 33 dB below the total. A file peaking at -40 dBFS is an extreme case.

**(d) Default-IR pass-through at about -2.3 dB.** True at 48 kHz (0 dB at 44.1 kHz, -7.2 dB at 96 kHz). The default IR is one sample of 1.0 declared at 44.1 kHz with `Normalise::no`; resampling and the `44100/fs` gain give 0.767. Conditions the report does not state: it happens only when a load is issued after `prepare()` on a cab that had no engine built from a file yet. `Convolution::Impl::prepare` drains pending loads synchronously (J `popAll`), so a state restored before `prepareToPlay` does not burst, and an IR-to-IR change keeps the old engine until the new one is ready. Duration: the report's "0.5 s warm-up" is offline time. I timed it: the engine arrives about 20 to 30 ms of wall clock after the load, then JUCE crossfades for 50 ms. So the burst is roughly 70 to 80 ms, about 15 dB above the cab level. It was not observed in a real-time host.

## Top three recommendations

**7.1 Loader v2.** Partly supported.
- Rate-independent level: follows from verified C02, C03.
- Engine ready before use: follows from verified C05, with the conditions in (d).
- Sinc resampling: the defect is real, the stated audible benefit rests on a synthetic IR (C04 caveat).
- Relative trim: real but worth about 0.2 dB in the worst case tested.
- The level rule itself does not follow from the references. The report's own finding is that every reference uses a fixed trim and none normalises by energy; 7.1 then proposes energy normalisation. The -9 dB target comes only from the 25 TONEX-derived stand-ins, as section 8 admits. The 1.07 dB spread is the same as today's rule, so no gain in consistency is shown.
- `NonUniform{2048}` cost ("about 1 % instead of 16 %") is tagged [inferred] and unmeasured, yet the acceptance test (under 2 %) depends on it. That `NonUniform` is zero latency is correct by J:740-745 and the header (`Latency{}` is 0).
- "Old sessions bit-identical" is a design intention, not tested.

**7.2 Speaker-load stage.** Supported as "the references have such a stage"; weaker as "this is what tubamp needs".
- C12, C14, C18 and the two literature sources are verified. The derivation of the two sections is correct algebra and the model is the author's own.
- Unstated assumptions: that a linear filter in front of the convolution stands in for what the references do (the report did not read where Guitar Rig's amps apply the record, and AmpliTube was not run); that amp-only NAM captures lack this response ("often taken into a resistive load" has no source and no measurement); `Qm = 8`, `Le = 1 mH`, `R2 = 40 ohm` are the author's choices, not values from the cited pages.
- TONEX moves bass and treble with one control; splitting it into two is a design choice.

**7.3 Second IR with auto-align.** Blend, polarity and delay follow from verified C10, C11, C17 and the Line 6 manual. Automatic alignment does not: no reference has it (the report's own table says so). Its only support is the prototype, which re-aligns two TONEX responses that were already aligned and then shifted by a known amount. That is the easy case. Default "on" is untested on real IR pairs from different vendors.

## Citations opened

| URL | exists | says what the report attributes |
| --- | --- | --- |
| https://raw.githubusercontent.com/sdatkinson/AudioDSPTools/main/dsp/ImpulseResponse.h and `.cpp` | yes | yes: 8192 limit, cubic resampling, `-18 dB x 48000/fs`, dot-product FIR |
| https://celestion.com/?p=10987 | yes | yes: "about 75% to 85% of the nominal impedance", 55 to 100 Hz, "60 ohm or more", 20, 30 or 40+ ohm |
| https://manuals.line6.com/en/helix-stadium/live/cab-blocks | yes | yes: 2048-sample 48 kHz mono, Delay "Auto" follows mic distance (cab models), Polarity for blends, two cabs make the block stereo |
| https://www.dafx.de/paper-archive/2011/Papers/74_e.pdf | yes | yes: Wefers and Vorlaender; "has no inherent latency"; O(N^2); "it is always beneficial to partition filters" |
| https://duncanamps.com/technical/dummyload.html | yes | yes for "5 times the rated impedance, or even higher". The circuit topology is in images; the text names the parts but not the full arrangement, as `literature.md` already notes |

None failed. Not opened: the Yeh slides and the avnirvana thread.

## Inferred or unsupported items presented as fact

- Summary line 1 and gap table: "loses 2.5 dB at 14 kHz on a 44.1 kHz file", "sounds duller". Measured on a doctored IR; not general.
- Summary line 2: "Every reference keeps the convolution at zero latency". AmpliTube is static reading only (table 5 itself says [inferred]). TONEX's engine was not read, and its VIR onset is 7 samples later than its bypassed chain at 44.1 kHz. Plugin Alliance was not isolated.
- Summary line 2: "none normalises user IRs by energy". TONEX's custom-IR handling was not read.
- Summary line 3: all three "model the speaker's impedance between amp and cab", tagged [decompiled]. For TONEX it is inferred from the response of one control. For Guitar Rig the report says elsewhere that where the amp applies the filter "was not read".
- Summary line 4: "Multi-mic blends are done on the impulse responses". True for Guitar Rig. AmpliTube runs one convolver per mic (report 3.2), so it blends audio.
- 7.2 Evidence: "NAM captures of an amp head are often taken into a resistive load; they lack this response." No tag, no source.
- Gap table: "amp-only captures ... sound flat in the bass and soft on top". No tag, no measurement.
- Gap table: "a burst of uncabbed amp sound on preset load". Depends on the host's call order; not observed in a host.
- AmpliTube unit reading (ohm, henry, farad) is tagged [inferred] in 3.2 but 7.2 cites the module as a "loudspeaker circuit" under [decompiled].

## Fences

- No PACE-protected binary was disassembled. Static work touched Guitar Rig 6, AmpliTube 5, TONE3000 and TONEX strings only. Plugin Alliance was black-box. The author's work directory holds no dump of a uaudio, Soundtoys, Relab, Sonnox, Antares, Lexicon or Neural DSP binary.
- Longest code block in the report is 18 lines (our own pseudo-code). Longest disassembly run in evidence is 15 lines (`tone3000_ir.md`). Under the 30-line limit.
- No WAV, NPY or preset is stored under `docs/research/fx/cab/`. Evidence holds three of AmpliTube's 103 impedance records and a few partition rows as excerpts, not the tables. Full dumps stay in the scratch directory.
- TONEX-measured responses were used as test IRs and to fit the -9 dB target. They are not stored in the repo and not proposed for shipping.
- The speaker-load model is the author's own: a lumped speaker derived in the report, with constants from literature ranges plus three chosen values. No vendor record is reused. The Air proposal says it will not reuse the reference's delays.

## Other observations

- The scratch volume filled up during this run (184 MB free at one point). I deleted my own WAVs and dumps and kept only small scripts. The disk is still at 100 %; later renders by any agent may fail.
- `--strings` on TONE3000 with a loose pattern prints megabytes of embedded JavaScript. Use a tight pattern.
- Report 3.1 says Phase full scale "is the 0.04 m / 343.2 m/s of the code". Measured 5.44 against 5.59 predicted; close, not equal.

## Overall judgment

Every row of the claims table reproduces, by disassembly at the named addresses, by re-rendering, or by an independent model of the JUCE loader that matches the installed AU to -96 dB. The measured tubamp binary is the main checkout's build, not a build of this worktree, but its cab code is the same and behaves exactly as the JUCE source predicts. The facts are sound. The weight the report puts on them is not always. The resampler headline uses a synthetic bright IR and overstates what a player would hear; the quiet-file effect is about 0.2 dB; the default-IR burst is real but short and conditional. Loader v2 is still justified by the 3 dB rate dependence and the burst, but its level rule contradicts the report's own finding about reference practice and rests on stand-in data. The speaker-load and second-IR proposals are reasonable designs whose need (resistive-load captures, auto-align on real IR pairs) is assumed rather than shown.
