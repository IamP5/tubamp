#pragma once

#include <juce_dsp/juce_dsp.h>

#include "Delay.h"
#include "Reverb.h"

#include <memory>

// Thin wrappers over juce::dsp for the fixed chain. Each block:
//   prepare(spec) -> reset() -> setParameters(...) (audio thread, per block) -> process(context)
// Parameter setters take plain floats already fetched from APVTS by the processor.
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
