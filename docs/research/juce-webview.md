# JUCE 8 WebView plugin UI — integration recipe for tubamp (Aug 2026)

Everything below is verified against the local checkout at
`build/_deps/juce-src` (JUCE **8.0.15**). Citations are `path:line` relative to
that directory; `J/` is shorthand for it. Web sources are listed in §6.

Target: replace `src/PluginEditor.*` (native JUCE editor) with a
`WebBrowserComponent` hosting a Vite + React + TypeScript SPA, fixed 1120x700,
two-way binding for all 41 frozen params, native functions for chain/model/IR/
preset/TONE3000 operations, backend→frontend events for meters and library
changes.

---

## 0. API surface, in one place

| Thing | Where |
|---|---|
| `WebBrowserComponent::Options` builder | `J/modules/juce_gui_extra/misc/juce_WebBrowserComponent.h:121` |
| `withNativeIntegrationEnabled` | `…juce_WebBrowserComponent.h:299` |
| `withNativeFunction(Identifier, NativeFunction)` | `…:320` |
| `withEventListener(Identifier, NativeEventListener)` | `…:333` |
| `withUserScript` / `withInitialisationData` | `…:346` / `…:360` |
| `withResourceProvider(provider, allowedOrigin)` | `…:387` (guarded by `JUCE_WEB_BROWSER_RESOURCE_PROVIDER_AVAILABLE`, `…:40-44`) |
| `withOptionsFrom(OptionsBuilder&)` | `…:409` |
| `getResourceProviderRoot()` | `…:501`, impl `…juce_WebBrowserComponent.cpp:613` |
| `emitEventIfBrowserIsVisible` | `…:592`, impl `…juce_WebBrowserComponent.cpp:607` |
| `pageAboutToLoad` (single-page guard) | `…:602` |
| Relays | `J/modules/juce_gui_extra/misc/juce_WebControlRelays.h:69,148,225` |
| APVTS attachments | `J/modules/juce_audio_processors/utilities/juce_ParameterAttachments.h:273,321,366` |
| Frontend JS module | `J/modules/juce_gui_extra/native/javascript/index.js` |
| Reference plugin | `J/examples/Plugins/WebViewPluginDemo.h:399-624` |

Resource-provider root, per platform (`juce_WebBrowserComponent.cpp:613-635`):

```
macOS / iOS / Linux :  juce://juce.backend/
Windows / Android   :  https://juce.backend/
```

On macOS the `juce` scheme is registered on the `WKWebViewConfiguration` only if
a resource provider was supplied
(`J/modules/juce_gui_extra/native/juce_WebBrowserComponent_mac.mm:876-879`), and
the scheme task handler serves `[url path]` — i.e. the provider receives
`"/index.html"`, `"/assets/x.js"`, and `"/"` for the root
(`…_mac.mm:513-518`, `…:537-559`). It sets only `Content-Length`,
`Content-Type`, plus `Access-Control-Allow-Origin: <allowedOrigin>` when one was
passed (`…_mac.mm:539-550`); a `std::nullopt` return becomes a bare 404
(`…_mac.mm:557-560`).

---

## 1. CMake

### 1.1 What actually toggles `JUCE_WEB_BROWSER`

Read `J/extras/Build/CMake/JUCEUtils.cmake:252-276`: `NEEDS_WEB_BROWSER` only
sets `JUCE_WEB_BROWSER=<bool>` **on Linux/BSD** (where it also links
`pkgconfig_JUCE_BROWSER_LINUX_DEPS` and, for plugins, the embedded subprocess
helper). On macOS the define comes from the module default
`J/modules/juce_gui_extra/juce_gui_extra.h:71-76` (`#define JUCE_WEB_BROWSER 1`),
and `WebKit.framework` is already linked because `juce_gui_extra` declares
`OSXFrameworks: WebKit` (`juce_gui_extra.h:55`). `juce_audio_utils` →
`juce_audio_processors` → `juce_gui_extra`
(`juce_audio_utils.h:54`, `juce_audio_processors.h:54`), so tubamp already links
it transitively.

**So the only thing blocking WebView today is `JUCE_WEB_BROWSER=0` in
`CMakeLists.txt:88`.** Set `NEEDS_WEB_BROWSER TRUE` anyway — it costs nothing on
macOS and is required the day a Linux/Windows target appears.

### 1.2 Source split: plugin gets the editor, console apps don't

`tubamp_smoke` and `tubamp_uishot` compile `${tubamp_sources}` (the whole
`src/**` glob) with `JUCE_WEB_BROWSER=0` (`CMakeLists.txt:118,140`). A WebView
editor compiled under `JUCE_WEB_BROWSER=0` will not compile at all —
`WebBrowserComponent`, the relays and the attachments are all inside
`#if JUCE_WEB_BROWSER` blocks (`juce_WebBrowserComponent.h:38`,
`juce_WebControlRelays.h:38`, `juce_ParameterAttachments.h:260`).

Cleanest split — **exclude the editor from the console targets and guard
`createEditor`**, rather than sprinkling `#if`s through editor code:

```cmake
file(GLOB_RECURSE tubamp_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

# DSP + state + library only: everything the headless targets can compile.
set(tubamp_core_sources ${tubamp_sources})
list(FILTER tubamp_core_sources EXCLUDE REGEX "/src/(WebEditor|ui/)")
```

```cpp
// PluginProcessor.cpp
juce::AudioProcessorEditor* TubampAudioProcessor::createEditor()
{
   #if TUBAMP_HEADLESS
    return nullptr;                         // smoke/uishot: no editor, no WebKit
   #else
    return new WebEditor (*this);
   #endif
}
// PluginProcessor.h
bool hasEditor() const override { return ! TUBAMP_HEADLESS; }
```

- `tubamp_smoke`: `target_sources(... ${tubamp_core_sources})`,
  `TUBAMP_HEADLESS=1`, keep `JUCE_WEB_BROWSER=0`. Nothing changes for it.
