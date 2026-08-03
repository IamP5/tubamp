
# JUCE 8 + CMake macOS AU Plugin for Logic Pro — Research Notes (Aug 2026)

## 0. Important version note (read first)
As of the current date, **JUCE has moved past the 8.x line**: `9.0.0` was tagged **2026‑07‑21**, and `8.0.15` (also 2026‑07‑21) is the **final/last release of the 8.x series**. If the goal is specifically "JUCE 8," pin to `8.0.15`. If greenfield and open to the newest framework, `9.0.0` is now current — but since the task explicitly asks for JUCE 8, all guidance below targets **8.0.15** unless noted.

Recent 8.x tags (GitHub API, `juce-framework/JUCE`):
- 8.0.15 — 2026‑07‑21 (last 8.x)
- 8.0.14 — 2026‑06‑22
- 8.0.13 — 2026‑05‑19
- 8.0.12 — 2025‑12‑16
- 8.0.11 — 2025‑12‑01
- 8.0.10 — 2025‑09‑15
- 8.0.9 — 2025‑09‑01
- 8.0.8 — 2025‑06‑02
- 8.0.7 — 2025‑04‑08
- 8.0.6 — 2025‑01‑10 (first widely-adopted stable 8.0 after the 8.0.0‑8.0.5 early releases)

Sources: https://github.com/juce-framework/JUCE/releases , https://api.github.com/repos/juce-framework/JUCE/releases

## 1. CMake integration: FetchContent vs submodule

