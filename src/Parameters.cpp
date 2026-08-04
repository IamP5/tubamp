#include "Parameters.h"

namespace params
{
namespace
{
constexpr int kVersionHint = 1;

/** Hint carried by every parameter added after the original layout. JUCE sorts an AU's
    parameter list by version hint before anything else, so hint-2 ids land after every
    hint-1 id whatever their hash — the list a host already knows keeps its order and
    the additions append to it. The AU parameter ID is a hash of the string id and does
    not depend on the hint, so saved automation is unaffected either way. */
constexpr int kVersionHint2 = 2;

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
               const juce::String& label = {}, int versionHint = kVersionHint)
{
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { id, versionHint }, name, range, defaultValue,
        juce::AudioParameterFloatAttributes().withLabel (label)));
}

void addBool (Layout& layout, const char* id, const juce::String& name, bool defaultValue,
              int versionHint = kVersionHint)
{
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { id, versionHint }, name, defaultValue));
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

    // --- pooled instances 2 and 3, then the amp's own tone stack
    //
    // Appended for the same reason as the fx enables above, and carrying version hint 2
    // so that a host which already knows the list appends them to it rather than
    // interleaving them by hash. Host-visible names are instance-numbered because Logic
    // lists parameters by name: three rows reading "Comp Threshold" would be unusable.
    //
    // Ranges, defaults and steps are copied verbatim from the instance-1 parameter each
    // one mirrors — if one of them ever changes, all three change together.
    //
    // The enables all default ON, including drive's (whose instance 1 defaults off):
    // adding a block to the chain by hand and hearing nothing until a second click is
    // the wrong first impression, and the block is not in the chain until it is added.
    addBool  (layout, comp2On,        "Comp 2 On",        true, kVersionHint2);
    addFloat (layout, comp2Threshold, "Comp 2 Threshold", { -60.0f, 0.0f, 0.1f },           -20.0f, "dB", kVersionHint2);
    addFloat (layout, comp2Ratio,     "Comp 2 Ratio",     logRange (1.0f, 20.0f, 0.01f),     4.0f,  ":1", kVersionHint2);
    addFloat (layout, comp2Attack,    "Comp 2 Attack",    logRange (0.1f, 100.0f, 0.01f),    5.0f,  "ms", kVersionHint2);
    addFloat (layout, comp2Release,   "Comp 2 Release",   logRange (10.0f, 1000.0f, 0.1f),   120.0f, "ms", kVersionHint2);
    addFloat (layout, comp2Makeup,    "Comp 2 Makeup",    { 0.0f, 24.0f, 0.1f },             0.0f,  "dB", kVersionHint2);

    addBool  (layout, comp3On,        "Comp 3 On",        true, kVersionHint2);
    addFloat (layout, comp3Threshold, "Comp 3 Threshold", { -60.0f, 0.0f, 0.1f },           -20.0f, "dB", kVersionHint2);
    addFloat (layout, comp3Ratio,     "Comp 3 Ratio",     logRange (1.0f, 20.0f, 0.01f),     4.0f,  ":1", kVersionHint2);
    addFloat (layout, comp3Attack,    "Comp 3 Attack",    logRange (0.1f, 100.0f, 0.01f),    5.0f,  "ms", kVersionHint2);
    addFloat (layout, comp3Release,   "Comp 3 Release",   logRange (10.0f, 1000.0f, 0.1f),   120.0f, "ms", kVersionHint2);
    addFloat (layout, comp3Makeup,    "Comp 3 Makeup",    { 0.0f, 24.0f, 0.1f },             0.0f,  "dB", kVersionHint2);

    addBool  (layout, drive2On,    "Drive 2 On",    true, kVersionHint2);
    addFloat (layout, drive2Gain,  "Drive 2 Gain",  { 0.0f, 36.0f, 0.1f },              12.0f, "dB", kVersionHint2);
    addFloat (layout, drive2Tone,  "Drive 2 Tone",  logRange (500.0f, 12000.0f, 1.0f), 4000.0f, "Hz", kVersionHint2);
    addFloat (layout, drive2Level, "Drive 2 Level", { -24.0f, 12.0f, 0.1f },             0.0f, "dB", kVersionHint2);

    addBool  (layout, drive3On,    "Drive 3 On",    true, kVersionHint2);
    addFloat (layout, drive3Gain,  "Drive 3 Gain",  { 0.0f, 36.0f, 0.1f },              12.0f, "dB", kVersionHint2);
    addFloat (layout, drive3Tone,  "Drive 3 Tone",  logRange (500.0f, 12000.0f, 1.0f), 4000.0f, "Hz", kVersionHint2);
    addFloat (layout, drive3Level, "Drive 3 Level", { -24.0f, 12.0f, 0.1f },             0.0f, "dB", kVersionHint2);

    addBool  (layout, eq2On,     "EQ 2 On",     true, kVersionHint2);
    addFloat (layout, eq2Bass,   "EQ 2 Bass",   { 0.0f, 10.0f, 0.1f }, 5.0f, {}, kVersionHint2);
    addFloat (layout, eq2Mid,    "EQ 2 Middle", { 0.0f, 10.0f, 0.1f }, 5.0f, {}, kVersionHint2);
    addFloat (layout, eq2Treble, "EQ 2 Treble", { 0.0f, 10.0f, 0.1f }, 5.0f, {}, kVersionHint2);

    addBool  (layout, eq3On,     "EQ 3 On",     true, kVersionHint2);
    addFloat (layout, eq3Bass,   "EQ 3 Bass",   { 0.0f, 10.0f, 0.1f }, 5.0f, {}, kVersionHint2);
    addFloat (layout, eq3Mid,    "EQ 3 Middle", { 0.0f, 10.0f, 0.1f }, 5.0f, {}, kVersionHint2);
    addFloat (layout, eq3Treble, "EQ 3 Treble", { 0.0f, 10.0f, 0.1f }, 5.0f, {}, kVersionHint2);

    addBool (layout, mod2On, "Mod 2 On", true, kVersionHint2);
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { mod2Type, kVersionHint2 }, "Mod 2 Type", modTypeChoices, 0));
    addFloat (layout, mod2Rate,  "Mod 2 Rate",  logRange (0.05f, 10.0f, 0.001f), 1.0f, "Hz", kVersionHint2);
    addFloat (layout, mod2Depth, "Mod 2 Depth", { 0.0f, 1.0f, 0.001f },          0.4f, {},   kVersionHint2);
    addFloat (layout, mod2Mix,   "Mod 2 Mix",   { 0.0f, 1.0f, 0.001f },          0.35f, {},  kVersionHint2);

    addBool (layout, mod3On, "Mod 3 On", true, kVersionHint2);
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { mod3Type, kVersionHint2 }, "Mod 3 Type", modTypeChoices, 0));
    addFloat (layout, mod3Rate,  "Mod 3 Rate",  logRange (0.05f, 10.0f, 0.001f), 1.0f, "Hz", kVersionHint2);
    addFloat (layout, mod3Depth, "Mod 3 Depth", { 0.0f, 1.0f, 0.001f },          0.4f, {},   kVersionHint2);
    addFloat (layout, mod3Mix,   "Mod 3 Mix",   { 0.0f, 1.0f, 0.001f },          0.35f, {},  kVersionHint2);

    addBool  (layout, delay2On,       "Delay 2 On",       true, kVersionHint2);
    addFloat (layout, delay2Time,     "Delay 2 Time",     logRange (20.0f, 2000.0f, 0.1f), 420.0f, "ms", kVersionHint2);
    addFloat (layout, delay2Feedback, "Delay 2 Feedback", { 0.0f, 0.95f, 0.001f },          0.35f, {},   kVersionHint2);
    addFloat (layout, delay2Mix,      "Delay 2 Mix",      { 0.0f, 1.0f, 0.001f },           0.25f, {},   kVersionHint2);

    addBool  (layout, delay3On,       "Delay 3 On",       true, kVersionHint2);
    addFloat (layout, delay3Time,     "Delay 3 Time",     logRange (20.0f, 2000.0f, 0.1f), 420.0f, "ms", kVersionHint2);
    addFloat (layout, delay3Feedback, "Delay 3 Feedback", { 0.0f, 0.95f, 0.001f },          0.35f, {},   kVersionHint2);
    addFloat (layout, delay3Mix,      "Delay 3 Mix",      { 0.0f, 1.0f, 0.001f },           0.25f, {},   kVersionHint2);

    addBool  (layout, reverb2On,      "Reverb 2 On",      true, kVersionHint2);
    addFloat (layout, reverb2Size,    "Reverb 2 Size",    { 0.0f, 1.0f, 0.001f }, 0.5f,  {}, kVersionHint2);
    addFloat (layout, reverb2Damping, "Reverb 2 Damping", { 0.0f, 1.0f, 0.001f }, 0.5f,  {}, kVersionHint2);
    addFloat (layout, reverb2Mix,     "Reverb 2 Mix",     { 0.0f, 1.0f, 0.001f }, 0.25f, {}, kVersionHint2);

    addBool  (layout, reverb3On,      "Reverb 3 On",      true, kVersionHint2);
    addFloat (layout, reverb3Size,    "Reverb 3 Size",    { 0.0f, 1.0f, 0.001f }, 0.5f,  {}, kVersionHint2);
    addFloat (layout, reverb3Damping, "Reverb 3 Damping", { 0.0f, 1.0f, 0.001f }, 0.5f,  {}, kVersionHint2);
    addFloat (layout, reverb3Mix,     "Reverb 3 Mix",     { 0.0f, 1.0f, 0.001f }, 0.25f, {}, kVersionHint2);

    // --- amp tone stack (inside the amp block, after the model)
    addBool  (layout, ampEqOn,     "Amp EQ On",     true, kVersionHint2);
    addFloat (layout, ampEqBass,   "Amp EQ Bass",   { 0.0f, 10.0f, 0.1f }, 5.0f, {}, kVersionHint2);
    addFloat (layout, ampEqMid,    "Amp EQ Middle", { 0.0f, 10.0f, 0.1f }, 5.0f, {}, kVersionHint2);
    addFloat (layout, ampEqTreble, "Amp EQ Treble", { 0.0f, 10.0f, 0.1f }, 5.0f, {}, kVersionHint2);

    return layout;
}
} // namespace params
