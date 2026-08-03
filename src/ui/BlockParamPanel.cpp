#include "BlockParamPanel.h"

#include "../Parameters.h"
#include "../PluginProcessor.h"
#include "BlockIcons.h"
#include "TubampLookAndFeel.h"

namespace tubamp
{
namespace
{
constexpr int headerH = 56;
constexpr int knobSize = 72;
constexpr int knobTextH = 18;
constexpr int knobLabelH = 14;
constexpr int knobCellW = 100;
constexpr int knobCellH = knobSize + knobTextH + 4 + knobLabelH;

void styleMicroLabel (juce::Label& label, juce::Colour colour = TubampLookAndFeel::textDim)
{
    label.setJustificationType (juce::Justification::centred);
    label.setFont (TubampLookAndFeel::kernedFont (11.0f, true));
    label.setColour (juce::Label::textColourId, colour);
}

/** Small uppercase caption painted directly (no Label needed). */
void drawCaption (juce::Graphics& g, const juce::String& caption, juce::Rectangle<int> area)
{
    g.setColour (TubampLookAndFeel::textFaint);
    g.setFont (TubampLookAndFeel::kernedFont (10.0f, true));
    g.drawText (caption, area, juce::Justification::centredLeft, false);
}
} // namespace

//==============================================================================
/** Everything the old AmpPanel did — model name + live engine status, library combo
    with prev/next/clear, manual import, the TONE3000 browse flow (SafePointer-guarded
    async callbacks + download progress), IN/OUT knobs, output mode and the A2 slim
    slider — relaid out for the wide panel: model management left, controls right. */
class AmpBody : public juce::Component,
                private juce::Timer
{
public:
    explicit AmpBody (TubampAudioProcessor& processor) : proc (processor)
    {
        modelNameLabel.setFont (TubampLookAndFeel::plainFont (18.0f, true));
        modelNameLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (modelNameLabel);

        TubampLookAndFeel::styleCombo (modelCombo);
        modelCombo.setTextWhenNothingSelected ("Select a model...");
        addAndMakeVisible (modelCombo);
        modelCombo.onChange = [this] { modelComboChanged(); };

        modelPrevButton.setGlyph (icons::makeChevronGlyph (false));
        modelPrevButton.setTooltip ("Previous model");
        modelPrevButton.onClick = [this] { stepModel (-1); };

        modelNextButton.setGlyph (icons::makeChevronGlyph (true));
        modelNextButton.setTooltip ("Next model");
        modelNextButton.onClick = [this] { stepModel (1); };

        modelClearButton.setGlyph (makeCrossGlyph());
        modelClearButton.setTooltip ("Clear the loaded model");
        modelClearButton.onClick = [this] { clearModelClicked(); };

        for (auto* button : { &modelPrevButton, &modelNextButton, &modelClearButton })
        {
            button->setHasPlate (true);
            addAndMakeVisible (*button);
        }

        addAndMakeVisible (importButton);
        importButton.onClick = [this] { importClicked(); };

        browseButton.setColour (juce::TextButton::buttonOnColourId, TubampLookAndFeel::accent);
        addAndMakeVisible (browseButton);
        browseButton.onClick = [this] { browseClicked(); };

        poweredByLabel.setFont (TubampLookAndFeel::plainFont (11.0f));
        poweredByLabel.setColour (juce::Label::textColourId, TubampLookAndFeel::textFaint);
        poweredByLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (poweredByLabel);

        hintLabel.setFont (TubampLookAndFeel::plainFont (11.0f));
        hintLabel.setColour (juce::Label::textColourId, TubampLookAndFeel::textDim);
        hintLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (hintLabel);

        progressBar.setVisible (false);
        addAndMakeVisible (progressBar);

        const auto ampAccent = TubampLookAndFeel::blockAccent (chain::BlockId::amp);

        for (auto* knob : { &inputKnob, &outputKnob })
        {
            TubampLookAndFeel::styleKnob (*knob, ampAccent);
            addAndMakeVisible (*knob);
        }

        styleMicroLabel (inputLabel);
        styleMicroLabel (outputLabel);
        styleMicroLabel (modeLabel);
        styleMicroLabel (slimLabel);
        modeLabel.setJustificationType (juce::Justification::centredLeft);
        slimLabel.setJustificationType (juce::Justification::centredLeft);

        for (auto* label : { &inputLabel, &outputLabel, &modeLabel, &slimLabel })
            addAndMakeVisible (*label);

        // "amp_out_mode" is a contract-fixed id (choice param owned by the DSP agent);
        // items must map to choice indices 0..2 exactly (Raw/Normalized/Calibrated).
        modeCombo.addItemList (params::ampOutModeChoices, 1);
        TubampLookAndFeel::styleCombo (modeCombo);
        addAndMakeVisible (modeCombo);

        slimSlider.setColour (juce::Slider::trackColourId, ampAccent);
        addAndMakeVisible (slimSlider);

        inputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            proc.apvts, params::ampInput, inputKnob);
        outputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            proc.apvts, params::ampOutput, outputKnob);
        // "amp_out_mode" / "amp_slim" are contract-fixed ids (params owned by the DSP agent).
        modeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            proc.apvts, params::ampOutMode, modeCombo);
        slimAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            proc.apvts, params::ampSlim, slimSlider);

        refreshModelList();
        startTimerHz (4);
    }

    void refreshModelList()
    {
        updatingCombo = true;
        modelCombo.clear (juce::dontSendNotification);

        auto models = proc.library.getModels();
        int selectedId = 0;
        auto loadedPath = proc.getLoadedModelPath();

        for (int i = 0; i < models.size(); ++i)
        {
            auto id = i + 1;
            modelCombo.addItem (models[i].displayName, id);

            if (models[i].file.getFullPathName() == loadedPath)
                selectedId = id;
        }

        modelCombo.setTextWhenNothingSelected ("Select a model...");
        modelCombo.setSelectedId (selectedId, juce::dontSendNotification);
        updatingCombo = false;
    }

    void paint (juce::Graphics& g) override
    {
        drawCaption (g, "MODEL", modelCaptionBounds);
        drawCaption (g, "LEVELS", levelCaptionBounds);

        g.setColour (TubampLookAndFeel::stroke);
        g.fillRect (dividerBounds);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto right = area.removeFromRight (360);
        dividerBounds = { right.getX() - 24, area.getY() + 8, 1, area.getHeight() - 16 };

        // --- left: model management ------------------------------------------
        auto left = area.withTrimmedRight (48);
        const int contentH = 200;
        left = left.withHeight (juce::jmin (contentH, left.getHeight()))
                   .withY (left.getCentreY() - juce::jmin (contentH, left.getHeight()) / 2);

        modelCaptionBounds = left.removeFromTop (14);
        left.removeFromTop (2);
        modelNameLabel.setBounds (left.removeFromTop (26));
        left.removeFromTop (12);

        auto comboRow = left.removeFromTop (32);
        modelClearButton.setBounds (comboRow.removeFromRight (32));
        comboRow.removeFromRight (4);
        modelNextButton.setBounds (comboRow.removeFromRight (32));
        comboRow.removeFromRight (4);
        modelPrevButton.setBounds (comboRow.removeFromRight (32));
        comboRow.removeFromRight (10);
        modelCombo.setBounds (comboRow);

        left.removeFromTop (12);

        auto buttonRow = left.removeFromTop (32);
        importButton.setBounds (buttonRow.removeFromLeft (104));
        buttonRow.removeFromLeft (8);
        browseButton.setBounds (buttonRow.removeFromLeft (168));
        buttonRow.removeFromLeft (12);
        poweredByLabel.setBounds (buttonRow.removeFromLeft (150));

        left.removeFromTop (12);
        progressBar.setBounds (left.removeFromTop (8));
        left.removeFromTop (6);
        hintLabel.setBounds (left.removeFromTop (18));

        // --- right: levels ----------------------------------------------------
        const int rightContentH = 220;   // caption + knob row + mode row + slim row
        right = right.withHeight (juce::jmin (rightContentH, right.getHeight()))
                     .withY (right.getCentreY() - juce::jmin (rightContentH, right.getHeight()) / 2);

        levelCaptionBounds = right.removeFromTop (14);
        right.removeFromTop (6);

        auto knobRow = right.removeFromTop (knobSize + knobTextH + 4 + knobLabelH);
        auto inSlot = knobRow.removeFromLeft (knobRow.getWidth() / 2);
        auto outSlot = knobRow;

        auto layoutKnob = [] (juce::Rectangle<int> slot, juce::Slider& knob, juce::Label& label)
        {
            auto cell = slot.withSizeKeepingCentre (knobCellW, slot.getHeight());
            label.setBounds (cell.removeFromBottom (knobLabelH));
            cell.removeFromBottom (4);
            knob.setBounds (cell);
        };

        layoutKnob (inSlot, inputKnob, inputLabel);
        layoutKnob (outSlot, outputKnob, outputLabel);

        right.removeFromTop (10);

        auto modeRow = right.removeFromTop (30);
        modeLabel.setBounds (modeRow.removeFromLeft (46));
        modeRow.removeFromLeft (6);
        modeCombo.setBounds (modeRow);

        right.removeFromTop (10);

        auto slimRow = right.removeFromTop (24);
        slimLabel.setBounds (slimRow.removeFromLeft (46));
        slimRow.removeFromLeft (6);
        slimSlider.setBounds (slimRow);
    }

