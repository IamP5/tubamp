# Chorus report: independent verification

Verifier scratch: `$RE_STATE/work/chorus-verify/` (own analysis tool `vx.py`, own test signals, own renders, disassembly dumps).
`vx.py` shares no code with the author's `cx.py`: own WAV reader, tap delay from the sinc-upsampled peak, pitch from the instantaneous frequency of a 1 kHz sine.
I self-tested `vx.py` on a synthetic copy of the JUCE chorus formula before using it.
Every render ran under `perl -e 'alarm N; exec @ARGV'`. AmpliTube 5 and UADx Studio D were not loaded. No GUI work.

Tolerances accepted: delay 0.02 ms, LFO rate 0.01 Hz, pitch 1.5 cents, level 0.5 dB, L/R phase 7 degrees.

## Claims

| id | verdict | what I checked (command or address) | what I saw | note |
| --- | --- | --- | --- | --- |
| C01 | VERIFIED | `disasm.py Choral --addr 0x8731c`, 0x8755c-0x875ec; `[sp]` constant at 0x87370 | Taps `buf[1-i]`, `buf[-i]`, `buf[-1-i]`, `buf[-2-i]`. c1 = 0.5(x1-xm1), c2 = xm1-2.5x0+2x1-0.5x2, c3 = 1.5(x0-x1)+0.5(x2-xm1). The 0.5 is `fmov.2d #0.5` stored at `[sp]`. | Exactly Catmull-Rom. The delay fed in is `[voice+0x8] + [voice+0x20]*m` (0x87558). |
| C02 | VERIFIED | `--addr 0x884ec`, 0x885e0-0x8860c; stub 0x1cb75dc via `otool -Iv`; `ChorusVoice::setSampleRate` 0x88060; lambda `$_1` 0x89984 | `exp2(a*30/1200)`, minus 1, times `(1/rate)*[x20+0x30]*0.5`, stored at `[x20+0x10]`. Stub is `_exp2`. `[x20+0x30]` is voice+0x40, written with the sample rate. `[x20+8]` is the Amount value written by `$_1`. `[x20+0x10]` is voice+0x20, the depth factor `process` uses. | Code path confirmed end to end. Rate is the per-voice rate (knob rate times mode ratio). |
| C03 | VERIFIED | 0x88514-0x88538 | `p^3 * 0x401d99999999999a (7.4) + 0.25`. Render at knob 0.4663 gave 1.000 Hz and at 0.7190 gave 3.001 Hz. | Static and measured agree. |
| C04 | VERIFIED | `--addr 0x898f4`, 0x89908-0x89958 | `p^2 * 0.99 + 0.01`, times 30.0, divided by 1200.0, into `exp2`. | Constants decoded: 0x3f847ae147ae147b = 0.01, 0x3fefae147ae147ae = 0.99. |
| C05 | VERIFIED | `--addr 0x88e80`; constructor 0x88c30-0x88c68; call at 0x87408-0x8741c | `1 - abs(2 - 4p)`. The function pointer is stored in the first `std::function` of the voice (voice+0xe0), the one `process` calls first. | Second function (voice+0x148) is the sine shaper. |
| C06 | VERIFIED | `--floats 0x1fed470 8`; `--addr 0x87a70`; jump table bytes at 0x1fed57a read from the file | 0.003, 0.003, 0.007, 0.007 loaded at 0x87b58 and stored to the start of the target block. Jump table sends mode 0 to 0x87b50. AU parameter 0 = 0 displays "Synth". Render: base delay 3.095 ms. | Next floats are 0.011, 0.011, 0.013. |
| C07 | VERIFIED | Re-rendered 7 takes with own clicks (`vx.py trk`) and 5 takes with a 1 kHz sine, dry subtracted (`vx.py fm --sub --g 0.5002`) | Clicks, Amount 50 %: 7.9, 7.9, 8.1 cents at 0.25, 1, 3 Hz. Amount 100 %: 31.2, 31.2, 31.5. Sine method: 7.77 cents at all three rates; 30.5 at 100 %. | Constancy across rate holds. The author's click method reads 0.1 to 1.2 cents high (8.4 where the truth is 7.8; 31 to 32 where the truth is 30.5). The author's 1 Hz, Amount 100 % take used a 12.5 ms click period with a 15.6 ms tap, so the track wrapped (their row shows 342 cents max). I re-did it with a 50 ms period. |
| C08 | VERIFIED | Re-rendered 4 modes, `vx.py lr` (Fourier phase and zero-lag correlation) | R minus L: 179.7 (Synth), 179.0 (Ensemble), 179.9 (Dimension), 0.1 (Universal) degrees. Zero-lag correlation -0.99, -0.84, -1.00, +1.00. Synth delay 3.096 to 11.685 ms. | Matches. |
| C09 | VERIFIED | `vx.py tapfr` on the Universal render; own closed-form for linear, Catmull-Rom and Lagrange | Spread 0.59 dB at 10 kHz, 2.58 dB at 15 kHz. Catmull-Rom predicts 0.53 and 2.53. Linear predicts 2.01 and 5.11. | The measurement cannot tell Hermite from a 4-point Lagrange: both give 0.53 and 2.53 dB. Only C01 identifies Hermite. |
| C10 | VERIFIED | Re-rendered Classic chorus at Intensity 0, 0.5, 1 with clicks and with a sine (right channel is wet only) | Sweep 1.703 to 4.380 ms. Rate 0.433, 1.846, 2.857 Hz. Pitch p95: 4.5, 19.1, 29.3 cents (clicks); 4.5, 19.3, 29.8 (sine). Delay crest 1.74 to 1.82 (triangle is 1.73). One wet tap. | At Intensity 1 the sweep narrows a little (1.73 to 4.33 ms). |
| C11 | VERIFIED | Re-rendered Dual at Intensity 0, 0.5, 1; Classic tap energy per channel | Dual phase 95.4, 94.4, 94.4 degrees; zero-lag correlation -0.075. Classic: left level at the wet tap position is 45 dB under right; right level at the dry tap position is 42 dB under left. | Author's 88.6 at Intensity 0 is 95.4 here (2.6 cycles in the window). Still quadrature. |
| C12 | VERIFIED | Re-rendered own white noise, third-octave band ratio (`vx.py spec`) | Wet relative to 1 kHz: -2.9 dB at 130 Hz, -3.3 at 4.6 kHz, -13.2 at 8.9 kHz, -22.7 at 12.3 kHz. Dry: -0.3 dB at 1 kHz, -1.1 at 12.3 kHz, -1.9 at 18 kHz, -4.1 at 45 Hz. | "Flat" for the dry path means within 1 dB from 100 Hz to 12 kHz. |
| C13 | VERIFIED | 220 Hz sine at -40, -30, -18, -6, 0 dBFS and silence | Dry RMS -43.517, -33.517, -21.517, -9.517, -3.517 dB. Wet -39.232, -29.232, -17.232, -5.232, +0.768. Silence in gives exact zeros. | Linear to 0.001 dB. Default Level only, as the report says. |
| C14 | VERIFIED | `--addr 0x2932d4`, `--addr 0x293358`, `--addr 0x2c406c`; stub 0x2756940 | `I*2.9 + 0.1`, then `69 + 17.3123*logf(f/440)` is stored. `setLfoF` converts that pitch back through a table. Chorus branch (mode flag 0): `[this+8] * 0.0026` stored at +0x154. | The rate is kept as a MIDI pitch, not in Hz. The claim's Hz law still holds: measured 1.551 Hz at 0.5 and 3.000 Hz at 1. The row's recheck ran as written and shows the three constants. |
| C15 | VERIFIED | Rebuilt the state headlessly (own `auhost state`, the author's `gr6state.py`), rendered clicks and sine | Swing 2.599 ms at 1.551 Hz; 2.581 ms at 3.000 Hz. Pitch 14.3 and 27.7 cents (clicks), 14.0 and 27.3 (sine). Phase 90.0 degrees at Stereo 1, 45.4 at Stereo 0.5. | Claim says 28.0 cents and 92 degrees. Within tolerance. |
| C16 | VERIFIED | `--floats 0x30c6a2c 12`; `nm` symbol size; `Module::Process` 0xdf91e4-0xdf9244 and 0xdf90b0; table copy at 0x152ab2c | Nine floats: 1.1378 three times, 1.6306, 2.0984, 2.5600, 2.9425 three times. Copied to the buffer at module+0x78. `Process` indexes it with linear interpolation and multiplies by `fs*0.001`. | Units are ms. Only five distinct values; the ends are flat. |
| C17 | VERIFIED | `--addr 0x152a1e0`, 0x152a40c-0x152a570; `Module::Process` both branches | `11v + 11(1-v)` times 0.5998; rate `(1-v) + 3v`, divided by fs. Other setter: `12.5R + 3.4(1-R)`. The first setter writes the fields the triangle branch reads (+0x44, +0x48); the second writes the fields the sine branch reads (+0x4c, +0x50). | Parameter names were not read from strings. The mapping is by which branch uses the fields. |
| C18 | VERIFIED | `--addr 0xe2da68`, 0xe2dbac-0xe2dc5c; stub 0x2166618 | `sinf(ph + 4.1888)`, `sinf(ph)`, `sinf(ph + 2.0944)`, twice (two LFO groups). | Stub is `_sinf`. |
| C19 | VERIFIED | `src/dsp/FxBlocks.cpp:357-358`, `juce_Chorus.h:113-126,158,169-172`, `juce_Chorus.cpp:42,45,138`; numerical simulation of that formula with a 1 kHz sine and with clicks | Source matches. Simulation: +43.1 / -44.1 cents at 1 Hz, depth 0.4; +387 / -500 at 10 Hz; +106 / -112 at depth 1. | `setCentreDelay(7.0f)` is line 357, not 358. The row is a closed-form number, not a measurement. Downward peaks are larger (44 and 501). |
| C20 | NOT VERIFIED (in part) | Fetched the URL; read the SDD-320 section and the "Mono to Stereo Chorus" paragraph | Present: "2 x BBD MN3007", "1 LFO", 80 Hz passive high-pass, modes 10 ms +-2.0 at 0.25 Hz, 7.5 +-2.5 at 0.25 Hz, 7.5 +-1.5 at 0.5 Hz, mode 4 with a 10 dB pad. Absent: any statement that the SDD-320 inverts the LFO on the second line. | The page states inversion for the Roland polysynth choruses in general, and calls the Dimension D a "unique algorithm". Antiphase triangle modulation for the Dimension D is stated in a comment reply on the electricdruid page the report cites elsewhere. The fact is likely right; the citation is wrong. |
| C21 | VERIFIED | Fetched both PDFs and read them | Schematic ET-10C: IC-2 is MN3002; scope strip entry G reads "5 us ~ 16 us". DAFx-10 page 2: delay = N / (2 f_cp). 512 * 5 us / 2 = 1.28 ms; 512 * 16 us / 2 = 4.10 ms. | Assumes the strip value is the full clock period. The schematic does not label it. |

Counts: 20 VERIFIED, 1 NOT VERIFIED (C20, citation of one sub-claim), 0 INCONCLUSIVE.

## Top three recommendations

R1 Wide. Follows from verified claims with two gaps.
- Supported: quadrature stereo (C11, C15, Dattorro read), cents law (C02, C04, C07), 30 cent ceiling (C04, C10), wet low-pass range (C09 render, C12), unequal pair rates (I read ratios 1, 1.4286, 1.7242 at 0x1fed48c).
- Gap 1: `D_max = 8 ms` breaks the cents law at low rates. At 30 cents the law needs more than 8 ms below 1.09 Hz. At depth 0.5 it clamps below 0.27 Hz. tubamp's rate knob goes down to 0.05 Hz. So "a steady detune that does not change with speed" is true only above those rates. Choral does not clamp (34.6 ms swing measured at 0.25 Hz). The R1 proof point at 0.25 Hz lands at 6.9 cents, inside its own band only by a small margin.
- Gap 2: the mix law is attributed to Choral, and Choral does not do it. See below.

R2 CE. Follows from verified claims (C10, C11, C12, C14 to C17, C21). Unstated assumptions:
- The pedal's rate stops at 2.86 Hz. tubamp's `mod_rate` goes to 10 Hz. A fixed 2.7 ms triangle sweep at 10 Hz is about 91 cents. "Fast settings reach about 29 cents" holds only if the rate is capped in this mode.
- The proposed filter (2-pole 4.6 kHz plus 1 pole 9 kHz, 1 pole 130 Hz high-pass) fits my wet measurement within 2 dB from 100 Hz to 12 kHz. Below 100 Hz it is about 3 dB too thin at 45 Hz.
- The 0.25 bend coefficient has no fit shown. The dry-to-wet ratio is open, as section 8 admits.

R3 Dimension. Weakest of the three.
- Rests on one literature page plus Choral's Dimension mode (C08 verified). The UADx model was not measured.
- The inverted second line is not in the cited page (C20). The triangle shape comes from the electricdruid comment, not from Anwander.
- The pitch figures are wrong for the design as written. "5 to 8 cents" comes from the sine formula (2 pi f A). R3 specifies a triangle. A triangle of +-2.0 ms at 0.25 Hz is 3.5 cents, +-2.5 ms at 0.25 Hz is 4.3 cents, +-1.5 ms at 0.5 Hz is 5.2 cents. The R3 proof threshold "5 to 9 cents at the two preset points" would fail at the first preset (3.5 cents) with the LFO the recommendation asks for.
- The low-pass value is a guess (tagged). The page's cross-feed of the other channel's dry signal is left out (stated in section 8).

Helper-read constants the recommendations use that the author says were not re-read: the 20 ms fade-switch-fade of the TC model (shared design and R6), the MXR tone shelves (R5), the Fulltone voice routing (R4). I did not trace the 20 ms ramp either; `applyTargetMode` starts at 0x66e5dc, not 0x66e5ec as the report tags it.

## Citations opened

- http://www.florian-anwander.de/roland_string_choruses/ exists. Dimension D mode numbers, MN3007, 80 Hz high-pass, Juno-60 rates 0.5 / 0.8 / 1 Hz and RS-202 6.25 / 0.66 Hz are all there. It does not state the SDD-320 inversion, and it gives no cents figure. The report's "5 to 8 cents for the Dimension D [literature: ...]" is the author's own arithmetic.
- http://colinraffel.com/publications/dafx2010practical.pdf exists. It gives delay = N / (2 f_cp), says companders are not typically used for chorus, flanger and vibrato, gives THD = 1.01^(N/1024) - 1, and says the nonlinearity is typically left out for 1024 stages or less. Matches.
- https://www.tonehome.de/app/download/8747046/boss_ce1_chorus.pdf exists. It is the ET-10C schematic with MN3002 and the "5 us ~ 16 us" strip entry. Matches.
- Also read: https://ccrma.stanford.edu/%7Edattorro/EffectDesignPart2.pdf. Table 6 (industry standard chorus 1.0 / 0.7071 / 0.0), Table 7 (chorus 1 to 30 ms, nominal 5), quadrature LFO for stereo, allpass for less than one semitone. Matches.
- Also read: https://electricdruid.net/weird-wiggles-bbd-clock-modulation/. CE-1 1.3 to 4 ms and the period-versus-frequency point match. The Dimension D antiphase remark is in a comment reply.

## Inferred or guessed items presented as fact

- Section 6 (Mix law row) and section 7 (shared design): "dry fixed" and "keeps dry at unity up to the midpoint, as the references do [decompiled: Choral 0x863cc]". I measured Choral's dry tap against Mix: 1.000, 0.875, 0.750, 0.625, 0.500 at Mix 0, 0.25, 0.5, 0.75, 1. Choral's dry falls 2.5 dB by the midpoint and 6 dB at full. Guitar Rig Ensemble is also a linear crossfade by the report's own section 3.2. Only Dattorro and the CE-1 topology support a fixed dry.
- Summary line 2: "Every reference chorus ... uses a triangle LFO for chorus, band-limits the wet path". The report's own section 3 lists counter-examples: Fulltone tri-chorus and Valhalla use sines, the TC model uses a filtered square, Valhalla's wet tap is flat to 15 kHz, Choral Universal is 20 kHz. Choral at Amount 100 % is 30.5 cents, at the edge of "under about 30".
- Summary line 1 and section 2 tag closed-form numbers and a source reading as `[measured]`.
- Summary line 4 drops the "default Level only" limit on the UADx level test.
- Section 4: "Three independent CE-1 models agree ... fixed sweep near 2.6 ms". The AmpliTube table spans 1.14 to 2.94 ms, so its sweep is at most 1.8 ms.
- R1 "does not change with speed" (see the D_max gap). R3 "5 to 8 cents". R4 "the three phases average out most of the audible pitch movement" has no tag and no evidence.
- "None is copied from a vendor table": true for tables. The R1 depth law `30 * x^2` cents is Choral's decompiled law (30, square taper) minus the 0.01 floor. That is a parameter range, which the fences allow, but it is not "derived from circuit, literature or measurement".

## Fences

- PACE binaries: no sign of disassembly. All `[decompiled]` tags name Choral, Flair, Guitar Rig 6, AmpliTube 5 or ValhallaSpaceModulator, all "readable+symbols" without the pace marker in `inventory.tsv`. The author's work directory holds no dump of a `uaudio_*` binary. One borderline act: the report says the author ran `sample` on the hung auhost process that hosted UADx Studio D. `sample` is not a debugger and no output was kept, but it does inspect a process running protected code.
- Listing length: longest pasted disassembly is 12 lines (`choral_hermite.txt`). Longest code block in the report is the 18-line design sketch. No static note has a code block of 15 lines or more.
- Vendor tables: none in the recommended design. R1 to R4 constants differ from Choral's mode table and from `gcV2TCurve`. The evidence folder does hold a condensed copy of Choral's per-mode constants (`choral_mode_table.tsv`) and the nine `gcV2TCurve` values; keep those out of any preset work.
- My own conduct: disassembled only Choral, Guitar Rig 6 and AmpliTube 5. UADx Brigade was rendered only. Wrote only this file and scratch under `$RE_STATE/work/chorus-verify/`.

## Overall judgment

The measurement and decompile base of the report is sound. Twenty of twenty-one claims reproduce from the binaries, the source and fresh renders, and an independent sine-based pitch estimate agrees with the author's click method to about one cent (the click method reads slightly high). The weak points are in the synthesis, not the data. The mix-law justification misreads Choral, whose dry level falls with Mix. The Dimension recommendation quotes pitch figures and an acceptance band computed for a sine while specifying a triangle, and cites a page that does not state the inverted second line. The cents law in R1 and the "about 29 cents" ceiling in R2 both stop holding inside tubamp's existing 0.05 to 10 Hz rate range unless the design adds a rate cap or drops the 8 ms clamp. The Summary's "every reference" sentence is broader than section 3 supports. None of this overturns the direction of the recommendations; R1 and R2 can proceed once those limits are written into the spec, and R3 needs its numbers redone.
