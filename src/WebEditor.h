#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "PluginProcessor.h"

#include <memory>
#include <optional>
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

/**
    Fixed 1120x700 WebView editor.

    Owns:
      - one relay per frozen APVTS parameter (29 sliders / 10 toggles / 2 combos),
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
    juce::var modelsVar() const;
    juce::var irsVar() const;
    juce::var presetListVar() const;
    juce::var abVar() const;
    juce::var modelVar() const;
    juce::var irVar() const;
    juce::var t3kVar() const;
    juce::var uiStateVar() const;

    // --- events
    void emit (const juce::Identifier& eventId, const juce::var& payload);
    void emitChainChanged();
    void emitLibraryChanged();
    void emitPresetChanged();
    void emitModelChanged();
    void emitIrChanged();
    void emitT3kStatus();
    void emitT3kError (const juce::String& message);
    /** Failure of a per-model download. The modelId lets the UI drop that model's
        progress row; the 1-arg overload is for errors with no model context. */
    void emitT3kError (const juce::String& message, juce::int64 modelId);

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

    juce::String lastModelPath, lastIrPath;
    int lastLatencySamples = -1;
    int pollDivider = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebEditor)
};
} // namespace tubamp
