
# Integrating NeuralAmpModelerCore into a third-party JUCE plugin — research notes
(Snapshot as of 2026-08-02. Core repo main @ commit `3cde95c354d5ba6da01316cad90b05cfc4855053`, version.h = 0.5.5-dev; latest tagged release = **v0.5.4** (2026-06-24, commit `1f42f88535884450104b8711d7595019afa0495b`).)

## 1. Repo layout (sdatkinson/NeuralAmpModelerCore)

```
NAM/
  activations.{h,cpp}      # tanh/fast-tanh/leaky-relu etc.
  compiler.h
  container.{h,cpp}        # SlimmableContainer arch (registers "SlimmableContainer")
  conv1d.{h,cpp}
  convnet.{h,cpp}          # registers "ConvNet"
  dsp.{h,cpp}               # base DSP class, Conv1x1, Buffer, dspData, prewarm machinery
  film.h
  gating_activations.h
  get_dsp.{h,cpp}           # nam::get_dsp() entry points, Version/support checking
  linear.{h,cpp}            # registers "Linear"
  lstm.{h,cpp}              # registers "LSTM"
  model_config.h            # ModelConfig abstract base + ConfigParserRegistry (singleton)
  registry.h                # factory::Helper / FactoryConfig — external-registration compat shim
  ring_buffer.{h,cpp}
  slimmable.h                # SlimmableModel interface (SetSlimmableSize)
  util.{h,cpp}
  version.h                  # NEURAL_AMP_MODELER_DSP_VERSION_{MAJOR,MINOR,PATCH}
  wavenet/
    a2_fast.{h,cpp}          # A2 fast-path (guarded by #if defined(NAM_ENABLE_A2_FAST))
    detail.h
    model.{h,cpp}            # registers "WaveNet"; also contains #if NAM_ENABLE_A2_FAST dispatch
    params.h
    slimmable.{h,cpp}
Dependencies/
  eigen/            (git submodule -> https://gitlab.com/libeigen/eigen, bumped to 5.0.1)
  AudioDSPTools/     (git submodule -> https://github.com/sdatkinson/AudioDSPTools.git — resampler, IR, noise gate, tone stack, wav I/O; used by the *plugin*, not required by core itself)
  nlohmann/json.hpp  (single header, vendored directly in-tree, NOT a submodule)
  info.txt
CMakeLists.txt        # builds only the `tools` (loadmodel/benchmodel/run_tests/render); does NOT expose a reusable `NAM` library target
tools/CMakeLists.txt   # globs NAM/*.cpp + NAM/*/*.cpp itself
generate_weights_a2.py
example_models/, example_audio/, docs/, Doxyfile, .readthedocs.yaml
```

**Important:** the top-level `CMakeLists.txt` does **not** define an `add_library(NAM ...)` target you can `add_subdirectory()` and link against. It only builds internal CLI tools (`loadmodel`, `benchmodel`, `run_tests`, `render`, `bench_a2_fast`) by globbing `NAM/*.cpp` + `NAM/*/*.cpp`. Every third-party project observed (including the ones checked below) builds its own static-library target by globbing those same sources themselves.

## 2. License

MIT, Copyright (c) 2023 Steven Atkinson. Full permissive text present at repo root `LICENSE`. Safe to vendor/statically link in a closed-source commercial plugin; just keep the license notice with your distribution (standard MIT attribution).

## 3. C++ standard, sample type, key macros

