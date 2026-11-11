# EQ study: independent verification

Verifier scratch (scripts, disassembly slices, my models): `$RE_STATE/work/eq-verify/`. Binaries: SE = Solid EQ, GR = Guitar Rig 6, AT = AmpliTube 5, TB = tubamp.
All renders ran under `perl -e 'alarm N; exec @ARGV'`. Rendered WAVs were deleted after analysis (the system disk was nearly full).

Result: 18 VERIFIED, 2 NOT VERIFIED, 0 INCONCLUSIVE. One further load-bearing statement that is not a row in `claims.tsv` (Solid EQ "skips flat bands") is also contradicted; see section 2.

## 1. Claims table

| id | verdict | what I checked (command or address) | what I saw | note |
| --- | --- | --- | --- | --- |
| C01 | VERIFIED | `disasm.py SE --dump 'Biquad::setPEQ\('`, read 0xed280..0xed51c in full; stubs by `otool -Iv`; black-box fit in `se_cramp.py` | 0xed3a0 forms w0^2 - pi^2. 0xed3a4..0xed3e0 builds the two-lane ratio and `fsqrt d15` = G1. 0xed420 and 0xed448 call `_tan` with w0/2 and Dw/2. C, D, A, B and the five coefficients follow at 0xed450..0xed4f8. | My own Orfanidis bell, with the code's bandwidth form, fits the measured HF bell to 0.0000 dB at 44.1 kHz and 0.0004 dB at 96 kHz. The best cookbook bell misses by 9.7 dB. |
| C02 | VERIFIED | `disasm.py SE --addr 0xfd9a4`, loop 0xfdbe0..0xfdcb4 | `fcvt d3,s1` at 0xfdc04. Four stages of `fmul, fmadd, fmadd, fmsub, fmsub` on d registers (b1 x1 + b0 x + b2 x2 - a1 y1 - a2 y2). `fcvt s2,d31` at 0xfdc7c. State stored as doubles at 0xfdcb8..0xfdcf4. | Direct form I, double state, as claimed. |
| C03 | NOT VERIFIED | `disasm.py SE --dump 'G_EQ::onSetSampleRate'`; `otool -tV` for callers of `setParameterSmoothing`; a parameter step during a render (`vhost`, HMF Gain 0.5 to 1.0 at frame 24000, 2049 Hz sine, block 64) | The constant is there: 0xefb18 loads 0x3ca3d70a (0.02f), times the sample rate (s8), divided by the int argument, rounded, floored at 1. But the plugin's audio constructor (`ComponentAudio<SolidEQAU,...>` at 0xf70c..0xf770) builds `G_EQ(true)` and then calls `G_EQ::setParameterSmoothing(false)`. That is the only call in the binary, and the function is not virtual. Measured: the envelope goes from -20.0 dBFS to -1.3 dBFS within 48 samples (1 ms) of the step and stays there. | The 20 ms ramp exists in the DSP class but is switched off in the shipped Audio Unit. A knob move is applied at once. |
| C04 | NOT VERIFIED | `disasm.py SE --addr 0x10f768`; dispatcher `--addr 0xf4018`; same step measurement as C03 | The code reads as claimed: per-sample loop, four `fadd.4s` advance 16 values, 12 calls to `calcFilters`. But the dispatcher sets mode bit 0x20 only when the smoothing-enable byte at +0x13da0 is non-zero (0xf406c `ldrb`, `cbz`). That byte is cleared at construction (see C03). So variants 32..63, including Modes 44, cannot be reached in this plugin. | True of the code, false of the product's behaviour. |
| C05 | VERIFIED | My own impulse renders, `se_cramp.py`: ids 7=1, 14=1, 12=1.0, 13=0.75 at 44.1 and 96 kHz, relative to a flat render | 44.1 kHz: 9.65 / 19.21 / 22.63 / 15.69 / 11.32 dB at 10 / 14 / 16 / 18 / 20 kHz, 9.76 dB at 0.495 fs. 96 kHz: 22.71 dB at 16 kHz. Same as the table to 0.01 dB. | Wording: the peak is +23.38 dB at 15.5 kHz; +22.63 dB is the value at 16 kHz. A cookbook bell with the same f0, gain and Q has +0.17 dB at 0.495 fs. |
| C06 | VERIFIED | My own renders and half-gain search, `se_q.py`, 48 kHz, Q knob 0.5 | E: 0.476 at +0.84 dB, 0.427 at +20.22 dB. G: 0.289 at +0.82 dB, 0.427 at +20.22 dB. | Tolerance 0.01 in Q. "Constant" in E means a 10 % drop. |
| C07 | VERIFIED | `auhost render aufx Tamp Tuba`, Amp EQ On 0 against 1 with 5/5/5; the author's noise file and my own noise (different seed) | Author's file: 175929 of 176400 differ, -99.5 dB. My noise: 175960 differ, -99.5 / -89.5 / -80.3 dB at 44.1 / 96 / 192 kHz. Two "off" renders are bit-identical. | Tolerance 0.5 dB. See section 3(b) for what was measured. |
| C08 | VERIFIED | Same host, my own 82.4 Hz sine at -20 dBFS, least-squares removal of the fundamental | off: -136.2 / -134.3 / -132.4 dBc. flat: -99.0 / -88.6 / -79.2. bass 10: -89.0 / -77.7 / -69.6. | Matches to 0.1 dB. This residual is float state rounding, not fused multiply-add: my model gives -68.5 dBc without it and -69.3 dBc with it at 192 kHz. |
| C09 | VERIFIED | `disasm.py TB --addr 0xe5d5c`, loop 0xe60c0..0xe60e4; my model `vmodel.c` (explicit `fmaf`, built with `-ffp-contract=off`) | `fmadd` at 0xe60c4, 0xe60d0, 0xe60dc. The loop computes lv1 = lv2 + fma(x, b1, -(y*a1)), so one product is rounded and the other is not. Model with that pattern: 99.73 % differ, -99.4 / -89.3 / -80.2 dB. Model without it: zero differing samples at 48, 96, 192 kHz. | At 44.1 kHz the treble band has b0 = 0.99999994, so even without fused multiply-add 95 % of samples differ, at -125 dB. |
| C10 | VERIFIED | Recomputed in numpy float32 from the JUCE formulas (`cramp.py`), 4000 log points 20 Hz..20 kHz | Treble shelf +-10 dB: 0.043 dB at 44.1 kHz. Middle +15 dB: 0.028. Middle -15 dB, Q 1.5: 0.007. Bass +-20 dB: 0.008. All three at 10: 0.033. Bell 8 kHz Q 1 +12 dB: 2.583. Shelf 8 kHz: 0.828. | See section 3(a). |
| C11 | VERIFIED | My own models (`vmodel.c zip`, `vmodel2.c`) and my own analysis (FFT brick-wall and a 4801-tap FIR high-pass) | Shipped policy, 512-sample blocks, 100 Hz: -62.5 dBFS peak (-61.7 with the block grid shifted by half a block). 64-sample blocks: -66.7. Two-pole smoothed knob, 16-sample redesign: -92.8 (claim -93.4). Stepped SVF: -56.8. Interpolated SVF, two poles: -93.9. | Tolerance 1 dB. It is a model, not the plugin. See section 3(c). |
| C12 | VERIFIED | `disasm.py GR --dump '^NI::DSP::EQ_Param::setFilter\('` and `processBuffer`; stub names by `otool -Iv` | 0x2798cc..0x27990c: 192000.0f and 96000.0f compares, floors 50.0f and 25.0f. 0x279918: 6.2831855f. 0x279948: divide by 2 fs, result stored as the ramp target at +0x150. setFilter calls only `sincosf`, `atan2f`, `hypotf` and complex multiply. processBuffer has no calls. No `_tan` or `_tanf` in either. | The recheck greps only for `_tan`; I also checked `_tanf` (stub 0x2757348) and the per-sample path. |
| C13 | VERIFIED | My own noise renders and cross-spectrum (`gr_v.py`), state written with the author's `gr6state.py` | 96 kHz: 20 kHz setting peaks at 17718 Hz (prediction 17709), 10 kHz setting at 9696 Hz. Gain ends +20.00 / -20.02 dB. 48 kHz: 17701 Hz, 16 samples latency. 44.1 kHz: 17377 Hz, 16 samples latency. | Tolerance 20 Hz (bin 5.9 Hz, noise estimate). At 48 kHz a filter at the host rate would peak at 14032 Hz, so the 2x internal rate holds. |
| C14 | VERIFIED | `disasm.py AT --floats 0x26af59c 7` and `0x26af5b8 10` | 100, 200, 400, 800, 1600, 3200, 6400. 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000. | I did not check which module loads which table. |
| C15 | VERIFIED | `disasm.py AT --addr 0x1033848`; callers in `--addr 0xf54c28` | 0x10338e0 `fsub s26,s23,s18` (1 - t). Each of five coefficients is old*(1 - t) + t*new. 0x1033960 adds 1/N to t per sample. `ParametricEQ::Rack::Process` calls it five times. | |
| C16 | VERIFIED | My own noise renders (`c16.py`, different seed), bx_console and UADx EQP-1A, black-box only | bx_console at 20 kHz: 4.96 dB at 44.1 kHz, 14.14 dB at 96 kHz. EQP-1A 16 kHz boost: 18.92 against 19.06 dB at 16 kHz. EQP-1A both at 10: +12.12 dB at 60 Hz, -6.51 dB at 500 Hz. | The largest EQP-1A rate difference is 0.21 dB at 18 kHz, a hair over the stated 0.2. |
| C17 | VERIFIED | Algebra on the SVF transfer function; my own SVF and biquad in `c18.py`; `design.py svf` and `orfsvf` rerun | The three mix forms reduce to the cookbook bell, low shelf and high shelf exactly. The Orfanidis mapping gives 0.000000 dB against the Orfanidis biquad over my whole grid. | |
| C18 | VERIFIED | My own grid (`c18.py`): 40 centres 100 Hz..8 kHz, six gains, 1500 points to 20 kHz, 44.1 kHz | Q 0.7: 0.94 dB decramped, 4.81 cookbook. Q 1: 0.52 and 3.29. Q 0.3: 9.58 and 8.33. | The Q 0.3 figure depends on the grid and the upper limit (claim 12.75 and 8.16). The conclusion holds: worse than the cookbook bell. |
| C19 | VERIFIED | My own nodal solver (`stack_v2.py`) against `yeh_coeffs`, four voicings, 125 interior settings, 400 frequencies | Worst difference 0.000000 dB. Noon dip: tweed 774 Hz, 9.7 dB below the lower shoulder. Marshall 734 Hz, 6.3 dB. | My first two guesses at the topology did not match (11 dB and 2.2 dB off). The match needs the series chain drawn in Yeh's Fig. 1. "Noon" uses the 30 % taper on bass and middle. |
| C20 | VERIFIED | WebFetch of both PDFs, then `pdftotext` on my own downloads | Vicanek section 3.3 has eqs. 13-19 and "f0 < (1 - 1/(2Q)) fNy". Yeh has the Bassman values, b1 and b2 as transcribed, a SPICE section and the bilinear section. | Vicanek treats G1 as a given. The G1 formula used in R3 is not in eqs. 13-18; it is eq. 17 evaluated at the Nyquist frequency. |

