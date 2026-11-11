# AmpliTube 5 chorus effects: static reading notes

Binary: `/Library/Audio/Plug-Ins/Components/AmpliTube 5.component/Contents/MacOS/AmpliTube 5` (arm64 slice). All
addresses are arm64 VM addresses. Commands assume:

```
export RE_STATE=.../scratchpad/re ; export RE=<repo>/tools/re
B="/Library/Audio/Plug-Ins/Components/AmpliTube 5.component/Contents/MacOS/AmpliTube 5"
T="$RE_STATE/thin/16735ea0f50a35a3-AmpliTube 5"      # thin slice; __TEXT vmaddr == file offset
W=$RE_STATE/work/chorus/at5
```

Helpers written in `$W` (scratch, not repo tools): `rd.py` (read string/float/double at a VM address),
`xref.py` (ADRP+ADD/LDR xref scanner over `__text`), `mafdec.py` / `dec.py` / `sinit.py` (heuristic string/constant
recovery). Raw dumps: `maf.txt`, `ce1.txt`, `ac_maf.txt`, `ac_pre.txt`, `mxr.txt`, `mxr_pre.txt`, `lfolin.txt`,
`at2chorus.txt`, `lc.txt`, `lfo.txt`, `dl.txt`, `chst.txt`, `fender.txt`, `fmaf.txt`, `terc.txt`.
Hopper was not used: the daemon queue was saturated by other agents (4000+ AT5 targets queued); all reading is
from `disasm.py`. libm stub names come from `otool -Iv "$T"` (`$W/stubs.txt`): 0x2165af0 `__exp10f`,
0x21663f0 `powf`, 0x21667ec `tanf`, 0x2165b38 `__sincosf_stret`, 0x2165d0c `exp2f`, 0x2165d18 `expf`, 0x2166618 `sinf`.

## 0. How an effect reaches its DSP

Each `IK::Comp::<Effect>::MAFProfile::MAFProfile()` passes a MAF module name to `RackBase/StompBase::MAFProfile`
and then registers UI-parameter-index -> MAF-parameter-name mediators, each with a `FloatConverter` (Norm, Linear,
PowNorm, dB) that maps the UI range to the engine range [decompiled: AmpliTube 5 `IK::Comp::*::MAFProfile::MAFProfile` 0x959f4c, 0x9663ec, 0x98b424, 0x9a0568, 0xa30670, 0xaa5688].
`FloatConverterLinear::Convert(x) = c + (d-c)(x-a)/(b-a)` with fields (a,b,c,d) at +8..+0x14 [decompiled: AmpliTube 5 `FloatConverterLinear::Convert` 0x6e6ddc].

| Effect | MAF name (address of literal) | DSP class that implements it |
|---|---|---|
| RackAnalogChorus | "AnalogChorusRack" (0x31e1929) | `IK::MAF::AT3::AnalogChorus` = `DynamicWrapperStereo<MeteredFXStereo<KIS::FX::AT4::ChorusCE1::Stereo>,2>` |
| RackDigitalChorus | "Chorus" (immediates at 0x966408) | `AudioEngine::AT2::Chorus<ChorusTraits>` via `ModuleSelector<ChorusFxSet>` (Stereo/Mono) |
| StompChorus | "Chorus" (immediates at 0xa30690) | same AT2 Chorus graph |
| RackFenderTriangleChorus | "FenderChorus" (0x31e1985) | `AudioEngine::AT2Fender::FenderChorus` wrapping `ChorusStereo<AT2::Chorus>` |
| RackFulltoneChorus | "FulltoneTriStereoChorus" (0x31e356d) | `IK::MAF::AT3::FulltoneTriStereoChorus` -> `KIS::FX::AT4::FullToneTERC::Module` |
| StompMXRStereoChorus | "MXRStereoChorus" (0x31e432f) | `IK::MAF::AT3::MXRStereoChorus` -> `KIS::FX::ATIP::MXRStereoChorus::Module` (mono; dual-mono in stereo) |

