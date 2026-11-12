#pragma once

#include <juce_dsp/juce_dsp.h>

#include "Chorus.h"
#include "Delay.h"
#include "Phaser.h"
#include "Reverb.h"
#include "Tremolo.h"

#include <memory>

// Thin wrappers over juce::dsp for the fixed chain. Each block:
//   prepare(spec) -> reset() -> setParameters(...) (audio thread, per block) -> process(context)
// Parameter setters take values already fetched from APVTS by the processor.
namespace tubamp
{
/** Fixed 5 Hz high-pass that always runs after the cab, matching the reference
    plugin's kDCBlockerFrequency stage. TPT rather than a biquad: at 5 Hz a direct-form
    IIR with float coefficients is numerically fragile, especially at 96 kHz. */
class DcBlocker
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void process (juce::dsp::AudioBlock<float> block);

private:
    juce::dsp::StateVariableTPTFilter<float> filter;
};

/** A delay in samples that follows a target: a target that moves smoothly (a drive's own
    latency slide, under 0.05 sample per sample at any supported rate) is followed exactly, and
    a jump in it (more than half a sample from one sample to the next) is taken out over a 10 ms
    cornerless slide that starts from wherever the delay is at that moment, so a second jump
    inside a slide retargets from the current fractional position. Allocation-free. */
class DelayGlide
{
public:
    static constexpr double kJumpSamples = 0.5;

    void prepare (double sampleRate) noexcept;
    /** The next next() lands on its target with no slide. */
    void reset() noexcept { snapNext = true; }
    /** One sample: the delay to apply, given this sample's target. */
    double next (double target) noexcept
    {
        if (snapNext)
        {
            snapNext = false;
            last = target;
            offset = start = 0.0;
            done = length;
        }
        else if (std::abs (target - last) > kJumpSamples)
        {
            start = offset = last + offset - target;
            done = 0;
        }

        last = target;

        if (done < length)
        {
            ++done;
            const double x = (double) done / (double) length;
            offset = start * (1.0 - x * x * (3.0 - 2.0 * x));
        }

        return last + offset;
    }

    bool sliding() const noexcept { return done < length; }
    /** The larger end of the slide in progress, or the target when there is none. */
    double reach() const noexcept { return last + (sliding() ? juce::jmax (0.0, start) : 0.0); }
    double current() const noexcept { return last + offset; }

private:
    double last = 0.0, offset = 0.0, start = 0.0;
    int length = 1, done = 1;
    bool snapNext = true;
};

/** How a gate, comp, drive, EQ or mod block goes in and out of the signal (docs/FX-ENGINES.md
    §6.0): its input and its engine's output are crossfaded over 10 ms when it is switched on or
    off, because the engines alone step at a hard switch under signal. The input is delayed by
    the engine's latency so the two line up, and stays delayed while the block is off when that
    latency stays counted (a non-Classic drive), so the audio is where the host was told it is.
    A delay that changes (a drive retyped while bypassed, a Classic drive whose 3 samples count
    only while it runs) moves through a DelayGlide, read with 4-point Lagrange interpolation,
    exact at whole samples. Audio thread after prepare(), allocation-free. */
class BypassFader
{
public:
    /** The longest latency it lines up: more than any drive type reports at any rate. */
    static constexpr int kMaxDelaySamples = 64;

    void prepare (const juce::dsp::ProcessSpec& spec);
    /** Silent input history, resting off. `snapOnFirstUse`: nothing was heard from the block
        before (after prepare), so the next process() starts at rest in the state it is asked
        for; otherwise (a block re-entering the chain, its engine just reset) a block that is
        on fades in after restart()'s settling time. */
    void reset (bool snapOnFirstUse) noexcept;
    /** The engine was just reset: it runs unheard for 10 ms before it fades in, so whatever
        its filters do while they fill from zero is over before it is heard. */
    void restart() noexcept { settle = settleSamples; }
    /** The next process() lands at rest in the state it is asked for, as after prepare(): no
        settle and no fade (a whole state restored). The input history is kept. */
    void snapNext() noexcept { snap = true; settle = 0; }
    /** The engine has to run this block: the block is on, or still fading out. */
    bool needsEngine (bool on) const noexcept { return on || (wet > 0.0f && ! snap); }
    /** Where the input path's delay is heading: the slide in progress taken to its larger end. */
    double delayReach() const noexcept { return glide.reach(); }
    bool delaySliding() const noexcept { return glide.sliding(); }

