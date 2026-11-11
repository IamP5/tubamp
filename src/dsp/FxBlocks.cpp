#include "FxBlocks.h"

#include "Parameters.h"

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

// --- mod block ----------------------------------------------------------------
// The rate range every non-Classic voicing clamps to (Chorus.h, Phaser.h, Tremolo.h).
constexpr double kModMinRateHz = 0.01, kModMaxRateHz = 20.0;
// A mod_type change crossfades over the switcher's 20 ms ramp.
constexpr double kModCrossfadeSeconds = 0.02;
// ChorusFx's Classic: juce::dsp::Chorus at a 7 ms centre delay swings up to 20 x 0.5 x depth ms.
constexpr double kClassicChorusCentreMs = 7.0, kClassicChorusSwingMs = 10.0;

// A block switched on or off crossfades its input and its engine over 10 ms.
constexpr double kBypassFadeSeconds = 0.01;
} // namespace

//==============================================================================
// BypassFader
//==============================================================================
void BypassFader::prepare (const juce::dsp::ProcessSpec& spec)
{
    chunkSamples = juce::jmax (1, (int) spec.maximumBlockSize);
    capacity = chunkSamples + kMaxDelaySamples + 1;
    history.setSize (juce::jmax (1, (int) spec.numChannels), capacity);
    step = (float) (1.0 / juce::jmax (1.0, kBypassFadeSeconds * spec.sampleRate));
    settleSamples = (int) std::lround (kBypassFadeSeconds * spec.sampleRate);
    reset (true);
}

void BypassFader::reset (bool snapOnFirstUse) noexcept
{
    history.clear();
    write = 0;
    wet = 0.0f;
    snap = snapOnFirstUse;
    settle = snapOnFirstUse ? 0 : settleSamples;
}

void BypassFader::follow (const juce::dsp::AudioBlock<float>& block, bool on) noexcept
{
    const int numChannels = juce::jmin ((int) block.getNumChannels(), history.getNumChannels());

    for (int offset = 0; offset < (int) block.getNumSamples();)
    {
        const int count = juce::jmin (chunkSamples, (int) block.getNumSamples() - offset);
        remember (block.getSubBlock ((size_t) offset, (size_t) count), numChannels);
        write = (write + count) % capacity;
        offset += count;
    }

    wet = on ? 1.0f : 0.0f;
    snap = false;
    settle = 0;
}

void BypassFader::remember (const juce::dsp::AudioBlock<float>& block, int numChannels) noexcept
{
    const int count = (int) block.getNumSamples();
    const int first = juce::jmin (count, capacity - write);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* source = block.getChannelPointer ((size_t) ch);
        float* ring = history.getWritePointer (ch);
        juce::FloatVectorOperations::copy (ring + write, source, first);
        juce::FloatVectorOperations::copy (ring, source + first, count - first);
    }
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
static_assert (std::size (params::modSyncNames) == (size_t) kNumModSyncChoices,
               "mod_sync choices must match kModSyncBeats");

Modulation::Timing Modulation::resolveTiming (const ModParams& p, bool classic) noexcept
{
    const int sync = juce::jlimit (0, kNumModSyncChoices - 1, p.sync);

    if (classic || sync == 0)
        return { p.rateHz, -1.0 };

    const double bpm = p.bpm > 1.0 ? p.bpm : 120.0;
    const double beats = kModSyncBeats[sync];
    const double hz = bpm / (60.0 * beats);

    // The engines clamp their rate to this range, and a clamped LFO cannot follow the song.
    if (hz < kModMinRateHz || hz > kModMaxRateHz)
        return { (float) juce::jlimit (kModMinRateHz, kModMaxRateHz, hz), -1.0 };

    // A host that reports no usable tempo gets 120 BPM, and a song position read at a tempo it
    // does not run at would hold the LFO still: it runs free instead.
    if (! p.playing || p.bpm <= 1.0 || ! std::isfinite (p.ppqPosition))
        return { (float) hz, -1.0 };

    const double cycles = p.ppqPosition / beats;
    return { (float) hz, cycles - std::floor (cycles) };
}

int Modulation::crossfadeSamples (double sampleRate) noexcept
{
    return juce::jmax (1, (int) std::lround (kModCrossfadeSeconds * sampleRate));
}

int Modulation::crossfadeHoldSamples (const ModParams& p, double sampleRate) noexcept
{
    if (juce::jlimit (0, numTypes - 1, p.type) != (int) Type::chorus
        || juce::jlimit (0, ChorusFx::numModes - 1, p.chorusMode) != (int) ChorusFx::Mode::classic)
        return 0;

    const double longestMs = kClassicChorusCentreMs + kClassicChorusSwingMs * juce::jlimit (0.0, 1.0, (double) p.depth);
    return (int) std::ceil (longestMs * 0.001 * sampleRate) + 1;
}

