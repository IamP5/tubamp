#include "TubampLookAndFeel.h"
#include "BlockIcons.h"

namespace tubamp
{
namespace
{
// Component property that switches drawToggleButton() between its three looks.
constexpr const char* toggleStyleProperty = "tubampToggleStyle";
constexpr const char* toggleStyleDot      = "dot";
constexpr const char* toggleStylePill     = "pill";

juce::Colour toggleAccent (juce::Button& b)
{
    return b.findColour (juce::ToggleButton::tickColourId);
}
} // namespace

juce::Colour TubampLookAndFeel::blockAccent (chain::BlockId id) noexcept
{
    switch (id)
    {
        case chain::BlockId::gate:   return juce::Colour (0xffa78bfa);
        case chain::BlockId::comp:   return juce::Colour (0xfffbbf24);
        case chain::BlockId::drive:  return juce::Colour (0xfff97316);
        case chain::BlockId::amp:    return juce::Colour (0xfff43f5e);
        case chain::BlockId::cab:    return juce::Colour (0xff2dd4bf);
        case chain::BlockId::eq:     return juce::Colour (0xff60a5fa);
        case chain::BlockId::mod:    return juce::Colour (0xff4ade80);
        case chain::BlockId::delay:  return juce::Colour (0xff818cf8);
        case chain::BlockId::reverb: return juce::Colour (0xff22d3ee);
    }

    return accent;
}

juce::Font TubampLookAndFeel::plainFont (float height, bool bold)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

juce::Font TubampLookAndFeel::kernedFont (float height, bool bold)
{
    return plainFont (height, bold).withExtraKerningFactor (0.08f);
}

void TubampLookAndFeel::styleKnob (juce::Slider& slider, juce::Colour accentColour,
                                   int textBoxWidth, int textBoxHeight)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, textBoxWidth, textBoxHeight);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accentColour);
    slider.setColour (juce::Slider::rotarySliderOutlineColourId, stroke);
    slider.setColour (juce::Slider::thumbColourId, accentColour);
    slider.setColour (juce::Slider::textBoxTextColourId, text);
    slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    slider.setColour (juce::Slider::textBoxHighlightColourId, accentColour.withAlpha (0.30f));
}

void TubampLookAndFeel::styleCombo (juce::ComboBox& box, bool borderless)
{
    box.setColour (juce::ComboBox::backgroundColourId,
                   borderless ? juce::Colours::transparentBlack : panel);
    box.setColour (juce::ComboBox::outlineColourId,
                   borderless ? juce::Colours::transparentBlack : stroke);
    box.setColour (juce::ComboBox::textColourId, text);
    box.setColour (juce::ComboBox::arrowColourId, borderless ? textDim : accent);

    if (borderless)
        box.setJustificationType (juce::Justification::centred);
}

void TubampLookAndFeel::makePowerDot (juce::ToggleButton& button, juce::Colour accentColour)
{
    button.getProperties().set (toggleStyleProperty, toggleStyleDot);
    button.setColour (juce::ToggleButton::tickColourId, accentColour);
    button.setClickingTogglesState (true);
}

void TubampLookAndFeel::makePowerPill (juce::ToggleButton& button, juce::Colour accentColour)
{
    button.getProperties().set (toggleStyleProperty, toggleStylePill);
    button.setColour (juce::ToggleButton::tickColourId, accentColour);
    button.setClickingTogglesState (true);
}

