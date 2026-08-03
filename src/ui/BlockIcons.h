#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>

#include "../dsp/ChainOrder.h"

/**
    Vector glyphs for the signal chain, drawn as thin geometric strokes inside a 0..1
    unit box so a caller can scale one to any tile/header size. Every path is meant to
    be *stroked* (never filled) with a rounded/curved PathStrokeType, so "dots" are
    simply very small circles. Each glyph stays well under 20 path operations.
*/
namespace tubamp::icons
{
/** Scales a unit-box path into `area` and strokes it with the current colour. */
inline void drawUnitPath (juce::Graphics& g, const juce::Path& unitPath,
                          juce::Rectangle<float> area, float strokeWidth)
{
    juce::Path p (unitPath);
    p.applyTransform (juce::AffineTransform::scale (area.getWidth(), area.getHeight())
                          .translated (area.getX(), area.getY()));
    g.strokePath (p, juce::PathStrokeType (strokeWidth, juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded));
}

inline void addLine (juce::Path& p, float x1, float y1, float x2, float y2)
{
    p.startNewSubPath (x1, y1);
    p.lineTo (x2, y2);
}

// --- block glyphs -----------------------------------------------------------

/** Two tall bars with a gap and a threshold tick between them. */
inline juce::Path makeGateIcon()
{
    juce::Path p;
    addLine (p, 0.20f, 0.10f, 0.20f, 0.90f);
    addLine (p, 0.80f, 0.10f, 0.80f, 0.90f);
    addLine (p, 0.40f, 0.50f, 0.60f, 0.50f);
    return p;
}

/** Two arrows converging on a horizontal line. */
inline juce::Path makeCompIcon()
{
    juce::Path p;
    addLine (p, 0.10f, 0.50f, 0.90f, 0.50f);
    p.startNewSubPath (0.50f, 0.08f);
    p.lineTo (0.50f, 0.34f);
    p.startNewSubPath (0.38f, 0.22f);
    p.lineTo (0.50f, 0.36f);
    p.lineTo (0.62f, 0.22f);
    p.startNewSubPath (0.50f, 0.92f);
    p.lineTo (0.50f, 0.66f);
    p.startNewSubPath (0.38f, 0.78f);
    p.lineTo (0.50f, 0.64f);
    p.lineTo (0.62f, 0.78f);
    return p;
}

/** A hard-clipped wave. */
inline juce::Path makeDriveIcon()
{
    juce::Path p;
    p.startNewSubPath (0.06f, 0.52f);
    p.lineTo (0.22f, 0.16f);
    p.lineTo (0.40f, 0.16f);
    p.lineTo (0.60f, 0.84f);
    p.lineTo (0.78f, 0.84f);
    p.lineTo (0.94f, 0.48f);
    return p;
}

/** Amp head: body, carry handle and two control dots. */
inline juce::Path makeAmpIcon()
{
    juce::Path p;
    p.addRoundedRectangle (0.10f, 0.32f, 0.80f, 0.48f, 0.06f);
    p.startNewSubPath (0.38f, 0.32f);
    p.quadraticTo (0.50f, 0.08f, 0.62f, 0.32f);
    addLine (p, 0.18f, 0.62f, 0.82f, 0.62f);
    p.addEllipse (0.25f, 0.42f, 0.07f, 0.07f);
    p.addEllipse (0.41f, 0.42f, 0.07f, 0.07f);
    return p;
}

/** Cab: cabinet square with a speaker cone and dust cap. */
inline juce::Path makeCabIcon()
{
    juce::Path p;
    p.addRoundedRectangle (0.10f, 0.10f, 0.80f, 0.80f, 0.08f);
    p.addEllipse (0.24f, 0.24f, 0.52f, 0.52f);
    p.addEllipse (0.44f, 0.44f, 0.12f, 0.12f);
    return p;
}

/** Three faders at different positions. */
inline juce::Path makeEqIcon()
{
    juce::Path p;
    addLine (p, 0.22f, 0.12f, 0.22f, 0.88f);
    addLine (p, 0.50f, 0.12f, 0.50f, 0.88f);
    addLine (p, 0.78f, 0.12f, 0.78f, 0.88f);
    addLine (p, 0.12f, 0.34f, 0.32f, 0.34f);
    addLine (p, 0.40f, 0.62f, 0.60f, 0.62f);
    addLine (p, 0.68f, 0.26f, 0.88f, 0.26f);
    return p;
}

/** Two full sine cycles. */
inline juce::Path makeModIcon()
{
    juce::Path p;
    p.startNewSubPath (0.06f, 0.50f);
    p.quadraticTo (0.17f, 0.06f, 0.28f, 0.50f);
    p.quadraticTo (0.39f, 0.94f, 0.50f, 0.50f);
    p.quadraticTo (0.61f, 0.06f, 0.72f, 0.50f);
    p.quadraticTo (0.83f, 0.94f, 0.94f, 0.50f);
    return p;
}

/** Three diminishing taps. */
inline juce::Path makeDelayIcon()
{
    juce::Path p;
    addLine (p, 0.18f, 0.12f, 0.18f, 0.88f);
    addLine (p, 0.50f, 0.28f, 0.50f, 0.72f);
    addLine (p, 0.82f, 0.41f, 0.82f, 0.59f);
    return p;
}

/** Three arcs radiating to the right of a source point. */
inline juce::Path makeReverbIcon()
{
    juce::Path p;
    const float from = juce::MathConstants<float>::pi * 0.22f;
    const float to   = juce::MathConstants<float>::pi * 0.78f;
    p.addCentredArc (0.14f, 0.50f, 0.26f, 0.26f, 0.0f, from, to, true);
    p.addCentredArc (0.14f, 0.50f, 0.48f, 0.48f, 0.0f, from, to, true);
    p.addCentredArc (0.14f, 0.50f, 0.70f, 0.70f, 0.0f, from, to, true);
    p.addEllipse (0.10f, 0.46f, 0.08f, 0.08f);
    return p;
}

inline juce::Path makeIcon (chain::BlockId id)
{
    switch (id)
    {
        case chain::BlockId::gate:   return makeGateIcon();
        case chain::BlockId::comp:   return makeCompIcon();
        case chain::BlockId::drive:  return makeDriveIcon();
        case chain::BlockId::amp:    return makeAmpIcon();
        case chain::BlockId::cab:    return makeCabIcon();
        case chain::BlockId::eq:     return makeEqIcon();
        case chain::BlockId::mod:    return makeModIcon();
        case chain::BlockId::delay:  return makeDelayIcon();
        case chain::BlockId::reverb: return makeReverbIcon();
    }

    return {};
}

// --- endpoint + UI glyphs ---------------------------------------------------

/** IN: an instrument jack (sleeve circle + cable). */
inline juce::Path makeInputIcon()
{
    juce::Path p;
    p.addEllipse (0.10f, 0.28f, 0.44f, 0.44f);
    addLine (p, 0.54f, 0.50f, 0.92f, 0.50f);
    return p;
}

/** OUT: a speaker cone. */
inline juce::Path makeOutputIcon()
{
    juce::Path p;
    p.startNewSubPath (0.14f, 0.36f);
    p.lineTo (0.34f, 0.36f);
    p.lineTo (0.58f, 0.14f);
    p.lineTo (0.58f, 0.86f);
    p.lineTo (0.34f, 0.64f);
    p.lineTo (0.14f, 0.64f);
    p.closeSubPath();
    p.addCentredArc (0.60f, 0.50f, 0.26f, 0.26f, 0.0f,
                     juce::MathConstants<float>::pi * 0.25f,
                     juce::MathConstants<float>::pi * 0.75f, true);
    return p;
}

/** The classic IEC power symbol: broken ring + stem. */
inline juce::Path makePowerGlyph()
{
    juce::Path p;
    p.addCentredArc (0.50f, 0.54f, 0.32f, 0.32f, 0.0f,
                     juce::MathConstants<float>::pi * 0.22f,
                     juce::MathConstants<float>::pi * 1.78f, true);
    addLine (p, 0.50f, 0.10f, 0.50f, 0.46f);
    return p;
}

inline juce::Path makeGearGlyph()
{
    juce::Path p;
    constexpr int teeth = 8;
    constexpr float rIn = 0.28f, rOut = 0.46f;

    for (int i = 0; i < teeth; ++i)
    {
        const auto a = juce::MathConstants<float>::twoPi * (float) i / (float) teeth;
        addLine (p, 0.5f + std::sin (a) * rIn, 0.5f - std::cos (a) * rIn,
                    0.5f + std::sin (a) * rOut, 0.5f - std::cos (a) * rOut);
    }

    p.addEllipse (0.5f - rIn, 0.5f - rIn, rIn * 2.0f, rIn * 2.0f);
    p.addEllipse (0.5f - 0.11f, 0.5f - 0.11f, 0.22f, 0.22f);
    return p;
}

inline juce::Path makeChevronGlyph (bool pointingRight)
{
    juce::Path p;

    if (pointingRight)
    {
        p.startNewSubPath (0.38f, 0.20f);
        p.lineTo (0.66f, 0.50f);
        p.lineTo (0.38f, 0.80f);
    }
    else
    {
        p.startNewSubPath (0.62f, 0.20f);
        p.lineTo (0.34f, 0.50f);
        p.lineTo (0.62f, 0.80f);
    }

    return p;
}

/** Save: a floppy outline with a shutter and a label. */
inline juce::Path makeSaveGlyph()
{
    juce::Path p;
    p.startNewSubPath (0.14f, 0.14f);
    p.lineTo (0.70f, 0.14f);
    p.lineTo (0.86f, 0.30f);
    p.lineTo (0.86f, 0.86f);
    p.lineTo (0.14f, 0.86f);
    p.closeSubPath();
    p.addRectangle (0.32f, 0.14f, 0.34f, 0.22f);
    p.addRectangle (0.30f, 0.54f, 0.40f, 0.32f);
    return p;
}

/** Remove-from-chain affordance. */
inline juce::Path makeTrashGlyph()
{
    juce::Path p;
    addLine (p, 0.14f, 0.26f, 0.86f, 0.26f);
    addLine (p, 0.40f, 0.26f, 0.40f, 0.14f);
    p.lineTo (0.60f, 0.14f);
    p.lineTo (0.60f, 0.26f);
    p.startNewSubPath (0.22f, 0.26f);
    p.lineTo (0.28f, 0.88f);
    p.lineTo (0.72f, 0.88f);
    p.lineTo (0.78f, 0.26f);
    addLine (p, 0.42f, 0.40f, 0.44f, 0.74f);
    addLine (p, 0.58f, 0.40f, 0.56f, 0.74f);
    return p;
}

inline juce::Path makePlusGlyph()
{
    juce::Path p;
    addLine (p, 0.50f, 0.22f, 0.50f, 0.78f);
    addLine (p, 0.22f, 0.50f, 0.78f, 0.50f);
    return p;
}

/** Flow chevron drawn on the connector between two blocks. */
inline juce::Path makeFlowArrow()
{
    juce::Path p;
    p.startNewSubPath (0.20f, 0.16f);
    p.lineTo (0.72f, 0.50f);
    p.lineTo (0.20f, 0.84f);
    return p;
}
} // namespace tubamp::icons