## 2. Solid EQ: extra statements checked

- **Orfanidis bells, double direct form I.** Confirmed by code and by measurement (C01, C02, C05).
- **20 ms ramps.** Contradicted for the shipped plugin (C03, C04). The step test shows an instant change.
- **"Skips bands that are flat" (Summary, 3.1, 5.5, R2).** Contradicted. I counted the second-order sections in all 64 loop variants. Every variant has at least four (8 `fmsub` in Modes 12, 16, 20, 24, 28; more in the others). No variant drops any of the four main bands. The five flag bytes that form the mode word are written in `calcFilters` from tests of the form "parameter value > 0.5" (0xeff84, 0xf04c0, 0xf1030, 0xf1304, 0xf1f08, 0xf25bc, 0xf36c0). Those are switch states (E/G model, bell or shelf, filters in or out), not "gain is zero". What is true: `setPEQ` returns identity coefficients when the gain or the frequency is zero (0xed2b0..0xed2e4), so a flat band is an exact pass-through in double arithmetic. It still runs.
- **Top bell near Nyquist against a plain bilinear bell.** At 44.1 kHz the measured bell has +9.76 dB at 0.495 fs. A cookbook bell with the same centre, gain and Q has +0.17 dB there and 7.00 dB at 20 kHz, where Solid EQ has 11.32 dB.