- `tubamp_uishot`: **retire it.** Its entire purpose is
  `editor->createComponentSnapshot()` (`tools/UiSnapshot.cpp:43`), and an
  `NSViewComponent`-hosted `WKWebView` does not render into a JUCE `Graphics`
  — you get a blank/placeholder rect (`WebBrowserComponent::paint` delegates to
  `fallbackPaint`, `juce_WebBrowserComponent.cpp:682-685`, whose macOS
  implementation is a no-op). Replace the
  screenshot workflow with Playwright/`chrome --headless --screenshot` against
  the Vite dev server (§5.7); that is strictly better anyway (real DOM, real
  CSS, per-state screenshots).

### 1.3 Plugin target

```cmake
juce_add_plugin(tubamp
    ...
    NEEDS_WEB_BROWSER           TRUE)     # was FALSE (CMakeLists.txt:81)

target_sources(tubamp PRIVATE ${tubamp_sources})

target_compile_definitions(tubamp PUBLIC
    JUCE_WEB_BROWSER=1                    # was 0 (CMakeLists.txt:88)
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    TUBAMP_HEADLESS=0
    TUBAMP_WEBUI_ZIP_PREFIX="${TUBAMP_WEBUI_ZIP_PREFIX}")
```

### 1.4 Building + bundling the Vite app

Layout: `ui/` (Vite project) → `ui/dist/` → zip → `juce_add_binary_data` →
`ZipFile` streamed by the resource provider. Per-file `juce_add_binary_data` is
possible but Vite emits content-hashed filenames, so the generated
`BinaryData::` symbol names would churn on every build — the zip keeps one
stable symbol.

```cmake
# ---------------------------------------------------------------------------
# Web UI: npm build -> dist/ -> zip -> binary data
# ---------------------------------------------------------------------------
find_program(NPM_EXECUTABLE npm REQUIRED)

set(webui_dir  "${CMAKE_CURRENT_SOURCE_DIR}/ui")
set(webui_dist "${webui_dir}/dist")
set(webui_zip  "${CMAKE_BINARY_DIR}/tubamp_webui.zip")

# The JUCE frontend module is not on npm (see §3.1). Vendor it into the Vite
# project at configure time so `import * as Juce from "./juce"` resolves.
# JUCE_MODULES_DIR is set by JUCE's own CMakeLists.txt:53.
file(COPY "${JUCE_MODULES_DIR}/juce_gui_extra/native/javascript/"
     DESTINATION "${webui_dir}/src/juce/")

file(GLOB_RECURSE webui_inputs CONFIGURE_DEPENDS
    "${webui_dir}/src/*" "${webui_dir}/index.html"
    "${webui_dir}/package.json" "${webui_dir}/vite.config.ts")

add_custom_command(
    OUTPUT  "${webui_dist}/index.html"
    COMMAND "${NPM_EXECUTABLE}" ci --prefer-offline --no-audit
    COMMAND "${NPM_EXECUTABLE}" run build
    WORKING_DIRECTORY "${webui_dir}"
    DEPENDS ${webui_inputs}
    COMMENT "Building tubamp web UI (vite)"
    VERBATIM)

# `cmake -E tar` stores paths relative to WORKING_DIRECTORY, so entries come out
# as "dist/index.html". Tell C++ about that prefix instead of trying to strip it.
set(TUBAMP_WEBUI_ZIP_PREFIX "dist/" CACHE INTERNAL "")

add_custom_command(
    OUTPUT  "${webui_zip}"
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${webui_zip}" --format=zip "dist"
    WORKING_DIRECTORY "${webui_dir}"
    DEPENDS "${webui_dist}/index.html"
    COMMENT "Zipping tubamp web UI"
    VERBATIM)

add_custom_target(tubamp_webui DEPENDS "${webui_zip}")

juce_add_binary_data(TubampWebUiData
    HEADER_NAME WebUiData.h
    NAMESPACE   webui
    SOURCES     "${webui_zip}")
add_dependencies(TubampWebUiData tubamp_webui)

target_link_libraries(tubamp PRIVATE nam_core TubampWebUiData
    juce::juce_audio_utils juce::juce_dsp)
```

`juce_add_binary_data` signature and behaviour: `JUCEUtils.cmake:459-533`
(`NAMESPACE`, `HEADER_NAME`, `SOURCES`; emits `BinaryDataN.cpp` +
`<HEADER_NAME>`, sets `POSITION_INDEPENDENT_CODE`, exposes the include dir as
`INTERFACE`). Symbol here is `webui::tubamp_webui_zip` /
`webui::tubamp_webui_zipSize`.

Dev-mode switch (§3.4):

```cmake
option(TUBAMP_WEBUI_DEV "Load the UI from the Vite dev server" OFF)
target_compile_definitions(tubamp PRIVATE
    TUBAMP_WEBUI_DEV=$<BOOL:${TUBAMP_WEBUI_DEV}>)
```

---

## 2. C++ editor

### 2.1 Member order is load-bearing

Destruction runs in reverse declaration order, and:

- `~WebBrowserComponent` calls `webViewDestructed` on every registered lifetime
  listener (`juce_WebBrowserComponent.cpp:601-604`) — the relays *are* those
  listeners (`juce_WebControlRelays.cpp:72-78`,
  `WebSliderRelay::buildOptions` adds `withWebViewLifetimeListener(this)`).
  ⇒ **relays must be declared before the browser.**
- Attachments hold `WebSliderRelay&` and call `removeListener` in their dtor
  (`juce_ParameterAttachments.cpp:290-293`).
  ⇒ **attachments must be declared after the browser** (so they die first) and
  before the relays are destroyed. Declaring them last satisfies both.

Exactly the demo's ordering: relays `WebViewPluginDemo.h:443-445`, browser
`:449`, attachments `:468-470`.

