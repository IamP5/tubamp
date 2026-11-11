# Guitar Rig 6 chorus internals: RigEnsemble and ChorusFlangerTCSCF (static read)

Binary: `/Library/Audio/Plug-Ins/Components/Guitar Rig 6.component/Contents/MacOS/Guitar Rig 6` (arm64 slice).
Read with `disasm.py`, and with `hopper.py` for control flow only. Dumps sit next to this file:
`ens.s`, `scf.s`, `fxunit_ctor.s`, `fxunit_set.s`, `ensau.s`, `sync.s`, `hopper.txt`.

Shell setup for every recheck command below:

```
export RE_STATE=/private/tmp/claude-501/-Users-tuba-Dev-projects-tubamp--claude-worktrees-tubamp-effects-reverse-engineer-90c7f7/cab46780-7fe6-4ba5-87cd-2d4971b7e79d/scratchpad/re
export RE=/Users/tuba/Dev/projects/tubamp/.claude/worktrees/tubamp-effects-reverse-engineer-90c7f7/tools/re
B="/Library/Audio/Plug-Ins/Components/Guitar Rig 6.component/Contents/MacOS/Guitar Rig 6"
D() { python3 $RE/disasm.py "$B" --max 100 "$@"; }
```

## 0. Shared facts

### 0.1 `NI::AB::ExpTable::m_fExpTable` (bss 0x47db080): the pitch/dB-to-linear table

- The table is filled at load time by `__GLOBAL__sub_I_unity_0_cxx.cxx` (0x6a9c1c). Entry 0 is 0x35800000 = 2^-20, and the loop calls `_exp2` on `i/60 + (-20)` (constants 60.0 = 0x404e000000000000 and -20.0 are in the code), so `v[i] = 2^(i/60 - 20)` for i = 0..2400. Each entry holds a (value, slope-to-next) float pair [decompiled: Guitar Rig 6 __GLOBAL__sub_I_unity_0_cxx.cxx 0x6a9c30-0x6a9cb0]. Recheck: `D --dump '__GLOBAL__sub_I_unity_0_cxx' | head -60`. The stub name `_exp2` for 0x27563dc comes from `objdump --macho --indirect-symbols`.
- Callers index it with linear interpolation, clamp at 2400 (0x960), and return 0 for a negative index [decompiled: Guitar Rig 6 RigEnsemble::setParamBass 0x292044-0x29208c].
- Index `5*p + 1381.8816` (0x44acbc36) gives `2^(p/12 + 3.0314) = 8.1758 * 2^(p/12)`, which is MIDI pitch p converted to Hz [inferred].
- Index `9.965784*dB + 1200` (0x411f73da and 0x44960000) gives `10^(dB/20)` [inferred].

### 0.2 Helpers named in the brief

- `processBuffer` and `processAudio` contain no `bl`/`blr` at all. Everything is inlined [decompiled: Guitar Rig 6 RigEnsemble::processBuffer 0x2c4340; ChorusFlangerTCSCF::processAudio 0x66e9e4].
- `LFOHelper`, `DelayLineVariableLength`, `DelayLineVariableSpeed` and `BBDSaturator` exist in the binary (0x60d1e4, 0x60d6b8, 0x69ced8, 0x60f154), but neither effect calls them, so I did not read them [decompiled]. Recheck: `D --dump 'RigEnsemble::processBuffer' | grep -cE '\sbl\s'` prints 0, apart from lines that belong to the next function.
- Neither effect has a BBD model. There is no clock-modulated delay, anti-alias or reconstruction filter, compander or noise source [decompiled: same two functions].

---

## 1. NI::DSP::RigEnsemble ("Ensemble" component)

The owning component is `NI::RIG::COMP::EnsembleAU::registerParameters` (0xe4530). It registers these parameters, all normalised to 0..1: Sync(TS, bool), Volume(Vol), Mode(Md, bool), Intensity(CI), Depth(VD), Rate(VR), Bass(B), Mid, Treble(Trb), Mix(Mx, displayed as percent), Lamp(L), Delay(Dly), Stereo(S, displayed as percent) [decompiled: Guitar Rig 6 EnsembleAU::registerParameters 0xe4554-0xe47f4; names read from the C strings at the x2/x3 addresses with `cstr.py`]. The Mode display callback is `ensemble_mode` (0xe4510). The Rate display callback `ensemble_vibrato_rate` (0xe41d4) repeats the `22v-16` pitch mapping given below [decompiled].

