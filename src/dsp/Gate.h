#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

// The noise gate block (docs/FX-ENGINES.md §4.1). One engine, three modes:
//
//   Classic   AudioDSPTools' trigger + gain, exactly as before (mode 0, frozen).
//   Tight     windowed-peak detector, 6 dB hysteresis, hold, dB-linear release to range.
//   Smooth    Tight's detector and state machine; closed = 3:1 downward expander.
//
// The gate measures where it sits and may apply its gain further down the chain (on the
// amp's mono output), so it splits the usual process() into measure() and apply().
namespace tubamp
{
/** Everything a gate needs for one block, in the units Parameters.h documents. */
struct GateParams
{
    int mode = 1;               // GateFx::Mode
    float thresholdDb = -80.0f; // [-100, 0]
    float releaseMs = 30.0f;    // [5, 500] time for 60 dB of gain change
    float holdMs = 6.0f;        // [0, 250] (kGateHoldDefault[Tight])
    float rangeDb = -90.0f;     // [-90, 0]; <= -89.95 means mute (gain exactly 0)
    float scHpfHz = 90.0f;      // [20, 300]; <= 20.5 means off
};

class GateFx
{
public:
    /** Order matches the gate_mode choice list and is FROZEN: reordering it silently
        repoints every saved session. Append only. */
    enum class Mode { classic = 0, tight = 1, smooth = 2 };
    static constexpr int numModes = 3;
    static constexpr int numTypes = numModes;
    static constexpr int kMaxCurveDelaySamples = 8192;

    GateFx();
    ~GateFx();

    /** Sizes the curve ring = maxBlock + kMaxCurveDelaySamples. */
    void prepare (const juce::dsp::ProcessSpec& spec);
    /** Tight/Smooth start closed and snap to the requested mode. Classic only drops its
        curve, as it always did (its trigger keeps its state). */
    void reset();
    void setParameters (const GateParams& params);
    /** Tight/Smooth only: delay applied to the curve when it is applied elsewhere (deferred
        onto the amp output). Clamped to [0, kMaxCurveDelaySamples]. Classic ignores it. */
    void setCurveDelay (int samples) noexcept;
    /** Analyses `block` (mean of its channels) and computes this block's gain curve.
        Does not touch the audio. Blocks larger than maxBlockSize: no curve (pass ungated). */
    void measure (const juce::dsp::AudioBlock<float>& block, int numSamples);
    /** In place, every channel, at the gate's own position: never delayed. A no-op unless
        the last measure() made a curve of exactly numSamples. Changes no state. */
    void apply (juce::dsp::AudioBlock<float> block, int numSamples);
    /** Deferred, one buffer (the amp's mono output): Tight/Smooth read the curve
        setCurveDelay() samples late. A no-op unless the last measure() made a curve of
        exactly numSamples. Changes no state. */
    void apply (float* mono, int numSamples);

    int getLatencySamples() const noexcept { return 0; }
    /** 0 closed, 1 hold, 2 open, at the end of the last measure(). */
    int getState() const noexcept;
    /** Max over the last measured block, dB >= 0. */
    float getGainReductionDb() const noexcept;
    /** Max detector level over the last block, dBFS (floor -120). */
    float getDetectorLevelDb() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GateFx)
};

// Where release and hold start for each mode, indexed by GateFx::Mode. A parameter has one
// default, which is the default mode's entry; the editor applies the others when the user
// picks a mode (docs/FX-ENGINES.md §2.7.4). Classic's entry is unused.

/** Release in ms for 60 dB: short for the hard gate, longer for the expander. */
inline constexpr float kGateReleaseDefault[GateFx::numModes] { 30.0f, 30.0f, 80.0f };
/** Hold in ms. Tight closes once nothing has crossed the close level for window (12 ms) +
    hold: 6 ms is the shortest span that rides a 41 Hz note's decay without chattering, and
    it mutes 50 ms palm-mute gaps to -38 dB (10 ms reaches only -30). Smooth keeps 10 ms. */
inline constexpr float kGateHoldDefault[GateFx::numModes] { 6.0f, 6.0f, 10.0f };
} // namespace tubamp
