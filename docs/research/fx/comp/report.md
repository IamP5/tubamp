# Compressor: reference study and improvement spec

## Verification status

Independently verified on 2026-11-11: 23 of 24 claims verified, 1 not. The full table is in `verification.md`.
Read these corrections before relying on the numbers below. The body of the report is unchanged.

- The claims about tubamp's own compressor were confirmed on the installed Audio Unit with the compressor in the chain: attack 2.21 ms, release 13.67 ms, THD at 100 Hz 1.99 % at defaults and 12.38 % at the fastest settings.
- R1: the Clean prototype's 0.000 % THD at 100 Hz only holds because 100 Hz is sample-locked at 48 kHz. With the 8 ms hold the residual is 0.21 % at 98 Hz, 0.56 % at 82.4 Hz, 0.83 % at 70 Hz and 0.14 % at 41.2 Hz (without the hold: 0.41, 0.48, 0.57, 0.96 %). It is inharmonic sidebands near -50 dBc, which the THD tool does not count. The detector hold is not supported as specified.
- C14: the 1176 release after a 0.1 s burst is 56 ms, not 43 ms. An analysis-script bug affects every UADx short-burst (tbF) release figure. Attack and the 95 / 123 ms values hold.
- R4: the Guitar Rig pedal's factory release is at least 2.9 s, not 1.4 s (truncated tail). R5's sketch gives no program dependence.
- R2: the meter ballistics are NI's constants, not ours.
- The cited UA page does not say the 1176 threshold rises with ratio; that is measured here. Summary lines 3 and 4 overstate the body.

Evidence paths are relative to this directory. `evidence/INDEX.txt` explains the layout and how to rerun.
Levels are sine peak dBFS at 48 kHz unless stated. "t63" is the time for the gain (in dB) to cover 63 % of its change.

## 1. Summary

- tubamp's compressor is JUCE's linear-domain peak compressor, per channel, hard knee; its knobs do not mean what they say: "5 ms / 120 ms" measures as 2.2 ms attack and 14 ms release [measured: evidence/bb/ours/default.tbA.tsv].
- That fast, level-dependent release distorts bass: 2.0 % THD at 100 Hz at default settings and 12.4 % at the fastest settings [measured: evidence/bb/ours/default.thd100.tsv, evidence/bb/ours/fast.thd100.tsv].
- Every reference that was read smooths the gain reduction after the gain computer, links stereo, and keeps release time constants long; the guitar pedal models are feedback designs with releases of 0.1 s to seconds [decompiled: Solid Bus Comp 0xef630; Guitar Rig 6 0x269344].
- None of the references uses lookahead; oversampling appears only around saturation stages [measured: evidence/params/solid_bus.txt; decompiled: Supercharger GT 0xf3808].
- Proposal: keep type 0 bit-exact, add a log-domain "Clean" engine with detector hold (0.000 % THD at 100 Hz in the prototype), then Squash, FET and Opto types, a gain-reduction meter, mix, knee, sidechain high-pass and auto makeup [measured: evidence/bb/proto/clean_default.thd100.tsv].

## 2. tubamp today

- Each instance is one `juce::dsp::Compressor<float>` plus a `juce::SmoothedValue<float>` makeup gain [decompiled: source src/PluginProcessor.h:471-473].
- Threshold, ratio, attack and release are pushed into the compressor once per block with no smoothing, then `process(context)` runs and the makeup gain is applied as one ramp per block [decompiled: source src/PluginProcessor.cpp:1290-1310].
- The makeup ramp length is 20 ms [decompiled: source src/PluginProcessor.cpp:22, :880-885].
- Parameter ranges are threshold -60..0 dB, ratio 1..20 (log), attack 0.1..100 ms (log), release 10..1000 ms (log), makeup 0..24 dB; defaults -20 dB, 4:1, 5 ms, 120 ms, 0 dB [decompiled: source src/Parameters.cpp:67-71].
- The detector is a peak detector: the absolute value of each sample goes into a `BallisticsFilter` whose default level type is `peak` [decompiled: source juce_BallisticsFilter.cpp:102-105, juce_BallisticsFilter.h:153].
- The ballistics are one branching one-pole on the linear rectified signal: attack coefficient when the input is above the state, release coefficient otherwise [decompiled: source juce_BallisticsFilter.cpp:107-110].
- The coefficient is `exp(-2*pi*1000 / (fs * t_ms))`, so the real time constant is the knob value divided by 2*pi (5 ms gives 0.8 ms, 120 ms gives 19 ms) [decompiled: source juce_BallisticsFilter.cpp:74, :126-130].
- The gain computer works in the linear domain with a hard knee: gain is 1 below threshold, else `(env / threshold)^(1/ratio - 1)` [decompiled: source juce_Compressor.cpp:104-106].
- The topology is feed-forward and the envelope state is kept per channel, so left and right get different gains [decompiled: source juce_Compressor.h:91-98, juce_BallisticsFilter.cpp:79].
- There is no knee control, sidechain filter, lookahead, auto makeup, dry mix or gain-reduction readout in the class [decompiled: source juce_Compressor.h:44-115].
- Because the release acts on the level before the gain computer, the gain is back at unity as soon as the envelope falls under the threshold, long before the envelope has decayed [inferred].
- Measured on a model of this code: static ratio 4.0 with a hard knee [measured: evidence/bb/ours/default.curve.tsv].
- At default settings the model gives attack t63 2.2 ms, release t63 13.8 ms and release t90 19.7 ms after 7 dB of gain reduction [measured: evidence/bb/ours/default.tbA.tsv].
- With attack 30 ms and release 1000 ms the model gives attack t63 10.2 ms and release t63 113 ms [measured: evidence/bb/ours/a30_r1000.tbA.tsv].
- THD of a 100 Hz sine at -10 dBFS is 1.99 % at defaults and 12.4 % at attack 0.1 ms / release 10 ms; at 1 kHz it is 0.20 % and 2.59 % [measured: evidence/bb/ours/default.thd100.tsv, evidence/bb/ours/fast.thd100.tsv, evidence/bb/ours/default.thd.tsv, evidence/bb/ours/fast.thd.tsv].

## 3. Reference implementations

Address notes for everything in this section are in `evidence/static-notes.txt`.

### 3.1 NI Solid Bus Comp (SSL G bus model, class `NI::DSP::CSL::G_BusComp`)

