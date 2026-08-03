#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <optional>
#include <vector>

#include "../dsp/ChainOrder.h"
#include "IconButton.h"

namespace tubamp
{
class TubampAudioProcessor;

// Bodies with enough behaviour of their own to live in the .cpp. Both are created
// once and kept alive for the panel's lifetime (never destroyed on block switches),
// so an in-flight TONE3000 download always lands somewhere.
class AmpBody;
class CabBody;

/** One knob of a generic block body: parameter id + the label under it. */
struct KnobSpec
{
    juce::String paramId;
    juce::String label;
};

/**
    The big editor for whichever block is selected in the SignalChainView: an
    accent-tinted header (icon, name, underline, power pill, remove affordance) over a
    body that switches per block — a centred row of knobs for the simple blocks, the
    type combo for Mod, the IR row for Cab, and the full model-management layout for
    the NAM Amp.
*/
class BlockParamPanel : public juce::Component
{
public:
    explicit BlockParamPanel (TubampAudioProcessor&);
    ~BlockParamPanel() override;

    /** Shows the given block, or the empty state when nothing is selected. */
    void setBlock (std::optional<chain::BlockId> id);

    /** Fired by the header's remove affordance (the chain view owns the removal). */
    std::function<void (chain::BlockId)> onRemoveBlock;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    struct KnobUI
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<SliderAttachment> attachment;
    };

    static std::vector<KnobSpec> knobSpecsFor (chain::BlockId id);

    void buildKnobs (const std::vector<KnobSpec>& specs, juce::Colour accentColour);
    void layoutKnobRow (juce::Rectangle<int> area);

    TubampAudioProcessor& proc;
    std::optional<chain::BlockId> block;
    juce::Colour accentColour;
    juce::Path headerIcon;

    juce::Label titleLabel;
    juce::ToggleButton powerButton;
    std::unique_ptr<ButtonAttachment> powerAttachment;
    IconButton removeButton { "remove", {} };

    juce::OwnedArray<KnobUI> knobs;

    juce::ComboBox modTypeCombo;
    std::unique_ptr<ComboBoxAttachment> modTypeAttachment;

    std::unique_ptr<AmpBody> ampBody;
    std::unique_ptr<CabBody> cabBody;

    juce::Label emptyLabel { {}, "Add a block to get started" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BlockParamPanel)
};
} // namespace tubamp
