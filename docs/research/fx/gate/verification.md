# Noise gate report: independent verification

Verifier re-read the source, re-disassembled every named address with `tools/re/disasm.py` or `objdump` on the cached thin slice, and re-rendered every measured claim.
Scratch (copies of `evidence/bb`, renders, dumps, extra scripts) is in `$RE_STATE/work/gate-verify/`.
Tolerance accepted for measurements: 0.1 dB on gains, 1 ms on times, unless stated.

Result: 18 VERIFIED, 2 NOT VERIFIED (C03, C19; both in part), 0 INCONCLUSIVE.

## Claims

| id | verdict | what I checked (command or address) | what I saw | note |
| --- | --- | --- | --- | --- |
| C01 | VERIFIED | `sed -n '27,31p;259p' src/dsp/FxBlocks.cpp` | 0.01, 0.1, 0.005, 0.01, 0.05. Line 259 passes them to `TriggerParams` in that order. | |
| C02 | VERIFIED | `NoiseGate.h:112-118`, `NoiseGate.cpp:20-23, 59-76, 179` | `-ratio*(L-T)^2` for L < T. `pow(10, x/10)` multiplies the sample. The same `threshold` is used in HOLDING (line 63) and MOVING (line 76). | |
| C03 | NOT VERIFIED (in part) | Rebuilt `ours`, `./run.sh ours:-40`. Then my own steady 1 kHz tones approached from -20 and from -90 dBFS (`gate-verify/indep.py`). | Falling ramp: -1.6, -12.4, -33.1, -63.9. Exact match. Rising ramp: -2.0, -13.3, -34.6, -65.9. Steady tones: -1.81, -12.84, -33.86, -64.88 from both sides. | The numbers hold only for the falling 10 dB/s ramp. "Same falling and rising" is contradicted by the report's own `rise_static` line (0.4 to 2.0 dB apart). The difference is detector lag, not hysteresis. The true static curve is 0.2 to 1.0 dB deeper than quoted and is direction-independent, so the conclusion "no hysteresis" stands. |
| C04 | VERIFIED | `./ours sig/burst.wav out/o60.wav -60 512`, `an.py burst`. Also a per-sample peak-ratio check in `indep.py`. | -1 dB at 64.0 ms (thr -40) and 131.8 ms (thr -60). My check: 64.3 and 131.8 ms. Thr -50 gives 97.3 ms. | 3.4 ms per dB over threshold, consistent with the report's 3.3. |
| C05 | VERIFIED | `./ours sig/chug.wav ... -50` and `-40`, `an.py gaps` | Gap gain -2.0 dB (thr -40), 0.0 dB (thr -50). | |
| C06 | VERIFIED | `--strings noise_gate`, `--doubles 0xedd80 4`, objdump 0xaaf38..0xab290, 0xc66e0..0xc6840, 0xab6ec | RTTI strings present. 0xaaf38 is the Trigger loop: exp2(-1/(time*sr)), clamp to [1e-12, 1000], 10*log10, -120 dB floor, quadratic law, 0.5 step, hold state. Call site writes 0.01 (immediate 0x3f847ae147ae147b) to +0x30, the pair {0.1, 0.005} from 0xedd80 to +0x40/+0x48 and {0.01, 0.05} from 0xedd90 to +0x50/+0x58, then `bl 0xaaf38`. Gain at 0xab6ec is exp10(GR/10)*x, called at 0xc682c after the model call. | Field offsets match the order ratio, open, hold, close read by the loop. |
| C07 | VERIFIED | `./run.sh gw aufx 72RG AAMa --set 1=-40 --set 7=0 --set 8=0 --set 11=0` | Both static lines equal the harness at every level to 0.1 dB. Opening identical. | Not part of the claim: the 82 Hz burst releases at 100 ms in Gateway against 64 ms in the harness. The report explains the 1 kHz tail by the DC blocker but does not mention this. |
| C08 | VERIFIED | `--addr 0x42c90` | `fmov s1,#-5.0; fadd s1,s9,s1; ... fmul by 0.05 (0x3d4ccccd); bl __exp10f`, stored at gate+0x38. Open = exp10f(thr*0.05) at gate+0x34. processChannel compares env with +0x34 when closed and +0x38 when open. | |
| C09 | VERIFIED | prepareToPlay 0x3dc94..0x3ddc4 and processChannel 0x46d8c..0x46dd4 | 80.0 (0x4054000000000000), tan(pi*80/sr), k = 1.41421, TPT state-variable coefficients, high-pass output `x - k*bp - lp`. 5000.0 with `1 - exp(-2*pi*5000/sr)` used as a one-pole low-pass. Hold = int(sr*0.05). Gain close coefficient `1 - exp(-1/(sr*0.1))` at +0x14, selected when target < gain. | |
| C10 | VERIFIED | 0x46e58..0x46e6c, 0x46d40, 0x42cc8 | `env * [gate+0x3c]`, cubed, `fmax` with 0x38d1b717 = 1e-4. +0x3c is written as 1/open at 0x42cc8..0x42cd0. | |
| C11 | VERIFIED | `--doubles 0x58e158 1`, `updateGEGainFactor` 0xefa58, constructor 0xedc18..0xedc28 | 4.8. Closed branch sets hyst = 3.0 and target 150. Open branch sets hyst = [state+0x70] + 3. The constructor stores the pair {0.0, 4.8} from 0x58e150 at state+0x68/+0x70 for both channel states (0xedc28, 0xedcf8). No other store to a state's +0x70 in the E_Dynamics code (0xedadc..0xefc14). | |
| C12 | VERIFIED | `./run.sh sdg aufx 'Ni$7' -NI- --set 1=1.0 --set 11=0 --set 12=0.522 --set 13=1.0 --set 14=1 --set 15=0` | Opens at -39.9, closes at -44.8 dBFS. Floor -41.9 dB. | Matches the code: thr = 68*0.522 - 72.5 = -37.0, so -40.0 and -44.8. The open gain is +5 dB at these settings (the compressor half is not neutral), which does not move the switching points. |
| C13 | VERIFIED | Rise ramp and my own step files at -48, -49, -49.5, -50, -51, -55 through `auhost render aumf bcfs Brwx` with the listed settings | Opens at -40.1. Steps to -48, -49, -49.5: gain 0.0 dB after 0.2, 1.0 and 2.9 s. Steps to -50, -51, -55: -69 dB after 1 s. | The recheck command cannot run as written: `stepdown.py gen` writes only -45, -50, -55, -60, -65, so there is no `step_49.wav`. I generated it. |
| C14 | VERIFIED | `--dump 'NI::DSP::NoiseGate::(processBuffer or onControlRate)'` | `L*L + R*R - [0xb4]`, sign change against the previous difference. Falling crossing copies [0x94] into the timer [0x90]. Rising crossing writes -1 to the timer. onControlRate sets state 0 and the release coefficient only when the timer is 0. No filter on the detector. | |
| C15 | VERIFIED | 0x29c6cc..0x29c704, 0x29c274, 0x29c294 | s14 = 0.5. env >= 0.5: gain = env. Else env^2; env >= 0.25: doubled; else times env times 8. env <= 1e-10: 0. | |
| C16 | VERIFIED | endInit 0x29b53c..0x29b560, processBuffer 0x29c470..0x29c4a0 | 0x44f50b33 = 1960.35, divided by the sample rate, stored to core+0xb0/+0xb4 (effect+0x13c, the pointer x25 used in the loop). The loop does `lp += (x - lp)*c` and takes max of abs(x - lp) over the two channels. 1960.35/2pi = 312.0 Hz. | |
| C17 | VERIFIED | 0x29b3c8..0x29b3d8, 0x29aab0, 0x29c2c8..0x29c300, 0x29c28c, 0x29bc70 | Gate: `fadd s3,s3,s3; str s3,[x0,#0xb4]` on a rising crossing while the learn flag is set. NR: max(abs L, abs R) times 4.0, kept if larger than the current threshold. Both windows: round(0.5*sr/controlDivider) ticks. | Gate wording is slightly loose. The code sets T = 2p at each upward crossing, so the result is 2 times the last sample that crossed. That is at most 2*max and can be less. Does not change R5. |
| C18 | VERIFIED | `--addr 0x1793718`, `--floats 0x318a520 4`, Preallocate 0x1443910..0x1443964, OnSetParameter 0x179385c | Loop: env > thr*[0x1c] clears the counter; env <= thr*[0x20] counts up to hold; then exponential close to floor [0x18]. Literal {0, 0.001, 1, 1} is stored at struct+0x14, so +0x1c and +0x20 are 1.0. OnSetParameter stores to +0x8, +0x14, +0x18, +0x28, +0x3c, +0x40 only. | |
| C19 | NOT VERIFIED (in part) | `python3 proto.py newhard -40 $RE slope=1 rel=0.03 hold=0.01`, `./run.sh ext:newhard`. Then all three bursts with `an.py burst`, a note-off phase sweep (`gate-verify/phase.py`), `hp=0`, and thr -60. | The command prints the claimed numbers: -20 dB at 29.7 ms, gap -90 dB, onset -0.5 dB, 0 crossings. But the third burst in the same output file gives 34.5 ms. Phase sweep at thr -40: 30.0 to 39.1 ms. Without the sidechain filter: 41 ms. At thr -60: 38 to 47 ms (1 kHz). | 30 ms is the best case. The time depends on where the note ends inside the 12 ms peak-hold window (up to 12 ms of jitter) and on level (0.35 ms per dB, as R1 itself says). The chug, onset and wobble figures are verified. The prototype also runs a 90 Hz sidechain high-pass by default, so this "R1" measurement includes R2. |
| C20 | VERIFIED | `src/PluginProcessor.cpp:1043-1044, 1229-1233, 1280-1283`, `src/dsp/FxBlocks.cpp:326-345` | `engine.process(mono, n)` then `gate.apply(mono, n)` in the same call. `apply` multiplies by the curve of the same block. No delay line anywhere on the curve. | The size of the misalignment was not measured by the report or by me. `NamEngine` reports latency only when it resamples. |

