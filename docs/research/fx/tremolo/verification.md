# Tremolo report: independent verification

Verifier scratch: `$RE_STATE/work/tremolo-verify/` (re-rendered WAVs in `bb/` and `bbtx/`, regenerated tables in `recheck/`, my own envelope analyser `myenv.py`, disassembly dumps). Nothing in the report, claims, evidence or prototype was edited.

Method. Decompiled rows: I dumped the named function with `disasm.py`, read the instructions, decoded the literals, and followed the loaded value to its use. Measured rows: I re-ran `measure_gr6.sh` and `measure_tonex.sh` (copies that write to my scratch) and diffed the tables against `evidence/`; then I re-analysed the decisive takes with `myenv.py`, which shares no code with the author's `tremenv.py` (time-domain lock-in, double boxcar of one carrier period, own edge finder). Accepted tolerance: 0.1 % on rate, 0.005 on gain, 0.3 ms on edge times, 0.15 dB on depth. Literature rows: I opened the URLs. AmpliTube 5 was not loaded; its rows are static only.

Result: 19 VERIFIED, 1 NOT VERIFIED, 0 INCONCLUSIVE.

## Claims

| id | verdict | what I checked (command or address) | what I saw | note |
|---|---|---|---|---|
| T01 | VERIFIED | `src/dsp/FxBlocks.cpp:426-441`, `src/Parameters.cpp:102-104`; own arithmetic | Code is `x*sqrt(1-mix) + x*modGain*sqrt(mix)`, `modGain = 1 - depth*(1-lfo)`. Depth 0.4, mix 0.35: gain 1.1612 to 1.3978, 1.61 dB swing, +2.91 dB peak. Minimum gain at depth 1 is `sqrt(1-mix)`, zero only at mix 1. | Kind says "measured"; it is source arithmetic. Default Mod Type is Chorus (`Parameters.h:245`, index 0), so "default patch" means default depth and mix once Tremolo is picked. |
| T02 | VERIFIED | GR6 `RigTremolo::processBuffer` 0x2e65b8-0x2e662c | Phase minus `[0x1e0]`; `fcsel` picks target = lane 2 of the ramp (floor) or 1.0; `csel` picks coefficient `[0x1f8]` or `[0x1f4]`; `state += coef*(target-state) + 1e-12` (0x2b8cbccc); state multiplies the input. | The gain also passes a bypass crossfade pair (`v5[0] + gain*v5[1]`). |
| T03 | VERIFIED | Re-render, section A; `myenv.py` | 0.4297, 0.7226, 1.2153, 2.0439, 3.4374, 5.7810, 9.7225, 16.354 Hz. Step ratio 1.6818 = 64^0.125. My analyser: 0.4297, 2.0439, 5.7812, 16.3516. | Table identical to the evidence file. Range constants 0.2555 and 16.3516 are at 0x2e5f04-0x2e5f18. |
| T04 | VERIFIED | Re-render, section B; `myenv.py`; `setParamIntens` 0x2e6170 | Floors 0.7500, 0.5000, 0.2500, 0.1000, 0.0000. Setter stores `1 - Intensity`. | |
| T05 | VERIFIED | `onSetSampleRate` 0x2e5fd0-0x2e5fec, `setParamUp` 0x2e61cc-0x2e6280, table builder 0x6a9c30; re-render | `[0xe4] = 6.283/sr`, `[0xe8] = [0xe4]*150.0` (0x43160000). `setParamUp`: index `5*(60 - 95*knob) + 1200` into a table built as `2^(i/60 - 20)`, so the ratio is `2^((60-95k)/12)`; then `fcmp/fcsel` caps `rate*ratio*[0xe4]` at `[0xe8]`. Measured rise at Up=0: 5.33 ms (mine 5.35) at 2.04 Hz, 2.35-2.38 ms at 5.78, 9.72 and 16.35 Hz. | The excerpt labels 0x40c90e56 as 6.2831855; it decodes to 6.2830. No effect on the claim. |
| T06 | VERIFIED | Re-render, section G; own correlation; 0x2e6604-0x2e6614 | corr -1.0000, gL+gR 1.2500 constant, gL^2+gR^2 0.7813 to 1.0608. Code: `gainL = Pan ? floor + 1 - state : state`. | |
| T07 | VERIFIED | GR6 `RigFender::processBuffer` 0x2c7448, 0x2c79ec-0x2c7a54, 0x2c7820-0x2c7858 | `w16 = 0xbf4ccccd` (-0.8) loaded at 0x2c7448, added to the phase at 0x2c7a10-0x2c7a14; lit = phase > 0.8; target `1 - [0x91c]*lit` to `[0x390]`; coefficient `[0x914]*(1-lit) + [0x918]*lit` to `[0x384]`; `[0x4e4]` times the signal to `[0x420]`. `[0x384]`, `[0x390]`, `[0x4e4]` are lane 1 of a 4-lane filter update at 0x2c7820-0x2c7858. | I did not resolve that bank's per-lane constants on the stack. The measured edges (T08) match a plain one-pole at the two corners, so the reading holds. |
| T08 | VERIFIED | Re-render Twang; `myenv.py` on `out_tw_m30_d1.wav`, `out_tw_s0.wav`, `out_tw_s1.wav` | Author's method on my render: fall 12.06 ms, rise 33.88 ms, floor 0.1168, duty 0.752, 3.8626 to 12.2480 Hz. Mine: 11.95 ms, 33.85 ms, 0.1155, 0.752, 3.8627 to 12.2465 Hz. | |
| T09 | VERIFIED | Same renders, -30 and -6 dBFS | 17.88 dB (mine 17.97) and 11.16 dB (mine 11.29). | |
| T10 | VERIFIED | GR6 `RigAC30::processBuffer` 0x2bce08-0x2bce2c, 0x2bc94c-0x2bc980, 0x2bca08-0x2bca58 | `p += [0x540]`; if p > 1 then `p -= 2` and `[0x544]` is negated; `lfo = [0x544]*(1 - p*p)`; copied to lanes 0 and 1 of a zero vector; `fadd.4s v25, v25, v26` at 0x2bc980; an interpolated table load follows at 0x2bca08; lane 1 is subtracted at 0x2bca58. | "Valve table" is the author's label for that table. The code only shows a table. |
| T11 | VERIFIED | Re-render AC Box; `myenv.py` | Clean 1.86 dB (-30), 1.57 dB (-6); Crunch 3.79 dB (-30), 1.22 dB (-6); crest 1.103, floor 0.890; 2.0010 to 7.0032 Hz. Mine: 1.86, 1.58, 3.81, 1.23 dB; 2.0010 to 6.9986 Hz. Presets doc1 and doc2 are named Clean and Crunch in the factory file. | |
| T12 | VERIFIED | AT5 0x14b30a0-0x14b30d0, 0xcdc7d0-0xcdc7fc; Preallocate 0x14b3a4c-0x14b3a64 and 0x13c50f0-0x13c5108; `disasm.py --floats` on both tables | Both loops: `out = (1-depth)*in + lfo*depth*in`, lfo linearly interpolated from a table pointer. Preallocate copies 404 bytes from 0x305f904 and 1024 bytes from 0x26257c0 into those pointers. Table minima 0.1718 (-15.3 dB) and 0.0600 (-24.4 dB). | Static only. "Never silent" assumes depth stays at or below 1. |
| T13 | VERIFIED | `bl_xref.py` on 0xcdc7a8; `bl` list of the function holding 0x15d7c94 | Five callers: the stomp wrapper and the PreAmp wrappers of Fender65DeluxeReverb, Fender65TwinReverb, FenderVibroKing, Fender64Vibroverb. In the Deluxe wrapper the `bl` order is StateSpace64_4_1_1 (0x15d7244), SpringReverbAT3 (0x15d76dc), FenderTremolo (0x15d7c94, last). | That the StateSpace object is the tone stack is not shown by the code I read. |
| T14 | VERIFIED | AT5 0xdb1a64 whole function; Preallocate 0x1487814-0x1487950; `--floats` on four tables | Skew: `x < s ? 0.5*x/s : 1 + 0.5*(x-1)/(1-s)`. Table at `[0x88]` (3-point triangle from 0x2fe249c) and a second table chosen by index `[0x14]` (raised cosine from 0x2fe2308, error 3e-8, or a 0/1 square from 0x2fe271c) are weighted by `[0x18]` and `[0x1c]`. `u = [0x20] - [0xf8]*lfo`. 157-point table from 0x2fe24a8: dB-linear, slope 33.90 dB, first value -33.3 dB, worst deviation 0.42 dB. Then `y = b0*x + z; z = b1*x - a1*y` with coefficients from 628.3185 (0x441d1463) and 2*sr. `out = in*y`. | |
| T15 | VERIFIED | Re-render TONEX; `myenv.py` | Rates 2.5750, 5.0500, 7.5247, 10.0000 Hz. Square at LEVEL 1: 0.0000 to 1.4142, edges 4.79 ms (mine 4.79). Square at LEVEL 0.6: 0.4000 to 1.2485 = 1-L and 1+0.414L. Envelope identical at -36, -18, -6 dBFS. SHAPE 0.5 cycle is a half-height triangle with steps. | With the triangle the measured ends are rounded: floor 0.026 and crest 1.388 at LEVEL 1. The exact law shows on the square only. |
| T16 | VERIFIED | Re-render; own zero-crossing lag estimate | Author's method: 0.0, 45.0, 90.2, 135.1, 180.0 degrees. Mine: 0.0, 44.9, 89.7, 134.5, 179.4. | |
| T17 | VERIFIED | Opened the URL | Page gives "HPF = 250pF + 1M, LPF = 220k + 5nF", about 636 Hz and 144 Hz, -6 dB/oct single-stage RC, two LFO signals 180 degrees out of phase. | |
| T18 | VERIFIED | Opened both PDFs, read pages 1 and 4 and DAFx page 7 | VTL5C3: turn-on 2.5 ms to 63 % of final R_ON, turn-off 35 ms to 100 kOhm. NSL-32SR2: rise 5 ms, decay 80 ms. DAFx-13 section 3.2: low-pass whose response time is switched by the sign of the input's derivative, about 12 ms up and 250 ms down, for the VTL5C3 or VTL5C3/2. | |
| T19 | VERIFIED | `tremolo_proto.py stereo`; own check at other depths | Linear at 180 degrees: -6.02 to -3.01 dB. Proposed: -3.01 to -3.01 dB. | True at depth 1 only. With the report's depth law the proposed law ripples 0.63 dB at depth 0.25, 0.59 dB at 0.5, 0.21 dB at 0.75. |
| T20 | NOT VERIFIED | `tremolo_proto.py chop`; same code with other carrier phases | The quoted run reproduces: -41.9 to -64.8 dBc, edges 2.92 ms. But the prototype's 1 kHz sine crosses zero at every gate edge (4 Hz edges at multiples of 125 ms). Cosine carrier: -30.2 to -59.1 dBc. 45 degree offset: -33.5 to -61.8. Carrier at 997 or 1001.5 Hz: -33.0 to -61.1. | The 2.92 ms edge and an improvement of 23 to 29 dB hold. The two dBc figures are a best case, not a property of "a 1 kHz carrier". |