## 3. The claims about tubamp

**(a) No cramping, 0.04 dB.** True. I recomputed the coefficients in float32 exactly as `juce_IIRFilter.cpp:208-275` and `juce_IIRFilter_Impl.h:41-56` do. Worst deviation from the analog prototype over 20 Hz..20 kHz at 44.1 kHz: treble shelf 0.043 dB, middle 0.028 dB, bass 0.008 dB, all three together 0.033 dB (boost) and 0.037 dB (cut).

**(b) Flat is not a pass-through.** True, and the stated cause is demonstrated, with two limits.

- What was measured. `auhost` loads `~/Library/Audio/Plug-Ins/Components/tubamp.component` (checked with `DYLD_PRINT_LIBRARIES`). Its binary has the same SHA-1 as `build/tubamp_artefacts/Release/AU/.../tubamp` in the main checkout. That build is from `main`, which is three commits ahead of this worktree (reverb and delay engines). `ToneStackEQ` is identical in both trees. A second, older, universal copy sits in `/Library/Audio/Plug-Ins/Components`; it was not the one measured.
- The default chain is the amp block alone (`ChainOrder.h:298-301`), with no model loaded. "Amp EQ On" gates only `ampEq.setParameters` and `ampEq.process` (`PluginProcessor.cpp:1241-1246`). The comparison is on against off, so the other always-on stages cancel. They are not transparent themselves (the off render differs from the input by -31 dB after gain matching), but they are linear and deterministic: two off renders are bit-identical. The residual is from the tone stack.
- Pure arithmetic predicts it. Three flat cookbook biquads in float32, transposed direct form II, white noise at -12 dBFS peak:

