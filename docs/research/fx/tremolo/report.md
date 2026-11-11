# Tremolo: reference study and improvement spec

## Verification status

Independently verified on 2026-11-11: 19 of 20 claims verified, 1 not. The full table is in `verification.md`.
Read these corrections before relying on the numbers below. The body of the report is unchanged.

- T20: the pulse splatter figures (-41.9 dBc unsmoothed, -64.8 dBc smoothed) only hold with the carrier at a zero crossing on every edge. Worst case is -30.2 and -59.1 dBc, which misses the -60 dBc limit in R1. The 2.9 ms edge time and a 23 to 29 dB improvement hold.
- R3: summed power is constant only at depth 1 and 180 degrees. At lower depths the ripple is up to 0.63 dB, and the square-root law lifts the smoothed floor to -38.7 dB.
- Stated as fact but inferred: that Guitar Rig's AC30 modulates output-valve bias (the code shows an offset added before a table lookup).
- The "default patch" tremolo numbers assume tremolo is selected. The default mod type is chorus.
- The (1 - depth)^2 law and the 120 Hz smoothing corner are engineering choices with no reference behind them.

Evidence tags: `[decompiled: binary symbol-or-address]`, `[measured: file]`, `[literature: url]`, `[source: file:line]` for tubamp's own code, `[inferred]`, `[guess]`. Evidence files live in `evidence/`. GR6 = Guitar Rig 6, AT5 = AmpliTube 5. Addresses are arm64 vm addresses.

## 1. Summary

- tubamp's tremolo is a sine gain LFO whose dry/wet "mix" adds two copies of the same signal: the default patch gives 1.6 dB of tremolo on top of a 2.9 dB level boost `[measured: evidence/prototype_tables.txt]`.
- No reference has a mix knob. They shape the LFO instead: a pulse through a two-rate one-pole (GR6), lopsided optical curves (GR6 and AT5 Fender models), sine/triangle/square with skew (AT5 Shape Shifter, TONEX) `[decompiled]` `[measured]`.
- Amp-style tremolo depends on level: GR6's AC30 adds the LFO to the output valves' bias and its depth falls from 3.8 dB to about 1.2 dB when driven `[measured: evidence/gr6_acbox_measure.tsv]`.
- Build, in order: a clean shaped voice with a real depth law and smoothing, an optical voice, stereo spread, tempo sync and faster rates, a level-dependent bias voice, a harmonic voice. The old algorithm stays as voice 0.
- GR6 and TONEX were measured headless. AT5 crashes on load in the host, so it is read from disassembly only.

## 2. tubamp today

Class `Modulation`, type `tremolo` `[source: src/dsp/FxBlocks.h:111-130]`, processing at `[source: src/dsp/FxBlocks.cpp:411-450]`.

- **Waveform.** One sine: `lfo = 0.5 + 0.5*sin(2*pi*phase)`, phase 0..1 `[source: src/dsp/FxBlocks.cpp:434]`. No other shape.
- **Depth law.** `modGain = 1 - depth*(1 - lfo)`, linear in gain, between `1 - depth` and 1 `[source: src/dsp/FxBlocks.cpp:435]`.
- **Mix.** `out = x*sqrt(1-mix) + x*modGain*sqrt(mix)` `[source: src/dsp/FxBlocks.cpp:426-427,440]`. Dry and wet are the same signal with no delay, so they add in amplitude, not in power. The total gain is `sqrt(1-mix) + sqrt(mix)*modGain`.
  - At the defaults (depth 0.4, mix 0.35 `[source: src/Parameters.cpp:103-104]`) the gain runs from 1.161 to 1.398: 1.61 dB of tremolo and a +2.91 dB peak `[measured: evidence/prototype_tables.txt, section legacy]`.
  - At mix 0.5 and depth 1 the tremolo is 6.0 dB deep with a +3.0 dB peak. Only mix 1 lets full depth reach silence `[measured: evidence/prototype_tables.txt, section legacy]`.
- **Rate.** `mod_rate`, 0.05 to 10 Hz, log skew centred on 0.707 Hz, default 1 Hz `[source: src/Parameters.cpp:18-23,102]`. Shared with chorus and phaser. No tempo sync.
- **Stereo.** The same gain is applied to every channel `[source: src/dsp/FxBlocks.cpp:437-441]`. No pan mode.
- **Smoothing.** None. Rate, depth and mix are read once per block and applied at once `[source: src/PluginProcessor.cpp:1412-1415]`. A depth or mix change steps the gain at a block boundary. The phase accumulator makes rate changes phase-continuous `[source: src/dsp/FxBlocks.cpp:443-446]`.
- **Level.** Pure gain, so the effect does not depend on signal level `[inferred]`.
- **State.** The phase resets to 0 when the type changes `[source: src/dsp/FxBlocks.cpp:385-393]`.

## 3. Reference implementations

### 3.1 Guitar Rig 6 "Tremolo" (`NI::DSP::RigTremolo`)

Signal flow per sample `[decompiled: Guitar Rig 6 NI::DSP::RigTremolo::processBuffer 0x2e6580]`, annotated excerpt in `evidence/gr6_rigtremolo_excerpt.txt`:

```
phase += 2*f/sr                         (wraps at 1 to -1)
target = (phase + offset - threshold < 0) ? floor : 1.0        floor = 1 - Intensity (20 ms ramp)
coef   = (target == floor) ? down : up
state += coef*(target - state) + 1e-12                         one-pole, two rates
gainR  = state ;  gainL = Pan ? (floor + 1 - state) : state
out    = in * gain
```