## Other things seen while re-measuring

- The GR6 Tremolo and TONEX tables regenerate exactly (one 0.01 ms difference, and the one-cycle square table starts at a different sample). The prototype regenerates `evidence/prototype_tables.txt` byte for byte.
- GR6 Twang, -18 dBFS, Intensity 1: my first render held 22,498 NaN samples in the left channel from 0.9 ms on, so that row came out `nan`. Three re-renders of the same state were clean (crest 0.9292, floor 0.1211, 17.70 dB). The plugin is not fully deterministic headless.
- Edge times on driven amp takes depend on the edge finder. Report 3.3 quotes "rise 46 ms, fall 117 ms" for AC Box Crunch at -6 dBFS; my analyser gives 46.2 and 39.6 ms on the same file. Report section 4 quotes 25.61 and 28.81 ms for Twang at -6 dBFS; mine gives 219 and 176 ms. The depth figures agree. Treat those four edge numbers as unverified. No claim row or recommendation uses them.
- The models built from the disassembly match the renders: GR6 Tremolo INIT rise 82.58 ms against 82.54 measured, Up=0.75 crest 0.806 against 0.805, Twang fall and rise 11.75 and 33.85 ms against 12.06 and 33.88.

## Top three recommendations

**R1 "Smooth" voice.** Follows from verified claims: the mix defect (T01), pulse with slew and a 150 Hz ceiling (T02, T05), linear depth to silence (T04, T15), wave crossfade, skew and a 100 Hz smoother (T14), +3 dB crest in TONEX (T15). The prototype gives the quoted numbers: floor -12.0 dB at depth 0.5, -24.1 dB at 0.75, silence at 1, crest +3.00 dB; pulse edges 2.92 ms; depth step 0.0147 without the ramp and 0.0014 with it. Weak points:
- It leans on T20. The acceptance threshold "splatter below -60 dBc" is not met by the prototype itself with a cosine carrier (-59.1 dBc). Either fix the carrier phase in the test or relax the threshold to about -58 dBc.
- The `(1 - depth)^2` law and the 120 Hz corner have no reference behind them. Section 8 says so; the recommendation text does not.
- The UI rule "select voice 1 when a user newly picks Tremolo" is a product decision with no evidence.

