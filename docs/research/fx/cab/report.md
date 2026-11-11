# Cabinet block: reference study and improvement spec

## Verification status

Independently verified on 2026-11-11: 20 of 20 claims verified, 4 with a scope caveat. The full table is in `verification.md`.
Read these corrections before relying on the numbers below. The body of the report is unchanged.

- The measured tubamp binary is the component in ~/Library/Audio/Plug-Ins, identical to the main checkout's build and not built from this worktree. The CabSim source is the same, and a numpy model of JUCE's loader matches the Audio Unit to -96 dB.
- The 2.5 dB loss at 14 kHz reproduces only on a synthetic IR with a 14 kHz burst added. On a plain cab IR the resampling error is at most 0.25 dB below 6 kHz. "Sounds duller" is not shown.
- The level change between 48 and 96 kHz is 3.0 dB. The quiet-file effect is at most 0.22 dB per third-octave for a file 40 dB down.
- The default-IR pass-through (-2.30 dB) occurs only on a first load issued after prepare and lasts roughly 70 to 80 ms (estimate).
- R 7.1: the -9 dB energy-normalised rule contradicts the finding that references use fixed trims, rests on TONEX-derived stand-in IRs, and gives the same 1.07 dB spread as today. The NonUniform CPU figure is unmeasured.
- R 7.2: the need for a speaker-load stage is asserted without a source. R 7.3: auto-align has no precedent in the references and was tested only on pre-aligned IRs.
- Five Summary lines overstate, including "every reference is zero latency" and "all three model impedance between amp and cab".

Binaries: GR = `Guitar Rig 6`, AT = `AmpliTube 5`, T3 = `TONE3000`, TX = `TONEX`, all under
`/Library/Audio/Plug-Ins/Components/<name>.component/Contents/MacOS/`. J = `build/_deps/juce-src/modules/juce_dsp/frequency/juce_Convolution.cpp`
(JUCE 8.0.15), RAS = `.../juce_audio_basics/sources/juce_ResamplingAudioSource.cpp`. Evidence files are in `evidence/`;
scripts in `evidence/bb/` and `evidence/proto/`. L1..L8 are the sources listed in `evidence/literature.md`.
"L2" of an impulse response means `10*log10(sum h^2)`.

## 1. Summary

- tubamp's cab is `juce::dsp::Convolution` (zero latency, uniform partitions) with JUCE's own loader. The loader resamples with linear interpolation, loses 2.5 dB at 14 kHz on a 44.1 kHz file, and makes the cab 3.1 dB louder at 96 kHz than at 48 kHz [measured: evidence/ours_resampler_bright.tsv, evidence/ours_level.tsv].
- Every reference keeps the convolution at zero latency with a short direct FIR head or a zero-latency FFT scheme; none normalises user IRs by energy. They use a fixed trim: -18 dB (TONE3000, NAM), -12 dB (Guitar Rig), -20 dB default (AmpliTube) [decompiled: T3 0x411bc, GR 0xb35dc, AT 0x154eac4; literature: L1].
- Guitar Rig, AmpliTube and TONEX all model the speaker's impedance between amp and cab: a bass resonance and a treble rise. TONEX exposes it as CAB RESONANCE: 10.3 dB of range at 109 Hz [decompiled: GR 0x2db578, AT 0xf1aba8] [measured: evidence/tonex_blend.txt].
- Multi-mic blends are done on the impulse responses, with explicit time offsets; TONEX's two mic IRs are aligned within 1.7 samples and blend as an exact linear mix [decompiled: GR 0xb2cec] [measured: evidence/tonex_blend.txt].
- Proposal, in order: a new loader (sinc resampling, rate-independent level, relative trim), a speaker-load stage (Resonance, Presence), a second IR with automatic alignment, 24 dB/oct cuts. Old sessions keep today's loader bit for bit.

## 2. tubamp today

Source: `src/dsp/FxBlocks.h:170-192`, `src/dsp/FxBlocks.cpp:614-690`, J, RAS.

| aspect | what we ship | where |
| --- | --- | --- |
| engine | `juce::dsp::Convolution` default constructor = `Latency{0}`: zero latency, uniform partitions | `FxBlocks.h:187`; J:1207, J:740-745 |
| partition | `blockSize = nextPowerOfTwo(maximumBlockSize)`; `fftSize = 2*blockSize` above 128, else `4*blockSize`; each segment holds `fftSize - blockSize` IR samples; `segments = N/(fftSize-blockSize) + 1` | J:161-165 |
| work per call | one forward and one inverse FFT on every `processSamples` call, plus `segments` complex multiply-adds once per block. A host that calls with less than the maximum block pays the two FFTs per call [inferred from J:241-306] | J:229-307 |
| channels | always two engines; a mono IR is used for both | J:464-476 |
| load call | `loadImpulseResponse(file, Stereo::yes, Trim::yes, 0, Normalise::yes)` | `FxBlocks.cpp:656-660` |
| length | the whole file (`size == 0`); no cap, no fade | J:718-719 |
| trim | both ends cut where `abs(sample) < 10^(-80/20)`, an absolute threshold; the smaller offset of the two channels is used | J:573, J:571-621 |
| resampling | `ResamplingAudioSource`: linear interpolation between two samples plus one 2nd-order Butterworth low-pass at `0.5*min(1, ratio)` of the rate | J:653-674; RAS:163, RAS:210-220 |
| normalisation | after resampling, every channel is multiplied by `0.125 / sqrt(max over channels of sum h^2)`: L2 = -18.06 dB at the session rate | J:623-651, J:787-790 |
| IR change | the file is read and the engine built on JUCE's background thread; the audio thread installs it when ready and crossfades linearly over 50 ms with both engines running; a further change waits for the fade to end | J:941-1016 (0.05 at J:951), J:1055-1076, J:1059 |
| before the first engine arrives | the factory's default IR: one sample of 1.0 declared at 44.1 kHz, not normalised, so it is resampled and scaled by `44100/fs` | J:805-815, J:792 |
| cut filters | `StateVariableTPTFilter` high-pass then low-pass after the convolution, resonance `1/sqrt(2)`: 12 dB/oct each | `FxBlocks.cpp:620-626, 687-688`; JUCE `juce_StateVariableTPTFilter.h:169` |
| parameters | `cab_on`; `cab_lowcut` 20..500 Hz (default 80); `cab_highcut` 2000..20000 Hz (default 8000), log skew | `src/Parameters.cpp:53, 91-92` |
| bypass | when `cab_on` is off or no IR is loaded the whole block is skipped, cut filters included, with no fade | `src/PluginProcessor.cpp:1381-1385` |
| latency | none added; the cab is not in the latency sum | `src/PluginProcessor.cpp:1724-1740` |
| thread safety | `irLoaded` and `irName` are plain members written by the message thread and read by the audio thread | `FxBlocks.h:189-190`, `FxBlocks.cpp:663, 670, 684` |
| full-rig captures | `gear_type` from the NAM metadata is matched for `cab`, `mic`, `full_rig`; the UI shows a rig label and a double-cab warning when such a model and the cab block are both in the path. Nothing is bypassed automatically | `src/dsp/NamEngine.cpp:184-187, 416`; `ui/src/hooks/useRigCab.ts:30-45`; `ui/src/features/panel/library.ts:56` |

