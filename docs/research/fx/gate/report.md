# Noise gate: reference study and improvement spec

## Verification status

Independently verified on 2026-11-11: 18 of 20 claims verified, 2 partly wrong. The full table is in `verification.md`.
Read these corrections before relying on the numbers below. The body of the report is unchanged.

- C03: the static gains are 1 to 2 dB lower on a rising ramp (-2.0 / -13.3 / -34.6 / -65.9 dB) than on a falling one. Steady tones agree from both sides, so "no hysteresis" holds.
- C19: the Tight prototype's 29.7 ms close time is its best case. It is 30 to 39 ms depending on where the note stops and 38 to 47 ms at a -60 dB threshold, which misses the 35 ms limit this report sets. "Independent of level" does not hold.
- The Tight numbers include the 90 Hz sidechain filter. The Smooth numbers used a 20 ms hold, not the proposed 10 ms.
- Stated as fact but inferred: "every reference gate uses hysteresis, hold or range" (Gateway uses none), "less CPU than mode 0", that TONE3000 "replaced" the gate, and the misalignment behind recommendation 4.

Binaries: GR = `Guitar Rig 6`, AT = `AmpliTube 5`, SD = `Solid Dynamics`, GW = `Gateway`,
T3 = `TONE3000`, all under `/Library/Audio/Plug-Ins/Components/<name>.component/Contents/MacOS/`.
Evidence files are in `evidence/`. `bb/` means `evidence/bb/` (signal generator, analyser,
runner, prototype). Levels are sine peak dBFS unless stated.

## 1. Summary

- tubamp ships the AudioDSPTools gate unchanged. Gateway ships the same code with the same constants; TONE3000 has replaced it with its own gate (band-passed sidechain, 5 dB hysteresis, 50 ms hold) [decompiled: GW 0xaaf38, T3 0x46d28].
- Our gate has no hysteresis, no range and no sidechain filter, and its close delay grows with level: 3.3 ms per dB above threshold, 64 ms to 132 ms in our tests [measured: evidence/blackbox_results.txt].
- It does not close between staccato palm mutes at all (gap gain 0 to -2 dB) [measured: evidence/blackbox_results.txt res_ours_-40.txt].
- Every reference gate uses some of: hysteresis (4.8 to 9.5 dB), a hold timer (20 to 100 ms), a range floor, a detector high-pass (80 to 312 Hz), or a progressive expander law that follows the note down.
- Proposal: keep the old gate as mode 0, add a Tight mode and a Smooth mode built on a peak-hold detector. The prototype closes 20 dB in 30 ms independent of level and mutes the palm-mute gaps [measured: evidence/blackbox_results.txt res_proto_tight.txt].

## 2. tubamp today

Source: `src/dsp/FxBlocks.h:69-109`, `src/dsp/FxBlocks.cpp:27-31,220-345`, and
`build/_deps/neuralampmodelercore-src/Dependencies/AudioDSPTools/dsp/NoiseGate.{h,cpp}` (below: `NG.h`, `NG.cpp`).

