#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

// The compressor block (docs/FX-ENGINES.md §4.2). One engine, five types:
//
//   Classic   juce::dsp::Compressor + smoothed makeup, exactly as before (type 0, frozen).
//   Clean     log-domain feed-forward, gain smoothed after the gain computer, linked.
//   Squash    log-domain feedback: the gain computer sees the output level (pedal style).
//   FET       Clean's detector with a two-stage release and a ratio-dependent threshold.
//   Opto      Clean's detector with a slow two-stage release and a wide knee.
//
// FET and Opto release more slowly after long, deep compression (programme dependence).
// A type change between Clean, Squash, FET and Opto hands the gain state over (no fade);
// a change to or from Classic crossfades the two outputs.
//
// Squash's release is the knob (t63) when the signal falls below the threshold, and while
// the output stays above it. A drop that leaves the input above the threshold but takes
// the output below it recovers sooner, because the open loop heads for 0 dB: that is the
// feedback character, and what sets Squash apart from Clean, whose recovery is the knob
// for every drop. Measured t63 at knob 150 ms (Clean: 149 ms in every cell), T -24,
// knee 6, from -4 dBFS:
//
//              to -8   -10    -14    -18    -22 dBFS
//     R 2      149    149    142    111    127
//     R 4      149    121     75     93    127
//     R 8       83     53     65     92    127
//     R 20      38     44     64     92    127
//
// The gain-reduction meter shows what the compressor applies to its wet path, as a
// hardware meter does: with mix under 100 % the net change heard is smaller. During a
// crossfade to or from Classic it shows the two engines' reductions blended with the fade
// gains, which is what the output gets.
namespace tubamp
{
/** Everything a compressor instance needs for one block, in the units Parameters.h documents. */
struct CompParams
{
    int type = 1;                 // CompFx::Type; out-of-range values clamp
    float thresholdDb = -20.0f;   // [-60, 0]
    float ratio = 4.0f;           // [1, 20]
    float attackMs = 5.0f;        // [0.1, 100]
    float releaseMs = 120.0f;     // [10, 1000]
    float makeupDb = 0.0f;        // [0, 24] manual makeup, added to auto makeup (total <= 48)
    float kneeDb = 6.0f;          // [0, 24] total knee width (kCompKneeDefault)
    float mixPct = 100.0f;        // [0, 100] parallel mix, 100 = fully compressed
    float scHpfHz = 20.0f;        // [20, 300] detector high-pass; <= 20.5 = off
    /** Adds -gc(-10 dBFS) to the makeup: a fixed gain, calibrated so that a steady signal
        peaking at -10 dBFS comes out at the level it went in. It does not follow the
        playing: a steady -20 dBFS signal comes out 4 to 10 dB louder, a -3 dBFS one 3.5 to
        7 dB quieter, and a plucked chord peaking at -10 dBFS 2 to 12 dB louder in rms (its
        decay is reduced less than its peak). */
    bool autoMakeup = false;
};

class CompFx
{
public:
    /** Order matches the comp_type choice list and is FROZEN: reordering it silently
        repoints every saved session. Append only. */
    enum class Type { classic = 0, clean = 1, squash = 2, fet = 3, opto = 4 };
    static constexpr int numTypes = 5;

    CompFx();
    ~CompFx();

    void prepare (const juce::dsp::ProcessSpec& spec);
    /** Clears detector and gain state. Does NOT touch the makeup smoother (Classic keeps
        today's behaviour: only prepare() re-primes it). */
    void reset();
    /** Audio thread, once per block before process() or skip(). */
    void setParameters (const CompParams& params);
    void process (juce::dsp::AudioBlock<float> block);
    /** Audio thread: the block is bypassed or absent this callback. Keeps the makeup
        smoother in step exactly like the old processor code did. */
    void skip (int numSamples) noexcept;

    int getLatencySamples() const noexcept { return 0; }
    /** Largest gain reduction applied during the last process(), dB >= 0, makeup excluded. */
    float getGainReductionDb() const noexcept;

    /** Test hook (fxbench --trace): while non-null, process() writes one gain-reduction
        value per sample (dB >= 0, makeup excluded) to it. The processor never sets it. */
    void setGainTrace (float* destination) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompFx)
};

// Where three knobs start for each type, indexed by CompFx::Type. A parameter has one
// default, which is the default type's entry; the editor applies the others when the user
// picks a type (docs/FX-ENGINES.md §2.7.4). Classic's entries equal the frozen defaults.

/** The knee is what most separates the characters: hard-ish for FET, very soft for Opto. */
inline constexpr float kCompKneeDefault[CompFx::numTypes]    { 6.0f, 6.0f, 6.0f, 4.0f, 12.0f };
/** Attack in ms: the knob means the same physical time in every type, but each type lives
    in a different range of it. */
inline constexpr float kCompAttackDefault[CompFx::numTypes]  { 5.0f, 5.0f, 2.5f, 0.8f, 10.0f };
/** Release in ms (FET/Opto: the fast stage; the slow stage follows it). */
inline constexpr float kCompReleaseDefault[CompFx::numTypes] { 120.0f, 120.0f, 150.0f, 55.0f, 60.0f };
} // namespace tubamp
