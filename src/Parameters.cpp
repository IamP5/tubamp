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

/** One new hint per shipped batch, never reusing a released one (docs/REVERB.md §5.3):
    hint 3 carries the reverb engine's twelve new parameters x3 instances. Adding them
    at hint 2 would interleave them among the shipped v2 block by hash and shuffle
    Logic's automation menu. */
constexpr int kVersionHint3 = 3;

/** Hint 4 carries Stage 3's Shimmer pair x3 instances (docs/REVERB.md §5.3). Nothing has
    shipped at hint 3 yet either, but the rule is one hint per BATCH, not per release:
    reusing 3 would interleave the shimmer ids among the twelve batch-1 ids by hash the
    day both are in a host's automation menu. */
constexpr int kVersionHint4 = 4;

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

    // --- stereo chain (docs/STEREO.md): stereo delay modes, reverb width. Appended for
    // the same reason as everything else above — these are new params on blocks whose
    // instance-1 ids are already frozen at hint 1. (The dual-NAM amp part of that spec
    // is superseded by amp2 as a chain block — see docs/SPLIT.md — so there is no
    // amp_stereo param here.)

    // Delay mode/ratio/width, all three instances. Choice list is frozen once shipped
    // (see delayModeChoices); ratio and width apply the same way to every instance.
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { delayMode, kVersionHint2 }, "Delay Mode", delayModeChoices, 0));
    addFloat (layout, delayRatio, "Delay Ratio", { 25.0f, 200.0f, 0.1f }, 100.0f, "%", kVersionHint2);
    addFloat (layout, delayWidth, "Delay Width", { 0.0f, 1.0f, 0.001f },   1.0f, {},   kVersionHint2);

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { delay2Mode, kVersionHint2 }, "Delay 2 Mode", delayModeChoices, 0));
    addFloat (layout, delay2Ratio, "Delay 2 Ratio", { 25.0f, 200.0f, 0.1f }, 100.0f, "%", kVersionHint2);
    addFloat (layout, delay2Width, "Delay 2 Width", { 0.0f, 1.0f, 0.001f },   1.0f, {},   kVersionHint2);

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { delay3Mode, kVersionHint2 }, "Delay 3 Mode", delayModeChoices, 0));
    addFloat (layout, delay3Ratio, "Delay 3 Ratio", { 25.0f, 200.0f, 0.1f }, 100.0f, "%", kVersionHint2);
    addFloat (layout, delay3Width, "Delay 3 Width", { 0.0f, 1.0f, 0.001f },   1.0f, {},   kVersionHint2);

    // Reverb width, all three instances. Default 1 matches ReverbFx's current hardcoded
    // width so old sessions render bit-identical.
    addFloat (layout, reverbWidth,  "Reverb Width",   { 0.0f, 1.0f, 0.001f }, 1.0f, {}, kVersionHint2);
    addFloat (layout, reverb2Width, "Reverb 2 Width", { 0.0f, 1.0f, 0.001f }, 1.0f, {}, kVersionHint2);
    addFloat (layout, reverb3Width, "Reverb 3 Width", { 0.0f, 1.0f, 0.001f }, 1.0f, {}, kVersionHint2);

    // --- split path (docs/SPLIT.md): amp2 (engine B) plus the split/mix fork-and-join
    // pair. Appended for the same reason as everything above — new params, hint 2.
    //
    // amp2_input/amp2_output copy amp_input/amp_output's range and default verbatim: a
    // second engine's gain staging must feel identical to the first's.
    addBool  (layout, amp2On,     "Amp 2 On",     true, kVersionHint2);
    addFloat (layout, amp2Input,  "Amp 2 Input",  { -20.0f, 20.0f, 0.1f }, 0.0f, "dB", kVersionHint2);
    addFloat (layout, amp2Output, "Amp 2 Output", { -40.0f, 40.0f, 0.1f }, 0.0f, "dB", kVersionHint2);

    addBool (layout, splitOn, "Split On", true, kVersionHint2);
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { splitMode, kVersionHint2 }, "Split Mode", splitModeChoices, 0));
    addFloat (layout, splitXover, "Split X-Over", logRange (100.0f, 4000.0f, 1.0f), 800.0f, "Hz", kVersionHint2);

    addBool  (layout, mixOn,     "Mix On",      true,  kVersionHint2);
    addFloat (layout, mixALevel, "Mix A Level", { -60.0f, 12.0f, 0.1f }, 0.0f, "dB", kVersionHint2);
    addFloat (layout, mixBLevel, "Mix B Level", { -60.0f, 12.0f, 0.1f }, 0.0f, "dB", kVersionHint2);
    addFloat (layout, mixAPan,   "Mix A Pan",   { -1.0f, 1.0f, 0.001f }, 0.0f, {},   kVersionHint2);
    addFloat (layout, mixBPan,   "Mix B Pan",   { -1.0f, 1.0f, 0.001f }, 0.0f, {},   kVersionHint2);
    addBool  (layout, mixBPhase, "Mix B Phase", false, kVersionHint2);
    addFloat (layout, mixLevel,  "Mix Level",   { -60.0f, 12.0f, 0.1f }, 0.0f, "dB", kVersionHint2);

    // --- reverb engine, batch 1 (docs/REVERB.md §5): twelve new parameters x3
    // instances at hint 3. reverb_size/damping/mix/width keep their ids, ranges and
    // defaults above and now drive the same engine.
    //
    // Grouped instance-major, in the spec's own parameter order within each instance —
    // the same shape as the delay stereo block above. reverb_algo is the compatibility
    // keystone (§5.3): default 0 = Room, so a state that never mentions it resolves to
    // Room on both restore paths. Its choice list is FROZEN at six entries, the two
    // unshipped modes included, so AudioParameterChoice's index/(numChoices-1)
    // normalisation never moves — see reverbAlgoChoices.

    // Decay is the mid-band T60, clamped at DSP level to the mode ceiling (§3.3); in
    // Reverse it is the window length and the UI relabels it WINDOW (§3.12). Pre-delay
    // is a musical delay and never reaches computeChainLatency. Both send filters
    // default to fully open, and Color defaults to neutral, so nothing an existing
    // preset stored changes tone through them. Bass Mult is a decay MULTIPLIER (Jot's
    // parameterisation), log so that x1.0 -- the geometric mean of x0.25 and x4.0, i.e.
    // no low-band shift -- sits at the centre of the travel.
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { reverbAlgo, kVersionHint3 }, "Reverb Algorithm", reverbAlgoChoices, 0));
    addFloat (layout, reverbDecay,      "Reverb Decay",        logRange (0.2f, 30.0f, 0.01f), 2.0f, "s", kVersionHint3);
    addFloat (layout, reverbPredelay,   "Reverb Pre-Delay",    { 0.0f, 250.0f, 0.1f }, 0.0f, "ms", kVersionHint3);
    addFloat (layout, reverbDiffusion,  "Reverb Diffusion",    { 0.0f, 1.0f, 0.001f }, 0.7f, {}, kVersionHint3);
    addFloat (layout, reverbLowCut,     "Reverb Low Cut",      logRange (20.0f, 800.0f, 1.0f), 20.0f, "Hz", kVersionHint3);
    addFloat (layout, reverbHighCut,    "Reverb High Cut",     logRange (1200.0f, 20000.0f, 1.0f), 20000.0f, "Hz", kVersionHint3);
    addFloat (layout, reverbMod,        "Reverb Mod",          { 0.0f, 1.0f, 0.001f }, 0.35f, {}, kVersionHint3);
    addFloat (layout, reverbBassMult,   "Reverb Bass Mult",    logRange (0.25f, 4.0f, 0.01f), 1.0f, "x", kVersionHint3);
    addFloat (layout, reverbErLevel,    "Reverb ER Level",     { 0.0f, 1.0f, 0.001f }, 0.5f, {}, kVersionHint3);
    addFloat (layout, reverbColor,      "Reverb Color",        { 0.0f, 1.0f, 0.001f }, 0.0f, {}, kVersionHint3);
    addFloat (layout, reverbTilt,       "Reverb Tilt",         { -1.0f, 1.0f, 0.001f }, 0.0f, {}, kVersionHint3);
    addFloat (layout, reverbDuck,       "Reverb Duck",         { 0.0f, 1.0f, 0.001f }, 0.0f, {}, kVersionHint3);

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { reverb2Algo, kVersionHint3 }, "Reverb 2 Algorithm", reverbAlgoChoices, 0));
    addFloat (layout, reverb2Decay,     "Reverb 2 Decay",      logRange (0.2f, 30.0f, 0.01f), 2.0f, "s", kVersionHint3);
    addFloat (layout, reverb2Predelay,  "Reverb 2 Pre-Delay",  { 0.0f, 250.0f, 0.1f }, 0.0f, "ms", kVersionHint3);
    addFloat (layout, reverb2Diffusion, "Reverb 2 Diffusion",  { 0.0f, 1.0f, 0.001f }, 0.7f, {}, kVersionHint3);
    addFloat (layout, reverb2LowCut,    "Reverb 2 Low Cut",    logRange (20.0f, 800.0f, 1.0f), 20.0f, "Hz", kVersionHint3);
    addFloat (layout, reverb2HighCut,   "Reverb 2 High Cut",   logRange (1200.0f, 20000.0f, 1.0f), 20000.0f, "Hz", kVersionHint3);
    addFloat (layout, reverb2Mod,       "Reverb 2 Mod",        { 0.0f, 1.0f, 0.001f }, 0.35f, {}, kVersionHint3);
    addFloat (layout, reverb2BassMult,  "Reverb 2 Bass Mult",  logRange (0.25f, 4.0f, 0.01f), 1.0f, "x", kVersionHint3);
    addFloat (layout, reverb2ErLevel,   "Reverb 2 ER Level",   { 0.0f, 1.0f, 0.001f }, 0.5f, {}, kVersionHint3);
    addFloat (layout, reverb2Color,     "Reverb 2 Color",      { 0.0f, 1.0f, 0.001f }, 0.0f, {}, kVersionHint3);
    addFloat (layout, reverb2Tilt,      "Reverb 2 Tilt",       { -1.0f, 1.0f, 0.001f }, 0.0f, {}, kVersionHint3);
    addFloat (layout, reverb2Duck,      "Reverb 2 Duck",       { 0.0f, 1.0f, 0.001f }, 0.0f, {}, kVersionHint3);

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { reverb3Algo, kVersionHint3 }, "Reverb 3 Algorithm", reverbAlgoChoices, 0));
    addFloat (layout, reverb3Decay,     "Reverb 3 Decay",      logRange (0.2f, 30.0f, 0.01f), 2.0f, "s", kVersionHint3);
    addFloat (layout, reverb3Predelay,  "Reverb 3 Pre-Delay",  { 0.0f, 250.0f, 0.1f }, 0.0f, "ms", kVersionHint3);
    addFloat (layout, reverb3Diffusion, "Reverb 3 Diffusion",  { 0.0f, 1.0f, 0.001f }, 0.7f, {}, kVersionHint3);
    addFloat (layout, reverb3LowCut,    "Reverb 3 Low Cut",    logRange (20.0f, 800.0f, 1.0f), 20.0f, "Hz", kVersionHint3);
    addFloat (layout, reverb3HighCut,   "Reverb 3 High Cut",   logRange (1200.0f, 20000.0f, 1.0f), 20000.0f, "Hz", kVersionHint3);
    addFloat (layout, reverb3Mod,       "Reverb 3 Mod",        { 0.0f, 1.0f, 0.001f }, 0.35f, {}, kVersionHint3);
    addFloat (layout, reverb3BassMult,  "Reverb 3 Bass Mult",  logRange (0.25f, 4.0f, 0.01f), 1.0f, "x", kVersionHint3);
    addFloat (layout, reverb3ErLevel,   "Reverb 3 ER Level",   { 0.0f, 1.0f, 0.001f }, 0.5f, {}, kVersionHint3);
    addFloat (layout, reverb3Color,     "Reverb 3 Color",      { 0.0f, 1.0f, 0.001f }, 0.0f, {}, kVersionHint3);
    addFloat (layout, reverb3Tilt,      "Reverb 3 Tilt",       { -1.0f, 1.0f, 0.001f }, 0.0f, {}, kVersionHint3);
    addFloat (layout, reverb3Duck,      "Reverb 3 Duck",       { 0.0f, 1.0f, 0.001f }, 0.0f, {}, kVersionHint3);

    // --- reverb engine, batch 2 (docs/REVERB.md §5, Stage 3): Shimmer, two parameters
    // x3 instances at hint 4, grouped per instance like the batch-1 block above so a
    // host's list keeps each reverb's level and interval adjacent. reverb_shimmer's 0
    // default is load-bearing twice over: it is the absent-means-default answer for every
    // state written before this batch (§5.3), and it is the value at which the shifter is
    // not touched at all, so Shimmer at 0 renders exactly as Hall does (§3.11).
    addFloat (layout, reverbShimmer, "Reverb Shimmer", { 0.0f, 1.0f, 0.001f }, 0.0f, {}, kVersionHint4);
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { reverbShimmerInterval, kVersionHint4 }, "Reverb Shimmer Interval",
        reverbShimmerIntervalChoices, 2));

    addFloat (layout, reverb2Shimmer, "Reverb 2 Shimmer", { 0.0f, 1.0f, 0.001f }, 0.0f, {}, kVersionHint4);
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { reverb2ShimmerInterval, kVersionHint4 }, "Reverb 2 Shimmer Interval",
        reverbShimmerIntervalChoices, 2));

    addFloat (layout, reverb3Shimmer, "Reverb 3 Shimmer", { 0.0f, 1.0f, 0.001f }, 0.0f, {}, kVersionHint4);
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { reverb3ShimmerInterval, kVersionHint4 }, "Reverb 3 Shimmer Interval",
        reverbShimmerIntervalChoices, 2));

    return layout;
}
} // namespace params