    /** In place, the input delayed by a fixed `delaySamples`. `engine (subBlock)` runs the
        block's engine; it is called whenever needsEngine (on) was true, on sub-blocks no larger
        than prepare() promised. */
    template <typename Engine>
    void process (juce::dsp::AudioBlock<float> block, bool on, int delaySamples, Engine&& engine) noexcept
    {
        process (block, on, (double) delaySamples, nullptr, engine, nullptr);
    }

    /** In place. The input path is delayed by `restDelay` while the engine is not running and
        by `engineDelay[i]` (sample i of the block, written by `engine` for the sub-block it
        runs, before it returns) while it is; null means restDelay throughout. `applied`, when
        not null, receives the input path's delay per sample: the block's own delay. */
    template <typename Engine>
    void process (juce::dsp::AudioBlock<float> block, bool on, double restDelay, const float* engineDelay,
                  Engine&& engine, float* applied) noexcept
    {
        const int numChannels = juce::jmin ((int) block.getNumChannels(), history.getNumChannels());
        const float target = on ? 1.0f : 0.0f;
        const bool fixedZero = restDelay == 0.0 && engineDelay == nullptr && ! glide.sliding()
                               && glide.current() == 0.0;

        if (snap)
        {
            snap = false;
            wet = target;
            glide.reset();
        }

        for (int offset = 0; offset < (int) block.getNumSamples();)
        {
            const int count = juce::jmin (chunkSamples, (int) block.getNumSamples() - offset);
            auto sub = block.getSubBlock ((size_t) offset, (size_t) count);
            remember (sub, numChannels);

            const bool runs = wet != target || on;

            if (runs)
                engine (sub);

            const float* engineAt = runs && engineDelay != nullptr ? engineDelay + offset : nullptr;
            const auto delayAt = [&] (int n)
            {
                if (fixedZero)
                    return 0.0;

                const double d = glide.next (engineAt != nullptr ? (double) engineAt[n] : restDelay);
                return juce::jlimit (0.0, (double) kMaxDelaySamples, d);
            };

            if (wet == target)
            {
                for (int n = 0; n < count && ! (fixedZero && applied == nullptr); ++n)
                {
                    const double d = delayAt (n);

                    if (! on && d != 0.0)
                        for (int ch = 0; ch < numChannels; ++ch)
                            sub.getChannelPointer ((size_t) ch)[n] = read (ch, n, d);

                    if (applied != nullptr)
                        applied[offset + n] = (float) d;
                }
            }
            else
            {
                for (int n = 0; n < count; ++n)
                {
                    if (settle > 0 && target > wet)
                        --settle;
                    else
                        wet = target > wet ? juce::jmin (target, wet + step) : juce::jmax (target, wet - step);

                    // Smoothstep over the linear ramp: no corner at either end of the fade, so a
                    // boosted block fading out leaves nothing above the music's own band.
                    const float gain = wet * wet * (3.0f - 2.0f * wet);
                    const double d = delayAt (n);

                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        const float dry = read (ch, n, d);
                        auto& y = sub.getChannelPointer ((size_t) ch)[n];
                        y = dry + gain * (y - dry);
                    }

                    if (applied != nullptr)
                        applied[offset + n] = (float) d;
                }
            }

            write = (write + count) % capacity;
            offset += count;
        }
    }

private:
    void remember (const juce::dsp::AudioBlock<float>& block, int numChannels) noexcept;
    int indexOf (int n, int delay) const noexcept { return (write + n - delay + capacity) % capacity; }

    /** The input `d` samples before sample n of the chunk being processed. Taps whole - 1 ...
        whole + 2, shifted up where whole - 1 would be in the future. */
    float read (int ch, int n, double d) const noexcept
    {
        const int whole = (int) d;

        if (juce::exactlyEqual ((double) whole, d))
            return history.getSample (ch, indexOf (n, whole));

        const int b = juce::jmax (0, whole - 1);
        const double x = d - b;
        const double w0 = -(x - 1.0) * (x - 2.0) * (x - 3.0) * (1.0 / 6.0);
        const double w1 = x * (x - 2.0) * (x - 3.0) * 0.5;
        const double w2 = -x * (x - 1.0) * (x - 3.0) * 0.5;
        const double w3 = x * (x - 1.0) * (x - 2.0) * (1.0 / 6.0);
        return (float) (w0 * history.getSample (ch, indexOf (n, b)) + w1 * history.getSample (ch, indexOf (n, b + 1))
                        + w2 * history.getSample (ch, indexOf (n, b + 2)) + w3 * history.getSample (ch, indexOf (n, b + 3)));
    }

    juce::AudioBuffer<float> history;
    DelayGlide glide;
    int capacity = 1, chunkSamples = 1, write = 0, settle = 0, settleSamples = 0;
    float wet = 0.0f, step = 1.0f;
    bool snap = true;
};