private:
    static juce::Path makeCrossGlyph()
    {
        juce::Path p;
        icons::addLine (p, 0.28f, 0.28f, 0.72f, 0.72f);
        icons::addLine (p, 0.72f, 0.28f, 0.28f, 0.72f);
        return p;
    }

    void timerCallback() override
    {
        auto loaded = proc.getLoadedModelPath();
        juce::String display;

        if (loaded.isNotEmpty())
        {
            // Report the ENGINE's live state, not just the requested path, so a model
            // that failed to reach the audio thread is immediately visible.
            const auto info = proc.namEngine.getModelInfo();
            // Non-ASCII literals must go through CharPointer_UTF8, or the middot
            // renders as mojibake ("Â·").
            const juce::String dot (juce::CharPointer_UTF8 ("  \xc2\xb7  "));
            display = juce::File (loaded).getFileNameWithoutExtension()
                      + (proc.namEngine.hasModel()
                             ? dot + juce::String (info.sampleRate / 1000.0, 0) + " kHz" + dot + "active"
                             : dot + "NOT RUNNING");
        }
        else
        {
            display = "No model - load a NAM capture";
        }

        if (modelNameLabel.getText() != display)
        {
            modelNameLabel.setText (display, juce::dontSendNotification);
            modelNameLabel.setColour (juce::Label::textColourId,
                                      loaded.isNotEmpty() ? TubampLookAndFeel::text
                                                          : TubampLookAndFeel::textDim);
        }

        // A preset/state load can swap the model behind our back: keep the combo in sync.
        if (loaded != lastLoadedPath)
        {
            lastLoadedPath = loaded;
            refreshModelList();
        }

        auto configured = proc.tone3000.isConfigured();
        hintLabel.setVisible (! configured && ! downloading);

        if (! configured)
            hintLabel.setText ("Add your TONE3000 publishable key in Settings to browse.",
                               juce::dontSendNotification);

        progressBar.setVisible (downloading);

        // proc.namEngine.isSlimmable() is contract-fixed (DSP-agent-owned API): true only
        // when the loaded model implements the A2 "slimmable" interface.
        auto slimmable = proc.namEngine.isSlimmable();
        slimSlider.setVisible (slimmable);
        slimLabel.setVisible (slimmable);
        slimSlider.setEnabled (slimmable);
    }

    void modelComboChanged()
    {
        if (updatingCombo)
            return;

        auto models = proc.library.getModels();
        auto index = modelCombo.getSelectedId() - 1;

        if (juce::isPositiveAndBelow (index, models.size()))
        {
            if (auto error = proc.loadModel (models[index].file); error.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                        "Model load failed", error);
        }
    }

    void stepModel (int delta)
    {
        auto models = proc.library.getModels();

        if (models.isEmpty())
            return;

        auto loadedPath = proc.getLoadedModelPath();
        int currentIndex = -1;

        for (int i = 0; i < models.size(); ++i)
            if (models[i].file.getFullPathName() == loadedPath)
                currentIndex = i;

        int nextIndex = currentIndex < 0 ? (delta > 0 ? 0 : models.size() - 1)
                                         : (currentIndex + delta + models.size()) % models.size();

        if (auto error = proc.loadModel (models[nextIndex].file); error.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                    "Model load failed", error);
        refreshModelList();
    }

    void clearModelClicked()
    {
        proc.clearModel();
        refreshModelList();
    }

    void importClicked()
    {
        fileChooser = std::make_unique<juce::FileChooser> ("Import NAM model", juce::File(), "*.nam");
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                auto file = fc.getResult();

                if (! file.existsAsFile())
                    return;

                juce::String error;
                auto installed = proc.library.importModel (file, error);

                if (! installed.existsAsFile())
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            "Import failed", error);
                    return;
                }

                if (auto loadError = proc.loadModel (installed); loadError.isNotEmpty())
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            "Model load failed", loadError);
            });
    }

    void browseClicked()
    {
        if (! proc.tone3000.isConfigured())
        {
            hintLabel.setText ("Add your TONE3000 publishable key in Settings to browse.",
                               juce::dontSendNotification);
            hintLabel.setVisible (true);
            return;
        }

        // Tone3000Client runs these on a background thread and delivers results via
        // MessageManager::callAsync at an arbitrary later time, well after this panel
        // (owned by the editor) may have been destroyed. Guard every access with a
        // SafePointer instead of capturing `this` directly.
        juce::Component::SafePointer<AmpBody> safeThis (this);

        proc.tone3000.startSelectFlow (
            [safeThis] (Tone3000Client::ToneModels toneModels)
            {
                if (safeThis == nullptr)
                    return;

                if (toneModels.models.isEmpty())
                    return;

                juce::PopupMenu menu;
                menu.setLookAndFeel (&safeThis->getLookAndFeel());

                for (int i = 0; i < toneModels.models.size(); ++i)
                    menu.addItem (i + 1, toneModels.models[i].name);

                menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (safeThis->browseButton),
                    [safeThis, toneModels] (int result)
                    {
                        if (safeThis == nullptr)
                            return;

                        if (result <= 0 || result > toneModels.models.size())
                            return;

                        auto model = toneModels.models[result - 1];
                        safeThis->downloading = true;
                        safeThis->progress = 0.0;
                        safeThis->progressBar.setVisible (true);

                        safeThis->proc.tone3000.downloadModel (model, ModelLibrary::getModelsDir(),
                            [safeThis] (float p) { if (safeThis != nullptr) safeThis->progress = (double) p; },
                            [safeThis] (juce::File savedFile)
                            {
                                if (safeThis == nullptr)
                                    return;

                                safeThis->downloading = false;
                                safeThis->progressBar.setVisible (false);

                                if (savedFile.existsAsFile())
                                {
                                    safeThis->refreshModelList();
                                    safeThis->proc.loadModel (savedFile);
                                    safeThis->refreshModelList();
                                }
                            },
                            [safeThis] (juce::String errorMessage)
                            {
                                if (safeThis == nullptr)
                                    return;

                                safeThis->downloading = false;
                                safeThis->progressBar.setVisible (false);
                                safeThis->hintLabel.setText (errorMessage, juce::dontSendNotification);
                                safeThis->hintLabel.setVisible (true);
                            });
                    });
            },
            [safeThis] (juce::String errorMessage)
            {
                if (safeThis == nullptr)
                    return;

                safeThis->hintLabel.setText (errorMessage, juce::dontSendNotification);
                safeThis->hintLabel.setVisible (true);
            });
    }

    TubampAudioProcessor& proc;

    juce::Label modelNameLabel;
    juce::ComboBox modelCombo;
    IconButton modelPrevButton { "modelPrev", {} }, modelNextButton { "modelNext", {} },
               modelClearButton { "modelClear", {} };
    juce::TextButton importButton { "Import..." };
    juce::TextButton browseButton { "Browse TONE3000" };
    juce::Label poweredByLabel { {}, "Powered by TONE3000" };
    juce::Label hintLabel;
    double progress = 0.0;
    juce::ProgressBar progressBar { progress };
    bool downloading = false;

    juce::Slider inputKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider outputKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label inputLabel { {}, "IN" }, outputLabel { {}, "OUT" };
    juce::Label modeLabel { {}, "MODE" };
    juce::ComboBox modeCombo;

    juce::Slider slimSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Label slimLabel { {}, "SLIM" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> inputAttachment, outputAttachment, slimAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment;

    std::unique_ptr<juce::FileChooser> fileChooser;
    bool updatingCombo = false;
    juce::String lastLoadedPath;

    juce::Rectangle<int> modelCaptionBounds, levelCaptionBounds, dividerBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpBody)
};