Initial sync: `WebSliderRelay::webViewConstructed` fires
`initialUpdateRequested` (`juce_WebControlRelays.cpp:83-87`) but at that moment
no attachment exists yet. The real initial sync comes from the frontend — every
`SliderState`/`ToggleState`/`ComboBoxState` ctor emits
`{eventType:"requestInitialUpdate"}` on construction (`index.js:164-166`,
`:325-327`, `:413-415`), which routes to
`WebSliderParameterAttachment::sendInitialUpdate` (`juce_ParameterAttachments.h:301`).
So do **not** hand-roll an initial push; just make sure the JS creates its state
objects.

### 2.2 Header

```cpp
// src/WebEditor.h
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

namespace tubamp
{
/** Refuses navigation away from our SPA (see WebViewPluginDemo.h:390-396). */
struct SinglePageBrowser : juce::WebBrowserComponent
{
    using WebBrowserComponent::WebBrowserComponent;
    bool pageAboutToLoad (const juce::String& newURL) override;
};

class WebEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit WebEditor (TubampAudioProcessor&);
    ~WebEditor() override;

    void resized() override;
    int getControlParameterIndex (juce::Component&) override
    {
        return paramIndexReceiver.getControlParameterIndex();
    }

private:
    void timerCallback() override;
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& path);
    juce::WebBrowserComponent::Options buildOptions();

    TubampAudioProcessor& proc;

    // --- 1. relays (must outlive `web`)
    std::vector<std::unique_ptr<juce::WebSliderRelay>>       sliderRelays;
    std::vector<std::unique_ptr<juce::WebToggleButtonRelay>> toggleRelays;
    std::vector<std::unique_ptr<juce::WebComboBoxRelay>>     comboRelays;
    juce::WebControlParameterIndexReceiver paramIndexReceiver;

    // --- 2. the browser
    std::unique_ptr<SinglePageBrowser> web;

    // --- 3. attachments (die before `web`)
    std::vector<std::unique_ptr<juce::WebSliderParameterAttachment>>       sliderAtt;
    std::vector<std::unique_ptr<juce::WebToggleButtonParameterAttachment>> toggleAtt;
    std::vector<std::unique_ptr<juce::WebComboBoxParameterAttachment>>     comboAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebEditor)
};
} // namespace tubamp
```

`std::unique_ptr` + `std::vector` rather than 41 named members: relays are
`JUCE_DECLARE_NON_COPYABLE`/`NON_MOVEABLE` (`juce_WebControlRelays.h:115-116`)
so they cannot live in a `std::vector<WebSliderRelay>` directly, and the vectors
still give the right construct/destruct ordering because the three groups are
declared in the order above.

### 2.3 Relays for every frozen param id

Use the **APVTS parameter id verbatim as the relay name** — the relay name is
what `Juce.getSliderState("input_trim")` looks up, so there is exactly one
namespace to remember. Full set (from `src/Parameters.h`, `src/Parameters.cpp`):

```cpp
// 29 floats -> WebSliderRelay
static const char* kSliderIds[] = {
    params::inputTrim, params::outputLevel, params::gateThreshold,
    params::compThreshold, params::compRatio, params::compAttack,
    params::compRelease, params::compMakeup,
    params::driveGain, params::driveTone, params::driveLevel,
    params::ampInput, params::ampOutput, params::ampCalLevel, params::ampSlim,
    params::cabLowCut, params::cabHighCut,
    params::eqBass, params::eqMid, params::eqTreble,
    params::modRate, params::modDepth, params::modMix,
    params::delayTime, params::delayFeedback, params::delayMix,
    params::reverbSize, params::reverbDamping, params::reverbMix };

// 10 bools -> WebToggleButtonRelay
static const char* kToggleIds[] = {
    params::gateOn, params::compOn, params::driveOn, params::ampOn,
    params::cabOn, params::eqOn, params::modOn, params::delayOn,
    params::reverbOn, params::ampCalInput };

// 2 choices -> WebComboBoxRelay
static const char* kComboIds[] = { params::ampOutMode, params::modType };
```

`amp_slim` is an `AudioParameterFloat` (`Parameters.cpp:80`), so it is a slider,
not a toggle. `amp_cal_input` is the only non-`*_on` bool (`Parameters.cpp:76`).

### 2.4 Options chain