- Topology is feedback: the detector is called with the previous output sample (input times the previous gain, scaled by 2^-7) [decompiled: Solid Bus Comp 0xef378, instructions 0xef3d8-0xef404].
- The detector is an instantaneous peak detector despite its name `updateRMS`: `|x|` clipped to 1, converted with `log2(|x|)/32`, floor -0.75 [decompiled: Solid Bus Comp 0xef630].
- The gain computer is in the log2 domain with a hard knee and the feedback law `c = clamp((R-1)*(L_out - T), 0, 1)` [decompiled: Solid Bus Comp 0xef694-0xef6b4, 0xef7b8].
- Ballistics act on the gain-reduction value `c`, after the gain computer: a branching one-pole, and the gain is `exp2(-32*state)` [decompiled: Solid Bus Comp 0xef6e8-0xef74c, 0xef3ec-0xef3fc].
- Coefficients are `exp(ln(0.8) / (t * fs))`, so the open-loop time constant is t/0.223 [decompiled: Solid Bus Comp 0xee120, constant 0xbfcc8ff7c79a9a20 at 0xee168].
- Attack table: 0.1, 0.3, 1, 3, 10, 30 ms [decompiled: Solid Bus Comp 0x614098].
- Release table: 0.2, 0.3, 0.4, 0.8, 1.6 and 1.2 (auto), each multiplied by 62.5 to give 12.5, 18.75, 25, 50, 100, 75 ms before the 1/0.223 factor [decompiled: Solid Bus Comp 0x6140c8, multiplier at 0xee5c0].
- Ratio table: 1.5, 2, 3, 4, 5, 10; a per-ratio threshold offset of -12, -9, -5, -4, -3.3, -0.2 dB is added, so the threshold rises with the ratio [decompiled: Solid Bus Comp 0x614038, 0x614068].
- Threshold knob: -18.6..+18.6 dB minus 20 dB; makeup knob: -5.25..+21 dB [decompiled: Solid Bus Comp 0x613f88, 0xee324, 0x613fe0].
- Auto release uses a second, slower envelope; while releasing, the fast state relaxes toward the larger of the target and the slow envelope [decompiled: Solid Bus Comp 0xef6c0-0xef710].
- Stereo link takes the minimum gain over channels; the output is `in*gain*makeup*wet + in*dry` [decompiled: Solid Bus Comp 0xef464].
- `processAudio` converts to double and calls only `log2`, `exp2` and the meter; it calls no resampler or delay [decompiled: Solid Bus Comp 0xeeccc].

### 3.2 NI Solid Dynamics (SSL E channel, class `NI::DSP::CSL::E_Dynamics`), compressor half

- Topology is feed-forward: the detector squares the gained input sample [decompiled: Solid Dynamics 0xef514, 0xef560].
- The mean-square smoother has a time constant of 8.25e-8 s, so the detector is effectively instantaneous peak in dB [decompiled: Solid Dynamics 0xee5b4, constant at 0xee5d4; 0xef578].
- The gain computer is in dB with a quadratic soft knee: inside the knee the overshoot is `(a + k)^2 / (4k)`, outside it is `a` [decompiled: Solid Dynamics 0xef5c8-0xef5e0].
- Ballistics act on the dB overshoot after the gain computer: one-pole attack, and release either one-pole or linear in dB (`e -= min(rate, e - target)`) [decompiled: Solid Dynamics 0xef624-0xef638].
- Coefficients are `exp(ln(0.2)/(t*fs))`; the linear release rate is `1/((rel/3600)*fs)` dB per sample [decompiled: Solid Dynamics 0xee5b4, constants at 0xee5ec and 0xee66c].
- Auto makeup is computed from the threshold and ratio knob positions by a resistor-network style formula, `0.44 + 5.9*f(thr, ratio)` [decompiled: Solid Dynamics 0xef008].
- The soft knee adds 8.5 dB of threshold compensation [decompiled: Solid Dynamics 0xef09c].
- The final gain is the minimum of compressor gain and gate/expander gain [decompiled: Solid Dynamics 0xef68c].

### 3.3 NI Supercharger and Supercharger GT (tube compressor, classes `Rockruepel_Comp*`)

- Supercharger GT is feedback: the input is multiplied by the current gain before it enters the sidechain filters [decompiled: Supercharger GT 0xf33dc, 0xf3408, 0xf348c].
- Its detector is a branching one-pole on `|sidechain|` in the linear domain [decompiled: Supercharger GT 0xf360c-0xf3638].
- Its static curve is a cubic-spline table lookup of the envelope, not a threshold/ratio formula [decompiled: Supercharger GT 0xf2aa8].
- Saturation sits after the gain cell and evaluates a five-harmonic generator twice per sample, which reads as 2x oversampling [decompiled: Supercharger GT 0xf3808; inferred].
- The dry path goes through a 16-sample ring before the mix, which matches the 16-sample latency the AU reports at 44.1 kHz [decompiled: Supercharger GT 0xf379c-0xf37ac; measured: evidence/params/supercharger_gt.txt].
- Parameters are smoothed per sample by one-poles with coefficient 0.005 [decompiled: Supercharger GT 0xf2d38].
- Supercharger's single Compress knob lowers the threshold and raises ratio and makeup together: low-level gain +5.2, +11.1, +17.1, +29.3 dB at 25, 50, 75, 100 % [measured: evidence/bb/supercharger/curve_c25.curve.tsv, curve_c50.curve.tsv, curve_c75.curve.tsv, curve_c100.curve.tsv].
- The read of both binaries is partial: the Supercharger (non-GT) core and the GT macro tables were not decoded [inferred].

### 3.4 NI Transient Master (secondary)

- It is an envelope shaper, not a compressor: Attack at maximum gives +14.4 dB of gain 1 ms into a burst, +3.8 dB at 30 ms and +0.5 dB at 100 ms, with unity steady-state gain [measured: evidence/bb/transient_master/att_up.tbA.tsv].

### 3.5 Guitar Rig 6 stomp compressor (`NI::DSP::CompressorPedal`)

- Topology is feedback with a linear gain cell: `y = x * G * (1 - env)` and the detector reads the post-gain signal [decompiled: Guitar Rig 6 0x269344, instructions 0x26943c-0x269444].
- The detector is the linked maximum of `|L|` and `|R|`, minus a fixed offset of 3.926, clamped to 0..1, then squared [decompiled: Guitar Rig 6 0x269380-0x2693a8, constant at 0x269360].
- The envelope is a branching one-pole on that squared value [decompiled: Guitar Rig 6 0x2693ac-0x2693cc].
- Sustain sets a pre-gain of `(s^2*7.8 + 0.44)*10.2`, 4.5x to 84x (13 to 38.5 dB) [decompiled: Guitar Rig 6 0x268e80].
- The knob laws give an attack time constant of 1.5 ms to 100 ms and a release time constant of 0.1 s to 20 s, if the run-time lookup table maps MIDI pitch to Hz [decompiled: Guitar Rig 6 0x268c18, 0x268cdc, display factors at 0xad498 and 0xad3a8; inferred].
- The headless measurement confirms that law: release t63 is 56, 210 and 788 ms at Release 0, 0.25 and 0.5, ratios 3.75 and 14.1 against the predicted 3.76 and 14.1 [measured: evidence/bb/gr6_stomp/A0_R0.tbA.tsv, A0_R25.tbA.tsv, A0_R50.tbA.tsv].
- Measured attack t63 is 2.7, 16.4 and 91 ms at Attack 0, 0.5 and 1 [measured: evidence/bb/gr6_stomp/A0_R0.tbA.tsv, A50_R0.tbA.tsv, A100_R0.tbA.tsv].
- The factory preset has +16 dB of low-level gain, a -1 dB point at -25 dBFS and a slope ratio above 35; its attack t63 is 45 ms and its release t63 1362 ms [measured: evidence/bb/gr6_stomp/factory.curve.tsv, factory.tbA.tsv].
- Release is slower after shallow compression: t63 82 ms after 5 dB of reduction against 56 ms after 15 dB at the same setting [measured: evidence/bb/gr6_stomp/A0_R0.tbB.tsv, A0_R0.tbA.tsv].
- All eight internal gains are ramped linearly over 20 ms [decompiled: Guitar Rig 6 0x268a54, 0x2690a4].