- Requires **C++20** (`CMAKE_CXX_STANDARD 20` in the core's own CMakeLists; third-party projects must set `target_compile_features(<target> PUBLIC cxx_std_20)` on whatever target compiles NAM sources).
- Sample type: `NAM_SAMPLE` — **double by default**, switch to `float` (to match JUCE's `AudioBuffer<float>`) via compile definition:
  ```cmake
  target_compile_definitions(nam_core PUBLIC NAM_SAMPLE_FLOAT=1)
  ```
  (from `NAM/dsp.h`: `#ifdef NAM_SAMPLE_FLOAT #define NAM_SAMPLE float #else #define NAM_SAMPLE double #endif`). All observed real-world JUCE integrations define `NAM_SAMPLE_FLOAT`.
- `NAM_DEFAULT_MAX_BUFFER_SIZE` (default 4096) — buffer size used internally by `prewarm()` if `SetMaxBufferSize` hasn't been called yet; override via `-DNAM_DEFAULT_MAX_BUFFER_SIZE=N`.
- `NAM_UNKNOWN_EXPECTED_SAMPLE_RATE` = -1.0 — sentinel a model reports when it doesn't know its own training sample rate (older `.nam` files).
- `NAM_ENABLE_A2_FAST` — compile define that turns on the specialized fast WaveNet path for "A2" shaped models (see §7). It's `option(NAM_ENABLE_A2_FAST "..." ON)` **only inside the core repo's own top-level CMakeLists.txt** — since third-party projects glob sources directly and don't use that CMakeLists, **this define is NOT set unless you add it yourself**. Without it, `a2_fast.h`/`a2_fast.cpp` compile to nothing (the whole content is `#if defined(NAM_ENABLE_A2_FAST) ... #endif`) and all models — including A2-shaped ones — run through the generic WaveNet path (still correct, just not fast-path-optimized).
- On GCC (older libstdc++), `std::atomic<std::shared_ptr>` requires a C++20 feature-test macro; a real integration (`willbearfruits/signalpatch`) had to apply an upstream patch / define `NAM_NONATOMIC_SHARED_PTR` to build cleanly on GCC 11. Worth testing on your minimum-supported Linux GCC version.

## 4. Dependencies and vendoring

- **Eigen** — required, header-only, vendored as a git submodule at `Dependencies/eigen` (recently bumped to Eigen 5.0.1). You need its include path (`Dependencies/eigen`) on your include path; add it as a **SYSTEM** include (`target_include_directories(... SYSTEM ...)`) to suppress warnings from Eigen internals.
- **nlohmann/json** — required, vendored directly as `Dependencies/nlohmann/json.hpp` (single header, not a submodule) — NAM headers do `#include "json.hpp"`, so `Dependencies/nlohmann` must be on the include path.
- **AudioDSPTools** — a *separate* sdatkinson repo/submodule containing `dsp::ResamplingContainer`, `dsp::ImpulseResponse`, `dsp::noise_gate`, `dsp::wav`, tone-stack, etc. This is **not required by NeuralAmpModelerCore itself** — it's what the official *plugin* uses for resampling/IR/gate. If you only need "load a .nam and run it," you don't need AudioDSPTools at all; you only need it if you want to reuse the plugin's `ResamplingContainer`-based resampling wrapper (recommended — see §6).

### Recommended vendoring approaches (both seen in the wild)

**A. CMake `FetchContent` (most common in surveyed real repos)**
```cmake
include(FetchContent)
FetchContent_Declare(NeuralAmpModelerCore
    GIT_REPOSITORY https://github.com/sdatkinson/NeuralAmpModelerCore.git
    GIT_TAG 3cde95c354d5ba6da01316cad90b05cfc4855053   # pin a commit/tag, e.g. v0.5.4
    GIT_SUBMODULES "Dependencies/eigen"   # pull only Eigen; AudioDSPTools optional
    GIT_SHALLOW FALSE                     # GIT_SHALLOW + GIT_SUBMODULES can conflict on some CMake/git combos
)
FetchContent_GetProperties(NeuralAmpModelerCore)
if(NOT neuralampmodelercore_POPULATED)
    FetchContent_Populate(NeuralAmpModelerCore)
endif()
set(nam_dir "${neuralampmodelercore_SOURCE_DIR}")

file(GLOB nam_core_sources
    "${nam_dir}/NAM/*.cpp"
    "${nam_dir}/NAM/wavenet/*.cpp")

add_library(nam_core STATIC ${nam_core_sources})
target_include_directories(nam_core SYSTEM PUBLIC
    "${nam_dir}"
    "${nam_dir}/Dependencies/nlohmann"
    "${nam_dir}/Dependencies/eigen")
target_compile_features(nam_core PUBLIC cxx_std_20)
target_compile_definitions(nam_core PUBLIC NAM_SAMPLE_FLOAT=1)
# Optional: target_compile_definitions(nam_core PUBLIC NAM_ENABLE_A2_FAST)
```

**B. Vendored git submodule** (checked into `ThirdParty/NeuralAmpModelerCore`) with the identical `add_library(nam_core STATIC …)` glob pattern, avoiding network access during CI builds — used by `atij666studio/NeuralStage`.

Either way **there is no upstream CMake target name to link against** (`NAM`, `nam`, etc. do not exist as exported targets) — you always end up creating your own static-library target (commonly named `nam_core`) from globbed sources. This is a genuine gotcha worth flagging explicitly: don't `add_subdirectory(NeuralAmpModelerCore)` expecting a usable target — it will just try to build the (irrelevant, and by default broken outside its own build tree) `tools` targets.

### Critical linker gotcha: architecture self-registration gets stripped

Every architecture (`WaveNet`, `LSTM`, `ConvNet`, `Linear`, `SlimmableContainer`) registers itself into `nam::ConfigParserRegistry` via a **file-scope static object**, e.g. in `NAM/wavenet/model.cpp`:
```cpp
static nam::ConfigParserHelper _register_WaveNet("WaveNet", nam::wavenet::create_config);
```
Nothing in your own code ever directly references symbols in these `.cpp` files (you only call `nam::get_dsp(path)`), so a normal static-library link will happily **strip the whole translation unit**, and `get_dsp()` will throw `"No config parser registered for architecture: WaveNet"` at runtime. You must force-include the whole archive:
```cmake
if(APPLE)
    target_link_options(your_target PRIVATE "LINKER:-force_load,$<TARGET_FILE:nam_core>")
elseif(UNIX)
    target_link_options(your_target PRIVATE "LINKER:--whole-archive" "$<TARGET_FILE:nam_core>" "LINKER:--no-whole-archive")
elseif(MSVC)
    target_link_options(your_target PRIVATE "/WHOLEARCHIVE:nam_core.lib")
endif()
```
(Alternatively, on POSIX, `target_link_libraries(your_target PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>")` — a CMake 3.24+ generator-expression shortcut for the same thing, used by `willbearfruits/signalpatch`.) This bit every real integration surveyed.

## 5. Exact API — loading a model and processing audio

Header: `NAM/get_dsp.h` (`namespace nam`).

```cpp
enum class Supported { NO = 0, PARTIAL = 1, YES = 2 };

struct DspLoadOptions {
  std::optional<bool> prewarm = std::nullopt;  // nullopt = leave current thread-local default;
                                                // false = skip prewarm in get_dsp(); true = force it
};

// Load from a .nam file path:
std::unique_ptr<DSP> get_dsp(const std::filesystem::path config_filename,
                              DspLoadOptions options = DspLoadOptions());

// Load from file, and also retrieve the raw dspData (arch name/config/weights/metadata):
std::unique_ptr<DSP> get_dsp(const std::filesystem::path config_filename, dspData& returnedConfig,
                              DspLoadOptions options = DspLoadOptions());

// Load from an already-parsed dspData / raw JSON (e.g. embedded binary resource):
std::unique_ptr<DSP> get_dsp(dspData& conf, DspLoadOptions options = DspLoadOptions());
std::unique_ptr<DSP> get_dsp(const nlohmann::json& config, dspData& returnedConfig, DspLoadOptions options = {});
std::unique_ptr<DSP> get_dsp(const nlohmann::json& config, DspLoadOptions options = DspLoadOptions());

double get_sample_rate_from_nam_file(const nlohmann::json& j); // -1 if unknown
```

`nam::DSP` (header `NAM/dsp.h`) — the object returned by `get_dsp`:

```cpp
class DSP {
public:
  DSP(int in_channels, int out_channels, double expected_sample_rate);
  virtual void prewarm();                                    // NOT real-time safe (allocates!)
  virtual void process(NAM_SAMPLE** input, NAM_SAMPLE** output, int num_frames);
  double GetExpectedSampleRate() const;
  int NumInputChannels() const;
  int NumOutputChannels() const;

  double GetInputLevel();   bool HasInputLevel();     // dBu RMS, 0dBFS-peak-1kHz-sine convention
  double GetOutputLevel();  bool HasOutputLevel();
  double GetLoudness() const; bool HasLoudness() const; // throws if !HasLoudness()

  virtual int GetPrewarmSamples();                     // # samples needed to "settle"; arch-specific

  virtual void Reset(double sampleRate, int maxBufferSize);   // calls prewarm() by default!
  void ResetAndPrewarm(double sampleRate, int maxBufferSize); // DEPRECATED, removed in v0.6.0
  virtual void SetPrewarmOnReset(bool prewarmOnReset);
  bool GetPrewarmOnReset() const;

  void SetInputLevel(double dBu); void SetLoudness(double dB); void SetOutputLevel(double dBu);
  int GetMaxBufferSize() const;
protected:
  virtual void SetMaxBufferSize(int maxBufferSize);
};
```

`process()` signature is **double-pointer-per-channel** (`NAM_SAMPLE** input`), i.e. `input[channel][frame]`, not interleaved — you must build a small `float* [nChannels]` (or `NAM_SAMPLE*[]`) pointer array per block pointing into your JUCE `AudioBuffer`'s channel data (this maps trivially onto `AudioBuffer<float>::getArrayOfWritePointers()` if you use `NAM_SAMPLE_FLOAT`).

`Reset(sampleRate, maxBufferSize)` — call this whenever host sample-rate or max block size changes (JUCE `prepareToPlay`); by default it **also calls `prewarm()`**, which is real-time-unsafe (allocates `std::vector`s and runs the network on synthetic zero input in `mMaxBufferSize`-sized chunks until `GetPrewarmSamples()` samples have been processed) — so `Reset`/model construction/loading must never happen on the audio thread. Use `SetPrewarmOnReset(false)` (or `DspLoadOptions{.prewarm=false}` at `get_dsp()` time) if you want to control prewarm timing separately (e.g. prewarm once on your background thread right after loading, then disable further auto-prewarm on subsequent `Reset()` calls triggered by sample-rate changes).

Thread-local default for prewarm-on-`Reset()` is `true` (`thread_local bool gPrewarmOnResetDefault = true;` in `dsp.cpp`); `nam::ScopedPrewarmOnResetDefault` is an RAII helper to override it for newly-constructed `DSP` objects on the calling thread only.

### Legacy directory loader
`std::unique_ptr<DSP> get_dsp_legacy(const std::filesystem::path dirname);` — old directory-based model format; not needed for current `.nam` files.

## 6. Expected sample rate & resampling (official plugin's `ResamplingNAM` pattern)

Fetched verbatim from `NeuralAmpModelerPlugin/NeuralAmpModeler/NeuralAmpModeler.h` (worth copying near-verbatim into a JUCE plugin, with `dsp::ResamplingContainer` swapped for your own polyphase/lanczos resampler if you don't want the AudioDSPTools dependency):

```cpp
// Some old .nam files don't encode a sample rate at all (report <= 0).
// Convention: assume 48kHz for those.
double GetNAMSampleRate(const std::unique_ptr<nam::DSP>& model) {
  const double assumedSampleRate = 48000.0;
  const double reportedEncapsulatedSampleRate = model->GetExpectedSampleRate();
  return reportedEncapsulatedSampleRate <= 0.0 ? assumedSampleRate : reportedEncapsulatedSampleRate;
}

class ResamplingNAM : public nam::DSP {
public:
  ResamplingNAM(std::unique_ptr<nam::DSP> encapsulated, double expected_sample_rate)
  : nam::DSP(encapsulated->NumInputChannels(), encapsulated->NumOutputChannels(), expected_sample_rate)
  , mEncapsulated(std::move(encapsulated))
  , mResampler(GetNAMSampleRate(mEncapsulated))
  {
    mBlockProcessFunc = [&](NAM_SAMPLE** in, NAM_SAMPLE** out, int n) { mEncapsulated->process(in, out, n); };
    if (mEncapsulated->HasLoudness())  SetLoudness(mEncapsulated->GetLoudness());
    if (mEncapsulated->HasInputLevel()) SetInputLevel(mEncapsulated->GetInputLevel());
    if (mEncapsulated->HasOutputLevel()) SetOutputLevel(mEncapsulated->GetOutputLevel());
    Reset(expected_sample_rate, /*maxBlockSize=*/2048); // conservative
  }

  void prewarm() override { mEncapsulated->prewarm(); }

  void process(NAM_SAMPLE** input, NAM_SAMPLE** output, int num_frames) override {
    if (num_frames > mMaxExternalBlockSize) throw std::runtime_error("More frames than expected!");
    if (!NeedToResample()) mEncapsulated->process(input, output, num_frames);
    else mResampler.ProcessBlock(input, output, num_frames, mBlockProcessFunc);
  }

  int GetLatency() const { return NeedToResample() ? mResampler.GetLatency() : 0; }

  void Reset(double sampleRate, int maxBlockSize) override {
    mExpectedSampleRate = sampleRate;
    mMaxExternalBlockSize = maxBlockSize;
    mResampler.Reset(sampleRate, maxBlockSize);
    const double upRatio = sampleRate / GetEncapsulatedSampleRate();
    const int maxEncapsulatedBlockSize = (int)std::ceil((double)maxBlockSize / upRatio);
    mEncapsulated->ResetAndPrewarm(sampleRate, maxEncapsulatedBlockSize);
  }

  double GetEncapsulatedSampleRate() const { return GetNAMSampleRate(mEncapsulated); }
private:
  bool NeedToResample() const { return GetExpectedSampleRate() != GetEncapsulatedSampleRate(); }
  std::unique_ptr<nam::DSP> mEncapsulated;
  dsp::ResamplingContainer<NAM_SAMPLE, 1, 12> mResampler;  // from AudioDSPTools; 12 = filter length param
  int mMaxExternalBlockSize = 0;
  std::function<void(NAM_SAMPLE**, NAM_SAMPLE**, int)> mBlockProcessFunc;
};
```

Key takeaways:
- Models don't run at the host's sample rate in general — they run at whatever rate they were trained/exported at (commonly 48kHz), reported via `DSP::GetExpectedSampleRate()`. `-1`/`<=0` means "unknown", and the plugin's convention is to assume 48kHz in that case.
- `ResamplingNAM` wraps a loaded `nam::DSP` and transparently up/downsamples host audio to the model's native rate, runs the model, resamples back — using `dsp::ResamplingContainer<SampleType, NumChannels, FilterOrderParam>` from AudioDSPTools (a Lanczos-style resampler pulled from iPlug2's WDL).
- `ResamplingNAM::GetLatency()` reports resampler-introduced latency (0 when host SR == model SR) — feed this into your `AudioProcessor::setLatencySamples()`.
- `Reset()` resizes the *encapsulated* model's max buffer size based on the up/down sample ratio, not the raw host block size — important if you write your own resampler wrapper.
- If you'd rather avoid the AudioDSPTools dependency (WDL/iPlug2 headers, needs `NOMINMAX`/`WIN32_LEAN_AND_MEAN`/`DEFAULT_BLOCK_SIZE` defines on Windows per real integrations), you can substitute JUCE's own `juce::dsp::Oversampling`/`juce::LagrangeInterpolator`/`juce::WindowedSincInterpolator` for the resampling step and reimplement the same `ResamplingNAM`-style wrapper — the *pattern* (encapsulate `nam::DSP`, override `process`/`Reset`/`GetLatency`) is what matters, not the specific resampler.

## 7. A2 architecture support

- There is **no separate `"architecture": "A2"` JSON string** — A2 models are still tagged `"architecture": "WaveNet"` in the `.nam` file; "A2" (standard, 8-channel, and "A2 nano", 3-channel) is a *shape* of WaveNet config that `wavenet::create_config` detects automatically (`is_a2_shape()`) and, when detected **and** the build was compiled with `NAM_ENABLE_A2_FAST` defined, routes to a hand-optimized fixed-topology fast-path implementation (`NAM/wavenet/a2_fast.{h,cpp}`, class `A2FastModel<Channels>`) instead of the generic `WaveNet` class — functionally identical output, just faster.
- Origin: **PR #251, "[FEATURE] A2 fast-path WaveNet for A2 nano + A2 standard"**, merged **2026-04-20** (commit `a7037e5616...`). First appeared in the **v0.5.x** release line (current: v0.5.4, 2026-06-24).
- Several bugfixes landed after: PR #273 (relax A2 head scale validation, 2026-06-02), PR #300 (fix A2 fast-path prewarm count to match generic WaveNet, 2026-06-25), PR #301 (reject conditioned/gated configs in the A2 fast-path detector, 2026-06-25), PR #295 (a since-reverted attempt at further A2 fast optimizations, 2026-06-19). Open issue #303 (2026-06-29) reports audio crackling when loading certain A1 models after A2 models — worth checking current status if you support hot-swapping models.
- Structural constants baked into `a2_fast.h`: 23 layers per layer-array (`kNumLayers`), kernel-size pattern `{6,6,6,6,6,6,6,6,6,6,6,6,6,6,15,15,6,6,6,6,6,6,6}`, LeakyReLU slope 0.01, head-rechannel conv kernel size 16.
- If you want the perf win, define `NAM_ENABLE_A2_FAST` on your `nam_core` target. If your app needs to hot-swap/morph model *weights* live (e.g. `WaveNet::set_weights_()`), note that `AllAmpsOnMePlugin` deliberately **excludes `a2_fast.cpp` from the build** (`list(FILTER nam_core_sources EXCLUDE REGEX "a2_fast\\.cpp$")`) and forces all models through the generic WaveNet path specifically because the fast-path model doesn't support the same runtime weight-swapping code path — a concrete gotcha if you need that use case.

## 8. Threading rules / atomic swap pattern (from the official plugin, iPlug2-based but framework-agnostic pattern)

There is **no worker-thread pool or `std::thread` inside NAM itself or the official plugin** — model loading (`nam::get_dsp(path)`, which does file I/O, JSON parsing, weight deserialization, and — unless disabled — a real-time-unsafe `prewarm()`) is simply executed **synchronously on whatever non-audio thread invokes it** (in the official plugin: the UI/message thread, inside the file-browser's completion callback). The critical rule is just: **never call `get_dsp()`, `Reset()`, or `prewarm()` from `processBlock`/the audio callback.**

Synchronization with the audio thread uses a **staging + flag pattern**, not full lock-free atomics on the pointer itself:

```cpp
// Members (NeuralAmpModeler.h):
std::unique_ptr<ResamplingNAM> mModel;         // "live" — touched only by audio thread
std::unique_ptr<ResamplingNAM> mStagedModel;   // written by UI/loader thread, consumed by audio thread
std::atomic<bool> mShouldRemoveModel = false;  // UI thread sets this to request unload
std::atomic<bool> mNewModelLoadedInDSP = false;
std::atomic<bool> mModelCleared = false;

// UI/message-thread callback (file browser "load" button):
std::string NeuralAmpModeler::_StageModel(const WDL_String& modelPath) {
  auto model = nam::get_dsp(std::filesystem::u8path(modelPath.Get()));      // I/O + prewarm here — OFF audio thread
  // ...validate channel counts...
  auto temp = std::make_unique<ResamplingNAM>(std::move(model), GetSampleRate());
  temp->Reset(GetSampleRate(), GetBlockSize());
  mStagedModel = std::move(temp);   // plain, non-atomic pointer write
  return "";
}

// Audio thread, first thing every ProcessBlock():
void NeuralAmpModeler::_ApplyDSPStaging() {
  if (mShouldRemoveModel) { mModel = nullptr; mShouldRemoveModel = false; _UpdateLatency(); }
  if (mStagedModel != nullptr) {
    mModel = std::move(mStagedModel);   // pointer-sized move; audio thread now owns it
    mStagedModel = nullptr;
    mNewModelLoadedInDSP = true;
    _UpdateLatency();
  }
}

void NeuralAmpModeler::ProcessBlock(...) {
  ...
  _ApplyDSPStaging();                    // called at top of every block, before touching mModel
  if (mModel != nullptr) mModel->process(triggerOutput, mOutputPointers, nFrames);
  else _FallbackDSP(...);                // pass-through if nothing loaded
  ...
}
```

Notes / caveats to carry into a JUCE port:
- `mStagedModel`/`mModel` themselves are **plain `unique_ptr`s, not `std::atomic<shared_ptr>` or lock-free** — correctness relies on: (1) single-writer (UI thread) / single-reader (audio thread) discipline, (2) `_ApplyDSPStaging()` being the *only* place `mModel` is reassigned on the audio thread, called once at the very start of the block before any read, and (3) pointer-sized writes being effectively atomic on all mainstream target architectures/compilers in practice (technically a benign data race under the strict C++ memory model — some forks patch this to `std::atomic<std::shared_ptr<...>>` where the toolchain supports it; that's exactly the GCC C++20 feature-gate issue mentioned in v0.5.4's changelog and in `signalpatch`'s patch).
- For a stricter JUCE implementation, prefer `juce::AbstractFifo`, a real lock-free SPSC pointer swap (`std::atomic<Model*>` with old-model reclaimed via a second staged/garbage slot, as done here for `mShouldRemoveModel`), or `std::atomic<std::shared_ptr<nam::DSP>>` (C++20) if your toolchain supports it cleanly.
- The old model isn't deleted on the audio thread in a special way here — `mModel = std::move(mStagedModel)` on the audio thread will run `~ResamplingNAM()` for the previous model's destructor **on the audio thread** when the local `unique_ptr` is replaced — actually here it's the reverse: the *new* `unique_ptr` overwrites `mModel`, so the *old* model's destructor runs synchronously in `_ApplyDSPStaging()`, i.e., **on the audio thread**. This is itself a subtle real-time-safety gotcha in the reference implementation (freeing memory / running a `unique_ptr` dtor on the audio thread) that a more careful JUCE integration might want to fix by deferring the old model's destruction to a background/message thread (e.g., via `juce::MessageManager::callAsync` capturing the old pointer, or an async deleter queue).
- `_UpdateLatency()` (called from `_ApplyDSPStaging`) recomputes `mModel->GetLatency()` (delegates to `ResamplingNAM::GetLatency()`, i.e., resampler latency) and calls `SetLatency()`/`setLatencySamples()` — always keep this in sync after any model swap or `OnReset`/`prepareToPlay`.

## 9. Sample rate handling summary (JUCE mapping)

- In JUCE `prepareToPlay(sampleRate, samplesPerBlock)`: call `model->Reset(sampleRate, samplesPerBlock)` (or, if using the `ResamplingNAM` wrapper, that call also re-derives/resizes the encapsulated model's internal block size based on the resample ratio).
- `GetExpectedSampleRate()` tells you the model's native rate; compare against your host rate to decide if resampling is needed (`ResamplingNAM::NeedToResample()` pattern above).
- Report resampler latency via `AudioProcessor::setLatencySamples(model->GetLatency())` (0 if no resampling needed).
- If host block size can exceed what you configured in `Reset(sampleRate, maxBlockSize)`, `ResamplingNAM::process()` will `throw std::runtime_error` — size your `maxBlockSize` generously (official plugin uses 2048 as a "conservative" constant, then also handles `OnReset`/`GetBlockSize()` reactively) and/or process in sub-chunks if your host can hand you arbitrarily large buffers.

## 10. Denormals / real-time safety gotchas

- The official plugin explicitly disables denormals for the whole `ProcessBlock`:
  ```cpp
  std::fenv_t fe_state;
  std::feholdexcept(&fe_state);
  disable_denormals();      // iPlug2 utility (sets FTZ/DAZ via platform intrinsics)
  ... process ...
  std::feupdateenv(&fe_state);
  ```
  In JUCE, the direct equivalent is `juce::ScopedNoDenormals noDenormals;` at the top of `processBlock()` — do this; NAM's recurrent/convolutional activations (tanh, sigmoid chains in LSTM/WaveNet) are exactly the kind of workload that can hit denormal-induced slowdowns on silence/near-silence input.
- **`prewarm()` is not real-time safe** — it allocates `std::vector`s every call and runs the model on a synthetic zero-input buffer; never call it (directly, or indirectly via `Reset()`'s default prewarm-on-reset behavior) from the audio thread. Load + `Reset()`/prewarm on a background/message thread, then hand the finished object to the audio thread via the staging pattern in §8.
- **Eigen alignment "sharp edge"** (documented in the core README): because DSP subclasses hold Eigen objects (`Eigen::MatrixXf` etc.) as members, certain compilers/optimization levels can misalign them, risking crashes with vectorized instructions. Workaround (with a performance cost) if you hit this: define `EIGEN_MAX_ALIGN_BYTES 0` and `EIGEN_DONT_VECTORIZE`. Tracked as core issue #67, which was closed after the Eigen submodule was bumped to 5.0.1 — likely resolved in current `main`/v0.5.x, but worth a smoke test on your actual toolchain/optimization flags before shipping, especially on 32-bit ARM or older MSVC.
- **Mono-only assumption in the reference plugin**: the official plugin hard-codes `constexpr size_t kNumChannelsInternal = 1;` and explicitly rejects models with `NumInputChannels() != 1` or `NumOutputChannels() != 1` in `_StageModel()`. NAM's core `DSP` class itself is generic over channel count (constructor takes `in_channels`/`out_channels`), but **all shipped `.nam` model architectures are trained/exported mono** — collapse your host's input to mono before feeding NAM, and (if stereo output is desired) duplicate the mono NAM output to both channels afterward, exactly like the reference plugin's `_ProcessInput`/`_ProcessOutput` mono-collapse/expand helpers.
- Windows-specific: AudioDSPTools' `ResamplingContainer.h` pulls in WDL headers that transitively include `<windows.h>`; define `NOMINMAX` and `WIN32_LEAN_AND_MEAN` (and, per one real integration, `DEFAULT_BLOCK_SIZE=1024`, an iPlug2 constant the resampler code expects) on your target if you reuse it.
- GCC 12 hard-errors (vs. GCC 13's warning) on a using-alias-shadows-class-name construct inside `ResamplingContainer.h`; real integrations work around it with `target_compile_options(... PRIVATE -fpermissive)` on Linux/non-Apple builds that touch that header.
- `run_tests`'s own CMake comments flag that Eigen's `GeneralBlockPanelKernel.h` throws warnings-as-errors in debug builds on some compilers — the core repo itself disables `-Werror` for `dsp.cpp`/`conv1d.cpp`; consider the same if you build with `-Werror` globally.

## 11. Loudness / level calibration (bonus, referenced by the official plugin's gain-staging, not asked for directly but load-bearing for a faithful port)

- `DSP::HasLoudness()`/`GetLoudness()` — dB loudness of a "typical" input, used by the plugin's "Normalized" output mode: `outputGainDB += (targetLoudness(-18dB) - modelLoudness)`.
- `DSP::HasInputLevel()`/`GetInputLevel()`, `HasOutputLevel()`/`GetOutputLevel()` — dBu RMS calibration levels (0dBFS peak = a defined dBu for a 1kHz sine), used by the plugin's "Calibrated" output mode and input-calibration feature (`kCalibrateInput`/`kInputCalibrationLevel` params) to match physical amp input/output levels.
- All three are optional per-model metadata (`Has*()` guards) — not every `.nam` file encodes them.

## 12. Primary source references
- Core repo: https://github.com/sdatkinson/NeuralAmpModelerCore (MIT)
- Official plugin: https://github.com/sdatkinson/NeuralAmpModelerPlugin (iPlug2-based, VST3/AU) — see `NeuralAmpModeler/NeuralAmpModeler.h` and `.cpp` for the full `ResamplingNAM` + staging pattern (not JUCE, but framework-agnostic DSP wiring).
- AudioDSPTools (resampler/IR/gate/tonestack used by the plugin): https://github.com/sdatkinson/AudioDSPTools
- A2 fast-path introduction: PR #251 (https://github.com/sdatkinson/NeuralAmpModelerCore/pull/251), merged 2026-04-20.
- Real-world JUCE+CMake integration examples reviewed for the vendoring/linking patterns above: `kylenamt/AllAmpsOnMePlugin`, `willbearfruits/signalpatch`, `atij666studio/NeuralStage` (all public GitHub repos found via GitHub code search for `NeuralAmpModelerCore` + `CMakeLists.txt`).