### 1.1 Object state (offsets into `this`)

`0x8` = fs. `0x80` = 1/fs. `0x84` = 2π/fs (`{1.0, 6.283}` at 0x2bdf260 divided by fs) [decompiled: onSetSampleRate 0x2c3adc-0x2c3af4]. Recheck: `D --floats 0x2bdf260 2`.

A `SmoothingToolbox<9>` sits at 0xe0. Current values are at 0xf0+4i, per-sample increments at 0x120+4i, targets at 0x150+4i. Names used below:

| slot | target addr | meaning |
|---|---|---|
| sm0 | 0x150 | delay offset (in units of sm1) |
| sm1 | 0x154 | delay scale, in samples |
| sm2 | 0x158 | gain into the delay write path, fixed at 1.0 |
| sm3 | 0x15c | g_bass - 1 |
| sm4, sm6 | 0x160, 0x168 | vol*(1-mix): gain of the filtered dry signal |
| sm5, sm7 | 0x164, 0x16c | vol*mix*4.0738: gain of the chorus signal |
| sm8 | 0x170 | bypass crossfade (1 on resume, 0 on suspend) |

`startSmooth` (0x313200) sets `inc = (target - cur) * [0x18c]` [decompiled]. The ramp length is `n = max(1, round(0.02*fs/blocksize))` blocks (0x3ca3d70a = 0.02), so every parameter moves as a linear ramp of about 20 ms. At the end of the ramp `onControlRate` snaps the values to their targets [decompiled: onSetSampleRate 0x2c3a84-0x2c3ac4; onControlRate 0x2937dc].

### 1.2 Delay voices and buffers

- There are two delay lines, one per channel (L buffer at 0x1b8, R buffer at 0x1d0), with one tap each. There is one voice per channel [decompiled: processBuffer 0x2c43f0-0x2c44bc].
- Buffer size is `2^(int(log2(0.012*fs)) + 1)`. That is 1024 samples at 44.1 and 48 kHz and 2048 at 96 kHz, and the mask is size-1 [decompiled: onSetSampleRate 0x2c3af8-0x2c3b34; 0x3c449ba6 = 0.012, 0x3f317218 = ln 2; the sizes are inferred].
- The write pointer decrements (`w = (w-1) & mask`). The read position is `w + floor(d)`, which is d samples older [decompiled: 0x2c43f0-0x2c4448].

### 1.3 Read interpolation

The read is linear with two taps, `y = b[i] + (b[i+1] - b[i])*frac`, and floor() handles negative values [decompiled: processBuffer 0x2c4418-0x2c448c]:

```
2c4418 ldr  s18,[x0,#0x254]      ; d = c1 (smoothed delay, samples)
2c442c fcvtzs w14,d19            ; floor(d)
2c4434 add  w12,w12,w14          ; i = w + floor(d)
2c443c ldr  s20,[x15,w14,sxtw#2] ; a = buf[i & mask]
2c4448 ldr  s21,[x15,w12,sxtw#2] ; b = buf[(i+1) & mask]
2c444c fsub s21,s21,s20 ; 2c4474 fmul s18,s21,s18 ; 2c448c fadd s18,s20,s18 ; a+(b-a)*frac
```

### 1.4 LFO

- Each channel has its own float phase accumulator, both in [-1,1): `ph += inc; if (ph > 1) ph -= 2`, where `inc = 2*f/fs` (0x1a0 and 0x1a4 hold the same value) [decompiled: processBuffer 0x2c43b0-0x2c43d0; setLfoF 0x2c41f0-0x2c4200].
- The waveform is a triangle, `tri0 = |ph0|` and `tri1 = |wrap(ph1 + [0xa0])|`, with range 0..1 [decompiled: 0x2c43cc-0x2c4404].
- Stereo comes from `setParamStereo`: `[0xa0] = 0.5*S` (0x292004). An offset of 0.5 phase units out of a period of 2 means the R LFO leads L by `90°*S`. The default is S = 0, which gives identical LFOs (onInit stores `0xa0 = 0` at 0x2c37e8) [decompiled; the degree conversion is inferred]. The stereo image does not use inverted wet or cross-mixing: L uses only the L input and R only the R input [decompiled: output stage, §1.8].
- `setLfoF(p)` converts a MIDI-pitch value to Hz through ExpTable [decompiled: setLfoF 0x2c4190-0x2c4204].
- Tempo sync (flag 0xc4) rounds `log2(f)` to an integer octave. If the leftover is more than 3 semitones it multiplies by 4/3 (0x3faaaaab); if less than -3 it multiplies by 0.75. It then scales by `2*[0xc0]`, and onInit sets `0xc0 = 2.0` [decompiled: setLfoF 0x2c406c-0x2c418c]. I treat 0xc0 as tempo in beats per second [guess].
- `displayLfoLamp` returns `0.65 + (ph0 > 0 ? 0.35 : -0.65) / max(1, f)`. The lamp is a square wave that dims above 1 Hz [decompiled: 0x2c4208; 0xbf266666 = -0.65, 0x3eb33333 = 0.35, 0x3f266666 = 0.65].

