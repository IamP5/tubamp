#include "PluginEditor.h"

#include "Parameters.h"

namespace tubamp
{
namespace
{
constexpr int headerHeight = 56;
constexpr int chainHeight = 118;
constexpr int footerHeight = 64;
} // namespace

TubampAudioProcessorEditor::TubampAudioProcessorEditor (TubampAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p),
      header (p),
      chainView (p),
      paramPanel (p)
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (header);
    addAndMakeVisible (chainView);
    addAndMakeVisible (paramPanel);

    chainView.onSelectionChanged = [this] (std::optional<chain::BlockId> id)
    {
        paramPanel.setBlock (id);
    };

    paramPanel.onRemoveBlock = [this] (chain::BlockId id)
    {
        chainView.removeBlock (id);
    };

    // The chain view resolved its own initial selection in its constructor.
    paramPanel.setBlock (chainView.getSelection());

    for (auto* knob : { &inputTrimKnob, &outputLevelKnob })
    {
        TubampLookAndFeel::styleKnob (*knob, TubampLookAndFeel::accent);
        knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob->setPopupDisplayEnabled (true, true, this);
        addAndMakeVisible (*knob);
    }

    for (auto* label : { &inputTrimLabel, &outputLevelLabel })
    {
        label->setJustificationType (juce::Justification::centred);
        label->setFont (TubampLookAndFeel::kernedFont (10.0f, true));
        label->setColour (juce::Label::textColourId, TubampLookAndFeel::textDim);
        addAndMakeVisible (*label);
    }

    inputTrimAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        p.apvts, params::inputTrim, inputTrimKnob);
    outputLevelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        p.apvts, params::outputLevel, outputLevelKnob);

    addAndMakeVisible (inputMeter);
    addAndMakeVisible (outputMeter);

    infoLabel.setFont (TubampLookAndFeel::plainFont (11.0f));
    infoLabel.setColour (juce::Label::textColourId, TubampLookAndFeel::textFaint);
    infoLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (infoLabel);

    setSize (1120, 700);
    startTimerHz (30);
}

TubampAudioProcessorEditor::~TubampAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void TubampAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (TubampLookAndFeel::bg);

    // Barely-there lightening towards the top centre: depth, not decoration.
    juce::ColourGradient glow (TubampLookAndFeel::bg.brighter (0.08f),
                               (float) getWidth() * 0.5f, -40.0f,
                               TubampLookAndFeel::bg,
                               (float) getWidth() * 0.5f, (float) getHeight() * 0.85f,
                               true);
    g.setGradientFill (glow);
    g.fillRect (getLocalBounds());

    // Footer plate.
    g.setColour (TubampLookAndFeel::bgElevated);
    g.fillRect (footerBounds);
    g.setColour (TubampLookAndFeel::stroke);
    g.fillRect (footerBounds.withHeight (1));
}

void TubampAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    header.setBounds (area.removeFromTop (headerHeight));
    chainView.setBounds (area.removeFromTop (chainHeight));

    footerBounds = area.removeFromBottom (footerHeight);
    paramPanel.setBounds (area.reduced (16, 14));

    auto footer = footerBounds.reduced (20, 4);

    auto inputSlot = footer.removeFromLeft (58);
    inputTrimLabel.setBounds (inputSlot.removeFromBottom (12));
    inputTrimKnob.setBounds (inputSlot.withSizeKeepingCentre (44, 44));
    footer.removeFromLeft (10);
    inputMeter.setBounds (footer.removeFromLeft (10).reduced (0, 6));

    auto outputSlot = footer.removeFromRight (58);
    outputLevelLabel.setBounds (outputSlot.removeFromBottom (12));
    outputLevelKnob.setBounds (outputSlot.withSizeKeepingCentre (44, 44));
    footer.removeFromRight (10);
    outputMeter.setBounds (footer.removeFromRight (10).reduced (0, 6));

    infoLabel.setBounds (footer.reduced (16, 0));
}

void TubampAudioProcessorEditor::timerCallback()
{
    inputMeter.pushLevel (processorRef.inputPeak.exchange (0.0f));
    outputMeter.pushLevel (processorRef.outputPeak.exchange (0.0f));

    juce::String info;
    auto latency = processorRef.getLatencySamples();
    auto modelInfo = processorRef.namEngine.getModelInfo();

    if (processorRef.namEngine.hasModel())
        info << "Model SR " << (int) modelInfo.sampleRate << " Hz"
             << juce::String (juce::CharPointer_UTF8 ("   \xc2\xb7   "));

    info << "Latency " << latency << " smp";
    infoLabel.setText (info, juce::dontSendNotification);
}
} // namespace tubamp