The name->class link is by exact name match plus parameter-name match (the MAFProfile handles equal the DSP class's
parameter names) [inferred]; the registry (`AT25::StompStereoSet::StompStereoSet` 0x10812f8, xref of 0x31e1929 at
0x1086694) uses a folded template `ModuleMap::Add` so the creator type is not visible at the call [decompiled: AmpliTube 5 0x1086694].
The AnalogChorus vtable (0x34c49b0) is preceded by the typeinfo of `DynamicWrapperStereo<MeteredFXStereo<ChorusCE1::Stereo>,2>` (0x34c4b70) and its `ProcessBuffer` calls `ChorusCE1::Stereo::Process` directly [decompiled: AmpliTube 5 `IK::MAF::AT3::AnalogChorus::ProcessBuffer` 0x152a0d0].

Not used by any of the six: `DelayLinePcrControlled`, `FilterComb`, `LinearComb2<FilterCombTraits...>` (the comb is
the flanger family; Pcr is MemoryMan only, template `<MemoryManTraitsDetails::DelayLineTraits,25,500>`) [decompiled: AmpliTube 5 symbol names].

## 1. AudioEngine modules (AT2 family)

**Lfo / LfoExporting / LfoFunctions.** Three shapes exist, registered by `FlangerTraits::Lfos::Lfos()` in the order
"Triangle", "Sine", "Square" [decompiled: AmpliTube 5 `AudioEngine::AT2::FlangerTraits::Lfos::Lfos` 0x10e35b0].
Sine: per sample `out = sinf(phase * k)` with `k` at +0x130 (2*pi) and `phase += inc` (+0x144), wrapped by -1 when >1; bipolar ±1 [decompiled: AmpliTube 5 `LfoExporting<LfoFunctions::Sine>::ProcessBuffer` 0x10e6478].
Triangle: `out = 1 - 4*|phase - 0.5|`, bipolar ±1, same accumulator [decompiled: AmpliTube 5 `LfoExporting<LfoFunctions::Triangle>::ProcessBuffer` 0x10e70cc].
"Square" is a natural cubic spline through a square-ish point set (`SplineCubic<...CurveSquare>`) [decompiled: AmpliTube 5 symbol names 0x10e3da0..0x10e5764]; its points were not read.
`inc = rate / fs` [decompiled: AmpliTube 5 `LfoExporting<Sine>::SetParameter` 0x10e5d08]. The LFO also exports (phase+1)/2 clamped to [0,1] as "LfoPosition" (for the GUI) [decompiled: AmpliTube 5 0x10e6518].
The LFO is computed per sample (audio-rate control) [decompiled: as above].

**LinearComb1<ChorusTraitsDetails::Scale,Offset>** ("LfoRescaler"): `out[n] = Offset(n) + Scale(n) * in[n]`.
Scale range 0..8, default 1; Offset range 0..50, default 12 [decompiled: AmpliTube 5 `LinearComb1<...ChorusTraitsDetails...>::DefineParameters` 0x10e29cc (fmov 8.0, 1.0), 0x10e2a74 (0x42480000 = 50.0, default 12.0)].
Both are smoothed once per block by a one-pole with coefficient `c = 1 - (fs/N) / (fs/N + pi*tau)` and then linearly ramped across the block, so knob moves do not click [decompiled: AmpliTube 5 `LinearComb1<...>::ProcessBuffer` 0x10e3140..0x10e3254].
Units: the output feeds DelayLineVariable's delay port, which is in ms, so Offset is base delay (ms) and Scale is the
peak excursion (ms) [inferred].

**DelayLineVariable<DelayLineVariableTraits>** ("DelayLine"): per-sample delay input in ms; `pos = -delay*fs/1000`;
`y = x[i]*(1+frac) - x[i-1]*frac` with `frac = pos - trunc(pos)` (≤0): plain 2-tap **linear interpolation** [decompiled: AmpliTube 5 `DelayLineVariable::ProcessBuffer` 0x10e22a4..0x10e2370].
The chorus instance is built with max delay 58.0 (ms) = 50 + 8 [decompiled: AmpliTube 5 `AT2::Chorus::DefineSubModules` 0x10dfb9c, `mov x8,#0x404d000000000000`].

