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
    params::reverbSize,    params::reverbDamping, params::reverbMix,
    // Instances 2 and 3 of the duplicable kinds, plus the amp's own tone stack. Array
    // order is free (a relay is keyed by its id in JS); it mirrors Parameters.cpp's
    // append order so the two lists stay diffable.
    params::comp2Threshold,  params::comp2Ratio,     params::comp2Attack,
    params::comp2Release,    params::comp2Makeup,
    params::comp3Threshold,  params::comp3Ratio,     params::comp3Attack,
    params::comp3Release,    params::comp3Makeup,
    params::drive2Gain,      params::drive2Tone,     params::drive2Level,
    params::drive3Gain,      params::drive3Tone,     params::drive3Level,
    params::eq2Bass,         params::eq2Mid,         params::eq2Treble,
    params::eq3Bass,         params::eq3Mid,         params::eq3Treble,
    params::mod2Rate,        params::mod2Depth,      params::mod2Mix,
    params::mod3Rate,        params::mod3Depth,      params::mod3Mix,
    params::delay2Time,      params::delay2Feedback, params::delay2Mix,
    params::delay3Time,      params::delay3Feedback, params::delay3Mix,
    params::reverb2Size,     params::reverb2Damping, params::reverb2Mix,
    params::reverb3Size,     params::reverb3Damping, params::reverb3Mix,
    params::ampEqBass,       params::ampEqMid,       params::ampEqTreble,
    // Stereo chain (docs/STEREO.md §4), appended in spec order: delay ratio/width
    // per instance, then reverb width per instance.
    params::delayRatio,      params::delay2Ratio,    params::delay3Ratio,
    params::delayWidth,      params::delay2Width,    params::delay3Width,
    params::reverbWidth,     params::reverb2Width,   params::reverb3Width,
    // Split/mix chain (docs/SPLIT.md §2), appended in spec order.
    params::amp2Input,       params::amp2Output,     params::splitXover,
    params::mixALevel,       params::mixBLevel,      params::mixAPan,
    params::mixBPan,         params::mixLevel };

const char* const kToggleIds[] = {
    params::gateOn,  params::compOn,  params::driveOn, params::ampOn,
    params::cabOn,   params::eqOn,    params::modOn,   params::delayOn,
    params::reverbOn, params::ampCalInput,
    params::fx1On,   params::fx2On,   params::fx3On,
    params::comp2On,   params::comp3On,
    params::drive2On,  params::drive3On,
    params::eq2On,     params::eq3On,
    params::mod2On,    params::mod3On,
    params::delay2On,  params::delay3On,
    params::reverb2On, params::reverb3On,
    params::ampEqOn,
    // Split/mix chain (docs/SPLIT.md §2), appended in spec order. amp_stereo is
    // gone — SUPERSEDED by amp2 as a chain block (docs/SPLIT.md, top).
    params::amp2On,  params::splitOn,  params::mixOn,  params::mixBPhase };

const char* const kComboIds[] = { params::ampOutMode, params::modType,
                                  params::mod2Type,   params::mod3Type,
                                  // stereo chain (docs/STEREO.md §4)
                                  params::delayMode,  params::delay2Mode, params::delay3Mode,
                                  // split/mix chain (docs/SPLIT.md §2)
                                  params::splitMode };

static_assert (std::size (kSliderIds) == 89, "29 frozen float params + 43 v2 instance / amp-EQ params + 9 stereo-chain params + 8 split/mix params");
static_assert (std::size (kToggleIds) == 30, "10 frozen bool params + 3 fx-slot bypasses + 13 v2 enables + 4 split/mix enables (amp_stereo removed)");
static_assert (std::size (kComboIds) == 8, "2 frozen choice params + the mod 2/3 types + delay_mode x3 + split_mode");

/** Upper bound on hosted parameters surfaced to the UI. A handful of plugins publish
    thousands; serializing all of them into every slot payload would cost more than it
    is worth when the plugin's own window is one click away. */
constexpr int kMaxHostedParams = 256;