**R2 "Optical" voice.** Follows from verified claims: lamp pulse into a switched two-rate one-pole (T07), its measured edges (T08), the AT5 table floors (T12), the photocell data (T18). The Strymon page does say the bulb is off for most of the cycle. The prototype gives 10.98 ms fall, 43.83 ms rise, 0.698 loud fraction and a -20.0 dB floor at 5 Hz. The constants (25 % lit, 5 ms, 20 ms, -20 dB) are the author's and differ from GR6's (20 %, 5.5 ms, 15.5 ms, -18.4 dB). Unstated: the figures drift with rate; at 12 Hz the prototype's crest is 0.961 and the depth 18.6 dB. The GR6 crest of 0.915 stays unexplained, as section 8 admits.

**R3 Stereo spread.** Follows from verified claims: TONEX 0 to 180 degrees (T16), GR6 mirrored pan and its power wobble (T06), the power law (T19). Unstated assumptions:
- Constant summed power holds only at depth 1 and 180 degrees. At lower depths the ripple is up to 0.63 dB, still better than the linear law's 1.3 to 3.0 dB above depth 0.5. At 90 degrees the proposed law still swings 11.5 dB.
- The exponent changes the wave. At 180 degrees a sine becomes a rectified half-sine with a cusp at the null. Through the 120 Hz smoother its floor is -38.7 dB re crest, not the "below -60 dB" that R1 promises for depth 1.
- The proof thresholds (ripple below 0.2 dB, correlation below -0.9) are met by the prototype with the smoother in place: 0.00 dB and -0.919.