**LinearComb2<ChorusTraitsDetails::MixerOut...>** ("MixerOut"): `out = A1*In1 + A2*In2`, A1 and A2 each 0..1, default 0.5 [decompiled: AmpliTube 5 `LinearComb2<...MixerOut...>::DefineParameters` 0x10e1260; `ProcessBuffer` 0x10e1aa8].

**DelayLinePcrControlled** (for completeness, used by MemoryMan): writes input, keeps a base length `b` (samples)
that glides linearly toward a new target `t = ms*fs/1000` (step stored at +0x184 = `1 - b_old/t`), and reads at
`pos = -b * ctrl[n]` where `ctrl` is a per-sample input that multiplies the length; linear interpolation as above [decompiled: AmpliTube 5 `DelayLinePcrControlled::Process(float,float,float*)` 0x11d95b0; `SetParameter` 0x11d79a0..0x11d79ec].
"Pcr" therefore behaves like a clock-ratio-controlled read: delay = base length × control ratio, which is how a BBD's
delay scales with 1/clock [inferred]. There is no BBD filter, noise or compander inside the module [decompiled: same].

**FilterComb, LinearComb1<FlangerTraits...>**: flanger family only; not read in detail [guess: not needed for chorus].

## 2. AT2 Chorus graph (StompChorus, RackDigitalChorus, and Fender's wrapped chorus)

Sub-modules, in `DefineSubModules`: "Lfo" (`ModuleSelector<FlangerTraits::Lfos>`), "LfoRescaler" (LinearComb1),
"DelayLine" (DelayLineVariable, 58 ms), "MixerOut" (LinearComb2) [decompiled: AmpliTube 5 `AT2::Chorus<ChorusTraits>::DefineSubModules` 0x10dfb14].
Wiring: Lfo.Out -> LfoRescaler.In -> DelayLine.InDelay; In -> DelayLine.In; DelayLine.Out -> MixerOut.In1; In -> MixerOut.In2 [decompiled: AmpliTube 5 `AT2::Chorus::ConnectSubModules` 0x10dfd34..0x10e002c, port-name immediates].
So: **one voice, no feedback, no filters**, `y = A1*wet + A2*dry`, `delay_ms(t) = Offset + Scale*lfo(t)`, lfo ∈ [-1,1] [inferred from the above].
Exposed parameters: Lfo.Rate (range 0.08..20, from the pool at 0x317d840 = {0.08, 20.0}), Lfo.Shape, Lfo.Phase,
Lfo.CurrentPosition, LfoRescaler.Scale as "Depth", LfoRescaler.Offset as "Bias", and "Level" (0..1, default 0.5) [decompiled: AmpliTube 5 `AT2::Chorus::DefineParameters` 0x10df018; `disasm.py "$B" --floats 0x317d840 2`].
How "Level"/"Mix" are mapped onto A1/A2 was not found (no OnSetParameter; done through linked-parameter descriptors) [guess: crossfade].

Stereo: `ChorusStereo<Chorus>` is `ModuleDualMono` with two full Chorus instances "L" and "R", linked parameters,
a multi-linked "Mix", and a "RelativePhase" parameter 0..1 default 0.5; `CompleteBuild` sets it to 0.5 [decompiled: AmpliTube 5 `ChorusStereo::DefineParameters` 0x10ddccc (0x10ddf20/24), `CompleteBuild` 0x10de360].
So the default stereo method is **two voices with LFOs 180° apart (antiphase)**, each channel its own wet+dry mix [inferred: RelativePhase is in cycles].
`ChorusFxSet` offers "Stereo" (ChorusStereo) and "Mono" (ChorusFxMono, which forces Fx.Level = 1.0 and mixes in PostFxMono) [decompiled: AmpliTube 5 `ChorusFxSet::ChorusFxSet` 0x10fce14; `ChorusFxMono::CompleteBuild` 0x10fdf18].

Mono compatibility (stereo mode): L+R = A2·2·dry + A1·(wetL+wetR); with antiphase LFOs the two wets have opposite
pitch offsets, so the sum keeps chorus character but partially cancels the pitch wobble [inferred].