- **LFO.** A pulse, not a sine. Its shape comes from the two slew rates `[decompiled: Guitar Rig 6 0x2e65d0-0x2e65fc]`.
- **Edges.** Each slew coefficient is `min(f * 2^((60 - 95*knob)/12), 150 Hz) * 2*pi/sr`, one for "Up" and one for "Down" `[decompiled: Guitar Rig 6 NI::DSP::RigTremolo::setParamUp 0x2e61cc, onSetSampleRate 0x2e5fd0-0x2e5fec]`. The corner tracks the LFO rate, from 32 times the rate (knob 0) to 0.13 times (knob 1), and never exceeds 150 Hz.
- **Width.** The threshold is `1.6*Width - 0.8` on a phase of -1..1 `[decompiled: Guitar Rig 6 NI::DSP::RigTremolo::setParamWidth 0x2e61a4]`. Measured duty of the loud half is `0.9 - 0.8*Width` `[measured: evidence/gr6_tremolo_measure.tsv, section D]`.
- **Depth.** Linear gain. The floor is `1 - Intensity` and Intensity 1 gives silence `[decompiled: Guitar Rig 6 NI::DSP::RigTremolo::setParamIntens 0x2e6170]` `[measured: evidence/gr6_tremolo_measure.tsv, section B]`. No make-up gain.
- **Rate.** 0.2555 to 16.3516 Hz, exponential in the knob (six octaves) `[decompiled: Guitar Rig 6 NI::DSP::RigTremolo::onInit 0x2e5f04-0x2e5f40, NI::DSP::SyncHelper::calcFreqFromInput 0x60bdc4]`. Measured 0.4297 Hz at knob 0.125 and 16.354 Hz at knob 1 `[measured: evidence/gr6_tremolo_measure.tsv, section A]`.
- **Tempo sync.** A sync switch routes the rate through `SyncHelper` with a note-value range and a host-position phase lock `[decompiled: Guitar Rig 6 NI::DSP::RigTremolo::setParamTempoSync 0x2e6354, onPositionChanged 0x2e6168]`.
- **Stereo.** "Pan" mirrors the left gain around the mid point, `gainL = floor + 1 - gainR` `[decompiled: Guitar Rig 6 0x2e6604-0x2e6614]`. The two gains sum to a constant, the summed power does not `[measured: evidence/gr6_tremolo_measure.tsv, section G]`.
- **Smoothing.** Intensity and the bypass crossfade ramp linearly over 20 ms `[decompiled: Guitar Rig 6 NI::DSP::RigTremolo::onSetSampleRate 0x2e5f7c-0x2e5fbc]`.
- **Level.** None. The envelope is identical at -6, -18 and -36 dBFS `[measured: evidence/gr6_tremolo_measure.tsv, section F]`.
- **Parameters.** Sync, Width, Rate, Pan, Intensity, Down, Up `[decompiled: Guitar Rig 6 NI::RIG::COMP::TremoloAU::registerParameters 0x19f35c]`. There is no mix.

### 3.2 Guitar Rig 6 "Twang Reverb" vibrato (`NI::DSP::RigFender`, blackface optical)

- **LFO.** A phase 0..1 with a "lit" flag while phase > 0.8, so the lamp is on for 20 % of the cycle `[decompiled: Guitar Rig 6 NI::DSP::RigFender::processBuffer 0x2c79ec-0x2c7a1c]`.
- **Photocell.** Target gain `1 - depth*lit` goes through a one-pole whose coefficient is switched by the flag: 29.14 Hz while lit, 10.30 Hz while dark `[decompiled: Guitar Rig 6 0x2c7a20-0x2c7a48, NI::DSP::RigFender::onSetSampleRate 0x2c67c0-0x2c67f4]`. Measured: the gain falls in 12.06 ms and recovers in 33.9 ms (10-90 %), loud for 75 % of the cycle `[measured: evidence/gr6_twang_measure.tsv]`.
- **Depth.** `depth = 0.88 * Intensity`, so the floor at full intensity is 0.12 (-18.4 dB) `[decompiled: Guitar Rig 6 NI::DSP::RigFender::setParamVibIntens 0x2c5ed0]`. Measured floor 0.117 `[measured: evidence/gr6_twang_measure.tsv]`.
- **Rate.** 3.86 to 12.25 Hz, exponential `[decompiled: Guitar Rig 6 NI::DSP::RigFender::setParamVibSpeed 0x2c5f2c, setVibratoRate 0x2c6c7c]`. Measured 3.863 to 12.248 Hz `[measured: evidence/gr6_twang_measure.tsv]`.
- **Placement.** The smoothed gain multiplies the signal between two stage groups inside the 4x oversampled amp loop `[decompiled: Guitar Rig 6 0x2c7a4c-0x2c7a54]`. That is before the power stage `[inferred]`.
- **Level.** The tremolo itself is a pure gain, but the stages after it compress. Depth at full intensity is 17.9 dB at -30 dBFS input and 11.2 dB at -6 dBFS `[measured: evidence/gr6_twang_measure.tsv]`.
- With the vibrato on, the crest of the gain sits at 0.915 of the vibrato-off level at low input `[measured: evidence/gr6_twang_measure.tsv]`. The one-pole model predicts 0.9995 `[measured: evidence/prototype_tables.txt, section refs]`. The cause was not traced.

### 3.3 Guitar Rig 6 "AC Box" tremolo (`NI::DSP::RigAC30`, bias)

- **LFO.** A parabolic sine: `p += 4*f/sr`, and when `p > 1` then `p -= 2` and the amplitude changes sign, `lfo = A*(1 - p^2)` `[decompiled: Guitar Rig 6 NI::DSP::RigAC30::processBuffer 0x2bce0c-0x2bce2c, 0x2bc94c-0x2bc970]`.
- **Application.** The LFO is added to two lanes of the signal vector before an interpolated valve-table lookup, and the two lanes are subtracted afterwards `[decompiled: Guitar Rig 6 0x2bc974-0x2bc980, 0x2bca08-0x2bca58]`. It is an offset ahead of a nonlinearity, not a gain. The two lanes are the push-pull output valves `[inferred]`.
- **Depth.** Amplitude `A = Depth * 50` in table units, or `* 10` in a GR5-compatibility mode `[decompiled: Guitar Rig 6 NI::DSP::RigAC30::setParamTremoloDepth 0x2bb3c4]`.
- **Rate.** `2 + 5*Speed` Hz, linear `[decompiled: Guitar Rig 6 NI::DSP::RigAC30::setParamTremoloSpeed 0x2bb234]`. Measured 2.001 to 7.003 Hz `[measured: evidence/gr6_acbox_measure.tsv]`.
- **Measured behaviour** `[measured: evidence/gr6_acbox_measure.tsv]`:
  - The gain swings above and below the no-tremolo level (1.106 and 0.883 at full depth, clean preset, -18 dBFS).
  - Clean preset: 1.85 to 1.95 dB deep from -42 to -18 dBFS, 1.57 dB at -6 dBFS, 0.65 dB at 0 dBFS.
  - Crunch preset: 3.79 dB at -30 dBFS, 1.08 to 1.34 dB from -18 to 0 dBFS. When driven the gain curve turns lopsided (rise 46 ms, fall 117 ms).
  - LFO feed-through at 4.5 Hz is -96.6 dBFS at -18 dBFS input and -46.1 dBFS at -6 dBFS input.