### 3.6 Guitar Rig 6 feedback compressor (`NI::DSP::FBCompLite`)

- It is a circuit-style feedback compressor: the detector is the linked `|post-gain signal|` and charges a capacitor state only when it exceeds it [decompiled: Guitar Rig 6 0x27bbc8, 0x27beac, 0x27bef0].
- Release has two fixed time constants, 75.9 ms and 484 ms, from two coupled states whose average is the control voltage [decompiled: Guitar Rig 6 0x27b518; 0x27bc28, 0x27bc48].
- Attack is an RC value `(37500*p + 6500) * 0.22 uF`, 1.43 to 9.68 ms [decompiled: Guitar Rig 6 0x27b3f4].
- The gain is a smooth function of the control voltage: 1 below 0.2455, `exp(-19.73*ln(v/0.2455)^4)` up to 0.3913, then `0.3936*(v/0.3913)^-8` [decompiled: Guitar Rig 6 0x27bc38, 0x27bc40, 0x27bf90, 0x27c02c, 0x27bc80].
- The coefficients use `-0.5/(tau*fs)` and the sidechain update appears twice in the loop, which reads as two sub-steps per sample [decompiled: Guitar Rig 6 0x27b444; inferred].
- This component was not measured [measured: evidence/bb/gr6_vc_GAP.txt].

### 3.7 Guitar Rig 6 tube compressor (`NI::DSP::TubeCompressor`)

- Topology is feed-forward [decompiled: Guitar Rig 6 0x3099bc].
- A parabolic soft clipper `u - u|u|/8` (limit +-2) is applied to the driven input before detection [decompiled: Guitar Rig 6 0x309a10-0x309a14].
- The detector is `|L + R|` with instant attack and a multiplicative decay, which is a constant dB/s release [decompiled: Guitar Rig 6 0x309b90-0x309b9c].
- The gain computer is in dB with a quadratic knee of total width 4k, k = 0.5..30 dB [decompiled: Guitar Rig 6 0x309ba8-0x309be4, 0x309570].
- Attack is a one-pole on the dB overshoot after the knee [decompiled: Guitar Rig 6 0x309ca8].
- Threshold range is -72..-12 dB and output gain -48..+48 dB [decompiled: Guitar Rig 6 0x309524, 0x309648].
- Measured headless, Knee at minimum gives a sharp bend at -36 dBFS with slope ratio 6.1, and Knee at maximum bends over the whole -60..0 dBFS range [measured: evidence/bb/gr6_tube/knee0.curve.tsv, knee100.curve.tsv].
- Attack t63 is 1.25 ms at Attack 0 and 18 ms at 0.5 [measured: evidence/bb/gr6_tube/factory.tbA.tsv, att50.tbA.tsv].
- Release depends on depth, as a level-domain release predicts: at Release 0.5, t63 is 1565 ms after 9 dB of reduction and 1104 ms after 5 dB [measured: evidence/bb/gr6_tube/rel50.tbA.tsv, rel50.tbB.tsv].

### 3.8 AmpliTube 5

- The `IK::Comp::*` classes hold UI and profiles only; DSP lives in `IK::KIS::FX::*` kernels [decompiled: AmpliTube 5 0x148185c].
- The Dyna Comp model is a circuit simulation driven by four device tables [decompiled: AmpliTube 5 0xc9ec28, tables at 0x239c24c, 0x239a308, 0x2399320, 0x2398338].
- Its code carries the constants 500000, 27000 and 1e-5, which match a 500k sensitivity pot, a 27k series resistor and a 10 uF envelope capacitor [decompiled: AmpliTube 5 0xc9ec44, 0xc9ec54, 0xc9ec70].
- The 1176 and LA-2A models are large circuit solvers (7676 and 8376 bytes of process code) with a method named `UpdateKMethod`, clip tables and an opto voltage-to-resistance table [decompiled: AmpliTube 5 0xff2e50, 0xff51d0, 0xfd213c, 0x31186b8, 0x2a5b660].
- The generic stereo compressor is feedback with a linear gain cell `gain = max((1 - k*env)*g, 0.01)` and an attack network built from component constants [decompiled: AmpliTube 5 0xefcaa8, 0xefc8f8].
- These kernels were not read to the level of time constants, and AmpliTube is static-only because it does not run under the headless host [inferred].

### 3.9 Plugin Alliance (black-box)

- Shadow Hills optical section: slope ratio 2.7-2.9, attack t63 1.7 ms, release t50 12 ms and t90 101 ms [measured: evidence/bb/shadow_hills/curve_opt_th16.curve.tsv, t_opt_th16.tbA.tsv].
- Shadow Hills discrete section: attack t63 0.83 ms at "0.1 ms" and 36 ms at "30 ms"; release t63 81 ms at "0.1 s" and 425 ms at "1.2 s" [measured: evidence/bb/shadow_hills/t_dis_a0p1ms_rec0p1.tbA.tsv, t_dis_a30ms_rec0p1.tbA.tsv, t_dis_a0p1ms_rec1p2.tbA.tsv].
- Its "Dual" recover is program dependent: release t50 94 ms after a 0.5 s burst and 1538 ms after a 5 s burst [measured: evidence/bb/shadow_hills/t_dis_a0p1ms_recdual.tbA.tsv, t_dis_a0p1ms_recdual.tbL.tsv].
- Its sidechain filter (90 Hz) cuts gain reduction at 40 Hz from 7.6 dB to 1.7 dB and leaves 1 kHz within 0.2 dB [measured: evidence/bb/shadow_hills/d_dis.fsteps.tsv, d_dis_scf.fsteps.tsv].
- bx_console Focusrite SC: the "5:1" step measures 4.3 near threshold and 10.7 at 0 dBFS; attack t63 1.4 to 67 ms; minimum release t63 110 ms [measured: evidence/bb/focusrite_sc/curve_ratio3.curve.tsv, t_a0_r0.tbA.tsv, t_a100_r0.tbA.tsv].
- Its maximum release setting is program dependent: t63 618 ms after 0.5 s and 1964 ms after 5 s [measured: evidence/bb/focusrite_sc/t_a0_r100.tbA.tsv, t_a0_r100.tbL.tsv].
- No static reading was done on either binary [inferred].

### 3.10 UADx (black-box only)