| aspect | what we ship | where |
| --- | --- | --- |
| player parameter | `gate_threshold`, -100..0 dB, step 0.1, default -80 | `src/Parameters.cpp:64` |
| sidechain | mean of all channels (sum / numChannels), no filter | `src/dsp/FxBlocks.cpp:318` |
| detector | one-pole on x^2, `alpha = 0.5^(1/(0.01*fs))`: 10 ms half-life, 14.4 ms time constant; clamped to [1e-12, 1000] | `NG.cpp:41,57`; `kGateTime` `FxBlocks.cpp:27` |
| level | `10*log10(power)`, so a sine reads 3 dB under its peak; floor -120 dB | `NG.cpp:17,58`; `NG.h:26` |
| gain law | `GR = -0.1*(L - T)^2` for L < T, else 0 (`kGateRatio = 0.1`) | `NG.h:116`; `FxBlocks.cpp:28` |
| gain applied | `10^(GR/10)` on amplitude, so the audible reduction is `0.2*(L-T)^2` dB: 5 dB under threshold gives -5 dB, 10 dB gives -20 dB, 20 dB gives -80 dB | `NG.cpp:22,179` |
| range | none. Maximum reduction is `-0.1*(-120 - T)^2`, which is silence for any usable threshold | `NG.h:117` |
| hysteresis | none: one threshold for opening and closing | `NG.cpp:62,76` |
| hold | HOLDING state forces 0 dB; it ends after the level has been under threshold for 10 ms (`kGateHoldTime`), the timer resets whenever the level is at or over threshold | `NG.cpp:59-73`; `FxBlocks.cpp:30` |
| open / close | MOVING state moves half the remaining distance per sample, slew-limited to `maxGR/0.005 s` opening and `maxGR/0.05 s` closing. The limits scale with threshold: 64 and 6.4 dB/ms at T = -80, 256 and 25.6 dB/ms at T = -40 [inferred from the formulas] | `NG.cpp:48-49,77-95`; `FxBlocks.cpp:29,31` |
| lookahead | none | `NG.cpp` |
| pre/post split | `measure()` runs at the gate's chain position; when an amp follows in the same segment the gain is applied to the NAM output, otherwise in place | `src/PluginProcessor.cpp:1043-1044,1233,1280-1283` |
| latency between the two | not handled: the curve of block n multiplies the amp output of block n, although the NAM resampler and any block between gate and amp can add latency [inferred: `src/PluginProcessor.cpp:1233`, `src/dsp/NamEngine.cpp:237`] | |
| stereo | one curve for all channels | `src/dsp/FxBlocks.cpp:326-336` |
| cost | double precision, one `log10` and one `pow` per sample | `NG.cpp:58,179` |

Fixed today: detector time 10 ms, ratio 0.1, open 5 ms, hold 10 ms, close 50 ms, floor -120 dB,
the quadratic law, the 0.5 approach factor.

Measured with the real AudioDSPTools code compiled into a harness (`bb/ours.cpp`), threshold -40:
static gain -1.6 / -12.4 / -33.1 / -63.9 dB at -40 / -45 / -50 / -55 dBFS, identical falling and rising;
opens to -1 dB in 1.2 ms; after a -20 dBFS burst stops the gain is still above -1 dB for 64 ms
and reaches -20 dB at 90 ms; with threshold -60 the same burst holds 132 ms [measured: evidence/blackbox_results.txt].

## 3. Reference implementations

### 3.1 Guitar Rig 6 "Noise Gate" (NI::DSP::NoiseGate)
Details: `evidence/gr6_noisegate.md`.
- Detector: the instantaneous `L^2 + R^2` compared with a power threshold. No envelope filter, no sidechain filter [decompiled: GR 0x29b394].
- One threshold. A falling crossing arms a hold timer, a rising crossing disarms it and starts the attack; the gate closes only when the timer runs out at control rate [decompiled: GR 0x29b348, 0x29b41c, 0x29b270].
- Hold 0.1 s to 30 s, attack 0.1 ms to 1 s as a linear gain ramp, release a one-pole decay with tau 1 ms to 10 s, threshold shown as -63 to -3 dB [decompiled: GR 0x29adb0, 0x29acdc, 0x29ae58, 0x13290c].
- Closes to silence. No range, no lookahead [decompiled: GR 0x29b30c].
- Learn: 0.5 s window with the gate shut; the threshold becomes twice the largest `L^2+R^2` seen (+3 dB) [decompiled: GR 0x29b1e8, 0x29b3d0].
- Lamp value is `1 - gain` [decompiled: GR 0x29b244].

### 3.2 Guitar Rig 6 "Noise Reduction" (NI::DSP::NoiseReduction)
Details: `evidence/gr6_noisereduction.md`. This is a downward expander plus an optional sliding low-pass, not a gate.
- Detector: each channel minus its own 312 Hz one-pole low-pass (a 6 dB/oct high-pass), rectified, maximum of the two channels, divided by the threshold and clipped at 1 [decompiled: GR 0x29c470, 0x29b53c].
- Peak follower with instant attack and a release of 75 dB per release time (200 ms to 3.2 s) [decompiled: GR 0x29bb54].
- Envelope: rises at most 0.002 per sample (0 to 1 in 500 samples); falls by `coef^(0.1 + 9.9*(env - peak))`, so it releases up to 100 times faster when it is far above the peak than when it rides on it [decompiled: GR 0x29c4b0, 0x29c6a0].
- Gain law on the normalized envelope e: `e` down to 0.5, `2e^2` down to 0.25, `8e^3` below. That is 2:1, 3:1 then 4:1 expansion, continuous at the joins [decompiled: GR 0x29c6d8..0x29c700].
- Hiss filter: a one-pole low-pass on the audio with `fc = 1000 + 9500*v` Hz. v is 2 when the mid signal above 2 kHz is at the hiss threshold and falls by 1 per 20 dB under it; it rises instantly and decays with tau 0.45 s [decompiled: GR 0x29c604..0x29c678, 0x29b4b8].
- Learn: threshold = 4 x the largest absolute sample in 0.5 s (+12 dB) [decompiled: GR 0x29c28c, 0x29c2f4].
- No hysteresis, no hold, no lookahead [decompiled: GR 0x29c1a0].

