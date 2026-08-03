#include "WebEditor.h"

#include "Parameters.h"
#include "dsp/ChainOrder.h"
#include "library/ModelLibrary.h"
#include "library/Tone3000Client.h"

#include <cstring>
#include <iterator>
#include <unordered_map>

#ifndef TUBAMP_WEBUI_DEV
 #define TUBAMP_WEBUI_DEV 0
#endif

#ifndef TUBAMP_BUNDLED_UI
 #define TUBAMP_BUNDLED_UI 0
#endif

#ifndef TUBAMP_WEBUI_ZIP_PREFIX
 #define TUBAMP_WEBUI_ZIP_PREFIX "dist/"
#endif

#if TUBAMP_BUNDLED_UI
 #include "WebUiData.h"
#endif

namespace tubamp
{
namespace
{
#if TUBAMP_WEBUI_DEV
// The Vite dev server. Hardcoded on both sides (vite.config.ts pins strictPort).
const juce::String kDevServerOrigin { "http://localhost:5173" };
const juce::String kDevServerUrl { "http://localhost:5173/" };
#endif

// Relay names are the APVTS ids verbatim: getSliderState("input_trim") in JS.
const char* const kSliderIds[] = {
    params::inputTrim,     params::outputLevel,   params::gateThreshold,
    params::compThreshold, params::compRatio,     params::compAttack,
    params::compRelease,   params::compMakeup,
    params::driveGain,     params::driveTone,     params::driveLevel,
    params::ampInput,      params::ampOutput,     params::ampCalLevel,
    params::ampSlim,
    params::cabLowCut,     params::cabHighCut,
    params::eqBass,        params::eqMid,         params::eqTreble,
    params::modRate,       params::modDepth,      params::modMix,
    params::delayTime,     params::delayFeedback, params::delayMix,
    params::reverbSize,    params::reverbDamping, params::reverbMix };

const char* const kToggleIds[] = {
    params::gateOn,  params::compOn,  params::driveOn, params::ampOn,
    params::cabOn,   params::eqOn,    params::modOn,   params::delayOn,
    params::reverbOn, params::ampCalInput };

const char* const kComboIds[] = { params::ampOutMode, params::modType };

static_assert (std::size (kSliderIds) == 29, "29 float params are frozen");
static_assert (std::size (kToggleIds) == 10, "10 bool params are frozen");
static_assert (std::size (kComboIds) == 2, "2 choice params are frozen");

//==============================================================================
juce::var makeObject (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    juce::DynamicObject::Ptr d { new juce::DynamicObject };

    for (const auto& p : props)
        d->setProperty (juce::Identifier (p.first), p.second);

    return juce::var { d.get() };
}

/** The `{ error?: string }` shape every fallible native function returns: an empty
    error string means success, and the property is simply absent. */
juce::var resultVar (const juce::String& error)
{
    juce::DynamicObject::Ptr d { new juce::DynamicObject };

    if (error.isNotEmpty())
        d->setProperty ("error", error);

    return juce::var { d.get() };
}

juce::var fileEntryVar (const juce::File& file, const juce::String& displayName)
{
    return makeObject ({ { "path", file.getFullPathName() }, { "name", displayName } });
}

juce::var optionalVar (const std::optional<double>& value)
{
    return value.has_value() ? juce::var { *value } : juce::var();
}

/** The bridge spells "absent" as JSON null, never as an empty string. */
juce::var nullableStringVar (const juce::String& value)
{
    return value.isNotEmpty() ? juce::var { value } : juce::var();
}

juce::var stringArrayVar (const juce::StringArray& values)
{
    juce::Array<juce::var> out;

    for (const auto& value : values)
        out.add (juce::var { value });

    return out;
}

juce::var t3kModelVar (const Tone3000Client::Model& model)
{
    return makeObject ({ { "id",           model.id },
                         { "name",         model.name },
                         { "modelUrl",     model.modelUrl },
                         { "size",         model.size },
                         { "architecture", model.architecture },
                         { "kind",         model.kind } });
}

juce::var t3kToneVar (const Tone3000Client::Tone& tone)
{
    return makeObject ({ { "id",             tone.id },
                         { "title",          tone.title },
                         { "description",    tone.description },
                         { "gear",           tone.gear },
                         { "format",         tone.format },
                         { "imageUrl",       nullableStringVar (tone.imageUrl) },
                         { "creator",        makeObject ({ { "username",  tone.creator.username },
                                                           { "avatarUrl", nullableStringVar (tone.creator.avatarUrl) } }) },
                         { "downloadsCount", tone.downloadsCount },
                         { "favoritesCount", tone.favoritesCount },
                         { "favorited",      tone.favorited },
                         { "makes",          stringArrayVar (tone.makes) },
                         { "tags",           stringArrayVar (tone.tags) },
                         { "sizes",          stringArrayVar (tone.sizes) },
                         { "modelsCount",    tone.modelsCount },
                         { "createdAt",      tone.createdAt } });
}

//==============================================================================
#if TUBAMP_BUNDLED_UI
const char* mimeForExtension (const juce::String& extension)
{
    static const std::unordered_map<std::string, const char*> table {
        { "html", "text/html" },       { "htm",  "text/html" },
        { "js",   "text/javascript" }, { "mjs",  "text/javascript" },
        { "css",  "text/css" },        { "json", "application/json" },
        { "map",  "application/json" },{ "svg",  "image/svg+xml" },
        { "png",  "image/png" },       { "jpg",  "image/jpeg" },
        { "jpeg", "image/jpeg" },      { "gif",  "image/gif" },
        { "webp", "image/webp" },      { "avif", "image/avif" },
        { "ico",  "image/vnd.microsoft.icon" },
        { "woff", "font/woff" },       { "woff2","font/woff2" },
        { "ttf",  "font/ttf" },        { "otf",  "font/otf" },
        { "wasm", "application/wasm" },{ "txt",  "text/plain" } };

    const auto it = table.find (extension.toLowerCase().toStdString());
    // Unknown extensions get a safe default rather than the demo's assertion:
    // a Vite bundle can legitimately emit anything. .js MUST be text/javascript,
    // WKWebView refuses to execute a module served as octet-stream.
    return it != table.end() ? it->second : "application/octet-stream";
}

juce::ZipFile* webUiZip()
{
    if (webui::tubamp_webui_zip == nullptr || webui::tubamp_webui_zipSize <= 0)
        return nullptr;

    static juce::MemoryInputStream stream { webui::tubamp_webui_zip,
                                            (size_t) webui::tubamp_webui_zipSize,
                                            false };
    static juce::ZipFile zip { stream };   // non-owning: the BinaryData blob is static
    return &zip;
}
#endif // TUBAMP_BUNDLED_UI

juce::WebBrowserComponent::Resource makeResource (const juce::String& text, const char* mime)
{
    const auto utf8 = text.toRawUTF8();
    const auto numBytes = std::strlen (utf8);

    std::vector<std::byte> bytes (numBytes);

    if (numBytes > 0)
        std::memcpy (bytes.data(), utf8, numBytes);

    return { std::move (bytes), juce::String (mime) };
}

/** Shown instead of crashing when the plugin was built without a UI bundle
    (TUBAMP_BUNDLED_UI=OFF) or the bundled zip is empty/corrupt. */
juce::WebBrowserComponent::Resource missingBundlePage()
{
    return makeResource (
        "<!doctype html><meta charset=\"utf-8\"><title>tubamp</title>"
        "<style>html,body{margin:0;height:100%;background:#0a0a0a;color:#ededed;"
        "font:13px/1.6 ui-sans-serif,-apple-system,system-ui,sans-serif;"
        "display:flex;align-items:center;justify-content:center}"
        "div{text-align:center;max-width:38ch}h1{font-size:15px;font-weight:600;margin:0 0 8px}"
        "p{margin:0;color:#8f8f8f}code{color:#ededed}</style>"
        "<div><h1>UI bundle missing</h1><p>This build has no web UI baked in. "
        "Configure with <code>-DTUBAMP_BUNDLED_UI=ON</code>, or run "
        "<code>npm run dev</code> in <code>ui/</code> and use a "
        "<code>TUBAMP_BUNDLED_UI=OFF</code> build.</p></div>",
        "text/html");
}
} // namespace

//==============================================================================
bool SinglePageBrowser::pageAboutToLoad (const juce::String& newURL)
{
   #if TUBAMP_WEBUI_DEV
    if (newURL.startsWith (kDevServerOrigin))
        return true;
   #endif

    return newURL == juce::WebBrowserComponent::getResourceProviderRoot();
}

//==============================================================================
WebEditor::WebEditor (TubampAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    for (auto* id : kSliderIds) sliderRelays.push_back (std::make_unique<juce::WebSliderRelay> (id));
    for (auto* id : kToggleIds) toggleRelays.push_back (std::make_unique<juce::WebToggleButtonRelay> (id));
    for (auto* id : kComboIds)  comboRelays .push_back (std::make_unique<juce::WebComboBoxRelay> (id));

    web = std::make_unique<SinglePageBrowser> (buildOptions());
    addAndMakeVisible (*web);

    for (size_t i = 0; i < sliderRelays.size(); ++i)
        if (auto* param = proc.apvts.getParameter (kSliderIds[i]))
            sliderAtt.push_back (std::make_unique<juce::WebSliderParameterAttachment> (
                *param, *sliderRelays[i], proc.apvts.undoManager));

    for (size_t i = 0; i < toggleRelays.size(); ++i)
        if (auto* param = proc.apvts.getParameter (kToggleIds[i]))
            toggleAtt.push_back (std::make_unique<juce::WebToggleButtonParameterAttachment> (
                *param, *toggleRelays[i], proc.apvts.undoManager));

    for (size_t i = 0; i < comboRelays.size(); ++i)
        if (auto* param = proc.apvts.getParameter (kComboIds[i]))
            comboAtt.push_back (std::make_unique<juce::WebComboBoxParameterAttachment> (
                *param, *comboRelays[i], proc.apvts.undoManager));

    jassert (sliderAtt.size() == sliderRelays.size());
    jassert (toggleAtt.size() == toggleRelays.size());
    jassert (comboAtt.size() == comboRelays.size());

    proc.onChainChanged          = [this] { emitChainChanged(); };
    proc.library.onChanged       = [this] { emitLibraryChanged(); };
    proc.presets.onPresetChanged = [this] { emitPresetChanged(); pollModelAndIr (true); };

    lastModelPath = proc.getLoadedModelPath();
    lastIrPath = proc.getLoadedIrPath();
    lastLatencySamples = proc.getLatencySamples();

    // The peaks are monotonic maxima that only this editor's timer ever resets, so
    // with the window closed they accumulate the loudest moment of the whole
    // session. Discard that backlog now or the first frame after reopening slams
    // both meters to whatever the loudest passage was.
    proc.inputPeak .exchange (0.0f);
    proc.outputPeak.exchange (0.0f);

   #if TUBAMP_WEBUI_DEV
    web->goToURL (kDevServerUrl);
   #else
    web->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
   #endif

    setSize (1120, 700);
    setResizable (false, false);
    startTimerHz (30);
}

WebEditor::~WebEditor()
{
    // A Timer firing emitEventIfBrowserIsVisible into a half-destroyed browser, or a
    // processor callback into a dead editor, are both real crashes. Kill them first.
    stopTimer();
    proc.onChainChanged = nullptr;
    proc.library.onChanged = nullptr;
    proc.presets.onPresetChanged = nullptr;
}

void WebEditor::resized()
{
    if (web != nullptr)
        web->setBounds (getLocalBounds());
}

//==============================================================================
juce::var WebEditor::chainOrderVar() const
{
    juce::Array<juce::var> tokens;

    for (auto id : proc.getChainOrder())
        tokens.add (juce::var { chain::infoFor (id).token });

    return tokens;
}

juce::var WebEditor::modelsVar() const
{
    juce::Array<juce::var> out;

    for (const auto& e : proc.library.getModels())
        out.add (fileEntryVar (e.file, e.displayName));

    return out;
}

juce::var WebEditor::irsVar() const
{
    juce::Array<juce::var> out;

    for (const auto& e : proc.library.getIrs())
        out.add (fileEntryVar (e.file, e.displayName));

    return out;
}

juce::var WebEditor::presetListVar() const
{
    juce::Array<juce::var> out;

    for (const auto& preset : proc.presets.getPresets())
    {
        juce::Array<juce::var> tags;

        for (const auto& tag : preset.tags)
            tags.add (juce::var { tag });

        out.add (makeObject ({ { "name",     preset.name },
                               { "path",     preset.file.getFullPathName() },
                               { "favorite", preset.favorite },
                               { "tags",     juce::var { tags } } }));
    }

    return out;
}

juce::var WebEditor::abVar() const
{
    return makeObject ({ { "activeSlot", proc.presets.getActiveSlot() },
                         { "aHasState",  proc.presets.slotHasState (0) },
                         { "bHasState",  proc.presets.slotHasState (1) } });
}

juce::var WebEditor::modelVar() const
{
    if (! proc.namEngine.hasModel())
        return {};

    const auto info = proc.namEngine.getModelInfo();

    return makeObject ({ { "path",           info.filePath },
                         { "name",           info.name },
                         { "sampleRateHz",   info.sampleRate },
                         { "loudnessDb",     optionalVar (info.loudnessDb) },
                         { "inputLevelDbu",  optionalVar (info.inputLevelDbu) },
                         { "outputLevelDbu", optionalVar (info.outputLevelDbu) },
                         { "gearType",       nullableStringVar (info.gearType) },
                         { "includesCab",    info.includesCab },
                         { "isSlimmable",    proc.namEngine.isSlimmable() },
                         { "latencySamples", proc.getLatencySamples() } });
}

juce::var WebEditor::irVar() const
{
    const auto path = proc.getLoadedIrPath();

    if (path.isEmpty())
        return {};

    const juce::File file { path };
    return fileEntryVar (file, file.getFileNameWithoutExtension());
}

juce::var WebEditor::t3kVar() const
{
    return makeObject ({ { "configured",    proc.tone3000.isConfigured() },
                         { "authenticated", proc.tone3000.isAuthenticated() },
                         { "username",      nullableStringVar (proc.tone3000.getUsername()) } });
}

juce::var WebEditor::uiStateVar() const
{
    return makeObject ({ { "chainOrder",        chainOrderVar() },
                         { "model",             modelVar() },
                         { "ir",                irVar() },
                         { "models",            modelsVar() },
                         { "irs",               irsVar() },
                         { "presets",           presetListVar() },
                         { "currentPresetName", proc.presets.getCurrentPresetName() },
                         { "ab",                abVar() },
                         { "t3k",               t3kVar() } });
}

//==============================================================================
void WebEditor::emit (const juce::Identifier& eventId, const juce::var& payload)
{
    if (web != nullptr)
        web->emitEventIfBrowserIsVisible (eventId, payload);
}

void WebEditor::emitChainChanged()
{
    emit ("chainChanged", makeObject ({ { "chainOrder", chainOrderVar() } }));
}

void WebEditor::emitLibraryChanged()
{
    emit ("libraryChanged", makeObject ({ { "models", modelsVar() }, { "irs", irsVar() } }));
}

void WebEditor::emitPresetChanged()
{
    emit ("presetChanged", makeObject ({ { "presets",           presetListVar() },
                                         { "currentPresetName", proc.presets.getCurrentPresetName() },
                                         { "ab",                abVar() } }));
}

void WebEditor::emitModelChanged()
{
    lastModelPath = proc.getLoadedModelPath();
    lastLatencySamples = proc.getLatencySamples();
    emit ("modelChanged", makeObject ({ { "model", modelVar() } }));
}

void WebEditor::emitIrChanged()
{
    lastIrPath = proc.getLoadedIrPath();
    emit ("irChanged", makeObject ({ { "ir", irVar() } }));
}

void WebEditor::emitT3kStatus()
{
    emit ("t3kStatus", t3kVar());
}

void WebEditor::emitT3kError (const juce::String& message)
{
    emit ("t3kError", makeObject ({ { "message", message } }));
}

void WebEditor::emitT3kError (const juce::String& message, juce::int64 modelId)
{
    emit ("t3kError", makeObject ({ { "message", message },
                                    { "modelId", modelId } }));
}

void WebEditor::pollModelAndIr (bool forceEmit)
{
    const auto modelPath = proc.getLoadedModelPath();
    const auto irPath = proc.getLoadedIrPath();
    const auto latency = proc.getLatencySamples();

    if (forceEmit || modelPath != lastModelPath || latency != lastLatencySamples)
        emitModelChanged();

    if (forceEmit || irPath != lastIrPath)
        emitIrChanged();
}

void WebEditor::timerCallback()
{
    // Reset-on-read, exactly as the old native editor did (PluginEditor.cpp:127-128):
    // each frame reports the peak accumulated since the previous frame, and the
    // decay/ballistics live in the UI.
    emit ("meters", makeObject ({ { "in",  (double) proc.inputPeak .exchange (0.0f) },
                                  { "out", (double) proc.outputPeak.exchange (0.0f) } }));

    // Model / IR / latency have no push notification; poll at 5 Hz (every 6th tick).
    if (++pollDivider >= 6)
    {
        pollDivider = 0;
        pollModelAndIr (false);
    }
}

//==============================================================================
juce::WebBrowserComponent::Options WebEditor::buildOptions()
{
    using Options = juce::WebBrowserComponent::Options;

    auto options = Options {}
                       .withNativeIntegrationEnabled()
                       // Without this JUCE swaps in a blank page whenever the editor is
                       // hidden and reloads on show — the exact churn that crashes
                       // pluginval/auval (research doc §5.1).
                       .withKeepPageLoadedWhenBrowserIsHidden()
                       .withOptionsFrom (paramIndexReceiver);

    for (auto& relay : sliderRelays) options = options.withOptionsFrom (*relay);
    for (auto& relay : toggleRelays) options = options.withOptionsFrom (*relay);
    for (auto& relay : comboRelays)  options = options.withOptionsFrom (*relay);

    // Static boot data: lands as arrays under window.__JUCE__.initialisationData.
    for (const auto& block : chain::blockInfos)
        options = options.withInitialisationData (
            "tubampBlocks",
            makeObject ({ { "token",       block.token },
                          { "displayName", block.displayName },
                          { "shortName",   block.shortName },
                          { "enableParam", block.enableParamId } }));

   #ifdef JucePlugin_VersionString
    options = options.withInitialisationData ("tubampVersion", JucePlugin_VersionString);
   #endif

    // -----------------------------------------------------------------------
    // Native functions. Always invoked on the message thread; the completion may
    // be resolved later, from any thread.
    // -----------------------------------------------------------------------
    options = options
        .withNativeFunction ("getUiState", [this] (auto&, auto complete)
        {
            complete (uiStateVar());
        })
        .withNativeFunction ("setChainOrder", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            juce::StringArray tokens;

            if (auto* array = args[0].getArray())
                for (const auto& token : *array)
                    tokens.add (token.toString());

            // An empty array is a deliberately empty chain; chain::fromString falls
            // back to the default order for empty input, so spell it out.
            proc.setChainOrder (chain::fromString (tokens.isEmpty()
                                                       ? juce::String (chain::emptyChainToken)
                                                       : tokens.joinIntoString (",")));
            complete ({});
        })
        .withNativeFunction ("loadModel", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            const auto error = proc.loadModel (juce::File (args[0].toString()));
            emitModelChanged();
            complete (resultVar (error));
        })
        .withNativeFunction ("clearModel", [this] (auto&, auto complete)
        {
            proc.clearModel();
            emitModelChanged();
            complete ({});
        })
        .withNativeFunction ("loadIr", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            const auto error = proc.loadIr (juce::File (args[0].toString()));
            emitIrChanged();
            complete (resultVar (error));
        })
        .withNativeFunction ("clearIr", [this] (auto&, auto complete)
        {
            proc.clearIr();
            emitIrChanged();
            complete ({});
        })
        .withNativeFunction ("importModel", [this] (auto&, auto complete)
        {
            if (chooserActive)
            {
                complete (resultVar ("A file dialog is already open."));
                return;
            }

            chooserActive = true;
            juce::Component::SafePointer<WebEditor> safeThis (this);

            fileChooser = std::make_unique<juce::FileChooser> ("Import NAM model", juce::File(), "*.nam");
            fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                          | juce::FileBrowserComponent::canSelectFiles,
                [safeThis, complete] (const juce::FileChooser& fc)
                {
                    if (safeThis == nullptr)
                        return;

                    safeThis->chooserActive = false;
                    const auto file = fc.getResult();

                    if (! file.existsAsFile())      // cancelled: not an error
                    {
                        complete (resultVar ({}));
                        return;
                    }

                    juce::String error;
                    const auto installed = safeThis->proc.library.importModel (file, error);

                    if (! installed.existsAsFile())
                    {
                        complete (resultVar (error.isNotEmpty() ? error : juce::String ("Import failed.")));
                        return;
                    }

                    const auto loadError = safeThis->proc.loadModel (installed);
                    safeThis->emitModelChanged();

                    if (loadError.isNotEmpty())
                    {
                        complete (resultVar (loadError));
                        return;
                    }

                    complete (fileEntryVar (installed, installed.getFileNameWithoutExtension()));
                });
        })
        .withNativeFunction ("importIr", [this] (auto&, auto complete)
        {
            if (chooserActive)
            {
                complete (resultVar ("A file dialog is already open."));
                return;
            }

            chooserActive = true;
            juce::Component::SafePointer<WebEditor> safeThis (this);

            fileChooser = std::make_unique<juce::FileChooser> ("Import impulse response", juce::File(), "*.wav");
            fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                          | juce::FileBrowserComponent::canSelectFiles,
                [safeThis, complete] (const juce::FileChooser& fc)
                {
                    if (safeThis == nullptr)
                        return;

                    safeThis->chooserActive = false;
                    const auto file = fc.getResult();

                    if (! file.existsAsFile())
                    {
                        complete (resultVar ({}));
                        return;
                    }

                    juce::String error;
                    const auto installed = safeThis->proc.library.importIr (file, error);

                    if (! installed.existsAsFile())
                    {
                        complete (resultVar (error.isNotEmpty() ? error : juce::String ("Import failed.")));
                        return;
                    }

                    const auto loadError = safeThis->proc.loadIr (installed);
                    safeThis->emitIrChanged();

                    if (loadError.isNotEmpty())
                    {
                        complete (resultVar (loadError));
                        return;
                    }

                    complete (fileEntryVar (installed, installed.getFileNameWithoutExtension()));
                });
        })
        .withNativeFunction ("loadPreset", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            const auto result = proc.presets.loadPreset (juce::File (args[0].toString()));
            complete (resultVar (result.wasOk() ? juce::String() : result.getErrorMessage()));
        })
        .withNativeFunction ("savePresetAs", [this] (auto&, auto complete)
        {
            // Native prompt: WebViews in plugin editors get no keyboard in Logic.
            auto* alert = new juce::AlertWindow ("Save preset",
                                                 "Enter a name for this preset:",
                                                 juce::MessageBoxIconType::NoIcon);
            alert->addTextEditor ("name", proc.presets.getCurrentPresetName());
            alert->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
            alert->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

            // The AlertWindow is a free-standing desktop window that outlives this
            // editor if the host closes it while the dialog is up.
            juce::Component::SafePointer<WebEditor> safeThis (this);

            alert->enterModalState (true, juce::ModalCallbackFunction::create (
                [safeThis, alert, complete] (int result)
                {
                    const std::unique_ptr<juce::AlertWindow> owned (alert);

                    if (safeThis == nullptr)
                        return;

                    if (result != 1)
                    {
                        complete (resultVar ({}));       // cancelled
                        return;
                    }

                    const auto name = alert->getTextEditorContents ("name").trim();

                    if (name.isEmpty())
                    {
                        complete (resultVar ("Preset name cannot be empty."));
                        return;
                    }

                    const auto saved = safeThis->proc.presets.savePreset (name);
                    complete (resultVar (saved.wasOk() ? juce::String() : saved.getErrorMessage()));
                }), false);
        })
        .withNativeFunction ("deletePreset", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            const auto result = proc.presets.deletePreset (juce::File (args[0].toString()));
            complete (resultVar (result.wasOk() ? juce::String() : result.getErrorMessage()));
        })
        .withNativeFunction ("setPresetFavorite", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            proc.presets.setFavorite (juce::File (args[0].toString()), (bool) args[1]);
            complete ({});
        })
        .withNativeFunction ("abCapture", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            // captureToSlot/recallSlot do not fire onPresetChanged themselves.
            proc.presets.captureToSlot (juce::jlimit (0, 1, (int) args[0]));
            emitPresetChanged();
            complete ({});
        })
        .withNativeFunction ("abRecall", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            proc.presets.recallSlot (juce::jlimit (0, 1, (int) args[0]));
            emitPresetChanged();
            pollModelAndIr (true);
            complete ({});
        })
        .withNativeFunction ("t3kConfigure", [this] (auto&, auto complete)
        {
            auto* alert = new juce::AlertWindow ("TONE3000",
                                                 "Paste your TONE3000 publishable key:",
                                                 juce::MessageBoxIconType::NoIcon);
            alert->addTextEditor ("key", proc.tone3000.getClientId());
            alert->getTextEditor ("key")->setTextToShowWhenEmpty ("t3k_pub_...", juce::Colours::grey);
            alert->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
            alert->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

            juce::Component::SafePointer<WebEditor> safeThis (this);

            alert->enterModalState (true, juce::ModalCallbackFunction::create (
                [safeThis, alert, complete] (int result)
                {
                    const std::unique_ptr<juce::AlertWindow> owned (alert);

                    if (safeThis == nullptr)
                        return;

                    if (result != 1)
                    {
                        complete (resultVar ({}));
                        return;
                    }

                    safeThis->proc.tone3000.setClientId (alert->getTextEditorContents ("key").trim());
                    safeThis->emitT3kStatus();
                    complete (resultVar ({}));
                }), false);
        })
        .withNativeFunction ("t3kSignIn", [this] (auto&, auto complete)
        {
            complete ({});   // fire and forget: the result arrives as an event

            if (! proc.tone3000.isConfigured())
            {
                emitT3kError ("Add your TONE3000 publishable key in Settings to sign in.");
                return;
            }

            juce::Component::SafePointer<WebEditor> safeThis (this);

            proc.tone3000.startSignIn (
                [safeThis]
                {
                    if (safeThis != nullptr)
                        safeThis->emitT3kStatus();   // now carries the username too
                },
                [safeThis] (juce::String error)
                {
                    if (safeThis != nullptr)
                        safeThis->emitT3kError (error);
                });
        })
        .withNativeFunction ("t3kSignOut", [this] (auto&, auto complete)
        {
            proc.tone3000.signOut();
            emitT3kStatus();
            complete ({});
        })
        .withNativeFunction ("t3kStartSelectFlow", [this] (auto&, auto complete)
        {
            complete ({});   // fire and forget: the result arrives as an event

            if (! proc.tone3000.isConfigured())
            {
                emitT3kError ("Add your TONE3000 publishable key in Settings to browse.");
                return;
            }

            // Tone3000Client delivers on the message thread at an arbitrary later
            // time, well after this editor may have been destroyed.
            juce::Component::SafePointer<WebEditor> safeThis (this);

            proc.tone3000.startSelectFlow (
                [safeThis] (Tone3000Client::ToneModels toneModels)
                {
                    if (safeThis == nullptr)
                        return;

                    safeThis->emitT3kStatus();   // the flow just authenticated us

                    if (toneModels.models.isEmpty())
                    {
                        // The old UI failed silently here; the spec asks for a toast.
                        safeThis->emitT3kError ("That tone has no downloadable NAM models.");
                        return;
                    }

                    juce::Array<juce::var> models;

                    for (const auto& model : toneModels.models)
                        models.add (t3kModelVar (model));

                    safeThis->emit ("t3kToneSelected",
                                    makeObject ({ { "toneId", toneModels.toneId },
                                                  { "models", juce::var { models } } }));
                },
                [safeThis] (juce::String error)
                {
                    if (safeThis != nullptr)
                        safeThis->emitT3kError (error);
                });
        })
        .withNativeFunction ("t3kBrowse", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            if (! proc.tone3000.isConfigured())
            {
                complete (resultVar ("Add your TONE3000 publishable key in Settings to browse."));
                return;
            }

            Tone3000Client::BrowseRequest request;

            if (auto* object = args[0].getDynamicObject())
            {
                request.kind  = object->getProperty ("kind").toString();
                request.shelf = object->getProperty ("shelf").toString();
                request.sort  = object->getProperty ("sort").toString();
                request.query = object->getProperty ("query").toString();
                request.gear  = object->getProperty ("gear").toString();   // null -> empty = all
                request.page  = juce::jmax (1, (int) object->getProperty ("page"));
            }

            // Resolved from the client's message-thread callback, arbitrarily later.
            juce::Component::SafePointer<WebEditor> safeThis (this);

            proc.tone3000.browse (request,
                [safeThis, complete] (Tone3000Client::TonePage page)
                {
                    if (safeThis == nullptr)
                        return;

                    juce::Array<juce::var> tones;

                    for (const auto& tone : page.tones)
                    {
                        safeThis->toneIsIr[tone.id] = tone.format == "ir";
                        tones.add (t3kToneVar (tone));
                    }

                    complete (makeObject ({ { "tones",      juce::var { tones } },
                                            { "page",       page.page },
                                            { "totalPages", page.totalPages },
                                            { "total",      page.total } }));
                },
                [safeThis, complete] (juce::String error)
                {
                    if (safeThis != nullptr)
                        complete (resultVar (error));
                });
        })
        .withNativeFunction ("t3kListModels", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            const auto toneId = (juce::int64) args[0];
            const auto entry = toneIsIr.find (toneId);
            const auto isIr = entry != toneIsIr.end() && entry->second;

            juce::Component::SafePointer<WebEditor> safeThis (this);

            proc.tone3000.listToneModels (toneId, isIr,
                [safeThis, complete] (juce::Array<Tone3000Client::Model> models)
                {
                    if (safeThis == nullptr)
                        return;

                    juce::Array<juce::var> out;

                    for (const auto& model : models)
                        out.add (t3kModelVar (model));

                    complete (makeObject ({ { "models", juce::var { out } } }));
                },
                [safeThis, complete] (juce::String error)
                {
                    if (safeThis != nullptr)
                        complete (resultVar (error));
                });
        })
        .withNativeFunction ("t3kSetFavorite", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            juce::Component::SafePointer<WebEditor> safeThis (this);

            proc.tone3000.setFavorite ((juce::int64) args[0], (bool) args[1],
                [safeThis, complete]
                {
                    if (safeThis != nullptr)
                        complete (resultVar ({}));
                },
                [safeThis, complete] (juce::String error)
                {
                    if (safeThis != nullptr)
                        complete (resultVar (error));
                });
        })
        .withNativeFunction ("t3kDownloadModel", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            complete ({});   // progress and completion arrive as events

            Tone3000Client::Model model;

            if (auto* object = args[0].getDynamicObject())
            {
                model.id           = (juce::int64) object->getProperty ("id");
                model.name         = object->getProperty ("name").toString();
                model.modelUrl     = object->getProperty ("modelUrl").toString();
                model.size         = object->getProperty ("size").toString();
                model.architecture = object->getProperty ("architecture").toString();
                model.kind         = object->getProperty ("kind").toString();
            }

            if (model.modelUrl.isEmpty())
            {
                emitT3kError ("That model has no download URL.", model.id);
                return;
            }

            juce::Component::SafePointer<WebEditor> safeThis (this);
            const auto modelId = model.id;
            const auto destDir = model.kind == "wav" ? ModelLibrary::getIrsDir()
                                                     : ModelLibrary::getModelsDir();

            proc.tone3000.downloadModel (model, destDir,
                [safeThis, modelId] (float progress)
                {
                    if (safeThis != nullptr)
                        safeThis->emit ("t3kProgress",
                                        makeObject ({ { "modelId",  modelId },
                                                      { "progress", (double) progress } }));
                },
                [safeThis, modelId] (juce::File savedFile)
                {
                    if (safeThis == nullptr)
                        return;

                    if (! savedFile.existsAsFile())
                    {
                        safeThis->emitT3kError ("The downloaded model could not be saved.", modelId);
                        return;
                    }

                    // The client writes straight to disk, so ModelLibrary never fired;
                    // the payload covers both dirs. The UI decides whether to
                    // loadModel/loadIr(path) afterwards.
                    safeThis->emitLibraryChanged();
                    safeThis->emit ("t3kComplete",
                                    makeObject ({ { "modelId", modelId },
                                                  { "path",    savedFile.getFullPathName() } }));
                },
                [safeThis, modelId] (juce::String error)
                {
                    if (safeThis != nullptr)
                        safeThis->emitT3kError (error, modelId);
                });
        });

    // -----------------------------------------------------------------------
    // Resource provider. The dev origin is only allowed in dev builds — the
    // Access-Control-Allow-Origin header it produces is a hole in ship builds.
    // -----------------------------------------------------------------------
   #if TUBAMP_WEBUI_DEV
    options = options.withResourceProvider ([this] (const auto& path) { return getResource (path); },
                                            kDevServerOrigin);
   #else
    options = options.withResourceProvider ([this] (const auto& path) { return getResource (path); });
   #endif

    return options;
}

