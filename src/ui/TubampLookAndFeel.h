#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../dsp/ChainOrder.h"

namespace tubamp
{
/**
    The single source of visual truth: palette, per-block accents and the drawing of
    every stock JUCE control. Flat and precise — arc knobs, 6 px rounded controls,
    accent-driven highlights, no skeuomorphism.

    Per-control accents travel through the standard colour ids
    (`Slider::rotarySliderFillColourId`, `ToggleButton::tickColourId`,
    `TextButton::buttonOnColourId`), so a panel only has to set the block accent once
    and every control it owns picks it up.
*/
class TubampLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // --- palette --------------------------------------------------------------
    static inline const juce::Colour bg         { 0xff0c0e12 }; // window base
    static inline const juce::Colour bgElevated { 0xff12151b }; // bands, knob faces
    static inline const juce::Colour panel      { 0xff161a21 }; // tiles, controls
    static inline const juce::Colour panelHi    { 0xff1c222b }; // hover / selected
    static inline const juce::Colour stroke     { 0xff262e39 };
    static inline const juce::Colour strokeHi   { 0xff333d4b };
    static inline const juce::Colour text       { 0xffe6eaf0 };
    static inline const juce::Colour textDim    { 0xff8b95a6 };
    static inline const juce::Colour textFaint  { 0xff5a6473 };
    static inline const juce::Colour accent     { 0xffff7a1a }; // brand amber

    // Meter gradient stops: green up to -12 dB, amber up to -3 dB, red above.
    static inline const juce::Colour meterLow  { 0xff34d399 };
    static inline const juce::Colour meterMid  { 0xfffbbf24 };
    static inline const juce::Colour meterHigh { 0xfff87171 };

    /** The accent that identifies a block everywhere it appears (tile, icon, header,
        knob arcs, power LED). */
    static juce::Colour blockAccent (chain::BlockId id) noexcept;

    // --- fonts ----------------------------------------------------------------
    static juce::Font plainFont (float height, bool bold = false);
    /** Letter-spaced font for uppercase micro-labels and section titles. */
    static juce::Font kernedFont (float height, bool bold = false);

    // --- shared control styling ------------------------------------------------
    /** Rotary + borderless value box + accent arc. */
    static void styleKnob (juce::Slider& slider, juce::Colour accentColour,
                           int textBoxWidth = 64, int textBoxHeight = 16);
    static void styleCombo (juce::ComboBox& box, bool borderless = false);
    /** Draws as a small round power dot (used on tiles and inline rows). */
    static void makePowerDot (juce::ToggleButton& button, juce::Colour accentColour);
    /** Draws as a wide "power" pill with an ON/OFF caption. */
    static void makePowerPill (juce::ToggleButton& button, juce::Colour accentColour);

    TubampLookAndFeel();

    // --- LookAndFeel_V4 --------------------------------------------------------
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawTickBox (juce::Graphics&, juce::Component&, float x, float y, float w, float h,
                      bool ticked, bool isEnabled,
                      bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;

    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    void drawProgressBar (juce::Graphics&, juce::ProgressBar&, int width, int height,
                          double progress, const juce::String& textToShow) override;

    void drawAlertBox (juce::Graphics&, juce::AlertWindow&,
                       const juce::Rectangle<int>& textArea, juce::TextLayout&) override;

    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowTitleFont() override;

private:
    /** Rounded rectangle whose "connected" edges are pushed outside the component so
        the visible corners on those sides come out square (segmented button groups). */
    static juce::Rectangle<float> connectedEdgeBounds (juce::Button&, float cornerRadius);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TubampLookAndFeel)
};
} // namespace tubamp