```cpp
juce::WebBrowserComponent::Options WebEditor::buildOptions()
{
    using Opts = juce::WebBrowserComponent::Options;

    auto o = Opts{}
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()   // see §5.1
        .withOptionsFrom (paramIndexReceiver);

    for (auto& r : sliderRelays) o = o.withOptionsFrom (*r);
    for (auto& r : toggleRelays) o = o.withOptionsFrom (*r);
    for (auto& r : comboRelays)  o = o.withOptionsFrom (*r);

    // ---- static boot data, injected before any resource loads (…h:360)
    for (const auto& b : chain::blockInfos)
    {
        juce::DynamicObject::Ptr d { new juce::DynamicObject };
        d->setProperty ("token",       b.token);
        d->setProperty ("displayName", b.displayName);
        d->setProperty ("shortName",   b.shortName);
        d->setProperty ("enableParam", b.enableParamId);
        o = o.withInitialisationData ("tubampBlocks", juce::var { d.get() });
    }
    o = o.withInitialisationData ("tubampVersion", JucePlugin_VersionString);

    // ---- native functions (always invoked on the message thread, …h:309)
    o = o
      .withNativeFunction ("getChainOrder", [this] (auto&, auto complete)
      {
          juce::Array<juce::var> tokens;
          for (auto id : proc.getChainOrder())
              tokens.add (juce::var { chain::infoFor (id).token });
          complete (tokens);
      })
      .withNativeFunction ("setChainOrder", [this] (const juce::Array<juce::var>& a, auto complete)
      {
          // arg 0 is a JS string[]; ChainOrder::fromString dedupes + drops junk.
          juce::StringArray toks;
          if (auto* arr = a[0].getArray())
              for (const auto& t : *arr) toks.add (t.toString());
          proc.setChainOrder (chain::fromString (toks.isEmpty()
                                                     ? juce::String (chain::emptyChainToken)
                                                     : toks.joinIntoString (",")));
          complete (juce::var { true });
      })
      .withNativeFunction ("listModels", [this] (auto&, auto complete)
      {
          juce::Array<juce::var> out;
          for (const auto& e : proc.library.getModels())
          {
              juce::DynamicObject::Ptr d { new juce::DynamicObject };
              d->setProperty ("name", e.displayName);
              d->setProperty ("path", e.file.getFullPathName());
              out.add (juce::var { d.get() });
          }
          complete (out);
      })
      .withNativeFunction ("loadModel", [this] (const juce::Array<juce::var>& a, auto complete)
      {
          const auto err = proc.loadModel (juce::File (a[0].toString()));
          complete (err.isEmpty() ? juce::var() : juce::var { err });   // null == ok
      })
      .withNativeFunction ("clearModel", [this] (auto&, auto c) { proc.clearModel(); c ({}); })
      .withNativeFunction ("listIrs",    /* mirror listModels over library.getIrs() */ …)
      .withNativeFunction ("loadIr",     /* mirror loadModel over proc.loadIr()    */ …)
      .withNativeFunction ("clearIr",    [this] (auto&, auto c) { proc.clearIr(); c ({}); })
      .withNativeFunction ("listPresets", …)
      .withNativeFunction ("savePreset", [this] (const juce::Array<juce::var>& a, auto complete)
      {
          const auto r = proc.presets.savePreset (a[0].toString());
          complete (r.wasOk() ? juce::var() : juce::var { r.getErrorMessage() });
      })
      .withNativeFunction ("loadPreset",   …)
      .withNativeFunction ("deletePreset", …)
      .withNativeFunction ("abCapture", [this] (const juce::Array<juce::var>& a, auto c)
                                        { proc.presets.captureToSlot ((int) a[0]); c ({}); })
      .withNativeFunction ("abRecall",  [this] (const juce::Array<juce::var>& a, auto c)
                                        { proc.presets.recallSlot ((int) a[0]); c ({}); })
      // TONE3000: async, so `complete` fires later — legal, it is callable from
      // any thread (juce_WebBrowserComponent.h:90-93).
      .withNativeFunction ("t3kSetClientId", …)
      .withNativeFunction ("t3kSelectTone", [this] (auto&, auto complete)
      {
          proc.tone3000.startSelectFlow (
              [complete] (Tone3000Client::ToneModels tm) { complete (toVar (tm)); },
              [complete] (juce::String e) { complete (juce::var { e }); });
      })
      .withNativeFunction ("t3kDownload", …)
      .withNativeFunction ("openLibraryFolder", [] (auto&, auto c)
      {
          ModelLibrary::getRootDir().revealToUser(); c ({});
      });

    // ---- resource provider (dev origin allowed only in dev builds)
   #if TUBAMP_WEBUI_DEV
    o = o.withResourceProvider ([this] (const auto& p) { return getResource (p); },
                                juce::URL { kDevServerUrl }.getOrigin());
   #else
    o = o.withResourceProvider ([this] (const auto& p) { return getResource (p); });
   #endif

    return o;
}
```

Native-function contract, verified:
`using NativeFunction = std::function<void (const Array<var>&, NativeFunctionCompletion)>`
(`juce_WebBrowserComponent.h:101`); the completion resolves the JS Promise and
"can be called from any thread" (`…h:90-93`); the callback itself "is always
called on the message thread" (`…h:309`). Duplicate names assert
(`…h:323`).

### 2.5 Constructor / dtor / resize

```cpp
namespace { const juce::String kDevServerUrl { "http://localhost:5173/" }; }

bool SinglePageBrowser::pageAboutToLoad (const juce::String& newURL)
{
    return newURL == getResourceProviderRoot() || newURL == kDevServerUrl;
}

WebEditor::WebEditor (TubampAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    for (auto* id : kSliderIds) sliderRelays.push_back (std::make_unique<juce::WebSliderRelay> (id));
    for (auto* id : kToggleIds) toggleRelays.push_back (std::make_unique<juce::WebToggleButtonRelay> (id));
    for (auto* id : kComboIds)  comboRelays .push_back (std::make_unique<juce::WebComboBoxRelay> (id));

    web = std::make_unique<SinglePageBrowser> (buildOptions());
    addAndMakeVisible (*web);

    for (size_t i = 0; i < sliderRelays.size(); ++i)
        sliderAtt.push_back (std::make_unique<juce::WebSliderParameterAttachment> (
            *proc.apvts.getParameter (kSliderIds[i]), *sliderRelays[i], proc.apvts.undoManager));
    // …same for toggles and combos

    proc.onChainChanged     = [this] { emitChainChanged(); };
    proc.library.onChanged  = [this] { web->emitEventIfBrowserIsVisible ("library.onChanged", {}); };
    proc.presets.onPresetChanged = [this] { emitPresetChanged(); };

   #if TUBAMP_WEBUI_DEV
    web->goToURL (kDevServerUrl);
   #else
    web->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
   #endif

    setSize (1120, 700);      // CSS px == JUCE px, see §5.5
    setResizable (false, false);
    startTimerHz (30);
}

WebEditor::~WebEditor()
{
    stopTimer();
    proc.onChainChanged = nullptr;          // processor outlives the editor
    proc.library.onChanged = nullptr;
    proc.presets.onPresetChanged = nullptr;
}

void WebEditor::resized() { web->setBounds (getLocalBounds()); }
```

`goToURL` must be called **at least once before any `emitEventIfBrowserIsVisible`**
— the JS framework isn't loaded until the first document load, otherwise you get
a JS exception + a C++ assertion (`juce_WebBrowserComponent.h:586-590`).

