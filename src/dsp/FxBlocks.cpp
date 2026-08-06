#include "FxBlocks.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <dsp/NoiseGate.h>

namespace tubamp
{
namespace
{
constexpr float kSmoothingSeconds = 0.02f;
constexpr float kDelaySmoothingSeconds = 0.15f; // long enough to hide zipper on time changes
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
// DelayFx
//==============================================================================
void DelayFx::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;

    // Worst case, not the knob's face value: delay_time tops out at 2000 ms and Dual
    // mode's delay_ratio at 200%, so line R can be asked for 4000 ms. Sizing for 2 s
    // would silently clamp the R time (setParameters jlimits to the line length) and
    // collapse Dual onto Stereo over the top half of the ratio range. +2 samples of
    // headroom for the interpolator.
    const int maxDelay = (int) std::ceil (spec.sampleRate * 4.0) + 2;
    delayLine.setMaximumDelayInSamples (maxDelay);
    delayLine.prepare (spec);

    const float initialDelay = juce::jlimit (1.0f, (float) maxDelay, (float) (0.42 * sampleRate));

    delaySamplesL.reset (sampleRate, (double) kDelaySmoothingSeconds);
    delaySamplesR.reset (sampleRate, (double) kDelaySmoothingSeconds);
    delaySamplesL.setCurrentAndTargetValue (initialDelay);
    delaySamplesR.setCurrentAndTargetValue (initialDelay);

    reset();
}

void DelayFx::reset()
{
    // Both delay lines are channels of one DelayLine, so this clears L and R.
    delayLine.reset();
}

void DelayFx::setParameters (float timeMs, float feedback01, float mix01,
                             int modeIndex, float ratioPct, float width01)
{
    const auto maxDelay = (float) delayLine.getMaximumDelayInSamples();
    const float timeL = timeMs * 0.001f * (float) sampleRate;

    mode = (Mode) juce::jlimit (0, 2, modeIndex);

    // Only Dual splits the two lengths; the other modes read R at the L time.
    const float timeR = mode == Mode::dual ? timeL * ratioPct * 0.01f : timeL;

    delaySamplesL.setTargetValue (juce::jlimit (1.0f, maxDelay, timeL));
    delaySamplesR.setTargetValue (juce::jlimit (1.0f, maxDelay, timeR));

    feedback = juce::jlimit (0.0f, 0.95f, feedback01);
    mix = juce::jlimit (0.0f, 1.0f, mix01);
    width = juce::jlimit (0.0f, 1.0f, width01);
}

void DelayFx::process (juce::dsp::AudioBlock<float> block)
{
    const auto numSamples = block.getNumSamples();
    const auto channels = block.getNumChannels();
    const float dryGain = 1.0f - mix;
    const float wetGain = mix;

    // Mono hosts collapse every mode onto the single-line path, and Stereo mode *is*
    // that path. Width at 1 is algebraically inert, so skipping the M/S round trip
    // there keeps old sessions bit-identical rather than merely equal to the ear.
    if (channels != 2 || (mode == Mode::stereo && juce::approximatelyEqual (width, 1.0f)))
    {
        delaySamplesR.skip ((int) numSamples); // keep R in step for a later mode switch

        for (size_t i = 0; i < numSamples; ++i)
        {
            // One smoothed delay value per frame, shared by all channels.
            const float delayInSamples = delaySamplesL.getNextValue();

            for (size_t ch = 0; ch < channels; ++ch)
            {
                auto* data = block.getChannelPointer (ch);
                const float dry = data[i];
                const float delayed = delayLine.popSample ((int) ch, delayInSamples, true);

                delayLine.pushSample ((int) ch, dry + delayed * feedback);
                data[i] = dry * dryGain + delayed * wetGain;
            }
        }

        return;
    }

    auto* left = block.getChannelPointer (0);
    auto* right = block.getChannelPointer (1);

    for (size_t i = 0; i < numSamples; ++i)
    {
        const float timeL = delaySamplesL.getNextValue();
        const float timeR = delaySamplesR.getNextValue();
        const float dryL = left[i], dryR = right[i];

        const float wetL = delayLine.popSample (0, timeL, true);
        const float wetR = delayLine.popSample (1, mode == Mode::dual ? timeR : timeL, true);

        if (mode == Mode::pingPong)
        {
            // Only line L is seeded, and each line feeds the other, so the first repeat
            // lands left at t and right at 2t. Mono sum matches the gate's convention.
            const float inMono = 0.5f * (dryL + dryR);
            delayLine.pushSample (0, inMono + wetR * feedback);
            delayLine.pushSample (1, wetL * feedback);
        }
        else
        {
            delayLine.pushSample (0, dryL + wetL * feedback);
            delayLine.pushSample (1, dryR + wetR * feedback);
        }

        // Width narrows the wet signal only; the dry stays true stereo. Feedback is fed
        // the raw taps, so width never changes how the repeats decay.
        const float mid = 0.5f * (wetL + wetR);
        const float side = 0.5f * (wetL - wetR) * width;

        left[i]  = dryL * dryGain + (mid + side) * wetGain;
        right[i] = dryR * dryGain + (mid - side) * wetGain;
    }
}

//==============================================================================
// ReverbFx
//==============================================================================
void ReverbFx::prepare (const juce::dsp::ProcessSpec& spec)
{
    reverb.prepare (spec);
    reverb.reset();
}

void ReverbFx::reset()
{
    reverb.reset();
}

void ReverbFx::setParameters (float size01, float damping01, float mix01, float width01)
{
    juce::dsp::Reverb::Parameters p;
    p.roomSize = juce::jlimit (0.0f, 1.0f, size01);
    p.damping = juce::jlimit (0.0f, 1.0f, damping01);
    p.wetLevel = juce::jlimit (0.0f, 1.0f, mix01);
    p.dryLevel = 1.0f - p.wetLevel;
    p.width = juce::jlimit (0.0f, 1.0f, width01);
    p.freezeMode = 0.0f;
    reverb.setParameters (p);
}

void ReverbFx::process (juce::dsp::AudioBlock<float> block)
{
    // juce::dsp::Reverb only handles mono or stereo.
    const auto channels = juce::jmin ((size_t) 2, block.getNumChannels());

    if (channels == 0)
        return;

    auto sub = block.getSubsetChannelBlock (0, channels);
    juce::dsp::ProcessContextReplacing<float> context (sub);
    reverb.process (context);
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