Measured on the installed AU with the cab alone in the chain (`chainOrder="cab"`, cuts at 20 Hz and 20 kHz):

- Latency is zero: the output peak of a unit-impulse IR is at the input sample [measured: evidence/ours_level.tsv, row delta_ir].
- Every IR comes out at L2 = -18.06 dB, whatever the file level [measured: evidence/ours_level.tsv].
- The same IR gives -18.07 dB at 1 kHz in a 44.1 kHz session, -17.6 dB at 48 kHz and -14.5 dB at 96 kHz [measured: evidence/ours_level.tsv, rows cab44_s44100, cab44_s48000, cab44_s96000]. The cause is the L2 rule: sampling the same response twice as densely halves the gain the rule applies per sample but doubles the number of taps [inferred from J:623-651].
- A file scaled by -40 dB gives a different response: +0.6 dB at 1 kHz, because the absolute trim threshold cuts its tail 40 dB earlier [measured: evidence/ours_level.tsv, row cab48_quiet].
- 240 samples of leading silence in the file are removed [measured: evidence/ours_level.tsv, row cab48_predelay].
- A 44.1 kHz file in a 48 kHz session is 0.5 dB too loud below 2 kHz and 1.8 dB down at 12 kHz, 2.5 dB at 14 kHz, 3.7 dB at 16 kHz against an ideally resampled copy; the time-domain error is -17 dB. A 96 kHz file has a -10 dB time-domain error [measured: evidence/ours_resampler_bright.tsv].
- For the first moments after a load the cab passes the dry signal through the default IR: -2.3 dB at 48 kHz, -7.2 dB at 96 kHz, 0 dB at 44.1 kHz [measured: evidence/ours_default_ir_transient.tsv].
- Cut filters: -3.0 dB at the set frequency, -12.3 dB one octave below the low cut [measured: evidence/ours_cuts.tsv].
- CPU, both channels, share of one core: 0.14 % for a 2229-sample IR at 512-sample blocks, 0.49 % at 32; a 2 s IR costs 0.84 % at 512 and 16.1 % at 32 [measured: evidence/ours_cpu.tsv].

## 3. Reference implementations

### 3.1 Guitar Rig 6

Details and excerpts: `evidence/gr6_convolution.md`, `evidence/gr6_cab_mixers.md`.

**Engine (`NI::DSP::Convolver`).** Zero latency: a 128-tap direct FIR runs on every sample and the rest of the IR goes through non-uniform FFT partitions [decompiled: GR NI::DSP::Convolver::processBuffer 0x619fec..0x61a020]. The schedule comes from a 283-row table keyed by IR length: 64-sample partitions only up to 1920 samples; 64x10 + 256xN from 2048 to 6912; then stages of 1024 or 2048, 8192 or 16384, up to 1048576; the limit is 2^26 samples [decompiled: GR NI::DSP::Convolver::PartitionArray 0x464b598]. Each stage starts at three times its block size [inferred from the table]. The FFT is NI's own split-radix code and the work of the larger stages is spread over slices of at most 64 samples [decompiled: GR 0x61714c, 0x658488].

**IR management (`ConvolutionBase`).** Two convolvers. A new IR is built on a background thread, set on the idle convolver, and the two are crossfaded over 20 ms rounded to whole blocks [decompiled: GR NI::DSP::ConvolutionBase::onSetSampleRate 0x2699e0, doBackgroundWork 0x26a3b4]. IRs are converted to the convolver rate with NI's `SampleRateConverter` at quality 2 (8-point, 5th-order polynomial interpolation) and multiplied by `irRate/convolverRate` [decompiled: GR 0x26a5a0, 0x26a674, 0x245e4ec]. When the rack runs 2x oversampled the convolver runs at half that rate with a down and an up converter around it [decompiled: GR 0x269a40..0x269a74].

**Cab signal path (`ConvolutionCabBase::processBuffer`).** Input summed to mono (`(L+R)*0.5`) unless Stereo is on; convolution; "Air"; volume; bypass crossfade [decompiled: GR 0x26bc10]. Air is eight delay taps from 15 ms to 45 ms in a 1.17 ratio through two one-pole filters [decompiled: GR NI::DSP::Air_GR::onSetSampleRate 0x25c254, 0x25c268].

**Level.** No measurement of the IR. Factory cabs use fixed trims (-18 dB and -27 dB for the two IRs of Matched Cabinet Pro [inferred from GR 0x1d3228]); the 8-slot mixer applies `-20*log10(N) - 12 dB` for N active slots [decompiled: GR 0xb35cc..0xb35e0]. A "Learn" button lowers the volume over 5 s until the output peak stays under 0.501 (-6 dBFS) [decompiled: GR 0x26c068, 0x269b40].

**Multi-mic mixing (`ControlRoomCustom8`, also the user IR Loader).** Up to eight IRs are summed into one stereo IR before convolution. Per slot: fader (x2), linear pan (`L = 2*f*(1-pan)`, `R = 2*f*pan`), polarity, a time offset of `phase * 0.04 m / 343.2 m/s` applied with a 4-point cubic fractional delay, and a room IR level [decompiled: GR NI::DSP::ControlRoomCustom8::calcIR 0xb302c..0xb31a8]. Bass and treble shelves are run over the summed IR, so they cost nothing at audio rate [decompiled: GR 0xb36f0..0xb3724]. There is no automatic alignment; the offset is a player control named Phase, shown in cm [decompiled: GR 0xc07dc]. The IR Loader reads user files as one channel at full length [decompiled: GR NI::RIG::COMP::IRLoaderAU::loadIRFromFile 0xc1260].

**Parameters.** IR Loader: Gain, Pan, Mute, Solo, Invert, Phase, Room and Room Mute for slots 1..8, plus Learn, Volume, Air, Bass, Treble [decompiled: GR NI::RIG::COMP::IRLoaderAU::registerParameters 0xc09ec, strings 0x4249bca, 0x424a282]. Matched Cabinet: Cabinet, Crossfader. Value ranges were not recovered (they sit in adaptor objects, not in the registration call).

**Speaker impedance.** A cab tells the amps upstream which impedance record to use; every amp class has `setCabinetImpedance(int, const float*)` [decompiled: GR NI::DSP::ConvolutionCabBase::tellAmpImpedance 0x26b5f8]. The amp turns the record into two biquads and one first-order section by bilinear transform [decompiled: GR NI::DSP::RigPlexi::setCabinetImpedance 0x2db578]. There are 27 records: a resonance at 30..120 Hz with Q 2.6..15, a second one at 18..630 Hz, and a shelf with its zero at 0.7..1.5 kHz and its pole at 1.6..3.9 kHz [decompiled: GR NI::DSP::RigCabinet::s_CabImpedancePZ 0x2c43644]. How the amp applies the filter (inside or after the power stage) was not read.

**Older cabinet (`RigCabinet`).** Pole/zero filter models converted to biquads, with mic type, position, distance, size, room, bass, treble, pan, phase and learn [decompiled: GR 0x2c266c, symbol list]. Internals not read.

No nonlinear speaker stage was found in the convolution cab path [decompiled: GR 0x26bc10].