### 1.5 Delay-time law per mode (Mode = bool at 0xa4)

The delay in samples is `c1 = onepole(sm1*(sm0 + tri0), k11)`, and the same with tri1 for R. The value is computed every sample [decompiled: processBuffer, lane 1 of the A/C vectors, 0x2c4560 then 0x2c44c4 on the next sample]. The one-pole coefficient vectors are K1 = 0x1f0..0x1fc and K2 = 0x200..0x20c, and each filter computes `y += (x-y)*k` with `k = min(1, 2π f_c/fs)` (forward Euler).

**Chorus mode (Mode=0)**

- `k11 = 1.0`, so the delay time is not smoothed [decompiled: setParamMode 0x2933c0].
- `sm1 = 0.0026*fs`, which is 2.6 ms (0x3b2a64c3) [decompiled: 0x2933cc-0x2933e0].
- `sm0 = ([0xb8]-1)/2` with `[0xb8] = 2.6*Dly + 1` (0x40266666) [decompiled: setParamDelay 0x2922b0-0x2922ec].
- Delay = `2.6 ms * (1.3*Dly + tri)`. The sweep is a fixed 2.6 ms peak-to-peak. The centre is 1.3 ms at Dly=0, 2.99 ms at the default Dly=0.5, and 4.68 ms at Dly=1. The minimum is 0 ms at Dly=0 and 3.38 ms at Dly=1 [inferred from the above].
- The **Intensity** knob sets the LFO **rate**, not the depth. `setParamChorusIntens` computes pitch `= 17.31234*ln((2.9*I + 0.1)/440) + 69`, which is `12*log2(f/440) + 69`, so `f = 0.1 + 2.9*I` Hz, i.e. 0.1 to 3.0 Hz, linear. The default is 1.55 Hz [decompiled: 0x2932e4-0x293330; constants 0x4039999a = 2.9, 0x3dcccccd = 0.1, 0x43dc0000 = 440, 0x418a7fac = 17.3123, 0x428a0000 = 69; onInit stores pitch -28.7891 (0xc1e65014), which ExpTable maps to 1.55 Hz].
- The triangle gives a constant |dτ/dt| of `2*2.6e-3*f`. The pitch steps between +x and -x every half cycle: about ±0.9 cents at 0.1 Hz, ±9 cents at 1 Hz, ±14 cents at 1.55 Hz, and +26.8/-27.2 cents at 3 Hz [inferred].

**Vibrato mode (Mode=1)**

