#include "PeakMeter.h"

#include "TubampLookAndFeel.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace tubamp
{
namespace
{
/** The gradient stops from the palette, keyed off the segment's own level. */
juce::Colour colourForDb (float dB)
{
    if (dB >= -3.0f)
        return TubampLookAndFeel::meterHigh;

    if (dB >= -12.0f)
        return TubampLookAndFeel::meterMid;

    return TubampLookAndFeel::meterLow;
}
} // namespace

float PeakMeter::positionForDb (float dB)
{
    return juce::jlimit (0.0f, 1.0f, (dB - minDb) / (maxDb - minDb));
}

void PeakMeter::pushLevel (float linearPeak)
{
    constexpr float decay = 0.75f;
    displayLevel = juce::jmax (linearPeak, displayLevel * decay);

    const auto now = juce::Time::getMillisecondCounterHiRes();

    if (displayLevel >= holdLevel)
    {
        holdLevel = displayLevel;
        holdStartMs = now;
    }
    else if (now - holdStartMs > 1500.0)
    {
        holdLevel = juce::jmax (displayLevel, holdLevel * 0.90f);
    }

    repaint();
}

void PeakMeter::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto level = positionForDb (juce::Decibels::gainToDecibels (displayLevel, minDb));
    const auto hold = positionForDb (juce::Decibels::gainToDecibels (holdLevel, minDb));

    constexpr float gap = 2.0f;
    const auto segmentH = (bounds.getHeight() - gap * (float) (numSegments - 1)) / (float) numSegments;

    for (int i = 0; i < numSegments; ++i)
    {
        // Segment 0 is at the bottom.
        const auto y = bounds.getBottom() - (float) (i + 1) * segmentH - (float) i * gap;
        const juce::Rectangle<float> segment (bounds.getX(), y, bounds.getWidth(), segmentH);

        const auto segmentDb = minDb + (maxDb - minDb) * ((float) i + 0.5f) / (float) numSegments;
        const auto colour = colourForDb (segmentDb);
        const auto lit = level > (float) i / (float) numSegments;

        g.setColour (TubampLookAndFeel::bg);
        g.fillRoundedRectangle (segment, 1.5f);

        g.setColour (lit ? colour : colour.withAlpha (0.10f));
        g.fillRoundedRectangle (segment, 1.5f);

        if (lit)
        {
            g.setColour (colour.withAlpha (0.25f));
            g.fillRoundedRectangle (segment.expanded (1.0f), 2.0f);
        }
    }

    if (hold > 0.001f)
    {
        const auto y = bounds.getBottom() - hold * bounds.getHeight();
        g.setColour (colourForDb (juce::Decibels::gainToDecibels (holdLevel, minDb)).withAlpha (0.9f));
        g.fillRect (juce::Rectangle<float> (bounds.getX(), juce::jmax (bounds.getY(), y - 1.0f),
                                            bounds.getWidth(), 2.0f));
    }
}
} // namespace tubamp