### 2.6 Events: meters at 30 Hz, and state notifications

`emitEventIfBrowserIsVisible` no-ops when the component is not visible
(`juce_WebBrowserComponent.cpp:607-611`), and each emit is a full
`evaluateJavascript("window.__JUCE__.backend.emitByBackend('id','<json>')")`
with double escaping (`…cpp:414-429`). Keep payloads small; 2 floats @30 Hz is
nothing, a 512-point spectrum @30 Hz is not.

```cpp
void WebEditor::timerCallback()
{
    juce::DynamicObject::Ptr d { new juce::DynamicObject };
    d->setProperty ("in",  proc.inputPeak .load (std::memory_order_relaxed));
    d->setProperty ("out", proc.outputPeak.load (std::memory_order_relaxed));
    web->emitEventIfBrowserIsVisible ("meters", juce::var { d.get() });
}

void WebEditor::emitChainChanged()
{
    juce::Array<juce::var> toks;
    for (auto id : proc.getChainOrder())
        toks.add (juce::var { chain::infoFor (id).token });
    web->emitEventIfBrowserIsVisible ("onChainChanged", toks);
}
```

Same shape for `presets.onPresetChanged` (emit `{name, activeSlot}`) and
`library.onChanged` (emit `{}`; the SPA re-calls `listModels`/`listIrs`).
Event ids beginning with `__juce` are reserved (`…h:574`).

Alternative for anything bulky (waveforms, IR curves, catalog pages): serve it
through the resource provider as JSON and `fetch()` it — that is what the demo
does for `spectrumData.json` (`WebViewPluginDemo.h:560-574`).

### 2.7 Resource provider over the binary-data zip

```cpp
#include "WebUiData.h"

static juce::ZipFile* webUiZip()
{
    static juce::MemoryInputStream stream { webui::tubamp_webui_zip,
                                            (size_t) webui::tubamp_webui_zipSize, false };
    static juce::ZipFile zip { &stream, false };
    return &zip;
}

static const char* mimeForExtension (const juce::String& ext)
{
    static const std::unordered_map<juce::String, const char*> m {
        { "html", "text/html" },        { "htm",  "text/html" },
        { "js",   "text/javascript" },  { "mjs",  "text/javascript" },
        { "css",  "text/css" },         { "json", "application/json" },
        { "map",  "application/json" }, { "svg",  "image/svg+xml" },
        { "png",  "image/png" },        { "jpg",  "image/jpeg" },
        { "jpeg", "image/jpeg" },       { "webp", "image/webp" },
        { "ico",  "image/vnd.microsoft.icon" },
        { "woff", "font/woff" },        { "woff2","font/woff2" },
        { "ttf",  "font/ttf" },         { "txt",  "text/plain" } };
    const auto it = m.find (ext.toLowerCase());
    return it != m.end() ? it->second : "application/octet-stream";
}

std::optional<juce::WebBrowserComponent::Resource>
WebEditor::getResource (const juce::String& path)
{
    // The mac scheme handler hands us [url path], i.e. "/" or "/assets/x.js".
    const auto rel = (path == "/" ? juce::String ("index.html")
                                  : path.fromFirstOccurrenceOf ("/", false, false));
    const auto entryName = juce::String (TUBAMP_WEBUI_ZIP_PREFIX) + rel;   // "dist/…"

    if (auto* zip = webUiZip())
        if (auto* entry = zip->getEntry (entryName))
            if (auto stream = rawToUniquePtr (zip->createStreamForEntry (*entry)))
            {
                std::vector<std::byte> bytes ((size_t) stream->getTotalLength());
                stream->setPosition (0);
                stream->read (bytes.data(), bytes.size());
                return juce::WebBrowserComponent::Resource {
                    std::move (bytes),
                    juce::String (mimeForExtension (rel.fromLastOccurrenceOf (".", false, false))) };
            }

    return std::nullopt;   // -> HTTP 404 (juce_WebBrowserComponent_mac.mm:557-560)
}
```

MIME map derived from the demo's (`WebViewPluginDemo.h:488-512`); the demo
asserts on unknown extensions — returning `application/octet-stream` is safer
for a Vite bundle that may emit fonts/wasm. **`text/javascript` on `.js` is
mandatory**: WKWebView refuses to execute a module served as
`application/octet-stream`.

---

## 3. JS / TypeScript frontend

### 3.1 The frontend module — vendor it, don't npm-install it

`J/modules/juce_gui_extra/native/javascript/package.json` declares
`{"name":"juce-framework-frontend","version":"7.0.7"}` and **it is not published
on the public npm registry**. JUCE's own demo installs it by path:

```json
"juce-framework-frontend": "file:../../../modules/juce_gui_extra/native/javascript"
```
(`J/examples/Plugins/WebViewPluginDemoGUI/package.json:12`)

For tubamp that path points into `build/_deps/…` (regenerated, gitignored), so a
`file:` dependency would break `npm ci` on a clean checkout. Use the
configure-time `file(COPY …)` from §1.4 into `ui/src/juce/` and import
relatively:

```ts
import * as Juce from "./juce/index.js";
```

Two files land there: `index.js` (the module) and `check_native_interop.js`
(imported by it, `index.js:35`). Add `ui/src/juce/` to `.gitignore` and make
`cmake --preset …` (or a `ui/prebuild.mjs`) the way developers get it. There are
unofficial mirrors on npm (`juce-framework-frontend-mirror`, `juce-webview`) —
they are third-party republishes and drift from your pinned JUCE, so prefer the
in-tree copy.

TypeScript needs a shim (the module ships no types):