- 1176 Rev A: slope ratio 5.7-6.1 at "4:1", 10-10.7 at "8:1", 25-33 at "20:1"; the -1 dB point moves from -25.7 to -21.9 to -19.6 dBFS as the ratio rises [measured: evidence/bb/ua_1176a/curve_ratio1.curve.tsv, curve_ratio2.curve.tsv, curve_ratio4.curve.tsv].
- 1176 Rev A attack t63 is 0.23 ms at the fastest setting, 0.99 ms at mid and 2.0 ms near the slowest, at 192 kHz with a 0.1 ms window [measured: evidence/bb/ua_1176a/hs192_fastest.tsv, hs192_mid.tsv, hs192_slowest.tsv].
- Turning its Attack fully down switches gain reduction off (0.1 dB) [measured: evidence/bb/ua_1176a/t_a0_r50.tbA.tsv].
- 1176 Rev A release at the fastest setting is program dependent: t63 43 ms after a 0.1 s burst, 95 ms after 0.5 s, 123 ms after 5 s, with t90 399 ms; the slowest setting gives t63 1118 ms [measured: evidence/bb/ua_1176a/t_a100_r100.tbF.tsv, t_a100_r100.tbA.tsv, t_a100_r100.tbL.tsv, t_a100_r0.tbA.tsv].
- 1176 Rev E: attack t63 0.18 to 2.4 ms; fastest release t63 31, 58 and 129 ms for the same three bursts [measured: evidence/bb/ua_1176e/hs192_fastest.tsv, hs192_slowest.tsv, t_a100_r100.tbF.tsv, t_a100_r100.tbA.tsv, t_a100_r100.tbL.tsv].
- The 1176 adds harmonics without gain reduction: with ratio "None" at -10 dBFS the 2nd harmonic is -55 dB and the 3rd -53 dB; at -40 dBFS THD is 0.007 % [measured: evidence/bb/ua_1176a/d_none.thd.tsv, d_none.thdlo.tsv].
- Its sidechain filter is a tilt, not only a high-pass: gain at -10 dBFS is -6.2 dB at 40 Hz, -17.8 dB at 1 kHz and -25.5 dB at 8 kHz, against a flat -13 dB with the filter off [measured: evidence/bb/ua_1176a/d_sc_a50_r50.fsteps.tsv, d_a50_r50.fsteps.tsv].
- LA-2A Gray: slope ratio 4.3-5.4 in Compress and 9-10.7 in Limit; attack t63 0.84 ms and t90 10.3 ms; release t50 78 ms, t63 132 ms, t90 720 ms [measured: evidence/bb/ua_la2a_gray/curve_comp_pr50.curve.tsv, curve_limit_pr50.curve.tsv, hs192_comp.tsv, t_comp_pr50.tbA.tsv].
- LA-2A Silver and Tube are close to Gray: release t50 89 and 84 ms, t90 937 and 1024 ms [measured: evidence/bb/ua_la2a_silver/t_comp_pr50.tbA.tsv, evidence/bb/ua_la2a_tc/t_comp_pr50.tbA.tsv].
- LA-2 is slower and shows optical memory: attack t63 7.8 ms; release t50 426 ms and t90 3.1 s after 0.5 s, t50 621 ms and t90 4.3 s after 5 s [measured: evidence/bb/ua_la2/t_comp_pr50.tbA.tsv, t_comp_pr50.tbL.tsv].
- LA-3A is the fastest optical: attack t63 0.47 ms, release t50 46 ms, t90 231 ms [measured: evidence/bb/ua_la3a/hs192_comp.tsv, t_comp_pr50.tbA.tsv].
- The LA-2A Emphasis control weights the detector toward high frequencies: at full emphasis gain reduction is 5.1 dB at 200 Hz and 13.7 dB at 3 kHz, against 15.6 and 14.8 dB flat [measured: evidence/bb/ua_la2a_gray/d_comp_pr50_emph.fsteps.tsv, d_comp_pr50.fsteps.tsv, d_comp_pr50.fstepslo.tsv].
- The 1176 models show 86 samples of delay at 48 kHz and 151 at 192 kHz, which fits oversampling filters and not a fixed lookahead time [measured: evidence/bb/ua_1176a/t_a100_r100.tbA.tsv, hs192_fastest.tsv].
- API 2500 could not be measured: `auhost render` hangs [measured: evidence/bb/ua_api2500_GAP.txt].

## 4. Measurements

Test signals:
- `curve`: 1 kHz steps -60..0 dBFS, 1 s each.
- `tbA`: 1 kHz burst from -40 to -10 dBFS for 0.5 s. `tbB`: same to -20 dBFS. `tbL`: 5 s burst. `tbF`: 8 kHz, 0.1 s.
- `hs192`: 10 kHz burst at 192 kHz.
- `thd` / `thd100`: 1 kHz / 100 Hz sine at -10 dBFS.
- `fsteps`: eight tones 40 Hz..8 kHz at -10 dBFS.
- The Guitar Rig tube compressor was driven 20 dB lower (bursts -60 to -30 and -40 dBFS) because its factory drive adds 25 dB.

Each evidence file starts with its exact commands. Timing from a 1 kHz carrier has a 1 ms window, so sub-millisecond attacks are taken from `tbF` or `hs192`.

### 4.1 Attack and release (ms)

| Unit and setting | attack t63 | release t63 (0.5 s burst) | same, shallower burst | same, 5 s burst | release t90 | Evidence (`evidence/bb/`) |
| --- | --- | --- | --- | --- | --- | --- |
| tubamp, 5 ms / 120 ms | 2.2 | 14 | no GR | n/a | 20 | ours/default.tbA.tsv |
| tubamp, 0.1 ms / 10 ms | 0.21 | 2 | no GR | n/a | 2 | ours/fast.tbA.tsv |
| Solid Bus, 0.1 ms / 0.1 s | 0.27 | 56 | 56 | 56 | 130 | solid_bus/hs192_a0p1ms.tsv, t_a0p1ms_rel0.* |
| Solid Bus, 30 ms / 0.1 s | 34.7 | 56 | 56 | 56 | 130 | solid_bus/t_a30ms_rel0.* |
| Solid Bus, 1 ms / auto | 2.7 | 513 | 435 | 663 | 1251 | solid_bus/t_a1ms_relauto.* |
| Solid Dynamics, fast / min, exp | 1.9 | 21 | 21 | 21 | 48 | solid_dyn/t_afast_rel0_m0.* |
| Solid Dynamics, fast / min, linear | 2.2 | 91 | 32 | 91 | 129 | solid_dyn/t_afast_rel0_m1.* |
| Supercharger 50 %, Punch off | 1.3 | 215 | 270 | 215 | 587 | supercharger/t_c50_p0.* |
| Supercharger 50 %, Punch on | 47.9 | 197 | 253 | 197 | 567 | supercharger/t_c50_p1.* |
| Supercharger GT, min / min | 0.19 | 42 | 49 | 42 | 113 | supercharger_gt/t_a0_r0.* |
| Supercharger GT, max / max | 24.1 | 465 | 471 | 465 | 1167 | supercharger_gt/t_a100_r100.* |
| GR6 Stomp Compressor, Attack 0 / Release 0 | 2.7 | 56 | 82 | 56 | 166 | gr6_stomp/A0_R0.* |
| GR6 Stomp Compressor, Attack 0 / Release 0.5 | 2.7 | 788 | 1152 | 788 | 2329 | gr6_stomp/A0_R50.* |
| GR6 Stomp Compressor, factory | 44.7 | 1362 | n/a | n/a | 2424 | gr6_stomp/factory.tbA.tsv |
| GR6 Tube Compressor, factory | 1.25 | 9 | 7 | n/a | 16 | gr6_tube/factory.* |
| GR6 Tube Compressor, Release 0.5 | 1.27 | 1565 | 1104 | n/a | 2713 | gr6_tube/rel50.* |
| UADx 1176 Rev A, fastest / fastest | 0.23 | 95 | 83 | 123 | 399 | ua_1176a/hs192_fastest.tsv, t_a100_r100.* |
| UADx 1176 Rev A, fastest / slowest | 0.23 | 1118 | 1047 | 1216 | 2112 | ua_1176a/t_a100_r0.* |
| UADx 1176 Rev E, fastest / fastest | 0.18 | 58 | 50 | 129 | 179 | ua_1176e/hs192_fastest.tsv, t_a100_r100.* |
| UADx 1176AE, fastest / fastest | 0.19 (tbF) | 101 | 85 | 103 | 288 | ua_1176ae/t_a100_r100.* |
| UADx LA-2 | 7.8 | 731 | no GR | 1106 | 3129 | ua_la2/t_comp_pr50.* |
| UADx LA-2A Gray | 0.84 | 132 | 171 | 133 | 720 | ua_la2a_gray/hs192_comp.tsv, t_comp_pr50.* |
| UADx LA-2A Silver | 1.8 | 159 | 131 | 169 | 937 | ua_la2a_silver/t_comp_pr50.* |
| UADx LA-2A Tube | 1.9 | 157 | 105 | 184 | 1024 | ua_la2a_tc/t_comp_pr50.* |
| UADx LA-3A | 0.47 | 82 | 55 | 105 | 231 | ua_la3a/hs192_comp.tsv, t_comp_pr50.* |
| Shadow Hills optical | 1.7 | 20 | 71 | 20 | 101 | shadow_hills/t_opt_th16.* |
| Shadow Hills discrete, 0.1 ms / Dual | 0.79 | 654 | 652 | 2319 | 3770 | shadow_hills/t_dis_a0p1ms_recdual.* |
| Focusrite SC, min / min | 1.4 | 110 | no GR | 110 | 247 | focusrite_sc/t_a0_r0.* |
| Proposed Clean, 5 ms / 120 ms | 5.7 | 128 | 128 (-15 dBFS) | n/a | 285 | proto/clean_default.* |