- Full depth never reaches silence in any measured setting `[measured: evidence/gr6_acbox_measure.tsv]`.

### 3.4 Guitar Rig 6 "Rotator"

Only the literal constants were read. `onSetSampleRate` holds 840, 705, 2350 and 2800 next to a 2*pi constant, and the treble and bass rotors have separate slow, fast and acceleration setters `[decompiled: Guitar Rig 6 NI::DSP::Rotator::onSetSampleRate 0x2f1998, setParamTrblFast 0x2f1d94, setParamBassFast 0x2f1eac]`. A two-band split with independently modulated bands is the same idea as harmonic tremolo `[inferred]`. The process function was not traced.

### 3.5 AmpliTube 5 "Opto Tremolo" stomp and Fender Tremolo

Both are a table-lookup LFO and a linear depth crossfade, `out = in*(1 - depth + depth*lfo)` `[decompiled: AmpliTube 5 IK::MAF::DynamicWrapper<IK::KIS::FX::AT3::Tremolo::Module,0>::ProcessBuffer 0x14b30b8-0x14b30d0]` `[decompiled: AmpliTube 5 IK::KIS::FX::AT3::FenderTremolo::Module::Process 0xcdc7e4-0xcdc7fc]`.

| | Tremolo stomp | Fender Tremolo |
|---|---|---|
| Phase | 30-bit accumulator `[decompiled: AmpliTube 5 0x14b30e0-0x14b30f0]` | sample counter times rate/sr, mapped to -1..1 `[decompiled: AmpliTube 5 0xcdc80c-0xcdc828]` |
| Table | 101 points at 0x305f904 | 256 points at 0x26257c0 |
| Shape | rises fast, creeps to 1.0 by 78 % of the cycle, falls in the last fifth; loud 70 % of the cycle | falls fast to the floor, stays low, recovers by about 45 %, then flat at 1.0; loud 65 % of the cycle |
| Floor | 0.172 (-15.3 dB) | 0.060 (-24.4 dB) |
| Rate | 0.5 to 10 `[decompiled: AmpliTube 5 IK::MAF::AT3::Tremolo::DefineParameters 0x187546c]` | 0.1 to 12 `[decompiled: AmpliTube 5 IK::MAF::AT3::FenderTremolo::DefineParameters 0x1620870]` |
| Sync | bpm and a division table, 1/32 triplet to 4 beats `[decompiled: AmpliTube 5 0x18758c0-0x1875904]` | none found |

Table shapes and floors: `[decompiled: AmpliTube 5 tables 0x305f904 and 0x26257c0, summary in evidence/at5_tables_summary.txt]`.

- Full depth never reaches silence because the tables bottom out above zero `[decompiled: evidence/at5_tables_summary.txt]`.
- Both are mono modules wrapped as dual mono with linked parameters. No stereo mode was found `[decompiled: AmpliTube 5 AudioEngine::ModuleDualMono<IK::MAF::AT3::Tremolo, LinkParameter> symbols]`.
- No smoothing of the gain, no level dependence, no mix `[decompiled: AmpliTube 5 0x14b3028, 0xcdc7a8]`.
- **In the amps.** Four Fender amp models call the same `FenderTremolo::Module::Process` from their pre-amp wrapper `[decompiled: AmpliTube 5 callers 0x15d7c94, 0x15dd4a4, 0x162391c, 0x1629c48]`. In the '65 Deluxe Reverb it is the last call, after the tone stack and the spring reverb `[decompiled: AmpliTube 5 IK::MAF::DynamicWrapperPreAmp<IK::KIS::FX::AT3::Fender65DeluxeReverb::Amp,1>::ProcessBuffer 0x15d7244, 0x15d76dc, 0x15d7c94]`. So it sits before the power amp module, as a plain gain.

### 3.6 AmpliTube 5 "Shape Shifter" (`SeymourDuncanShapeShifter`)

The most complete tremolo in the set `[decompiled: AmpliTube 5 IK::KIS::FX::AT3::SeymourDuncanShapeShifter::Module::Process 0xdb1a64]`, excerpt in `evidence/at5_tremolo_excerpt.txt`:

```
x    = n / period
w    = x < s ? 0.5*x/s : 1 + 0.5*(x-1)/(1-s)        skew, s = 0.01 + 0.98*Shape
lfo  = (1-m)*triangle(w) + m*(sine(w) or square(w)) Wave: 0 sine, 0.5 triangle, 1 square
u    = (0.957 + 0.043*d) - d*lfo
gain = onepole_100Hz( gainlaw(u) )                  gainlaw: straight line in dB, -33.3 dB at u=0
out  = in * gain * 10^(4*d/20)
```