// Editor geometry. The window used to be frozen at 1120x700; it is now resizable, so
// these are a starting point and a sane range rather than the truth. The default is
// deliberately roomy (ToneX-class): the amp dock lays out three columns side by side
// and the board benefits from the extra height.
constexpr int kDefaultEditorWidth = 1280, kDefaultEditorHeight = 800;
// Floor derived from the content, not picked: the amp dock's three columns (model,
// levels, tone) are the widest fixed content — see the --min-app-w derivation in
// ui/src/theme/tokens.css, which must stay in sync with these numbers.
constexpr int kMinEditorWidth = 1000, kMinEditorHeight = 600;
// Ceiling only to stop a stray drag creating an absurd window; large enough for any
// hosted plugin editor measured so far.
constexpr int kMaxEditorWidth = 3200, kMaxEditorHeight = 2000;

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

/** Hosted parameters are plain AudioProcessorParameters, not RangedAudioParameters,
    so the only value representation they all guarantee is normalised 0..1 plus the
    plugin's own formatted string. That is exactly what crosses the bridge. */
juce::var hostedParamVar (const juce::AudioProcessorParameter& param, int index)
{
    const float value = param.getValue();

    return makeObject ({ { "index", index },
                         { "name",  param.getName (64) },
                         { "label", param.getLabel() },
                         { "value", (double) value },
                         { "text",  param.getText (value, 32) } });
}
} // namespace

//==============================================================================
struct FxEditorWindow final : public juce::DocumentWindow
{
    FxEditorWindow (juce::AudioProcessor& processor, const juce::String& title,
                    std::function<void()> onCloseRequested)
        : DocumentWindow (title, juce::Colours::black, DocumentWindow::closeButton),
          onClose (std::move (onCloseRequested))
    {
        setUsingNativeTitleBar (true);

        // A plugin that ships no editor still gets a usable window: JUCE's generic
        // editor renders its parameter list. Better than a dead menu item.
        auto* editor = processor.hasEditor() ? processor.createEditorIfNeeded() : nullptr;

        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (processor);

        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        if (onClose != nullptr)
            onClose();
    }

    std::function<void()> onClose;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxEditorWindow)
};

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
    proc.onFxSlotChanged         = [this] (int slot) { emitFxSlotChanged (slot); };

    // Fires before the processor retires a slot's instance, whatever caused it — the
    // panel, a preset load, an A/B recall or a host state restore. The window showing
    // that instance has to go first: an AudioProcessor must outlive its editor.
    proc.onFxSlotRetiring = [this] (int slot)
    {
        if (gestureSlot == slot)
            endOutstandingFxGesture();

        // Both places a hosted editor can live. An embedded one is just as fatal to
        // leave behind as a windowed one: the instance behind it is about to die.
        if (embedSlot == slot)
        {
            unmountEmbed();
            emitEmbedChanged();
        }

        closeFxWindow (slot);
    };

    lastModelPath = proc.getLoadedModelPath();
    lastModelPathB = proc.getLoadedModelPathB();
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

    // Resizable, but the grip lives in the page: a ResizableCornerComponent would be a
    // JUCE-painted child underneath a native WebView that covers the whole editor, so it
    // would be neither visible nor clickable. See the setEditorSize native function.
    setResizable (true, false);
    setResizeLimits (kMinEditorWidth, kMinEditorHeight, kMaxEditorWidth, kMaxEditorHeight);

    const auto saved = proc.getEditorSize();
    setSize (saved.x > 0 ? juce::jlimit (kMinEditorWidth, kMaxEditorWidth, saved.x) : kDefaultEditorWidth,
             saved.y > 0 ? juce::jlimit (kMinEditorHeight, kMaxEditorHeight, saved.y) : kDefaultEditorHeight);

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
    proc.onFxSlotChanged = nullptr;

    // Every hosted editor dies with us. A window left open would be showing a plugin
    // the processor is free to retire the moment we stop listening for the hook —
    // and the hook itself must not outlive the windows it closes.
    endOutstandingFxGesture();
    unmountEmbed();
    proc.onFxSlotRetiring = nullptr;

    for (auto& window : fxWindows)
        window.reset();
}

//==============================================================================
juce::Rectangle<int> WebEditor::usableScreenArea() const
{
    // The display we are actually on, minus menu bar and dock. Feasibility of an embed
    // is judged against this rather than kMaxEditorWidth/Height: a 3200x2000 ceiling
    // says yes to plugins that could never fit a laptop screen.
    const auto& displays = juce::Desktop::getInstance().getDisplays();

    if (auto* display = displays.getDisplayForRect (getScreenBounds()))
        return display->userArea;

    return displays.getPrimaryDisplay() != nullptr ? displays.getPrimaryDisplay()->userArea
                                                   : juce::Rectangle<int> { 0, 0, kMaxEditorWidth, kMaxEditorHeight };
}

