#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace tubamp
{
class TubampAudioProcessor;

/**
    Preset = APVTS parameter state + model file name + IR file name + metadata,
    stored as JSON in the library's presets/ directory.

    Also owns the A/B compare slots: captureToSlot/recallSlot snapshot the full
    plugin state (params + model + IR) in memory. Message-thread only.
*/
class PresetManager
{
public:
    explicit PresetManager (TubampAudioProcessor& processor);

    struct PresetInfo
    {
        juce::String name;
        juce::File file;
        bool favorite = false;
        juce::StringArray tags;
    };

    juce::Array<PresetInfo> getPresets() const;      // sorted, from presets dir
    juce::String getCurrentPresetName() const;

    /** Saves current plugin state under name (overwrites existing). */
    juce::Result savePreset (const juce::String& name);
    juce::Result loadPreset (const juce::File& presetFile);
    juce::Result deletePreset (const juce::File& presetFile);
    void setFavorite (const juce::File& presetFile, bool favorite);

    /** Creates a handful of factory presets on first run (no models bundled —
        they reference "no model" and sensible FX settings). */
    void createFactoryPresetsIfMissing();

    // A/B compare
    void captureToSlot (int slot);      // slot 0 = A, 1 = B
    void recallSlot (int slot);
    bool slotHasState (int slot) const;
    int  getActiveSlot() const;

    std::function<void()> onPresetChanged;

private:
    TubampAudioProcessor& proc;
    juce::String currentName { "Init" };
    juce::var slots[2];
    int activeSlot = 0;
};
} // namespace tubamp
