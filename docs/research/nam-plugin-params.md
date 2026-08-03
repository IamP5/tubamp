# Official Neural Amp Modeler Plugin — Parameter & DSP Spec

Source: `sdatkinson/NeuralAmpModelerPlugin` (GitHub, `main` branch, fetched 2026-08-03),
plus `sdatkinson/NeuralAmpModelerCore` (`Dependencies/AudioDSPTools/dsp/`, and top-level `NAM/slimmable.h`).

Files pulled directly (raw.githubusercontent.com) and read in full:
- `NeuralAmpModeler/NeuralAmpModeler.h`
- `NeuralAmpModeler/NeuralAmpModeler.cpp`
- `NeuralAmpModeler/ToneStack.h` / `ToneStack.cpp`
- `NeuralAmpModeler/NeuralAmpModelerControls.h` (partial — file-browser control)
- `NeuralAmpModelerCore/NAM/slimmable.h`

Local copies already vendored in this repo's build tree and read in full:
- `build/_deps/neuralampmodelercore-src/Dependencies/AudioDSPTools/dsp/NoiseGate.h` / `.cpp`
- `build/_deps/neuralampmodelercore-src/Dependencies/AudioDSPTools/dsp/RecursiveLinearFilter.h` / `.cpp`

---

## 1. Full parameter list

Declared in `NeuralAmpModeler::NeuralAmpModeler()`, `NeuralAmpModeler.cpp:83-96`, enum in `NeuralAmpModeler.h:31-51`.

| Param (`EParams`) | UI label | Init call | Default | Range | Step | Units |
|---|---|---|---|---|---|---|
| `kInputLevel` | "Input" | `InitGain` | 0.0 | -20.0 .. 20.0 | 0.1 | dB |
| `kNoiseGateThreshold` | "Threshold" | `InitGain` | -80.0 | -100.0 .. 0.0 | 0.1 | dB |
| `kToneBass` | "Bass" | `InitDouble` | 5.0 | 0.0 .. 10.0 | 0.1 | (knob units, "5 is noon") |
| `kToneMid` | "Middle" | `InitDouble` | 5.0 | 0.0 .. 10.0 | 0.1 | (knob units) |
| `kToneTreble` | "Treble" | `InitDouble` | 5.0 | 0.0 .. 10.0 | 0.1 | (knob units) |
| `kOutputLevel` | "Output" | `InitGain` | 0.0 | -40.0 .. 40.0 | 0.1 | dB |
| `kNoiseGateActive` | "NoiseGateActive" | `InitBool` | true | — | — | toggle |
| `kEQActive` | "ToneStack" | `InitBool` | true | — | — | toggle |
| `kOutputMode` | "OutputMode" | `InitEnum` | index 1 | {"Raw","Normalized","Calibrated"} | — | enum |
| `kIRToggle` | "IRToggle" | `InitBool` | true | — | — | toggle |
| `kCalibrateInput` | "CalibrateInput" | `InitBool` | false | — | — | toggle |
| `kInputCalibrationLevel` | "InputCalibrationLevel" | `InitDouble` | 12.0 | -60.0 .. 60.0 | 0.1 | dBu |
| `kSlim` | "Slim" | `InitDouble` | 0.0 | 0.0 .. 1.0 | 0.01 | normalized (model-size control) |

