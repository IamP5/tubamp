#include "FxBlocks.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace tubamp
{
namespace
{
constexpr float kMinCutoffHz = 20.0f;

// --- DC blocker, mirroring kDCBlockerFrequency ------------------------------
constexpr float kDcBlockerHz = 5.0f;

float clampCutoff (float hz, double sampleRate)
{
    const auto nyquistLimit = (float) (sampleRate * 0.49);
    return juce::jlimit (kMinCutoffHz, juce::jmax (kMinCutoffHz + 1.0f, nyquistLimit), hz);
}
} // namespace

//==============================================================================
// DcBlocker
//==============================================================================
void DcBlocker::prepare (const juce::dsp::ProcessSpec& spec)
{
    filter.prepare (spec);
    filter.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    // Deliberately not clampCutoff(): that floors at 20 Hz, and this stage is meant
    // to sit below the audible band. 5 Hz is safe at every supported sample rate.
    filter.setCutoffFrequency (kDcBlockerHz);
    filter.reset();
}

void DcBlocker::reset()
{
    filter.reset();
}

void DcBlocker::process (juce::dsp::AudioBlock<float> block)
{
    juce::dsp::ProcessContextReplacing<float> context (block);
    filter.process (context);
}

//==============================================================================
// Modulation
//==============================================================================
void Modulation::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;

    chorus.prepare (spec);
    phaser.prepare (spec);

    chorus.setCentreDelay (7.0f);
    chorus.setFeedback (0.0f);
    phaser.setCentreFrequency (600.0f);
    phaser.setFeedback (0.0f);

    reset();
}

void Modulation::reset()
{
    chorus.reset();
    phaser.reset();
    tremPhase = 0.0f;
}

void Modulation::setParameters (Type type, float rateHz, float depth01, float mix01)
{
    rate = juce::jlimit (0.01f, 100.0f, rateHz);
    depth = juce::jlimit (0.0f, 1.0f, depth01);
    mix = juce::jlimit (0.0f, 1.0f, mix01);

    if (type != currentType)
    {
        currentType = type;
        // Clearing the unused engine's state avoids a stale burst when switching back.
        // Neither reset() allocates, so this is fine on the audio thread.
        chorus.reset();
        phaser.reset();
        tremPhase = 0.0f;
    }

    switch (currentType)
    {
        case Type::chorus:
            chorus.setRate (rate);
            chorus.setDepth (depth);
            chorus.setMix (mix);
            break;

        case Type::phaser:
            phaser.setRate (rate);
            phaser.setDepth (depth);
            phaser.setMix (mix);
            break;

        case Type::tremolo:
        default:
            break;
    }
}

void Modulation::process (juce::dsp::AudioBlock<float> block)
{
    if (block.getNumSamples() == 0)
        return;

    if (currentType == Type::chorus)
    {
        juce::dsp::ProcessContextReplacing<float> context (block);
        chorus.process (context);
        return;
    }

    if (currentType == Type::phaser)
    {
        juce::dsp::ProcessContextReplacing<float> context (block);
        phaser.process (context);
        return;
    }

    // Tremolo: sine LFO amplitude modulation, equal-power dry/wet blend.
    const float dryGain = std::sqrt (1.0f - mix);
    const float wetGain = std::sqrt (mix);
    const float phaseInc = rate / (float) sampleRate;
    const auto numSamples = block.getNumSamples();
    const auto numChannels = block.getNumChannels();

    for (size_t i = 0; i < numSamples; ++i)
    {
        const float lfo = 0.5f + 0.5f * std::sin (juce::MathConstants<float>::twoPi * tremPhase);
        const float modGain = 1.0f - depth * (1.0f - lfo);

        for (size_t ch = 0; ch < numChannels; ++ch)
        {
            auto* data = block.getChannelPointer (ch);
            data[i] = data[i] * dryGain + data[i] * modGain * wetGain;
        }

        tremPhase += phaseInc;

        if (tremPhase >= 1.0f)
            tremPhase -= 1.0f;
    }
}

//==============================================================================
// CabSim
//==============================================================================
void CabSim::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;

    convolution.prepare (spec);

    lowCut.prepare (spec);
    lowCut.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    lowCut.setCutoffFrequency (clampCutoff (80.0f, spec.sampleRate));

    highCut.prepare (spec);
    highCut.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    highCut.setCutoffFrequency (clampCutoff (8000.0f, spec.sampleRate));
}

void CabSim::reset()
{
    convolution.reset();
    lowCut.reset();
    highCut.reset();
}

juce::String CabSim::loadImpulseResponse (const juce::File& wavFile)
{
    if (! wavFile.existsAsFile())
        return "IR file not found: " + wavFile.getFullPathName();

    // Convolution::loadImpulseResponse() is fire-and-forget (it hands the file to an
    // internal background thread), so validate the file here to get a real error message.
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (wavFile));

    if (reader == nullptr)
        return "Unsupported audio file: " + wavFile.getFileName();

    if (reader->lengthInSamples <= 0)
        return "Impulse response is empty: " + wavFile.getFileName();

    reader.reset();

    convolution.loadImpulseResponse (wavFile,
                                     juce::dsp::Convolution::Stereo::yes,
                                     juce::dsp::Convolution::Trim::yes,
                                     0,
                                     juce::dsp::Convolution::Normalise::yes);

    irName = wavFile.getFileNameWithoutExtension();
    irLoaded = true;
    return {};
}

void CabSim::clearImpulseResponse()
{
    // The Convolution keeps the last IR internally; process() simply stops using it.
    irLoaded = false;
    irName = {};
}

void CabSim::setParameters (float lowCutHz, float highCutHz)
{
    lowCut.setCutoffFrequency (clampCutoff (lowCutHz, sampleRate));
    highCut.setCutoffFrequency (clampCutoff (highCutHz, sampleRate));
}

void CabSim::process (juce::dsp::AudioBlock<float> block)
{
    juce::dsp::ProcessContextReplacing<float> context (block);

    if (irLoaded)
        convolution.process (context);

    lowCut.process (context);
    highCut.process (context);
}
} // namespace tubamp
