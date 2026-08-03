#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace tubamp
{
/** Vertical segmented peak meter (10 rounded segments, green → amber → red per the
    palette) with a peak-hold tick that sits for ~1.5 s and then falls.

    The owner calls pushLevel() ~30 Hz with a linear peak read from an atomic (and is
    expected to reset that atomic to 0 after reading, so the meter only shows activity
    since the last poll); this component applies its own decay for a smooth fall-off
    between polls. */
class PeakMeter : public juce::Component
{
public:
    void paint (juce::Graphics&) override;
    void pushLevel (float linearPeak);

private:
    static constexpr int numSegments = 10;
    static constexpr float minDb = -60.0f, maxDb = 6.0f;

    /** dB -> 0..1 across the meter's range. */
    static float positionForDb (float dB);

    float displayLevel = 0.0f;
    float holdLevel = 0.0f;
    double holdStartMs = 0.0;
};
} // namespace tubamp
