#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Single source of truth for every parameter in the chain. Default order:
// Trim -> Gate trigger -> Comp -> Drive -> NAM -> Gate gain -> Cab IR -> Tone stack
//   -> Mod -> Delay -> Reverb -> DC blocker -> Out
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

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

inline const juce::StringArray modTypeChoices { "Chorus", "Phaser", "Tremolo" };

/** Output-gain compensation modes, in the official plugin's index order. */
enum class OutputMode { raw = 0, normalized = 1, calibrated = 2 };

inline const juce::StringArray ampOutModeChoices { "Raw", "Normalized", "Calibrated" };
} // namespace params