### 2a. StompChorus
Mediators: idx2 "Rate" = PowNorm (constant 0.3 at 0x317af00) then Linear (0,1)->(0.1,5) Hz (pool 0x31803d0);
idx3 "Depth" Linear (0,10)->(0,4) ms (pool 0x31803e0); idx4 "Level" Norm (0,10) (0x317a6c8); idx7 "Shape" two-state [decompiled: AmpliTube 5 `IK::Comp::StompChorus::MAFProfile::MAFProfile` 0xa306c8..0xa308fc; `disasm.py "$B" --floats 0x31803d0 4`, `--floats 0x31803e0 4`, `--floats 0x317af00 2`].
"Bias" is not mediated, so the base delay stays at the Offset default 12 ms [inferred]. Delay = 12 ± (0..4) ms.
Rate law: UI^? power-normalised with exponent-like constant 0.3 then linear to 0.1..5 Hz [inferred; exact PowNorm formula not read].
Pitch (sine, 4 ms): ±4 cents at 0.1 Hz, ±43 at 1 Hz, ±205 at 5 Hz; triangle about 2/pi of that [inferred: cents = 1200 log2(1 + 2*pi*f*S)].
Which Shape index is sine vs triangle: not determined (map is keyed by name).

### 2b. RackDigitalChorus
Mediators: "ChorusShape" (2 states), "Rate" (no converter: raw Hz into Lfo.Rate, so 0.08..20 Hz), "Depth" Linear
(0,100)->(0,4) ms (pool 0x31801d0), "Mode", "Mix" Norm(0,100), "OutLevel" dB [decompiled: AmpliTube 5 `IK::Comp::RackDigitalChorus::MAFProfile::MAFProfile` 0x9663ec; `disasm.py "$B" --floats 0x31801d0 4`].
Base delay: Offset default 12 ms (not mediated) [inferred]. Same graph as 2a.

## 3. RackAnalogChorus = KIS::FX::AT4::ChorusCE1 (Boss CE-1 style)

Signal flow (`ChorusCE1::Stereo::Process` 0xdf93a0): mono sum `m = 0.5*(L+R)` into a scratch buffer; `wet = Module::Process(m)`;
`out0 = mix*wet + (1-mix)*m` then `out0 *= 1.5 - |mix - 0.5|`; if StereoMode: out1 = out0, else out1 = dry `m`; both × Gain [decompiled: AmpliTube 5 `ChorusCE1::Stereo::Process` 0xdf93a0..0xdf97d4].
`mix` (+0xac) and Gain (+0xa8) are initialised to 1.0 and no parameter writes mix [decompiled: AmpliTube 5 `DynamicFactory<MeteredFXStereo<ChorusCE1::Stereo>,2>::Preallocate` 0x152aae4 (`str d9`, d9 = {1,1}); `AnalogChorus::OnSetParameter` 0x152a1e0].
So out0 is **100 % wet**, and with StereoMode off out1 is the **dry** signal: the CE-1 "A = effect, B = direct" two-output scheme; the mix happens acoustically or on the mono sum [inferred].
Mono sum (StereoMode off) = wet + dry, i.e. a 50/50 chorus; with StereoMode on, both outputs are wet only (vibrato-style) [inferred].

Mono core (`ChorusCE1::Module::Process` 0xdf9004), per sample [decompiled: AmpliTube 5 0xdf9004..0xdf9394]:
1. LFO. Mode flag +0xc: nonzero -> sine path, zero -> triangle path. Sine: 11th-order Taylor polynomial with range
   reduction (2π 0x40c90fdb, π/2, π, 3π/2 0x4096cbe4; coeffs -1/6 0xbe2aaaab, 1/120 0x3c088889, -1.98e-4 0xb9500d01, 2.75e-6 0x3638ef1d, -2.5e-8 0xb2d7322b), phase in cycles (+0x38) incremented by +0x40. Triangle: `1 - 4|p-0.5|`, phase +0x24, inc +0x2c.
