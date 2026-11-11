#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

// The drive block (docs/FX-ENGINES.md §4.3). One engine, six types:
//
//   Classic     2x IIR oversampling + tanh + tone LPF + level, exactly as before (type 0, frozen).
//   Screamer    TS-808 style: pre-clip high-pass, diodes in the op-amp feedback, clean path summed.
//   Distortion  RAT style: op-amp gain with two RC legs, rail limit, diode pair to ground.
//   Fuzz        Big Muff style: booster, sustain, two feedback-clipping stages, passive tone blend.
//   Boost       clean gain + one-pole high cut.
//   Treble      clean first-order high shelf.
namespace tubamp
{
/** Everything a drive instance needs for one block, in the units Parameters.h documents. */
struct DriveParams
{
    int type = 1;            // DriveFx::Type
    float gainDb = 12.0f;    // [0, 36]
    float toneHz = 4000.0f;  // [500, 12000]
    float levelDb = 0.0f;    // [-24, 12]
};

class DriveFx
{
public:
    /** Order matches the drive_type choice list and is FROZEN: reordering it silently
        repoints every saved session. Append only. */
    enum class Type { classic = 0, screamer = 1, distortion = 2, fuzz = 3, boost = 4, treble = 5 };
    static constexpr int numTypes = 6;

    DriveFx();
    ~DriveFx();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    /** Audio thread, once per block before process(). */
    void setParameters (const DriveParams& params);
    void process (juce::dsp::AudioBlock<float> block);

    /** latencyFor (current type, prepared rate). */
    int getLatencySamples() const noexcept;
    /** Pure function of (type, rate), so the message thread can ask without touching the
        audio-thread object (docs/FX-ENGINES.md §4.3.7, §6.3). */
    static int latencyFor (int type, double sampleRate) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DriveFx)
};

// Where the three knobs start for each type, indexed by DriveFx::Type. A parameter has one
// default, which is the default type's entry; the editor applies the others when the user
// picks a type (docs/FX-ENGINES.md §2.7.4). Classic's entries equal the frozen defaults.

/** Gain in dB, d = gain / 36 (Screamer: the Drive pot's position, audio taper; Distortion: the
    stage's 1 kHz gain from 0 to 62 dB, linear in d; Fuzz: the sustain divider from -78 dB to
    full, -78 (1 - d)^2 dB; see Drive.cpp). */
inline constexpr float kDriveGainDefault[DriveFx::numTypes]  { 12.0f, 12.0f, 18.0f, 18.0f, 12.0f, 12.0f };
/** Tone in Hz (Screamer, Distortion, Fuzz: T = ln (tone / 500) / ln 24 is the pot position). */
inline constexpr float kDriveToneDefault[DriveFx::numTypes]  { 4000.0f, 4000.0f, 2449.0f, 2449.0f, 12000.0f, 3000.0f };
/** Level in dB. Distortion and Fuzz (L2, L3) are measured: the level at which a -18 dBFS
    1 kHz sine at that row's gain and tone leaves at -12 dBFS peak (tools/fxbench/check_drive.py
    prints it; D16 checks it). */
inline constexpr float kDriveLevelDefault[DriveFx::numTypes] { 0.0f, 0.0f, -8.2f, 5.0f, 0.0f, 0.0f };
} // namespace tubamp
