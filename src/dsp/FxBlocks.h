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
    crossfades from the outgoing engine over crossfadeSamples(), so once the fade is over
    the output is the old block's, bit for bit. */
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
    int active = 0, outgoing = -1, fadePosition = 0, fadeLength = 1;
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
