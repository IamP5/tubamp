# Compressor study: independent verification

Verifier scratch: `$RE_STATE/work/comp-verify/` (`src/` holds my own code, `dis/` my disassembly dumps). Nothing in the report, claims or evidence was edited.
My own tools, written without the author's scripts: `src/jucecomp.cpp` compiles the verbatim `juce_BallisticsFilter.{h,cpp}` and `juce_Compressor.{h,cpp}` (float) behind a 20-line shim; `src/clean.cpp` / `clean2.cpp` implement the report's R1 and R5 sketches from the report text; `src/an.py` generates signals and analyses them (least-squares harmonic fit for THD, a "residual" figure = everything that is not the fundamental, one-period sliding RMS for timing, plus a direct gain trace); `src/bb.py` drives `auhost` under a 240 s alarm.
Tolerances accepted: timing 3 % or 0.1 ms, THD 0.01 % absolute, static curve 0.1 dB, decompiled constants exact.

## Claims

| id | verdict | what I checked (command or address) | what I saw | note |
| --- | --- | --- | --- | --- |
| C01 | VERIFIED | `juce_BallisticsFilter.cpp:74,98-130`, `.h:153` | `expFactor = -2*pi*1000/fs`, `cte = exp(expFactor/timeMs)`, abs rectifier, `cte = in > yold ? cteAT : cteRL`, default `levelType = peak` | exp(-1/(fs*tau)) with tau = knob/2pi: 5 ms is 0.80 ms, 120 ms is 19.1 ms. tubamp never calls `setLevelCalculationType` (`src/PluginProcessor.cpp:1290-1310`). |
| C02 | VERIFIED | `juce_Compressor.cpp:99-121`, `.h:91-98`, `juce_BallisticsFilter.cpp:79` | `env = filter(|x|)`, then `gain = env < thr ? 1 : pow(env/thr, 1/ratio - 1)`, `yold` sized per channel | Ballistics are before the gain law. No link between channels. |
| C03 | VERIFIED | `jucecomp` on my own -40/-10 dBFS 1 kHz burst; also the installed AU with `chainOrder="comp"` | compiled JUCE source: attack t63 2.21 ms, release t63 13.79 ms, t90 19.69 ms. Real AU: 2.21 / 13.67 / 19.58 ms | Direct gain trace (no 1 ms window) gives 1.69 / 13.31 / 19.13 ms. Release shrinks with depth: 6.8 ms after a -15 dBFS burst. |
| C04 | VERIFIED | `jucecomp` and the real AU on a 100 Hz -10 dBFS sine | 1.988 % at defaults, 12.381 % at 0.1 ms / 10 ms, in both the compiled source and the AU | AU with Comp On = 0 gives 0.000 % and unity gain, so the compressor is the only nonlinearity in that path. |
| C05 | VERIFIED | `disasm.py "Solid Bus Comp" --addr 0xef378`; `otool -Iv` for the stubs | 0xef3d8 `d9 = in * 2^-7`; 0xef3dc loads state[+8]; 0xef3e4 calls updateRMS with it; 0xef3ec-0xef3fc `exp2(clamp(-32*state[0], -32, 0))`; 0xef400-0xef404 `state[+8] = d9 * gain` | Stub 0x545954 is `_exp2`. The detector input is last sample's output. |
| C06 | VERIFIED | `--addr 0xef630`, `--addr 0xee2b4` (setParameter), `--addr 0xef7b8` | `L = log2(min(|x|,1))/32` (stub 0x545c48 = `_log2`), `c = clamp((L - [0x38]) * -[0x60], 0, 1)`, then `c > state ? [0x40] : [0x48]` one-pole on state[0] | Setter writes the attack-table coefficient to +0x40 and the release-table one to +0x48. `[0x60] = 1 - ratio` (0xef7cc-0xef7dc). |
| C07 | VERIFIED | `--doubles` at the four addresses; setParameter 0xee4ec-0xee554, 0xee5b4-0xee620, 0xee820-0xee84c | values as claimed; attack table /1000 then `exp(ln 0.8/(t*fs))`; release table x62.5 (0x404f400000000000) /1000; ratio table inverted into +0x58; offset table added to threshold dB in updateRatioFactor | Roles confirmed, not only values. |
| C08 | VERIFIED | `bb.py` re-render, own bursts and analysis | fixed release: t63 56.25 ms for -10, -20 dBFS and 5 s bursts. Auto: 512.6 / 434.7 / 662.8 ms | Identical to the evidence. |
| C09 | VERIFIED | `--addr 0x269344` | 0x269360 constant 0xc07b4396 = -3.926; 0x26943c-0x269444 `x*G*(1-env)`; 0x269380-0x2693a8 abs, max of lanes 0/1, add -3.926, clamp 0..1, square; 0x2693ac-0x2693cc branching one-pole | Detector lanes and output lanes use different ramped gains but the same `(1-env)`. With the sidechain flag [0x98] set the detector is the external input instead. |
| C10 | VERIFIED | `--addr 0x268c18`, `0x268cdc`, `0xad400`, `0xad310` | -72.7068, 44.3756, -91.7262, -28.3312, 159.155, 0.159155 at the named instructions; value*5 + 1381.88 indexes a table | With a MIDI-pitch-to-Hz table the law gives 1.5..100 ms and 0.1..20 s; I computed release ratios 3.76 and 14.14 for knob 0 / 0.25 / 0.5. The table itself was not read (the claim says "if"). |
| C11 | VERIFIED | `--addr 0x309570`, `--addr 0x3099bc` (0x309b68-0x309ca8) | `k = (59p+1)/2`; detector from the driven, soft-clipped input; `u = (o+2k)/k`, overshoot `k*u^2/8` for 0<u<4, else `o`; one-pole on the dB overshoot | `k*u^2/8 = (o+2k)^2/(8k)`: quadratic knee of total width 4k. Feed-forward. |
| C12 | VERIFIED | `disasm.py "Solid Dynamics" --addr 0xef514` | 0xef560 square of gained input, 0xef59c `_log10`, 0xef5c8-0xef5e8 `(a+k)^2/(4k)`, 0xef624-0xef638 `e -= min(rate, e - target)` when flag [0xc8], else one-pole | |
| C13 | VERIFIED | `bb.py` re-render | mode On: t90 129.3 ms after 11.60 dB GR, 46.2 ms after 4.10 dB. Mode Off: 48.2 ms both | |
| C14 | NOT VERIFIED | `bb.py` re-render of the three bursts and both 192 kHz probes; `cb.py timing` on the author's own file | attack 0.23 and 1.98 ms: confirmed. Release t63 after 0.5 s and 5 s: 95.7 and 123.8 ms: confirmed. Release after the 0.1 s burst is 56.0 ms, not 43 ms | The 43 ms comes from an analysis error. `cb.py` zero-pads the last `lag` (86) samples; with a 6-sample window at 8 kHz the zero tail drags `g_post` to -1.52 dB (true -0.18 dB, see `g_post_db` in the evidence file). The author's own 192 kHz file for the same 0.1 s burst says 55.4 ms. |
| C15 | VERIFIED | `bb.py` curve re-render | -1 dB point -25.7 / -22.0 / -19.6 dBFS; slope ratio 5.7-6.1, 10.0-10.8, 25-35 | |
| C16 | VERIFIED | `bb.py` re-render (48 kHz burst with 10 s tail, 192 kHz probe) | release t50 78.4, t63 132.9, t90 731.7 ms; attack t63 0.83, t90 9.86 ms | t90 differs 1.7 % and 4 % (my steady-state window is 50 ms, the author's 25 ms). |
| C17 | VERIFIED | six `bb.py` re-renders at 100 Hz | 2.113, 2.730, 3.529, 3.749, 0.507, 1.024 % | |
| C18 | VERIFIED | `--addr 0xed3b0`, `--addr 0xed40c` | 0.3 (0x3fd3333333333333), -4.60517 (0xc0126bb1bbb55515), `exp(-4.60517/(0.3 fs))`, `1/(0.3 fs)`; track: `max(g, 0.1)`, `_log10`, one-pole when rising, `state -= min(rate, state - target)` when falling | The constants sit at 0xed3c4 and 0xed3d4, not at 0xed3b8 and 0xed3c4 as the claim says. Substance is right. |
| C19 | VERIFIED | `disasm.py "AmpliTube 5" --addr 0xc9ec28` (static only, plugin not loaded); symbol list | 0xc9ec44 500000, 0xc9ec54 27000, 0xc9ec70 1e-5, used as `((1-p)*500000 + 27000) * 300 * 1e-5`; symbols `DynacompTableRon::cTable` 0x239c24c, `...Ibd` 0x239a308; interpolated lookups through [x0+0x160] and [x0+0x228] | Process reaches the tables through object pointers, not by address. "10 uF" is the author's reading of 1e-5. |
| C20 | VERIFIED | read the PDF (pages 1-4) | Kroening, Dempwolf, Zoelzer, DAFx-11. Table: C10 = 10 uF, R11 = 27 k. Section 2.3: envelope voltage on C10 controls I_ABC. Section 4.2: "simplify the feedback loop as a one sample delay" | The paper's OTA is an LM13700 and its sensitivity pot is 470 k. |
| C21 | VERIFIED | fetched the MathWorks page | quadratic knee term `(1/R-1)(x-T+W/2)^2/(2W)`, smoothing `gc -> gs` in dB with attack/release branch, make-up `-x_sc(0)` | The page defines attack and release as 10-90 % times (`log 9`), not 63 %. |
| C22 | VERIFIED | `clean.cpp` on my own signals; the author's `proto.py` cross-checked | attack t63 5.67 ms, release t63 127.67 ms at -10, -15, -20 and 0 dBFS bursts, slope ratio 4.00, THD 0.0000 % at 100 Hz, 0.052 % at 82.4 Hz, 0.398 % without hold | The numbers reproduce, but the 0.000 % is a special case. See "Clean prototype" below. |
| C23 | VERIFIED | `clean2.cpp` with the stated fit parameters; 1176 and LA-2A re-rendered | FET fit 65.8 / 97.7 / 394.8 ms against my 1176 60.8 / 95.7 / 405 ms. Opto fit 87.8 / 142.0 / 696.4 ms against my LA-2A 78.4 / 132.9 / 731.7 ms | The fit matches one burst length only. The prototype gives 65.8 / 97.7 / 394.8 ms after 0.1 s, 0.5 s and 5 s bursts alike; the 1176 gives t63 56 / 96 / 124 ms. |
| C24 | VERIFIED | `bb.py` with the author's five state files and the factory state | release t63 56.4 / 210.3 / 787.9 ms (ratios 3.73, 14.0); attack t63 2.69 / 16.35 / 91.29 ms; factory low-level gain +15.97 dB, slope ratio 36.6 mid, 104 top | I did not rebuild the states. The release ratios match the law I computed from the disassembly, which supports that the states hold what they claim. |

Counts: 23 VERIFIED, 1 NOT VERIFIED, 0 INCONCLUSIVE.

## tubamp itself: (a), (b), (c)

Model fidelity, line by line against the JUCE source. `tools/ours.py` has the same rectifier (`abs`), the same coefficient (`exp(-2*pi*1000/(fs*ms))`, zero under 0.001 ms), the same branch test (`v > yold`), the same update (`v + c*(yold - v)`), the same gain law (`yold < thr ? 1 : (yold/thr)^(1/ratio-1)`), state per channel, and a static makeup. Differences: it runs in float64 where tubamp runs float32, and it applies makeup as a constant where tubamp ramps it. Neither changes a reported digit.

- (a) Confirmed from source. The per-sample coefficient is `exp(-1/(fs*tau))` with `tau = knob/(2*pi)`. The filter runs on `|x|` and its output feeds the gain law, so attack and release act on the level, before the gain computer.
- (b) My numbers at the 5 ms / 120 ms defaults, -40 to -10 dBFS 1 kHz burst: attack t63 2.21 ms, release t63 13.79 ms, t90 19.69 ms from the compiled JUCE source; 2.21 / 13.67 / 19.58 ms from the installed AU. A direct gain trace gives 1.69 / 13.31 / 19.13 ms. These figures are for 7 dB of gain reduction; after 3.2 dB the release t63 is 6.8 ms.
- (c) My numbers, 100 Hz at -10 dBFS: 1.988 % at defaults, 12.381 % at 0.1 ms / 10 ms. Same in the compiled source and in the AU. At 1 kHz: 0.204 % and 2.588 %.

The real thing. The installed `aufx Tamp Tuba` starts with the amp alone in its chain. I took its state with `auhost state`, changed `chainOrder="amp"` to `chainOrder="comp"` in the XML blob, and rendered with `--state`. Confirmation that the compressor is in the path: gain is -6.97 dB with Comp On, 0.00 dB with Comp On = 0, and unity with an empty chain. The AU does not null sample-for-sample against the model (-23 dB at 100 Hz) because the plugin's fixed path shifts phase at low frequency while leaving the level untouched; every metric still agrees to three digits.

## Clean prototype

Reproduced with my own implementation of the R1 sketch: THD 0.0000 % at 100 Hz, release t63 127.67 ms at both burst depths (and at -20 and 0 dBFS), attack t63 5.67 ms, slope ratio 4.00.

Where it stops holding. "THD" below is harmonics 2-10 only; "residual" is everything except the fundamental; pp is the peak-to-peak gain ripple.

| Frequency | hold 8 ms: THD / residual / pp | no hold: THD / residual / pp | tubamp today, residual |
| --- | --- | --- | --- |
| 100 Hz | 0.000 % / 0.000 % / 0.000 dB | 0.398 % / 0.398 % / 0.111 dB | 1.99 % |
| 110 Hz | 0.005 % / 0.023 % / 0.007 dB | 0.362 % / 0.362 % / 0.101 dB | |
| 98 Hz | 0.023 % / 0.206 % / 0.054 dB | 0.406 % / 0.406 % / 0.113 dB | 2.03 % |
| 82.4 Hz | 0.052 % / 0.558 % / 0.143 dB | 0.483 % / 0.483 % / 0.135 dB | 2.40 % |
| 70 Hz | 0.076 % / 0.825 % / 0.215 dB | 0.568 % / 0.568 % / 0.159 dB | 2.80 % |
| 61.74 Hz | 0.000 % / 0.000 % / 0.000 dB | 0.644 % / 0.644 % / 0.180 dB | |
| 41.2 Hz | 0.140 % / 0.141 % / 0.048 dB | 0.963 % / 0.963 % / 0.269 dB | |

- 100 Hz at 48 kHz is exactly 480 samples per cycle, so every sampled peak has the same height and `d >= held` re-arms the hold each half cycle. That is why the result is exactly zero.
- At a frequency that is not locked to the sample rate the sampled peaks vary. When a peak comes in slightly lower than the held one the hold is not re-armed; it expires on the falling slope, `held` then follows the waveform down to zero, and the gain releases for several milliseconds. The result is gain modulation at beat frequencies: at 82.4 Hz the spectrum shows components at 39 Hz, 126 Hz and 4 Hz near -50 dBc. They are not harmonics, so `sig.py thd` does not count them.
- Between 62.5 Hz and about 100 Hz the hold makes the total residual worse than no hold (0.56 % against 0.48 % at 82.4 Hz, 0.83 % against 0.57 % at 70 Hz). It is still four times better than tubamp today.
- Below 62.5 Hz the hold expires just before the next peak and the dip is short, so 61.74 Hz and 55 Hz look clean; at 41.2 Hz the residual is 0.14 %.
- The same holds for the Squash prototype: 0.000 % at 100 Hz, residual 0.10 % at 98 Hz and 0.26 % at 82.4 Hz.

## Other measurement problems found on the way

- `cb.py timing` pads the aligned output with `lag` zeros and averages `g_post` up to 1 ms before the end. When the carrier period is shorter than `lag` minus 1 ms the zero tail is inside the average. This hits every UADx `tbF` file (lag 86, 6-sample window): `g_post_db` reads about 1.3 dB below `g_pre_db` in each (1176 Rev A -1.52 against -0.18, Rev E -4.86 against -3.54, 1176AE -8.00 against -6.71), although the 1 kHz files of the same settings recover fully. Release figures taken from those files are wrong: report lines 135, 136, 254 and the "43 ms" in the character table. Attack figures from the same files are not affected. NI and Plugin Alliance files have lag 0 or a 1 kHz carrier and are fine.
- Slow releases were measured on tails that end before the gain has recovered. Guitar Rig Stomp Compressor factory preset: the evidence file has `g_post_db` 9.4 against `g_pre_db` 16.0 on a 3 s tail and reports t63 1362 ms; with a 10 s tail I get t63 2893 ms and t90 6764 ms. The same symptom (`g_post` below `g_pre`) is in `ua_1176a/t_a100_r0.tbA` (1118 ms), `shadow_hills/t_dis_a0p1ms_recdual.tbL` (2319 ms) and `focusrite_sc/t_a0_r100.tbL` (1964 ms). Treat those four as lower bounds.

## The three top recommendations

- R1, Clean engine. The core follows from verified claims: what tubamp does today (C01-C04, also measured on the real AU), smoothing after the gain computer in the references (C06, C11, C12), the published design (C21), and the prototype's timing and ratio (C22). Without any hold the prototype already gives release = knob at every depth and 0.40 % at 100 Hz against 1.99 %. The detector hold does not follow. Its headline evidence, 0.000 % at 100 Hz, is the sample-locked special case; off that grid the hold adds inharmonic modulation and is worse than no hold from 62.5 to 100 Hz. The 8 ms value is a [guess] by the report's own section 8. The statement that the hold "bridges half a period down to 62.5 Hz" is false as implemented. Two acceptance rows (THD at 100 Hz under 0.1 %, THD at 82.4 Hz under 0.2 %) pass only because harmonic THD ignores the sidebands; a residual or THD+N measure at a non-locked frequency is needed.
- R2, gain-reduction meter. Follows from C18 (verified). The proof value is right: I measure 7.50 dB of reduction for a -10 dBFS sine. Two weak points. "Every reference has a GR meter" is asserted without a tag or evidence for most units. The ballistics (0.3 s to 99 %, 20 dB per 0.3 s, 20 dB scale) are NI's constants taken from the binary, which contradicts the section's opening line that all constants are the author's own or fitted.
- R3, mix, sidechain high-pass, auto makeup. Mix and sidechain filter rest on parameter listings and on the Shadow Hills measurement, which I re-rendered: 40 Hz gain reduction goes from 7.57 to 1.71 dB with the filter in, 1 kHz stays at 7.60 / 7.45 dB. The auto-makeup rule is the published one (C21) moved to a -10 dBFS reference; that reference level is a [guess] per section 8. The stated proof for the high-pass is reachable with a one-pole filter (I estimate 6.3 dB difference at the default settings). No dependence on a NOT VERIFIED claim.

Lower-ranked items that lean on weak evidence, for completeness. R4 quotes a factory release t63 of 1.4 s that is really at least 2.9 s, and its build parameters are a [guess]. R5 is sold against the gap "program-dependent release", but the sketch gives the same release after any burst length; it reproduces the two-slope shape of one 0.5 s burst only. Its slow-end mapping uses the truncated 1118 ms figure.

## Citations opened

- https://www.mathworks.com/help/audio/ug/dynamic-range-control.html : exists and says what the report attributes to it (quadratic knee, smoothing of the gain computer output, make-up = -x_sc(0)).
- https://www.dafx.de/paper-archive/2011/Papers/22_e.pdf : exists; it is the Dyna Comp analysis; C10 = 10 uF, R11 = 27 k, feedback of the bias current from the envelope on C10, one-sample delay. As attributed.
- https://www.uaudio.com/blogs/ua/1176-collection-tips : exists; it gives the 20 to 800 microsecond attack and the 50 ms to 1.1 s release. It does not say the threshold rises with the ratio. Report line 266 attributes that to this page; `evidence/lit-notes.txt` line 74 shows the statement came from a different document ("same Moore PDF"). The fact itself is supported by the measurement in C15.

## [inferred] or [guess] presented as fact

- Summary line 3: "Every reference that was read smooths the gain reduction after the gain computer". The report's own section 3 contradicts "every": Supercharger GT smooths `|sidechain|` in the linear domain before its curve (line 65), the Guitar Rig pedal smooths the squared detector signal (line 81), and the Guitar Rig tube compressor releases on the level (lines 103, 109). Section 5 states it correctly for three units.
- Summary line 4: "None of the references uses lookahead; oversampling appears only around saturation stages". The oversampling reading is tagged [inferred] in section 3.3, and for the UADx units the absence of lookahead is an inference from how the delay scales with sample rate. The Summary gives both as measured or decompiled fact.
- Summary line 5 and R1: "0.000 % THD at 100 Hz" is given without the condition that the tone is locked to the sample rate.
- R2: "every reference has a GR meter" carries no tag.
- Section 7 opening: "All constants below are ours". R2's are NI's.
- R1's 62.5 Hz statement and R4's and R5's guessed parameters are tagged correctly.

## Fences

- No PACE-protected binary was disassembled. The author's dumps (`$RE_STATE/work/comp/dis`, `sym`) cover Solid Bus Comp, Solid Dynamics, Supercharger, Supercharger GT, Transient Master, Guitar Rig 6 and AmpliTube 5 only; `static-notes.txt` names no UADx address; the Hopper cache has no UADx entry. UADx, Shadow Hills and Focusrite SC appear as black-box measurements only.
- No decompiled listing is pasted. The report's only code block is the design sketch (11 lines). `static-notes.txt` is 78 lines of one-line address notes across five binaries, with no instruction listing. No evidence file exceeds 40 kB.
- No vendor lookup table is in the recommended design. The Solid Bus tables appear only in the reference description (section 3.1). The one borrowed set of constants is the meter ballistics in R2 (three numbers), noted above.
- My own work: AmpliTube 5 and UADx API 2500 were not instantiated; AmpliTube was read statically only. Every render ran under an alarm. No GUI. Writes went only to this file and to `$RE_STATE/work/comp-verify/`. The disk filled up during the run (other sessions); I deleted my own scratch WAVs and nothing else.

## Overall judgment

The study's description of tubamp's current compressor is correct and now confirmed three ways: from the JUCE source, from that source compiled, and from the installed plugin with the compressor alone in its chain. The decompiled claims hold at the instruction level, including the roles of the constants, and the black-box numbers reproduce, with one exception: release times read from 8 kHz bursts on the UADx units are wrong because of a zero-padding error in the timing script, and a few slow releases are lower bounds because the test tails were too short. The top recommendation is sound in its core (log-domain gain computer, smoothing on the gain reduction, honest time constants, linked detection), but its headline feature, the detector hold with "0.000 % THD", is an artefact of testing at a frequency locked to the sample rate; at the low E the hold is slightly worse than no hold on total residual, and the proposed acceptance metric cannot see that. The hold should be re-tested at non-locked frequencies with a residual measure before it is built. R2 and R3 follow from verified evidence. One literature attribution is wrong (the 1176 threshold statement) and two Summary lines overstate what the body shows.