2. Control value V = base + depth·amp·lfo (sine: +0x50, +0x4c, +0x34; triangle: +0x48, +0x44, +0x20; amp = 1.0 at init 0x152aab8/0x152aac4).
3. V goes through the 9-point table `ChorusCE1::gcV2TCurve` at 0x30c6a2c (x from -7.5 to 22.5 in 3.75 steps, flat
   extrapolation), linearly interpolated, giving delay in ms: 1.138 ms for V≤0, 2.943 ms for V≥15 (end points;
   `disasm.py "$B" --floats 0x30c6a2c 9`; table parameters {inv-step 0.2667, x0 -7.5, xend 22.5} at 0x3186e60) [decompiled: AmpliTube 5 symbol `IK::KIS::FX::AT4::ChorusCE1::gcV2TCurve`].
   This models the BBD: LFO voltage -> VCO clock -> delay; the end values fit a 512-stage BBD at ≈225 kHz and ≈87 kHz clock [inferred: N/(2 f_clk)].
4. `samples = ms * fs * 0.001` (0x3a83126f); write, read at integer + frac, **linear interpolation**.
5. Buffer = next power of two above 3 ms × fs (0x3b449ba6) [decompiled: AmpliTube 5 `Stereo2::SetSampleRate` 0xdf9a6c; same formula in Preallocate].
No pre/post BBD filters, no compander, no noise, no clipping, no feedback in this module [decompiled: whole Module::Process read].

Parameter setters (`IK::MAF::AT3::AnalogChorus::OnSetParameter` 0x152a1e0; names from static init 0x212953c) [decompiled]:
- "Gain": `+0xa8 = expf(v * 0.11512925)` = 10^(dB/20); range ±15 dB (DefineParameters 0x152962c).
- "ChorusInt" (0..1, def 0.5): triangle rate `+0x28 = 1 + 2v` Hz; depth `+0x44 = 11*0.5998186 = 6.598`; base `+0x48 = 4.4694 (0x408f0529)` (the depth expression is `11v + 11(1-v)`, i.e. constant).
- "VibRate" (0..1): sine rate `+0x3c = 12.5R + 3.4(1-R)` Hz (3.4..12.5 Hz); sine depth `+0x4c = (2.5R + 11(1-R))·D·0.5998186`.
- "VibDepth" (0..1): base `+0x50 = D·0.3437985·13 = 4.469·D`, depth as above.
- "Mode": `+0xc = (v == 0)` -> Mode 0 = vibrato (sine), Mode 1 = chorus (triangle).
- "StereoMode": `+0xb0 = (v != 0)`.
UI side: ChorusInt and VibDepth Norm(0..100), VibRate Norm(0.1..7.9) (display units only), Gain dB, Meter [decompiled: AmpliTube 5 `IK::Comp::RackAnalogChorus::MAFProfile` 0x959f4c; `--floats 0x317abf0 2`].

Derived numbers [inferred, slope of gcV2TCurve ≈0.123–0.131 ms/V in 0..11 V]:
- Chorus: V spans 4.469 ± 6.598 = -2.13..11.07 V -> delay 1.138..2.538 ms (clamped flat at 1.138 ms for the ~16 % of
  the cycle where V<0), centre ≈1.84 ms. Triangle -> constant pitch offset: ±6 cents at 1 Hz (Int 0), ±12 at 2 Hz, ±18 at 3 Hz (Int max).
- Vibrato (D=1): R=0: 3.4 Hz, ±30 cents; R=0.5: 7.95 Hz, ±43 cents; R=1: 12.5 Hz, ±25 cents (depth shrinks with rate,
  keeping pitch excursion roughly constant). The bottom of the sweep also hits the 0 V clamp at low R.
No smoothing of any setter: values jump at block boundaries [decompiled: setters write fields directly].

## 4. StompMXRStereoChorus = KIS::FX::ATIP::MXRStereoChorus::Module (mono)

Per block (`DynamicWrapper<MXRStereoChorus::Module,0>::ProcessBuffer` 0x1477c4c) [decompiled]:
1. LFO: `IK::KIS::TK::LFOLinearInterp::Process` (0x1041354): integer-period phase counter, phase/period ∈ [0,1)
   looked up in `MXRStereoChorusData::cLFOSineTable` (0x31083dc, 105 floats, linear interpolation); the table equals
   `0.5 + 0.5 sin(2πp)` to float precision (max error 0.0) -> unipolar sine [decompiled + check script in this session].
