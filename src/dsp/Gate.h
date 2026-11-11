#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

// The noise gate block (docs/FX-ENGINES.md §4.1). One engine, three modes:
//
//   Classic   AudioDSPTools' trigger + gain, exactly as before (mode 0, frozen).
//   Tight     windowed-peak detector, 6 dB hysteresis, dB-linear release to range.
//   Smooth    Tight's detector and state machine; closed = 3:1 downward expander on a
//             steadied estimate of the floor, which its gain follows at most 1 dB per 10 ms.
//
// The closing rule (Tight and Smooth). What closes the gate is how fast the level fell through
// the hysteresis band, not how far it fell, because the noise floor decides how far it can fall.
// The level is the 12 ms windowed peak of the lower of two readings: the 4th-order sidechain
// filter and a one-pole twin raised by its largest shortfall, so the filter's ringing after a
// loud low note holds nothing open. A fall from the open level to the close level within 50 ms
// is a stop, at any floor depth (a hand stop takes 1 to 44 ms): the gate waits the hold, plus
// the length of any fast dip the level came back from with the gate open in the last moments
// (a chord's beating partials, which the window cannot fill; at most 20 ms), then releases at the
// release rate. A slower fall is a decay or a beat trough: the gate waits twice as long as the
// fall took (at most 1 s), and starts that wait again whenever the level climbs 6 dB back from
// its lowest point. A beat trough that falls as fast as a stop cannot be told from one: the gate
// reaches -20 dB on the stop's timing, then, if it remembers such troughs, falls only 20 dB per
// 1.5 remembered trough lengths, so the trough costs a partial dip and the crest reopens it.
// Opening follows the onset: version 2's 0.2 ms linear rise for a pick, up to a 5 ms
// smoothstep for a swell (half the time the level took to climb the band).
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
    float scHpfHz = 75.0f;      // [20, 300], 4th-order Butterworth; <= 20.5 means off (kGateScHpfDefault[Tight])
};

class GateFx
{
public:
    /** Order matches the gate_mode choice list and is FROZEN: reordering it silently
        repoints every saved session. Append only. */
    enum class Mode { classic = 0, tight = 1, smooth = 2 };
    static constexpr int numModes = 3;
    static constexpr int numTypes = numModes;
    /** The largest curve delay honoured: the processor's lane rings hold 65 536 samples, so no
        span it can align is longer. A larger request is clamped to this. */
    static constexpr int kMaxCurveDelaySamples = 65535;

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
        Does not touch the audio. Blocks larger than maxBlockSize: no curve (pass ungated);
        the detector and the meters still follow them (Classic reads open, 0 dB). */
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
/** Hold in ms: how long the gate stays open after a note stops (its level falls from the open to
    the close level within 50 ms), on top of the 12 ms detector window, at any floor depth. Slow
    fades and beat troughs are ridden by the closing rule whatever the hold, so it stays short
    for palm mutes: with 6 ms every 50 ms gap closes over floors 12 to 40 dB under the notes.
    Smooth keeps 10 ms. */
inline constexpr float kGateHoldDefault[GateFx::numModes] { 6.0f, 6.0f, 10.0f };
/** Sidechain high-pass corner in Hz (4th-order Butterworth): a low E reads 1.7 dB under its
    level, 50 / 60 Hz hum 14 / 8 dB. Was 90 Hz at 2nd order (3.9 dB; 11 / 8 dB). A guitar
    default: for bass use the knob's minimum (exactly off) or about 40 Hz (check_gate F rows). */
inline constexpr float kGateScHpfDefault[GateFx::numModes] { 75.0f, 75.0f, 75.0f };
} // namespace tubamp