//==============================================================================
/** The Cab block's IR row (combo + prev/next/clear/import). The LO CUT / HI CUT knobs
    come from the panel's generic knob row. */
class CabBody : public juce::Component
{
public:
    explicit CabBody (TubampAudioProcessor& processor) : proc (processor)
    {
        TubampLookAndFeel::styleCombo (irCombo);
        irCombo.setTextWhenNothingSelected ("No IR loaded");
        addAndMakeVisible (irCombo);
        irCombo.onChange = [this] { irComboChanged(); };

        irPrevButton.setGlyph (icons::makeChevronGlyph (false));
        irPrevButton.setTooltip ("Previous IR");
        irPrevButton.onClick = [this] { stepIr (-1); };

        irNextButton.setGlyph (icons::makeChevronGlyph (true));
        irNextButton.setTooltip ("Next IR");
        irNextButton.onClick = [this] { stepIr (1); };

        irClearButton.setGlyph (makeCrossGlyph());
        irClearButton.setTooltip ("Clear the loaded IR");
        irClearButton.onClick = [this] { clearIrClicked(); };

        for (auto* button : { &irPrevButton, &irNextButton, &irClearButton })
        {
            button->setHasPlate (true);
            addAndMakeVisible (*button);
        }

        importButton.setTooltip ("Import an impulse response (.wav)");
        addAndMakeVisible (importButton);
        importButton.onClick = [this] { importClicked(); };

        refreshIrList();
    }