**Measured headless** (rack with one component, gate off; section 4.9). An empty rack delays the signal by 16 samples; Control Room Pro's response peaks 4 samples after that, so the convolver adds no block latency [measured: evidence/gr6_blackbox.tsv, rows mc_off, crp_s1]. Factory IRs last 43 to 45 ms (t60 2064 and 2156 samples at 48 kHz) [measured: evidence/gr6_blackbox.tsv]. The response is the same within 0.1 dB at 44.1, 48 and 96 kHz [measured: evidence/gr6_blackbox.tsv, rows mc_c0_*]. Control Room Pro's Phase at full scale delays a slot by 5.4 samples at 48 kHz, which is the 0.04 m / 343.2 m/s of the code; lower settings follow a curve, not a line [measured: evidence/gr6_blackbox.tsv]. PhaseInv flips the sign with no delay; Pan fully left gives +6.0 dB left and silence right; Bass at maximum adds 14 dB at 60 Hz [measured: evidence/gr6_blackbox.tsv]. Matched Cabinet's Crossfade at 0.5 is exactly half of each end response (residual -139 dB), and its Air control lengthens the response from 43 ms to 188 ms and makes the two channels differ [measured: evidence/gr6_blackbox.tsv]. Five Matched Cabinet models span L2 -8.0 to -1.1 dB at the default Master Volume [measured: evidence/gr6_blackbox.tsv]. Matched Cabinet Pro and the IR Loader were not measured: their factory files carry no component id or do not exist. Cabinets & Mics in its INIT state passes the signal unchanged.

### 3.2 AmpliTube 5

Details: `evidence/at5_cab_notes.txt` (helper notes, spot-checked), `evidence/at5_impedance.md`. Static only: the plugin crashes in its constructor under the headless host.

**Engine (`IK::KIS::TK::Conv`).** Chosen from IR length and a zero-latency flag: under 8192 samples, uniform partitions; from 8192, non-uniform (FFT 512, 1024, 2048, 4096, 8192). With the zero-latency flag, a direct FIR of one block replaces the first partition [decompiled: AT IK::KIS::TK::Conv::IR::IR 0x102bc78..0x102bd4c]. The cab asks for zero latency with block 128: 128 direct taps plus 15 partitions of 128 with a 256-point vDSP FFT [decompiled: AT IK::MAF::AT3::Cabinet::OnSampleFrequencyChange 0x154b768, 0x154b76c].

**IR length.** 2048 samples below 88.2 kHz, 4096 from 88.2 kHz: 43 to 46 ms [decompiled: AT 0x154a9e0..0x154a9f8]. Custom IRs are cropped to 2048, 4096 or 8192 samples by file rate (up to 48 kHz, up to 96 kHz, above), with no fade and no normalisation [decompiled: AT IK::Products::AT5::CustomIRLoader::Utils::CreateCroppedBuffer 0x1bde8c..0x1bdec0].

**Resampling.** Factory IRs once at load (`ResamplerOneShotHQ`); custom IRs through a 32-tap, 64-phase Blackman-Nuttall windowed-sinc rational resampler [decompiled: AT 0x10365c4, IK::KIS::TK::Convolver::ConvolverAutoResampling2::Initialize 0x102fac4].

**Microphones.** Two per cab, each with its own convolver. Position is not a filter: each mic position is a bilinear mix of four measured IRs from a grid of up to 15 x 5 per speaker, mic and region, mixed in the time domain; a non-zero angle blends a second set by `|angle|/180` [decompiled: AT MicPlacementMixer::Generate 0xf17ed8..0xf18084]. The farther mic is delayed by `(d - d_min) * 0.01 / 340 * fs` samples: physical delay, not alignment [decompiled: AT Cab::TryUpdateFlexiCabIR 0xf120b4..0xf120e4]. Per mic: Model (18), X, Y (-1..1), Distance (0..1), Speaker (4), Angle (2), Level (0..6), Pan, Phase invert [decompiled: AT IK::MAF::AT3::Cabinet::DefineParameters 0x154c9a8..0x154cb70].

**Level.** No normalisation; a per-cabinet dB trim table, and 1/N for N speakers [decompiled: AT 0x30fc6c0, 0xf11f6c]. Custom IR Volume -201..+6 dB, default -20 dB [decompiled: AT 0x154eac4].

**Custom IR controls.** HiPass 20..500 Hz and LowPass 5..20 kHz, each bypassed at its end stop; Phase invert; Size 0.75..1.25; Tilt -10..+10 dB; Delay 0..25 ms; Volume [decompiled: AT 0x154ea18..0x154eac4, 0xf13698].

**Room.** Two convolvers with 8192-sample room IRs; Width, Pan, Level, Phase, Solo [decompiled: AT 0x1550f10, 0x154de30..0x154dee4].

**Speaker impedance.** A separate Impedance module between amp and cab, re-set whenever the amp or the cab changes [decompiled: AT IK::Products::AT4::ImpedanceModuleManager::OnCabAParameter 0x3c8ad8]. It builds a 4-state (or 6-state) continuous model from circuit values and discretises it with a bilinear transform [decompiled: AT IK::KIS::FX::AT5::ImpedanceFilter::Module::UpdateImpedance1Left 0xf1aba8]. There are 103 records, one per cab model; the first three read as nominal 16, 16 and 8 ohm, voice-coil 12.5, 10.2 and 5.3 ohm, and an L-C pair resonating at 67, 60 and 78 Hz [decompiled: AT gcImpedanceVector 0x3171fe8] [inferred: unit reading].

**Switching.** The IR is rebuilt in place and set; no two-engine crossfade was found in this path [decompiled: AT 0xf125d4]. No nonlinear speaker stage in the cab path [decompiled: AT, helper notes section 7].

### 3.3 TONE3000

Details: `evidence/tone3000_ir.md`.

- `juce::dsp::Convolution`, default (uniform, zero latency) for IRs up to 1 s; `NonUniform{8192}` above [decompiled: T3 TONE3000Processor::prepareBlockModelOffThread 0x690c8..0x69408].
- `loadImpulseResponse(file, Stereo::no, Trim::yes, 10 s, Normalise::no)`; a second object with `Stereo::yes` when the file has two channels [decompiled: T3 0x69418..0x69428, 0x69738..0x69748].
- Level: a fixed -18 dB on IR blocks, and a +-24 dB block output gain; the UI text says "IR pre-trimmed -18 dB" [decompiled: T3 0x411bc].
- The convolution is built, prepared and run once on a helper thread before the block joins the chain [decompiled: T3 0x69440, 0x6ab7c].
- In an oversampled chain the IR runs at the base rate between a down and an up cascade [decompiled: T3 0x40d54..0x40db0].
- No IR blend, no alignment, no cab-specific cut filters were found; blocks have a general EQ [decompiled: T3 symbol list: setBlockEqBand 0x55ee8].
- Capture types are labelled in the UI ("Amp + Cab" for `amp-cab` and `full-rig`); no automatic cab bypass was found [decompiled: T3 strings].

### 3.4 TONEX

No symbols; strings in `evidence/tonex_strings.md`, measurements in section 4. The VIR engine is also in AmpliTube 5 as `IK::KIS::FX::ToneX::CabVIRLight` [decompiled: AT 0xfb9198].

