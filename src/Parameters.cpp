#include "Parameters.h"

namespace params
{
namespace
{
constexpr int kVersionHint = 1;

/** Range whose normalised 0.5 point sits at the geometric mean — the natural
    feel for frequency / time / ratio controls. */
juce::NormalisableRange<float> logRange (float minValue, float maxValue, float interval)
{
    juce::NormalisableRange<float> range { minValue, maxValue, interval };
    range.setSkewForCentre (std::sqrt (minValue * maxValue));
    return range;
}

using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

void addFloat (Layout& layout, const char* id, const juce::String& name,
               juce::NormalisableRange<float> range, float defaultValue,
               const juce::String& label = {})
{
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { id, kVersionHint }, name, range, defaultValue,
        juce::AudioParameterFloatAttributes().withLabel (label)));
}

void addBool (Layout& layout, const char* id, const juce::String& name, bool defaultValue)
{
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { id, kVersionHint }, name, defaultValue));
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    Layout layout;

    // --- block enables (everything on out of the box except the drive pedal)
    addBool (layout, gateOn,   "Gate On",   true);
    addBool (layout, compOn,   "Comp On",   true);
    addBool (layout, driveOn,  "Drive On",  false);
    addBool (layout, ampOn,    "Amp On",    true);
    addBool (layout, cabOn,    "Cab On",    true);
    addBool (layout, eqOn,     "EQ On",     true);
    addBool (layout, modOn,    "Mod On",    true);
    addBool (layout, delayOn,  "Delay On",  true);
    addBool (layout, reverbOn, "Reverb On", true);

    // --- input / output
    addFloat (layout, inputTrim,   "Input Trim",   { -24.0f, 24.0f, 0.1f },  0.0f, "dB");
    addFloat (layout, outputLevel, "Output Level", { -60.0f, 12.0f, 0.1f },  0.0f, "dB");

    // --- gate
    addFloat (layout, gateThreshold, "Gate Threshold", { -100.0f, 0.0f, 0.1f }, -80.0f, "dB");

    // --- compressor
    addFloat (layout, compThreshold, "Comp Threshold", { -60.0f, 0.0f, 0.1f }, -20.0f, "dB");
    addFloat (layout, compRatio,     "Comp Ratio",     logRange (1.0f, 20.0f, 0.01f),    4.0f, ":1");
    addFloat (layout, compAttack,    "Comp Attack",    logRange (0.1f, 100.0f, 0.01f),   5.0f, "ms");
    addFloat (layout, compRelease,   "Comp Release",   logRange (10.0f, 1000.0f, 0.1f),  120.0f, "ms");
    addFloat (layout, compMakeup,    "Comp Makeup",    { 0.0f, 24.0f, 0.1f },            0.0f, "dB");

    // --- drive
    addFloat (layout, driveGain,  "Drive Gain",  { 0.0f, 36.0f, 0.1f },              12.0f, "dB");
    addFloat (layout, driveTone,  "Drive Tone",  logRange (500.0f, 12000.0f, 1.0f), 4000.0f, "Hz");
    addFloat (layout, driveLevel, "Drive Level", { -24.0f, 12.0f, 0.1f },             0.0f, "dB");

    // --- amp (NAM)
    addFloat (layout, ampInput,  "Amp Input",  { -20.0f, 20.0f, 0.1f }, 0.0f, "dB");
    addFloat (layout, ampOutput, "Amp Output", { -40.0f, 40.0f, 0.1f }, 0.0f, "dB");
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { ampOutMode, kVersionHint }, "Output Mode", ampOutModeChoices,
        (int) OutputMode::normalized));
    addBool  (layout, ampCalInput, "Calibrate Input", false);
    addFloat (layout, ampCalLevel, "Input Calibration Level", { -60.0f, 60.0f, 0.1f }, 12.0f, "dBu");
    // 0 = smallest/cheapest sub-model, 1 = full size. Default matches the official
    // plugin's kSlim default of 0.0 (NeuralAmpModeler.cpp:96).
    addFloat (layout, ampSlim, "Slim", { 0.0f, 1.0f, 0.01f }, 0.0f);

    // --- cab
    addFloat (layout, cabLowCut,  "Cab Low Cut",  logRange (20.0f, 500.0f, 1.0f),      80.0f, "Hz");
    addFloat (layout, cabHighCut, "Cab High Cut", logRange (2000.0f, 20000.0f, 1.0f), 8000.0f, "Hz");

    // --- tone stack (0-10 knob units, 5 = flat)
    addFloat (layout, eqBass,   "Bass",   { 0.0f, 10.0f, 0.1f }, 5.0f);
    addFloat (layout, eqMid,    "Middle", { 0.0f, 10.0f, 0.1f }, 5.0f);
    addFloat (layout, eqTreble, "Treble", { 0.0f, 10.0f, 0.1f }, 5.0f);

    // --- modulation
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { modType, kVersionHint }, "Mod Type", modTypeChoices, 0));
    addFloat (layout, modRate,  "Mod Rate",  logRange (0.05f, 10.0f, 0.001f), 1.0f, "Hz");
    addFloat (layout, modDepth, "Mod Depth", { 0.0f, 1.0f, 0.001f },          0.4f);
    addFloat (layout, modMix,   "Mod Mix",   { 0.0f, 1.0f, 0.001f },          0.35f);

    // --- delay
    addFloat (layout, delayTime,     "Delay Time",     logRange (20.0f, 2000.0f, 0.1f), 420.0f, "ms");
    addFloat (layout, delayFeedback, "Delay Feedback", { 0.0f, 0.95f, 0.001f },           0.35f);
    addFloat (layout, delayMix,      "Delay Mix",      { 0.0f, 1.0f, 0.001f },            0.25f);

    // --- reverb
    addFloat (layout, reverbSize,    "Reverb Size",    { 0.0f, 1.0f, 0.001f }, 0.5f);
    addFloat (layout, reverbDamping, "Reverb Damping", { 0.0f, 1.0f, 0.001f }, 0.5f);
    addFloat (layout, reverbMix,     "Reverb Mix",     { 0.0f, 1.0f, 0.001f }, 0.25f);

    // --- external AudioUnit slots
    //
    // Appended at the very end, deliberately, even though they belong with the other
    // block enables above. Inserting them there would shift the index of every
    // parameter that follows, and while the AU parameter *ids* are content hashes (so
    // saved automation survives), the indices are what reach the host for
    // touch-to-select and what order a generic editor lists. Growing the list only at
    // the end costs nothing and changes nothing that already exists.
    //
    // On by default so that adding a slot and loading a plugin into it is audible
    // without a second click; an empty slot is a pass-through either way.
    addBool (layout, fx1On, "FX 1 On", true);
    addBool (layout, fx2On, "FX 2 On", true);
    addBool (layout, fx3On, "FX 3 On", true);

    return layout;
}
} // namespace params