void Modulation::prepare (const juce::dsp::ProcessSpec& spec)
{
    chorus.prepare (spec);
    phaser.prepare (spec);
    tremolo.prepare (spec);
    outgoingAudio.setSize (juce::jmax (1, (int) spec.numChannels), juce::jmax (1, (int) spec.maximumBlockSize));
    sampleRate = spec.sampleRate;
    fadeLength = crossfadeSamples (spec.sampleRate);
    reset();
}

void Modulation::reset()
{
    chorus.reset();
    phaser.reset();
    tremolo.reset();
    outgoing = -1;
    snapType = true;
}

void Modulation::setParameters (const ModParams& newParams)
{
    params = newParams;
}

void Modulation::resetEngine (int type)
{
    switch ((Type) type)
    {
        case Type::chorus:  chorus.reset();  break;
        case Type::phaser:  phaser.reset();  break;
        case Type::tremolo: tremolo.reset(); break;
    }
}

void Modulation::configure (int type, bool current)
{
    switch ((Type) type)
    {
        case Type::chorus:
            if (current)
            {
                const int mode = juce::jlimit (0, ChorusFx::numModes - 1, params.chorusMode);
                const auto timing = resolveTiming (params, mode == (int) ChorusFx::Mode::classic);
                chorusSent = { mode, timing.rateHz, params.depth, params.mix, params.width, params.tone, timing.syncPhase };
            }
            else
            {
                chorusSent.syncPhase = -1.0;
            }

            chorus.setParameters (chorusSent);
            break;

        case Type::phaser:
            if (current)
            {
                const int model = juce::jlimit (0, PhaserFx::numModels - 1, params.phaserModel);
                const auto timing = resolveTiming (params, model == (int) PhaserFx::Model::classic);
                phaserSent = { model, timing.rateHz, params.depth, params.mix, params.color, timing.syncPhase };
            }
            else
            {
                phaserSent.syncPhase = -1.0;
            }

            phaser.setParameters (phaserSent);
            break;

        case Type::tremolo:
            if (current)
            {
                const int voice = juce::jlimit (0, TremoloFx::numVoices - 1, params.tremVoice);
                const auto timing = resolveTiming (params, voice == (int) TremoloFx::Voice::classic);
                tremoloSent = { voice, timing.rateHz, params.depth, params.mix, params.width, params.tone,
                                params.shape, params.wave, timing.syncPhase };
            }
            else
            {
                tremoloSent.syncPhase = -1.0;
            }

            tremolo.setParameters (tremoloSent);
            break;
    }
}

void Modulation::runEngine (int type, juce::dsp::AudioBlock<float> block)
{
    switch ((Type) type)
    {
        case Type::chorus:  chorus.process (block);  break;
        case Type::phaser:  phaser.process (block);  break;
        case Type::tremolo: tremolo.process (block); break;
    }
}

void Modulation::process (juce::dsp::AudioBlock<float> block)
{
    const auto numSamples = block.getNumSamples();
    const auto numChannels = block.getNumChannels();

    if (numSamples == 0)
        return;

    const int requested = juce::jlimit (0, numTypes - 1, params.type);

    if (snapType)
    {
        snapType = false;
        active = requested;
        outgoing = -1;
    }
    else if (requested != active)
    {
        // Every switch starts its engine in the block the old one did, so a second switch
        // inside a crossfade cuts the first one short.
        outgoing = active;
        active = requested;
        resetEngine (active);
        fadePosition = 0;
        fadeHold = crossfadeHoldSamples (params, sampleRate);
    }

    // Without room for the outgoing engine's copy (a block larger than prepare() promised)
    // the switch completes outright, which is exactly the old jump.
    if ((int) numSamples > outgoingAudio.getNumSamples() || (int) numChannels > outgoingAudio.getNumChannels())
        outgoing = -1;

    configure (active, true);

    if (outgoing < 0)
    {
        runEngine (active, block);
        return;
    }

    juce::dsp::AudioBlock<float> outgoingBlock (outgoingAudio.getArrayOfWritePointers(), numChannels, numSamples);
    outgoingBlock.copyFrom (block);
    configure (outgoing, false);
    runEngine (outgoing, outgoingBlock);
    runEngine (active, block);

    const int count = juce::jmin ((int) numSamples, fadeHold + fadeLength - fadePosition);

    for (size_t ch = 0; ch < numChannels; ++ch)
    {
        auto* data = block.getChannelPointer (ch);
        const auto* old = outgoingBlock.getChannelPointer (ch);

        for (int i = 0; i < count; ++i)
        {
            const float w = (float) juce::jmax (0, fadePosition + i - fadeHold) / (float) fadeLength;
            data[i] = old[i] + w * (data[i] - old[i]);
        }
    }

    fadePosition += count;

    if (fadePosition >= fadeHold + fadeLength)
        outgoing = -1;
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
