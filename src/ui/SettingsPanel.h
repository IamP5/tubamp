#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "TubampLookAndFeel.h"

namespace tubamp
{
class TubampAudioProcessor;

/** Content for the settings DialogWindow: TONE3000 publishable key entry + sign out.
    The dialog is a top-level window (not a child of the editor), so it carries its own
    look-and-feel instance — that keeps the styling alive exactly as long as the dialog
    and never outlives it. */
class SettingsPanel : public juce::Component
{
public:
    explicit SettingsPanel (TubampAudioProcessor& processor);
    ~SettingsPanel() override;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    TubampAudioProcessor& proc;
    TubampLookAndFeel lookAndFeel;

    juce::Label titleLabel { {}, "TONE3000" };
    juce::Label keyLabel { {}, "Publishable key" };
    juce::TextEditor keyEditor;
    juce::TextButton applyButton { "Save" };
    juce::TextButton signOutButton { "Sign out" };
    juce::Label statusLabel;

    juce::Label calibrationTitleLabel { {}, "Amp Calibration" };
    juce::ToggleButton calibrateInputButton { "Calibrate input" };
    juce::Label calLevelLabel { {}, "Input level (dBu)" };
    juce::Slider calLevelSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> calibrateInputAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> calLevelAttachment;
};
} // namespace tubamp