- The whole plugin runs at 44.1 kHz inside: at 44.1 kHz a bypassed chain is a pure 7-sample delay; at 48 kHz and 96 kHz the impulse is band-limited and later [measured: evidence/tonex_rates.tsv].
- VIR IRs are 2048 samples at 44.1 kHz (46 ms) [decompiled: AT IK::KIS::FX::ToneX::CabVIRLight::IRPool::LoadIR 0xfb9308..0xfb933c] [measured: evidence/tonex_rates.tsv, t60 = 2053].
- Two mics (three models), each with X and Z, one Blend, one Resonance; Presence and Depth are a power-amp EQ [decompiled: TX strings 0xc43869] [measured: evidence/tonex_cab_params.tsv].
- Blend is an exact linear mix of the two mic responses [measured: evidence/tonex_blend.txt].
- Mic IRs are time-aligned: moving a mic from Z = 0 to Z = 1 moves the response by 1.1 samples [measured: evidence/tonex_blend.txt].
- The IRs are close to minimum phase but not converted to it [measured: evidence/tonex_blend.txt].
- Custom IRs: the UI asks for WAV, 44.1 kHz, 16-bit mono [decompiled: TX string 0xc5827a].
- Cab type is MODEL (the cab inside the capture), VIR or OFF [measured: evidence/tonex_cab_params.tsv].

### 3.5 Plugin Alliance (black-box only)

The amp cannot be switched off, so the cab is not isolated. Ampeg SVTVR Classic reports zero latency; its two recording chains start 7 and 9 samples later than the empty chain. bx_rockrack V3 Player reports zero latency and exposes only presets [measured: evidence/pa_blackbox.tsv].

### 3.6 NadIR, Emissary

No arm64 slice. Skipped.

## 4. Measurements

All at 48 kHz unless the row says otherwise. Commands are in the first lines of each file.

### 4.1 tubamp: level against session rate and file (evidence/ours_level.tsv)

| IR file | session | L2 dB | gain at 1 kHz dB |
| --- | --- | --- | --- |
| unit impulse | 48 k | -18.93 (cuts at 20 Hz / 20 kHz remove 0.87 dB) | -18.06 |
| cab IR stored at 44.1 k | 44.1 k | -18.06 | -18.07 |
| same | 48 k | -18.06 | -17.55 |
| same | 96 k | -18.06 | -14.49 |
| cab IR stored at 48 k | 48 k | -18.06 | -17.73 |
| same file scaled -40 dB | 48 k | -18.06 | -17.14 |
| stereo impulse, R = 0.5 L | 48 k | L -18.93, R -24.95 | -18.06 |

### 4.2 tubamp: resampler error (evidence/ours_resampler_bright.tsv)

| Hz | 1 k | 4 k | 8 k | 10 k | 12 k | 14 k | 16 k | time-domain residual |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 44.1 k file, dB | +0.53 | +0.19 | -0.47 | -0.71 | -1.83 | -2.51 | -3.72 | -17.0 dB |
| 96 k file, dB | +0.13 | +0.05 | +0.29 | -0.57 | +1.09 | -0.23 | -1.03 | -10.4 dB |

### 4.3 tubamp: CPU, percent of one core, two channels (evidence/ours_cpu.tsv)

| IR samples | block 32 | 64 | 128 | 512 | 1024 |
| --- | --- | --- | --- | --- | --- |
| 2229 (46 ms) | 0.49 | 0.32 | 0.24 | 0.14 | 0.17 |
| 9600 (200 ms) | 1.45 | 0.68 | 0.42 | 0.20 | 0.19 |
| 24000 (0.5 s) | 3.45 | 1.47 | 0.75 | 0.31 | 0.25 |
| 48000 (1 s) | 6.68 | 2.74 | 1.30 | 0.49 | 0.34 |
| 96000 (2 s) | 16.11 | 5.76 | 2.43 | 0.84 | 0.49 |

Offline render on Apple Silicon, wall time minus the cab-off baseline. The plugin's fixed cost sets a floor near 0.1 to 0.2 %.

### 4.4 TONEX: latency and rate (evidence/tonex_rates.tsv)

| case | rate | onset | energy centroid | L2 dB | t60 samples |
| --- | --- | --- | --- | --- | --- |
| model off, cab off | 44.1 k | 7 | 7.0 | 0.00 | 8 |
| model off, cab off | 48 k | 25 | 28.0 | -0.82 | 60 |
| model off, cab off | 96 k | 31 | 39.4 | -3.43 | 137 |
| model off, VIR | 44.1 k | 14 | 60.2 | -6.75 | 2053 |
| model off, VIR | 48 k | 34 | 85.0 | -7.12 | 2254 |
| model off, VIR | 96 k | 48 | 150.3 | -10.13 | 4487 |

The AU reports 0.00015873 s (7 samples at 44.1 kHz) at every rate [measured: evidence/tonex_cab_params.tsv]; the real delay at 48 kHz is 28 samples.

### 4.5 TONEX: microphone position (evidence/tonex_mic.tsv)

| MIC 1 X | L2 dB | 125 Hz | 1.6 kHz | 4 kHz | 5 kHz |
| --- | --- | --- | --- | --- | --- |
| 0 (cap) | -5.45 | 12.5 | -1.2 | -1.4 | 0.4 |
| 0.5 | -9.74 | 11.8 | -5.4 | -9.0 | -9.4 |
| 1 (edge) | -13.28 | 8.6 | -5.2 | -17.9 | -15.8 |

| MIC 1 Z | L2 dB | 60 Hz | 125 Hz | 4 kHz | delay against Z = 0 |
| --- | --- | --- | --- | --- | --- |
| 0 | -6.62 | 0.7 | 12.4 | -1.3 | 0 |
| 0.5 | -7.64 | -2.6 | 10.0 | -0.7 | 0.94 samples |
| 1 | -8.61 | -5.6 | 7.9 | -0.9 | 1.12 samples |

Position moves 16 dB of treble and 8 dB of overall level; distance moves the bass by 4.5 dB and the level by 2 dB. No loudness compensation is applied across positions.

### 4.6 TONEX: blend (evidence/tonex_blend.txt)

`blend 0.5 = 0.5000*mic1 + 0.5000*mic2`, residual -79.8 dB; `blend 0.25 = 0.7500*mic1 + 0.2500*mic2`, residual -83.2 dB. Cross-correlation delay between the close condenser and the far dynamic: 1.69 samples.

### 4.7 TONEX: Cab Resonance, Presence, Depth relative to 0.5 (evidence/tonex_res_diff.tsv)

| control | 60 Hz | 80 | 100 | 110 | 125 | 160 | 315 | 1 k | 2.5 k | 5 k | 10 k | 20 k |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Resonance 0 | -1.3 | -2.7 | -4.3 | -4.5 | -4.0 | -2.1 | -0.2 | -1.1 | -2.8 | -3.7 | -4.3 | -4.9 |
| Resonance 1 | 0.5 | 1.9 | 5.0 | 5.8 | 4.3 | 1.3 | 0.2 | 0.6 | 2.2 | 3.6 | 5.0 | 6.4 |
| Presence 1 | 0.0 | 0.1 | 0.1 | 0.1 | 0.2 | 0.3 | 0.8 | 2.5 | 3.5 | 4.5 | 5.5 | 6.0 |
| Depth 1 | 5.1 | 6.0 | 5.4 | 4.9 | 4.2 | 2.8 | 0.8 | 0.1 | 0.0 | 0.0 | 0.0 | 0.0 |