- Rate pitch `= 22*VR - 16`. That is f = 8.1758*2^(pitch/12): 3.24 Hz at VR=0, 6.12 Hz at 0.5 (the default, pitch -5), and 11.56 Hz at 1. The curve is exponential [decompiled: setParamVibratoRate 0x2931f8; onInit 0x2c3764 stores -5.0].
- Depth: `s = 1.26*VD + 0.09` (0x3fa147ae, 0x3db851ec), and `sm1 = s*0.002*fs` (0x3b03126f). That is 0.18 to 2.7 ms peak-to-peak [decompiled: setParamVibratoDepth 0x29321c-0x2932a8].
- Offset: `b0 = s < 0.5 ? 24 - 38*s : 6.6 - 3.2*s` (0xc2180000, 0xc04ccccd, 0x40d33333), which is continuous at s=0.5 where b0=5. Then `sm0 = (r*b0 - 1)/2`, where `r = 10^((10*Dly - 5)/20)`, i.e. Delay acts as ±5 dB scaling of the offset (setParamDelay 0x292228-0x2922d8) [decompiled].
- The minimum delay therefore stays near 1.7 to 2.4 ms, and the sweep grows with Depth [inferred]. Default (VD=0.5, Dly=0.5): about 2.37 to 3.81 ms before smoothing [inferred].
- `k11 = min(1, 1.6223 * 2π/fs)` (0x3fcfa787). The delay-time signal is **low-passed at 1.62 Hz** [decompiled: setParamMode 0x29337c-0x29339c; onSetSampleRate 0x2c3c9c-0x2c3cb4]. I checked this at both sites because it is surprising.
  - The smoothing rounds the triangle almost into a sine and cuts its amplitude by `1/sqrt(1+(f/1.62)^2)`: 0.45 at 3.24 Hz and 0.14 at 11.56 Hz [inferred].
  - Resulting peak deviation: about 10 cents at the default settings. At VD=1 it is 17 to 19 cents across the rate range; above about 3 Hz it is nearly independent of rate, because |H|*f is roughly 1.62 Hz [inferred; computed as 2π*f*(8/π²*pp/2)*|H|].
  - This looks like a CE-1-style RC-smoothed triangle [guess].

### 1.6 Wet-path filters (per channel, all one-pole forward-Euler, `k = min(1, 2π f/fs)`)

Constant cutoffs are ExpTable entries read at fixed offsets from 0x47db080, each with a fractional part. I converted each one to Hz with `2^((off/8 + frac)/60 - 20)`.

| stage | lane / coef addr | cutoff | source |
|---|---|---|---|
| input HP (x - LP) | c0 / 0x1f0 | 25.96 Hz (pitch 20) | onSetSampleRate 0x2c3bf8 (off 0x2e48, frac 0.88159) |
| LP after HP → "dry" e0 | e0 / 0x210 | 10548 Hz (pitch 124) | 0x2c3c28 (off 0x3e88) |
| low-shelf LP inside the shelf | c3 / 0x1fc | f_mid*2/(g_bass+1) | setParamBass 0x2920a8-0x2920d4, setParamMid |
| LP after the shelf | e2 / 0x218 | 4186 Hz (pitch 108) | 0x2c3c48 (off 0x3c08) |
| LP before the delay write | c2 / 0x1f8 | 14917 Hz (pitch 130) | 0x2c3c68 (off 0x3f78) |
| Treble LP on the delay output | e1 / 0x214 | 73.4 Hz to 23.7 kHz | setParamTreble 0x29218c |

All rows are [decompiled]. The physical-unit conversions are [inferred].

- **Bass** sets the shelf depth: `g = 10^((20*B - 20)/20)`, i.e. -20 to 0 dB (cut only). sm3 = g - 1 [decompiled: 0x292014-0x292098].
- **Mid** sets the shelf frequency: `f_mid = 8.1758*2^(((1-M)*80 + 57)/12)`. That is 220 Hz at M=1, 2217 Hz at M=0.5 (the default), and 22.35 kHz at M=0 [decompiled: setParamMid 0x2920dc-0x292118; 0x42a00000 = 80, 0x42640000 = 57].
- The signal written to the delay is `LP14.9k( LP4.19k( e0 + (g-1)*LP_fs(e0) ) )`, where `e0 = LP10.5k(x - LP26(x))` [decompiled: lane wiring in processBuffer 0x2c44a8-0x2c4574, sm2=1 set in onInit 0x2c39c0]. The shelf adds e0 delayed by one sample, which is a pipelining artefact [inferred].
- **Treble** sets the LP on the delay output: `f = 8.1758*2^((100*T + 38)/12)` (0x42c80000 = 100, 0x42180000 = 38). The default T=0.5 gives 1318.5 Hz [decompiled: setParamTreble; onInit 0x2c3888].
- There is no pre/de-emphasis pair and no saturation [decompiled].

### 1.7 Feedback

None. The delay write value comes only from the input chain. Neither delay output (e1/f1) feeds back [decompiled: processBuffer lane wiring].

### 1.8 Output, dry/wet and gain

