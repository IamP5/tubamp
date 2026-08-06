#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "PluginProcessor.h"

#include <array>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace tubamp
{
/** Refuses navigation away from our SPA: a plugin editor has no business following
    links, and JUCE#1376/#1415 make stray navigations a crash risk in hosts. */
struct SinglePageBrowser : juce::WebBrowserComponent
{
    using WebBrowserComponent::WebBrowserComponent;
    bool pageAboutToLoad (const juce::String& newURL) override;
};

/** Separate top-level window for a third-party plugin's own editor.

    A hosted editor can also be shown inside our window (see setEmbedSlot) — it is a
    native view, so it can be a sibling of the WebView even though it can never be part
    of the page. This window remains first-class regardless: it is the only way an
    oversized editor can be shown at all, and the fallback if an embedded native view
    turns out not to receive keyboard focus inside Logic. */
struct FxEditorWindow;

/**
    Resizable WebView editor.

    The window is sized by us and only by us: JUCE's AU wrapper reverts a host-driven
    resize on the next parentSizeChanged, while a plugin-driven setSize propagates out
    through childBoundsChanged -> resizeHostWindow. The grip the user drags is drawn in
    the page, because a ResizableCornerComponent would sit underneath a native WebView
    that covers the whole editor.

    Owns:
      - one relay per APVTS parameter (89 sliders / 30 toggles / 8 combos),
        each named *verbatim* after its APVTS id, plus the matching Web*Attachment;
      - the native-function surface described in docs/REACT-UI.md (chain, model, IR,
        preset, A/B and TONE3000 operations);
      - the backend -> frontend event stream (30 Hz meters + state notifications).

    Member ordering is load-bearing, see docs/research/juce-webview.md §2.1:
    relays -> browser -> attachments. Destruction runs in reverse, so attachments
    (which hold relay references) die first, then the browser (whose dtor notifies
    the relays as lifetime listeners), then the relays.

    Text entry lives in native juce::AlertWindow prompts because WebViews inside
    plugin editors get no keyboard input in Logic (research doc §5.3).
*/
class WebEditor final : public juce::AudioProcessorEditor,
                        private juce::Timer
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

    juce::WebBrowserComponent::Options buildOptions();
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& path);

    // --- payload builders (message thread)
    juce::var chainOrderVar() const;

    /** Row lengths of the board's arrangement. Empty means "auto": the chain carries no
        user-arranged row split and the page wraps it itself. */
    juce::var chainRowsVar() const;

    juce::var modelsVar() const;
    juce::var irsVar() const;
    juce::var presetListVar() const;
    juce::var abVar() const;
    juce::var modelVar() const;
    /** modelVar()'s twin for engine B — same ModelInfo shape, empty var when B has no
        model loaded. */
    juce::var modelVarB() const;
    juce::var irVar() const;
    juce::var t3kVar() const;
    juce::var fxSlotVar (int slot) const;
    juce::var fxSlotsVar() const;
    juce::var uiStateVar() const;

    /** Hosted parameters of a live slot, as {index,name,label,value,text}. Empty for
        an empty or non-live slot. */
    juce::var fxParamsVar (int slot) const;

    // --- events
    void emit (const juce::Identifier& eventId, const juce::var& payload);
    void emitChainChanged();
    void emitLibraryChanged();
    void emitPresetChanged();
    void emitModelChanged();
    /** emitModelChanged()'s twin for engine B, mirroring the same "poll and emit on
        change" reasoning — the processor has no B-side change notification either. */
    void emitModelBChanged();
    void emitIrChanged();
    void emitT3kStatus();
    void emitT3kError (const juce::String& message);
    /** Failure of a per-model download. The modelId lets the UI drop that model's
        progress row; the 1-arg overload is for errors with no model context. */
    void emitT3kError (const juce::String& message, juce::int64 modelId);
    void emitFxSlotChanged (int slot);

    /** Closes the native window showing a slot's plugin. Called before the processor
        retires that instance — an AudioProcessor must outlive its editor. */
    void closeFxWindow (int slot);

    /** The hosted parameter the given slot's control is currently dragging, or -1.
        Set from the bridge's gesture calls so the metered push does not fight a drag
        that is already in flight. */
    juce::AudioProcessorParameter* fxParamFor (int slot, int index) const;

    /** Balances any gesture left open on a hosted parameter. */
    void endOutstandingFxGesture();

    // --- embedding a hosted editor in our own window
    //
    // The hosted editor is a native view, so it is a SIBLING of the WebView rather than
    // anything inside the page, and it composites above it. The page reserves a
    // rectangle and reports it; we put the view there. Nothing the page draws can cover
    // it, which is why setEmbedVisible exists.

    /** Mounts slot's editor as a child of this editor, or unmounts when slot < 0.
        Returns an error when the plugin cannot fit the display. */
    juce::String setEmbedSlot (int slot);
    void unmountEmbed();
    void emitEmbedChanged (const juce::String& error = {});
    juce::var embedVar (const juce::String& error) const;

    /** Reacts to a hosted editor resizing itself after attach — several plugins only
        report a usable size once their view is in a window, and a few change it later. */
    void childBoundsChanged (juce::Component* child) override;

    juce::var editorSizeVar() const;

    /** Applies embedMinWindow to the resize limits, and grows the window once if it is
        currently smaller. Called on mount, on unmount, and whenever the page revises
        the requirement. */
    void applyEmbedSizeFloor();
    void emitEditorSizeChanged();

    /** Largest window that still fits the display we are on. Embedding is refused
        against this, not against an arbitrary constant. */
    juce::Rectangle<int> usableScreenArea() const;

    /** Nothing in the processor announces "the loaded model/IR changed" (the old
        editor polled at 4 Hz for exactly this reason), and a preset load or host
        state restore can swap either behind our back. Poll and emit on change. */
    void pollModelAndIr (bool forceEmit);

    TubampAudioProcessor& proc;

    // --- 1. relays: must outlive `web`, whose dtor calls back into them.
    std::vector<std::unique_ptr<juce::WebSliderRelay>>       sliderRelays;
    std::vector<std::unique_ptr<juce::WebToggleButtonRelay>> toggleRelays;
    std::vector<std::unique_ptr<juce::WebComboBoxRelay>>     comboRelays;
    juce::WebControlParameterIndexReceiver paramIndexReceiver;

    // --- 2. the browser: exactly one per editor, never re-goToURL'd.
    std::unique_ptr<SinglePageBrowser> web;

    // --- 3. attachments: declared last so they are destroyed first.
    std::vector<std::unique_ptr<juce::WebSliderParameterAttachment>>       sliderAtt;
    std::vector<std::unique_ptr<juce::WebToggleButtonParameterAttachment>> toggleAtt;
    std::vector<std::unique_ptr<juce::WebComboBoxParameterAttachment>>     comboAtt;

    std::unique_ptr<juce::FileChooser> fileChooser;
    bool chooserActive = false;

    /** toneId -> "is an IR tone". t3kListModels(toneId) carries no format (frozen
        bridge contract) but the client needs one to pick the architecture filter;
        every browse page we serialize feeds this, unknown ids mean nam. */
    std::unordered_map<juce::int64, bool> toneIsIr;

    juce::String lastModelPath, lastModelPathB, lastIrPath;
    int lastLatencySamples = -1;
    int pollDivider = 0;

    // --- external AudioUnit slots
    //
    // Declared after `web` so they are destroyed before it, and torn down explicitly
    // in the destructor anyway: a hosted editor must never outlive our editor, because
    // the processor is free to retire the instance behind it once we stop listening.
    std::array<std::unique_ptr<FxEditorWindow>, chain::numFxSlots> fxWindows;

    /** Slot whose parameter values are pushed on the timer, or -1. Only the slot the
        user is actually looking at is metered. */
    int watchedFxSlot = -1;

    /** Hosted parameter currently held by a UI gesture, as {slot, index}, or {-1,-1}.
        The metered push skips it so an in-flight drag is never fought by an echo. */
    int gestureSlot = -1, gestureParam = -1;

    int fxPollDivider = 0;

    /** The hosted editor currently drawn inside our window, and which slot it belongs
        to (-1 when none). Not owned via the slot's FxEditorWindow — a plugin has one
        active editor, so embedding and popping out are mutually exclusive. */
    std::unique_ptr<juce::AudioProcessorEditor> embedEditor;
    int embedSlot = -1;
    bool embedVisible = true;

    /** Where the page wants the hole, in its own CSS pixels. */
    juce::Rectangle<int> embedRect;

    /** Guards against re-entering the size push while we are the ones resizing. */
    bool inEmbedLayout = false;
    bool embedTooSmall = false;

    /** Smallest window that can still show the current embed, as reported by the page
        (plugin size + its chrome). Becomes the resize floor while embedded, which is
        what stops a drag from shrinking the window out from under the plugin. */
    juce::Point<int> embedMinWindow;
    juce::Rectangle<int> lastEmbedSize;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebEditor)
};
} // namespace tubamp