    void refreshIrList()
    {
        updatingCombo = true;
        irCombo.clear (juce::dontSendNotification);

        auto irs = proc.library.getIrs();
        int selectedId = 0;
        auto loadedPath = proc.getLoadedIrPath();

        for (int i = 0; i < irs.size(); ++i)
        {
            auto id = i + 1;
            irCombo.addItem (irs[i].displayName, id);

            if (irs[i].file.getFullPathName() == loadedPath)
                selectedId = id;
        }

        irCombo.setTextWhenNothingSelected ("No IR loaded");
        irCombo.setSelectedId (selectedId, juce::dontSendNotification);
        updatingCombo = false;
    }

    void paint (juce::Graphics& g) override
    {
        drawCaption (g, "IMPULSE RESPONSE", captionBounds);
    }

    void resized() override
    {
        auto area = getLocalBounds().withSizeKeepingCentre (juce::jmin (620, getWidth()), getHeight());
        captionBounds = area.removeFromTop (14);
        area.removeFromTop (4);

        auto row = area.removeFromTop (32);
        importButton.setBounds (row.removeFromRight (100));
        row.removeFromRight (10);
        irClearButton.setBounds (row.removeFromRight (32));
        row.removeFromRight (4);
        irNextButton.setBounds (row.removeFromRight (32));
        row.removeFromRight (4);
        irPrevButton.setBounds (row.removeFromRight (32));
        row.removeFromRight (10);
        irCombo.setBounds (row);
    }

private:
    static juce::Path makeCrossGlyph()
    {
        juce::Path p;
        icons::addLine (p, 0.28f, 0.28f, 0.72f, 0.72f);
        icons::addLine (p, 0.72f, 0.28f, 0.28f, 0.72f);
        return p;
    }