- **Shapes.** Three tables: a raised cosine, a 3-point triangle, a square that is high for half the cycle `[decompiled: evidence/at5_tables_summary.txt]`. The Wave knob crossfades triangle against one of the other two `[decompiled: AmpliTube 5 IK::MAF::AT3::SeymourDuncanShapeShifter::OnSetParameter 0x1826d10-0x1826d54]`.
- **Skew.** A piecewise-linear phase warp moves the half-cycle point to `s` `[decompiled: AmpliTube 5 0xdb1bc0-0xdb1be4]`. With the square it becomes the duty cycle `[inferred]`.
- **Depth law.** dB-linear through a 157-point table, slope 33.9 dB per unit, floor -33.3 dB `[decompiled: evidence/at5_tables_summary.txt]`. Make-up `+4 dB * depth` `[decompiled: AmpliTube 5 0x1826b88-0x1826ba4]`.
- **Smoothing.** A one-pole low-pass at 100 Hz on the gain, after the law `[decompiled: AmpliTube 5 0xdb1b78-0xdb1b98, coefficients 0x1487924-0x1487950]`.
- **Rate.** `0.52 + 15.105*knob` Hz, times 1, 2, 3 or 4 in ratio mode, ceiling 15.625 Hz, plus bpm sync `[decompiled: AmpliTube 5 IK::KIS::FX::AT3::SeymourDuncanShapeShifter::Module::UpdateRate 0xdb1e28]`. A rate change rescales the sample counter so the phase does not jump `[decompiled: AmpliTube 5 0xdb1f0c-0xdb1f30]`.
- The hardware it models lists rate 1 to 20 per second, Wave sine to triangle to square, Shape 10 to 90 %, depth 0 to 28 dB, ratios 1:1 to 4:1 `[literature: https://www.modulargrid.net/p/modules/view/14298]`.

### 3.7 AmpliTube 5 Silvertone 1484 and Vox AC30 TB amps

Read by a helper agent; the lines quoted here were rechecked against the binary.

- **Silvertone 1484.** A sine LFO, `g = (1 - d/2) + (d/2)*sin`, is mapped through a 200-point table and the signal is multiplied by `2T - 1` `[decompiled: AmpliTube 5 IK::MAF::DynamicWrapperPreAmp<IK::KIS::FX::AT3::Silvertone1484::Amp,1>::ProcessBuffer 0x1829a54-0x1829a48]`. The table is flat near the top and steep near the bottom: gain 0.896 at g = 1, 0.647 at 0.77, 0.027 at 0.534 `[decompiled: evidence/at5_tables_summary.txt]`. Depth is `d = (0.4661*Strength)^0.4` `[decompiled: AmpliTube 5 IK::MAF::AT3::Silvertone1484::OnSetParameter 0x182b200-0x182b228]`. It is a static curve on the gain, so it does not react to level `[inferred]`.
- **Vox AC30 TB.** `m = 0.5*(1 + sine*k)` with `k = 64/(64 + rate^2)`, `out = 10*in*m` `[decompiled: AmpliTube 5 IK::MAF::DynamicWrapperPreAmp<IK::KIS::FX::AT3::VoxAC30TB::Amp,1>::ProcessBuffer 0x188bd1c-0x188bd48]`. Depth is not a knob. It shrinks as the rate rises `[inferred]`.
- Neither has gain smoothing `[decompiled: AmpliTube 5 0x1829a54-0x1829a48, 0x188bd08-0x188bd48]`.

### 3.8 TONEX 1.13.1 (stripped binary, black-box only)

TONEX exposes its tremolo as AU parameters: TREM RATE, SHAPE, SPREAD, LEVEL, SYNC, TIMESIGN, plus MOD POSITION (pre or post amp) `[measured: evidence/params_listing.txt]`.

- **Rate.** Linear, `0.1 + 9.9*knob` Hz (2.575, 5.05, 7.525, 10.0 Hz at 0.25, 0.5, 0.75, 1) `[measured: evidence/tonex_tremolo_measure.tsv]`.
- **Shape.** 0 is a triangle. 1 is a 50 % square. Between, a crossfade: at 0.5 the cycle is a half-height triangle with a step at the mid crossings `[measured: evidence/tonex_tremolo_measure.tsv, one-cycle table]`.
- **Depth (LEVEL).** Linear: floor `1 - L`, crest `1 + 0.414*L`. At L = 1 the square runs from 0.0000 to 1.4142 `[measured: evidence/tonex_tremolo_measure.tsv]`. So full depth is silence and the crest carries +3 dB of make-up.
- **Edges.** The square's edges take 4.79 ms (10-90 %) `[measured: evidence/tonex_tremolo_measure.tsv, 3 kHz carrier row]`.
- **Stereo.** SPREAD shifts the right channel's LFO by 0 to 180 degrees, linearly (45.0, 90.2, 135.1, 180.0) `[measured: evidence/tonex_tremolo_measure.tsv]`.
- **Level.** None: identical at -36, -18 and -6 dBFS `[measured: evidence/tonex_tremolo_measure.tsv]`.

### 3.9 Not studied

- Tone King Imperial MKII: no arm64 slice and PACE-protected, skipped as briefed.
- AT5 ZVex Seek Trem (a step sequencer) and X-VIBE tremolo: located by symbol (`IK::KIS::FX::AT3::ZVexSeekTrem::Module::Process 0xdd2484`, `IK::KIS::FX::XVibe::Tremolo`), not read.
- No installed plugin implements harmonic tremolo. That voice rests on literature only.

## 4. Measurements

All renders are 48 kHz. Each evidence file starts with the commands that produced it. `evidence/tools/measure_gr6.sh` and `evidence/tools/measure_tonex.sh` regenerate everything. Gain is the demodulated envelope of a steady sine divided by its level without the effect; rise and fall are 10-90 % of the swing.

**GR6 Tremolo: rate and edges** `[measured: evidence/gr6_tremolo_measure.tsv]`

| Rate knob | Hz measured | Hz predicted `0.2555*64^knob` | Rise at Up=0 (ms) | Predicted from `min(32 f, 150 Hz)` |
|---|---|---|---|---|
| 0.125 | 0.4297 | 0.4297 | 25.42 | 25.4 |
| 0.5 | 2.0439 | 2.0440 | 5.33 | 5.35 |
| 0.75 | 5.7810 | 5.7810 | 2.38 | 2.33 |
| 1 | 16.354 | 16.352 | 2.38 | 2.33 |