## Top three recommendations

**R1 (new engine, Tight and Smooth modes).** The need follows from verified claims: C01 to C05 (what ours does), C08 to C13 (hysteresis, hold and floors in the references). The design direction is supported. Three things in R1 lean on C19 and do not hold as written:
- "The Python prototype already meets the first four" acceptance tests. It does not meet "-20 dB within 35 ms at 1 kHz at thresholds -40 and -60". I get 38 to 47 ms at -60 and 30 to 39 ms at -40 depending on phase. The 82 Hz limit (50 ms) is met per sample (39 to 49 ms) but `an.py` with its 24 ms window reports 52 to 59 ms at -60. Step-down was not in the evidence; I ran it and it passes (open at -45.9, closed at -46.1).
- The Summary says the prototype closes "independent of level". It does not. The level term is 0.35 ms per dB and the window adds up to 12 ms.
- Unstated: the Tight numbers were taken with the 90 Hz sidechain filter on. The Smooth numbers in section 4 (31 ms, 57 ms) were taken with hold = 20 ms, the script default, not the 10 ms default in R1's parameter table (with 10 ms I get 21 and 46.5 ms). "Less CPU than mode 0" is not measured. The prototype is mono Python.
Either the 35 ms threshold or the detector (window length, decay) needs to change before this is used as an acceptance test.