    void irComboChanged()
    {
        if (updatingCombo)
            return;

        auto irs = proc.library.getIrs();
        auto index = irCombo.getSelectedId() - 1;

        if (juce::isPositiveAndBelow (index, irs.size()))
            proc.loadIr (irs[index].file);
    }

    void stepIr (int delta)
    {
        auto irs = proc.library.getIrs();

        if (irs.isEmpty())
            return;

        auto loadedPath = proc.getLoadedIrPath();
        int currentIndex = -1;

        for (int i = 0; i < irs.size(); ++i)
            if (irs[i].file.getFullPathName() == loadedPath)
                currentIndex = i;

        int nextIndex = currentIndex < 0 ? (delta > 0 ? 0 : irs.size() - 1)
                                         : (currentIndex + delta + irs.size()) % irs.size();

        if (auto error = proc.loadIr (irs[nextIndex].file); error.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                    "IR load failed", error);
        refreshIrList();
    }

    void clearIrClicked()
    {
        proc.clearIr();
        refreshIrList();
    }

    void importClicked()
    {
        fileChooser = std::make_unique<juce::FileChooser> ("Import impulse response", juce::File(), "*.wav");
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                auto file = fc.getResult();

                if (! file.existsAsFile())
                    return;

                juce::String error;
                auto installed = proc.library.importIr (file, error);

                if (installed.existsAsFile())
                    proc.loadIr (installed);

                refreshIrList();
            });
    }

    TubampAudioProcessor& proc;
    juce::ComboBox irCombo;
    IconButton irPrevButton { "irPrev", {} }, irNextButton { "irNext", {} }, irClearButton { "irClear", {} };
    juce::TextButton importButton { "Import..." };
    std::unique_ptr<juce::FileChooser> fileChooser;
    bool updatingCombo = false;

    juce::Rectangle<int> captionBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CabBody)
};