**GR6 Tremolo: depth, width, level, stereo** `[measured: evidence/gr6_tremolo_measure.tsv]`

| Setting | Result |
|---|---|
| Intensity 0.25 / 0.5 / 0.75 / 0.9 / 1 | floor 0.7500 / 0.5000 / 0.2500 / 0.1000 / 0.0000 |
| Width 0 / 0.25 / 0.5 / 0.75 / 1 | loud for 0.900 / 0.700 / 0.500 / 0.300 / 0.100 of the cycle |
| Up 0.25 / 0.5 / 0.75 (rate 2.04 Hz) | rise 21.06 / 82.58 / 179.75 ms; at 0.75 the crest only reaches 0.805 |
| Input -6 / -18 / -36 dBFS | identical: crest 0.9988, floor 0.2512 |
| Pan on | envelope correlation -1.0000, gL + gR = 1.2500 constant, gL^2 + gR^2 from 0.781 to 1.061 |

**GR6 amp tremolos** `[measured: evidence/gr6_twang_measure.tsv]` `[measured: evidence/gr6_acbox_measure.tsv]`

| Amp, setting | Input | Crest | Floor | Depth dB | Rise ms | Fall ms |
|---|---|---|---|---|---|---|
| Twang, Intensity 1 | -30 dBFS | 0.915 | 0.117 | 17.88 | 33.88 | 12.06 |
| Twang, Intensity 1 | -18 dBFS | 0.929 | 0.123 | 17.60 | 33.29 | 12.21 |
| Twang, Intensity 1 | -6 dBFS | 1.082 | 0.299 | 11.16 | 25.61 | 28.81 |
| AC Box clean, Depth 1 | -30 dBFS | 1.103 | 0.890 | 1.86 | 62.85 | 62.83 |
| AC Box clean, Depth 1 | -6 dBFS | 1.074 | 0.897 | 1.57 | 64.48 | 72.12 |
| AC Box crunch, Depth 1 | -30 dBFS | 1.179 | 0.762 | 3.79 | 61.00 | 61.50 |
| AC Box crunch, Depth 1 | -6 dBFS | 1.022 | 0.888 | 1.22 | 45.88 | 116.94 |

Rows of the AC Box file with less than 0.5 dB of modulation at 0 dBFS input report a wrong rate or `nan`; the envelope tracker loses the cycle there.

**TONEX** `[measured: evidence/tonex_tremolo_measure.tsv]`

| Setting | Result |
|---|---|
| LEVEL 0.25 / 0.5 / 0.75 / 1 (triangle) | floor 0.757 / 0.513 / 0.270 / 0.026, crest 1.097 / 1.194 / 1.291 / 1.388 |
| SHAPE 0 / 0.5 / 0.75 / 1 at 5.05 Hz | rise 76.3 / 56.5 / 18.3 / 5.8 ms (440 Hz carrier) |
| SHAPE 1, LEVEL 1, 3 kHz carrier | floor 0.0000, crest 1.4142, edges 4.79 ms |
| SPREAD 0 / 0.5 / 1 | right leads by 0.0 / 90.2 / 180.0 degrees |

**Models against measurement** `[measured: evidence/prototype_tables.txt, section refs]`

| Quantity | Model built from the disassembly | Measured |
|---|---|---|
| GR6 Tremolo INIT preset, rise | 82.58 ms | 82.54 ms |
| GR6 Tremolo Up=0.75, crest | 0.806 | 0.805 |
| GR6 Twang fall / rise at 6.87 Hz | 11.75 / 33.85 ms | 12.06 / 33.88 ms |
| GR6 Twang floor | 0.124 | 0.117 |

## 5. Industry practice

What the references agree on:

- **No dry/wet mix.** GR6, TONEX, both AT5 stomps and the Shape Shifter have rate, depth and shape controls and no mix `[decompiled: Guitar Rig 6 0x19f35c]` `[measured: evidence/params_listing.txt]` `[decompiled: AmpliTube 5 0x14b30b8, 0xdb1a64]`.
- **The shape is the product.** Nobody ships a bare sine as the only choice. The shapes on offer are: pulse with two slew rates (GR6), triangle to square (TONEX), sine to triangle to square with skew (Shape Shifter), and lopsided optical curves (GR6 Twang, both AT5 tables).
- **Optical means asymmetric.** GR6 models it as a 20 % lamp pulse into a 29 Hz / 10 Hz switched one-pole `[decompiled: Guitar Rig 6 0x2c79ec-0x2c7a48]`. AT5 stores the finished curve `[decompiled: evidence/at5_tables_summary.txt]`. Photocell data sheets give 2.5 ms on and 35 ms off (VTL5C3), 5 ms and 80 ms (NSL-32SR2) `[literature: https://www.qsl.net/wa1ion/vactrol/vactrol.pdf]`. A DAFx vactrol model uses the same device, a low-pass whose response time is switched by the direction of change, about 12 ms and 250 ms `[literature: https://dafx.de/paper-archive/2013/papers/44.dafx2013_submission_56.pdf]`. The classic circuit keeps the lamp off, and the signal up, for well over half the cycle `[literature: https://www.strymon.net/amplifier-tremolo-technology-white-paper/]`.
- **Depth law.** Linear gain down to silence (GR6, TONEX), linear with a table floor of -15 to -24 dB (AT5 stomps), or dB-linear down to -33 dB (Shape Shifter). Make-up is 0 dB (GR6, AT5 stomps), +3 dB at the crest (TONEX) or +4 dB (Shape Shifter).
- **Hard edges are smoothed.** 150 Hz ceiling on the slew (GR6), 100 Hz one-pole (Shape Shifter), 4.8 ms edges (TONEX). The sine and table tremolos have none.
- **Rate.** Ranges top out between 10 and 16 Hz: 0.26-16.4 (GR6), 0.1-10 (TONEX), 0.5-10 and 0.1-12 (AT5), 0.52-15.6 with x2/x3/x4 (Shape Shifter). Amp models are narrower: 3.9-12.2 Hz (Twang), 2-7 Hz (AC Box). Real valve LFOs run 1 to 10 Hz, a Fender-value oscillator 4.3 to 8 Hz `[literature: https://valvewizard.co.uk/trem1.html]`. Knob laws differ: exponential (GR6), linear (TONEX, Shape Shifter).
- **Tempo sync** is present in GR6, AT5 and TONEX.
- **Stereo.** Either mirrored gains (GR6) or a 0-180 degree LFO offset (TONEX, and the Shape Shifter hardware `[literature: https://www.premierguitar.com/gear/seymour-duncan-shape-shifter-stereo-tap-tremolo-review]`). Nobody uses an equal-power pan law.
- **Stand-alone tremolos ignore level.** Only tremolo inside an amp model reacts to level, either because it modulates bias (AC Box) or because it sits ahead of stages that compress (Twang).
- **Position in amps.** After the pre-amp and reverb, before the power stage (GR6 Twang, AT5 Fender models). The real blackface circuit injects at the vibrato channel's 220 k mixing resistor and its 50 k intensity pot loads the signal `[literature: https://robrobinette.com/AB763_Modifications.htm]`.