### 3.3 AmpliTube 5
Details: `evidence/at5_gates.md`.
- "Noise Gate" stomp: peak detector with instant attack and exponential release (0.02 s setting), separate open and close threshold factors, a hold counter, one-pole opening, exponential closing down to a floor, and a "GateState" readout [decompiled: AT 0x1793718, 0xaa79d8]. Both threshold factors are initialised to 1.0 and never written, so the shipped stomp has no hysteresis [decompiled: AT 0x1443910]. Declared ranges: threshold -100..0 (default -50) dB, attack 0.5..2 (1) ms, hold 50..200 (100) ms, release 20..1500 (200) ms, depth -100..-20 (-60) dB [decompiled: AT 0x17930d8; name-to-range pairing inferred].
- MXR Smart Gate model: |x| follower with 7 ms attack and 200 ms release (time to 10 %), a static level-to-gain table, three modes. Full mode bottoms out at -30 dB 25 dB under the knee; hiss and mid modes only move a high shelf (3 kHz or 1.5 kHz, up to -22 dB) over an 11 dB transition [decompiled: AT 0xd8ac78, 0xd8a970, 0xd8b2e4].
- Rocktron Hush IIB model, also used by the "Stargate" stomp: feedback compressor, feed-forward expander, and a two-pole low-pass whose cutoff is linear in a rail-limited control voltage, `fc = 6402.27*c + 72789.8` Hz [decompiled: AT 0xef13cc, 0xef178c, 0x184c560].
- All three run per channel inside a dual-mono wrapper with a link parameter [decompiled: AT symbol `ModuleDualMono<IK::MAF::AT3::NoiseGate, LinkParameter>` 0x1270584].

### 3.4 Solid Dynamics gate/expander (NI::DSP::CSL::E_Dynamics)
Details: `evidence/solid_dynamics_ge.md`.
- Level is the instantaneous sample level in dB (the smoothing coefficient is zero at audio rates), then a dB-domain envelope with a 0.1 ms or 13 ms attack and the release time [decompiled: SD 0xee5d4, 0xefa68, 0xeecd0, 0xeef74].
- Gate: opens at threshold - 3 dB, closes at threshold - 7.8 dB. Hysteresis 4.8 dB, no hold [decompiled: SD 0xefad4..0xefb1c, literal 0x58e158]. Measured: opens at -39.9, closes at -44.8 dBFS [measured: evidence/blackbox_results.txt res_sd_gate.txt].
- Closing: the reduction in dB runs a one-pole toward 150 dB and is clipped at Range, so the gain falls at a near-constant dB/s to the floor [decompiled: SD 0xefae4, 0xefb44..0xefb64].
- Expander: reduction = threshold - level, a 1:2 slope, no hysteresis [decompiled: SD 0xefaa4]. Measured 1.0 dB of reduction per dB [measured: evidence/blackbox_results.txt res_sd_exp.txt].
- Range floor measured 47 dB under the open gain in gate mode [measured: evidence/blackbox_results.txt res_sd_gate.txt].

### 3.5 Gateway (Atkinson Advanced Modeling)
Details: `evidence/gateway_gate.md`. It is the AudioDSPTools gate: same Trigger and Gain code, same constants
(0.01, 0.1, 0.005, 0.01, 0.05), trigger before the model and gain after it [decompiled: GW 0xaaf38, 0xab6ec, 0xc671c..0xc682c].
Static curve and opening time match our harness to 0.1 dB [measured: evidence/blackbox_results.txt res_gw.txt].
Parameters: Threshold -100..0, default -80, and an on/off switch [measured: `auhost params aufx 72RG AAMa`].