juce::var WebEditor::editorSizeVar() const
{
    const auto screen = usableScreenArea();

    // The reported minimum is the EFFECTIVE one: while a plugin is embedded the window
    // cannot shrink below what that plugin needs, and the page's grip clamps against
    // exactly these numbers.
    return makeObject ({ { "width",     getWidth() },
                         { "height",    getHeight() },
                         { "minWidth",  juce::jmax (kMinEditorWidth, embedMinWindow.x) },
                         { "minHeight", juce::jmax (kMinEditorHeight, embedMinWindow.y) },
                         { "maxWidth",  juce::jmin (kMaxEditorWidth, screen.getWidth()) },
                         { "maxHeight", juce::jmin (kMaxEditorHeight, screen.getHeight()) } });
}

void WebEditor::applyEmbedSizeFloor()
{
    const auto screen = usableScreenArea();

    const int minW = juce::jlimit (kMinEditorWidth, juce::jmin (kMaxEditorWidth, screen.getWidth()),
                                   juce::jmax (kMinEditorWidth, embedMinWindow.x));
    const int minH = juce::jlimit (kMinEditorHeight, juce::jmin (kMaxEditorHeight, screen.getHeight()),
                                   juce::jmax (kMinEditorHeight, embedMinWindow.y));

    setResizeLimits (minW, minH, kMaxEditorWidth, kMaxEditorHeight);

    // One deterministic grow, here, instead of a reactive loop reacting to its own
    // resize events: that raced the relayout and overshot, and mid-drag it fought the
    // user's hand.
    if (getWidth() < minW || getHeight() < minH)
        setSize (juce::jmax (getWidth(), minW), juce::jmax (getHeight(), minH));
    else
        emitEditorSizeChanged();
}

void WebEditor::emitEditorSizeChanged()
{
    emit ("editorSizeChanged", editorSizeVar());
}

juce::var WebEditor::embedVar (const juce::String& error) const
{
    return makeObject ({ { "slot",   embedSlot },
                         { "width",  embedEditor != nullptr ? embedEditor->getWidth() : 0 },
                         { "height", embedEditor != nullptr ? embedEditor->getHeight() : 0 },
                         { "error",  error } });
}

void WebEditor::emitEmbedChanged (const juce::String& error)
{
    emit ("fxEmbedChanged", embedVar (error));
}

void WebEditor::unmountEmbed()
{
    if (embedEditor == nullptr)
    {
        embedSlot = -1;
        return;
    }

    removeChildComponent (embedEditor.get());

    // The hosted AU's own editor destructor calls editorBeingDeleted for us — but only
    // when its Cocoa view actually got created (juce_AudioUnitPluginFormat.mm:75-84).
    // A plugin whose view never arrived would otherwise leave a stale activeEditor and
    // every later open would silently fall back to the generic editor.
    auto* raw = embedEditor.get();
    auto* instance = proc.getFxInstance (embedSlot);

    embedEditor.reset();

    if (instance != nullptr && instance->getActiveEditor() == raw)
        instance->editorBeingDeleted (raw);

    embedSlot = -1;
    embedVisible = true;
    lastEmbedSize = {};
    embedTooSmall = false;

    if (embedMinWindow != juce::Point<int>())
    {
        embedMinWindow = {};
        applyEmbedSizeFloor();
    }
}