What the literature adds:

- **Bias tremolo.** The LFO moves the bias of the push-pull power valves; low idle current reduces gain, with crossover distortion near the minimum and more valve distortion near the maximum, and supply sag changes the response `[literature: https://www.strymon.net/amplifier-tremolo-technology-white-paper/]`. A hot bias loses tremolo intensity `[literature: https://robrobinette.com/AA1164_Princeton_Reverb.htm]`.
- **Harmonic tremolo.** A low-pass of 220 k with 5 nF (about 144 Hz) and a high-pass of 250 pF with 1 M (about 636 Hz), each a single RC stage at 6 dB per octave, feed two valves whose gains are modulated by LFOs 180 degrees apart `[literature: https://sites.google.com/site/davidmorrinoldsite/home/trouble/trouble-amps/fender-harmonic-vibrato]`. The opposite LFO feed-throughs cancel at the mixer and the filters' phase shifts give a moving notch `[literature: https://www.premierguitar.com/articles/the-59-63-fender-concert-vibrato-1]`.

## 6. Gap analysis

| Aspect | Ours | References | Consequence for the player |
|---|---|---|---|
| Depth and mix | Coherent equal-power mix: default is 1.6 dB deep, +2.9 dB louder | No mix. Depth alone sets the floor | The default tremolo is barely audible and switching it on raises the level. Depth and mix fight each other. |
| Reaching silence | Only at mix 1 | GR6 and TONEX reach 0; others floor at -15 to -33 dB | No chop or stutter at normal settings. |
| Waveform | Sine | Pulse with slew, triangle/square, skew, optical curves | No throb, no helicopter, no lopsided amp feel. |
| Edge smoothing | Not needed for a sine | 100-150 Hz slew, about 5 ms edges | Nothing to fix today; required as soon as a square exists. |
| Parameter smoothing | None, values step per block | 20 ms ramps (GR6) | Zipper steps when depth or mix is automated or dragged. |
| Rate range | 0.05-10 Hz | Up to 16 Hz, x4 ratio, bpm sync | No fast flutter, no tempo-locked tremolo. The bottom two octaves are too slow to be heard as tremolo. |
| Stereo | Same gain on all channels | Mirrored pan, 0-180 degree spread | The stereo chain gets no movement from tremolo. |
| Make-up | None | +3 dB (TONEX), +4 dB (Shape Shifter) | Once the mix boost is gone, deep tremolo will sound quieter than bypass. |
| Level interaction | None | Bias depth falls when driven; pre-power-stage tremolo is compressed | Amp-style tremolo that breathes with picking is not available. |
| Tone | None | Harmonic tremolo in the literature only | No brownface voice. |

## 7. Recommendations

Shared rules for every item:

- **Compatibility.** A new choice parameter `<k>_trem_voice` selects the algorithm, `<k>` being `mod`, `mod2`, `mod3`. Value 0, "Classic", is today's code path, untouched, and is the default, so every saved session and every host that never sets the parameter sounds identical. The UI selects voice 1 when a user newly picks Tremolo. The choice list is frozen once shipped.
- **Existing parameters.** New voices read `<k>_rate` and `<k>_depth` and ignore `<k>_mix`; the UI hides the mix knob for them. Evidence: no reference has a mix (section 5).
- **Depth law of the new voices** (ours): `floor = (1 - depth)^2`, `gain = (floor + (1 - floor)*w) * 10^(3*depth/20)`, `w` being the LFO shape with 1 = loud. Depth 0.5 gives -12 dB, 0.75 gives -24 dB, 1 gives silence, and the crest rises to +3 dB at full depth `[measured: evidence/prototype_tables.txt, section depth]`. It sits between the linear law of GR6 and TONEX, where half the knob is only 6 dB, and the dB law of the Shape Shifter. The +3 dB crest matches TONEX's measured crest of 1.4142.
- **Smoothing** (ours): depth is ramped over 20 ms; every voice's gain passes a 120 Hz one-pole (the Optical voice's own filter already does that job). Evidence: 20 ms ramps and a 150 Hz ceiling in GR6, 100 Hz in the Shape Shifter, 4.8 ms edges in TONEX.
- **Cost.** Every voice is a handful of multiplies per sample and channel. Latency 0.
- **Verification** uses the method of section 4 on tubamp's own AU: `auhost render` of a steady sine, then `evidence/tools/tremenv.py`.

Reference code for every shape: `prototype/tremolo_proto.py`, tables in `evidence/prototype_tables.txt`.

### R1. "Smooth" voice: shaped LFO, real depth, smoothing (highest value, smallest cost)

- **Build.** Voice 1. Sine, triangle and pulse from one control, with skew, the depth law above, and the two smoothers.
- **Evidence.** The mix defect of section 2. Crossfaded wave shapes and skew: Shape Shifter (section 3.6) and TONEX (section 3.8). Pulse with duty and smoothed edges: GR6 (section 3.1).
- **Design.**