Exact source lines (`NeuralAmpModeler.cpp:83-96`):
```cpp
GetParam(kInputLevel)->InitGain("Input", 0.0, -20.0, 20.0, 0.1);
GetParam(kToneBass)->InitDouble("Bass", 5.0, 0.0, 10.0, 0.1);
GetParam(kToneMid)->InitDouble("Middle", 5.0, 0.0, 10.0, 0.1);
GetParam(kToneTreble)->InitDouble("Treble", 5.0, 0.0, 10.0, 0.1);
GetParam(kOutputLevel)->InitGain("Output", 0.0, -40.0, 40.0, 0.1);
GetParam(kNoiseGateThreshold)->InitGain("Threshold", -80.0, -100.0, 0.0, 0.1);
GetParam(kNoiseGateActive)->InitBool("NoiseGateActive", true);
GetParam(kEQActive)->InitBool("ToneStack", true);
GetParam(kOutputMode)->InitEnum("OutputMode", 1, {"Raw", "Normalized", "Calibrated"});
GetParam(kIRToggle)->InitBool("IRToggle", true);
GetParam(kCalibrateInput)->InitBool(kCalibrateInputParamName.c_str(), kDefaultCalibrateInput);
GetParam(kInputCalibrationLevel)
  ->InitDouble(kInputCalibrationLevelParamName.c_str(), kDefaultInputCalibrationLevel, -60.0, 60.0, 0.1, "dBu");
GetParam(kSlim)->InitDouble("Slim", 0.0, 0.0, 1.0, 0.01);
```

No knob is linear-skewed beyond iPlug2's default `InitGain`/`InitDouble` shape (no explicit `SetShape` call) — treat all as linear in their declared range.

---

## 2. Tone stack

**Class:** `dsp::tone_stack::BasicNamToneStack` (`ToneStack.h`/`ToneStack.cpp`), a plugin-local class — **not** a generic AudioDSPTools tone-stack class. It's built out of three `recursive_linear_filter::Biquad` subclasses from `AudioDSPTools/dsp/RecursiveLinearFilter.h`: `LowShelf`, `Peaking`, `HighShelf`. Those Biquad coefficient formulas are the standard [Audio EQ Cookbook](https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html) equations (see `RecursiveLinearFilter.cpp:107-164`).

**Topology and exact mapping from the 0-10 knob value (`val`) to filter params** (`ToneStack.cpp:22-61`):

| Stage | Filter type | Frequency | Q | Gain (dB) formula | Gain range |
|---|---|---|---|---|---|
| Bass | Low shelf | 150 Hz (fixed) | 0.707 (fixed) | `4.0 * (val - 5.0)` | ±20 dB |
| Middle | Peaking | 425 Hz (fixed) | `1.5` if `midGainDB < 0`, else `0.7` | `3.0 * (val - 5.0)` | ±15 dB |
| Treble | High shelf | 1800 Hz (fixed) | 0.707 (fixed) | `2.0 * (val - 5.0)` | ±10 dB |

Note the mid-band Q is **gain-dependent** (asymmetric): wider (Q=1.5) when cutting, narrower (Q=0.7) when boosting — the comment explains this is deliberately done "to sound less honky" on boost.

**Series order** (`ToneStack.cpp:3-10`, `BasicNamToneStack::Process`):
```cpp
DSP_SAMPLE** bassPointers = mToneBass.Process(inputs, numChannels, numFrames);
DSP_SAMPLE** midPointers = mToneMid.Process(bassPointers, numChannels, numFrames);
DSP_SAMPLE** treblePointers = mToneTreble.Process(midPointers, numChannels, numFrames);
```
Bass → Middle → Treble, in series (cascaded biquads), each a standalone `RecursiveLinearFilter::Base` (3-in/3-out coefficient recursive filter using the Audio EQ Cookbook LowShelf/Peaking/HighShelf formulas).

**Position in the signal chain:** *after* the NAM model and *after* the noise-gate gain is applied, *before* the IR (see chain in section 4). Bypassable as a unit via `kEQActive`.

---

## 3. Noise gate

