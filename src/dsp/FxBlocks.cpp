#include "FxBlocks.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace tubamp
{
namespace
{
constexpr float kSmoothingSeconds = 0.02f;
constexpr float kMinCutoffHz = 20.0f;

// --- tone stack, mirroring dsp::tone_stack::BasicNamToneStack ---------------
constexpr float kBassHz = 150.0f, kMidHz = 425.0f, kTrebleHz = 1800.0f;
constexpr float kShelfQ = 0.707f;
constexpr float kMidQCut = 1.5f, kMidQBoost = 0.7f;
constexpr float kBassDbPerUnit = 4.0f, kMidDbPerUnit = 3.0f, kTrebleDbPerUnit = 2.0f;
constexpr float kToneCentre = 5.0f;

// --- DC blocker, mirroring kDCBlockerFrequency ------------------------------
constexpr float kDcBlockerHz = 5.0f;

float clampCutoff (float hz, double sampleRate)
{
    const auto nyquistLimit = (float) (sampleRate * 0.49);
    return juce::jlimit (kMinCutoffHz, juce::jmax (kMinCutoffHz + 1.0f, nyquistLimit), hz);
}
} // namespace

//==============================================================================
// Drive
//==============================================================================
void Drive::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;

    // 2x (2^1) polyphase IIR: cheapest option with negligible latency, which keeps the
    // reported plugin latency equal to the NAM resampler's.
    oversampling = std::make_unique<juce::dsp::Oversampling<float>> (
        spec.numChannels, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    oversampling->initProcessing (spec.maximumBlockSize);

    toneFilter.prepare (spec);
    toneFilter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    toneFilter.setCutoffFrequency (clampCutoff (4000.0f, sampleRate));

    gainLin.reset (sampleRate, (double) kSmoothingSeconds);
    levelLin.reset (sampleRate, (double) kSmoothingSeconds);

    reset();
}

void Drive::reset()
{
    if (oversampling != nullptr)
        oversampling->reset();

    toneFilter.reset();
    gainLin.setCurrentAndTargetValue (gainLin.getTargetValue());
    levelLin.setCurrentAndTargetValue (levelLin.getTargetValue());
}

void Drive::setParameters (float gainDb, float toneHz, float levelDb)
{
    gainLin.setTargetValue (juce::Decibels::decibelsToGain (gainDb));
    levelLin.setTargetValue (juce::Decibels::decibelsToGain (levelDb));
    toneFilter.setCutoffFrequency (clampCutoff (toneHz, sampleRate));
}

void Drive::process (juce::dsp::AudioBlock<float> block)
{
    if (oversampling == nullptr || block.getNumSamples() == 0)
        return;

    auto upBlock = oversampling->processSamplesUp (block);

    const auto factor = (size_t) oversampling->getOversamplingFactor();
    const auto numOsSamples = upBlock.getNumSamples();
    const auto numChannels = upBlock.getNumChannels();

    float currentGain = gainLin.getCurrentValue();

    for (size_t i = 0; i < numOsSamples; ++i)
    {
        // The smoother runs at the base rate, so only advance it once per host sample.
        if (i % factor == 0)
            currentGain = gainLin.getNextValue();

        for (size_t ch = 0; ch < numChannels; ++ch)
        {
            auto* data = upBlock.getChannelPointer (ch);
            data[i] = std::tanh (data[i] * currentGain);
        }
    }

    oversampling->processSamplesDown (block);

    juce::dsp::ProcessContextReplacing<float> context (block);
    toneFilter.process (context);

    const auto numSamples = block.getNumSamples();
    const auto numOutChannels = block.getNumChannels();

    for (size_t i = 0; i < numSamples; ++i)
    {
        const float level = levelLin.getNextValue();

        for (size_t ch = 0; ch < numOutChannels; ++ch)
            block.getChannelPointer (ch)[i] *= level;
    }
}

//==============================================================================
// ToneStackEQ
//==============================================================================
void ToneStackEQ::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;

    bass.prepare (spec);
    mid.prepare (spec);
    treble.prepare (spec);

    // Build flat coefficients for the new rate and mark the cached values as flat,
    // so the next setParameters() only rebuilds what actually differs.
    lastBass = lastMid = lastTreble = kToneCentre;
    using AC = juce::dsp::IIR::ArrayCoefficients<float>;
    *bass.state   = AC::makeLowShelf   (sampleRate, kBassHz,   kShelfQ,    1.0f);
    *mid.state    = AC::makePeakFilter (sampleRate, kMidHz,    kMidQBoost, 1.0f);
    *treble.state = AC::makeHighShelf  (sampleRate, kTrebleHz, kShelfQ,    1.0f);

    reset();
}

void ToneStackEQ::reset()
{
    bass.reset();
    mid.reset();
    treble.reset();
}

void ToneStackEQ::setParameters (float bass010, float mid010, float treble010)
{
    // Coefficient maths allocates a new Coefficients object with the Coefficients<float>
    // factories, so use the allocation-free ArrayCoefficients factories here instead.
    using AC = juce::dsp::IIR::ArrayCoefficients<float>;

    if (! juce::approximatelyEqual (bass010, lastBass))
    {
        lastBass = bass010;
        const float gainDb = kBassDbPerUnit * (bass010 - kToneCentre);
        *bass.state = AC::makeLowShelf (
            sampleRate, kBassHz, kShelfQ, juce::Decibels::decibelsToGain (gainDb));
    }

    if (! juce::approximatelyEqual (mid010, lastMid))
    {
        lastMid = mid010;
        const float gainDb = kMidDbPerUnit * (mid010 - kToneCentre);
        // Wider when cutting, narrower when boosting — boosting at Q 1.5 sounds honky.
        const float q = gainDb < 0.0f ? kMidQCut : kMidQBoost;
        *mid.state = AC::makePeakFilter (
            sampleRate, kMidHz, q, juce::Decibels::decibelsToGain (gainDb));
    }

    if (! juce::approximatelyEqual (treble010, lastTreble))
    {
        lastTreble = treble010;
        const float gainDb = kTrebleDbPerUnit * (treble010 - kToneCentre);
        *treble.state = AC::makeHighShelf (
            sampleRate, kTrebleHz, kShelfQ, juce::Decibels::decibelsToGain (gainDb));
    }
}

void ToneStackEQ::process (juce::dsp::AudioBlock<float> block)
{
    juce::dsp::ProcessContextReplacing<float> context (block);
    bass.process (context);
    mid.process (context);
    treble.process (context);
}

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