```
s     = 0.1 + 0.8*trem_shape                                    rise/fall split, and the pulse's duty
w0    = ph < s ? 0.5*ph/s : 0.5 + 0.5*(ph - s)/(1 - s)          crest sits at ph = s
sine  = 0.5 - 0.5*cos(2*pi*w0) ;  tri = 1 - |2*w0 - 1|
pulse = frac(ph - s/2) < s                                      centred on the crest
w     = wave <= 0.5 ? lerp(sine, tri, 2*wave) : lerp(tri, pulse, 2*wave - 1)
gain  = onepole_120Hz( (floor + (1 - floor)*w) * makeup )
```

- **New parameters.**

| id | range | default | meaning |
|---|---|---|---|
| `<k>_trem_voice` | choice: Classic, Smooth, Optical, Bias, Harmonic | 0 | algorithm |
| `<k>_trem_wave` | 0..1 | 0 | 0 sine, 0.5 triangle, 1 pulse (Smooth only) |
| `<k>_trem_shape` | 0..1 | 0.5 | Smooth: rise/fall split and pulse duty. Bias: knee. Harmonic: crossover. |

- **Proof.** 1 kHz sine at -18 dBFS, 4 Hz.
  - Classic voice: null against the current build below -140 dBFS at four settings.
  - Depth 1, wave 0: crest +3.0 dB +/- 0.2, floor below -60 dB, loud half 0.50 +/- 0.01.
  - Depth 0.5: floor -12.0 dB +/- 0.5.
  - Wave 1: edges 2.9 ms +/- 0.5 (10-90 %), loud fraction equal to `s` +/- 0.01, spectral splatter 3-6 kHz from the carrier below -60 dBc. The prototype gives -64.8 dBc against -41.9 dBc unsmoothed `[measured: evidence/prototype_tables.txt, section chop]`.
  - Depth stepped 0 to 1 between blocks: no sample-to-sample gain step above 0.005. The prototype gives 0.0014 with the ramp and 0.015 without it `[measured: evidence/prototype_tables.txt, section chop]`.

### R2. "Optical" voice

- **Build.** Voice 2: a lamp pulse into a two-rate one-pole.
- **Evidence.** GR6 Twang (decompiled and measured, section 3.2), both AT5 tables (section 3.5), photocell data sheets and the DAFx vactrol model (section 5).
- **Design (our constants).**

```
lit    = phase >= 0.75                                  lamp on for a quarter of the cycle
target = 1 - 0.9*depth*lit                              floor -20 dB at full depth
gain  += (lit ? a_lit : a_dark) * (target - gain)       tau_lit 5 ms, tau_dark 20 ms
```

  - 5 ms and 20 ms sit inside the spread of the sources: 2.5-6 ms on and 35-80 ms off for the bare parts `[literature: https://www.qsl.net/wa1ion/vactrol/vactrol.pdf]`, 5.5 ms and 15.5 ms equivalent in GR6. The gain recovers sooner than the cell's resistance does, because the shunt stops mattering once the cell is well above the source impedance `[inferred]`.
  - Floor -20 dB lies between GR6 (-18.4 dB), the AT5 stomp (-15.3 dB) and the AT5 Fender table (-24.4 dB).
  - The prototype gives a 10.98 ms fall, a 43.8 ms recovery and 70 % of the cycle loud at 5 Hz `[measured: evidence/prototype_tables.txt, section optical]`.
- **Parameters.** None beyond R1. Wave and shape are ignored.
- **Proof.** At 5 Hz, depth 1: fall 11 ms +/- 1.5, rise 44 ms +/- 5, loud fraction 0.70 +/- 0.03, floor -20 dB +/- 1. Envelope identical within 0.1 dB between -36 and -6 dBFS.

### R3. Stereo spread

- **Build.** The right channel's LFO phase leads by `trem_spread` degrees. Applies to voices 1 to 4.
- **Evidence.** TONEX SPREAD measured 0-180 degrees (section 3.8). GR6 Pan (section 3.1), whose summed power wobbles from 0.781 to 1.061.
- **Design (ours).** `phR = frac(ph + spread/360)`. To keep loudness steady when the channels oppose each other, raise the shape to a power that slides from 1 to 0.5: `w' = w^(1 - spread/360)`. With a sine at 180 degrees the two channel powers then sum to a constant, where the linear law ripples by 3 dB `[measured: evidence/prototype_tables.txt, section stereo]`.
- **Parameter.** `<k>_trem_spread`, 0..180 degrees, default 0.
- **Proof.** Stereo sine, depth 1, sine wave. Spread 0: left and right null. Spread 180: envelope correlation below -0.9 and summed power ripple below 0.2 dB. Spread 90: right leads by 90 degrees +/- 2.

### R4. Tempo sync and faster rates

- **Build.** A sync division and a rate multiplier.
- **Evidence.** Sync in GR6, AT5 and TONEX; rates to 16 Hz; the x1 to x4 ratio of the Shape Shifter (sections 3.1, 3.5, 3.6, 3.8).
- **Design.** `rate = sync ? bpm/60 / beats(division) : mod_rate * mult`, clamped to 0.05..20 Hz. While the host plays, the phase is slaved to the song position so the dip lands on the grid; GR6 does this in `LFOHelper::onPositionChanged` `[decompiled: Guitar Rig 6 0x60d2c8]`. When the rate changes, keep the phase (already true of the accumulator).
- **Parameters.**

| id | range | default |
|---|---|---|
| `<k>_trem_sync` | choice: Off, 1/1, 1/2, 1/4., 1/4, 1/4T, 1/8., 1/8, 1/8T, 1/16., 1/16, 1/16T, 1/32 | Off |
| `<k>_trem_mult` | choice: x1, x2, x3, x4 | x1 |

- **Proof.** 120 bpm, 1/8: 4.000 Hz +/- 0.1 %, gain minimum within 2 ms of each eighth. Rate 10 Hz, x2: 20.0 Hz +/- 0.1 %.

### R5. "Bias" voice: depth that follows the playing