Every row is [measured] in the files named in its last column.

### 4.2 Distortion in gain reduction (THD %, sine at -10 dBFS)

| Unit and setting | 1 kHz | 100 Hz | 1 kHz at -40 dBFS (no GR) | Evidence (`evidence/bb/`) |
| --- | --- | --- | --- | --- |
| tubamp default | 0.20 | 1.99 | n/a | ours/default.thd.tsv, default.thd100.tsv |
| tubamp fastest | 2.59 | 12.38 | n/a | ours/fast.thd.tsv, fast.thd100.tsv |
| Solid Bus, 0.1 ms / 0.1 s | 0.23 | 2.11 | 0.000 | solid_bus/d_a0p1ms_rel0.*, d_a1ms_rel0.thdlo.tsv |
| Solid Dynamics, fast / min | 0.28 | 2.73 | 0.000 | solid_dyn/d_afast_rel0.* |
| Supercharger 50 % | 0.20 | 0.64 | 0.026 | supercharger/d_c50_dirt0.* |
| Supercharger 50 %, Dirt on | 10.76 | 11.47 | 0.235 | supercharger/d_c50_dirt1.* |
| Supercharger GT 75 % | 0.076 | 0.73 | 0.004 | supercharger_gt/d_c75.* |
| GR6 Stomp Compressor, factory | 0.009 | 0.089 | 0.000 | gr6_stomp/factory.thd.tsv, factory.thd100.tsv, factory.thdlo.tsv |
| GR6 Stomp Compressor, Attack 0 / Release 0 | 0.585 | 4.57 | 0.000 | gr6_stomp/A0_R0.* |
| UADx 1176 Rev A, mid / mid | 0.097 | 0.51 | 0.009 | ua_1176a/d_a50_r50.* |
| UADx 1176 Rev A, fastest / fastest | 0.41 | 3.53 | n/a | ua_1176a/d_a100_r100.* |
| UADx 1176 Rev E, fastest / fastest | 0.38 | 3.36 | n/a | ua_1176e/d_a100_r100.* |
| UADx LA-2A Gray | 0.19 | 1.02 | 0.009 | ua_la2a_gray/d_comp_pr50.* |
| UADx LA-3A | 0.32 | 2.92 | 0.000 | ua_la3a/d_comp_pr50.* |
| Shadow Hills optical | 0.36 | 3.75 | 0.000 | shadow_hills/d_opt.* |
| Shadow Hills discrete | 0.13 | 1.17 | n/a | shadow_hills/d_dis.* |
| Focusrite SC, min / min | 0.007 | 0.49 | 0.000 | focusrite_sc/d_a0_r0.* |
| Proposed Clean default, 8 ms hold | 0.000 | 0.000 | n/a | proto/clean_default.* |
| Proposed Clean 0.1 ms / 10 ms, 8 ms hold | 0.000 | 0.000 | n/a | proto/clean_fast.* |
| Proposed Clean 0.1 ms / 10 ms, no hold | 0.575 | 4.67 | n/a | proto/clean_fast_nohold.* |

Every row is [measured] in the files named in its last column.

### 4.3 Static curves

| Unit and setting | -1 dB point (dBFS) | slope ratio near threshold | slope ratio at top | Evidence (`evidence/bb/`) |
| --- | --- | --- | --- | --- |
| tubamp, -20 dB, 4:1 | -18.0 | 4.0 | 4.0 | ours/default.curve.tsv |
| Solid Bus 1.5 / 2 / 4 / 10 | -28.4 / -26.6 / -22.5 / -19.4 | 1.5 / 1.9 / 3.7 / 8.1 | 1.5 / 2.0 / 3.8 / 8.5 | solid_bus/curve_r*.curve.tsv |
| Solid Dynamics 4:1 hard / soft | -28.0 / -26.4 | 3.8 / 2.1 | 3.9 / 3.4 | solid_dyn/curve_hard_r4.curve.tsv, curve_soft_r4.curve.tsv |
| GR6 Stomp Compressor, factory | -25.3 | 37 | 100 | gr6_stomp/factory.curve.tsv |
| GR6 Tube Compressor, knee min / max | -35.2 / -50.7 | 6.1 / 2.6 | 5.4 / 3.4 | gr6_tube/knee0.curve.tsv, knee100.curve.tsv |
| UADx 1176 Rev A 4 / 8 / 20 | -25.7 / -21.9 / -19.6 | 5.7 / 10 / 25 | 6.1 / 10.7 / 33 | ua_1176a/curve_ratio*.curve.tsv |
| UADx LA-2A Gray comp / limit | -28.3 / -28.4 | 5.4 / 9.0 | 4.3 / 10.7 | ua_la2a_gray/curve_*.curve.tsv |
| UADx LA-3A comp / limit | -21.9 / -22.0 | 4.1 / 5.5 | 5.0 / 20 | ua_la3a/curve_*.curve.tsv |
| Shadow Hills optical | -22.3 | 2.9 | 2.7 | shadow_hills/curve_opt_th16.curve.tsv |
| Focusrite SC "5:1" | -17.7 | 4.3 | 10.7 | focusrite_sc/curve_ratio3.curve.tsv |

Every row is [measured] in the files named in its last column.

### 4.4 Sidechain frequency dependence

