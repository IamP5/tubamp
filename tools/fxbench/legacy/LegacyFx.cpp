#include "LegacyFx.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <dsp/NoiseGate.h>

// FROZEN: see LegacyFx.h. Everything from here to LegacyComp is the base commit's
// src/dsp/FxBlocks.cpp text (DcBlocker and CabSim left out) with only the namespace
// changed; LegacyComp is src/PluginProcessor.cpp's compressor code.
namespace tubamp::legacy
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

// --- noise gate, mirroring NeuralAmpModeler::ProcessBlock:346-352 ----------
constexpr double kGateTime = 0.01;
constexpr double kGateRatio = 0.1;
constexpr double kGateOpenTime = 0.005;
constexpr double kGateHoldTime = 0.01;
constexpr double kGateCloseTime = 0.05;

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
// NoiseGate
//==============================================================================
struct NoiseGate::Impl
{
    ::dsp::noise_gate::Trigger trigger;
    ::dsp::noise_gate::Gain gain;

    // The AudioDSPTools objects work in DSP_SAMPLE, which is not necessarily float,
    // so audio crosses in and out of them through these fixed-size scratch buffers.
    std::vector<DSP_SAMPLE> monoIn;  // what the trigger listens to
    std::vector<DSP_SAMPLE> ones;    // probe fed to the gain to read back its curve

    // The gain's own output for a unit input == the linear gain-reduction curve.
    // Owned by `gain`, valid until its next Process() call.
    const DSP_SAMPLE* curve = nullptr;
    int curveFrames = 0;

    int maxBlockSize = 0;
    double sampleRate = 48000.0;

    /** Runs both objects at full size so their vectors reach their final capacity.
        Every later block is smaller or equal, and std::vector never releases capacity
        on shrink, so Process() cannot allocate on the audio thread afterwards. */
    void primeBuffers()
    {
        if (maxBlockSize <= 0)
            return;

        std::fill (monoIn.begin(), monoIn.end(), (DSP_SAMPLE) 0);
        run (maxBlockSize, -80.0f);
        curve = nullptr;
        curveFrames = 0;
    }

    void run (int numSamples, float thresholdDb)
    {
        trigger.SetSampleRate (sampleRate);
        trigger.SetParams (::dsp::noise_gate::TriggerParams (
            kGateTime, (double) thresholdDb, kGateRatio, kGateOpenTime, kGateHoldTime, kGateCloseTime));

        DSP_SAMPLE* triggerIn[1] = { monoIn.data() };
        trigger.Process (triggerIn, 1, (size_t) numSamples);

        // Trigger::Process pushed the new reduction curve into `gain` via the listener
        // link. Running the gain over a buffer of ones turns that dB curve into the
        // linear per-sample multiplier, which we can then apply to any channel count.
        DSP_SAMPLE* probe[1] = { ones.data() };
        curve = gain.Process (probe, 1, (size_t) numSamples)[0];
        curveFrames = numSamples;
    }
};

NoiseGate::NoiseGate() : impl (std::make_unique<Impl>())
{
    impl->trigger.AddListener (&impl->gain);
}

NoiseGate::~NoiseGate() = default;

void NoiseGate::prepare (double sampleRate, int maxBlockSize)
{
    impl->sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    impl->maxBlockSize = juce::jmax (1, maxBlockSize);

    impl->monoIn.assign ((size_t) impl->maxBlockSize, (DSP_SAMPLE) 0);
    impl->ones.assign ((size_t) impl->maxBlockSize, (DSP_SAMPLE) 1);

    impl->primeBuffers();
}

void NoiseGate::reset()
{
    // The trigger keeps no externally resettable state; invalidating the curve is
    // enough to stop apply() from reusing a stale block.
    impl->curve = nullptr;
    impl->curveFrames = 0;
}