- **Build.** Voice 3: a sine tremolo whose depth shrinks as the note gets louder.
- **Evidence.** AC Box: 3.8 dB when quiet, about 1.2 dB when driven (section 3.3). Bias tremolo depends on idle current `[literature: https://robrobinette.com/AA1164_Princeton_Reverb.htm]`.
- **Design (ours).** Take a push-pull pair of three-halves-law devices `[literature: https://en.wikipedia.org/wiki/Space_charge]`, `i = max(0, v + b)^1.5` per side. Its small-signal gain goes with `sqrt(b)`; once the signal is far above the bias, the gain stops depending on `b`. A closed form fitted to the numerically computed pair `[measured: evidence/prototype_tables.txt, section bias]`:

```
u    = 0.5 + 0.5*cos(2*pi*ph)                    1 = dip
env  = peak follower of the block's mono sum     attack 5 ms, release 150 ms
q    = env / knee                                knee = -24..0 dBFS from trem_shape, default -12 dBFS
gain = sqrt( (1 - depth*u + q) / (1 + q) )
```

  - Depth 0.75 gives 5.6 dB at -40 dBFS, 3.0 dB at -18 dBFS, 1.25 dB at -6 dBFS `[measured: evidence/prototype_tables.txt, section bias]`.
  - It is applied as a clean gain. tubamp's amp block supplies the distortion, so the voice adds no aliasing.
  - The fit is within 0.2 dB of the exact pair at and above the knee, and up to 2.1 dB shallow below it, where the exact pair's crossover region deepens the tremolo `[measured: evidence/prototype_tables.txt, section bias]`.
- **Free alternative.** Placing any tremolo voice before the amp block makes the amp compress it, as measured on Twang (17.9 dB falling to 11.2 dB). Say so in the UI hint.
- **Proof.** 440 Hz sine, depth 0.75, default knee: depth 5.5 dB +/- 0.5 at -40 dBFS and 1.25 dB +/- 0.4 at -6 dBFS.

### R6. "Harmonic" voice

- **Build.** Voice 4: two bands, ducked in antiphase.
- **Evidence.** Literature only: the brownface circuit values and its antiphase LFOs (section 5).
- **Design (ours).** The circuit's own filters (144 Hz low-pass, 636 Hz high-pass) do not sum back to the input: run in antiphase they sit 14.7 dB down at 300 Hz at mid-cycle and their broadband level moves between -3.0 and -9.7 dB `[measured: evidence/prototype_tables.txt, section harmonic]`. A complementary split keeps depth 0 transparent:

```
L = onepole_lowpass(x, fc) ;  H = x - L          fc = 300 Hz default (geometric mean of 144 and 636), 150..600 Hz from trem_shape
u = 0.5 - 0.5*cos(2*pi*ph)
out = sqrt(2/(2 - depth)) * ( sqrt(1 - depth*u)*L + sqrt(1 - depth*(1-u))*H )
```

  - L and H of a first-order split are 90 degrees apart, so their powers add; the power-law gains keep the sum steady for a spectrum balanced across the split `[inferred]`.
  - At depth 1 the prototype swings 11.3 dB at 82 Hz against 16.5 dB the other way at 2 kHz, with a broadband ripple of 3.1 dB `[measured: evidence/prototype_tables.txt, section harmonic]`.
  - The moving notch of the real circuit is not reproduced. It comes from the non-complementary filters.
- **Cost.** One extra one-pole per channel.
- **Proof.** Depth 0: null against dry below -100 dB. Depth 1: 82 Hz and 2 kHz sines each swing at least 10 dB with envelope correlation below -0.9; pink noise RMS ripple below 3.5 dB.

### Considered and not recommended

- **Keeping a mix for the new voices.** Dry and wet are coherent, so a mix is a second, worse depth knob.
- **Sampled LFO tables** like AT5's. They are vendor data, and the two-rate one-pole reproduces the same family from three numbers.
- **Oversampling.** The gain signal is band-limited to 120 Hz; its sidebands stay next to the carrier.
- **A waveshaping bias stage.** It would alias and would double up on the amp block.

## 8. Open questions and unverified items

- AmpliTube 5 has no measurement. It dies in `IK::Products::AT5::SharedPluginData::SharedPluginData()` when the host instantiates it, with or without a window `[measured: evidence/at5_blackbox_gap.txt]`. Its tremolo behaviour is from disassembly and table read-outs.
- The order of the three float arguments of `AudioEngine::Parameter` (taken as min, max, default) was not confirmed, so AT5's quoted rate ranges are `[inferred]`.
- GR6 Twang: the 0.915 crest with the vibrato on, and the slow tail of its recovery, are not explained by the one-pole pair. `setParamVibratoOn` changes a coefficient at `[0x288]` from a 2.6 Hz corner to 196 Hz when the vibrato is switched on `[decompiled: Guitar Rig 6 NI::DSP::RigFender::setParamVibratoOn 0x2c5f44]`; its role was not traced.
- GR6 AC Box: the units of the valve table and which stage the two lanes belong to are inferred, not traced. The GR5-mode flag's default was not read.
- GR6 tempo sync: the note-value enum behind `setNoteRange(13, 31)` was not decoded. Sync was not measured in any plugin.
- AT5 Silvertone 1484 speed range (4 to 10 Hz, squared knob) comes from the helper's read and was not rechecked.
- GR6 Rotator, AT5 ZVex Seek Trem and X-VIBE tremolo were not read.
- TONEX's edge smoothing type (one-pole or ramp) is not known; only the 4.79 ms edge time is.
- No primary source was found for the blackface speed range, the Fender photocell's own time constants, or a perceptual study of tremolo depth laws. The `(1 - depth)^2` law and the 120 Hz slew are engineering choices bracketed by the references.
- Parameter ids proposed here are tremolo-only. If the chorus and phaser specs also want sync or stereo spread, one shared pair of ids would be better than three; that needs one decision across the three reports.
- Harmonic tremolo has no installed reference; R6 should be judged by ear against recordings before it ships.
