#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "BlockIcons.h"
#include "TubampLookAndFeel.h"

namespace tubamp
{
/** A button that paints a unit-box vector glyph (see BlockIcons.h) instead of text:
    header chevrons, save, gear, the "remove block" affordance. Optionally sits on a
    rounded plate; without one it is just the glyph, which keeps the header airy. */
class IconButton : public juce::Button
{
public:
    IconButton (const juce::String& componentName, juce::Path unitGlyph)
        : juce::Button (componentName), glyph (std::move (unitGlyph))
    {
        setWantsKeyboardFocus (false);
    }

    void setGlyph (juce::Path newGlyph)
    {
        glyph = std::move (newGlyph);
        repaint();
    }

    void setColours (juce::Colour normal, juce::Colour hover)
    {
        normalColour = normal;
        hoverColour = hover;
        repaint();
    }

    /** Fraction of the shorter side left as padding around the glyph. */
    void setInset (float newInset) { inset = newInset; repaint(); }
    void setStrokeWidth (float w)  { strokeWidth = w; repaint(); }
    void setHasPlate (bool shouldHavePlate) { plate = shouldHavePlate; repaint(); }

    void paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                      bool shouldDrawButtonAsDown) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const auto alpha = isEnabled() ? 1.0f : 0.35f;

        if (plate)
        {
            g.setColour ((shouldDrawButtonAsHighlighted ? TubampLookAndFeel::panelHi
                                                        : TubampLookAndFeel::panel)
                             .withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (bounds.reduced (0.5f), 6.0f);
            g.setColour (TubampLookAndFeel::stroke.withMultipliedAlpha (alpha));
            g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);
        }

        const auto side = juce::jmin (bounds.getWidth(), bounds.getHeight()) * (1.0f - inset * 2.0f);
        const auto glyphArea = juce::Rectangle<float> (side, side).withCentre (bounds.getCentre());

        auto colour = shouldDrawButtonAsHighlighted ? hoverColour : normalColour;
        if (shouldDrawButtonAsDown)
            colour = colour.brighter (0.3f);

        g.setColour (colour.withMultipliedAlpha (alpha));
        icons::drawUnitPath (g, glyph, glyphArea, strokeWidth);
    }

private:
    juce::Path glyph;
    juce::Colour normalColour { TubampLookAndFeel::textDim };
    juce::Colour hoverColour { TubampLookAndFeel::text };
    float strokeWidth = 1.8f;
    float inset = 0.28f;
    bool plate = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconButton)
};
} // namespace tubamp