**Submodule (JUCE's own official pattern, used in `examples/CMake/AudioPlugin`)**:
```cmake
add_subdirectory(JUCE)   # JUCE checked out as a git submodule at ./JUCE
```

**FetchContent** (community-preferred for reproducible, pinned builds without submodule friction — Pamplejuce / Melatonin-style templates use this):
```cmake
cmake_minimum_required(VERSION 3.22)
project(MyPlugin VERSION 1.0.0)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(JUCE)
```
Then `juce_add_plugin(...)` and `target_link_libraries(...)` as usual. `find_package(JUCE CONFIG REQUIRED)` is the third option if JUCE was installed system-wide (least common for indie/personal projects).

Sources: https://github.com/juce-framework/JUCE/blob/master/docs/CMake%20API.md , https://melatonin.dev/blog/how-to-use-cmake-with-juce/

## 2. `juce_add_plugin()` — options needed for Logic-visible AU

Minimal AU-capable target (from JUCE's own `examples/CMake/AudioPlugin/CMakeLists.txt`):
```cmake
juce_add_plugin(AudioPluginExample
    PLUGIN_MANUFACTURER_CODE Juce
    PLUGIN_CODE Dem0
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "Audio Plugin Example"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)
```

Full/relevant option reference from `docs/CMake API.md`:
- **FORMATS**: space-separated list, valid values `Standalone Unity VST3 AU AUv3 AAX VST LV2`. For Logic Pro you need at minimum `AU` (Logic does not load VST/VST3 — AU only, plus AUv3 for sandboxed/iPad-adjacent use, rarely needed for desktop Logic).
- **PLUGIN_MANUFACTURER_CODE**: 4-char company ID. **For AU compatibility must contain at least one upper-case letter.**
- **PLUGIN_CODE**: 4-char unique plugin ID. **For AU compatibility must contain exactly one upper-case letter.**
- **AU_MAIN_TYPE**: valid values are `kAudioUnitType_Effect`, `kAudioUnitType_FormatConverter`, `kAudioUnitType_Generator`, `kAudioUnitType_MIDIProcessor`, `kAudioUnitType_Mixer`, `kAudioUnitType_MusicDevice`, `kAudioUnitType_MusicEffect`, `kAudioUnitType_OfflineEffect`, `kAudioUnitType_Output`, `kAudioUnitType_Panner`. For a straightforward audio effect this maps to the 4-char OSType `'aufx'` (i.e. set `AU_MAIN_TYPE kAudioUnitType_Effect`).
- **VST3_CATEGORIES**: `Fx Instrument Analyzer Delay Distortion Drum Dynamics EQ External Filter Generator Mastering Modulation Mono Network NoOfflineProcess OnlyOfflineProcess OnlyRT Pitch Shift Restoration Reverb Sampler Spatial Stereo Surround Synth Tools Up-Downmix` (not AU-relevant but needed if `VST3` is also in FORMATS).
- **BUNDLE_ID**: `com.yourcompany.productname` reverse-DNS string — required for a real AU bundle identity, distinct from AU manufacturer/plugin 4-char codes.
- **COPY_PLUGIN_AFTER_BUILD**: `TRUE`/`FALSE`, default `FALSE`. Set `TRUE` to auto-install after every build.
- **AU_COPY_DIR** / **VST3_COPY_DIR**: destination paths for the post-build copy. Default AU copy dir on macOS is `~/Library/Audio/Plug-Ins/Components`; can override to the system-wide `/Library/Audio/Plug-Ins/Components` if installing for all users.
- **NEEDS_CURL**, **NEEDS_WEB_BROWSER**: mainly Linux-relevant (link WebKit/Curl); leave `FALSE` unless you hit link errors, or set `NEEDS_WEB_BROWSER TRUE` if embedding `WebBrowserComponent`.
- **HARDENED_RUNTIME_ENABLED**, **HARDENED_RUNTIME_OPTIONS** (`com.apple.security.*` entitlement keys), **APP_SANDBOX_ENABLED/OPTIONS**: macOS notarization/entitlement plumbing — needed later for distribution, not for local Logic testing.

### Concrete example for a Logic-Pro-visible AU effect plugin
```cmake
cmake_minimum_required(VERSION 3.22)
project(MyDelayFX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(MyDelayFX
    COMPANY_NAME "YourCompany"
    BUNDLE_ID "com.yourcompany.mydelayfx"
    PLUGIN_MANUFACTURER_CODE Ybgs        # >=1 uppercase letter
    PLUGIN_CODE Mdfx                     # exactly 1 uppercase letter
    FORMATS AU VST3 Standalone
    AU_MAIN_TYPE kAudioUnitType_Effect   # -> 'aufx'
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    PRODUCT_NAME "My Delay FX"
    COPY_PLUGIN_AFTER_BUILD TRUE
    AU_COPY_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/Components"
    VST3_COPY_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")

target_sources(MyDelayFX PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(MyDelayFX PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(MyDelayFX PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
```
`'aufx'` is the standard 4-char AU "type" for an effect; `AU_MAIN_TYPE kAudioUnitType_Effect` is how JUCE's CMake maps to it. Manufacturer/subtype codes (`PLUGIN_MANUFACTURER_CODE` / `PLUGIN_CODE`) become the other two OSType 4-char codes `auval` checks against.

Sources: https://github.com/juce-framework/JUCE/blob/master/docs/CMake%20API.md , https://github.com/juce-framework/JUCE/blob/master/examples/CMake/AudioPlugin/CMakeLists.txt

## 3. Getting the AU to actually show up in Logic Pro

1. **Build location**: JUCE's AU wrapper builds a `.component` bundle; with `COPY_PLUGIN_AFTER_BUILD TRUE` + `AU_COPY_DIR`, it's copied to `~/Library/Audio/Plug-Ins/Components/` (per-user) or `/Library/Audio/Plug-Ins/Components/` (system-wide, needs sudo). Logic scans both locations.
2. **Validate with auval** before ever opening Logic:
   ```
   auval -v aufx Mdfx Ybgs
   ```
   (`aufx` = type from `AU_MAIN_TYPE kAudioUnitType_Effect`, `Mdfx` = `PLUGIN_CODE`, `Ybgs` = `PLUGIN_MANUFACTURER_CODE`). `auval -a` lists all registered AUs if you're unsure of codes. Logic performs essentially the same validation pass internally at scan time, so a failing `auval` = a plugin Logic will reject or blacklist.
3. **Force a rescan** after every rebuild — macOS/Logic caches AU component registration aggressively:
   ```
   killall -9 AudioComponentRegistrar
   ```
   then relaunch Logic (or use Logic's own Plug-in Manager → Reset & Rescan Selection if the cache is stubborn). Some devs report needing a full logout/login in persistent-cache edge cases.
4. **Codesigning for local dev**: **Ad-hoc signing is fine** for local Logic testing on Apple Silicon/modern macOS, because Logic's hardened runtime / Gatekeeper checks generally block *unsigned* code more than ad-hoc-signed code, but arm64 in particular effectively requires *some* signature (even ad hoc) to run at all — unsigned arm64 Mach-O binaries are refused by the kernel's code-signing enforcement.
   ```
   sudo codesign --force --deep --sign - ~/Library/Audio/Plug-Ins/Components/MyDelayFX.component
   killall -9 AudioComponentRegistrar
   ```
   A real Developer ID signature + notarization is only required for **distribution** to other users (Gatekeeper on a machine that isn't yours), not for building/running/testing on your own dev Mac.
5. **Version string format**: Logic can flag AU version as "invalid" if the CMake `project(... VERSION x.y.z)` isn't a clean 3-part semantic version (e.g. use `1.0.0`, not `1.0.0.0` or missing components) — a known JUCE-forum gotcha.
6. Common failure mode reported on the JUCE forum: plugin builds fine and passes `auval`, works in other hosts (Ableton), but doesn't appear in Logic — usually resolved by clean rescans, checking Logic isn't filtering it out via Plug-in Manager's per-plugin enable toggle, and confirming the bundle actually landed in one of the two scanned Components folders (not a stray custom path).

Sources: https://forum.juce.com/t/au-plugin-version-in-logic-pro-x/46992 , https://forum.juce.com/t/cmake-based-plugin-not-showing-up-in-logic-9-and-garage-band-10-3-although-working-in-logic-pro-x-and-auval-passes/43007 , https://forum.juce.com/t/plugin-passes-auval-test-but-doesnt-show-up-in-logic-pro/56954 , https://gist.github.com/olilarkin/8f378d212b0a59944d84f9f47061d70f , https://moonbase.sh/articles/debugging-your-audio-unit-plugin-with-auval-aka-auvaltool/

## 4. JUCE 8 licensing (personal/open project) — as of 2026

Current tiers on juce.com/get-juce:
- **Starter (free)**: usable for closed-source or open/personal projects; **no splash-screen requirement** — this is a real change vs. JUCE 5/6/7, where the Personal tier forced a "Made with JUCE" splash on every plugin/app launch. Multiple community threads (forum.juce.com, forum.hise.audio) confirm JUCE 8's Starter tier drops the mandatory splash screen. One community source cites an approximate revenue ceiling around $20k/year for Starter-tier commercial use (verify exact current cap on juce.com before shipping commercially — pricing pages change and this figure wasn't confirmed verbatim on the fetched page).
- **Indie**: $40/mo subscription or $800 one-time perpetual; revenue/funding cap "up to $300,000"; 12-month minimum commitment for perpetual.
- **Pro**: $175/mo subscription or $3,500 one-time perpetual; no revenue cap.
- **Educational**: free, revenue cap "up to $20,000."
- **Dual license / AGPLv3**: JUCE is also offered under AGPLv3. Fully compliant open-source projects (source published, AGPL-compatible) can use JUCE without a commercial license at all — the natural fit for a personal open-source plugin project, since AGPLv3 doesn't cap revenue but requires releasing your full source under AGPL.

**Practical takeaway for a personal/open project**: either (a) use the free **Starter** tier if you intend to keep the plugin closed-source/hobby-scale (no splash screen required in JUCE 8), or (b) use the **AGPLv3** license path if you're happy open-sourcing the plugin's code, which removes any revenue-tier ambiguity entirely. Confirm exact current thresholds at https://juce.com/get-juce/ since pricing/cap figures are the part of vendor pages most likely to drift.

Sources: https://juce.com/get-juce/ , https://forum.hise.audio/topic/14433/juce-8-starter-license-free-tier-hise-commercial-closed-source-license , https://forum.juce.com/t/splash-screen-for-personal-juce-5-license/21908 , https://forum.juce.com/t/licensing-for-a-free-plug-in/66179

## 5. Modern JUCE APIs worth using

**Parameters — `juce::AudioProcessorValueTreeState` (APVTS)**
- Central pattern for JUCE 8 plugins: construct with `ParameterLayout` (a variadic/vector-based set of `RangedAudioParameter`s or `AudioProcessorParameterGroup`s), each parameter owned via `std::unique_ptr`.
- Built on `juce::ValueTree`, so you get free undo support, XML serialize/deserialize (`copyState()`/`replaceState()` for `getStateInformation`/`setStateInformation`), and thread-safe listener notifications.
- UI binding via `SliderAttachment`, `ComboBoxAttachment`, `ButtonAttachment` (and Web-equivalents, see below) — keeps UI and DSP parameter values in sync automatically.
- One APVTS per processor instance; lifetime must match the processor's lifetime.
- Tutorial: https://juce.com/tutorials/tutorial_audio_processor_value_tree_state/

**`juce::dsp` module — effect building blocks**
The `juce_dsp` module ships ready-made, sample-type-templated processors, confirmed present and documented in JUCE 8/master `docs.juce.com`:
- `dsp::Compressor<SampleType>` — threshold/ratio/attack/release compressor.
- `dsp::NoiseGate<SampleType>`.
- `dsp::Chorus<SampleType>`, `dsp::Phaser<SampleType>` — modulation effects.
- `dsp::Reverb` (wraps the classic Freeverb-style JUCE reverb as a `dsp`-compatible processor).
- `dsp::DelayLine<SampleType, InterpolationType>` — for delay/echo effects, with selectable interpolation (Linear/Lagrange3rd/Thiran/None).
- `dsp::IIR::Filter` / `dsp::IIR::Coefficients` and `dsp::StateVariableTPTFilter<SampleType>` — filters (the TPT SVF is the modern topology-preserving-transform recommendation over the older `StateVariableFilter`, which is deprecated).
- `dsp::Convolution` — for loading impulse responses (cab/room IRs); see tutorial `tutorial_dsp_convolution`.
- `dsp::Oversampling<SampleType>` — for clean nonlinear processing (drive/saturation) at higher internal sample rates.
- All fit into `dsp::ProcessorChain` and operate on `dsp::AudioBlock`/`dsp::ProcessContext` for a consistent prepare/process/reset lifecycle.
- Full namespace reference: https://docs.juce.com/master/namespacejuce_1_1dsp.html ; demo project: `examples/Plugins/DSPModulePluginDemo.h` in the JUCE repo.

**`WebBrowserComponent` — JUCE 8's WebView UI system**
- Major JUCE 8 feature (juce.com/blog/juce-8-feature-overview-webview-uis/): lets you build plugin UIs as HTML/CSS/JS (e.g. React) instead of native `juce::Component` graphics.
- Bidirectional native↔JS bridge: `evaluateJavascript()`, `emitEventIfBrowserIsVisible()`, `WebBrowserComponent::Options::withNativeIntegrationEnabled()` injects a `window.__JUCE__.backend` shim; `withNativeFunction()` exposes named C++ callbacks callable from JS as async Promises.
- Parameter binding equivalents to APVTS attachments: `WebSliderRelay`/`WebSliderParameterAttachment`, `WebToggleButtonRelay`/`...Attachment`, `WebComboBoxRelay`/`...Attachment` — JS side reads via `Juce.getSliderState()` etc.
- Serving local UI assets: `withResourceProvider()` acts as an embedded lightweight web server for bundled HTML/JS/CSS (or point at a dev server e.g. `localhost:3000` for hot-reload during development).
- **Navigating to arbitrary external sites (e.g. tone3000.com)**: nothing in the docs/tutorials prevents it — `WebBrowserComponent` is a full OS-native browser view (WKWebView on macOS, WebView2 on Windows) and can load any URL you point it at via its constructor/`goToURL()`. However, JUCE's own official examples and tutorials only demonstrate loading local/bundled content or a local dev server — there is no first-party guidance, sandboxing model, or precedent for browsing arbitrary third-party websites (like tone3000.com) *from inside a plugin UI*, and doing so raises real concerns for a Logic Pro AU: pulling a full interactive browser (with its own navigation, cookies, potential ads, and unbounded network access) into a plugin's real-time-hosted UI is unusual, host-plugin GUI threading has its own constraints, and Logic Pro's plugin sandboxing/hardened runtime entitlements would need explicit network-access entitlements set via the CMake `HARDENED_RUNTIME_OPTIONS`/`APP_SANDBOX_OPTIONS`. Treat this as technically possible but non-standard; if the goal is only to *fetch* content (e.g. download an IR file from tone3000.com), prefer `juce::URL` HTTP calls over embedding a full browser tab.
- Windows-specific: `JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING` for static-linking the WebView2 loader; not relevant on macOS (macOS uses WKWebView, no separate runtime install needed).
- Sources: https://juce.com/blog/juce-8-feature-overview-webview-uis/ , https://docs.juce.com/master/classjuce_1_1WebBrowserComponent_1_1Options.html

**`juce::URL::downloadToFile` — HTTP downloads**
- `URL::downloadToFile(File, DownloadTaskOptions)` returns a `std::unique_ptr<URL::DownloadTask>` for async, resumable, OS-native background downloads — preferred over `createInputStream`/`WebInputStream` for reliability, especially on mobile but also generally recommended for file downloads (e.g. pulling an IR/preset pack) on desktop.
- `DownloadTaskOptions` lets you set custom headers, a `DownloadTaskListener` (progress/finished callbacks), and whether to POST.
- `DownloadTask` exposes `getLengthDownloaded()`, `getTotalLength()` (may be `-1` if server didn't send `Content-Length`), `isFinished()`, `statusCode()`.
- This is the natural API for "download a cab IR or preset from a URL into the plugin's resources folder," distinct from and simpler than driving a full `WebBrowserComponent` session.
- Sources: https://docs.juce.com/master/classURL_1_1DownloadTask.html , https://docs.juce.com/master/classURL_1_1DownloadTaskOptions.html

## 6. macOS deployment target & universal binary

- **Universal binary (arm64 + x86_64)**: set `CMAKE_OSX_ARCHITECTURES` to both, e.g. `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` (as a CMake cache var, set at initial configure — must be building on a Mac with an SDK that supports both slices; an x86_64 Mac cannot cross-compile arm64 without a suitable toolchain, but any modern Xcode command-line tools on either architecture Mac can build universal since Xcode 12+ SDKs bundle both).
- **CMAKE_OSX_DEPLOYMENT_TARGET**: must also be set at initial CMake configure time (changing later requires a fresh build dir/cache clear). There's no single official JUCE-mandated minimum, but community practice for a 2025/2026 personal Logic Pro plugin:
  - `11.0` (Big Sur) is a common, sensible modern floor — it's the first macOS version with universal-binary-native (Apple Silicon) support and is old enough (2020) that essentially all active Logic Pro users are past it.
  - `10.13`/`10.14` still appear in older JUCE forum threads for max backward compatibility, but are increasingly unnecessary — Logic Pro itself has been raising its own minimum macOS requirement over time (current Logic Pro versions require considerably newer macOS already), so targeting anything much older than the current Logic Pro's own OS floor buys little.
  - Recommendation: check Logic Pro's currently-published minimum macOS requirement on Apple's site at build time and set `CMAKE_OSX_DEPLOYMENT_TARGET` to match it (or one major version below for safety margin), rather than blindly using an old default.
- Sources: https://forum.juce.com/t/cmake-macos-universal-binary-build-how-to-link-to-a-target-for-x86-64-only/42502 , https://forum.juce.com/t/cmake-plugin-and-os-11-universal-binary/41997 , https://github.com/juce-framework/JUCE/blob/master/docs/CMake%20API.md

## 7. Recommended CLI dev loop

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release --target MyDelayFX_AU

sudo codesign --force --deep --sign - \
    ~/Library/Audio/Plug-Ins/Components/MyDelayFX.component
killall -9 AudioComponentRegistrar

auval -v aufx Mdfx Ybgs
# open Logic Pro, or: open -a "Logic Pro"
```