- `L = in_L + (vol*((1-mix)*e0 + 4.0738*mix*e1) - in_L) * sm8`, and the same for R with f0/f1 [decompiled: processBuffer 0x2c4578-0x2c45a4; setParamMix 0x291f94-0x291fc8; 0x40825c92 = 4.0738].
- In normal use sm8 = 1. The **dry** signal inside the effect is therefore the filtered e0 (26 Hz to 10.5 kHz), not the raw input [inferred].
- **Mix** is a linear crossfade with the wet side boosted by +12.2 dB. This is not an equal-power law [decompiled].
- At Mix=1 the output is pure wet, which is vibrato [inferred].
- **Volume** is -15 to +15 dB: `vol = 10^((30v - 15)/20)` [decompiled: setParamVolume 0x291ea0-0x291f18].
- sm8 is the bypass fade: onResume sets it to 1 (0x2c4008) and onSuspend sets it to 0 (0x2c403c), with the 20 ms ramp [decompiled].

### 1.9 Click handling

- Every gain and the delay offset ramp over about 20 ms through SmoothingToolbox [decompiled].
- A mode switch changes the delay offset target (smoothed). It switches the k11 smoothing coefficient and the LFO rate at once, without a crossfade [decompiled: setParamMode]. An audible jump in delay slope is possible at the switch [inferred].
- The LFO phase is not reset on a parameter change. onReset zeros the phases and the filter states [decompiled: onReset 0x2c3ff0].

### 1.10 Mono compatibility

- With S=0 and a mono input, L and R are identical. They sum without any cancellation [inferred].
- With S>0 the two wets differ only in LFO phase (up to 90°). The sum averages two choruses. No wet signal is inverted, so nothing cancels [inferred].

---

## 2. NI::DSP::ChorusFlangerTCSCF (TC SCF model)

### 2.1 Owner and parameters

- The SCF is an `FXPLUGIN` DSP core embedded in `NI::DSP::FXFDelayFXUnit` at offset 0xa00. The FXFDelayFXUnit constructor stores the SCF vtable 0x44b5a70 at `[x19,#0xa00]` (0x60fe00) [decompiled].
- `FXFDelayFXUnit` belongs to `NI::DSP::FXFDelay` (Replika XT), which GR6 exposes as `NI::RIG::COMP::ReplikaGR` [decompiled: symbols; 14 direct calls from `FXFDelay::setParameter` at 0x67cbe0-0x67cd34 set param 4 on 14 SCF instances].
- No RIG::COMP class registers TCSCF parameters directly. The display names and ranges live in FXFDelay's parameter tables, which I did not read (FXFDelay::setParameter is 35 KB) [decompiled: absence of any TCSCF-named component].
- `FXFDelayFXUnit::setChorusMode` sets the target mode to 0 (0x6977e4). `setFlangerMode` sets it to 2 (0x6979f8). `setChorusMix` and `setFlangerFeedback` both write SCF param 2 with the same per-mode scalings as SCF::setParameter [decompiled].
- Mode 1 is "Pitch Modulation". The name comes from the string table and the dry-cancelling behaviour in §2.7 [inferred].

`setParameter(id, v)` uses a jump table at 0x37fd558 (bytes `00 05 0f 26 2b 3c a5 57`, base 0x66effc) [decompiled]:

| id | field | effect |
|---|---|---|
| 0 | 0x8 | Rate → `SyncHelper::setFrequency(double)` on SyncHelper@0x78. The LFO frequency in Hz lands in 0x88 (double) |
| 1 | 0xc | Depth: `0x174 = v²`, `0x168 = v² * {1.2, 2.8, 4.5}[mode]` (table at 0x38a7508) |
| 2 | 0x10 | Mix/feedback (mode dependent, §2.7) |
| 3 | 0x14 | `sm0 = 2v`: stereo spread (§2.8) |
| 4 | 0x18 | tempo sync on/off (> 0.5) |
| 5 | 0x1c | mode: v ≤ 1/3 → 0, ≤ 2/3 → 1, else 2 (0x3eaaaaab, 0x3f2aaaab) |
| 6 | - | no-op |
| 7 | 0x20 | delay offset: adds `0.5*v` to the delay multiplier |

Rate range: the FXFDelayFXUnit constructor sets the SCF SyncHelper maximum to 10.0 Hz (`fmov.2d v0,#10` stored at SCF+0xb0/0xb8). It sets the minimum to 0 and mapping mode 3 (`w26=3` stored at SCF+0xd0) [decompiled: FXFDelayFXUnit ctor 0x60fdb8-0x60fecc]. Mode 3 in `calcFreqFromInput` interpolates geometrically, in pitch, between min and max (0x60bdc4-0x60be74, 440 = 0x407b8...) [decompiled]. With min = 0 that formula is undefined, so the host must override min somewhere I did not find. No `setFrequencyRange` or `setInputMappingMode` call exists in the FXFDelay address range [decompiled: call-site grep]. **The effective Rate knob curve is not determined.**