//==============================================================================
BlockParamPanel::BlockParamPanel (TubampAudioProcessor& processor)
    : proc (processor), accentColour (TubampLookAndFeel::accent)
{
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setFont (TubampLookAndFeel::kernedFont (13.0f, true));
    addAndMakeVisible (titleLabel);

    TubampLookAndFeel::makePowerPill (powerButton, accentColour);
    powerButton.setTooltip ("Enable or bypass this block");
    addAndMakeVisible (powerButton);

    removeButton.setGlyph (icons::makeTrashGlyph());
    removeButton.setColours (TubampLookAndFeel::textFaint, TubampLookAndFeel::text);
    removeButton.setTooltip ("Remove this block from the chain");
    removeButton.setInset (0.24f);
    removeButton.onClick = [this]
    {
        if (block.has_value() && onRemoveBlock != nullptr)
            onRemoveBlock (*block);
    };
    addAndMakeVisible (removeButton);

    modTypeCombo.addItemList (params::modTypeChoices, 1);
    TubampLookAndFeel::styleCombo (modTypeCombo);
    addChildComponent (modTypeCombo);
    modTypeAttachment = std::make_unique<ComboBoxAttachment> (proc.apvts, params::modType, modTypeCombo);

    ampBody = std::make_unique<AmpBody> (proc);
    addChildComponent (ampBody.get());

    cabBody = std::make_unique<CabBody> (proc);
    addChildComponent (cabBody.get());

    emptyLabel.setJustificationType (juce::Justification::centred);
    emptyLabel.setFont (TubampLookAndFeel::plainFont (15.0f));
    emptyLabel.setColour (juce::Label::textColourId, TubampLookAndFeel::textFaint);
    addChildComponent (emptyLabel);

    // The library is a single-listener callback: the panel owns it and fans it out to
    // whichever body needs refreshing (model list and IR list).
    proc.library.onChanged = [this]
    {
        ampBody->refreshModelList();
        cabBody->refreshIrList();
    };

    setBlock ({});
}