**Class:** `dsp::noise_gate::Trigger` (analyzes/decides) + `dsp::noise_gate::Gain` (applies), from `AudioDSPTools/dsp/NoiseGate.h`/`.cpp`. Confirmed **trigger/gain split architecture**: the `Trigger` is fed the **input** (pre-model) signal and computes a per-sample gain-reduction curve in dB; it pushes that curve to any registered `Gain` listeners via `AddListener`/`SetGainReductionDB`. The `Gain` object is applied later to the **model's output** (post-model), not to the trigger's own output (the trigger's own `Process()` just passes its input straight through unchanged — see `NoiseGate.cpp:106-109`, `memcpy` of inputs to outputs).

Listener wiring: `mNoiseGateTrigger.AddListener(&mNoiseGateGain);` (`NeuralAmpModeler.cpp:98`).

**Threshold parameter:** `kNoiseGateThreshold`, range -100.0 to 0.0 dB, default -80.0 dB, step 0.1 (user-controllable knob, fed straight in as `TriggerParams.threshold`).

**Fixed (non-user-facing) time/ratio constants**, set fresh every block in `ProcessBlock` (`NeuralAmpModeler.cpp:346-352`):
```cpp
const double time = 0.01;        // level-detector time constant, seconds
const double threshold = GetParam(kNoiseGateThreshold)->Value();
const double ratio = 0.1;        // "quadratic" expansion ratio
const double openTime = 0.005;   // seconds, max reduction -> 0 dB
const double holdTime = 0.01;    // seconds, hold open before re-closing
const double closeTime = 0.05;   // seconds, 0 dB -> max reduction
```
These override the `Trigger` constructor's own baked-in defaults (`NoiseGate.cpp:25-29`: `mParams(0.05, -60.0, 1.5, 0.002, 0.050, 0.050)` — time=0.05s, threshold=-60dB, ratio=1.5, open=2ms, hold=50ms, close=50ms), which only matter if the plugin never calls `SetParams` (it always does, every block, when the gate is active).

**Detector algorithm** (`NoiseGate.cpp:36-110`): per-channel envelope follower with a **single-pole smoothed power estimate**, `alpha = 0.5^(1/(time*sampleRate))`, clamped to `[MINIMUM_LOUDNESS_POWER, 1000.0]` where `MINIMUM_LOUDNESS_DB = -120 dB`. Gain reduction is **quadratic** below threshold: `-ratio * (levelDB - threshold)^2` (never linear dB/dB). State machine has two states, `MOVING` and `HOLDING`; movement toward 0 dB (opening) is rate-limited by `dOpen = maxGainReduction / openTime * dt`, movement toward max reduction (closing) by `dClose`. Once fully open it enters `HOLDING` for `holdTime`, then reverts to `MOVING` (re-closes) if still below threshold.

**Gain application** (`NoiseGate.cpp:149-182`, `dsp::noise_gate::Gain::Process`): `output[c][s] = 10^(gainReductionDB[c][s]/10) * input[c][s]` — note this is a **power-based** (÷10, not ÷20) dB-to-linear conversion, consistent with the trigger computing reduction from squared/power-domain levels.

**Position in chain:** Trigger runs on the *raw input* (after input gain, before the model). Gain is applied to the *model output* (after the model, before the tone stack). See section 4.

---

## 4. Full DSP chain, input → output

From `NeuralAmpModeler::ProcessBlock` (`NeuralAmpModeler.cpp:322-398`), condensed to the essential order:

1. **`_ProcessInput`**: external I/O collapsed to mono internal buffer, **input gain applied here** (`mInputGain`, computed in `_SetInputGain`). If not standalone (`APP_API` undefined, i.e. plugin context), also divides by `nChansIn` to average stereo→mono without loudness doubling.
2. **Noise gate trigger** (if `kNoiseGateActive`): `mNoiseGateTrigger.Process(mInputPointers, ...)` — analyzes input, passes it through unchanged.
3. **NAM model**: `mModel->process(triggerOutput, mOutputPointers, nFrames)` (or `_FallbackDSP` passthrough copy if no model loaded).
4. **Noise gate gain** (if active): `mNoiseGateGain.Process(mOutputPointers, ...)` — applies the gain-reduction curve computed in step 2 to the model's output.
5. **Tone stack** (if `kEQActive` and tone stack exists): `mToneStack->Process(gateGainOutput, ...)` — Bass→Mid→Treble in series (section 2).
6. **IR convolution** (if IR loaded and `kIRToggle`): `mIR->Process(toneStackOutPointers, ...)`.
7. **DC-blocking high-pass filter**: fixed 5 Hz cutoff (`kDCBlockerFrequency = 5.0`), `recursive_linear_filter::HighPass`, always applied (not user-toggleable), runs *after* the IR.
8. **`_ProcessOutput`**: mono internal buffer broadcast to all external output channels, **output gain applied here** (`mOutputGain`, computed in `_SetOutputGain`, includes normalization/calibration math — section 5). In standalone (`APP_API`) the result is hard-clamped to `[-1, 1]`; in-DAW it is not clamped.

So, in order: **Input gain → Gate trigger (analysis only) → NAM model → Gate gain (applied) → Tone stack (Bass→Mid→Treble) → IR → 5 Hz DC-blocking HPF → Output gain (incl. normalize/calibrate)**.

Relevant snippet (`NeuralAmpModeler.cpp:342-393`):
```cpp
sample** triggerOutput = mInputPointers;
if (noiseGateActive) { ... triggerOutput = mNoiseGateTrigger.Process(mInputPointers, ...); }

if (mModel != nullptr) mModel->process(triggerOutput, mOutputPointers, nFrames);
else _FallbackDSP(triggerOutput, mOutputPointers, ...);

sample** gateGainOutput = noiseGateActive ? mNoiseGateGain.Process(mOutputPointers, ...) : mOutputPointers;
sample** toneStackOutPointers = (toneStackActive && mToneStack != nullptr)
                                  ? mToneStack->Process(gateGainOutput, ...) : gateGainOutput;
sample** irPointers = toneStackOutPointers;
if (mIR != nullptr && GetParam(kIRToggle)->Value())
  irPointers = mIR->Process(toneStackOutPointers, ...);

const recursive_linear_filter::HighPassParams highPassParams(sampleRate, kDCBlockerFrequency);
mHighPass.SetParams(highPassParams);
sample** hpfPointers = mHighPass.Process(irPointers, ...);

_ProcessOutput(hpfPointers, outputs, ...);
```

---

## 5. Output modes — exact math

`_SetInputGain()` and `_SetOutputGain()`, `NeuralAmpModeler.cpp:690-730`.

**Input gain** (always, regardless of output mode):
```cpp
iplug::sample inputGainDB = GetParam(kInputLevel)->Value();
if ((mModel != nullptr) && (mModel->HasInputLevel()) && GetParam(kCalibrateInput)->Bool())
{
  inputGainDB += GetParam(kInputCalibrationLevel)->Value() - mModel->GetInputLevel();
}
mInputGain = DBToAmp(inputGainDB);
```
So input calibration only kicks in if the loaded model embeds an `inputLevel` metadata field *and* the user has enabled `CalibrateInput`; then the input knob's dB value is offset by `(user calibration target dBu) - (model's reported input level dBu)`.

**Output gain**, per `kOutputMode`:
```cpp
double gainDB = GetParam(kOutputLevel)->Value();
if (mModel != nullptr) {
  switch (GetParam(kOutputMode)->Int()) {
    case 1: // Normalized
      if (mModel->HasLoudness()) {
        const double loudness = mModel->GetLoudness();
        const double targetLoudness = -18.0;
        gainDB += (targetLoudness - loudness);
      }
      break;
    case 2: // Calibrated
      if (mModel->HasOutputLevel()) {
        const double inputLevel = GetParam(kInputCalibrationLevel)->Value();
        const double outputLevel = mModel->GetOutputLevel();
        gainDB += (outputLevel - inputLevel);
      }
      break;
    case 0: // Raw
    default: break;
  }
}
mOutputGain = DBToAmp(gainDB);
```

- **Raw** (mode 0): output knob value used as-is, no model-metadata compensation.
- **Normalized** (mode 1, the *default*): if the model reports a loudness value, output gain is offset so the model's measured loudness is brought to a fixed target of **-18.0 dB** (i.e., `gainDB += targetLoudness(-18) - modelLoudness`). This is independent of the input-calibration dBu system — it's a loudness-matching normalization, not a dBu-referenced one.
- **Calibrated** (mode 2): only meaningful together with an input-calibrated signal chain; offsets output gain by `(model's reported output level dBu) - (the user's input-calibration-level dBu setting)`, i.e. it assumes the input was calibrated to `kInputCalibrationLevel` dBu and scales output to match the model's own dBu output-level metadata, restoring a real-world dBu reference through the model.

All three modes still add the user's `kOutputLevel` knob value on top (trim around the computed reference).

---

## 6. Model/IR file row UX

Implemented in `NAMFileBrowserControl` (`NeuralAmpModelerControls.h:264+`), a shared class used for both the model row and the IR row (each instantiated once with a different file extension: `"nam"` for the model browser, `"wav"` for the IR browser — `NeuralAmpModeler.cpp:233-236`, `262-266`).

**Prev/Next semantics** (`NeuralAmpModelerControls.h`, `OnAttached()`, `prevFileFunc`/`nextFileFunc`):
```cpp
auto prevFileFunc = [&](IControl* pCaller) {
  const auto nItems = NItems();
  if (nItems == 0) return;
  mSelectedItemIndex--;
  if (mSelectedItemIndex < 0) mSelectedItemIndex = nItems - 1;
  LoadFileAtCurrentIndex();
};
auto nextFileFunc = [&](IControl* pCaller) {
  const auto nItems = NItems();
  if (nItems == 0) return;
  mSelectedItemIndex++;
  if (mSelectedItemIndex >= nItems) mSelectedItemIndex = 0;
  LoadFileAtCurrentIndex();
};
```
Yes — **prev/next cycles through files in the same directory as the currently loaded file, and wraps around** at both ends. The item list (`mItems`, populated by the inherited `IDirBrowseControlBase::AddPath`/`SetupMenu`) is (re)built whenever a file is loaded (via the file-picker dialog, or via a `kMsgTagLoadedModel`/`kMsgTagLoadedIR` message — see `OnMsgFromDelegate`), by taking the loaded file's containing directory (`directory.remove_filepart(true)`) and listing all files with the matching extension in it. So navigating prev/next never leaves the directory of the currently-loaded file.

**Clear (X) button**: `clearFileFunc` sends `SendArbitraryMsgFromUI(mClearMsgTag)` (which is `kMsgTagClearModel` or `kMsgTagClearIR` depending on instance), resets the label to the default placeholder string, and sets browser state to `Empty`. On the DSP side this sets `mShouldRemoveModel`/`mShouldRemoveIR = true` (`OnMessage`, `NeuralAmpModeler.cpp:551-552`), and `_ApplyDSPStaging()` (called at the top of every `ProcessBlock`) actually tears down the model/IR pointer and clears the stored path on the next audio callback (`NeuralAmpModeler.cpp:598-613`).

**File-name button**: clicking it either opens the file picker (if showing the default placeholder) or opens a popup menu of the directory's matching files (letting you jump directly instead of stepping prev/next).

---

## 7. The "Gateway" plugin and its model-row slider

Per a web search of tone3000.com / neuralampmodeler.com/gearspace (2026-08-03): **"Gateway" is the branded name of the current official player app from neuralampmodeler.com**, built by Steve Atkinson in partnership with TONE3000, released as the front-end for **NAM Architecture 2 (A2)** profiles. It is not a third-party skin of an old NAM plugin — architecturally it lines up with this same `sdatkinson/NeuralAmpModelerPlugin` codebase (same six-knob row: Input, Threshold+Gate toggle, Bass/Middle/Treble+EQ toggle, Output; same model-row layout with prev/next arrows, clear (X), globe "get more" button; same IR row; same settings gear), just carrying the A2-era branding/name.

**The small horizontal slider next to the model row is almost certainly the `kSlim` parameter** (`NeuralAmpModeler.h:49`, `NeuralAmpModeler.cpp:96`, UI wiring `NeuralAmpModeler.cpp:255-259` and `300-305`):
```cpp
GetParam(kSlim)->InitDouble("Slim", 0.0, 0.0, 1.0, 0.01);
```
It drives `nam::SlimmableModel::SetSlimmableSize(double val)` (`NeuralAmpModelerCore/NAM/slimmable.h`), an interface implemented by A2-era "slimmable" models that "reduce their computational cost at the expense of quality" — 0.0 = minimum size/CPU, 1.0 = maximum size/quality; the interpretation (sub-model selection, channel pruning, etc.) is model-specific. In the UI it's an icon (`kCtrlTagSlimmableIcon`) that's only shown when the loaded model actually implements `SlimmableModel` (`_UpdateControlsFromModel`, `NeuralAmpModeler.cpp:959-963`: `const bool show = mModel->GetSlimmableModel() != nullptr;`), and clicking it reveals an overlay knob (`kCtrlTagSlimKnob`) for the `Slim` param. So: it's a **quality/CPU tradeoff control for A2 slimmable models**, not an IR mix or a generic quality/oversampling setting — and it only appears/is enabled for models that support it, matching the reasoning in the task that Gateway gained something new tied to A2.

I did not find any oversampling/resampling-quality *user setting* in the settings gear — the plugin does per-model **sample-rate resampling** automatically and transparently (`ResamplingNAM` wrapper in `NeuralAmpModeler.h:98-194`, using `dsp::ResamplingContainer<NAM_SAMPLE, 1, 12>`) whenever the model's expected sample rate differs from the host's, but this isn't a user-facing "quality" toggle — it's silent correctness handling with no UI control. The settings gear (`kCtrlTagSettingsBox`, `NAMSettingsPageControl`) surfaces: sample rate / input & output calibration-level info about the loaded model (`ModelInfo` struct, `_UpdateControlsFromModel`), the `CalibrateInput` toggle + `InputCalibrationLevel` knob, and the `OutputMode` radio buttons (Raw/Normalized/Calibrated) — no separate CPU/oversampling quality knob beyond the A2 `Slim` control.

---

## Quick-reference summary for the tubamp implementation

**Parameter table**: see section 1 (exact ranges/defaults/steps for Input, Threshold, Bass/Mid/Treble, Output, plus the boolean/enum toggles).

**Chain order**: Input gain → Gate trigger (input-side, analysis only, passthrough) → NAM model → Gate gain (applied post-model) → Tone stack (Bass low-shelf → Mid peaking → Treble high-shelf, in series) → IR convolution → fixed 5 Hz DC-blocking HPF → Output gain (Raw/Normalized(-18dB target)/Calibrated).

**Tone stack coefficients** (Audio EQ Cookbook biquads, `sampleRate` = host rate):
- Bass: low shelf, f=150 Hz, Q=0.707, gainDB = `4*(val-5)` (±20 dB)
- Middle: peaking, f=425 Hz, Q = `1.5` if gainDB<0 else `0.7`, gainDB = `3*(val-5)` (±15 dB)
- Treble: high shelf, f=1800 Hz, Q=0.707, gainDB = `2*(val-5)` (±10 dB)

**Noise gate**: trigger analyzes input, quadratic power-domain expansion below threshold (`-0.1*(levelDB-threshold)^2`), single-pole level detector with `time=0.01s`, `openTime=0.005s`, `holdTime=0.01s`, `closeTime=0.05s`, gain applied downstream to the model's output via `10^(gainReductionDB/10)`.
