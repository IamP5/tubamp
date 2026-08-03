#include "HeaderBar.h"

#include "../PluginProcessor.h"
#include "BlockIcons.h"
#include "SettingsPanel.h"
#include "TubampLookAndFeel.h"

namespace tubamp
{
namespace
{
constexpr int pillWidth = 380;
constexpr int pillHeight = 30;
} // namespace

HeaderBar::HeaderBar (TubampAudioProcessor& processor) : proc (processor)
{
    TubampLookAndFeel::styleCombo (presetCombo, true);
    presetCombo.setTooltip ("Preset");
    addAndMakeVisible (presetCombo);
    presetCombo.onChange = [this] { selectPreset (presetCombo.getSelectedId() - 1); };

    prevButton.setGlyph (icons::makeChevronGlyph (false));
    prevButton.setTooltip ("Previous preset");
    prevButton.onClick = [this] { stepPreset (-1); };

    nextButton.setGlyph (icons::makeChevronGlyph (true));
    nextButton.setTooltip ("Next preset");
    nextButton.onClick = [this] { stepPreset (1); };

    saveButton.setGlyph (icons::makeSaveGlyph());
    saveButton.setTooltip ("Save preset");
    saveButton.setColours (TubampLookAndFeel::textDim, TubampLookAndFeel::accent);
    saveButton.onClick = [this] { savePreset(); };

    for (auto* button : { &prevButton, &nextButton, &saveButton })
        addAndMakeVisible (*button);

    for (auto* button : { &slotAButton, &slotBButton })
    {
        button->setClickingTogglesState (false);
        button->setColour (juce::TextButton::buttonOnColourId, TubampLookAndFeel::accent);
        addAndMakeVisible (*button);
    }

    slotAButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    slotBButton.setConnectedEdges (juce::Button::ConnectedOnLeft);

    slotAButton.onClick = [this]
    {
        if (juce::ModifierKeys::currentModifiers.isShiftDown())
            proc.presets.captureToSlot (0);
        else
            proc.presets.recallSlot (0);

        updateAbButtons();
    };
    slotBButton.onClick = [this]
    {
        if (juce::ModifierKeys::currentModifiers.isShiftDown())
            proc.presets.captureToSlot (1);
        else
            proc.presets.recallSlot (1);

        updateAbButtons();
    };
    slotAButton.setTooltip ("Recall A - shift-click to capture");
    slotBButton.setTooltip ("Recall B - shift-click to capture");

    settingsButton.setGlyph (icons::makeGearGlyph());
    settingsButton.setTooltip ("Settings");
    settingsButton.setInset (0.22f);
    settingsButton.onClick = [this] { openSettings(); };
    addAndMakeVisible (settingsButton);

    proc.presets.onPresetChanged = [this] { refreshPresets(); };
    refreshPresets();
    updateAbButtons();
    startTimerHz (2);
}

HeaderBar::~HeaderBar()
{
    proc.presets.onPresetChanged = nullptr;
}

void HeaderBar::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    g.setGradientFill (juce::ColourGradient (TubampLookAndFeel::panel, 0.0f, 0.0f,
                                             TubampLookAndFeel::bgElevated, 0.0f, (float) getHeight(),
                                             false));
    g.fillRect (bounds);
    g.setColour (TubampLookAndFeel::stroke);
    g.fillRect (bounds.removeFromBottom (1));

    // Brand mark: accent dot + wordmark.
    const auto dot = juce::Rectangle<float> (8.0f, 8.0f)
                         .withCentre ({ 24.0f, (float) getHeight() * 0.5f });
    g.setColour (TubampLookAndFeel::accent.withAlpha (0.30f));
    g.fillEllipse (dot.expanded (3.5f));
    g.setColour (TubampLookAndFeel::accent);
    g.fillEllipse (dot);

    g.setColour (TubampLookAndFeel::text);
    g.setFont (TubampLookAndFeel::plainFont (18.0f, true).withExtraKerningFactor (0.02f));
    g.drawText ("tubamp", juce::Rectangle<int> (38, 0, 120, getHeight()),
                juce::Justification::centredLeft, false);

