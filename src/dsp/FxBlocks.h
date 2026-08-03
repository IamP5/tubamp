#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

// Thin wrappers over juce::dsp for the fixed chain. Each block:
//   prepare(spec) -> reset() -> setParameters(...) (audio thread, per block) -> process(context)
// Parameter setters take plain floats already fetched from APVTS by the processor.
namespace tubamp
{
/** Oversampled waveshaper drive: gain -> tanh -> tone LPF -> level. Stereo-capable. */
class Drive
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters (float gainDb, float toneHz, float levelDb);
    /** Processes in place. */
    void process (juce::dsp::AudioBlock<float> block);

private:
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    juce::dsp::StateVariableTPTFilter<float> toneFilter;
    juce::SmoothedValue<float> gainLin { 1.0f }, levelLin { 1.0f };
    double sampleRate = 44100.0;
};

/** Port of the official plugin's dsp::tone_stack::BasicNamToneStack: three Audio EQ
    Cookbook biquads in series, driven by 0-10 knob values where 5 is flat.

      Bass    low shelf  150 Hz,  Q 0.707,          gain 4*(v-5) dB
      Middle  peaking    425 Hz,  Q 1.5 cut/0.7 boost, gain 3*(v-5) dB
      Treble  high shelf 1800 Hz, Q 0.707,          gain 2*(v-5) dB

    The middle band's Q is deliberately asymmetric (wider when cutting) — see
    ToneStack.cpp:22-61 in the reference plugin. */
class ToneStackEQ
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    /** Knob units in [0, 10]; 5 = flat. */
    void setParameters (float bass010, float mid010, float treble010);
    void process (juce::dsp::AudioBlock<float> block);

private:
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
                                                  juce::dsp::IIR::Coefficients<float>>;
    Filter bass, mid, treble;
    double sampleRate = 44100.0;
    float lastBass = 1e9f, lastMid = 1e9f, lastTreble = 1e9f;
};

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

/** Port of the official plugin's noise gate: AudioDSPTools' dsp::noise_gate::Trigger
    measures the pre-model signal and hands a per-sample gain-reduction curve to a
    dsp::noise_gate::Gain that is applied further down the chain (post-model).

    Usage per block, audio thread:
      measure (postTrimBlock, n, thresholdDb);   // analysis only, does not touch audio
      ... model ...
      apply (block, n);                          // or apply (mono, n)

    The trigger is always fed a single mono sum, so left and right are gated by the
    same curve and the buffer geometry never changes underneath the AudioDSPTools
    objects (they reallocate whenever the channel count moves).

    AudioDSPTools' headers are kept out of this header on purpose: their DSP_SAMPLE
    typedef is a build-wide macro, and Trigger/Gain would pull the whole dsp:: namespace
    into every translation unit that includes FxBlocks.h. */
class NoiseGate
{
public:
    NoiseGate();
    ~NoiseGate();

    /** Sizes and primes the internal buffers so the audio thread never allocates.
        Blocks larger than maxBlockSize are passed through ungated. */
    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    /** Audio thread. Analyses `block` summed to mono; leaves `block` untouched. */
    void measure (const juce::dsp::AudioBlock<float>& block, int numSamples, float thresholdDb);

    /** Audio thread. Applies the curve from this block's measure() to every channel. */
    void apply (juce::dsp::AudioBlock<float> block, int numSamples);
    /** Audio thread. Applies the curve from this block's measure() to one mono buffer. */
    void apply (float* mono, int numSamples);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoiseGate)
};

/** Chorus / Phaser / Tremolo behind one interface. */
class Modulation
{
public:
    enum class Type { chorus = 0, phaser = 1, tremolo = 2 };

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters (Type type, float rateHz, float depth01, float mix01);
    void process (juce::dsp::AudioBlock<float> block);

private:
    juce::dsp::Chorus<float> chorus;
    juce::dsp::Phaser<float> phaser;
    // Tremolo: simple LFO amplitude modulation with mix.
    float tremPhase = 0.0f;
    Type currentType = Type::chorus;
    float rate = 1.0f, depth = 0.5f, mix = 0.5f;
    double sampleRate = 44100.0;
};

/** Stereo delay with feedback + mix. */
class DelayFx
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters (float timeMs, float feedback01, float mix01);
    void process (juce::dsp::AudioBlock<float> block);

private:
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLine { 96000 * 2 };
    juce::SmoothedValue<float> delaySamples { 24000.0f };
    float feedback = 0.3f, mix = 0.25f;
    double sampleRate = 44100.0;
};

/** Plate-style reverb (juce::dsp::Reverb) with size/damping/mix. */
class ReverbFx
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParameters (float size01, float damping01, float mix01);
    void process (juce::dsp::AudioBlock<float> block);

private:
    juce::dsp::Reverb reverb;
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