## Citations

Five opened, all exist and say what the report attributes to them.

- `sites.google.com/site/davidmorrinoldsite/.../fender-harmonic-vibrato`: component values, 636 Hz, 144 Hz, 6 dB/oct, antiphase LFOs. Confirmed.
- `qsl.net/wa1ion/vactrol/vactrol.pdf`: VTL5C3 2.5 ms and 35 ms, NSL-32SR2 5 ms and 80 ms. Confirmed. The "2.5-6 ms on" in R2 takes its 6 ms from the VTL5C4 row, whose turn-off is 1.5 s, not within the "35-80 ms" the report quotes beside it.
- `dafx.de/paper-archive/2013/papers/44.dafx2013_submission_56.pdf`: section 3.2, derivative-switched low-pass, 12 ms and 250 ms. Confirmed.
- `strymon.net/amplifier-tremolo-technology-white-paper/`: photo-trem duty cycle well over 50 % with the bulb off; bias tremolo with crossover distortion, valve distortion near maximum, supply sag. Confirmed.
- `valvewizard.co.uk/trem1.html`: 1 to 10 Hz, Fender values 4.3 to 8 Hz. Confirmed.

## Inferred items presented as fact

- Summary, line 3: "GR6's AC30 adds the LFO to the output valves' bias". Section 3.3 tags the valve identity `[inferred]` and section 8 says the stage and the table units were not traced. The code shows an offset added to two lanes before a table lookup, nothing more. `evidence/gr6_amp_tremolo_excerpt.txt` also writes "push-pull grid signals" without a tag. R5 names its voice "Bias" on this basis.
- Summary, line 1: "the default patch gives 1.6 dB of tremolo". The default Mod Type is Chorus. The figure is right for default depth and mix.
- Section 5 "Position in amps": "before the power stage (GR6 Twang ...)" is stated flat; section 3.2 tags it `[inferred]`.
- Section 3.5 tags the AT5 rate ranges `[decompiled]` and section 5 repeats them as fact; section 8 says the argument order was not confirmed and calls them `[inferred]`. No top recommendation depends on them.
- R4 says sync is "present in GR6, AT5 and TONEX" and that GR6 slaves the phase to song position. Sync was not measured anywhere and the GR6 note enum was not decoded (section 8).