Presence 0 and Depth 0 mirror the rows above. All three work with the amp model off, so they are linear filters outside the neural model. Resonance 1 against 0: 10.3 dB at 109 Hz, half-gain band 84 to 143 Hz [measured: evidence/tonex_blend.txt].

### 4.8 TONEX: level across cab models (evidence/tonex_cabs.tsv)

Ten cab models, default mics: L2 from -10.9 dB to -6.3 dB. Three mic models on one cab: -6.9 to -6.4 dB.

### 4.9 Guitar Rig 6: cabinet components (evidence/gr6_blackbox.tsv)

| case | rate | peak index | L2 dB | t60 samples | 100 Hz | 1 kHz | 4 kHz |
| --- | --- | --- | --- | --- | --- | --- | --- |
| component off (empty rack) | 48 k | 16 | -0.47 | 25 | 0.0 | 0.0 | 0.0 |
| Matched Cabinet, AC Box, Air 0 | 44.1 k | 43 | -0.77 | 1898 | 7.4 | 4.1 | 0.0 |
| same | 48 k | 44 | -1.14 | 2064 | 7.4 | 4.1 | 0.0 |
| same | 96 k | 55 | -4.15 | 4095 | 7.4 | 4.1 | 0.0 |
| same, Crossfade 1 | 48 k | 44 | -0.02 | 2058 | -2.8 | 9.3 | 3.6 |
| same, Air 1 | 48 k | 44 | L 0.20, R 0.39 | 9045 | 5.6 | 5.0 | 0.2 |
| Matched Cabinet, Cabinet 0 | 48 k | 31 | -6.33 | 1816 | 1.5 | -3.1 | -2.1 |
| Control Room Pro, slot 1 | 48 k | 20 | -11.81 | 2156 | -2.0 | -6.9 | -8.7 |
| same, Phase 1 = 1 | 48 k | 26 | -11.81 | 2161 | -2.0 | -6.9 | -8.7 |
| same, Gain 1 = 1 (default 0.5) | 48 k | 20 | 0.57 | 2156 | 10.4 | 5.5 | 3.6 |
| same, Pan 1 = 0, Stereo on | 48 k | 20 | L -5.79, R silent | 2156 | 4.1 | -0.8 | -2.7 |
| same | 96 k | 9 | -14.82 | 4279 | -2.0 | -6.9 | -8.7 |
| Control Room, INIT | 48 k | 42 | -16.93 | 2029 | -2.9 | -12.4 | -10.7 |

The L2 falls 3 dB per doubling of the rate while the response stays put: the IR is scaled by source rate over session rate, as the code says.

### 4.10 Prototype checks of our own designs (evidence/proto_*.tsv)

- Level rule, 25 IRs: K-weighted loudness of a guitar-amp spectrum through the cab, against cab off. JUCE's rule: -11.9 dB mean, 1.07 dB standard deviation. Peak normalisation: 2.40 dB deviation. A pink-weighted 80 Hz..5 kHz rule: 1.73 dB. L2 referenced to 48 kHz with a -9 dB target: -2.8 dB mean, 1.07 dB deviation, and the same gain at 44.1, 48 and 96 kHz [measured: evidence/proto_norm.tsv].
- Alignment: delaying one of the two mic IRs by 0.25 ms puts an 11.5 dB hole in a third-octave band of the 50/50 blend; inverting it loses 4.5 dB overall. Cross-correlation alignment finds the delay and polarity in all six cases and restores the blend to +2.5 dB with a worst band of -0.1 dB [measured: evidence/proto_align.tsv].
- Resampler: a 32-zero-crossing Kaiser sinc is within 0.09 dB of band-limited resampling to 20 kHz, residual -41 dB [measured: evidence/proto_resample.tsv].
- Speaker-load filter: see 7.2 [measured: evidence/proto_load.tsv].

## 5. Industry practice

| question | Guitar Rig 6 | AmpliTube 5 | TONE3000 | TONEX | NAM plugin (L1, L2) |
| --- | --- | --- | --- | --- | --- |
| engine | 128-tap FIR head + non-uniform FFT (64, 256, 1024..) | 128-tap FIR head + uniform 128 (non-uniform from 8192 taps) | JUCE uniform; non-uniform head 8192 above 1 s | not read (VIR engine in AT binary) | direct FIR, dot product |
| added latency | 0: peak 4 samples after the empty rack [measured] | 0 [inferred] | 0 (JUCE zero-latency mode) | 7 samples for the whole plugin at 44.1 k [measured] | 0 |
| IR length | up to 2^26 samples | 2048 / 4096; custom 2048..8192 | up to 10 s | 2048 at 44.1 k | 8192 taps |
| truncation, fade | none found | crop, no fade | JUCE trim at -80 dBFS | n/a | hard cut at 8192 |
| resampling | 8-point 5th-order polynomial, gain `src/dst` | windowed sinc (32 taps x 64 phases) | JUCE linear + Butterworth | fixed 44.1 k inside | cubic, gain `48000/fs` |
| level rule | fixed trims; `-12 dB - 20log10(N)`; Learn to -6 dBFS peak | per-cab trim table; custom IR default -20 dB | fixed -18 dB, +-24 dB trim | cabs within 4.6 dB L2 | fixed -18 dB |
| stereo | mono sum unless Stereo; stereo IR from pans | two mono mics panned; stereo custom IR | mono engine + stereo engine | mono output | mono or two channels |
| minimum phase | not found | not found | no | no [measured] | no |
| mic blend | 8 slots: gain, pan, polarity, offset, room; mixed into one IR | 2 mics, level, pan, polarity, physical delay | none | 2 mics, linear blend, pre-aligned IRs | none |
| automatic alignment | no | no | n/a | not needed (aligned content) | n/a |
| filters | bass and treble shelves baked into the IR | custom IR: HP 20..500, LP 5k..20k, tilt | block EQ | not found | none |
| speaker impedance | per-cab pole/zero record sent to the amp | circuit state-space module between amp and cab | none found | Cab Resonance; Presence, Depth | none |
| nonlinear speaker | none found | none found | none | not found | none |
| room, air | Air: 8 taps 15..45 ms; room IR per slot | room convolvers | none | none | none |
| IR switch | two convolvers, 20 ms crossfade | in-place rebuild | built off-thread, then JUCE 50 ms fade | not measured | not read |
| oversampling | convolver at base rate under 2x | n/a | IR at base rate | n/a | n/a |

What they agree on:

- **Zero added latency.** Three engines reach it three ways: FIR head plus FFT tail (GR, AT), a zero-latency FFT scheme (JUCE), a plain FIR (NAM). A direct FIR "has no inherent latency" but costs O(N^2); partitioning is always worth it for real-time FIR filtering [literature: https://www.dafx.de/paper-archive/2011/Papers/74_e.pdf].
- **Short IRs.** 2048 samples is the working size in AmpliTube, TONEX and Helix [decompiled: AT 0x154a9f4] [literature: https://manuals.line6.com/en/helix-stadium/live/cab-blocks]; NAM stops at 8192 [literature: https://raw.githubusercontent.com/sdatkinson/AudioDSPTools/main/dsp/ImpulseResponse.h].
- **Fixed trim, not energy normalisation, for user IRs.** -18 dB with a `48000/fs` law in NAM [literature: https://raw.githubusercontent.com/sdatkinson/AudioDSPTools/main/dsp/ImpulseResponse.cpp], -18 dB in TONE3000, -12 dB in Guitar Rig, -20 dB default in AmpliTube. A fixed trim is rate independent and keeps the file's own level; it does not level-match IRs from different makers.
- **Rate-correct gain after resampling:** multiply by source rate over destination rate (GR, JUCE's non-normalised path, NAM).
- **Blending on the impulse responses, with explicit time control.** Guitar Rig and AmpliTube expose the time offset; TONEX ships pre-aligned content; Helix has a Delay per cab whose Auto setting follows the mic distance, and a Polarity switch for blends [literature: https://manuals.line6.com/en/helix-stadium/live/cab-blocks]. For IRs from different sources, removing the time delay is the more useful step, and a minimum-phase version is not the cab's own response [literature: https://www.avnirvana.com/threads/minimum-phase.4699/].
- **A speaker-impedance stage in the three IK and NI products.** The physics: a guitar speaker's voice-coil resistance is about 75 to 85 % of nominal, it resonates between about 55 and 100 Hz with a peak of 60 ohm or more, and its impedance rises to 20..40+ ohm at the top [literature: https://celestion.com/?p=10987]; peaks can be five times the rated impedance or more, and a reactive dummy load reproduces this with a series R, an L with a parallel R, and a parallel R-L-C [literature: https://duncanamps.com/technical/dummyload.html]. A capture made into a resistive load does not contain this response.
- **No nonlinear speaker stage** in any cab path read. The published model for a Celestion Vintage 30 found the distortion significant only at low frequencies and concluded that the salient effects are linear and the nonlinear ones subtle [literature: https://ccrma.stanford.edu/~dtyeh/cabinet08/dafx08cabinet.pdf].

## 6. Gap analysis

| aspect | ours | references | consequence for the player |
| --- | --- | --- | --- |
| IR resampling | linear interpolation, -2.5 dB at 14 kHz, -17 dB error | polynomial or sinc | a 44.1 kHz IR sounds duller in a 48 kHz session than in the vendor's player |
| level against session rate | +3.1 dB at 96 kHz against 48 kHz | fixed, rate-compensated | the same preset is louder in a 96 kHz project; drive into later blocks changes |
| absolute level | L2 -18.06 dB: about 12 dB quieter than cab off | -6 to -11 dB (TONEX), about -9 dB for a full-scale file under the -18 dB law [measured: evidence/proto_norm.tsv] | turning the cab on drops the level; our cab is quieter than other players with the same file |
| level trim | none | every reference has one | no way to balance IRs |
| quiet files | response changes with file level (absolute trim) | no absolute threshold | low-level IR files lose their tail |
| first moments after load | dry signal through a default IR at -2.3 dB (48 k) | engine built before use (T3), two-engine swap (GR) | a burst of uncabbed amp sound on preset load |
| bypass | hard switch, cuts skipped too | GR crossfades the bypass | click on toggling the cab |
| long IRs at small buffers | 16 % CPU for 2 s at 32 samples | non-uniform partitions | room IRs are costly at low-latency settings |
| speaker impedance | none | GR, AT, TONEX | amp-only captures made into a resistive load sound flat in the bass and soft on top; no Resonance or Presence control |
| second IR | none | 2 mics (AT, TONEX), 8 slots (GR), dual cab (Helix) | the common two-mic blend needs two plugin instances, with no alignment help |
| cut slopes | 12 dB/oct only | AT uses biquads bypassed at the end stops | a 12 dB/oct high cut leaves fizz that a steeper cut removes |
| cut at the end stops | the 20 kHz low-pass still acts (-0.87 dB of a white IR) | bypassed at the end stops (AT) | "fully open" is not transparent |
| stereo IRs | two channels, balance kept | same or mono | fine |
| latency | zero | zero | fine |
| mic position, room | none | AT, TONEX, GR | needs measured content; out of scope for an IR loader |

## 7. Recommendations

Ranked by audible benefit against cost. Parameter ids are new and appended; `cab_on`, `cab_lowcut` and `cab_highcut` are untouched. Every new parameter has a neutral default.

### 7.1 Loader v2: correct resampling, rate-independent level, relative trim, engine ready before use

**Evidence.** Section 2 measurements (resampler error, +3.1 dB at 96 kHz, quiet-file change, default-IR burst); GR's polynomial converter and `src/dst` gain [decompiled: GR 0x26a5a0, 0x26a674]; AT's windowed sinc [decompiled: AT 0x102fac4]; T3 builds and runs the engine before the swap [decompiled: T3 0x69440]; fixed-trim practice (section 5).

**What to build.** Our own preparation step on the loader thread, which hands JUCE a finished buffer so JUCE neither trims, resamples nor normalises:

```
loadV2(file, fs):
  x, fsFile = read all channels (max 2), at most 10 s
  # 1 trim, relative to the IR's own peak, same offsets for both channels
  p     = max |x|
  start = first index with |x| >= p * 10^(-60/20), minus 8 samples (not below 0)
  end   = last  index with |x| >= p * 10^(-80/20)
  x     = x[start..end]; fade the last 64 samples with a raised cosine
  # 2 resample, response preserving
  y = kaiserSinc(x, fsFile -> fs, 32 zero crossings per side, beta 9, cutoff 0.49*min(fsFile, fs))
  y *= fsFile / fs
  # 3 level, independent of fs:  E = mean of |H(f)|^2 over 0..24 kHz, max of the channels
  E = (1/24000) * integral_0^24000 |Y(f)|^2 df       # equals sum(y^2) at fs = 48000
  y *= 10^(-9/20) / sqrt(E)
  # 4 engine: built and primed off the audio thread
  conv = new juce::dsp::Convolution(NonUniform{2048})   # zero latency; tail engine only if the IR is longer
  conv.loadImpulseResponse(move(y), fs, Stereo::yes, Trim::no, Normalise::no)
  conv.prepare(spec); run conv on silence until conv.getCurrentIRSize() == y.length
  publish conv to the audio thread; audio thread crossfades old -> new over 20 ms, then retires the old one
```

Constants are ours. The -9 dB target puts a file that peaks at 0 dBFS at the level the open -18 dB law gives it: the 25 measured IRs have L2 8.9 dB above their peak on average, so the two rules meet at -9.1 dB [measured: evidence/proto_norm.tsv]. With it the cab is 2.8 dB quieter than cab-off on a guitar-amp spectrum, standard deviation 1.07 dB, instead of 11.9 dB quieter [measured: evidence/proto_norm.tsv]. The 60 dB start threshold and 8-sample pre-roll keep the IR's own onset shape; the 80 dB end threshold is JUCE's figure made relative. A pink-weighted rule and peak normalisation were tried and rejected (1.73 dB and 2.40 dB deviation) [measured: evidence/proto_norm.tsv].

The loader version is a property of the loaded IR, saved in the plugin state next to `irPath` (attribute `irLoader`, absent or 1 = today's loader, 2 = v2). It is not a parameter. `clearImpulseResponse` and the bypass use the same 20 ms crossfade. `irLoaded` becomes atomic.

**New parameter.**

| id | name | range | default |
| --- | --- | --- | --- |
| `cab_level` | Cab Level | -24..+24 dB, step 0.1 | 0 |

Applied as a smoothed gain after the cut filters in both loader versions.

**CPU and latency.** Audio cost unchanged for IRs up to 2048 samples; latency stays zero. For longer IRs the head is a 2048-sample uniform engine and the tail runs in 2048-sample blocks, so the cost at small buffers should fall to roughly the 1024-block column of 4.3 plus the 2229-sample row (about 1 % instead of 16 % for 2 s at 32 samples) [inferred from evidence/ours_cpu.tsv]. Load-time work: one sinc resample of a few thousand samples.

**Compatibility.** A state without `irLoader` loads through the existing call with the existing options and the existing `Convolution` object: bit-identical. Only IRs loaded from the UI after the change get `irLoader="2"`. `cab_level` defaults to 0 dB.

**Proof.** Re-run `t1.py`, `t2.py` and the bright-IR test with v2:

- gain at 1 kHz of `ir_cab44.wav` in 44.1, 48 and 96 kHz sessions within 0.1 dB of each other (today 3.6 dB apart);
- `ir_bright44.wav` and `ir_bright96.wav` against `ir_bright48.wav` at 48 kHz: within 0.1 dB up to 16 kHz and residual below -40 dB (today -3.7 dB and -17 / -10 dB);
- `ir_cab48_quiet.wav` identical to `ir_cab48.wav` within 0.05 dB (today 0.6 dB);
- with a 0.5 s warm-up the output is silent or the full IR response, never the default-IR response of `ours_default_ir_transient.tsv`;
- 2 s IR at 32-sample blocks below 2 % CPU;
- a state saved by the current build renders bit-identically.

### 7.2 Speaker-load stage: Resonance and Presence

**Evidence.** GR sends a per-cab impedance record to the amp [decompiled: GR 0x26b5f8, 0x2db578]; AT discretises a loudspeaker circuit between amp and cab [decompiled: AT 0xf1aba8]; TONEX's Cab Resonance moves 10.3 dB at 109 Hz and 11 dB at 20 kHz, and its Presence and Depth are +-6 dB [measured: evidence/tonex_res_diff.tsv]. Speaker values from Celestion and Aiken (section 5). NAM captures of an amp head are often taken into a resistive load; they lack this response.

**What to build.** A linear filter in front of the convolution, derived from a lumped speaker driven by an amplifier of output resistance `Ra`. With voice-coil resistance `Re`, a parallel resonant branch of peak resistance `Rp`, mechanical Q `Qm` and frequency `f0`, and a lossy inductance `Le` in parallel with `R2`, the voltage across the speaker, normalised to 1 at mid frequencies, splits into two sections when the low and the high part are given their own `Ra` and `Rb`:

```
low section (resonance), exact:
  G_res(s) = (s^2 + s*w0/Qz + w0^2) / (s^2 + s*w0/Qp + w0^2)
  Qz = Qm / (1 + Rp/Re)        Qp = Qm / (1 + Rp/(Re + Ra))       peak gain = Qp/Qz
high section (presence), exact:
  G_pres(s) = (1 + s/wz) / (1 + s/wp)
  wz = Re*R2 / (Le*(Re + R2))  wp = (Re + Rb)*R2 / (Le*(Re + R2 + Rb))   top gain = wp/wz
player value g (dB) -> resistance:   R = Re * ( rho / ((1 + rho)/10^(g/20) - 1) - 1 ),  rho = Rp/Re (or R2/Re)
```

Our constants, for an 8 ohm guitar speaker: `Re = 6.5` ohm (81 % of nominal), `Rp = 40` ohm (peak 46.5 ohm, 5.8 times nominal), `Qm = 8`, `R2 = 40` ohm, `Le = 1.0 mH` (zero at 890 Hz), `f0` default 100 Hz. Sources: Celestion's 75..85 % and "60 ohm or more", Aiken's "5 times the rated impedance", and the 20..40+ ohm treble rise [literature: https://celestion.com/?p=10987, https://duncanamps.com/technical/dummyload.html]. A 12 dB setting corresponds to an output resistance of 44 ohm, a 6 dB setting to 9 ohm [measured: evidence/proto_load.tsv]. Discretise each section with a bilinear transform, each corner prewarped; at 44.1 kHz the result is within 0.07 dB of the analog curve [measured: evidence/proto_load.tsv].

Resulting response (dB), from `evidence/proto_load.tsv`:

| setting | 60 Hz | 80 | 100 | 125 | 160 | 400 | 1 k | 2.5 k | 6.3 k | 16 k |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Resonance 6 | 1.6 | 4.0 | 6.0 | 4.0 | 1.8 | 0.2 | 0.0 | 0.0 | 0.0 | 0.0 |
| Resonance 12 | 2.1 | 6.0 | 12.0 | 6.0 | 2.4 | 0.2 | 0.0 | 0.0 | 0.0 | 0.0 |
| Presence 6 | 0.0 | 0.0 | 0.0 | 0.1 | 0.1 | 0.6 | 2.3 | 4.7 | 5.8 | 6.0 |
| Presence 12 | 0.0 | 0.0 | 0.1 | 0.1 | 0.1 | 0.7 | 3.2 | 7.7 | 10.9 | 11.8 |

The half-gain width of the bell at 10 dB (about 80 to 125 Hz) is close to TONEX's measured 84 to 143 Hz, and the treble rise starts at the same place (near 500 Hz) [measured: evidence/tonex_blend.txt, evidence/proto_load.tsv]. The constants are not taken from any vendor table.

**New parameters.**

| id | name | range | default |
| --- | --- | --- | --- |
| `cab_res` | Cab Resonance | 0..12 dB, step 0.1 | 0 |
| `cab_res_freq` | Resonance Freq | 60..160 Hz, log, step 1 | 100 |
| `cab_pres` | Cab Presence | 0..12 dB, step 0.1 | 0 |

The UI should say that these are meant for amp-only captures and are normally left at 0 for captures taken through a real cabinet or a reactive load.

**CPU and latency.** One biquad and one first-order section per channel; coefficients at control rate; zero latency.

**Compatibility.** At 0 dB both sections are exactly unity and are skipped, so old sessions are unchanged. The stage runs under the same gate as today's cab (block on and an IR loaded).

**Proof.** Unit-impulse IR, cuts open: with `cab_res = 6` the magnitude is +6.0 +-0.3 dB at 100 Hz, +4.0 +-0.5 dB at 80 and 125 Hz, under 0.3 dB at 400 Hz; with `cab_pres = 6` it is +2.3 +-0.3 dB at 1 kHz and +5.8 +-0.3 dB at 6.3 kHz, at 44.1, 48 and 96 kHz. Both at 0: output bit-identical to the current build.

### 7.3 Second IR with blend, polarity and automatic alignment

**Evidence.** GR mixes up to eight IRs into one before convolution, with fader, pan, polarity and time offset [decompiled: GR 0xb302c..0xb31a8]; AT runs two mics [decompiled: AT 0x154c9a8]; TONEX's blend is a linear mix of aligned IRs [measured: evidence/tonex_blend.txt]; Helix gives each side Delay and Polarity [literature: https://manuals.line6.com/en/helix-stadium/live/cab-blocks]. A 0.25 ms offset between two IRs costs 11.5 dB in one band; cross-correlation alignment recovers it [measured: evidence/proto_align.tsv].

**What to build.** IR B goes through the same preparation as IR A (7.1, steps 1 to 3, each normalised on its own). Then, on the loader thread:

```
align(A, B):                               # both at fs, band 80 Hz..5 kHz
  c[k]   = cross-correlation of A and B, 8x oversampled, |lag| <= 5 ms
  k0     = argmax |c[k]|
  delay  = k0 / 8 samples (fractional), polarity = sign(c[k0])
composite = (1 - mix) * A + mix * polarity * userPolarity * shift(B, delay + userDelay)
```

`shift` is a windowed-sinc fractional delay applied to the IR. The composite goes to the engine as in 7.1 step 4, so the audio thread still runs one convolution. A change of mix, delay or polarity rebuilds the composite, at most once per crossfade (20 ms). The blend law is linear, as measured in TONEX; the level is not re-normalised after the blend.

**New parameters and state.**

| id | name | range | default |
| --- | --- | --- | --- |
| `cab_mix` | IR Blend | 0..100 % (0 = IR A only) | 0 |
| `cab_b_align` | Auto Align | off / on | on |
| `cab_b_delay` | IR B Delay | -2.00..+2.00 ms, step 0.01 | 0 |
| `cab_b_invert` | IR B Invert | off / on | off |

State attribute `irPathB` next to `irPath`. IR B always uses loader v2. If IR A was loaded by the old loader, loading an IR B reloads A through v2 (the user changed the cab; the state then carries `irLoader="2"`).

**CPU and latency.** No audio-rate cost; zero latency. Each rebuild is a few thousand multiply-adds plus the engine construction on the loader thread.

**Compatibility.** With no `irPathB` nothing changes.

**Proof.** Load the same IR as A and B, with B's file delayed by 24 samples and inverted, `cab_mix = 50`: with Auto Align on, the response is within 0.2 dB of IR A alone from 80 Hz to 5 kHz; with it off, the third-octave minimum is below -6 dB. With two different IRs, `cab_mix = 25` equals `0.75*A + 0.25*B'` with a residual below -60 dB.

### 7.4 Steeper cuts, transparent end stops

**Evidence.** Ours is 12 dB/oct [measured: evidence/ours_cuts.tsv]; AT's custom-IR filters are bypassed at 20 Hz and 20 kHz [decompiled: AT 0xf13698]; our 20 kHz setting still removes 0.87 dB of a white IR [measured: evidence/ours_level.tsv].

**What to build.** A slope choice. At 24 dB/oct each cut is two state-variable sections with Butterworth Q values 0.541 and 1.307. In the 24 dB mode a cut at its end stop (20 Hz, 20 kHz) is skipped.

| id | name | range | default |
| --- | --- | --- | --- |
| `cab_cut_slope` | Cut Slope | 12 dB/oct, 24 dB/oct | 12 dB/oct |

**CPU and latency.** Two more second-order sections per channel; zero latency.

**Compatibility.** Choice 0 is the current filter pair, unchanged, end stops included.

**Proof.** Unit-impulse IR: -24 +-1 dB one octave past each cutoff in the 24 dB mode; flat within 0.05 dB from 20 Hz to 20 kHz with both cuts at their end stops.

### 7.5 Full-rig captures: offer the bypass

**Evidence.** TONEX ties the cab choice to the capture: MODEL, VIR or OFF [measured: evidence/tonex_cab_params.tsv]; we already detect the case and warn [`ui/src/hooks/useRigCab.ts:30-45`].

**What to build.** UI only: when a capture with a cab is loaded while the cab block is on, the existing warning gets a one-click "bypass cab" action. No DSP change, no new parameter.

### 7.6 Air (low priority)

**Evidence.** GR adds eight reflections between 15 and 45 ms after the cab [decompiled: GR 0x25c0fc]; AT uses room convolvers [decompiled: AT 0x1550f10].

**What to build.** A short early-reflection tail added to the composite IR at load time, so it costs nothing at audio rate: six taps at the extra path lengths of a small room's first reflections (image-source geometry for a room and cab position we choose, not the reference's delays), each low-passed at 4 kHz and scaled by `1/distance`, mixed by one amount control.

| id | name | range | default |
| --- | --- | --- | --- |
| `cab_air` | Air | 0..100 % | 0 |

Proof: at 0 the composite IR is unchanged; at 100 the added energy is -12 +-1 dB against the dry IR.

### 7.7 Not recommended

- **Minimum-phase conversion of IRs.** It replaces the cab's phase with one that only shares the magnitude; delay alignment solves the blending problem and keeps the response [literature: https://www.avnirvana.com/threads/minimum-phase.4699/] [measured: evidence/proto_align.tsv].
- **A nonlinear speaker stage.** No reference has one in the cab path, and the published model calls the effect subtle and confined to low frequencies [literature: https://ccrma.stanford.edu/~dtyeh/cabinet08/dafx08cabinet.pdf].
- **Microphone position or distance controls.** In AmpliTube and TONEX these are interpolation between measured IRs [decompiled: AT 0xf17ed8]; without that content they would be invented EQ.
- **Our own FFT engine.** JUCE's engine is zero latency and costs 0.14 to 0.5 % for cab-length IRs [measured: evidence/ours_cpu.tsv]. A 128-tap FIR head with non-uniform partitions (GR, AT) would only pay off for long IRs at small buffers, which `NonUniform` covers.

## 8. Open questions and unverified items

- The CPU of JUCE's `NonUniform{2048}` mode at 32-sample blocks is estimated, not measured; its tail FFT runs in one callback every 2048 samples and may spike. Guitar Rig spreads that work over time [decompiled: GR 0x61714c]. Measure before choosing the head size.
- JUCE's zero-latency engine does two FFTs per call. Whether Logic or tubamp's own block splitting ever calls the cab with partial blocks was not measured.
- Guitar Rig: value ranges of the IR Loader parameters; the Phase display shows ten times the distance that the measured delay corresponds to (40 cm against 4 cm at full scale); how the amps apply the impedance filter; Convolver CPU; Matched Cabinet Pro and the IR Loader could not be put in a headless rack.
- AmpliTube: mapping of Mic Level (0..6) and Cabinet Phase (-20..20); the pan law; what the 20 ms counter ramps; the three-state CabinetQuality; the unit reading of the impedance records is ours.
- TONEX: the VIR interpolation code (`CabVIRLight::UpdateIR`) was not read; the plugin's reported latency (7 samples at 44.1 kHz) understates the delay at other rates, which was measured but not explained.
- Plugin Alliance cabs could not be isolated from their amps.
- The level study uses 25 responses measured from one product's cab engine as stand-ins for real IR files, and one synthetic judge spectrum. Repeat it on a library of commercial IR files and real DI recordings before fixing the -9 dB target.
- Gardner's paper and Wefers' thesis were not readable (paywall, no open copy); the Fractal Audio wiki returned 403. Their content is not used as evidence.
- The installed tubamp AU was measured, not a build of this worktree. The cab code is the same class; the measurement was not repeated on a fresh build.