//==============================================================================
std::optional<juce::WebBrowserComponent::Resource> WebEditor::getResource (const juce::String& path)
{
    // The macOS scheme handler hands us [url path]: "/" for the root, otherwise
    // "/index.html", "/assets/index-abc123.js", ...
    const auto relative = (path == "/" ? juce::String ("index.html")
                                       : path.fromFirstOccurrenceOf ("/", false, false));

   #if TUBAMP_BUNDLED_UI
    if (auto* zip = webUiZip())
    {
        if (auto* entry = zip->getEntry (juce::String (TUBAMP_WEBUI_ZIP_PREFIX) + relative))
        {
            if (const std::unique_ptr<juce::InputStream> stream { zip->createStreamForEntry (*entry) })
            {
                std::vector<std::byte> bytes ((size_t) juce::jmax ((juce::int64) 0, stream->getTotalLength()));

                if (! bytes.empty())
                    stream->read (bytes.data(), (int) bytes.size());

                const auto extension = relative.fromLastOccurrenceOf (".", false, false);
                return juce::WebBrowserComponent::Resource { std::move (bytes),
                                                             juce::String (mimeForExtension (extension)) };
            }
        }
    }
   #endif

    // No bundle (or a broken one): serve a readable placeholder for the document
    // request rather than a bare 404 / blank WKWebView.
    if (relative == "index.html")
        return missingBundlePage();

    return std::nullopt;
}
} // namespace tubamp