void NoiseGate::measure (const juce::dsp::AudioBlock<float>& block, int numSamples, float thresholdDb)
{
    impl->curve = nullptr;
    impl->curveFrames = 0;

    const auto numChannels = (int) block.getNumChannels();

    if (numSamples <= 0 || numChannels <= 0 || numSamples > impl->maxBlockSize)
        return; // oversized block: pass through ungated rather than reallocate here

    const float scale = 1.0f / (float) numChannels;

    for (int i = 0; i < numSamples; ++i)
    {
        float sum = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
            sum += block.getChannelPointer ((size_t) ch)[i];

        impl->monoIn[(size_t) i] = (DSP_SAMPLE) (sum * scale);
    }

    impl->run (numSamples, thresholdDb);
}

void NoiseGate::apply (juce::dsp::AudioBlock<float> block, int numSamples)
{
    if (impl->curve == nullptr || impl->curveFrames != numSamples)
        return;

    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
    {
        auto* data = block.getChannelPointer (ch);

        for (int i = 0; i < numSamples; ++i)
            data[i] *= (float) impl->curve[i];
    }
}

void NoiseGate::apply (float* mono, int numSamples)
{
    if (mono == nullptr || impl->curve == nullptr || impl->curveFrames != numSamples)
        return;

    for (int i = 0; i < numSamples; ++i)
        mono[i] *= (float) impl->curve[i];
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
// LegacyComp
//==============================================================================
namespace
{
constexpr double kGainSmoothingSeconds = 0.02;

/** Applies a smoothed gain to the whole buffer as a single ramp — one pass, no zipper. */
void applySmoothedGain (juce::AudioBuffer<float>& buffer, int numSamples,
                        juce::SmoothedValue<float>& smoothed)
{
    const float start = smoothed.getCurrentValue();
    const float end = smoothed.skip (numSamples);

    if (juce::approximatelyEqual (start, end))
        buffer.applyGain (0, numSamples, start);
    else
        buffer.applyGainRamp (0, numSamples, start, end);
}
} // namespace

void LegacyComp::prepare (const juce::dsp::ProcessSpec& spec)
{
    compressor.prepare (spec);
    compressor.reset();

    compMakeupLin.reset (spec.sampleRate, kGainSmoothingSeconds);
    makeupPrimed = false;
}

void LegacyComp::reset()
{
    compressor.reset();
}

void LegacyComp::setParameters (float thresholdDb, float ratio_, float attackMs, float releaseMs, float makeupDb)
{
    threshold = thresholdDb;
    ratio = ratio_;
    attack = attackMs;
    release = releaseMs;
    makeup = makeupDb;

    if (! makeupPrimed)
    {
        compMakeupLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (makeup));
        makeupPrimed = true;
    }
}

void LegacyComp::process (juce::dsp::AudioBlock<float> block)
{
    const auto numSamples = (int) block.getNumSamples();
    juce::dsp::ProcessContextReplacing<float> context (block);

    compressor.setThreshold (threshold);
    compressor.setRatio (juce::jmax (1.0f, ratio));
    compressor.setAttack (juce::jmax (0.0f, attack));
    compressor.setRelease (juce::jmax (0.0f, release));
    compressor.process (context);

    // The processor applied the makeup through an AudioBuffer over the same channels;
    // a non-owning view keeps applyGain's approximatelyEqual shortcut identical.
    float* channels[2] { block.getChannelPointer (0),
                         block.getNumChannels() > 1 ? block.getChannelPointer (1) : nullptr };
    juce::AudioBuffer<float> buf (channels, juce::jmin (2, (int) block.getNumChannels()), numSamples);

    compMakeupLin.setTargetValue (juce::Decibels::decibelsToGain (makeup));
    applySmoothedGain (buf, numSamples, compMakeupLin);
}

void LegacyComp::skip (int numSamples)
{
    compMakeupLin.setTargetValue (juce::Decibels::decibelsToGain (makeup));
    compMakeupLin.skip (numSamples);
}
} // namespace tubamp::legacy