```ts
// ui/src/juce/index.d.ts
export interface SliderProperties {
  start: number; end: number; skew: number;
  name: string; label: string;
  numSteps: number; interval: number; parameterIndex: number;
}
export interface ListenerList {
  addListener(fn: () => void): number;
  removeListener(id: number): void;
}
export interface SliderState {
  properties: SliderProperties;
  valueChangedEvent: ListenerList;
  propertiesChangedEvent: ListenerList;
  getScaledValue(): number;
  getNormalisedValue(): number;
  setNormalisedValue(v: number): void;
  sliderDragStarted(): void;
  sliderDragEnded(): void;
}
export interface ToggleState {
  properties: { name: string; parameterIndex: number };
  valueChangedEvent: ListenerList;
  propertiesChangedEvent: ListenerList;
  getValue(): boolean;
  setValue(v: boolean): void;
}
export interface ComboBoxState {
  properties: { name: string; parameterIndex: number; choices: string[] };
  valueChangedEvent: ListenerList;
  propertiesChangedEvent: ListenerList;
  getChoiceIndex(): number;
  setChoiceIndex(i: number): void;
}
export function getSliderState(name: string): SliderState;
export function getToggleState(name: string): ToggleState;
export function getComboBoxState(name: string): ComboBoxState;
export function getNativeFunction(name: string): (...args: unknown[]) => Promise<unknown>;
export function getBackendResourceAddress(path: string): string;
export class ControlParameterIndexUpdater {
  constructor(annotationAttribute: string);
  handleMouseMove(e: MouseEvent): void;
}
```

Every field above is read off the implementation:
`SliderState` (`index.js:135-271`) with `properties` defaults at `:147-156`,
`setNormalisedValue` `:178`, `sliderDragStarted/Ended` `:192`/`:201`,
`getScaledValue` `:226`, `getNormalisedValue` `:239`;
`ToggleState` (`:302-358`), `getValue`/`setValue` `:331`/`:336`;
`ComboBoxState` (`:389-457`), `getChoiceIndex`/`setChoiceIndex` `:424`/`:434`;
`getNativeFunction` `:73`; `getBackendResourceAddress` `:485`;
`ControlParameterIndexUpdater` `:522`; exports `:570-577`.

### 3.2 Parameter binding, React

Key facts that shape the hook:

- `valueChangedEvent` listeners take **no arguments** — `callListeners()` is
  invoked with nothing (`index.js:211`, `:349`, `:453`). Read the value from the
  state object inside the callback.
- Normalised ↔ scaled conversion happens **in JS**, from `properties.skew/start/
  end` (`index.js:239-254`), and snapping from `properties.interval`
  (`index.js:257-270`). Those properties arrive asynchronously via
  `propertiesChangedEvent` (backend pushes them in
  `WebSliderParameterAttachment::sendInitialUpdate`,
  `juce_ParameterAttachments.cpp:295-316`; toggle `:352`, combo `:401`).
  Subscribe to **both** events or your
  first render shows a 0..1 linear range.
- Gestures: call `sliderDragStarted()` before the first `setNormalisedValue` of
  a drag and `sliderDragEnded()` after the last, otherwise host automation
  recording sees no touch (they map to `attachment.beginGesture/endGesture`,
  `juce_ParameterAttachments.h:299-300`).

```tsx
// ui/src/hooks/useSliderParam.ts
import { useEffect, useMemo, useState } from "react";
import * as Juce from "../juce/index.js";

export function useSliderParam(id: string) {
  const state = useMemo(() => Juce.getSliderState(id), [id]);
  const [value, setValue] = useState(state.getNormalisedValue());
  const [props, setProps] = useState(state.properties);

  useEffect(() => {
    const v = state.valueChangedEvent.addListener(() =>
      setValue(state.getNormalisedValue()));
    const p = state.propertiesChangedEvent.addListener(() => {
      setProps({ ...state.properties });
      setValue(state.getNormalisedValue());
    });
    return () => {
      state.valueChangedEvent.removeListener(v);
      state.propertiesChangedEvent.removeListener(p);
    };
  }, [state]);

  return {
    value,                                   // normalised 0..1
    scaled: state.getScaledValue(),          // e.g. dB / Hz / ms
    label: props.label,                      // "dB", "Hz", ":1" …
    name: props.name,                        // "Input Trim"
    numSteps: props.numSteps,
    parameterIndex: props.parameterIndex,    // for the annotation attribute
    setValue: (v: number) => { state.setNormalisedValue(v); setValue(v); },
    dragStart: () => state.sliderDragStarted(),
    dragEnd:   () => state.sliderDragEnded(),
  };
}
```

Toggles / combos are simpler:

```ts
const gate = Juce.getToggleState("gate_on");      // gate.getValue() / setValue(bool)
const mode = Juce.getComboBoxState("amp_out_mode");
// mode.properties.choices === ["Raw","Normalized","Calibrated"]  (pushed from
// juce_ParameterAttachments.cpp:401-412)
// mode.getChoiceIndex() / mode.setChoiceIndex(i)
```

### 3.3 Native functions and events

```ts
const getChainOrder = Juce.getNativeFunction("getChainOrder");
const setChainOrder = Juce.getNativeFunction("setChainOrder");

const order = (await getChainOrder()) as string[];   // ["gate","comp",…]
await setChainOrder(["amp", "cab", "eq"]);
```

`getNativeFunction` returns a plain function that emits `__juce__invoke` and
returns a `Promise` resolved when C++ calls the completion (`index.js:73-92`,
promise plumbing `:37-62`). It warns (does not throw) if the name is unknown to
the backend (`index.js:74-77`), which is why the SPA still boots in a plain
browser.

Backend→frontend events go through the low-level backend object (there is no
wrapper in `index.js`); the API is defined in `check_native_interop.js:115-145`:

```ts
const token = window.__JUCE__.backend.addEventListener(
  "meters",
  (o: { in: number; out: number }) => setMeters(o)
);
// cleanup
window.__JUCE__.backend.removeEventListener(token);
```

`addEventListener` returns `[eventId, id]` and `removeEventListener` takes that
tuple back (`check_native_interop.js:100-113`). Declare it once:

```ts
// ui/src/juce/global.d.ts
declare global {
  interface Window {
    __JUCE__: {
      backend: {
        addEventListener(id: string, fn: (payload: any) => void): [string, number];
        removeEventListener(token: [string, number]): void;
        emitEvent(id: string, payload: unknown): void;
      };
      initialisationData: Record<string, any[]>;
    };
  }
}
export {};
```

Boot data from `withInitialisationData` lands as **arrays** under
`window.__JUCE__.initialisationData.<name>` (`juce_WebBrowserComponent.cpp:185`,
documented `…h:353-358`) — so `tubampBlocks` is `Block[]` and `tubampVersion` is
`[string]`.

Fetching provider-served JSON from the SPA:

```ts
const url = Juce.getBackendResourceAddress("catalog.json");  // juce://juce.backend/catalog.json
const data = await (await fetch(url)).json();
```
(`index.js:485-501` picks the right prefix per `__juce__platform`.)

Host parameter-under-mouse (Logic's "touch to select", AU
`getControlParameterIndex`): annotate DOM nodes with the parameter index and
pump mousemove:

```tsx
const updater = useMemo(() => new Juce.ControlParameterIndexUpdater("data-param-index"), []);
<div onMouseMove={(e) => updater.handleMouseMove(e.nativeEvent)}>
  <Knob data-param-index={trim.parameterIndex} … />
</div>
```
Backed by `WebControlParameterIndexReceiver`
(`J/modules/juce_gui_extra/misc/juce_WebControlParameterIndexReceiver.h:54-71`).

### 3.4 Dev mode / HMR

`WebBrowserComponent` has no notion of "dev server" — you just `goToURL` a
different address. Two things must line up:

1. `goToURL(kDevServerUrl)` instead of `getResourceProviderRoot()`.
2. `withResourceProvider(provider, "http://localhost:5173")` — the allowed-origin
   argument (`…h:387-393`) makes the handler emit
   `Access-Control-Allow-Origin` (`…_mac.mm:545-550`) so `fetch()` from the
   dev-server origin can still reach `juce://juce.backend/…`.

Plus `SinglePageBrowser::pageAboutToLoad` must whitelist the dev URL (§2.5), and
Vite must bind the port you hardcode.

```ts
// ui/vite.config.ts
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  base: "./",                       // emit relative asset URLs
  server: { port: 5173, strictPort: true },
  build: { outDir: "dist", assetsDir: "assets", target: "safari15",
           sourcemap: false, emptyOutDir: true },
});
```

`base: "./"` keeps index.html referencing `./assets/…`, which resolves against
`juce://juce.backend/` — no absolute-path assumptions in the provider.
`target: "safari15"` matches the WKWebView on macOS 11+ deployment target
(`CMAKE_OSX_DEPLOYMENT_TARGET "11.0"`, `CMakeLists.txt:7`); leaving Vite's
default `modules` target can emit syntax older Safari chokes on.

Web inspector: `developerExtrasEnabled` is set only under `JUCE_DEBUG`
(`…_mac.mm:880-883`) — Debug builds get right-click → Inspect Element and show up
in Safari ▸ Develop ▸ *host app*.

Recommended loop: `cmake -B build -DTUBAMP_WEBUI_DEV=ON`, run the **Standalone**
target, `npm run dev` in `ui/` — full HMR with live parameters. Ship builds
flip the option off, which also drops the CORS hole.

---

## 4. Migration order (suggested)

1. CMake: source split + `TUBAMP_HEADLESS`, `JUCE_WEB_BROWSER=1`, retire
   `tubamp_uishot`. Verify `tubamp_smoke` still builds/passes.
2. `ui/` scaffold (`npm create vite@latest ui -- --template react-ts`), vendor
   `src/juce/`, zip + binary-data plumbing, `WebEditor` serving a stub
   `index.html`. Confirm the AU opens in Logic showing the stub.
3. Relays + attachments for all 41 ids; port the knobs/toggles/combos.
4. Native functions: chain first (it is the app's spine), then library, presets,
   TONE3000.
5. Events: meters, `onChainChanged`, `library.onChanged`,
   `presets.onPresetChanged`.
6. Delete `src/ui/**` and `src/PluginEditor.*`.

---

## 5. Risks

### 5.1 auval / pluginval and editor churn
`auval -v aufx Tamp Tuba` opens and closes the editor; pluginval level ≥ 5 does
it repeatedly. There are open reports of `WebViewPluginDemo` crashing pluginval
level 10 on macOS/arm64 during the editor open/close test, and of WKWebView
getting stuck on `about:blank` when the native view is destroyed and recreated
(juce-framework/JUCE#1415, forum "WebView crashes PluginVal"). Mitigations:
`withKeepPageLoadedWhenBrowserIsHidden()` (`…h:168`) — without it JUCE swaps in a
blank page on hide and reloads on show, which is exactly the churn path;
construct the `WebBrowserComponent` once per editor and never re-`goToURL`; and
`stopTimer()` + null the processor callbacks in `~WebEditor` (a `Timer` firing
`emitEventIfBrowserIsVisible` into a half-destroyed browser is a real crash).
Budget time for a `auval`/pluginval pass before shipping; this is the single
biggest schedule risk in the migration.

### 5.2 Hardened runtime / entitlements
The AU is loaded into Logic's own hardened-runtime process (or
`AUHostingServiceXPC` when sandboxed), so **you do not sign the plugin's own
entitlements — you inherit the host's**. WKWebView itself works there (Logic
already hosts web views), and the `juce://` scheme handler is in-process, so no
network entitlement is needed for the UI. What *does* need
`com.apple.security.network.client` is the **Standalone** target when the
TONE3000 flow runs, and the loopback listener the PKCE redirect needs
(`src/library/Tone3000Client.h:16-19`) — that is a server socket on
`127.0.0.1`, which a sandboxed Standalone build also needs
`com.apple.security.network.server` for. Do not use `file://` URLs for UI assets;
sandboxed WKWebView needs `loadFileURL:allowingReadAccessToURL:` and JUCE does
not go that route — the resource provider sidesteps the whole problem.

