#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Single source of truth for every parameter in the chain. The classic arrangement
// (chain::classicOrder(); a fresh instance starts with the amp alone):
// Trim -> Gate trigger -> Comp -> Drive -> NAM -> Gate gain -> Amp EQ -> Cab IR
//   -> Tone stack -> Mod -> Delay -> Reverb -> DC blocker -> Out
//
// The blocks between trim and the DC blocker are user-orderable and removable — that
// arrangement is plugin state, not a parameter (see src/dsp/ChainOrder.h). Each block
// keeps its automatable "*_on" bypass here; every id below is frozen for AU
// compatibility.
//
// The amp-core parameters (amp_*, eq_*, gate_threshold) mirror the official NAM
// plugin one-for-one — ranges, defaults and steps included. See
// docs/research/nam-plugin-params.md for the code-cited spec they come from.
namespace params
{
// Block enables
inline constexpr auto gateOn    = "gate_on";
inline constexpr auto compOn    = "comp_on";
inline constexpr auto driveOn   = "drive_on";
inline constexpr auto ampOn     = "amp_on";
inline constexpr auto cabOn     = "cab_on";
inline constexpr auto eqOn      = "eq_on";
inline constexpr auto modOn     = "mod_on";
inline constexpr auto delayOn   = "delay_on";
inline constexpr auto reverbOn  = "reverb_on";
// External AudioUnit slots (dsp/FxHost.h). Appended after the original nine so the
// AU parameter list only ever grows — existing indices are untouched.
inline constexpr auto fx1On     = "fx1_on";
inline constexpr auto fx2On     = "fx2_on";
inline constexpr auto fx3On     = "fx3_on";

// Input / output
inline constexpr auto inputTrim   = "input_trim";    // dB  [-24, 24]
inline constexpr auto outputLevel = "output_level";  // dB  [-60, 12]

// Gate
inline constexpr auto gateThreshold = "gate_threshold"; // dB [-100, 0]

// Compressor
inline constexpr auto compThreshold = "comp_threshold"; // dB [-60, 0]
inline constexpr auto compRatio     = "comp_ratio";     // [1, 20]
inline constexpr auto compAttack    = "comp_attack";    // ms [0.1, 100]
inline constexpr auto compRelease   = "comp_release";   // ms [10, 1000]
inline constexpr auto compMakeup    = "comp_makeup";    // dB [0, 24]

// Drive (oversampled waveshaper)
inline constexpr auto driveGain  = "drive_gain";   // dB [0, 36]
inline constexpr auto driveTone  = "drive_tone";   // Hz [500, 12000] post-shaper LPF
inline constexpr auto driveLevel = "drive_level";  // dB [-24, 12]

// Amp (NAM block) — mirrors kInputLevel / kOutputLevel / kOutputMode /
// kCalibrateInput / kInputCalibrationLevel / kSlim in the official plugin.
inline constexpr auto ampInput    = "amp_input";     // dB [-20, 20] into the model
inline constexpr auto ampOutput   = "amp_output";    // dB [-40, 40] out of the model
inline constexpr auto ampOutMode  = "amp_out_mode";  // choice: Raw | Normalized | Calibrated
inline constexpr auto ampCalInput = "amp_cal_input"; // bool: offset amp_input by the model's input level
inline constexpr auto ampCalLevel = "amp_cal_level"; // dBu [-60, 60] interface calibration level
inline constexpr auto ampSlim     = "amp_slim";      // [0, 1] A2 slimmable size: 0 = cheapest, 1 = full quality

// Cab (IR convolution)
inline constexpr auto cabLowCut  = "cab_lowcut";   // Hz [20, 500]  HPF after IR
inline constexpr auto cabHighCut = "cab_highcut";  // Hz [2000, 20000] LPF after IR

// Tone stack (NAM knob units: 0-10, 5 is noon / flat)
inline constexpr auto eqBass   = "eq_bass";    // [0, 10] low shelf 150 Hz,  4 dB per unit
inline constexpr auto eqMid    = "eq_mid";     // [0, 10] peak 425 Hz,       3 dB per unit
inline constexpr auto eqTreble = "eq_treble";  // [0, 10] high shelf 1800 Hz, 2 dB per unit

// Modulation
inline constexpr auto modType  = "mod_type";   // choice: Chorus | Phaser | Tremolo
inline constexpr auto modRate  = "mod_rate";   // Hz [0.05, 10]
inline constexpr auto modDepth = "mod_depth";  // [0, 1]
inline constexpr auto modMix   = "mod_mix";    // [0, 1]

// Delay
inline constexpr auto delayTime     = "delay_time";     // ms [20, 2000]
inline constexpr auto delayFeedback = "delay_feedback"; // [0, 0.95]
inline constexpr auto delayMix      = "delay_mix";      // [0, 1]

// Reverb
inline constexpr auto reverbSize    = "reverb_size";    // [0, 1]
inline constexpr auto reverbDamping = "reverb_damping"; // [0, 1]
inline constexpr auto reverbMix     = "reverb_mix";     // [0, 1]

//==============================================================================
// Pooled instances.
//
// Six of the nine built-in kinds may sit in the chain up to three times (see
// dsp/ChainOrder.h), and every instance needs REAL parameters — a chain block whose
// knobs a host cannot automate is not a block. Instance 1 is the frozen id above
// ("comp_threshold" is compressor 1's threshold), so presets and automation written
// before the pool existed keep working; instances 2 and 3 get the ids below, appended
// at the very end of the layout for the same reason the fx enables are.
//
// Every range, default and step below is a copy of the instance-1 parameter it mirrors:
// an added instance must be indistinguishable from the first one.
inline constexpr int maxInstances = 3; // == chain::maxInstancesPerKind

inline constexpr auto comp2On        = "comp2_on";
inline constexpr auto comp2Threshold = "comp2_threshold";
inline constexpr auto comp2Ratio     = "comp2_ratio";
inline constexpr auto comp2Attack    = "comp2_attack";
inline constexpr auto comp2Release   = "comp2_release";
inline constexpr auto comp2Makeup    = "comp2_makeup";
inline constexpr auto comp3On        = "comp3_on";
inline constexpr auto comp3Threshold = "comp3_threshold";
inline constexpr auto comp3Ratio     = "comp3_ratio";
inline constexpr auto comp3Attack    = "comp3_attack";
inline constexpr auto comp3Release   = "comp3_release";
inline constexpr auto comp3Makeup    = "comp3_makeup";

inline constexpr auto drive2On    = "drive2_on";
inline constexpr auto drive2Gain  = "drive2_gain";
inline constexpr auto drive2Tone  = "drive2_tone";
inline constexpr auto drive2Level = "drive2_level";
inline constexpr auto drive3On    = "drive3_on";
inline constexpr auto drive3Gain  = "drive3_gain";
inline constexpr auto drive3Tone  = "drive3_tone";
inline constexpr auto drive3Level = "drive3_level";

inline constexpr auto eq2On     = "eq2_on";
inline constexpr auto eq2Bass   = "eq2_bass";
inline constexpr auto eq2Mid    = "eq2_mid";
inline constexpr auto eq2Treble = "eq2_treble";
inline constexpr auto eq3On     = "eq3_on";
inline constexpr auto eq3Bass   = "eq3_bass";
inline constexpr auto eq3Mid    = "eq3_mid";
inline constexpr auto eq3Treble = "eq3_treble";

inline constexpr auto mod2On    = "mod2_on";
inline constexpr auto mod2Type  = "mod2_type";
inline constexpr auto mod2Rate  = "mod2_rate";
inline constexpr auto mod2Depth = "mod2_depth";
inline constexpr auto mod2Mix   = "mod2_mix";
inline constexpr auto mod3On    = "mod3_on";
inline constexpr auto mod3Type  = "mod3_type";
inline constexpr auto mod3Rate  = "mod3_rate";
inline constexpr auto mod3Depth = "mod3_depth";
inline constexpr auto mod3Mix   = "mod3_mix";

inline constexpr auto delay2On       = "delay2_on";
inline constexpr auto delay2Time     = "delay2_time";
inline constexpr auto delay2Feedback = "delay2_feedback";
inline constexpr auto delay2Mix      = "delay2_mix";
inline constexpr auto delay3On       = "delay3_on";
inline constexpr auto delay3Time     = "delay3_time";
inline constexpr auto delay3Feedback = "delay3_feedback";
inline constexpr auto delay3Mix      = "delay3_mix";

inline constexpr auto reverb2On      = "reverb2_on";
inline constexpr auto reverb2Size    = "reverb2_size";
inline constexpr auto reverb2Damping = "reverb2_damping";
inline constexpr auto reverb2Mix     = "reverb2_mix";
inline constexpr auto reverb3On      = "reverb3_on";
inline constexpr auto reverb3Size    = "reverb3_size";
inline constexpr auto reverb3Damping = "reverb3_damping";
inline constexpr auto reverb3Mix     = "reverb3_mix";

// Amp tone stack: the EQ that lives inside the amp block, between the model's gated
// output and the amp-out gain. Same maths and knob units as the standalone eq block
// (0-10, 5 = flat), its own parameters — a chain without an eq block still has an amp
// with tone controls.
inline constexpr auto ampEqOn     = "amp_eq_on";
inline constexpr auto ampEqBass   = "amp_eq_bass";
inline constexpr auto ampEqMid    = "amp_eq_mid";
inline constexpr auto ampEqTreble = "amp_eq_treble";

// Instance-indexed views of the ids above (index 0 = instance 1). The processor
// resolves its per-instance pointer arrays through these; the ids stay literals, so
// nothing anywhere may derive one by string manipulation.
inline constexpr const char* compOnIds[maxInstances]        { compOn,        comp2On,        comp3On };
inline constexpr const char* compThresholdIds[maxInstances] { compThreshold, comp2Threshold, comp3Threshold };
inline constexpr const char* compRatioIds[maxInstances]     { compRatio,     comp2Ratio,     comp3Ratio };
inline constexpr const char* compAttackIds[maxInstances]    { compAttack,    comp2Attack,    comp3Attack };
inline constexpr const char* compReleaseIds[maxInstances]   { compRelease,   comp2Release,   comp3Release };
inline constexpr const char* compMakeupIds[maxInstances]    { compMakeup,    comp2Makeup,    comp3Makeup };

inline constexpr const char* driveOnIds[maxInstances]    { driveOn,    drive2On,    drive3On };
inline constexpr const char* driveGainIds[maxInstances]  { driveGain,  drive2Gain,  drive3Gain };
inline constexpr const char* driveToneIds[maxInstances]  { driveTone,  drive2Tone,  drive3Tone };
inline constexpr const char* driveLevelIds[maxInstances] { driveLevel, drive2Level, drive3Level };

inline constexpr const char* eqOnIds[maxInstances]     { eqOn,     eq2On,     eq3On };
inline constexpr const char* eqBassIds[maxInstances]   { eqBass,   eq2Bass,   eq3Bass };
inline constexpr const char* eqMidIds[maxInstances]    { eqMid,    eq2Mid,    eq3Mid };
inline constexpr const char* eqTrebleIds[maxInstances] { eqTreble, eq2Treble, eq3Treble };

inline constexpr const char* modOnIds[maxInstances]    { modOn,    mod2On,    mod3On };
inline constexpr const char* modTypeIds[maxInstances]  { modType,  mod2Type,  mod3Type };
inline constexpr const char* modRateIds[maxInstances]  { modRate,  mod2Rate,  mod3Rate };
inline constexpr const char* modDepthIds[maxInstances] { modDepth, mod2Depth, mod3Depth };
inline constexpr const char* modMixIds[maxInstances]   { modMix,   mod2Mix,   mod3Mix };

inline constexpr const char* delayOnIds[maxInstances]       { delayOn,       delay2On,       delay3On };
inline constexpr const char* delayTimeIds[maxInstances]     { delayTime,     delay2Time,     delay3Time };
inline constexpr const char* delayFeedbackIds[maxInstances] { delayFeedback, delay2Feedback, delay3Feedback };
inline constexpr const char* delayMixIds[maxInstances]      { delayMix,      delay2Mix,      delay3Mix };

inline constexpr const char* reverbOnIds[maxInstances]      { reverbOn,      reverb2On,      reverb3On };
inline constexpr const char* reverbSizeIds[maxInstances]    { reverbSize,    reverb2Size,    reverb3Size };
inline constexpr const char* reverbDampingIds[maxInstances] { reverbDamping, reverb2Damping, reverb3Damping };
inline constexpr const char* reverbMixIds[maxInstances]     { reverbMix,     reverb2Mix,     reverb3Mix };

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

inline const juce::StringArray modTypeChoices { "Chorus", "Phaser", "Tremolo" };

/** Output-gain compensation modes, in the official plugin's index order. */
enum class OutputMode { raw = 0, normalized = 1, calibrated = 2 };

inline const juce::StringArray ampOutModeChoices { "Raw", "Normalized", "Calibrated" };
} // namespace params