TubampLookAndFeel::TubampLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, bg);
    setColour (juce::DocumentWindow::backgroundColourId, bg);

    setColour (juce::Slider::rotarySliderFillColourId, accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, stroke);
    setColour (juce::Slider::thumbColourId, accent);
    setColour (juce::Slider::trackColourId, accent);
    setColour (juce::Slider::backgroundColourId, panel);
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    setColour (juce::Label::textColourId, text);
    setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);

    setColour (juce::ComboBox::backgroundColourId, panel);
    setColour (juce::ComboBox::outlineColourId, stroke);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::arrowColourId, accent);
    setColour (juce::ComboBox::buttonColourId, panel);

    setColour (juce::PopupMenu::backgroundColourId, panel);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, textDim);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.22f));
    setColour (juce::PopupMenu::highlightedTextColourId, text);

    setColour (juce::TextButton::buttonColourId, panel);
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, accent);

    setColour (juce::TextEditor::backgroundColourId, bgElevated);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, stroke);
    setColour (juce::TextEditor::focusedOutlineColourId, accent);
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.30f));
    setColour (juce::TextEditor::highlightedTextColourId, text);
    setColour (juce::CaretComponent::caretColourId, accent);

    setColour (juce::ToggleButton::textColourId, text);
    setColour (juce::ToggleButton::tickColourId, accent);
    setColour (juce::ToggleButton::tickDisabledColourId, strokeHi);

    setColour (juce::AlertWindow::backgroundColourId, panel);
    setColour (juce::AlertWindow::textColourId, text);
    setColour (juce::AlertWindow::outlineColourId, strokeHi);

    setColour (juce::ProgressBar::backgroundColourId, bgElevated);
    setColour (juce::ProgressBar::foregroundColourId, accent);

    setColour (juce::TooltipWindow::backgroundColourId, panelHi);
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, strokeHi);

    setColour (juce::ScrollBar::thumbColourId, strokeHi);
    setColour (juce::ListBox::backgroundColourId, panel);
}

//==============================================================================
void TubampLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                          float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                                          juce::Slider& slider)
{
    const auto alpha = slider.isEnabled() ? 1.0f : 0.35f;
    const auto hovered = slider.isEnabled() && slider.isMouseOverOrDragging (true);

    auto fill = slider.findColour (juce::Slider::rotarySliderFillColourId)
                      .withMultipliedAlpha (alpha);
    if (hovered)
        fill = fill.brighter (0.18f);

    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height)
                            .reduced (3.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto lineW = juce::jmax (2.5f, radius * 0.11f);
    const auto arcRadius = radius - lineW * 0.5f;
    const auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                         rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (slider.findColour (juce::Slider::rotarySliderOutlineColourId)
                       .withMultipliedAlpha (alpha));
    g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    if (std::abs (angle - rotaryStartAngle) > 0.001f)
    {
        juce::Path valueArc;
        valueArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                rotaryStartAngle, angle, true);
        g.setColour (fill);
        g.strokePath (valueArc, juce::PathStrokeType (lineW, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
    }

    // Thumb dot riding the tip of the value arc.
    const juce::Point<float> tip (centre.x + std::sin (angle) * arcRadius,
                                  centre.y - std::cos (angle) * arcRadius);
    const auto dotR = lineW * 0.85f;
    g.setColour (fill.brighter (0.35f));
    g.fillEllipse (juce::Rectangle<float> (dotR * 2.0f, dotR * 2.0f).withCentre (tip));

    // Dark face with a subtle top-lit gradient and a hairline ring.
    const auto faceR = juce::jmax (2.0f, arcRadius - lineW * 1.6f);
    const auto face = juce::Rectangle<float> (faceR * 2.0f, faceR * 2.0f).withCentre (centre);
    g.setGradientFill (juce::ColourGradient (panelHi.withMultipliedAlpha (alpha), face.getCentreX(), face.getY(),
                                             bgElevated.withMultipliedAlpha (alpha), face.getCentreX(), face.getBottom(),
                                             false));
    g.fillEllipse (face);
    g.setColour (stroke.withMultipliedAlpha (alpha));
    g.drawEllipse (face.reduced (0.5f), 1.0f);

    // Needle from the centre outwards.
    const juce::Point<float> needleStart (centre.x + std::sin (angle) * faceR * 0.30f,
                                          centre.y - std::cos (angle) * faceR * 0.30f);
    const juce::Point<float> needleEnd (centre.x + std::sin (angle) * faceR * 0.82f,
                                        centre.y - std::cos (angle) * faceR * 0.82f);
    g.setColour (fill.brighter (0.25f));
    g.drawLine ({ needleStart, needleEnd }, juce::jmax (1.5f, lineW * 0.6f));
}

void TubampLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                          float sliderPos, float minSliderPos, float maxSliderPos,
                                          juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
                                          minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const auto alpha = slider.isEnabled() ? 1.0f : 0.35f;
    const auto fill = slider.findColour (juce::Slider::trackColourId).withMultipliedAlpha (alpha);
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
    const auto horizontal = style == juce::Slider::LinearHorizontal;
    const auto thickness = 4.0f;

    auto track = horizontal ? bounds.withSizeKeepingCentre (bounds.getWidth(), thickness)
                            : bounds.withSizeKeepingCentre (thickness, bounds.getHeight());
    g.setColour (stroke.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (track, thickness * 0.5f);

    auto filled = track;
    if (horizontal)
        filled = filled.withRight (sliderPos);
    else
        filled = filled.withTop (sliderPos);

    g.setColour (fill);
    g.fillRoundedRectangle (filled, thickness * 0.5f);

    const auto thumbR = juce::jmin (7.0f, (horizontal ? bounds.getHeight() : bounds.getWidth()) * 0.5f);
    const juce::Point<float> thumbCentre = horizontal ? juce::Point<float> (sliderPos, track.getCentreY())
                                                      : juce::Point<float> (track.getCentreX(), sliderPos);
    g.setColour (bgElevated.withMultipliedAlpha (alpha));
    g.fillEllipse (juce::Rectangle<float> (thumbR * 2.0f, thumbR * 2.0f).withCentre (thumbCentre));
    g.setColour (fill.brighter (0.2f));
    g.drawEllipse (juce::Rectangle<float> (thumbR * 2.0f, thumbR * 2.0f).withCentre (thumbCentre).reduced (1.0f), 2.0f);
}

juce::Rectangle<float> TubampLookAndFeel::connectedEdgeBounds (juce::Button& button, float cornerRadius)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const auto flags = button.getConnectedEdgeFlags();

    if ((flags & juce::Button::ConnectedOnLeft) != 0)
        bounds.setLeft (bounds.getX() - cornerRadius);
    if ((flags & juce::Button::ConnectedOnRight) != 0)
        bounds.setRight (bounds.getRight() + cornerRadius);
    if ((flags & juce::Button::ConnectedOnTop) != 0)
        bounds.setTop (bounds.getY() - cornerRadius);
    if ((flags & juce::Button::ConnectedOnBottom) != 0)
        bounds.setBottom (bounds.getBottom() + cornerRadius);

    return bounds;
}

void TubampLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                              const juce::Colour& backgroundColour,
                                              bool shouldDrawButtonAsHighlighted,
                                              bool shouldDrawButtonAsDown)
{
    constexpr float corner = 6.0f;
    const auto bounds = connectedEdgeBounds (button, corner);
    const auto on = button.getToggleState();
    const auto accentColour = button.findColour (juce::TextButton::buttonOnColourId);
    const auto alpha = button.isEnabled() ? 1.0f : 0.4f;

    auto fill = on ? accentColour.withAlpha (0.18f) : backgroundColour;
    if (shouldDrawButtonAsDown)
        fill = fill.darker (0.25f);
    else if (shouldDrawButtonAsHighlighted)
        fill = on ? accentColour.withAlpha (0.28f) : panelHi;

    g.setColour (fill.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (bounds, corner);

    g.setColour ((on ? accentColour.withAlpha (0.85f)
                     : (shouldDrawButtonAsHighlighted ? strokeHi : stroke)).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (bounds, corner, 1.0f);
}

void TubampLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                          bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    const auto style = button.getProperties()[toggleStyleProperty].toString();
    const auto on = button.getToggleState();
    const auto alpha = button.isEnabled() ? 1.0f : 0.4f;
    const auto accentColour = toggleAccent (button).withMultipliedAlpha (alpha);

    if (style == toggleStylePill)
    {
        auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        const auto radius = bounds.getHeight() * 0.5f;

        g.setColour ((on ? accentColour.withAlpha (0.16f) : panel).withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (bounds, radius);
        g.setColour ((on ? accentColour : stroke).withMultipliedAlpha (
                         shouldDrawButtonAsHighlighted ? 1.0f : 0.85f));
        g.drawRoundedRectangle (bounds, radius, 1.0f);

        auto glyphArea = bounds.removeFromLeft (bounds.getHeight()).reduced (bounds.getHeight() * 0.28f);
        g.setColour (on ? accentColour : textFaint);
        icons::drawUnitPath (g, icons::makePowerGlyph(), glyphArea, 1.8f);

        g.setFont (kernedFont (10.0f, true));
        g.drawText (on ? "ON" : "OFF", bounds.reduced (2.0f, 0.0f),
                    juce::Justification::centredLeft, false);

        juce::ignoreUnused (shouldDrawButtonAsDown);
        return;
    }

    if (style == toggleStyleDot || button.getButtonText().isEmpty())
    {
        const auto bounds = button.getLocalBounds().toFloat();
        const auto diameter = juce::jmin (bounds.getWidth(), bounds.getHeight());
        const auto circle = juce::Rectangle<float> (diameter, diameter).withCentre (bounds.getCentre());

        if (on)
        {
            g.setColour (accentColour.withAlpha (0.20f));
            g.fillEllipse (circle.expanded (2.0f));
        }

        g.setColour ((on ? accentColour : panelHi).withMultipliedAlpha (alpha));
        g.fillEllipse (circle);
        g.setColour ((on ? accentColour.brighter (0.2f) : strokeHi).withMultipliedAlpha (alpha));
        g.drawEllipse (circle.reduced (0.5f), 1.0f);

        g.setColour (on ? bg : textFaint);
        icons::drawUnitPath (g, icons::makePowerGlyph(), circle.reduced (diameter * 0.28f),
                             juce::jmax (1.4f, diameter * 0.09f));

        juce::ignoreUnused (shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
        return;
    }

    // Labelled checkbox (settings dialog).
    auto bounds = button.getLocalBounds();
    const auto boxSize = juce::jmin (18, bounds.getHeight());
    auto boxArea = bounds.removeFromLeft (boxSize + 8).withSizeKeepingCentre (boxSize, boxSize);

    drawTickBox (g, button, (float) boxArea.getX(), (float) boxArea.getY(),
                 (float) boxArea.getWidth(), (float) boxArea.getHeight(),
                 on, button.isEnabled(), shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

    g.setColour (button.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (alpha));
    g.setFont (plainFont (13.0f));
    g.drawText (button.getButtonText(), bounds.reduced (4, 0), juce::Justification::centredLeft, true);
}

void TubampLookAndFeel::drawTickBox (juce::Graphics& g, juce::Component& component,
                                     float x, float y, float w, float h,
                                     bool ticked, bool isEnabled,
                                     bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused (shouldDrawButtonAsDown);

    const auto alpha = isEnabled ? 1.0f : 0.4f;
    const juce::Rectangle<float> box (x, y, w, h);
    const auto accentColour = component.findColour (juce::ToggleButton::tickColourId)
                                       .withMultipliedAlpha (alpha);

    g.setColour ((ticked ? accentColour.withAlpha (0.18f)
                         : (shouldDrawButtonAsHighlighted ? panelHi : panel)).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour ((ticked ? accentColour : stroke).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (box.reduced (0.5f), 4.0f, 1.0f);

    if (ticked)
    {
        juce::Path tick;
        tick.startNewSubPath (0.22f, 0.52f);
        tick.lineTo (0.44f, 0.74f);
        tick.lineTo (0.80f, 0.28f);
        g.setColour (accentColour);
        icons::drawUnitPath (g, tick, box, 2.0f);
    }
}

void TubampLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                      int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    const auto alpha = box.isEnabled() ? 1.0f : 0.4f;
    const auto over = box.isMouseOver (true);

    g.setColour (box.findColour (juce::ComboBox::backgroundColourId).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (bounds, 6.0f);

    auto outline = box.findColour (juce::ComboBox::outlineColourId);
    if (over && ! outline.isTransparent())
        outline = strokeHi;

    if (! outline.isTransparent())
    {
        g.setColour (outline.withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (bounds, 6.0f, 1.0f);
    }

    juce::Path arrow;
    arrow.startNewSubPath (0.2f, 0.38f);
    arrow.lineTo (0.5f, 0.64f);
    arrow.lineTo (0.8f, 0.38f);
    const auto arrowArea = juce::Rectangle<float> (10.0f, 8.0f)
                               .withCentre ({ bounds.getRight() - 13.0f, bounds.getCentreY() });
    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withMultipliedAlpha (alpha));
    icons::drawUnitPath (g, arrow, arrowArea, 1.8f);
}

void TubampLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (10, 1, box.getWidth() - 30, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
    label.setJustificationType (box.getJustificationType());
}

void TubampLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
    g.fillAll (panel);
    g.setColour (strokeHi);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);
}

void TubampLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height,
                                                  juce::TextEditor& editor)
{
    g.setColour (editor.findColour (juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height), 6.0f);
}

void TubampLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height,
                                               juce::TextEditor& editor)
{
    g.setColour (editor.hasKeyboardFocus (true) ? accent.withAlpha (0.8f) : stroke);
    g.drawRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f),
                            6.0f, 1.0f);
}