BlockParamPanel::~BlockParamPanel()
{
    proc.library.onChanged = nullptr;
}

std::vector<KnobSpec> BlockParamPanel::knobSpecsFor (chain::BlockId id)
{
    switch (id)
    {
        case chain::BlockId::gate:
            return { { params::gateThreshold, "THRESH" } };
        case chain::BlockId::comp:
            return { { params::compThreshold, "THRESH" }, { params::compRatio, "RATIO" },
                     { params::compAttack, "ATTACK" }, { params::compRelease, "RELEASE" },
                     { params::compMakeup, "MAKEUP" } };
        case chain::BlockId::drive:
            return { { params::driveGain, "GAIN" }, { params::driveTone, "TONE" },
                     { params::driveLevel, "LEVEL" } };
        case chain::BlockId::cab:
            return { { params::cabLowCut, "LO CUT" }, { params::cabHighCut, "HI CUT" } };
        case chain::BlockId::eq:
            return { { params::eqBass, "BASS" }, { params::eqMid, "MID" }, { params::eqTreble, "TREBLE" } };
        case chain::BlockId::mod:
            return { { params::modRate, "RATE" }, { params::modDepth, "DEPTH" }, { params::modMix, "MIX" } };
        case chain::BlockId::delay:
            return { { params::delayTime, "TIME" }, { params::delayFeedback, "FDBK" },
                     { params::delayMix, "MIX" } };
        case chain::BlockId::reverb:
            return { { params::reverbSize, "SIZE" }, { params::reverbDamping, "DAMP" },
                     { params::reverbMix, "MIX" } };
        case chain::BlockId::amp:
            break;   // the amp body carries its own knobs
    }

    return {};
}

void BlockParamPanel::buildKnobs (const std::vector<KnobSpec>& specs, juce::Colour blockColour)
{
    knobs.clear();

    for (const auto& spec : specs)
    {
        auto* knob = knobs.add (new KnobUI());
        TubampLookAndFeel::styleKnob (knob->slider, blockColour);
        addAndMakeVisible (knob->slider);

        knob->label.setText (spec.label, juce::dontSendNotification);
        styleMicroLabel (knob->label);
        addAndMakeVisible (knob->label);

        knob->attachment = std::make_unique<SliderAttachment> (proc.apvts, spec.paramId, knob->slider);
    }
}

void BlockParamPanel::setBlock (std::optional<chain::BlockId> id)
{
    block = id;
    powerAttachment.reset();

    const bool hasBlock = block.has_value();
    const bool isAmp = hasBlock && *block == chain::BlockId::amp;
    const bool isCab = hasBlock && *block == chain::BlockId::cab;
    const bool isMod = hasBlock && *block == chain::BlockId::mod;

    accentColour = hasBlock ? TubampLookAndFeel::blockAccent (*block) : TubampLookAndFeel::accent;
    headerIcon = hasBlock ? icons::makeIcon (*block) : juce::Path();

    titleLabel.setVisible (hasBlock);
    powerButton.setVisible (hasBlock);
    removeButton.setVisible (hasBlock);
    emptyLabel.setVisible (! hasBlock);

    ampBody->setVisible (isAmp);
    cabBody->setVisible (isCab);
    modTypeCombo.setVisible (isMod);

    if (isCab)
        cabBody->refreshIrList();   // a preset load may have swapped the IR meanwhile

    if (hasBlock)
    {
        const auto& info = chain::infoFor (*block);
        titleLabel.setText (info.displayName, juce::dontSendNotification);
        titleLabel.setColour (juce::Label::textColourId, TubampLookAndFeel::text);

        powerButton.setColour (juce::ToggleButton::tickColourId, accentColour);
        powerAttachment = std::make_unique<ButtonAttachment> (proc.apvts, info.enableParamId, powerButton);

        buildKnobs (knobSpecsFor (*block), accentColour);
    }
    else
    {
        knobs.clear();
    }

    resized();
    repaint();
}