| fs | no fused multiply-add | fused as in the binary | measured on the plugin |
| --- | --- | --- | --- |
| 44.1 kHz | 95.4 % differ, -125.0 dB | 99.73 % differ, -99.4 dB | 99.75 %, -99.5 dB |
| 48 kHz | 0 differ | 99.78 %, -98.1 dB | -98.3 dB (author) |
| 96 kHz | 0 differ | 99.91 %, -89.3 dB | 99.92 %, -89.5 dB |
| 192 kHz | 0 differ | 99.97 %, -80.2 dB | 99.97 %, -80.3 dB |

- Limit 1. The -70 dBc at bass 10 and 192 kHz is not caused by fused multiply-add. It is float state rounding in the direct form. The report says this in section 2; the Summary and the gap table blur the two.
- Limit 2. This is the arm64 build. On x86_64 clang does not fuse by default, so flat would be bit-exact there except at 44.1 and 88.2 kHz. The report does not say so. `snapToZero`, which the report lists as part of what we ship, is a no-op on arm64 (`JUCE_SNAP_TO_ZERO` is defined empty unless `JUCE_INTEL`).

**(c) Clicks at -62.5 dBFS.** True as a model result. The model matches the source: the knob range has a 0.1 step (`Parameters.cpp:95-97`), `setParameters` runs once per host block and swaps coefficients in place while the filter state is kept (`FxBlocks.cpp:155-186`). My own implementation, with all three bands and the binary's arithmetic, gives -62.5 dBFS peak above 1 kHz (512-sample blocks, 100 Hz at -12 dBFS, bass 5 to 10 in 0.5 s). Two different high-pass methods agree to 0.1 dB. Shifting the block grid gives -61.7. It scales with block size: -56.9 at 1024, -51.0 at 2048. With 64-sample blocks most of the click is the 0.1 quantisation: -66.7 quantised, -81.0 unquantised. Nobody has measured this on the plugin itself.

## 4. Top three recommendations

**R1, knob smoothing.** The need follows from C11 (verified, model only). The fix level (-93.4 dBFS) is reproduced by my model at -92.8. The supporting sentence "Both Native Instruments products ramp over 20 ms" does not hold for Solid EQ: the shipped plugin does not ramp (C03). For Guitar Rig the ramp is read from code only (0.02 constant in `EQ_Param::onSetSampleRate`, no enable flag in `onControlRate`); it was not measured. AmpliTube's per-sample crossfade is verified (C15).
The statement that static renders stay bit-identical follows from the sketch, not from a test. It holds if (1) the smoother starts at the target and (2) `design(5)` gives the coefficients `prepare()` builds today. I checked (2): `decibelsToGain(0)` is exactly 1.0f and the middle Q picks the boost value at 0 dB, as today. Unstated condition: static blocks must still be processed in one call. On Intel builds `snapToZero` acts at the end of every process call, so cutting a static block into 16-sample pieces would change low-level tails. On arm64 it makes no difference. The sketch also never sets `moving = true`.

**R2, new core and `eq_type`.** Rests on C07, C08, C09 and C17, all verified. Double state fits what Solid EQ does (C02). Two weak points. First, "skipped ... as Solid EQ does" is wrong: Solid EQ does not skip flat bands (section 2). The design does not need it, because m = (1, 0, 0) is exact, which I confirmed by algebra for the bell and both shelves; the tilt and the cut filters are not exact at their neutral settings and do need the skip. Second, the report frames the flat residual as a precision problem. The cause at flat is fused multiply-add, so cheaper fixes exist for Classic (skip a band at knob 5, or build the filter loop without contraction). The report mentions the first only in section 8 and not the second. The float-SVF comparison "at least 25 dB cleaner" is not a claims row; my model gives -123 / -121 / -117 dBc where the report says -117 / -122 / -102, so the conclusion holds and the exact numbers do not reproduce.