- With no sidechain filter engaged, gain reduction is flat within 0.3 dB from 40 Hz to 3 kHz on Solid Bus Comp, Solid Dynamics, 1176 Rev A, Shadow Hills discrete and Focusrite SC [measured: evidence/bb/solid_bus/d_a1ms_rel0.fsteps.tsv, solid_dyn/d_afast_rel0.fsteps.tsv, ua_1176a/d_a50_r50.fsteps.tsv, shadow_hills/d_dis.fsteps.tsv, focusrite_sc/d_a0_r0.fsteps.tsv].
- Supercharger GT's Detector HP at its lowest setting leaves 9.4 dB of reduction at 40 Hz against 23.0 dB at 1 kHz [measured: evidence/bb/supercharger_gt/d_c75_hp0.fsteps.tsv, d_c75.fstepslo.tsv].

## 5. Industry practice

- Smoothing belongs after the gain computer: Solid Bus Comp, Solid Dynamics and the Guitar Rig tube compressor all smooth the gain-reduction value, not the input level [decompiled: Solid Bus Comp 0xef630; Solid Dynamics 0xef624; Guitar Rig 6 0x309ca8].
- The measured consequence is a release that does not depend on depth: 56 ms at both burst depths on Solid Bus Comp [measured: evidence/bb/solid_bus/t_a0p1ms_rel0.tbA.tsv, t_a0p1ms_rel0.tbB.tsv].
- A published design reference states the same placement: gain smoothing on the gain computer output in dB, a quadratic soft knee, and auto makeup equal to minus the static curve at 0 dB [literature: https://www.mathworks.com/help/audio/ug/dynamic-range-control.html].
- Guitar-stage compressors are feedback designs: both vendors' pedal models read the post-gain signal and use a linear gain cell [decompiled: Guitar Rig 6 0x269344; AmpliTube 5 0xefcaa8].
- The published circuit analysis of the Dyna Comp describes an OTA whose bias current is fed back from an envelope detector on its own output, with a 10 uF envelope capacitor and a 27 k resistor, simulated with a one-sample delay in the loop [literature: https://www.dafx.de/paper-archive/2011/Papers/22_e.pdf].
- The pedal model behaves as a limiter with a lot of make-up: +16 dB below threshold and a slope ratio above 35 at its factory preset [measured: evidence/bb/gr6_stomp/factory.curve.tsv].
- In a feedback loop the effective attack is the open-loop time constant divided by roughly the ratio: Solid Bus Comp's 30 ms step is 134 ms open loop and measures 34.7 ms at 4:1 [decompiled: Solid Bus Comp 0xee120; measured: evidence/bb/solid_bus/t_a30ms_rel0.tbA.tsv].
- Feedback attack also slows for small overshoots: 34.7 ms for a 30 dB step and 40.8 ms for a 20 dB step at the same setting [measured: evidence/bb/solid_bus/t_a30ms_rel0.tbA.tsv, t_a30ms_rel0.tbB.tsv].
- Release minimums are long: fastest release t63 is 56 ms on Solid Bus Comp, 21 ms on Solid Dynamics, 42 ms on Supercharger GT, 56 ms on the Guitar Rig pedal, 43-123 ms on the 1176 Rev A and 110 ms on Focusrite SC [measured: section 4.1 files].
- Bass distortion at the fastest release is still 2-5 % at 100 Hz on the references that allow a fast release, so it comes with a fast peak detector and is managed by release time, not removed [measured: section 4.2 files].
- The pedal's factory release is long enough to hide it: 0.089 % at 100 Hz [measured: evidence/bb/gr6_stomp/factory.thd100.tsv].
- Program-dependent release is the norm on the character units: two-stage on the 1176 and LA-2A models, auto on Solid Bus Comp, Dual on Shadow Hills, memory on the LA-2 [measured: section 4.1 files].
- Two exponentials fit the measured two-stage releases: weight 0.6 at 60 ms plus 0.4 at 500 ms turns the LA-2A Gray's 78 / 132 / 720 ms into 88 / 142 / 696 ms; weight 0.7 at 55 ms plus 0.3 at 350 ms turns the 1176 Rev A's 61 / 95 / 399 ms into 66 / 98 / 395 ms [measured: evidence/bb/proto/opto.tbA.tsv, evidence/bb/proto/fet.tbA.tsv, evidence/bb/ua_la2a_gray/t_comp_pr50.tbA.tsv, evidence/bb/ua_1176a/t_a100_r100.tbA.tsv].
- Stereo is linked on every unit that was read: minimum gain, maximum of `|L|,|R|`, or `|L+R|` [decompiled: Solid Bus Comp 0xef464; Guitar Rig 6 0x269380, 0x309b90].
- No reference adds lookahead: the NI and Plugin Alliance units report zero latency and the UADx delay scales like oversampling filters [measured: evidence/params/solid_bus.txt, evidence/params/shadow_hills.txt, evidence/bb/ua_1176a/hs192_fastest.tsv].
- A parallel mix or dry control is present on Solid Bus Comp, Solid Dynamics, both Superchargers, Shadow Hills, Focusrite SC and the UADx 1176, LA-2, LA-2A Gray/Silver and LA-3A [measured: evidence/params/].
- A sidechain high-pass or tilt is an option, off by default, on Supercharger GT, Shadow Hills (90 Hz), the UADx 1176 and the LA-2A [measured: evidence/params/supercharger_gt.txt, evidence/params/shadow_hills.txt, evidence/params/ua_1176a.txt, evidence/params/ua_la2a_gray.txt].
- Parameter changes are de-zippered by 20 ms linear ramps (Guitar Rig) or per-sample one-poles (Supercharger GT) [decompiled: Guitar Rig 6 0x268a54; Supercharger GT 0xf2d38].
- NI's gain-reduction meter clamps at 20 dB, rises through a one-pole that reaches 99 % in 0.3 s, and falls linearly at 20 dB per 0.3 s [decompiled: Solid Bus Comp 0xed40c, 0xed3b0].
- For guitar, a compressor before gain stages evens the level into the drive and raises the noise floor; a blend control keeps the pick attack; a tone control restores highs [literature: https://www.wamplerpedals.com/wamplerblog/item/2324-compressors-for-guitar-a-simple-guide; https://www.premierguitar.com/gear/keeley-compressor-plus-review].
- The 1176 is specified at 20-800 microseconds attack and 50 ms-1.1 s release, with the threshold rising with the ratio [literature: https://www.uaudio.com/blogs/ua/1176-collection-tips].
- The LA-2A is specified at 10 ms attack, about 60 ms for the first half of the release and 0.5-5 s for the rest [literature: https://www.soundpure.com/p/universal-audio-teletronix-la-2a/47].

What defines each guitar-relevant character, in numbers:

| Character | Topology | Attack | Release | Curve | Colour | Source |
| --- | --- | --- | --- | --- | --- | --- |
| OTA pedal squash (Dyna Comp / Ross) | feedback, linear gain cell | 2.7-91 ms t63 | 56 ms to seconds t63, slower after shallow compression | +16 dB below threshold, slope ratio 35+ | none in the GR6 model | [decompiled: Guitar Rig 6 0x269344; measured: evidence/bb/gr6_stomp/] |
| FET (1176) | feedback | 0.2-2.4 ms t63 | 43 ms-1.2 s, two-stage, program dependent | slope 6 to 30, threshold rises 6 dB from 4:1 to 20:1 | 2nd -55 dB, 3rd -53 dB at -10 dBFS | [measured: evidence/bb/ua_1176a/, evidence/bb/ua_1176e/; literature: https://www.uaudio.com/blogs/ua/1176-collection-tips] |
| Optical (LA-2A) | not determined | 0.8-2.3 ms to 63 %, 10-19 ms to 90 % | 78 ms to 50 %, 0.7-1.0 s to 90 % | soft, slope 4-5 (comp), 9+ (limit) | 2nd -57 dB, 3rd -58 dB in 10 dB GR | [measured: evidence/bb/ua_la2a_gray/, evidence/bb/ua_la2a_silver/] |
| Clean VCA (SSL) | feedback or feed-forward, log domain | 0.1-30 ms | 56-450 ms time constant, auto | hard or quadratic knee, exact ratio | none | [decompiled: Solid Bus Comp 0xef630; Solid Dynamics 0xef514] |

## 6. Gap analysis

| Aspect | tubamp | References | Consequence for the player |
| --- | --- | --- | --- |
| Where ballistics act | on the linear level, before the gain computer | on gain reduction, after the gain computer | release depends on depth and is 6-8x shorter than the knob says |
| Time-constant meaning | knob / 2*pi | knob is the 63 % or a calibrated time | settings from other compressors do not transfer |
| Bass distortion | 2.0 % at 100 Hz at defaults, 12.4 % at fastest | 0.1-1 % at factory or mid settings, 2-5 % at fastest | low strings and palm mutes get gritty before the amp model |
| Minimum release | 1.6 ms effective | 21-110 ms | the lower part of the release knob only adds distortion |
| Knee | hard | quadratic soft knee or progressive curves | audible grab at the threshold on clean guitar |
| Stereo | per-channel gain | linked | image shifts after stereo effects |
| Topology choice | feed-forward only | feedback for pedal, FET and bus types | no pedal-style squash |
| Program-dependent release | none | two-stage or auto on every character unit | a single release must trade pumping against dull sustain |
| Sidechain filter | none | optional high-pass or tilt | low E drives the gain for the whole chord |
| Makeup | manual 0-24 dB | manual, auto on the E-channel, one-knob macros | level drops when the threshold is lowered |
| Mix | none | present on most | no parallel compression to keep pick attack |
| Meter | none | every unit has a GR meter | the player cannot see how hard the block works |
| Parameter smoothing | threshold and ratio jump per block | 20 ms ramps or one-poles | possible zipper when automating threshold |
| Lookahead, oversampling | none | none in the gain path | no gap |

Row sources: sections 2, 3, 4 and 5.

## 7. Recommendations

All new parameters exist per instance (`comp_*`, `comp2_*`, `comp3_*`). Existing ids and ranges stay frozen. A new id `comp_type` selects the algorithm; value 0 runs today's code path unchanged, so old sessions render bit-for-bit. Sessions saved before the id existed must restore to 0; new instances may default to 1 through the state-version check (`kStateVersion`, src/PluginProcessor.cpp:36). All constants below are ours: chosen, or fitted to the measurements named.

### R1. Clean engine (type 1): log-domain feed-forward with detector hold

- Build: linked peak detector, 8 ms peak hold, dB gain computer with quadratic knee, branching one-pole on the gain reduction in dB, time constants equal to the knob values.
- Evidence: placement and knee as in section 5; tubamp's measured times and THD in section 2; the hold removes 100 Hz ripple in the prototype (0.000 % against 1.99 % today) [measured: evidence/bb/proto/clean_default.thd100.tsv, evidence/bb/ours/default.thd100.tsv].
- Sketch (per sample, all channels share one gain):

```
d    = max over channels of |x[ch]|                 // link
if d >= held: held = d; cnt = holdSamples           // holdSamples = 0.008 * fs
elif cnt > 0: cnt -= 1
else:        held = d
L    = 20*log10(max(held, 1e-6))
o    = L - T
gc   = 0                         if o <= -W/2
       s*(o + W/2)^2 / (2W)      if |o| < W/2        // s = 1/R - 1
       s*o                       otherwise
g    = (gc < g) ? aA*g + (1-aA)*gc : aR*g + (1-aR)*gc   // aX = exp(-1/(tauX*fs))
y[ch] = x[ch] * 10^((g + makeup)/20)
```

- The 8 ms hold bridges half a period down to 62.5 Hz, so a 7-string low B (61.7 Hz) is just outside it [inferred].
- New parameters: `comp_type` choice {Legacy, Clean, Squash, FET, Opto}, layout default 0; `comp_knee` 0..24 dB, default 6.
- Cost: one `log10` and one `pow` per sample per instance, no latency, a few bytes of state [inferred].
- Proof: run the section 4 tests on the built plugin at -20 dB / 4:1 / 5 ms / 120 ms. Type 0 must null against the current build.

| Metric | Accept | Prototype |
| --- | --- | --- |
| attack t63 | 5 ms +-1 ms | 5.7 ms |
| release t63, -10 and -15 dBFS bursts | 120-130 ms, equal | 128 ms at both |
| THD at 100 Hz | under 0.1 % | 0.000 % |
| THD at 82.4 Hz | under 0.2 % | 0.054 % |
| slope ratio | 4.0 +-0.1 | 4.0 |

Prototype values are [measured: evidence/bb/proto/clean_default.tbA.tsv, clean_default.tbB.tsv, clean_default.thd100.tsv, clean_default.thd82.tsv, clean_default.curve.tsv].

### R2. Gain-reduction meter value

- Build: one `std::atomic<float>` per instance holding the largest gain reduction in dB in the block (for type 0, `-20*log10` of the smallest gain applied). The UI applies ballistics: rise through a one-pole with 65 ms time constant, fall at 67 dB/s, scale 0..20 dB.
- Evidence: every reference has a GR meter; those ballistics are NI's [decompiled: Solid Bus Comp 0xed40c, 0xed3b0].
- New parameters: none (read-only value, same route as the level meters at src/PluginProcessor.h:200).
- Cost: one compare per sample. No audio change.
- Proof: a -10 dBFS sine at -20 dB / 4:1 on type 1 must read 7.5 dB +-0.3 dB.

### R3. Mix, sidechain high-pass, auto makeup

- Build: dry/wet mix on the block output; a one-pole high-pass on the detector signal only; optional auto makeup.
- Evidence: mix and sidechain filters are common (section 5); Shadow Hills' 90 Hz filter cuts 40 Hz gain reduction from 7.6 to 1.7 dB [measured: evidence/bb/shadow_hills/d_dis_scf.fsteps.tsv].
- Sketch: `out = mix*wet + (1-mix)*dry` (no latency, so no delay compensation); detector input `d = |hp(x)|`; auto makeup `M = -gc(L_ref)` with `L_ref = -10 dBFS`, which is the published `-x_sc(0)` rule evaluated at a guitar peak level instead of 0 dBFS [literature: https://www.mathworks.com/help/audio/ug/dynamic-range-control.html].
- New parameters: `comp_mix` 0..100 %, default 100; `comp_sc_hpf` 20..300 Hz (20 = off), default 20; `comp_auto_makeup` bool, default off (the manual makeup knob adds on top).
- Cost: one multiply-add per sample for each feature.
- Proof: at mix 50 % the output must equal the mean of dry and wet to -80 dB. With `comp_sc_hpf` at 100 Hz, gain reduction at 40 Hz must be at least 5 dB less than at 1 kHz on `fsteps`. With auto makeup on, a -10 dBFS sine must leave within 0.5 dB of its input level at any ratio.

### R4. Squash (type 2): OTA-pedal style feedback compressor

- Build: the same dB gain computer driven by the level of the previous output sample, with the feedback law, a slow release and a low threshold plus makeup for sustain.
- Evidence: both pedal models are feedback (section 5); the Guitar Rig pedal measures +16 dB below threshold, slope ratio above 35, attack 45 ms and release t63 1.4 s at its factory preset [measured: evidence/bb/gr6_stomp/factory.curve.tsv, factory.tbA.tsv].
- Sketch: `L = 20*log10(max over channels of |y_prev|)`; `gc = (1 - R)*(L - T)` through the same knee; open-loop attack time constant = knob value times R, so the closed-loop attack stays near the knob value; release time constant = knob value times 4 (the 10..1000 ms knob becomes 40 ms..4 s); one-sample delay in the loop.
- Build parameters: R limited to 2..12, knee 6 dB, hold 8 ms, makeup from auto makeup [guess].
- New parameters: none beyond `comp_type`.
- Cost: same as R1.
- Proof: at T = -34 dB, R = 8, attack 2.5 ms, release 150 ms.

| Metric | Accept | Prototype (open-loop 20 ms / 600 ms) |
| --- | --- | --- |
| slope ratio | 7.5-8.5 | 7.7-8.0 |
| attack t63 | under 4 ms | 2.75 ms |
| release t63 | 550-650 ms | 571 ms |
| THD at 100 Hz | under 0.1 % | 0.000 % |

Prototype values are [measured: evidence/bb/proto/squash_fb.curve.tsv, squash_fb.tbA.tsv, squash_fb.thd100.tsv].

### R5. FET (type 3) and Opto (type 4): two-stage release

- Build: R1 with two gain-reduction states that share the attack and release at different rates, summed with a weight.
- Evidence: fits in section 5 [measured: evidence/bb/proto/fet.tbA.tsv, evidence/bb/proto/opto.tbA.tsv].
- Sketch: `g = w*g1 + (1-w)*g2`, both updated like `g` in R1 with release time constants `tau1` and `tau2`.

| Build parameter | FET | Opto | Source |
| --- | --- | --- | --- |
| weight w | 0.7 | 0.6 | fit [measured: evidence/bb/ua_1176a/t_a100_r100.tbA.tsv, evidence/bb/ua_la2a_gray/t_comp_pr50.tbA.tsv] |
| tau1 at release knob minimum | 55 ms | 60 ms | same fit |
| tau2 | 6.4 * tau1 | 8.3 * tau1 | same fit (350/55, 500/60) |
| release knob maps tau1 to | 55 ms..1.0 s | 60 ms..600 ms | [measured: evidence/bb/ua_1176a/t_a100_r0.tbA.tsv, evidence/bb/ua_la2/t_comp_pr50.tbA.tsv] |
| attack knob maps to | 0.2..2.5 ms | 1..10 ms | [measured: evidence/bb/ua_1176a/hs192_fastest.tsv, hs192_slowest.tsv, evidence/bb/ua_la2a_gray/hs192_comp.tsv, evidence/bb/ua_la2/t_comp_pr50.tbA.tsv] |
| knee | 4 dB | 12 dB | [guess], shaped after evidence/bb/ua_1176a/curve_ratio1.curve.tsv and evidence/bb/ua_la2a_gray/curve_comp_pr50.curve.tsv |
| threshold offset with ratio | +2.6 dB per doubling of R above 4 | none | [measured: evidence/bb/ua_1176a/curve_ratio1.curve.tsv, curve_ratio4.curve.tsv] |
| ratio | knob | knob limited to 2..10 | [measured: evidence/bb/ua_la2a_gray/curve_limit_pr50.curve.tsv] |

- Optional colour for FET, off in the first build: `y = x + a*x^2 - b*x^3` with a = 0.0113 and b = 0.090, which gives a 2nd harmonic at -55 dB and a 3rd at -53 dB for a -10 dBFS sine, run at 2x oversampling [measured: evidence/bb/ua_1176a/d_none.thd.tsv; inferred].
- New parameters: none beyond `comp_type`.
- Cost: one extra one-pole per sample. No latency without the colour stage.
- Proof: `tbA` at the fastest release.

| Type | Accept t50 / t63 / t90 | Prototype |
| --- | --- | --- |
| FET | 60-70 / 90-105 / 370-420 ms | 66 / 98 / 395 ms |
| Opto | 80-95 / 130-150 / 650-750 ms | 88 / 142 / 696 ms |

Prototype values are [measured: evidence/bb/proto/fet.tbA.tsv, evidence/bb/proto/opto.tbA.tsv].

### R6. Smooth threshold and ratio

- Build: 20 ms linear ramps on threshold (dB) and on the slope `1/R - 1` for types 1-4.
- Evidence: Guitar Rig ramps all pedal gains over 20 ms [decompiled: Guitar Rig 6 0x268a54].
- New parameters: none. Cost: two adds per sample.
- Proof: automate threshold -40 to -10 dB at 10 Hz on a steady sine; no sideband above -70 dB at multiples of the block rate.

Ranking by audible benefit against cost: R1, R2, R3, R4, R5, R6.

## 8. Open questions and unverified items

- The Giannoulis, Massberg and Reiss JAES 2012 paper could not be opened; the design statements rest on the MathWorks page that cites it and on the decompiled references [inferred].
- The THAT Corp AES preprints were read only by a helper from scanned PDFs and are not cited as evidence here; the feedback speed-up is supported by the Solid Bus Comp measurement instead [inferred].
- ElectroSmash's Dyna Comp analysis was unreachable, so no published attack and release figures for the pedal are cited [inferred].
- Guitar Rig 6's VC 76, VC 2A, VC 160 and Fast Comp were not measured: the state builder failed on their presets [measured: evidence/bb/gr6_vc_GAP.txt].
- AmpliTube 5 is static-only, and its 1176, LA-2A, Fairchild and Fender compressor kernels were not read in detail [inferred].
- The TONEX compressor, which is exposed as AU parameters, was not measured [inferred].
- Solid Bus Comp's auto-release slow constants were not located in the code; the behaviour is measured instead [measured: evidence/bb/solid_bus/t_a1ms_relauto.tbL.tsv].
- Supercharger (non-GT) and the Guitar Rig limiters were not read in detail [inferred].
- Whether the UADx LA-2A models are feedback could not be decided from outside [inferred].
- 1176 attack below 0.2 ms is under the resolution of the method (0.1 ms window at 192 kHz) [inferred].
- Supercharger GT with Attack at minimum gave a near-silent output on the 8 kHz burst; the cause is unknown and those rows are not used [measured: evidence/bb/supercharger_gt/t_a0_r100.tbF.tsv].
- UADx API 2500 was not measured [measured: evidence/bb/ua_api2500_GAP.txt].
- The 8 ms hold, the auto-makeup reference level and the Squash release scaling are design choices that need a listening test with a real DI guitar track [guess].