juce::String WebEditor::setEmbedSlot (int slot)
{
    if (slot == embedSlot)
        return {};

    unmountEmbed();

    if (slot < 0 || slot >= chain::numFxSlots)
    {
        emitEmbedChanged();
        return {};
    }

    auto* instance = proc.getFxInstance (slot);

    if (instance == nullptr)
        return "No plugin is loaded in this slot.";

    // One AudioProcessor has one active editor, so embedding and the pop-out window are
    // mutually exclusive for a given slot.
    closeFxWindow (slot);

    auto* created = instance->hasEditor() ? instance->createEditorIfNeeded() : nullptr;

    if (created == nullptr)
        created = new juce::GenericAudioProcessorEditor (*instance);

    // Judge feasibility against the display we are actually on, not against
    // kMaxEditorWidth/Height: a 3200x2000 ceiling accepts plugins that could never fit
    // a laptop screen, and we would only find out after mounting. The chrome around the
    // hole (header, dock, footer, gutters) is what the window needs on top of the
    // plugin itself. Refusing here is what makes the pop-out fallback reliable.
    constexpr int kEmbedChromeWidth = 32, kEmbedChromeHeight = 48 + 32 + 260 + 56 + 24;
    const auto screen = usableScreenArea();

    if (created->getWidth() + kEmbedChromeWidth > screen.getWidth()
        || created->getHeight() + kEmbedChromeHeight > screen.getHeight())
    {
        const auto needed = juce::String (created->getWidth()) + juce::String (" x ")
                            + juce::String (created->getHeight());

        if (instance->getActiveEditor() == created)
            instance->editorBeingDeleted (created);

        delete created;

        return "This plugin's editor (" + needed + ") is too large to show inside tubamp on this display.";
    }

    embedEditor.reset (created);
    embedTooSmall = false;
    embedSlot = slot;
    embedVisible = true;

    addAndMakeVisible (*embedEditor);
    embedEditor->toFront (false);

    // The size is provisional on purpose. Some plugins report a placeholder until their
    // view is attached, and a few resize themselves afterwards; childBoundsChanged
    // republishes whenever that happens, so the page treats every size as the latest
    // rather than the final one.
    lastEmbedSize = embedEditor->getBounds();
    resized();
    emitEmbedChanged();

    return {};
}

void WebEditor::childBoundsChanged (juce::Component* child)
{
    if (child == nullptr || child != embedEditor.get() || inEmbedLayout)
        return;

    // Fires for moves as well as resizes, and we move it ourselves in resized(); only a
    // genuine size change is worth telling the page about.
    const auto bounds = embedEditor->getBounds();

    if (bounds.getWidth() == lastEmbedSize.getWidth() && bounds.getHeight() == lastEmbedSize.getHeight())
        return;

    lastEmbedSize = bounds;
    emitEmbedChanged();
}

void WebEditor::endOutstandingFxGesture()
{
    if (gestureSlot >= 0)
        if (auto* param = fxParamFor (gestureSlot, gestureParam))
            param->endChangeGesture();

    gestureSlot = gestureParam = -1;
}

void WebEditor::resized()
{
    if (web != nullptr)
        web->setBounds (getLocalBounds());

    if (embedEditor != nullptr)
    {
        // The page tells us where the hole is; we only place the view in it. The rect
        // arrives in CSS pixels, which are the same units as our logical pixels here —
        // the WebView fills the editor 1:1 and JUCE applies no extra scale of its own.
        const juce::ScopedValueSetter<bool> guard (inEmbedLayout, true);

        const auto size = embedEditor->getBounds().withZeroOrigin();

        // A hosted editor cannot be clipped or scaled: it is a native view, so it is
        // bounded by the window and by nothing else in between. If the page has not
        // reserved room for it yet, or has reserved less than it needs, showing it
        // anyway would paint the plugin straight over the header and the dock. Hide it
        // and say why — the page turns that into "needs more room / pop out".
        const bool haveRect = ! embedRect.isEmpty();
        const bool fits = haveRect
                          && embedRect.getWidth() >= size.getWidth()
                          && embedRect.getHeight() >= size.getHeight();

        if (fits)
            embedEditor->setTopLeftPosition (embedRect.getCentreX() - size.getWidth() / 2,
                                             embedRect.getY());

        embedEditor->setVisible (embedVisible && fits);

        if (haveRect && ! fits && ! embedTooSmall)
        {
            embedTooSmall = true;
            emitEmbedChanged ("The plugin's editor needs more room than the window has.");
        }
        else if (fits && embedTooSmall)
        {
            embedTooSmall = false;
            emitEmbedChanged();
        }
    }

    proc.setEditorSize ({ getWidth(), getHeight() });
    emitEditorSizeChanged();
}

//==============================================================================
juce::var WebEditor::chainOrderVar() const
{
    juce::Array<juce::var> tokens;

    for (auto id : proc.getChainOrder())
        tokens.add (juce::var { chain::infoFor (id).token });

    return tokens;
}