2. `delay_samples = lo + lfo*(hi - lo)` (lo +0x124, hi +0x128), computed per sample.
3. Delay line: buffer = next pow2 above 15.2 ms·fs (0x3c79096c); linear interpolation [decompiled: 0x1477d20..0x1477d60; `SetSampleRate` 0xf8d4a4].
4. Wet -> first-order "Bass" shelf (+0x28..+0x34) -> biquad "Treble" bell (+0x38..+0x50), both TDF-II.
5. Dry: copied, or (when "BassPad" ≠ 1) through a first-order shelf (+0x54..+0x60).
6. `out = dry + wet * g` (g at +0x18) -> first-order output shelf (+0x64..+0x70).
No feedback.

Constants (`Module::SetSampleRate` 0xf8d204 and `IK::MAF::AT3::MXRStereoChorus::OnSetParameter` 0x178b6d4; names "Intensity", "Width", "Rate", "Bass", "Treble", "BassPad" from static init 0x214954c) [decompiled]:
- Rate r (0..1): `f = 0.1 + 9.9 r^6` Hz (powf 6, 0x411e6666 = 9.9, 0x3dcccccd = 0.1); period stored as integer samples, phase counter kept modulo the new period (no phase jump).
- Width w and rate together: `lo = 0.55 ms·(4.5 + 2.75·√w·4^r)`, `hi = 0.55 ms·(4.5 + (23 - 10r)·√w)`, ×fs (0x3a102de0 = 0.00055 s). At w=0 both = 2.475 ms.
- Intensity I: `g = 0.13 + 0.385·√I` (0x3f451eb8 = 0.77, ×0.5; 0x3e051eb8 = 0.13); default 0.13 at init.
- Bass b: `H(s) = (s + ωa)/(s + ωb)`, ωa = 2π·200·10^(0.75b), ωb = 2π·200·10^(-0.75b), bilinear K = 2fs, no prewarp: unity at HF, +30·b dB at DC.
- Treble t: analog RBJ-style peaking `(s²/ωc² + s·A/(Qωc) + 1)/(s²/ωc² + s/(AQωc) + 1)`, A = 10^(24t/40), Q = 0.3 (0x3e99999a),
  fc = min(0.45 fs, 6500 Hz) prewarped with tanf; peak gain +24·t dB.
- BassPad dry shelf: one-pole at ω = 691.15/fs (110 Hz) via `α = (1 - sin ω)/cos ω`; H = LP + 0.3162·HP: 0 dB at DC, -10 dB above ~110 Hz.
- Output shelf: `(s + 2114.66)/(s + 2987.03)` (0x44842a82×2, 0x44bab083×2), bilinear K = 2fs: -3 dB below ~340 Hz, 0 dB above ~475 Hz.
UI: Intensity/Width/Rate/Bass/Treble Norm(0..10) -> 0..1 [decompiled: AmpliTube 5 `IK::Comp::StompMXRStereoChorus::MAFProfile` 0xaa5688].
Module defaults: Bass 0.5, Treble 0.5, g 0.13, Width 0.5, Rate 0.5 (0x318a830, d9 = {0.5,0.5}) [decompiled: `DynamicFactory<MXRStereoChorus::Module>::Preallocate` 0x14788f8..0x1478900].
The bass/treble gains being boosts at default 0.5 (+15 dB LF wet, +12 dB at 6.5 kHz) is surprising; the arithmetic was read twice, but it should be confirmed by measurement [inferred].

Pitch deviation (w=1, sine of amplitude (hi-lo)/2) [inferred]: r=0: 0.1 Hz, delay 3.99..15.13 ms, ±6 cents;
r=0.5: 0.25 Hz, 5.5..12.4 ms, ±9.5 cents; r=0.75: 1.86 Hz, 6.75..11.0 ms, ±42 cents; r=1: 10 Hz, 8.5..9.6 ms, ±59 cents.