/** A crossfade between two outputs that keeps their sum's short-term power on the straight line
    in dB between the two (Drive.cpp's type switch, the chorus's mode switch): equal-power gains
    on a cornerless ramp, scaled by the measured powers and correlation of the two (two cascaded
    4 ms one-poles each), in three bands split at 150 Hz and 1 kHz, a gain for each band of each channel, the correction bounded
    at +18 dB where they nearly cancel. The
    outgoing output is heard alone for a hold first. Two outputs in antiphase (two effects on one
    held note sweep through it) cancel where their gained amplitudes meet, and no gain undoes a
    null, so the ramp starts on a moment where they correlate no worse than kMinCorrelation and,
    by the correlation's trend, will not fall below it before the fade is over; after maxWait
    samples on the first moment that is good enough now, after longestWait on any. The powers are measured from start() on, so the fade starts
    on settled figures. Allocation-free. */
class PowerCrossfade
{
public:
    static constexpr double kMinCorrelation = -0.5;

    void prepare (double sampleRate) noexcept;
    /** A fade starts: `hold` samples of the outgoing output alone, then a ramp of `length`
        samples, which waits after the hold for a moment the two will not cancel through (up to
        `maxWait` samples), or at least do not now (up to `longestWait`). */
    void start (int hold, int length, int maxWait, int longestWait) noexcept;
    /** Sample i of both channels' outputs: `out` (heard now) and `in` (coming in) give one
        result in `out`. True when the fade is over with this sample: from the next one on, `in`
        alone is the output. */
    bool mix (float* const* out, const float* const* in, int numChannels, int i) noexcept;
    /** Samples from start() to the end of the fade. */
    int span() const noexcept { return elapsed; }

private:
    static constexpr int kMaxChannels = 8, kBands = 3;

    double coefficient = 1.0;
    double splitCoefficient[kBands - 1] {};
    double power[2][3] {};   // two smoothing stages of: outgoing power, incoming power, their product
    double bandPower[kBands][kMaxChannels][2][3] {};   // the same per band (low, mid, high) and channel
    double lowOut[kBands - 1][kMaxChannels] {}, lowIn[kBands - 1][kMaxChannels] {};
    double correlationLag = 1.0;
    int hold = 0, length = 1, fadeLength = 1, maxWait = 0, longestWait = 0, elapsed = 0, advanced = 0, waited = 0;
};

/** Note values for mod_sync in quarter-note beats per LFO cycle; index 0 is "Off". The
    order matches modSyncNames in Parameters.h and is FROZEN (FxBlocks.cpp ties the two
    sizes). Append only. */
inline constexpr double kModSyncBeats[] {
    0.0,         // Off
    0.125,       // 1/32
    1.0 / 6.0,   // 1/16T
    0.25,        // 1/16
    1.0 / 3.0,   // 1/8T
    0.375,       // 1/16.
    0.5,         // 1/8
    2.0 / 3.0,   // 1/4T
    0.75,        // 1/8.
    1.0,         // 1/4
    4.0 / 3.0,   // 1/2T
    1.5,         // 1/4.
    2.0,         // 1/2
    3.0,         // 1/2.
    4.0,         // 1/1
    8.0,         // 2/1
    16.0         // 4/1
};
inline constexpr int kNumModSyncChoices = (int) (sizeof (kModSyncBeats) / sizeof (kModSyncBeats[0]));

/** Everything a mod block needs for one block: its parameters, raw, and the host
    transport. The dispatcher resolves sync and hands each engine its own params. */
struct ModParams
{
    int type = 0;                 // Modulation::Type (mod_type); out-of-range values clamp
    int chorusMode = 1;           // ChorusFx::Mode
    int phaserModel = 1;          // PhaserFx::Model
    int tremVoice = 1;            // TremoloFx::Voice
    float rateHz = 1.0f;          // mod_rate; a synced non-Classic voicing ignores it
    float depth = 0.4f;
    float mix = 0.35f;
    int sync = 0;                 // index into kModSyncBeats; 0 = Off
    float width = 1.0f;
    float tone = 0.5f;
    float shape = 0.5f;
    float wave = 0.0f;
    bool color = true;
    double bpm = 120.0;           // host tempo as reported; <= 1 (none, 0, negative) means 120, free-running
    double ppqPosition = 0.0;     // host position at the block's first sample, in quarter notes
    bool playing = false;         // the host is playing and gave ppqPosition
};