juce::var WebEditor::chainRowsVar() const
{
    juce::Array<juce::var> rows;

    for (auto length : proc.getChainRows())
        rows.add (juce::var { length });

    return rows;
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

juce::var WebEditor::modelVarB() const
{
    if (! proc.namEngineB.hasModel())
        return {};

    const auto info = proc.namEngineB.getModelInfo();

    return makeObject ({ { "path",           info.filePath },
                         { "name",           info.name },
                         { "sampleRateHz",   info.sampleRate },
                         { "loudnessDb",     optionalVar (info.loudnessDb) },
                         { "inputLevelDbu",  optionalVar (info.inputLevelDbu) },
                         { "outputLevelDbu", optionalVar (info.outputLevelDbu) },
                         { "gearType",       nullableStringVar (info.gearType) },
                         { "includesCab",    info.includesCab },
                         { "isSlimmable",    proc.namEngineB.isSlimmable() },
                         // No separate B-side latency accessor: the L/R compensation
                         // ring makes proc.getLatencySamples() the truthful scalar for
                         // both channels (docs/STEREO.md §Latency reporting).
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
                         { "chainRows",         chainRowsVar() },
                         { "model",             modelVar() },
                         { "modelB",            modelVarB() },
                         { "ir",                irVar() },
                         { "models",            modelsVar() },
                         { "irs",               irsVar() },
                         { "presets",           presetListVar() },
                         { "currentPresetName", proc.presets.getCurrentPresetName() },
                         { "ab",                abVar() },
                         { "t3k",               t3kVar() },
                         { "fxSlots",           fxSlotsVar() },
                         { "fxSupported",       FxCatalog::isSupported() },
                         { "editorSize",        editorSizeVar() },
                         { "fxEmbed",           embedVar ({}) } });
}

//==============================================================================
juce::AudioProcessorParameter* WebEditor::fxParamFor (int slot, int index) const
{
    auto* instance = proc.getFxInstance (slot);

    if (instance == nullptr)
        return nullptr;

    const auto& all = instance->getParameters();

    return juce::isPositiveAndBelow (index, all.size()) ? all[index] : nullptr;
}

juce::var WebEditor::fxParamsVar (int slot) const
{
    juce::Array<juce::var> out;

    if (auto* instance = proc.getFxInstance (slot))
    {
        const auto& all = instance->getParameters();
        const int count = juce::jmin (all.size(), kMaxHostedParams);

        // Never skip: array position and `index` must stay identical, because the
        // metered fxParamValues push is index-aligned with this list and a hole in one
        // of the two would silently shift every value onto the wrong control.
        for (int i = 0; i < count; ++i)
        {
            if (auto* param = all[i])
                out.add (hostedParamVar (*param, i));
            else
                out.add (makeObject ({ { "index", i }, { "name", juce::String() },
                                       { "label", juce::String() }, { "value", 0.0 },
                                       { "text", juce::String() } }));
        }
    }

    return out;
}

juce::var WebEditor::fxSlotVar (int slot) const
{
    const auto info = proc.getFxSlotInfo (slot);

    // hasEditor is read from the instance rather than the record: a plugin only
    // answers it once it exists, and it decides whether the panel offers "Open plugin
    // window" as the plugin's own UI or as the generic fallback.
    auto* instance = proc.getFxInstance (slot);

    return makeObject ({ { "slot",            slot },
                         { "identifier",      info.identifier },
                         { "name",            info.name },
                         { "manufacturer",    info.manufacturer },
                         { "occupied",        info.occupied },
                         { "missing",         info.missing },
                         { "live",            info.live },
                         { "loading",         info.loading },
                         { "latencySamples",  info.latencySamples },
                         { "error",           info.error },
                         { "hasEditor",       instance != nullptr && instance->hasEditor() },
                         { "params",          fxParamsVar (slot) } });
}

juce::var WebEditor::fxSlotsVar() const
{
    juce::Array<juce::var> out;

    for (int slot = 0; slot < chain::numFxSlots; ++slot)
        out.add (fxSlotVar (slot));

    return out;
}

//==============================================================================
void WebEditor::emit (const juce::Identifier& eventId, const juce::var& payload)
{
    if (web != nullptr)
        web->emitEventIfBrowserIsVisible (eventId, payload);
}

void WebEditor::emitChainChanged()
{
    emit ("chainChanged", makeObject ({ { "chainOrder", chainOrderVar() },
                                        { "chainRows",  chainRowsVar() } }));
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

void WebEditor::emitModelBChanged()
{
    lastModelPathB = proc.getLoadedModelPathB();
    lastLatencySamples = proc.getLatencySamples();
    emit ("modelBChanged", makeObject ({ { "modelB", modelVarB() } }));
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

void WebEditor::emitFxSlotChanged (int slot)
{
    if (slot < 0 || slot >= chain::numFxSlots)
        return;

    emit ("fxSlotChanged", makeObject ({ { "slot", slot }, { "state", fxSlotVar (slot) } }));
}

void WebEditor::closeFxWindow (int slot)
{
    if (slot < 0 || slot >= chain::numFxSlots)
        return;

    fxWindows[(size_t) slot].reset();

    // A window we just closed cannot be the one being metered.
    if (watchedFxSlot == slot)
        gestureSlot = gestureParam = -1;
}

void WebEditor::pollModelAndIr (bool forceEmit)
{
    const auto modelPath = proc.getLoadedModelPath();
    const auto modelPathB = proc.getLoadedModelPathB();
    const auto irPath = proc.getLoadedIrPath();
    const auto latency = proc.getLatencySamples();

    // Sampled once, before either emitter: both of them refresh lastLatencySamples, so
    // re-reading it below would make the B side's latency clause dead and leave
    // modelB.latencySamples stale on a latency-only change (slim size, A model swap).
    const bool latencyChanged = latency != lastLatencySamples;

    if (forceEmit || modelPath != lastModelPath || latencyChanged)
        emitModelChanged();

    if (forceEmit || modelPathB != lastModelPathB || latencyChanged)
        emitModelBChanged();

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

    // Hosted parameter values, 10 Hz, for the one slot the user is looking at. A
    // hosted plugin can move its own parameters (its window, an LFO, a preset change)
    // and nothing notifies us, so this is a poll by necessity — but only ever over
    // one visible slot's parameters, never all three.
    if (++fxPollDivider >= 3)
    {
        fxPollDivider = 0;

        if (watchedFxSlot >= 0)
        {
            if (auto* instance = proc.getFxInstance (watchedFxSlot))
            {
                const auto& all = instance->getParameters();
                const int count = juce::jmin (all.size(), kMaxHostedParams);

                juce::Array<juce::var> values, texts;

                for (int i = 0; i < count; ++i)
                {
                    auto* param = all[i];

                    if (param == nullptr)
                    {
                        values.add (0.0);
                        texts.add (juce::String());
                        continue;
                    }

                    const float value = param->getValue();
                    values.add ((double) value);
                    texts.add (param->getText (value, 32));
                }

                emit ("fxParamValues", makeObject ({ { "slot",   watchedFxSlot },
                                                     { "values", values },
                                                     { "texts",  texts } }));
            }
        }
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

            // Row lengths are advisory and separately validated by the processor, so
            // anything that is not an array of numbers becomes an empty vector ("auto")
            // rather than taking the order down with it.
            std::vector<int> rows;

            if (auto* array = args[1].getArray())
            {
                rows.reserve ((size_t) array->size());

                for (const auto& length : *array)
                    rows.push_back ((int) length);
            }

            // parseOrder, not parseOrderOrLegacy: an empty array from the page is a
            // deliberately empty chain and must never resurrect the classic order.
            proc.setChainOrder (chain::parseOrder (tokens.joinIntoString (",")), rows);
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
        .withNativeFunction ("loadModelB", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            const auto error = proc.loadModelB (juce::File (args[0].toString()));
            emitModelBChanged();
            complete (resultVar (error));
        })
        .withNativeFunction ("clearModelB", [this] (auto&, auto complete)
        {
            proc.clearModelB();
            emitModelBChanged();
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
        //======================================================================
        // External AudioUnit slots. Hosted parameters deliberately do not go through
        // relays: a relay needs a static id at construction time, and a hosted
        // plugin's parameter list only exists once it has loaded.
        .withNativeFunction ("fxListPlugins", [this] (auto&, auto complete)
        {
            juce::Array<juce::var> plugins;

            for (const auto& entry : proc.fxCatalog.enumerateEffects())
                plugins.add (makeObject ({ { "identifier",   entry.identifier },
                                           { "name",         entry.name },
                                           { "manufacturer", entry.manufacturer },
                                           { "version",      entry.version } }));

            complete (makeObject ({ { "plugins",   plugins },
                                    { "supported", FxCatalog::isSupported() } }));
        })
        .withNativeFunction ("fxLoad", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            const int slot = (int) args[0];

            if (slot < 0 || slot >= chain::numFxSlots)
            {
                complete (resultVar ("Invalid slot"));
                return;
            }

            // Asynchronous by design: the outcome arrives as fxSlotChanged. Resolving
            // here only means the request was accepted.
            proc.loadFxPlugin (slot, args[1].toString());
            complete (resultVar ({}));
        })
        .withNativeFunction ("fxClear", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            proc.clearFxPlugin ((int) args[0]);
            complete ({});
        })
        .withNativeFunction ("fxOpenEditor", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            const int slot = (int) args[0];

            if (slot < 0 || slot >= chain::numFxSlots)
            {
                complete (resultVar ("Invalid slot"));
                return;
            }

            auto* instance = proc.getFxInstance (slot);

            if (instance == nullptr)
            {
                complete (resultVar ("No plugin is loaded in this slot."));
                return;
            }

            // A processor has exactly one active editor, so the embed has to go before
            // the window can have it.
            if (embedSlot == slot)
            {
                unmountEmbed();
                emitEmbedChanged();
            }

            if (auto& window = fxWindows[(size_t) slot]; window != nullptr)
            {
                window->toFront (true);
                complete (resultVar ({}));
                return;
            }

            fxWindows[(size_t) slot] = std::make_unique<FxEditorWindow> (
                *instance,
                proc.getFxSlotInfo (slot).name,
                [this, slot] { closeFxWindow (slot); });

            complete (resultVar ({}));
        })
        .withNativeFunction ("fxSetParam", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            if (auto* param = fxParamFor ((int) args[0], (int) args[1]))
                param->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, (float) (double) args[2]));

            complete ({});
        })
        .withNativeFunction ("fxBeginGesture", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            // Gestures must balance, so an unfinished one is closed before a new one
            // opens — a pointer capture lost to a window switch would otherwise leave
            // the hosted plugin thinking it is still being dragged.
            endOutstandingFxGesture();

            const int slot = (int) args[0], index = (int) args[1];

            if (auto* param = fxParamFor (slot, index))
            {
                param->beginChangeGesture();
                gestureSlot = slot;
                gestureParam = index;
            }

            complete ({});
        })
        .withNativeFunction ("fxEndGesture", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            if ((int) args[0] == gestureSlot && (int) args[1] == gestureParam)
                endOutstandingFxGesture();

            complete ({});
        })
        .withNativeFunction ("fxWatchSlot", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            const int slot = (int) args[0];
            watchedFxSlot = (slot >= 0 && slot < chain::numFxSlots) ? slot : -1;
            fxPollDivider = 0;
            complete ({});
        })
        //======================================================================
        // Window size. The page owns the resize grip because JUCE cannot give us one
        // here: a ResizableCornerComponent is a JUCE-painted child and the WebView is a
        // native view covering the whole editor, so the grip would be both invisible and
        // unclickable. Nor can the host resize us — JUCE's AU wrapper reverts a
        // host-driven resize on the next parentSizeChanged, while a plugin-driven
        // setSize propagates out through childBoundsChanged -> resizeHostWindow.
        .withNativeFunction ("getEditorSize", [this] (auto&, auto complete)
        {
            complete (editorSizeVar());
        })
        .withNativeFunction ("setEditorSize", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            const auto screen = usableScreenArea();

            const int width = juce::jlimit (kMinEditorWidth, juce::jmin (kMaxEditorWidth, screen.getWidth()),
                                            (int) args[0]);
            const int height = juce::jlimit (kMinEditorHeight, juce::jmin (kMaxEditorHeight, screen.getHeight()),
                                             (int) args[1]);

            setSize (width, height);
            complete (makeObject ({ { "width", getWidth() }, { "height", getHeight() } }));
        })
        //======================================================================
        // Embedding a hosted editor.
        .withNativeFunction ("fxSetEmbedSlot", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            complete (resultVar (setEmbedSlot ((int) args[0])));
        })
        .withNativeFunction ("fxSetEmbedRect", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            embedRect = { (int) args[0], (int) args[1], (int) args[2], (int) args[3] };
            resized();
            complete ({});
        })
        .withNativeFunction ("fxSetEmbedMinWindow", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            embedMinWindow = { (int) args[0], (int) args[1] };
            applyEmbedSizeFloor();
            complete ({});
        })
        .withNativeFunction ("fxSetEmbedVisible", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            embedVisible = (bool) args[0];

            if (embedEditor != nullptr)
                embedEditor->setVisible (embedVisible);

            complete ({});
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