**R3, decramped bell above 2 kHz.** Follows from C01, C05, C18 and C20, all verified. "Solid EQ does this for every bell" is supported by the black-box fit. The G1 line is cited to Vicanek eqs. 13-18 but is not there (see C20); it is correct. The 1.5 to 2.5 kHz blend is a guess and is tagged as one. The Q 0.3 failure figure depends on the grid.

## 5. Citations opened

- `https://www.vicanek.de/articles/BiquadFits.pdf`: exists. Section 3.3 "Orfanidis Peaking EQ", eqs. 13-19, the validity condition, and "Oversampling provides remedy but at a high CPU cost" are all there.
- `https://ccrma.stanford.edu/~dtyeh/papers/yeh06_dafx.pdf`: exists. Bassman values, coefficient expressions, SPICE verification and bilinear discretisation are there.
- `https://duncanamps.com/technical/tonestack.html`: exists. It says the circuit "cannot offer any kind of mid boost", gives 20 dB of variation at 81 Hz and 26 dB for treble, and uses "30% of resistance at midway".
- `https://www.ranecommercial.com/legacy/note122.html`: exists. It defines proportional-Q and constant-Q as the report says.

None failed. One imprecision: the G1 formula attributed to Vicanek (above).

## 6. Inferred or guessed items presented as fact

- Summary bullet 3 and section 5 items 4 and 5: Solid EQ "ramps parameters over 20 ms and skips bands that are flat", tagged [decompiled]. Both are readings of code that the shipped plugin does not execute that way. Both are false as behaviour.
- Summary bullet 2: the -62 dBFS clicks are tagged [measured]. They come from a model. Section 8 says so; the Summary does not.
- Summary bullet 2 and the gap table: "the single-precision filter is not transparent". At flat the cause is fused multiply-add on arm64, not single precision.
- R1 evidence: "Both Native Instruments products ramp over 20 ms" stated as fact.
- R2 design: "is skipped once its ramp has finished, as Solid EQ does" stated as fact.
- Section 5 item 3: "Guitar Rig runs the rack at twice the host rate" is tagged [measured] only. It is an inference from peak placement and latency. My measurement supports it.
- Gap table, "consequence for the player": "audible clicks" and "a noise-like residual that a high-gain amp after the EQ raises" are untested inferences without a tag. So is "players expect pedal behaviour" under Not recommended.
- Not checked by me: Summary bullet 4 (AmpliTube Pultec and Guitar Rig tone stack as state-space models) and the other section 3.2 and 3.3 items that are not claims rows.

## 7. Fences

- No PACE-protected binary was disassembled. The disassembly files in `$RE_STATE/work/eq/` cover AmpliTube 5, Guitar Rig 6, Solid EQ and tubamp only. The UADx and Plugin Alliance folders hold parameter lists, scripts and measurement tables. The extra host source in the UADx folder is the stock host; it has no debugger or memory access.
- Decompiled excerpts in `evidence/` are 43, 29 and 21 lines in total, annotated, with no single listing over 30 lines. The report pastes none.
- No vendor table is in the design. The Solid EQ splines are named and not taken. Tone stack component values come from literature. Two things to be aware of: the seven graphic centres in R5 are the same numbers as the AmpliTube table at 0x26af59c (they are also the plain octave series from 100 Hz), and the G1 expression in R3 was read from the Solid EQ code path, though it is Orfanidis' published formula.
- I loaded no AmpliTube 5, UADx API Vision or Hitsville plugin. My only additions are scratch files, including a copy of the host with one extra option to step a parameter during a render.

## 8. Overall judgment

The measurements and the arithmetic in this report are sound: every number I re-rendered or recomputed came back within tolerance, and the three tubamp findings (no cramping, flat is not a pass-through on the arm64 build because of fused multiply-add, block-rate coefficient steps click in a faithful model) hold. The reading of Solid EQ's behaviour is where it fails. The 20 ms ramp and the per-sample redesign exist in the DSP class but are disabled in the shipped Audio Unit, where a knob step lands within 1 ms, and the "flat bands are skipped" reading is wrong: all four bands run in every loop variant. Those errors sit in the Summary, in the industry-practice section and in the evidence lines of R1 and R2. They do not remove the case for R1, R2 or R3, which stand on tubamp's own defects and on verified design numerics, but the report overstates how much the best reference agrees with them, and it should say that the flat residual is an arm64 code-generation effect with cheaper fixes than a new filter core.