**R2 (sidechain high-pass).** Rests on C09 and C16 (verified) and the patent (checked, says what is claimed). The Butterworth numbers are right (-12.3, -9.4, -5.0 dB). The benefit for tubamp is not measured; the report says so. The gap-table line "50/60 Hz hum holds the gate open" is inference stated as fact. Unstated: a 100 Hz corner also lowers the detector reading of low E by 5 dB, which shifts the effective threshold for low notes. The prototype used 90 Hz, not 100 Hz.

**R3 (gate-state meter).** Not backed by any row of claims.tsv. I checked its three references directly and they hold: GR `displayGateLamp` 0x29b244 returns 1 - gain; AT writes 1 or 0 at 0x1793824..0x1793838 and the string `GateState` is in the stomp profile at 0xaa787c; SD has `getGateMeter` adaptors at 0xe760. tubamp already publishes meters through atomics (`src/PluginProcessor.h:201, 380`). This is a UI feature; it follows from the references and needs no measurement.

## Citations opened

| url | exists | says what the report attributes to it |
| --- | --- | --- |
| https://www.thomann.de/be/fortin_zuul_plus_blackout_noise_gate.htm | yes | Three controls (threshold, hold, release): yes. Key In and Out jacks: yes. "Key input fed from the guitar while the gate sits in the amp loop": only in a customer review on that page, not in the product description. Weak source for that sentence. |
| https://en.wikipedia.org/wiki/Noise_gate | yes | Yes. Two thresholds, the close one "a few dB below". |
| https://patents.google.com/patent/US4696044A/en | yes | Yes. Two single-pole 6 dB/oct high-pass stages (12 dB/oct) ahead of the detector, and avoiding "breathing" on a small level surge. Title is "Dynamic noise reduction with logarithmic control" (Waller, later assigned to Rocktron). |