### 3.6 Plugin Alliance bx_console Focusrite SC (black box only)
- Controls: threshold -40..+20 dB, range 0..80 dB, hold 20 ms..4 s, release 0.1..5 s, fast attack switch, expander switch, key input, link [measured: `auhost params aumf bcfs Brwx`, evidence/blackbox_results.txt].
- Gate: opens at -40.1 dBFS; after a step down it stays open indefinitely at -49 and closes at -50. Static hysteresis about 9.5 dB [measured: evidence/blackbox_results.txt "step-down"].
- After a burst stops: -1 dB at 31 ms (20 ms hold), -6 dB at 37 ms, -20 dB at 147 ms, then a slow tail to the floor; floor -69 dB at the 80 dB setting [measured: evidence/blackbox_results.txt res_bx_gate.txt].
- Attack: under 0.3 ms with the fast switch, -1 dB at 4.3 ms without [measured: evidence/blackbox_results.txt res_bx_gate.txt, res_bx_gate_slow.txt].
- Expander: 0.49 dB of reduction per dB under the knee (about 1:1.5), soft knee, no hysteresis [measured: evidence/blackbox_results.txt res_bx_exp.txt].
- bx_rockrack V3 Player exposes only Power, Master Out and Preset; its gate cannot be set from the host [measured: `auhost params aufx bxrp Brwx`]. Not measured.

### 3.7 TONE3000
Details: `evidence/tone3000_gate.md`. No AudioDSPTools gate symbols; its own `NoiseGate::processChannel`:
- Sidechain: 80 Hz second-order Butterworth high-pass, then a 5 kHz one-pole low-pass [decompiled: T3 0x3dd44..0x3ddc4].
- Envelope of the absolute value: 0.2 ms attack, 25 ms release [decompiled: T3 0x3dcb4, 0x3dcd8].
- Open threshold `10^(thr/20)`, close threshold 5 dB lower, hold 50 ms [decompiled: T3 0x42c78..0x42cc4, 0x3dd28].
- Closed target `max((env/open)^3, 1e-4)`: a cubic expander law with a -80 dB floor instead of a mute [decompiled: T3 0x46e58..0x46e6c].
- Gain smoothing 0.2 ms opening, 100 ms closing [decompiled: T3 0x3dd20, 0x3dcf4].
- Applied in place per channel at the gate position; no pre/post split in this function [decompiled: T3 0x42d1c].
- A Python model built from these constants keeps the gain above -1 dB for 133 ms after a burst and does not close in 75 ms palm-mute gaps [measured: evidence/blackbox_results.txt res_t3k_model.txt; model, not the plugin].

TONEX was not studied.

## 4. Measurements

All from `evidence/blackbox_results.txt`; one block per row, produced by `bb/run.sh`. Thresholds set so each gate opens near -40 dBFS.

| gate | opens at | closes at (static) | hysteresis | floor | -1 dB after note-off | -20 dB after note-off | open to -1 dB |
| --- | --- | --- | --- | --- | --- | --- | --- |
| tubamp today (thr -40) | -39 (soft) | same | 0 | mute | 64 ms | 90 ms | 1.2 ms |
| tubamp today (thr -60) | | | 0 | mute | 132 ms | 166 ms | |
| Gateway | -39 (soft) | same | 0 | mute | 66 ms | 100 ms | 1.2 ms |
| Solid Dynamics gate, fast attack, min release | -39.9 | -44.8 | 4.9 | -47 dB | 66 ms | 79 ms | < 0.3 ms |
| bx_console gate, fast attack, min hold/release | -40.1 | -49 to -50 | 9.5 | -69 dB | 31 ms | 147 ms | < 0.3 ms |
| TONE3000 (model from constants) | -40 | -45 | 5 | -80 dB | 133 ms | 352 ms | 0.8 ms |
| prototype Tight (R1) | -40 | -46 | 6 | -90 dB | 20 ms | 30 ms | 1.0 ms |
| prototype Smooth (R1) | -40 | -46 (soft) | 6 | -90 dB | 31 ms | 57 ms | 1.0 ms |

"Note-off" is the 1 kHz burst test: -20 dBFS for 0.3 s, then -70 dBFS. Gateway's -20 dB time is set by a residue under -90 dBFS from its DC blocker, not by the gate (`evidence/gateway_gate.md`).

