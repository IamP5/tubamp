#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "IconButton.h"

namespace tubamp
{
class TubampAudioProcessor;

/** Top bar: brand mark, the preset pill (prev / preset combo / next / save), the A|B
    compare pair (shift-click captures) and the settings gear, which opens the
    TONE3000 / calibration dialog. */
class HeaderBar : public juce::Component,
                  private juce::Timer
{
public:
    explicit HeaderBar (TubampAudioProcessor& processor);
    ~HeaderBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void refreshPresets();

private:
    void timerCallback() override;
    void savePreset();
    void selectPreset (int index);
    void stepPreset (int delta);
    void updateAbButtons();
    void openSettings();

    TubampAudioProcessor& proc;

    juce::ComboBox presetCombo;
    IconButton prevButton { "prevPreset", {} }, nextButton { "nextPreset", {} },
               saveButton { "savePreset", {} };
    juce::TextButton slotAButton { "A" }, slotBButton { "B" };
    IconButton settingsButton { "settings", {} };

    juce::Rectangle<int> presetPillBounds;
    bool updatingCombo = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderBar)
};
} // namespace tubamp