### 2.2 Constants set at sample-rate time (onSetSampleRate 0x611408)

All constants here are [decompiled]. Recheck: `D --dump 'ChorusFlangerTCSCF::onSetSampleRate'` and `D --doubles 0x37fccf0 1; D --doubles 0x37fcc68 1`.

- `0x30` = fs. `0x38` = 2π/fs (double 6.283185 at 0x37fccf0).
- Max delay `0x44 = 0.05*fs`, i.e. 50 ms (0x3d4ccccd). The buffer holds `round(0.05*fs) + 10` floats (vector at 0x210).
- `0x40 = 472 * 2π/fs` (0x43ec0000) and `0x64 = 141 * 2π/fs` (0x430d0000). These are one-pole coefficients.
- `w = 25132.74/fs = 2π*4000/fs` (0x37fcc68). Then `0x58 = (1-w)/(1+w)` and `0x5c = 0.54*w/(1+w)` (0x3f0a3d71).
- The smoothing ramp count `0x1f8 = max(1, round(0.02*fs/block))`, which is about 20 ms, the same as the Ensemble.

### 2.3 Per-mode constants (applyTargetMode 0x66e5dc)

All constants here are [decompiled].

| mode | base delay [0x34] | depth gain (0x38a7508) | LFO filter cap | input gain sm3 | p2 → |
|---|---|---|---|---|---|
| 0 chorus | 4.4 ms (0x3b902de1) | 1.2 | 3.28 Hz (0x4051eb85) | 0.5 | sm5 = 0.9*p2 (0x3f666666) |
| 1 pitch mod | 1.39 ms (0x3ab630aa) | 2.8 | 2.0 Hz | 0.5 | sm6 = 1.85*p2 (0x3feccccd) |
| 2 flanger | 1.39 ms | 4.5 | 2.0 Hz | 0.425 (0x3ed9999a) | sm4 = 0.64*p2 (0x3f23d70a) |

- Defaults before the per-mode write: sm4 = 0.04, sm5 = 1, sm6 = 1, sm2 = 1 (doubles {0.04, 1.0} at 0x37fcff0).
- `sm1 = 1/(1 + 0.64*p2)` in flanger mode and `1/1.04` otherwise (0x3d23d70a = 0.04).

### 2.4 Delay voices, buffer, interpolation

- There is one delay line with one tap, fed by a mono sum. The input is `x = sm2*sm3*(inL + inR) + 1e-20` (0x1e3ce508 is the denormal guard) [decompiled: processAudio 0x66ea9c-0x66eab8].
- Delay in samples = `fs * base * (1 + lfo + 0.5*p7)`, clamped to the range 1 to 0.05*fs [decompiled: 0x66eae8-0x66eb14].
- The read is a **4-tap cubic Lagrange** read. The coefficients are the standard ones for taps at offsets -1, 0, +1, +2 [decompiled: 0x66eb98-0x66ebf0]:
  - `c1 = (-2a - 3b + 6c - d)/6`
  - `c2 = (3a - 6b + 3c)/6`
  - `c3 = (-a + 3b - 3c + d)/6`
  - `y = b + f*(c1 + f*(c2 + f*c3))`
- The "a" tap, however, is fetched at **i-2**, not i-1: `sub w7, w4, #0x2` at 0x66eb3c. I checked this three ways: with disasm.py, with `otool -tv`, and in Hopper's pseudo-code (`r7 = r4 - 0x2`, hopper.txt line 1055) [decompiled].
  - The result is still exact at integer positions [inferred: the a-weights cancel at f=0 and f=1].
  - Between samples there is an error of `-(y[i-2] - y[i-1]) * f(1-f)(2-f)/6`, with a peak weight of 0.064 [inferred].
  - For a sine that is roughly -41 dB at 1 kHz and -28 dB at 5 kHz relative to the signal, at fs = 48 k [inferred].
  - The other taps (i, i+1, i+2) wrap only at the top end (`>= len`). The i-2 tap can go negative near index 0 [decompiled; the out-of-range read is inferred].
- The write pointer decrements, and the write happens after the read [decompiled: 0x66ec34-0x66ec44].

