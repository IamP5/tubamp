#include "Parameters.h"

#include "dsp/Chorus.h"
#include "dsp/Compressor.h"
#include "dsp/Delay.h"
#include "dsp/Drive.h"
#include "dsp/Eq.h"
#include "dsp/Gate.h"
#include "dsp/Phaser.h"
#include "dsp/Reverb.h"
#include "dsp/Tremolo.h"

namespace params
{
// The engines index their enums and kDelaySyncBeats by these choice lists.
static_assert (std::size (delayTypeNames) == (size_t) tubamp::DelayFx::numTypes,
               "delay_type choices must match DelayFx::Type");
static_assert (std::size (reverbTypeNames) == (size_t) tubamp::ReverbFx::numTypes,
               "reverb_type choices must match ReverbFx::Type");
static_assert (std::size (delaySyncNames) == (size_t) tubamp::kNumDelaySyncChoices,
               "delay_sync choices must match kDelaySyncBeats");

static_assert (std::size (gateModeNames) == (size_t) tubamp::GateFx::numTypes,
               "gate_mode choices must match GateFx::Mode");
static_assert (std::size (compTypeNames) == (size_t) tubamp::CompFx::numTypes,
               "comp_type choices must match CompFx::Type");
static_assert (std::size (driveTypeNames) == (size_t) tubamp::DriveFx::numTypes,
               "drive_type choices must match DriveFx::Type");
static_assert (std::size (eqTypeNames) == (size_t) tubamp::EqFx::numTypes,
               "eq_type choices must match EqFx::Type");
static_assert (std::size (eqVoiceNames) == (size_t) tubamp::EqFx::numVoices,
               "eq_voice choices must match EqFx::Voice");
static_assert (std::size (modChorusModeNames) == (size_t) tubamp::ChorusFx::numTypes,
               "mod_chorus_mode choices must match ChorusFx::Mode");
static_assert (std::size (modPhaserModelNames) == (size_t) tubamp::PhaserFx::numTypes,
               "mod_phaser_model choices must match PhaserFx::Model");
static_assert (std::size (modTremVoiceNames) == (size_t) tubamp::TremoloFx::numTypes,
               "mod_trem_voice choices must match TremoloFx::Voice");

// Damping starts where the default reverb type's table entry puts it (the editor
// applies the other types' entries when one is picked). reverb_damping is a v1
// parameter whose default has always been 0.5, which is why Plate, the default type,
// is the type the knob's centre is calibrated to.
constexpr float kReverbDampingAtDefaultType = tubamp::kReverbDampingDefault[(int) tubamp::ReverbFx::Type::plate];
static_assert (kReverbDampingAtDefaultType == 0.5f, "reverb_damping's default is frozen at 0.5");

// The new parameters' defaults are the engines' own (docs/FX-ENGINES.md §2.7.3-4): each
// Params struct starts on the recommended type, knobs with a per-type table take that
// type's entry, and the rest take the struct's starting value. tests/golden/params-v4.tsv
// records the result, so a later change to an engine header shows up in that diff.
constexpr tubamp::GateParams kGateStart {};
constexpr tubamp::CompParams kCompStart {};
constexpr tubamp::EqParams kEqStart {};
constexpr tubamp::ChorusParams kChorusStart {};
constexpr tubamp::PhaserParams kPhaserStart {};
constexpr tubamp::TremoloParams kTremStart {};

static_assert (kGateStart.mode == (int) tubamp::GateFx::Mode::tight, "gate_mode starts on Tight");
static_assert (kCompStart.type == (int) tubamp::CompFx::Type::clean, "comp_type starts on Clean");
static_assert (kEqStart.type == (int) tubamp::EqFx::Type::parametric, "eq_type starts on Parametric");
static_assert (kEqStart.voice == (int) tubamp::EqFx::Voice::british, "eq_voice starts on British");
static_assert (kChorusStart.mode == (int) tubamp::ChorusFx::Mode::wide, "mod_chorus_mode starts on Wide");
static_assert (kPhaserStart.model == (int) tubamp::PhaserFx::Model::phase90, "mod_phaser_model starts on Phase 90");
static_assert (kTremStart.voice == (int) tubamp::TremoloFx::Voice::smooth, "mod_trem_voice starts on Smooth");

namespace
{
constexpr int kVersionHint = 1;

/** Hint carried by every parameter added after the original layout. JUCE sorts an AU's
    parameter list by version hint before anything else, so hint-2 ids land after every
    hint-1 id whatever their hash — the list a host already knows keeps its order and
    the additions append to it. The AU parameter ID is a hash of the string id and does
    not depend on the hint, so saved automation is unaffected either way. */
constexpr int kVersionHint2 = 2;

/** Hint for parameters added after the hint-2 set shipped. Within one hint the AU list
    is sorted by id hash, so adding to hint 2 would interleave the new ids among the
    shipped hint-2 ones and shift their AU indices; a higher hint sorts the whole group
    after them instead. Every later release that adds parameters needs the next number. */
constexpr int kVersionHint3 = 3;

/** Hint for the gate, compressor, drive, EQ and mod engine parameters, added after the
    hint-3 set (reverb/delay engines) shipped — the same reason as kVersionHint3: a fresh
    hint sorts the whole group after every shipped id in the AU parameter list. */
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

void addChoice (Layout& layout, const char* id, const juce::String& name,
                const juce::StringArray& choices, int defaultIndex, int versionHint)
{
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { id, versionHint }, name, choices, defaultIndex));
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
    addFloat (layout, reverbDamping, "Reverb Damping", { 0.0f, 1.0f, 0.001f }, kReverbDampingAtDefaultType);
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
    addFloat (layout, reverb2Damping, "Reverb 2 Damping", { 0.0f, 1.0f, 0.001f }, kReverbDampingAtDefaultType,  {}, kVersionHint2);
    addFloat (layout, reverb2Mix,     "Reverb 2 Mix",     { 0.0f, 1.0f, 0.001f }, 0.25f, {}, kVersionHint2);

