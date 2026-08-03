#include "SettingsPanel.h"
#include "../PluginProcessor.h"
#include "TubampLookAndFeel.h"

namespace tubamp
{
SettingsPanel::SettingsPanel (TubampAudioProcessor& processor) : proc (processor)
{
    setLookAndFeel (&lookAndFeel);
    setSize (380, 320);

    titleLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    addAndMakeVisible (titleLabel);

    addAndMakeVisible (keyLabel);

    keyEditor.setText (proc.tone3000.getClientId(), juce::dontSendNotification);
    keyEditor.setTextToShowWhenEmpty ("t3k_pub_...", TubampLookAndFeel::textDim);
    addAndMakeVisible (keyEditor);

    addAndMakeVisible (applyButton);
    applyButton.onClick = [this]
    {
        proc.tone3000.setClientId (keyEditor.getText().trim());
        statusLabel.setText ("Saved.", juce::dontSendNotification);
    };

    addAndMakeVisible (signOutButton);
    signOutButton.onClick = [this]
    {
        proc.tone3000.signOut();
        statusLabel.setText ("Signed out.", juce::dontSendNotification);
    };

    statusLabel.setColour (juce::Label::textColourId, TubampLookAndFeel::textDim);
    addAndMakeVisible (statusLabel);

    calibrationTitleLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    addAndMakeVisible (calibrationTitleLabel);

    addAndMakeVisible (calibrateInputButton);

    calLevelLabel.setColour (juce::Label::textColourId, TubampLookAndFeel::textDim);
    addAndMakeVisible (calLevelLabel);
    calLevelSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 20);
    addAndMakeVisible (calLevelSlider);

    // "amp_cal_input" / "amp_cal_level" are contract-fixed ids (new params owned by
    // the DSP agent), used as raw string literals to avoid depending on their header.
    calibrateInputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        proc.apvts, "amp_cal_input", calibrateInputButton);
    calLevelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        proc.apvts, "amp_cal_level", calLevelSlider);
}

SettingsPanel::~SettingsPanel()
{
    setLookAndFeel (nullptr);
}

void SettingsPanel::paint (juce::Graphics& g)
{
    g.fillAll (TubampLookAndFeel::bg);
}

void SettingsPanel::resized()
{
    auto area = getLocalBounds().reduced (16);
    titleLabel.setBounds (area.removeFromTop (28));
    area.removeFromTop (12);
    keyLabel.setBounds (area.removeFromTop (18));
    keyEditor.setBounds (area.removeFromTop (28));
    area.removeFromTop (10);

    auto buttonRow = area.removeFromTop (32);
    applyButton.setBounds (buttonRow.removeFromLeft (100));
    buttonRow.removeFromLeft (8);
    signOutButton.setBounds (buttonRow.removeFromLeft (100));

    area.removeFromTop (8);
    statusLabel.setBounds (area.removeFromTop (18));

    area.removeFromTop (16);
    calibrationTitleLabel.setBounds (area.removeFromTop (28));
    area.removeFromTop (10);
    calibrateInputButton.setBounds (area.removeFromTop (24));
    area.removeFromTop (8);
    calLevelLabel.setBounds (area.removeFromTop (18));
    calLevelSlider.setBounds (area.removeFromTop (28));
}
} // namespace tubamp