/** The mod block (docs/FX-ENGINES.md §6.5): ChorusFx, PhaserFx or TremoloFx, chosen by
    mod_type. Each engine fades its own voicing switches; the dispatcher passes them straight
    on. A mod_type change does what the old block did — the incoming engine is reset and runs
    from this block on — while the outgoing engine stays heard alone for
    crossfadeHoldSamples() (the incoming one settling unheard), then a PowerCrossfade over
    crossfadeSamples() hands over, so once the fade is over the output is the old block's, bit
    for bit. A change asked for during a fade waits for it to end, then fades from the type it
    reached to the newest request. */
class Modulation
{
public:
    enum class Type { chorus = 0, phaser = 1, tremolo = 2 };
    static constexpr int numTypes = 3;

    /** The LFO one voicing runs at: rate in Hz and the phase in cycles at the block's first
        sample (-1 = free-running). */
    struct Timing
    {
        float rateHz;
        double syncPhase;
    };

    /** Classic voicings always get mod_rate, free-running (bit-identity). Otherwise sync Off
        is mod_rate; a note value is that note at the host tempo, phase-locked to the song
        position while the host plays. */
    static Timing resolveTiming (const ModParams& params, bool classic) noexcept;
    /** Length of a mod_type crossfade at `sampleRate`, in samples. */
    static int crossfadeSamples (double sampleRate) noexcept;
    /** The longest a mod_type crossfade waits after its hold for the two engines to correlate
        well enough to fade (PowerCrossfade), in samples. */
    static int crossfadeWaitSamples (double sampleRate) noexcept;
    /** ...and the longest it waits in all while they are still in antiphase. */
    static int crossfadeLongestWaitSamples (double sampleRate) noexcept;
    /** How long a mod_type crossfade holds the outgoing engine alone before it starts, in
        samples, for the incoming type and voicing in `params`: the time the incoming engine
        needs from its reset to its steady output. A Classic chorus starts from an empty delay
        line and its wet steps in when the line reaches it (7 + 10 x depth ms, the most
        juce::dsp::Chorus delays at the centre delay ChorusFx sets), and 10 ms more lets the
        power estimates catch that step; the other chorus modes fade their wet in once their
        lines hold enough history (at most 34 + 10 ms); a phaser or tremolo settles in 30 ms
        (the phaser's own switch warms its incoming voicing that long). */
    static int crossfadeHoldSamples (const ModParams& params, double sampleRate) noexcept;

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    /** Audio thread, once per block before process(). */
    void setParameters (const ModParams& newParams);
    void process (juce::dsp::AudioBlock<float> block);

private:
    void resetEngine (int type);
    /** Hands an engine its parameters: the current ones, or for the outgoing engine of a
        crossfade the last ones it had, still following the song if it was synced. */
    void configure (int type, bool current);
    void runEngine (int type, juce::dsp::AudioBlock<float> block);

    ChorusFx chorus;
    PhaserFx phaser;
    TremoloFx tremolo;
    ChorusParams chorusSent;
    PhaserParams phaserSent;
    TremoloParams tremoloSent;
    ModParams params;
    juce::AudioBuffer<float> outgoingAudio;
    PowerCrossfade crossfade;
    double sampleRate = 44100.0;
    int active = 0, outgoing = -1, fadeLength = 1;
    bool snapType = true;
};

/** Cab IR loader: juce::dsp::Convolution + post low/high cut filters.
    loadImpulseResponse() is safe from the message thread (Convolution swaps
    internally in a background thread). */
class CabSim
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    /** Returns error string, empty on success. */
    juce::String loadImpulseResponse (const juce::File& wavFile);
    void clearImpulseResponse();
    bool hasImpulseResponse() const noexcept { return irLoaded; }
    juce::String getImpulseResponseName() const { return irName; }
    void setParameters (float lowCutHz, float highCutHz);
    void process (juce::dsp::AudioBlock<float> block);

private:
    juce::dsp::Convolution convolution;
    juce::dsp::StateVariableTPTFilter<float> lowCut, highCut;
    bool irLoaded = false;
    juce::String irName;
    double sampleRate = 44100.0;
};
} // namespace tubamp
