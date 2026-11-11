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
// A mod_type change crossfades over the engines' own voicing fade [choice: Phaser.cpp and
// Tremolo.cpp's 40 ms]: two unrelated effects need longer than the drive's 20 ms for the
// power estimates to ride over a low note's cycle.
constexpr double kModCrossfadeSeconds = 0.04;
// ChorusFx's Classic: juce::dsp::Chorus at a 7 ms centre delay swings up to 20 x 0.5 x depth ms.
constexpr double kClassicChorusCentreMs = 7.0, kClassicChorusSwingMs = 10.0;
// ChorusFx modes 1-4: the longest line a mode reads (Chorus.cpp, 34 ms) plus its 10 ms wet
// fade-in after reset, rounded up.
constexpr double kChorusSettleSeconds = 0.05;
// The longest a mod_type fade waits for the two engines to stop cancelling [choice: a quarter of
// the 1 s LFO cycle mod_rate starts at].
constexpr double kModCrossfadeWaitSeconds = 0.25;
// Classic chorus: after its wet steps in, ChorusFx's 10 ms fade-in time of the other modes.
constexpr double kClassicChorusWetSettleSeconds = 0.01;
// PhaserFx and TremoloFx: Phaser.cpp warms an incoming voicing for 30 ms before it fades.
constexpr double kModSettleSeconds = 0.03;

// A block switched on or off crossfades its input and its engine over 10 ms.
constexpr double kBypassFadeSeconds = 0.01;

// PowerCrossfade: Drive.cpp's power estimate and floor (kSwitchPowerSeconds, kSwitchPowerFloor);
// the correction is bounded at +18 dB as Phaser.cpp's is, not the drive's +10.4 dB: two
// effects on one note can sit near antiphase for the whole fade, where the drive's two types
// of one pedal hardly do.
constexpr double kPowerSeconds = 0.004;
constexpr double kPowerSlack = 1.0 / (7.943 * 7.943 - 1.0);
constexpr double kPowerFloor = 1.0e-12;
} // namespace

//==============================================================================
// DelayGlide
//==============================================================================
void DelayGlide::prepare (double sampleRate) noexcept
{
    length = juce::jmax (1, (int) std::lround (kBypassFadeSeconds * sampleRate));
    done = length;
    snapNext = true;
}

//==============================================================================
// PowerCrossfade
//==============================================================================
void PowerCrossfade::prepare (double sampleRate) noexcept
{
    coefficient = 1.0 - std::exp (-1.0 / (kPowerSeconds * sampleRate));
    start (0, 1, 0);
}

void PowerCrossfade::start (int holdSamples, int lengthSamples, int maxWaitSamples) noexcept
{
    std::fill (&power[0][0], &power[0][0] + 6, 0.0);
    hold = juce::jmax (0, holdSamples);
    length = juce::jmax (1, lengthSamples);
    maxWait = juce::jmax (0, maxWaitSamples);
    elapsed = advanced = waited = 0;
}

bool PowerCrossfade::mix (float* const* out, const float* const* in, int numChannels, int i) noexcept
{
    double a = 0.0, b = 0.0, ab = 0.0;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const double x = out[ch][i], y = in[ch][i];
        a += x * x;
        b += y * y;
        ab += x * y;
    }

    const double raw[3] { a, b, ab };

    for (int k = 0; k < 3; ++k)
    {
        power[0][k] += coefficient * (raw[k] - power[0][k]);
        power[1][k] += coefficient * (power[0][k] - power[1][k]);
    }

    const int position = elapsed++;
    const double powerOut = power[1][0], powerIn = power[1][1], cross = power[1][2];

    if (position < hold)
        return false;

    // Two outputs in antiphase cancel where their gained amplitudes meet, and no gain undoes a
    // null: the ramp starts on a moment where they correlate well enough (or after maxWait).
    if (advanced == 0)
    {
        const double both = std::sqrt (powerOut * powerIn);

        if (both >= kPowerFloor && cross < kMinCorrelation * both && waited < maxWait)
        {
            ++waited;
            return false;
        }
    }

    ++advanced;

    const double ramp = advanced / (double) length;

    if (ramp >= 1.0)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            out[ch][i] = in[ch][i];

        return true;
    }

    const double r = ramp * ramp * (3.0 - 2.0 * ramp);
    const double gOut = std::cos (0.5 * juce::MathConstants<double>::pi * r);
    const double gIn = std::sin (0.5 * juce::MathConstants<double>::pi * r);
    const double want = std::exp ((1.0 - r) * std::log (powerOut + kPowerFloor) + r * std::log (powerIn + kPowerFloor));
    const double got = gOut * gOut * powerOut + gIn * gIn * powerIn + 2.0 * gOut * gIn * cross;
    const double slack = kPowerSlack * want + 1.0e-30;
    const double scale = std::sqrt ((want + slack) / (juce::jmax (0.0, got) + slack));

    for (int ch = 0; ch < numChannels; ++ch)
        out[ch][i] = (float) (scale * (gOut * out[ch][i] + gIn * in[ch][i]));

    return false;
}