Stereo: the module is mono. Stereo slots instantiate `ModuleDualMono<IK::MAF::AT3::MXRStereoChorus, ModuleDualMonoDetails::LinkParameter>` (two linked copies) [decompiled: AmpliTube 5 symbol 0x12931b0]. Both copies start their LFO counter at 0 with identical parameters, so L and R are identical (no stereo spread, perfectly mono compatible) [inferred]. No inverted-wet second output was found.
No parameter smoothing: lo/hi and filter coefficients are recomputed and applied immediately [decompiled: OnSetParameter].

## 5. RackFenderTriangleChorus = AT2Fender::FenderChorus

`FenderChorus` is a wrapper: one sub-module "WrappedChorus" = `ChorusStereo<AT2::Chorus>` (two linked Chorus voices
L/R) plus its own Rate 0.08..20 (default 1) and Depth 0..8 (default 1) parameters linked down [decompiled: AmpliTube 5 `AT2Fender::FenderChorus::DefineSubModules` 0x10dcaec; `DefineParameters` 0x10dc880 (0x3da3d70a = 0.08), 0x10dc928].
UI mediators (Linear converters, pools read with `--floats`) [decompiled: AmpliTube 5 `IK::Comp::RackFenderTriangleChorus::MAFProfile` 0x98b424]:
- "Mix" (1..10) -> 0.1..0.5 (0x3180270)
- "Rate" (0.08..10) -> 0.18..10 Hz (0x3180280)
- "Depth" (1..10) -> 0..3 ms Scale (0x3180290)
- "Bias" (1..10) -> 8..25 ms Offset (0x31802a0)
- "RelativePhase" (1..10) -> 0..0.5 cycles (0x3180250)
- "LfoPosition" (GUI), "OutLevel" (dB).
So: two voices, delay = (8..25) ± (0..3) ms, LFO offset between L and R from 0 to 180°, wet share 0.1..0.5 [inferred].
Triangle LFO: the effect name says triangle and the selector offers "Triangle" first, but the code that forces Shape
for this effect was **not found** [guess: Triangle]. With triangle at 3 ms: ±21 cents at 1 Hz, ±61 cents at 3 Hz [inferred].
No BBD filters, feedback or saturation (pure AT2 graph) [decompiled: graph in §2].

## 6. RackFulltoneChorus = KIS::FX::AT4::FullToneTERC (Fulltone Choral / Tri-Stereo)

From `FullToneTERC::Module::Process` 0xe2da68 [decompiled unless tagged]:
- Input: mono `(L+R)/2`, prefiltered (biquad states at +0x1c8..) and written into **three** delay lines.
- LFO A (phase +0x140, rad, inc +0x144): sinf at θ, θ+2π/3 (0x40060aa6), θ+4π/3 (0x40860aa6) -> three-phase,
  120° apart. Per voice k: `cA_k = I_k·(depthA·sin_k + 6.5) + (1 - I_k)·3.6` (6.5 fmov, 0x40666666 = 3.6), with I_k = IntensityLeft/Center/Right (+0x10/+0x14/+0x18).
- LFOs B and C (phases +0x150, +0x160; same 120° spacing): `cB_k = 6.5 + 0.8913·dB·sinB_k + 0.1087·dC·sinC_k` (0x3f642c86, 0x3dde9bd3).
- Each control path is one-pole filtered (states +0x174.., +0x184..), gated by flags +0xd/+0xe, then `c_k = cA_k + cB_k`.
- `c_k` -> per-voice V2T lookup tables (+0xc8..+0xe0, +0xf0..+0x108, +0x118..+0x130; same scheme as CE-1) -> ms -> ×(+0x4) samples; linear interpolation.
- Each wet voice passes a cascade of first-order + biquad filters (+0x1f0..+0x2e8) (BBD anti-alias/reconstruction style) [inferred: coefficients from `UpdateFilters` 0xe2d640 not decoded].
- Output: `L = R = dry_filtered·(+0x30)`; Stereo on (+0xc): `L -= 1.5·g·(v1+v2)`, `R -= 1.5·g·(v2+v3)`; Stereo off: both `-= 1.5·g·(v1+v2+v3)`; then both × (+0x2c). g at +0x34.
  -> wet is **polarity-inverted** relative to dry; centre voice shared, outer voices hard-panned.
