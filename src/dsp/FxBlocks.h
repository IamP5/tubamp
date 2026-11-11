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

/** How a gate, comp, drive, EQ or mod block running a non-Classic type goes in and out of the
    signal (docs/FX-ENGINES.md §6.0): its input and its engine's output are crossfaded over
    10 ms when it is switched on or off, because the engines alone step at a hard switch under
    signal. The input is delayed by the engine's latency so the two line up, and stays delayed
    while the block is off when that latency stays counted (a non-Classic drive), so the audio
    is where the host was told it is. Audio thread after prepare(), allocation-free. */
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
    /** The engine has to run this block: the block is on, or still fading out. */
    bool needsEngine (bool on) const noexcept { return on || (wet > 0.0f && ! snap); }
    /** A Classic block, which switches hard: only keeps the input history and the rest state
        current, so a switch to a non-Classic type finds both. The audio is not touched. */
    void follow (const juce::dsp::AudioBlock<float>& block, bool on) noexcept;

    /** In place. `engine (subBlock)` runs the block's engine; it is called whenever
        needsEngine (on) was true, on sub-blocks no larger than prepare() promised. */
    template <typename Engine>
    void process (juce::dsp::AudioBlock<float> block, bool on, int delaySamples, Engine&& engine) noexcept
    {
        const int numChannels = juce::jmin ((int) block.getNumChannels(), history.getNumChannels());
        const int delay = juce::jlimit (0, kMaxDelaySamples, delaySamples);
        const float target = on ? 1.0f : 0.0f;

        if (snap)
        {
            snap = false;
            wet = target;
        }

        for (int offset = 0; offset < (int) block.getNumSamples();)
        {
            const int count = juce::jmin (chunkSamples, (int) block.getNumSamples() - offset);
            auto sub = block.getSubBlock ((size_t) offset, (size_t) count);
            remember (sub, numChannels);

            if (wet == target)
            {
                if (on)
                    engine (sub);
                else if (delay > 0)
                    for (int ch = 0; ch < numChannels; ++ch)
                        for (int n = 0; n < count; ++n)
                            sub.getChannelPointer ((size_t) ch)[n] = history.getSample (ch, indexOf (n, delay));
            }
            else
            {
                engine (sub);

                for (int n = 0; n < count; ++n)
                {
                    if (settle > 0 && target > wet)
                        --settle;
                    else
                        wet = target > wet ? juce::jmin (target, wet + step) : juce::jmax (target, wet - step);

                    // Smoothstep over the linear ramp: no corner at either end of the fade, so a
                    // boosted block fading out leaves nothing above the music's own band.
                    const float gain = wet * wet * (3.0f - 2.0f * wet);

                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        const float dry = history.getSample (ch, indexOf (n, delay));
                        auto& y = sub.getChannelPointer ((size_t) ch)[n];
                        y = dry + gain * (y - dry);
                    }
                }
            }

            write = (write + count) % capacity;
            offset += count;
        }
    }

private:
    void remember (const juce::dsp::AudioBlock<float>& block, int numChannels) noexcept;
    int indexOf (int n, int delay) const noexcept { return (write + n - delay + capacity) % capacity; }

    juce::AudioBuffer<float> history;
    int capacity = 1, chunkSamples = 1, write = 0, settle = 0, settleSamples = 0;
    float wet = 0.0f, step = 1.0f;
    bool snap = true;
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
    double bpm = 120.0;           // host tempo; <= 1 (or none) means 120, as for the delay
    double ppqPosition = 0.0;     // host position at the block's first sample, in quarter notes
    bool playing = false;         // the host is playing and gave ppqPosition
};

/** The mod block (docs/FX-ENGINES.md §6.5): ChorusFx, PhaserFx or TremoloFx, chosen by
    mod_type. Each engine fades its own voicing switches. A mod_type change does what the
    old block did — the incoming engine is reset and runs from this block on — and also
    crossfades from the outgoing engine over crossfadeSamples() (after
    crossfadeHoldSamples()), so once the fade is over the output is the old block's, bit for
    bit. */
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
    /** How long a mod_type crossfade holds the outgoing engine alone before it starts, in
        samples, for the type and voicing in `params`: a Classic chorus starts from an empty
        delay line and its wet steps in when the line reaches it (7 + 10 x depth ms, the most
        juce::dsp::Chorus delays at the centre delay ChorusFx sets), so the fade waits for that.
        0 for every other voicing, which starts clean from its reset. */
    static int crossfadeHoldSamples (const ModParams& params, double sampleRate) noexcept;

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    /** Audio thread, once per block before process(). */
    void setParameters (const ModParams& newParams);
    void process (juce::dsp::AudioBlock<float> block);

private:
    void resetEngine (int type);
    /** Hands an engine its parameters: the current ones, or for the outgoing engine of a
        crossfade the last ones it had, free-running. */
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
    double sampleRate = 44100.0;
    int active = 0, outgoing = -1, fadePosition = 0, fadeLength = 1, fadeHold = 0;
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