//==============================================================================
// BypassFader
//==============================================================================
void BypassFader::prepare (const juce::dsp::ProcessSpec& spec)
{
    chunkSamples = juce::jmax (1, (int) spec.maximumBlockSize);
    // The interpolated read reaches two samples past the longest delay.
    capacity = chunkSamples + kMaxDelaySamples + 4;
    history.setSize (juce::jmax (1, (int) spec.numChannels), capacity);
    step = (float) (1.0 / juce::jmax (1.0, kBypassFadeSeconds * spec.sampleRate));
    settleSamples = (int) std::lround (kBypassFadeSeconds * spec.sampleRate);
    glide.prepare (spec.sampleRate);
    reset (true);
}

void BypassFader::reset (bool snapOnFirstUse) noexcept
{
    history.clear();
    write = 0;
    wet = 0.0f;
    snap = snapOnFirstUse;
    settle = snapOnFirstUse ? 0 : settleSamples;
    glide.reset();
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

int Modulation::crossfadeWaitSamples (double sampleRate) noexcept
{
    return (int) std::lround (kModCrossfadeWaitSeconds * sampleRate);
}

int Modulation::crossfadeHoldSamples (const ModParams& p, double sampleRate) noexcept
{
    if (juce::jlimit (0, numTypes - 1, p.type) != (int) Type::chorus)
        return (int) std::lround (kModSettleSeconds * sampleRate);

    if (juce::jlimit (0, ChorusFx::numModes - 1, p.chorusMode) != (int) ChorusFx::Mode::classic)
        return (int) std::lround (kChorusSettleSeconds * sampleRate);

    // ...and then as long again as the others' fade-in, so the power estimates have caught the
    // wet that just stepped in before the fade leans on them.
    const double longestMs = kClassicChorusCentreMs + kClassicChorusSwingMs * juce::jlimit (0.0, 1.0, (double) p.depth);
    return (int) std::ceil (longestMs * 0.001 * sampleRate) + 1 + (int) std::lround (kClassicChorusWetSettleSeconds * sampleRate);
}

void Modulation::prepare (const juce::dsp::ProcessSpec& spec)
{
    chorus.prepare (spec);
    phaser.prepare (spec);
    tremolo.prepare (spec);
    outgoingAudio.setSize (juce::jmax (1, (int) spec.numChannels), juce::jmax (1, (int) spec.maximumBlockSize));
    sampleRate = spec.sampleRate;
    fadeLength = crossfadeSamples (spec.sampleRate);
    crossfade.prepare (spec.sampleRate);
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
            else if (chorusSent.syncPhase >= 0.0)
            {
                const auto timing = resolveTiming (params, false);
                chorusSent.rateHz = timing.rateHz;
                chorusSent.syncPhase = timing.syncPhase;
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
            else if (phaserSent.syncPhase >= 0.0)
            {
                const auto timing = resolveTiming (params, false);
                phaserSent.rateHz = timing.rateHz;
                phaserSent.syncPhase = timing.syncPhase;
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
            else if (tremoloSent.syncPhase >= 0.0)
            {
                const auto timing = resolveTiming (params, false);
                tremoloSent.rateHz = timing.rateHz;
                tremoloSent.syncPhase = timing.syncPhase;
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
        crossfade.start (crossfadeHoldSamples (params, sampleRate), fadeLength, crossfadeWaitSamples (sampleRate));
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

    constexpr int maxChannels = 8;
    const int channels = juce::jmin ((int) numChannels, maxChannels);
    float* heard[maxChannels] {};
    const float* coming[maxChannels] {};

    for (int ch = 0; ch < channels; ++ch)
    {
        heard[ch] = outgoingBlock.getChannelPointer ((size_t) ch);
        coming[ch] = block.getChannelPointer ((size_t) ch);
    }

    int count = (int) numSamples;

    for (int i = 0; i < (int) numSamples; ++i)
    {
        if (crossfade.mix (heard, coming, channels, i))
        {
            count = i + 1;
            outgoing = -1;
            break;
        }
    }

    for (int ch = 0; ch < channels; ++ch)
        juce::FloatVectorOperations::copy (block.getChannelPointer ((size_t) ch), heard[ch], count);
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