```
66eb34 add  w4, w13, w4     ; i = write + floor(d)
66eb38 add  w5, w4, #0x1    ; i+1
66eb3c sub  w7, w4, #0x2    ; i-2   <- used as the "-1" tap
66eb40 add  w19, w4, #0x2   ; i+2
66eb98 fmul s31,s27,s16     ; -2*a     (s16 = -2.0)
66eb9c fmul s8, s28,s17     ;  3*b     (s17 =  3.0)
66eba4 fmul s9, s29,s18     ;  6*c     (s18 =  6.0)
66ebb0 fdiv s31,s31,s18     ; c1/6 ... Horner at 66ebdc-66ebf0
```

### 2.5 LFO

- The phase is a **double** accumulator in [-1,1). It advances by `[0x140] = 2*f/fs` per sample, set in setParameter at 0x66f24c-0x66f258 [decompiled: processAudio 0x66ecc8-0x66ed14].
- A phase offset `[0x148]` is added before shaping; tempo-sync position handling sets it [decompiled: 0x66ed2c-0x66ed40].
- An optional glide (0x150/0x158/0x160) re-aligns the phase to the host position over N samples [decompiled: 0x66ecb0-0x66eccc].
- Shape: a **square wave** `sq = ±1` (0x170). It toggles at both the wrap and the zero crossing of the phase [decompiled: 0x66ed44-0x66ed6c, 0x66ea58-0x66ea60].
- The square becomes `(sq + 1.11)*[0x168]` (0x3f8e147b = 1.11) and then passes through **two cascaded one-pole low-passes** with `k = (2π/fs)*min(f_lfo, cap)` [decompiled: 0x66ea64-0x66ea98; the cap appears in setParameter 0x66f25c-0x66f28c and applyTargetMode 0x66e720-0x66e750].
  - The corner sits at the LFO frequency itself, so the output is near-sinusoidal. The fundamental is 0.637 of the square amplitude, the 3rd harmonic is 6.7% and the 5th is 1.5% [inferred].
  - Above the cap (3.28 or 2 Hz) the amplitude falls as `1/(1+(f/cap)^2)` [inferred].
- Because of the 1.11 offset the delay modulation is always positive. It sits at a mean of 1.11*k*D² above the base and swings ±0.637*k*D² at low rates [inferred].
- Depth enters as **D²**, a quadratic taper [decompiled: setParameter 0x66f014-0x66f170].
- There is one LFO and one voice. There are no per-channel LFO phases [decompiled].

Delay ranges (D = depth param, p7 = 0, f below the cap) [inferred]:

| mode | D=0.5: mean, ±amp | D=1: mean, ±amp | cents at D=1 |
|---|---|---|---|
| chorus | 5.87 ms ±0.84 | 10.26 ms ±3.36 | 18 @0.5 Hz, 36 @1 Hz, 72 @2 Hz |
| pitch | 2.47 ms ±0.62 | 5.71 ms ±2.48 | 27 @1 Hz, 53 @2 Hz, 37 @5 Hz |
| flanger | 3.13 ms ±1.00 | 8.33 ms ±3.98 | 43 @1 Hz, 85 @2 Hz |

p7 adds `0.5*p7*base`: up to +2.2 ms in chorus and +0.7 ms in the other modes [inferred].

### 2.6 Signal flow per sample (processAudio 0x66ea9c-0x66ec78)

All lines in the block are [decompiled]:

```
x    = sm2*sm3*(L+R) + 1e-20
lp   = 0.54 * LP1_bilinear(x)          ; y = b(x+x1) + a*y1 (0x50..0x5c)
s25  = sm5*lp
dry  = x - sm6*s25                     ; s24
D    = lagrange_read(delay)            ; §2.4
s22  = sm1*(dry + D)
h1   = HP141(sm4*s22)                  ; x - onepole state (0x60, k=0x64)
h2   = HP472(s25 + h1)                 ; (0x3c, k=0x40)
buf[w] = -2.3 * h2                     ; 0xc0133333 = -2.3
outL = (1-sm2^2)*x + sm2*s22
outR = (1-sm2^2)*x + sm2*(s22 + sm0*(dry - s22))
```