### 5.3 Keyboard focus in DAWs
Known, unresolved: WebBrowserComponent inside a plugin editor gets no keyboard
input in Logic on Apple silicon — focus stays in the DAW
(forum.juce.com/t/…/53053). JUCE routes focus via
`focusGainedWithDirection` → `[[webView window] makeFirstResponder: webView]`
(`…_mac.mm:992-1001`), which depends on the host giving the plugin view key
focus at all. **Design around it**: no free-text inputs in the primary flow.
Preset naming and the TONE3000 publishable key are the two places tubamp needs
typing — route both through a native `juce::AlertWindow`/`FileChooser` triggered
by a native function, not an HTML `<input>`. Cmd-X/C/V/A are forwarded manually
(`…_mac.mm:190-225`) so clipboard works even when plain typing doesn't.

Related: right-clicking inside the WebView has crashed plugins in Logic
(juce-framework/JUCE#1376) — disable the context menu
(`document.addEventListener("contextmenu", e => e.preventDefault())`) and use
your own long-press/button menus.

### 5.4 Resource-provider origin and `fetch()`
The SPA's origin is `juce://juce.backend` (macOS). Relative `fetch("data.json")`
works; anything absolute must go through `Juce.getBackendResourceAddress()`
(`index.js:485`). A non-standard scheme means: no `localStorage`/`sessionStorage`
guarantees, no cookies, no service workers, and some libraries that sniff
`location.protocol === "https:"` will misbehave. Persist all UI state in C++
(APVTS / the properties file) via native functions, never in web storage.
`crossOrigin`/CORS on the dev origin only works because of the explicit
`allowedOrigin` header (`…_mac.mm:545-550`); ship builds must not pass it.

### 5.5 Retina / scaling
The WKWebView is hosted as an `NSViewComponent`, so AppKit handles the backing
scale: `window.devicePixelRatio` is 2 on retina and 1 CSS px == 1 JUCE px.
`setSize(1120, 700)` therefore gives a 1120x700 CSS viewport — design the CSS at
those numbers and do not add manual DPI math. Host-driven scaling
(`AudioProcessorEditor::setScaleFactor`) is *not* propagated into the web view by
JUCE; since tubamp is fixed-size and non-resizable this is a non-issue today,
but if resizing is ever added, drive it from C++ (`setSize`) and let CSS reflow,
never from `window.resizeTo()`.

### 5.6 Build-system friction
`npm ci` inside the CMake build makes a clean configure+build require network
and a Node toolchain. Keep the `add_custom_command` OUTPUT-based (as in §1.4) so
incremental builds skip it, and consider committing `ui/dist/` (or a prebuilt
zip) behind an `option(TUBAMP_WEBUI_BUILD "…" ON)` escape hatch for CI machines
without Node. Also note `juce_add_binary_data` bakes the zip into every format's
binary — an ~800 KB React bundle adds ~800 KB to the AU *and* the Standalone.

### 5.7 Loss of the headless screenshot
`tubamp_uishot` stops being meaningful (§1.2). Nothing in the current harness
replaces it; plan a Playwright script against the dev server (`npx playwright
screenshot http://localhost:5173 shot.png`) plus a fixture that stubs
`window.__JUCE__` so states can be driven without the plugin running. The stub
is cheap because `check_native_interop.js:45-67` already installs a no-op
`window.__JUCE__` when the backend is absent.

---

## 6. Sources

Primary (local JUCE 8.0.15 checkout — all API claims above are cited inline):
`J/modules/juce_gui_extra/misc/juce_WebBrowserComponent.{h,cpp}`,
`J/modules/juce_gui_extra/misc/juce_WebControlRelays.{h,cpp}`,
`J/modules/juce_gui_extra/misc/juce_WebControlParameterIndexReceiver.h`,
`J/modules/juce_gui_extra/native/juce_WebBrowserComponent_mac.mm`,
`J/modules/juce_gui_extra/native/javascript/{index.js,check_native_interop.js,package.json}`,
`J/modules/juce_audio_processors/utilities/juce_ParameterAttachments.{h,cpp}`,
`J/examples/Plugins/WebViewPluginDemo.h`,
`J/examples/Plugins/WebViewPluginDemoGUI/package.json`,
`J/extras/Build/CMake/JUCEUtils.cmake`.

Secondary:
- [JUCE 8 Feature Overview: WebView UIs](https://juce.com/blog/juce-8-feature-overview-webview-uis/) — the official write-up (the "Tutorial: Web views in plugins and applications" URL 404s; this blog post is the current canonical page).
- [JanWilczek/juce-webview-tutorial](https://github.com/JanWilczek/juce-webview-tutorial) — the CMake zip/binary-data pattern reproduced in §1.4 (`plugin/CMakeLists.txt`).
- [JUCE#1415 — WebView crashing every few reloads](https://github.com/juce-framework/JUCE/issues/1415)
- [JUCE#1376 — Right clicking in WebView causes crash](https://github.com/juce-framework/JUCE/issues/1376)
- [JUCE#1577 — Bad WebBrowserComponent behavior in FL Studio on macOS](https://github.com/juce-framework/JUCE/issues/1577)
- [Forum: no keyboard input in WebBrowserComponent in Logic](https://forum.juce.com/t/anyone-know-why-webbrowsercomponent-within-my-audio-plugin-editor-cant-get-any-keyboard-input-in-logic/53053)
- [Forum: WebView crashes PluginVal](https://forum.juce.com/t/webview-crashes-pluginval/64703)
- [Forum: WKWebView can't open file:// urls from sandboxed apps](https://forum.juce.com/t/bug-with-fix-webbrowsercomponent-wkwebview-cant-open-file-urls-from-sandboxed-apps/43936)