- Speed s (0..1): `rateA = 0.0265 + 10.9735·s^4.877` Hz (powf; 0x3cd91687, 0x412f9375, 0x409c1062), or tempo-synced (clamped 0.0265..11 Hz); B and C increments come from fixed rates at +0x148/+0x158 [decompiled: `IK::MAF::AT3::FulltoneTriStereoChorus::OnSetParameter` 0x16b1274..0x16b13ec]. Values of depthA/B/C and rateB/C (set at init) were not read.
- Mono sum (stereo): 2·dry − 1.5g(v1 + 2v2 + v3): no cancellation of wet voices, centre weighted [inferred].
Parameters: Stereo, Preset/Manual, IntensityLeft/Center/Right, Speed (UI Norm 0..10) [decompiled: static init 0x213d5bc; MAFProfile 0x9a0568].

## 7. Brief: flangers (not read in depth)
The flanger family uses `LinearComb1<FlangerTraits::Scale,Offset>`, `FilterComb<FilterCombTraits>` (feedback comb with
`LinearComb2<Feedback, Dry>`) and the same LFO selector [decompiled: symbol names]; constants not read.

## Comparison table

| Effect | Voices | Base delay | Depth | Rate | LFO | Stereo | Feedback | Filters | Mix law |
|---|---|---|---|---|---|---|---|---|---|
| RackAnalogChorus (CE-1) | 1 | V2T curve 1.14–2.94 ms; chorus ctrl 4.47 V (≈1.84 ms centre) | chorus 1.14–2.54 ms fixed; vib ≤ ±6.6 V scaled by depth, shrinks with rate | chorus 1–3 Hz (Intensity); vib 3.4–12.5 Hz | triangle (chorus), sine (vibrato) | out A wet / out B dry, or both wet | none | none (V2T curve only) | 100 % wet out A; dry on B |
| StompMXRStereoChorus | 1 (dual-mono copies) | lo 2.5–8.5 ms | hi−lo up to 11.1 ms (w,r dependent) | 0.1 + 9.9 r^6 Hz | sine (table) unipolar | none (L = R) | none | wet LF shelf (+30b dB @200 Hz), wet bell 6.5 kHz Q0.3 (+24t dB), dry pad, out shelf | dry 1 + wet 0.13..0.515 |
| RackFenderTriangleChorus | 2 (L/R) | 8–25 ms (Bias) | 0–3 ms | 0.18–10 Hz | triangle [guess] | L/R LFO offset 0–180° | none | none | wet share 0.1–0.5 (exact law not found) |
| StompChorus | 1 or 2 (Mono/Stereo set) | 12 ms default Offset | 0–4 ms | 0.1–5 Hz (pow-law) | Shape selectable (Tri/Sine/Square) | antiphase (RelativePhase 0.5) | none | none | A1·wet + A2·dry (Level mapping not found) |
| RackDigitalChorus | 1 or 2 | 12 ms default | 0–4 ms | 0.08–20 Hz (raw) | ChorusShape selectable | antiphase | none | none | Mix 0–100 % + OutLevel |
| RackFulltoneChorus | 3 | ≈6.5 / 3.6 ctrl units -> V2T tables | per-voice intensity crossfade | 0.0265–11 Hz (s^4.88) + 2 fixed LFOs | 3-phase sine (120°) ×3 LFOs | L = v1+v2, R = v2+v3, wet inverted | none | BBD-style pre/post filter cascades | dry + (−1.5g)·wet |

## Not determined
- Which Shape index selects sine vs triangle in the AT2 chorus; whether FenderChorus forces Triangle.
- The mapping from AT2 "Level"/"Mix" to MixerOut A1/A2.
- StompChorus PowNorm exact formula (constant 0.3 at 0x317af00 read, formula not).
- Fulltone: depthA/B/C, rateB/C, V2T table contents, filter coefficients (`UpdateFilters` 0xe2d640).
- Whether StereoMode in the AnalogChorus rack is exposed in the UI (MAFProfile maps only Gain, ChorusInt, VibDepth, VibRate, Meter by the heuristic decoder; Mode/StereoMode mediators may exist but were not resolved).
