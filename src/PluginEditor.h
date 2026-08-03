#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "ui/BlockParamPanel.h"
#include "ui/HeaderBar.h"
#include "ui/PeakMeter.h"
#include "ui/SignalChainView.h"
#include "ui/TubampLookAndFeel.h"

namespace tubamp
{
/** Fixed-size (1120x700) dark editor:

      HeaderBar          brand, preset pill, A|B, settings
      SignalChainView    IN -[blocks]- [+] - OUT, drag to reorder
      BlockParamPanel    the selected block, full width
      footer             input trim + meter, model/latency info, meter + output level
*/
class TubampAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::Timer
{
public:
    explicit TubampAudioProcessorEditor (TubampAudioProcessor&);
    ~TubampAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    TubampAudioProcessor& processorRef;
    TubampLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { this, 700 };

    HeaderBar header;
    SignalChainView chainView;
    BlockParamPanel paramPanel;

    // Footer
    juce::Slider inputTrimKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider outputLevelKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label inputTrimLabel { {}, "IN TRIM" }, outputLevelLabel { {}, "OUT LEVEL" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> inputTrimAttachment, outputLevelAttachment;
    PeakMeter inputMeter, outputMeter;
    juce::Label infoLabel;

    juce::Rectangle<int> footerBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TubampAudioProcessorEditor)
};
} // namespace tubamp
