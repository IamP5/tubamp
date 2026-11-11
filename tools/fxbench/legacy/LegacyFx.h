#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

// FROZEN reference copy of the effect blocks as they were at commit bf81728
// (src/dsp/FxBlocks.h/.cpp and the compressor code of src/PluginProcessor.cpp), for
// tubamp_fxbench --classic-null (docs/FX-ENGINES.md §2.3, §5.2). The class bodies are
// the base commit's text with only the namespace changed. Never edit this file: it is
// the definition of "Classic", and the engines' Classic paths must match it sample for
// sample.
namespace tubamp::legacy
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

/** The compressor block exactly as TubampAudioProcessor ran it at bf81728: a
    juce::dsp::Compressor<float> plus the makeup SmoothedValue
    (PluginProcessor.cpp:1319-1331 in process, :1096-1104 and :1328-1331 in skip,
    :893-898 for the makeup prime, which here happens on the first setParameters()
    after prepare(), the moment the processor would have read the parameter). */
class LegacyComp
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec);
    /** What the processor's re-entry reset did: the compressor only, never the makeup. */
    void reset();
    void setParameters (float thresholdDb, float ratio, float attackMs, float releaseMs, float makeupDb);
    void process (juce::dsp::AudioBlock<float> block);
    void skip (int numSamples);

private:
    juce::dsp::Compressor<float> compressor;
    juce::SmoothedValue<float> compMakeupLin { 1.0f };
    float threshold = -20.0f, ratio = 4.0f, attack = 5.0f, release = 120.0f, makeup = 0.0f;
    bool makeupPrimed = false;
};
} // namespace tubamp::legacy