## Inferred or guessed items presented as fact

- Summary line 5: "closes 20 dB in 30 ms independent of level", tagged [measured]. Only one threshold was measured, and level independence is false (see C19).
- Summary line 4: "Every reference gate uses some of ...". Gateway is a studied reference and uses none of them (same code as ours, 10 ms hold). Guitar Rig and AmpliTube figures in that line come from code only; they were not rendered.
- Summary line 1: TONE3000 "has replaced" the AudioDSPTools gate, tagged [decompiled]. What is decompiled is that it has its own gate and no `noise_gate` symbols. "Replaced" is inference.
- R1: "The Python prototype already meets the first four", tagged [measured]. Two of the four were not in the evidence at the stated settings and one fails (above).
- R1: "less than mode 0" CPU. No measurement. A guess without a tag.
- R4: "Today the gate shuts that many samples before the tail it was measured on." Stated as fact; section 8 admits it is inferred from code. Section 6 repeats it as fact ("curve leads the audio when the amp resamples").
- Section 2, opening line: "identical falling and rising" (see C03).
- R5 and the ISP sentence are tagged [inferred] and [guess] correctly.

## Fences

- No PACE-protected binary was disassembled. The report, the evidence files and every dump in `$RE_STATE/work/gate/` name only Guitar Rig 6, AmpliTube 5, Solid Dynamics, Gateway and TONE3000. bx_console was measured black box only; there is no thin slice or dump of it.
- No decompiled listing over 30 lines. The longest fenced block is 17 lines (`evidence/gr6_noisereduction.md`), and it is the author's own pseudo-code. There are no raw disassembly lines in the report or evidence. No lookup table is copied.
- I disassembled the same five binaries and nothing else. I wrote only this file and scratch under `$RE_STATE/work/gate-verify/`.

## Overall judgment

The static reading is sound. Every decompiled constant I checked is at the stated address and is used the way the report says, and every black-box number reproduces from the listed commands. The description of tubamp's current gate and its weaknesses (no hysteresis, level-dependent close delay of about 3.3 ms per dB, no closing in 75 ms gaps) is verified. The weak part is the prototype evidence for R1: the headline "30 ms, independent of level" is a best case from one burst at one threshold, the prototype misses the report's own 35 ms acceptance limit at threshold -60, and its measurements quietly include the R2 sidechain filter. C03's "same falling and rising" is contradicted by the report's own data, though the point it supports is true. The recommendations can go ahead as design direction; the R1 acceptance numbers and the Summary's last line should not be relied on as written.