## Fences

- Copy protection: respected. No PACE-protected binary appears in `$RE_STATE/thin/`, in the author's work directory, in the report or in the evidence. Tone King is listed as skipped. Only Guitar Rig 6 and AmpliTube 5 were disassembled; TONEX was measured black-box.
- Listing length: respected. The report's code blocks are 3 to 6 lines of the author's own pseudo-code. The evidence excerpts are annotated one-line summaries, at most about 17 lines per function.
- Vendor tables: respected. The prototype holds no data arrays. `evidence/at5_tables_summary.txt` gives statistics and an 11-point sample per AT5 table, which is characterisation, not a copy. The prototype's "refs" section restates GR6 constants (0.8, 0.88, 29.14 Hz, 10.30 Hz, 150 Hz, the slew law) to compare model against measurement; the recommended voices use different constants.
- One item for the orchestrator: `evidence/at5_blackbox_gap.txt` records one `auhost edit` attempt on AmpliTube 5 "under the gui lock". It crashed before a window opened. Whether the author's brief allowed GUI work I cannot tell.
- My own run: no GUI, AmpliTube 5 not loaded, no protected binary touched, writes only to `$RE_STATE/work/tremolo-verify/` and this file. WebFetch stored the two PDFs in the session's tool-results folder.

## Overall judgment

The report's factual base is sound. Every decompiled constant I checked is at the stated address and is used the way the claim says, both measurement scripts regenerate the evidence tables, an independent analyser agrees with them within 0.3 ms and 0.15 dB, and the five citations I opened support the text. The one claim that does not stand as written is T20: its splatter figures come from a carrier that happens to be at a zero crossing at every gate edge, and the general figures are 6 to 12 dB worse, which puts R1's own -60 dBc acceptance threshold out of reach in the worst case. The other soft spots are presentational: the AC Box "output valve bias" reading is an inference stated as fact in the Summary, the stereo power law is constant only at full depth, and a few edge times on driven amp takes depend on the analysis method. None of these changes the ranking of R1 to R3.