void BlockParamPanel::layoutKnobRow (juce::Rectangle<int> area)
{
    if (knobs.isEmpty())
        return;

    const int rowWidth = knobs.size() * knobCellW;
    auto row = area.withSizeKeepingCentre (rowWidth, juce::jmin (knobCellH, area.getHeight()));

    for (auto* knob : knobs)
    {
        auto cell = row.removeFromLeft (knobCellW);
        knob->label.setBounds (cell.removeFromBottom (knobLabelH));
        cell.removeFromBottom (4);
        knob->slider.setBounds (cell);
    }
}

void BlockParamPanel::resized()
{
    auto area = getLocalBounds().reduced (1);

    auto header = area.removeFromTop (headerH);
    auto headerControls = header.reduced (20, 0);
    removeButton.setBounds (headerControls.removeFromRight (28).withSizeKeepingCentre (28, 28));
    headerControls.removeFromRight (10);
    powerButton.setBounds (headerControls.removeFromRight (84).withSizeKeepingCentre (84, 28));

    titleLabel.setBounds (48, 14, 320, 20);

    auto body = area.reduced (24, 14);
    emptyLabel.setBounds (body);

    if (! block.has_value())
        return;

    if (*block == chain::BlockId::amp)
    {
        ampBody->setBounds (body);
        return;
    }

    if (*block == chain::BlockId::cab)
    {
        auto top = body.removeFromTop (56);
        cabBody->setBounds (top);
        layoutKnobRow (body);
        return;
    }

    if (*block == chain::BlockId::mod)
    {
        auto top = body.removeFromTop (44);
        modTypeCombo.setBounds (top.withSizeKeepingCentre (180, 30));
        layoutKnobRow (body);
        return;
    }

    layoutKnobRow (body);
}

void BlockParamPanel::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

    g.setColour (TubampLookAndFeel::bgElevated);
    g.fillRoundedRectangle (bounds, 10.0f);
    g.setColour (TubampLookAndFeel::stroke);
    g.drawRoundedRectangle (bounds, 10.0f, 1.0f);

    if (! block.has_value())
        return;

    // Accent wash behind the header, fading out to the right.
    const auto header = juce::Rectangle<float> (bounds.getX(), bounds.getY(),
                                                bounds.getWidth(), (float) headerH);
    g.setGradientFill (juce::ColourGradient (accentColour.withAlpha (0.10f), header.getX(), header.getY(),
                                             accentColour.withAlpha (0.0f), header.getRight() * 0.6f, header.getY(),
                                             false));
    g.fillRect (header.reduced (1.0f, 1.0f));

    g.setColour (TubampLookAndFeel::stroke);
    g.fillRect (juce::Rectangle<float> (header.getX() + 1.0f, header.getBottom(), header.getWidth() - 2.0f, 1.0f));

    g.setColour (accentColour);
    icons::drawUnitPath (g, headerIcon, { 20.0f, (float) headerH * 0.5f - 10.0f, 20.0f, 20.0f }, 1.8f);

    // Thin accent underline under the block name.
    g.fillRect (juce::Rectangle<float> (48.0f, 38.0f, 48.0f, 2.0f));
}
} // namespace tubamp