Palm-mute test (`chug.wav`: 82 Hz notes of 75 ms every 150 ms over -66 dBFS RMS noise; gain measured in the last 10 ms of each gap and in the first 10 ms of each note):

| gate | gap gain | first 10 ms of the note |
| --- | --- | --- |
| tubamp today, thr -40 | -2.0 dB | 0.0 dB |
| tubamp today, thr -50 | 0.0 dB | 0.0 dB |
| Gateway, thr -40 | -1.6 dB | -0.2 dB |
| Solid Dynamics gate | not closed | |
| bx_console gate | -5.3 dB | -0.2 dB |
| Solid Dynamics expander | -10.9 dB | -5.8 dB (13 ms attack) |
| TONE3000 model | 0.0 dB | 0.0 dB |
| prototype Smooth | -19.2 dB | 0.0 dB |
| prototype Tight | -90 dB | -0.5 dB |

Level wobble test (`hover.wav`, 1 kHz at -40 dBFS, +-3 dB at 3 Hz, threshold at -40): tubamp today swings between 0 and -7 dB with 32 crossings of -6 dB in 6 s; Solid Dynamics, bx_console and both prototypes stay open with 0 crossings [measured: evidence/blackbox_results.txt, "hover" lines].

## 5. Industry practice

What the references agree on:
- Detect on the dry guitar. Gateway keeps the NAM split (trigger before the model, gain after) [decompiled: GW 0xc6760, 0xc682c]. Hardware does the same with a key input fed from the guitar while the gate sits in the amp loop [literature: https://www.thomann.de/be/fortin_zuul_plus_blackout_noise_gate.htm].
- Nobody uses lookahead: none of the five decompiled detectors delays the audio [decompiled: GR 0x29b30c, AT 0x1793718, SD 0xefa58, T3 0x46d28, GW 0xaaf38]. Opening in 1 ms or less is the norm [measured: evidence/blackbox_results.txt].
- Hard gates use two thresholds a few dB apart: 4.8 dB (SD), 5 dB (T3), 9.5 dB (bx_console) [decompiled: SD 0x58e158, T3 0x42c90; measured: evidence/blackbox_results.txt]. The textbook description is the same [literature: https://en.wikipedia.org/wiki/Noise_gate].
- Hold exists wherever the detector is fast: 20 ms minimum (bx_console), 50 ms (T3), 100 ms default (AT), 100 ms minimum (GR) [measured: evidence/blackbox_results.txt; decompiled: T3 0x3dd28, AT 0x1793220, GR 0x29adb0]. The Fortin Zuul+ has exactly three knobs: threshold, hold, release [literature: https://www.thomann.de/be/fortin_zuul_plus_blackout_noise_gate.htm].
- A floor instead of a mute is common: -30 dB (MXR model), -60 dB default (AT), a range control (SD, bx_console), -80 dB (T3) [decompiled: AT 0x317c890, 0x1443910; T3 0x46d40].
- The detector ignores hum: 312 Hz one-pole high-pass (GR Noise Reduction), 80 Hz two-pole (T3) [decompiled: GR 0x29b53c, T3 0x3dd44]. The Rocktron patent puts two cascaded 6 dB/oct high-pass stages ahead of its detector [literature: https://patents.google.com/patent/US4696044A/en].
- "Musical" units are expanders, not gates: 2:1 to 4:1 progressive (GR), cubic (T3), 1:2 (SD, Hush model), 1:1.5 (bx_console) [decompiled: GR 0x29c6d8, T3 0x46e58, SD 0xefaa4; measured: evidence/blackbox_results.txt res_bx_exp.txt].
- Dynamic filtering is a separate, optional layer: a low-pass or shelf that closes as the level falls (GR hiss filter, MXR model, Hush model) [decompiled: GR 0x29c628, AT 0xd8b2e4, AT 0xef178c]. The patent states the aim: avoid the "breathing" of a wide-open filter on small level surges [literature: https://patents.google.com/patent/US4696044A/en].
- Program-dependent release: GR Noise Reduction speeds its envelope release up to 100 times when the signal has collapsed [decompiled: GR 0x29c244]. ISP advertises the same idea for the Decimator; I could not open a primary source [guess].
- Learned thresholds: only Guitar Rig, a 0.5 s peak capture plus a margin [decompiled: GR 0x29b3d0, 0x29c2f4].
- Stereo: sum of squares (GR gate), maximum (GR NR), linkable dual mono (AT, SD, bx_console), unlinked per channel (T3) [decompiled: GR 0x29b3a4, 0x29c488; T3 0x42ce0].

What makes a gate feel tight without eating sustain, in numbers:
- The clamp must not depend on how hard the note was hit. Ours waits 3.3 ms per dB over threshold because a 10 ms half-life power detector falls at 0.3 dB/ms [measured: evidence/blackbox_results.txt; inferred from `NG.cpp:41`]. A 30 dB hot palm mute therefore rings for about 100 ms before the gate moves.
- 16th notes at 200 bpm are 75 ms apart. To be heard as tight the gate has to be 20 dB down within roughly 30 to 40 ms of the mute and open again within 1 ms [inferred from the chug test geometry].
- Sustain is protected by the distance between the thresholds, not by a slow detector: a note that fades through the open threshold keeps ringing until it is 5 to 10 dB lower [measured: evidence/blackbox_results.txt res_sd_gate.txt, step-down].
- An expander law below the close threshold lets the tail fade instead of stopping: 3:1 gives -8 dB at 4 dB under and -18 dB at 9 dB under [measured: evidence/blackbox_results.txt res_proto_default.txt].

## 6. Gap analysis

| aspect | ours | references | consequence for the player |
| --- | --- | --- | --- |
| close delay | 3.3 ms per dB over threshold (64 to 132 ms measured) | 20 to 66 ms, set by hold | chugs are not tight; noise fills the gaps between palm mutes |
| hysteresis | none | 4.8 to 9.5 dB on hard gates | level wobble at the threshold is expanded by up to 7 dB; decaying notes flutter |
| hold | 10 ms, only counted under threshold | 20 to 100 ms, adjustable | no control over tightness |
| release | follows the detector and a quadratic law; nominal 50 ms is not what happens | a time in ms, or dB/s | player cannot trade tightness against tail |
| range | mute only | floor from -30 to -80 dB or a range knob | no "just reduce the hiss by 20 dB" setting |
| law | quadratic in dB, fixed | hard gate or 1.5:1 to 4:1 expander, selectable in SD and bx_console | one feel only |
| sidechain filter | none | 80 Hz two-pole to 312 Hz one-pole | 50/60 Hz hum holds the gate open; threshold has to go above the hum |
| threshold default | -80 dB, far under any real noise floor | -50 dB (AT), learn (GR) | gate does nothing until the player finds the knob |
| meter | none | lamp or gain-reduction value in GR, AT, SD | player sets the threshold blind |
| pre/post split | yes | Gateway same; hardware key input | keep |
| latency alignment | none between trigger and deferred gain | n/a in references | curve leads the audio when the amp resamples |
| dynamic low-pass | none | GR, MXR model, Hush model | hiss audible during decays |
| CPU | double, log10 + pow per sample | float, table or polynomial exp/log in GR | small but avoidable |

## 7. Recommendations

Parameter ids are new and appended; `gate_threshold` keeps its id, range and meaning in mode 0.

### R1. Add a new gate engine; keep the old one as mode 0 (highest benefit)
Build: `gate_mode` 0 = Classic (today's code path, untouched, bit-for-bit), 1 = Tight (hard gate), 2 = Smooth (expander tail).
Evidence: section 4 tables; hysteresis, hold and range in section 5.

Design (ours; per sample on the mono sidechain `s`, `fs` = sample rate):
```
a   = |sc(s)|                               sc = R2 filter, or s
if a >= pk:      pk = a; n = Nwin           Nwin = 12 ms: longer than half a period of 42 Hz
elif n > 0:      n -= 1                     peak is held for one window
else:            pk = pk*kd + a*(1-kd)      kd = exp(-1/(3 ms*fs)): fast fall once the note is gone
open = 10^(T/20); close = open*10^(-H/20)   H = 6 dB
if pk >= open:               state = OPEN; h = Nhold
elif state == OPEN:
    if pk >= close:          h = Nhold      still inside the hysteresis band
    elif h > 0:              h -= 1
    else:                    state = CLOSED
target_dB = 0                               if OPEN
          = range                           if CLOSED and mode Tight
          = clamp((S-1)*20*log10(pk/close), range, 0)   if CLOSED and mode Smooth, S = 3
gr_dB moves toward target_dB by at most 60/(attack*fs) up or 60/(release*fs) down per sample
gain = 10^(gr_dB/20)                        applied where the old gain is applied
```
Where the constants come from: Nwin = 12 ms is half the period of 42 Hz, the lowest note we expect, so the held peak cannot droop between half-waves [inferred]. kd = 3 ms costs 0.35 ms per dB, against 3.3 ms per dB today [inferred; measured for today: evidence/blackbox_results.txt]. H = 6 dB sits inside the 4.8 to 9.5 dB the references use and above the 0 dB ripple of a held peak [measured: evidence/blackbox_results.txt]. The dB-linear ramp is the shape Solid Dynamics produces and the shape of our own legacy slew limit [decompiled: SD 0xefb44; `NG.cpp:48-49`]. S = 3 is the middle of the 2:1 to 4:1 range in the references.

New parameters:
| id | range | default | note |
| --- | --- | --- | --- |
| `gate_mode` | 0 Classic, 1 Tight, 2 Smooth | 0 when absent from a saved state; 1 for new instances | follow the engine-type pattern of reverb and delay |
| `gate_release` | 5..500 ms (time for 60 dB) | 30 ms Tight, 80 ms Smooth | one parameter; UI shows the mode's default |
| `gate_hold` | 0..250 ms | 10 ms | |
| `gate_range` | -90..0 dB | -90 | -90 is treated as mute |

Fixed: attack 0.5 ms for 60 dB, H = 6 dB, Nwin 12 ms, kd 3 ms, S = 3. In modes 1 and 2 the threshold is the peak level of the sidechain in dBFS (mode 0 keeps the mean-square reading, 3 dB lower for a sine).
Cost: one abs, a few compares, one `exp` per sample (or every 8 samples with linear interpolation); less than mode 0. No latency.
Compatibility: mode 0 calls the existing AudioDSPTools objects exactly as now. Old sessions have no `gate_mode` and load as 0.
Proof (commands in `bb/`): `./run.sh` on the built plugin with mode 1, threshold -40.
- burst test: -20 dB within 35 ms of note-off at 1 kHz and within 50 ms at 82 Hz, at thresholds -40 and -60 (today 90 and 166 ms);
- chug test: gap gain <= -30 dB and first-10-ms gain >= -1 dB (today -2 and 0 dB);
- hover test: 0 crossings of -6 dB (today 32);
- step-down test: stays open 5 dB under the open threshold, closes 7 dB under;
- mode 0 output is bit-identical to the current build on every test signal.
The Python prototype (`bb/proto.py`) already meets the first four [measured: evidence/blackbox_results.txt res_proto_tight.txt, res_proto_default.txt].

### R2. Sidechain high-pass in modes 1 and 2
Build: a second-order Butterworth high-pass on the detector input only, default 100 Hz. At 100 Hz the detector sees 50 Hz at -12.3 dB, 60 Hz at -9.4 dB and low E (82.4 Hz) at -5.0 dB [inferred: Butterworth magnitude formula].
Evidence: GR Noise Reduction 312 Hz, TONE3000 80 Hz, Rocktron patent 12 dB/oct [decompiled: GR 0x29b53c, T3 0x3dd44; literature: https://patents.google.com/patent/US4696044A/en].
Parameter: `gate_sc_hp`, 20..300 Hz, default 100 (20 = off in practice). Could stay internal.
Cost: one TPT state-variable filter on one channel. No latency. Mode 0 unaffected.
Proof: 50 Hz hum at -44 dBFS plus the chug signal, threshold -50: gate closes in the gaps with the filter (gap gain <= -30 dB) and stays open without it. Not yet measured.

### R3. Gate-state meter for the UI
Build: publish the current gain reduction in dB and the state (closed, hold, open) through atomics, as the other meters do. References: GR lamp `1 - g`, AT "GateState", SD `getGateMeter` [decompiled: GR 0x29b244, AT 0xaa79d8, SD symbol at 0xe760].
Add the detector level next to the threshold so the knob can be set by eye. Works for mode 0 too (read the curve's last value).
Cost: two atomic stores per block. Proof: UI shows closed within 100 ms of silence and open on the first pick.

### R4. Align the deferred gain with the audio
Build: when the gain is applied after the amp, delay the gain curve by the latency of everything between the gate and the amp output (`namEngine.getLatencySamples()` plus any reported latency of blocks in between) with a small ring buffer.
Evidence: the curve is applied to the same block it was measured on [inferred: `src/PluginProcessor.cpp:1233`]. Today the gate shuts that many samples before the tail it was measured on.
Cost: a ring of a few thousand floats. No added latency. Applies to all modes; with zero latency the output is unchanged, so mode 0 stays bit-identical at matching sample rates.
Proof: with a model that forces resampling, a 1 kHz burst through gate + amp: the gain edge and the audio edge coincide within 1 sample (today they differ by the resampler latency). The size of today's error was not measured.

### R5. Learn threshold
Build: a UI action that captures the largest held peak `pk` of the sidechain for 0.5 s while the player mutes the strings, then sets `gate_threshold` to that peak + 6 dB (clamped to the range).
Evidence: both Guitar Rig units do a 0.5 s capture and add +3 dB (gate) or +12 dB (noise reduction) [decompiled: GR 0x29b3d0, 0x29c28c]. Our margin sits between them and equals H, so the close threshold lands on the measured noise peak [inferred].
Parameter: none (a transient UI command; the result is an ordinary parameter change). Cost: negligible.
Proof: with -60 dBFS RMS noise at the input the learned threshold lands 6 +- 1 dB above the noise peak and the gate stays closed for 10 s.
Better default: the -80 dB default cannot gate real pickup noise. A default near -60 dB for new instances would be closer to AmpliTube's -50 dB [decompiled: AT 0x1793104], but the right value is a guess without recordings of real noise floors [guess]. Learn removes the need to guess.

### R6. Level-tracking low-pass on the decay (optional, later)
Build: in Smooth mode, a one-pole low-pass on the post-amp signal whose cutoff slides from about 1 kHz to open as the detector level rises through the last 20 to 40 dB above the floor, fast up, slow down (0.3 to 0.5 s).
Evidence: GR hiss filter (1 kHz + 9.5 kHz per 20 dB, 0.45 s release), MXR model (22 dB shelf over 11 dB), Hush model (two-pole sliding low-pass) [decompiled: GR 0x29c628, 0x29b4b8; AT 0x317c8a8, 0xef178c].
Parameter: `gate_hiss`, 0..1, default 0. Cost: one one-pole per channel plus a coefficient update. Risk: dulls quiet picking; keep off by default.
Proof: with amp hiss at -50 dBFS RMS behind a decaying note, noise above 4 kHz is at least 10 dB lower when the note is 10 dB above threshold, and a -20 dBFS note loses less than 0.5 dB at 5 kHz.

Not recommended: lookahead. No reference uses it, and 1 ms opening loses 0.5 dB over the first 10 ms of a note in the prototype [measured: evidence/blackbox_results.txt res_proto_tight.txt].

## 8. Open questions and unverified items

- Guitar Rig 6 and AmpliTube 5 gates were not rendered (no GUI allowed); their timing figures come from code only.
- The Guitar Rig dB table at 0x47db080 is filled at run time; its contents are inferred from the index scaling.
- Guitar Rig gate learn: the getter and setter of the threshold are not inverses (0x29b1ac vs 0x29b09c). Whether the UI writes the value back, which would raise the learned threshold, was not checked.
- AmpliTube Noise Gate: the pairing of declared ranges with parameter names is by order; the names are inline strings I did not decode. Which of them the stomp UI exposes is unknown.
- AmpliTube Hush model: sidechain filter corners and the rectifier network were not decoded; the 1:2 net slope is inferred from the two gain laws.
- Solid Dynamics: the Range formula uses a second parameter (`q`) I did not identify; expander floor measured about 34 dB, gate floor 47 dB.
- bx_console: black box only. The slow tail after -40 dB and the lag on slow ramps (closes at -61.5 dBFS on a 10 dB/s decay) are not explained.
- bx_rockrack V3 Player and TONEX: not measured.
- TONE3000 timing is from a model of the decompiled constants, not from the plugin; the threshold parameter's range and default were not read.
- R2, R4, R5 and R6 acceptance numbers are targets; only R1 has prototype measurements.
- The size of the trigger-to-gain misalignment in tubamp today (R4) is inferred from code, not measured.
- ISP Decimator's adaptive release is known to me only from retailer summaries; no primary source opened.
- The prototype is Python and mono. It uses the proposed constants but not tubamp's block structure.