    // Preset pill.
    const auto pill = presetPillBounds.toFloat();
    g.setColour (TubampLookAndFeel::panel);
    g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
    g.setColour (TubampLookAndFeel::stroke);
    g.drawRoundedRectangle (pill.reduced (0.5f), pill.getHeight() * 0.5f, 1.0f);
}

void HeaderBar::resized()
{
    auto area = getLocalBounds().reduced (20, 0);

    settingsButton.setBounds (area.removeFromRight (32).withSizeKeepingCentre (30, 30));
    area.removeFromRight (16);
    slotBButton.setBounds (area.removeFromRight (36).withSizeKeepingCentre (36, 28));
    slotAButton.setBounds (area.removeFromRight (36).withSizeKeepingCentre (36, 28));

    // The pill starts after the wordmark and never runs into the A|B pair.
    constexpr int pillX = 170;
    const auto available = juce::jmax (160, slotAButton.getX() - 24 - pillX);
    presetPillBounds = { pillX, (getHeight() - pillHeight) / 2,
                         juce::jmin (pillWidth, available), pillHeight };

    auto pill = presetPillBounds.reduced (4, 3);
    prevButton.setBounds (pill.removeFromLeft (24));
    saveButton.setBounds (pill.removeFromRight (24));
    nextButton.setBounds (pill.removeFromRight (24));
    presetCombo.setBounds (pill.reduced (4, 0));
}

void HeaderBar::timerCallback()
{
    updateAbButtons();
}

void HeaderBar::refreshPresets()
{
    updatingCombo = true;
    presetCombo.clear (juce::dontSendNotification);

    auto presets = proc.presets.getPresets();
    int selectedId = 0;
    auto currentName = proc.presets.getCurrentPresetName();

    for (int i = 0; i < presets.size(); ++i)
    {
        auto id = i + 1;
        presetCombo.addItem (presets[i].name, id);

        if (presets[i].name == currentName)
            selectedId = id;
    }

    presetCombo.setSelectedId (selectedId, juce::dontSendNotification);

    if (selectedId == 0)
        presetCombo.setTextWhenNothingSelected (currentName);

    updatingCombo = false;
}

void HeaderBar::selectPreset (int index)
{
    if (updatingCombo)
        return;

    auto presets = proc.presets.getPresets();

    if (juce::isPositiveAndBelow (index, presets.size()))
        proc.presets.loadPreset (presets[index].file);
}

void HeaderBar::stepPreset (int delta)
{
    auto presets = proc.presets.getPresets();

    if (presets.isEmpty())
        return;

    auto currentName = proc.presets.getCurrentPresetName();
    int currentIndex = 0;

    for (int i = 0; i < presets.size(); ++i)
        if (presets[i].name == currentName)
            currentIndex = i;

    auto next = (currentIndex + delta + presets.size()) % presets.size();
    proc.presets.loadPreset (presets[next].file);
}

void HeaderBar::updateAbButtons()
{
    auto active = proc.presets.getActiveSlot();
    slotAButton.setToggleState (active == 0, juce::dontSendNotification);
    slotBButton.setToggleState (active == 1, juce::dontSendNotification);
}

void HeaderBar::savePreset()
{
    auto* aw = new juce::AlertWindow ("Save preset", "Enter a name for this preset:",
                                      juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("name", proc.presets.getCurrentPresetName());
    aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    // The AlertWindow is a free-standing desktop window that outlives this editor if
    // the host closes it while the dialog is up — never dereference a raw `this` here.
    juce::Component::SafePointer<HeaderBar> safeThis (this);

    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, aw] (int result)
    {
        std::unique_ptr<juce::AlertWindow> owned (aw);

        if (safeThis == nullptr || result != 1)
            return;

        auto name = aw->getTextEditorContents ("name").trim();

        if (name.isNotEmpty())
            safeThis->proc.presets.savePreset (name);
    }), false);
}

void HeaderBar::openSettings()
{
    auto* content = new SettingsPanel (proc);

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Settings";
    options.dialogBackgroundColour = TubampLookAndFeel::bg;
    options.content.setOwned (content);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}
} // namespace tubamp