void TubampLookAndFeel::drawProgressBar (juce::Graphics& g, juce::ProgressBar& bar,
                                         int width, int height, double progress,
                                         const juce::String& textToShow)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
    const auto radius = juce::jmin (4.0f, bounds.getHeight() * 0.5f);

    g.setColour (bar.findColour (juce::ProgressBar::backgroundColourId));
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (stroke);
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);

    if (progress >= 0.0)
    {
        auto filled = bounds.reduced (1.0f);
        filled = filled.withWidth (filled.getWidth() * (float) juce::jlimit (0.0, 1.0, progress));
        g.setColour (bar.findColour (juce::ProgressBar::foregroundColourId));
        g.fillRoundedRectangle (filled, juce::jmax (1.0f, radius - 1.0f));
    }

    if (textToShow.isNotEmpty() && height > 12)
    {
        g.setColour (text);
        g.setFont (plainFont (11.0f));
        g.drawText (textToShow, bounds, juce::Justification::centred, false);
    }
}

void TubampLookAndFeel::drawAlertBox (juce::Graphics& g, juce::AlertWindow& window,
                                      const juce::Rectangle<int>& textArea, juce::TextLayout& textLayout)
{
    const auto bounds = window.getLocalBounds().toFloat().reduced (0.5f);

    g.setColour (window.findColour (juce::AlertWindow::backgroundColourId));
    g.fillRoundedRectangle (bounds, 8.0f);
    g.setColour (window.findColour (juce::AlertWindow::outlineColourId));
    g.drawRoundedRectangle (bounds, 8.0f, 1.0f);

    // Thin accent rule under the title area, mirroring the block headers.
    g.setColour (accent);
    g.fillRect (juce::Rectangle<float> (bounds.getX() + 20.0f, bounds.getY() + 34.0f, 40.0f, 2.0f));

    textLayout.draw (g, textArea.toFloat());
}

juce::Font TubampLookAndFeel::getLabelFont (juce::Label& label)
{
    return label.getFont();
}

juce::Font TubampLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return plainFont (juce::jmin (14.0f, (float) box.getHeight() * 0.6f));
}

juce::Font TubampLookAndFeel::getPopupMenuFont()
{
    return plainFont (13.0f);
}

juce::Font TubampLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return plainFont (juce::jmin (13.0f, (float) buttonHeight * 0.55f));
}

juce::Font TubampLookAndFeel::getAlertWindowMessageFont()
{
    return plainFont (13.0f);
}

juce::Font TubampLookAndFeel::getAlertWindowTitleFont()
{
    return plainFont (16.0f, true);
}
} // namespace tubamp