    addBool  (layout, reverb3On,      "Reverb 3 On",      true, kVersionHint2);
    addFloat (layout, reverb3Size,    "Reverb 3 Size",    { 0.0f, 1.0f, 0.001f }, 0.5f,  {}, kVersionHint2);
    addFloat (layout, reverb3Damping, "Reverb 3 Damping", { 0.0f, 1.0f, 0.001f }, kReverbDampingAtDefaultType,  {}, kVersionHint2);
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

    // --- reverb and delay engine types (docs/REVERB-DELAY.md §1), hint 3: added after
    // the hint-2 set shipped. Delay instances 1-3, then reverb instances 1-3, each in
    // the spec's per-instance order. Knobs with a per-type starting point (delay high
    // cut, color and duck; reverb decay) default to the default type's table entry; the
    // editor applies the other types' entries when one is picked.
    const char* const delayNames[maxInstances]  { "Delay", "Delay 2", "Delay 3" };
    const char* const reverbNames[maxInstances] { "Reverb", "Reverb 2", "Reverb 3" };

    for (int k = 0; k < maxInstances; ++k)
    {
        const juce::String name { delayNames[k] };

        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { delayTypeIds[k], kVersionHint3 }, name + " Type", delayTypeChoices,
            (int) tubamp::DelayFx::Type::digital));
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { delaySyncIds[k], kVersionHint3 }, name + " Sync", delaySyncChoices, 0));
        addFloat (layout, delayLowCutIds[k],  name + " Low Cut",  logRange (20.0f, 1000.0f, 1.0f),    80.0f,  "Hz", kVersionHint3);
        addFloat (layout, delayHighCutIds[k], name + " High Cut", logRange (1000.0f, 20000.0f, 1.0f),
                  tubamp::kDelayHighCutDefault[(int) tubamp::DelayFx::Type::digital], "Hz", kVersionHint3);
        addFloat (layout, delayModAmtIds[k],  name + " Mod",      { 0.0f, 1.0f, 0.001f },             0.2f,  {},   kVersionHint3);
        addFloat (layout, delayColorIds[k],   name + " Color",    { 0.0f, 1.0f, 0.001f },
                  tubamp::kDelayColorDefault[(int) tubamp::DelayFx::Type::digital], {}, kVersionHint3);
        addFloat (layout, delayDuckIds[k],    name + " Duck",     { 0.0f, 1.0f, 0.001f },
                  tubamp::kDelayDuckDefault[(int) tubamp::DelayFx::Type::digital], {}, kVersionHint3);
    }

    for (int k = 0; k < maxInstances; ++k)
    {
        const juce::String name { reverbNames[k] };

        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { reverbTypeIds[k], kVersionHint3 }, name + " Type", reverbTypeChoices,
            (int) tubamp::ReverbFx::Type::plate));
        addFloat (layout, reverbPreDelayIds[k], name + " Pre-Delay", { 0.0f, 250.0f, 0.1f },             10.0f,  "ms", kVersionHint3);
        addFloat (layout, reverbDecayIds[k],    name + " Decay",     logRange (0.2f, 20.0f, 0.01f),
                  tubamp::kReverbDecayDefault[(int) tubamp::ReverbFx::Type::plate], "s", kVersionHint3);
        addFloat (layout, reverbLowCutIds[k],   name + " Low Cut",   logRange (20.0f, 1000.0f, 1.0f),    120.0f, "Hz", kVersionHint3);
        addFloat (layout, reverbHighCutIds[k],  name + " High Cut",  logRange (1000.0f, 20000.0f, 1.0f), 8000.0f, "Hz", kVersionHint3);
        addFloat (layout, reverbModAmtIds[k],   name + " Mod",       { 0.0f, 1.0f, 0.001f },             0.35f,  {},   kVersionHint3);
    }

    // --- cab dry/wet. A cabinet IR wants 100 % (the default, and what every session
    // saved before this existed gets); a room or reverb IR is meant to be blended in.
    addFloat (layout, cabMix, "Cab Mix", { 0.0f, 100.0f, 0.1f }, 100.0f, "%", kVersionHint3);


    // --- gate, compressor, drive, EQ and mod engines (docs/FX-ENGINES.md §2.7.1), hint 4:
    // the gate, then comp, drive, EQ and mod instances 1-3, each in the spec's
    // per-instance order. Defaults come from the engine headers (kGateStart etc. above).
    addChoice (layout, gateMode, "Gate Mode", gateModeChoices, kGateStart.mode, kVersionHint4);
    addFloat (layout, gateRelease, "Gate Release", logRange (5.0f, 500.0f, 0.1f),
              tubamp::kGateReleaseDefault[kGateStart.mode], "ms", kVersionHint4);
    addFloat (layout, gateHold,    "Gate Hold",    { 0.0f, 250.0f, 0.1f },
              tubamp::kGateHoldDefault[kGateStart.mode], "ms", kVersionHint4);
    addFloat (layout, gateRange,   "Gate Range",   { -90.0f, 0.0f, 0.1f },         kGateStart.rangeDb, "dB", kVersionHint4);
    addFloat (layout, gateScHpf,   "Gate SC HPF",  logRange (20.0f, 300.0f, 1.0f),
              tubamp::kGateScHpfDefault[kGateStart.mode], "Hz", kVersionHint4);

    const char* const compNames[maxInstances]  { "Comp", "Comp 2", "Comp 3" };
    const char* const driveNames[maxInstances] { "Drive", "Drive 2", "Drive 3" };
    const char* const eqNames[maxInstances]    { "EQ", "EQ 2", "EQ 3" };
    const char* const modNames[maxInstances]   { "Mod", "Mod 2", "Mod 3" };

    for (int k = 0; k < maxInstances; ++k)
    {
        const juce::String name { compNames[k] };

        addChoice (layout, compTypeIds[k], name + " Type", compTypeChoices, kCompStart.type, kVersionHint4);
        addFloat (layout, compKneeIds[k],  name + " Knee",   { 0.0f, 24.0f, 0.1f },
                  tubamp::kCompKneeDefault[kCompStart.type], "dB", kVersionHint4);
        addFloat (layout, compMixIds[k],   name + " Mix",    { 0.0f, 100.0f, 0.1f },         kCompStart.mixPct,  "%",  kVersionHint4);
        addFloat (layout, compScHpfIds[k], name + " SC HPF", logRange (20.0f, 300.0f, 1.0f), kCompStart.scHpfHz, "Hz", kVersionHint4);
        addBool (layout, compAutoMakeupIds[k], name + " Auto Makeup", kCompStart.autoMakeup, kVersionHint4);
    }

    for (int k = 0; k < maxInstances; ++k)
        addChoice (layout, driveTypeIds[k], juce::String (driveNames[k]) + " Type", driveTypeChoices,
                   (int) tubamp::DriveFx::Type::screamer, kVersionHint4);

    const char* const graphicNames[numEqGraphicBands] { "100 Hz", "200 Hz", "400 Hz", "800 Hz",
                                                        "1.6 kHz", "3.2 kHz", "6.4 kHz" };

    for (int k = 0; k < maxInstances; ++k)
    {
        const juce::String name { eqNames[k] };

        addChoice (layout, eqTypeIds[k], name + " Type", eqTypeChoices, kEqStart.type, kVersionHint4);
        addFloat (layout, eqLowCutIds[k],  name + " Low Cut",  logRange (20.0f, 400.0f, 1.0f),     kEqStart.lowCutHz,  "Hz", kVersionHint4);
        addFloat (layout, eqHighCutIds[k], name + " High Cut", logRange (2000.0f, 20000.0f, 1.0f), kEqStart.highCutHz, "Hz", kVersionHint4);
        addFloat (layout, eqMidFreqIds[k], name + " Mid Freq", logRange (100.0f, 8000.0f, 1.0f),   kEqStart.midFreqHz, "Hz", kVersionHint4);
        addFloat (layout, eqMidQIds[k],    name + " Mid Q",    logRange (0.5f, 8.0f, 0.01f),       kEqStart.midQ,      {},   kVersionHint4);
        addFloat (layout, eqTiltIds[k],    name + " Tilt",     { -6.0f, 6.0f, 0.1f },              kEqStart.tiltDb,    "dB", kVersionHint4);
        addFloat (layout, eqLevelIds[k],   name + " Level",    { -12.0f, 12.0f, 0.1f },            kEqStart.levelDb,   "dB", kVersionHint4);
        addChoice (layout, eqVoiceIds[k], name + " Voice", eqVoiceChoices, kEqStart.voice, kVersionHint4);

        for (int band = 0; band < numEqGraphicBands; ++band)
            addFloat (layout, eqGraphicIds[band][k], name + " " + graphicNames[band], { -12.0f, 12.0f, 0.1f },
                      kEqStart.graphicDb[band], "dB", kVersionHint4);
    }

    for (int k = 0; k < maxInstances; ++k)
    {
        const juce::String name { modNames[k] };

        addChoice (layout, modChorusModeIds[k],  name + " Chorus Mode",  modChorusModeChoices,  kChorusStart.mode,  kVersionHint4);
        addChoice (layout, modPhaserModelIds[k], name + " Phaser Model", modPhaserModelChoices, kPhaserStart.model, kVersionHint4);
        addChoice (layout, modTremVoiceIds[k],   name + " Trem Voice",   modTremVoiceChoices,   kTremStart.voice,   kVersionHint4);
        addChoice (layout, modSyncIds[k],        name + " Sync",         modSyncChoices,        0,                  kVersionHint4);
        addFloat (layout, modWidthIds[k], name + " Width", { 0.0f, 1.0f, 0.001f }, kChorusStart.width, {}, kVersionHint4);
        addFloat (layout, modToneIds[k],  name + " Tone",  { 0.0f, 1.0f, 0.001f }, kChorusStart.tone,  {}, kVersionHint4);
        addFloat (layout, modShapeIds[k], name + " Shape", { 0.0f, 1.0f, 0.001f }, kTremStart.shape,   {}, kVersionHint4);
        addFloat (layout, modWaveIds[k],  name + " Wave",  { 0.0f, 1.0f, 0.001f }, kTremStart.wave,    {}, kVersionHint4);
        addBool (layout, modColorIds[k], name + " Color", kPhaserStart.color, kVersionHint4);
    }

    return layout;
}
} // namespace params