- The pre-delay LP is a bilinear one-pole whose K is `2π*4000/fs`, with no prewarp. Its effective -3 dB point is about `fs/π*atan(2π*4000/fs)`: 7.4 kHz at 48 k and 7.3 kHz at 44.1 k. Its DC gain is 0.54 [inferred from the coefficients in §2.2].
- The wet path is high-passed at 472 Hz (one-pole) and the feedback at 141 Hz. The delay write is **inverted** and scaled by 2.3 [decompiled].

### 2.7 Mode behaviour, feedback and dry/wet law (in-band, below about 7 kHz where lp ≈ 0.54x) [inferred unless noted]

- **Chorus (0):**
  - dry = (1 - 0.486*p2)*x. Wet = -1.118*p2*x(t-τ), above 472 Hz.
  - p2 is a mix control: it takes low-passed signal out of the dry path and feeds it to the delay. Dry and wet are equal at p2 ≈ 0.62.
  - There is a constant small feedback: loop gain -2.3*0.04/1.04 = **-0.088**.
- **Pitch mod (1):**
  - dry = (1 - 0.999*p2)*x. At p2=1 the in-band dry cancels and the output is pure vibrato. Wet = -1.242*x(t-τ).
  - Above about 7 kHz the dry signal returns, because lp rolls off.
  - Feedback is the same -0.088.
- **Flanger (2):**
  - dry ≈ 0.46x and wet ≈ -1.24x(t-τ).
  - Feedback through HP141/HP472 has gain `-2.3*0.64*p2/(1+0.64*p2)`, i.e. 0 to **-0.898**. It is negative, has no limiter and no saturation [decompiled for the loop structure].
  - The output is normalised by sm1 = 1/(1+0.64*p2). The input gain drops to 0.425 [decompiled].
- Dry is always present and never crossfaded away, except for the in-band cancellation in mode 1. The "mix" is a subtraction of a low-passed copy, not a crossfade [inferred].

### 2.8 Stereo and mono compatibility

- The input is summed to mono first [decompiled].
- `outL = sm1*(dry + D)`. `outR = (1-2*p3)*sm1*(dry + D) + 2*p3*dry` [decompiled, written with sm0 = 2*p3]:
  - p3 = 0: R = L, a mono output.
  - p3 = 0.5: R is the dry path only.
  - p3 = 1: R ≈ 1.04*dry - 0.96*D, so the wet is **inverted on R** [inferred].
- At p3 = 1, `L + R ≈ 2*dry`, so the wet cancels when the outputs are summed to mono [inferred]. This is the classic SCF "L = dry+wet, R = dry-wet" image.
- The FXFDelayFXUnit constructor leaves p3 = 0 and p7 = 0 (0xa14 and 0xa20 zeroed at 0x60fe18-0x60fe1c). I did not find where Replika sets them [decompiled for the zeros only].

### 2.9 Smoothing and clicks

- sm0..sm7 ramp linearly over about 20 ms (`processControlRate` 0x66ed94; per-sample increments at 0x1b0) [decompiled].
- **Mode switch:** setParameter(5) sets a pending flag (0x2c) and a target `sm2 = 0`. sm2 scales both the input and the wet, so the output fades to silence over about 20 ms. When the ramp ends, `processControlRate` calls `applyTargetMode`. That changes the base delay, the depth gain and the coefficients, then ramps sm2 back to 1 [decompiled: 0x66f2b8-0x66f2c4, 0x66efa0-0x66efb8].
- While suspended (0x2d), targets jump without a ramp [decompiled: onSuspend 0x611118, setParameter tail 0x66f298].
- The FXFDelayFXUnit mode setters skip the fade. They zero the filter states and the delay pointer, then call applyTargetMode directly [decompiled: setChorusMode 0x697714-0x6977f4].
- Depth changes are smoothed only by the two LFO low-passes, whose corner is the LFO rate, about 1 s at 0.16 Hz. The delay-offset p7 is not smoothed [inferred].

---

## 3. Could not determine

- How Replika XT maps its UI knobs onto SCF params 0, 3 and 7: the Rate range and curve in Hz, whether Width/p3 is ever non-zero, and the delay-offset knob.
- SCF parameter display names and units, which live in FXFDelay's parameter metadata (not read).
- Whether the i-2 Lagrange tap is a bug or deliberate. The arithmetic is certain; the intent is unknown.
- Ensemble tempo-sync BPM scaling (field 0xc0, set by onBPMChanged, not read in detail).
- No black-box measurements were made. This was a static read only.
