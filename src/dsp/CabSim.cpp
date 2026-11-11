#include "CabSim.h"
#include "EngineHandoff.h"
#include "../library/ToneSidecar.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>

namespace tubamp
{
namespace
{
// Every constant below is the reference plugin's (tone3000-plugin
// ProcessorModelLoader.cpp / ChainBlock.h / Processor.cpp), so one .wav means one
// level and one length in both plugins.
constexpr double kMaxIrSeconds = 10.0;
constexpr double kShortIrMaxSeconds = 1.0;
constexpr double kNormalizationReferenceRate = 48000.0;
constexpr float kMinNormalizationGain = 0.0039810717f;      // -48 dB
constexpr float kCabPadGain = 0.12589254f;                  // -18 dB
constexpr int kNonUniformHeadSamples = 8192;
constexpr int kMaxConvolverBlock = 256;
constexpr double kInstallSettleSeconds = 0.15;
constexpr juce::int64 kMaxFileBytes = 50 * 1024 * 1024;

constexpr double kMixSmoothingSeconds = 0.05;
constexpr double kTruncationFadeSeconds = 0.02;

constexpr float kMinCutoffHz = 20.0f;

float clampCutoff (float hz, double sampleRate)
{
    const auto nyquistLimit = (float) (sampleRate * 0.49);
    return juce::jlimit (kMinCutoffHz, juce::jmax (kMinCutoffHz + 1.0f, nyquistLimit), hz);
}

juce::dsp::ProcessSpec convolverSpecFor (const juce::dsp::ProcessSpec& host)
{
    // juce::dsp::Convolution transforms a block of the *prepared* size on every call,
    // whatever it is actually given: prepared for a host's promised 8192 and called
    // with 64, it burns most of a core. Capped, and fed in slices (see process()).
    return { host.sampleRate,
             (juce::uint32) juce::jlimit (1, kMaxConvolverBlock, (int) host.maximumBlockSize),
             2 };
}

/** Runs silence through a freshly prepared convolver. juce::dsp::Convolution can
    still install its engine on the first process() call, behind a 50 ms cross-fade
    from the dry signal; a convolver that goes live must already be past that. */
void settle (juce::dsp::Convolution& convolver, const juce::dsp::ProcessSpec& spec)
{
    juce::AudioBuffer<float> silence (2, (int) spec.maximumBlockSize);

    for (auto remaining = (int) std::ceil (kInstallSettleSeconds * spec.sampleRate); remaining > 0;)
    {
        const auto n = juce::jmin (remaining, (int) spec.maximumBlockSize);
        silence.clear();

        juce::dsp::AudioBlock<float> block (silence);
        auto sub = block.getSubBlock (0, (size_t) n);
        convolver.process (juce::dsp::ProcessContextReplacing<float> (sub));
        remaining -= n;
    }
}
} // namespace

//==============================================================================
/** One impulse response, ready to run: its convolvers and the gain that goes with
    it. Built and destroyed on the message thread, run on the audio thread. */
struct CabSim::Engine
{
    // Left kernel on every channel. Always there.
    std::unique_ptr<juce::dsp::Convolution> mono;

    // Left kernel on L, right kernel on R. Only for a two-channel file.
    std::unique_ptr<juce::dsp::Convolution> stereo;

    float wetGain = 1.0f;       // normalization x cab pad
    double seconds = 0.0;       // trimmed kernel length

    // Audio thread: which convolver is in the path. `kernelChosen` is false until
    // the engine's first block, when either may start (both are settled on silence).
    bool usingStereo = false;
    bool kernelChosen = false;

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        mono->prepare (spec);
        settle (*mono, spec);

        if (stereo != nullptr)
        {
            stereo->prepare (spec);
            settle (*stereo, spec);
        }
    }

    void reset()
    {
        mono->reset();

        if (stereo != nullptr)
            stereo->reset();
    }
};

struct CabSim::Impl
{
    EngineHandoff<Engine> handoff;

    juce::dsp::ProcessSpec hostSpec { 48000.0, 512, 2 };
    juce::dsp::ProcessSpec convolverSpec { convolverSpecFor (hostSpec) };

    juce::dsp::StateVariableTPTFilter<float> lowCut, highCut;
    juce::SmoothedValue<float> mix { 1.0f };
    juce::AudioBuffer<float> dry;

    std::atomic<bool> irPresent { false };
    std::atomic<int> image { (int) Image::none };
    std::atomic<double> tailSeconds { 0.0 };

    // Serialises everything that builds, stages or re-prepares an engine. Loads are
    // message-thread work, but prepareToPlay arrives on whatever thread the host
    // likes: without this it could take a freshly staged engine back, re-prepare it
    // and stage it again right over a newer one the message thread put there in
    // between. Never taken on the audio thread; the only lock taken under it is the
    // info spin lock, so it cannot be part of a cycle with a host's own locks.
    juce::CriticalSection buildLock;

    juce::SpinLock infoLock;
    Info info;

    // Audio thread: the path around the cab changed (an amp behind it was added,
    // removed or bypassed) and the other convolver has to take over. It goes through
    // the same fade as an IR swap — close, switch on the silent sample, open —
    // because the other convolver's state is whatever it held when it last ran:
    // switching in place would cut one tail off and replay a stale one.
    bool kernelSwitchPending = false;

    static bool wantsStereo (const Engine& engine, size_t numChannels, bool allowStereo) noexcept
    {
        return engine.stereo != nullptr && numChannels > 1 && allowStereo;
    }

    /** Audio thread: convolve `block` in place with the engine's kernel(s). */
    void convolve (Engine& engine, juce::dsp::AudioBlock<float> block, bool allowStereo, SwapFade& fade)
    {
        const bool wanted = wantsStereo (engine, block.getNumChannels(), allowStereo);

        if (! engine.kernelChosen)
        {
            engine.usingStereo = wanted;
            engine.kernelChosen = true;
        }
        else if (wanted != engine.usingStereo && ! kernelSwitchPending)
        {
            kernelSwitchPending = true;
            fade.requestOut();
        }

        auto& convolver = engine.usingStereo ? *engine.stereo : *engine.mono;
        convolver.process (juce::dsp::ProcessContextReplacing<float> (block));

        image.store ((int) (engine.usingStereo ? Image::stereo
                                               : engine.stereo != nullptr ? Image::monoPath : Image::monoFile),
                     std::memory_order_relaxed);
    }

    /** Audio thread, on the silent sample of a kernel switch. */
    void switchKernel (Engine& engine, size_t numChannels, bool allowStereo)
    {
        const bool wanted = wantsStereo (engine, numChannels, allowStereo);

        if (wanted != engine.usingStereo)
        {
            // Clears the incoming convolver's stale tail. For a long reverb that is
            // megabytes of memset — once, at a moment the block is silent anyway.
            (wanted ? *engine.stereo : *engine.mono).reset();
            engine.usingStereo = wanted;
        }

        lowCut.reset();
        highCut.reset();
    }

    void onSwap (Engine* engine)
    {
        // The filters still hold the old IR's tail; the swap is the silent sample.
        lowCut.reset();
        highCut.reset();

        // A kernel switch that was closing belongs to the engine that just left.
        kernelSwitchPending = false;

        if (engine == nullptr)
            image.store ((int) Image::none, std::memory_order_relaxed);
    }
};

//==============================================================================
CabSim::CabSim() : impl (std::make_unique<Impl>())
{
    // process() slices by this buffer's length, so it must never be empty — not even
    // for a block that somehow runs before prepare().
    impl->dry.setSize (2, (int) impl->convolverSpec.maximumBlockSize);
}
CabSim::~CabSim() = default;

void CabSim::prepare (const juce::dsp::ProcessSpec& spec)
{
    const juce::ScopedLock build (impl->buildLock);

    impl->hostSpec = spec;
    impl->convolverSpec = convolverSpecFor (spec);

    auto& handoff = impl->handoff;
    handoff.restart (spec.sampleRate);

    // Audio is stopped: the convolvers can be rebuilt for the new rate right here.
    // JUCE keeps the trimmed kernel at the file's rate and resamples from that.
    if (auto* live = handoff.get())
        live->prepare (impl->convolverSpec);

    if (auto pending = handoff.takeStaged())
    {
        pending->prepare (impl->convolverSpec);
        handoff.stage (std::move (pending));
    }

    impl->lowCut.prepare (spec);
    impl->lowCut.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    impl->lowCut.setCutoffFrequency (clampCutoff (80.0f, spec.sampleRate));

    impl->highCut.prepare (spec);
    impl->highCut.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    impl->highCut.setCutoffFrequency (clampCutoff (8000.0f, spec.sampleRate));

    impl->mix.reset (spec.sampleRate, kMixSmoothingSeconds);
    impl->mix.setCurrentAndTargetValue (impl->mix.getTargetValue());

    impl->dry.setSize (2, (int) impl->convolverSpec.maximumBlockSize, false, false, true);

    // Audio is stopped: what is pending becomes the live IR here, not a block late.
    handoff.swapNow ([this] (Engine* engine) { impl->onSwap (engine); });
}

void CabSim::reset()
{
    if (auto* live = impl->handoff.get())
        live->reset();

    impl->lowCut.reset();
    impl->highCut.reset();
}

juce::String CabSim::loadImpulseResponse (const juce::File& wavFile)
{
    const juce::ScopedLock build (impl->buildLock);
    collectGarbage();

    if (! wavFile.existsAsFile())
        return "IR file not found: " + wavFile.getFullPathName();

    if (wavFile.getSize() > kMaxFileBytes)
        return "File is too large: " + wavFile.getFileName();

    // Read once, decode from memory: the same bytes give the energy for the
    // normalization and the kernel for the convolvers. The read is bounded on its
    // own account, not on the size the file reports: a path out of a session or a
    // preset can name anything, and a device node says "0 bytes" and never ends.
    juce::MemoryBlock bytes;

    {
        juce::FileInputStream in (wavFile);

        if (! in.openedOk())
            return "Couldn't read the file: " + wavFile.getFileName();

        in.readIntoMemoryBlock (bytes, (juce::int64) kMaxFileBytes + 1);
    }

    if (bytes.isEmpty())
        return "Couldn't read the file: " + wavFile.getFileName();

    if ((juce::int64) bytes.getSize() > kMaxFileBytes)
        return "File is too large: " + wavFile.getFileName();

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (
        formats.createReaderFor (std::make_unique<juce::MemoryInputStream> (bytes, false)));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0
        || reader->numChannels == 0)
        return "Not a valid audio file: " + wavFile.getFileName();

    const double fileRate = reader->sampleRate;
    const auto maxFrames = (juce::int64) (kMaxIrSeconds * fileRate);
    const bool truncated = reader->lengthInSamples > maxFrames;
    const auto numFrames = (int) juce::jmin (reader->lengthInSamples, maxFrames);
    const auto fileChannels = (int) reader->numChannels;

    juce::AudioBuffer<float> file (fileChannels, numFrames);

    if (! reader->read (file.getArrayOfWritePointers(), fileChannels, 0, numFrames))
        return "Couldn't decode the file: " + wavFile.getFileName();

    reader.reset();

    // Unit-energy gain, over every channel of the file (a stereo IR is therefore
    // about 3 dB below its mono fold-down) and referred to 48 kHz so that it does
    // not depend on the rate the file happens to be stored at.
    double sumSquares = 0.0;

    for (int ch = 0; ch < fileChannels; ++ch)
    {
        const auto* data = file.getReadPointer (ch);

        for (int n = 0; n < numFrames; ++n)
            sumSquares += (double) data[n] * (double) data[n];
    }

    // A float WAV can hold NaN or Inf. One such sample makes the convolver emit NaN
    // for good, and it would latch in every filter behind the cab.
    if (! std::isfinite (sumSquares))
        return "The file contains invalid samples: " + wavFile.getFileName();

    const double l2 = std::sqrt (sumSquares * fileRate / kNormalizationReferenceRate);
    const float normalization = std::isfinite (l2) && l2 > 0.0
                                    ? juce::jlimit (kMinNormalizationGain, 1.0f, (float) (1.0 / l2))
                                    : 1.0f;

    const int kernelChannels = juce::jlimit (1, 2, fileChannels);
    juce::AudioBuffer<float> kernel (kernelChannels, numFrames);

    for (int ch = 0; ch < kernelChannels; ++ch)
        kernel.copyFrom (ch, 0, file, ch, 0, numFrames);

    // A file cut at the limit would end on a step. The reference hard-cuts; a short
    // ramp costs nothing and removes the tick at the end of a long reverb.
    if (truncated)
    {
        const int fade = juce::jmin (numFrames, (int) (kTruncationFadeSeconds * fileRate));
        kernel.applyGainRamp (numFrames - fade, fade, 1.0f, 0.0f);
    }

    // Which convolver: decided on the untrimmed length, as an upper bound.
    const bool longKernel = (double) numFrames / fileRate > kShortIrMaxSeconds;
    const auto spec = impl->convolverSpec;

    auto makeConvolver = [&kernel, fileRate, longKernel, &spec] (juce::dsp::Convolution::Stereo stereo)
    {
        auto convolver = longKernel
            ? std::make_unique<juce::dsp::Convolution> (juce::dsp::Convolution::NonUniform { kNonUniformHeadSamples })
            : std::make_unique<juce::dsp::Convolution>();

        convolver->loadImpulseResponse (juce::AudioBuffer<float> (kernel), fileRate, stereo,
                                        juce::dsp::Convolution::Trim::yes,
                                        juce::dsp::Convolution::Normalise::no);
        convolver->prepare (spec);
        settle (*convolver, spec);
        return convolver;
    };

    auto engine = std::make_unique<Engine>();

    try
    {
        engine->mono = makeConvolver (juce::dsp::Convolution::Stereo::no);

        if (kernelChannels > 1)
            engine->stereo = makeConvolver (juce::dsp::Convolution::Stereo::yes);
    }
    catch (const std::exception& e)
    {
        return "Failed to prepare the IR: " + juce::String (e.what());
    }

    engine->seconds = (double) engine->mono->getCurrentIRSize() / spec.sampleRate;

    // Cabinet or room. TONE3000's own tag wins where the file came with one; without
    // it the trimmed length decides.
    const auto sidecar = ToneSidecar::read (wavFile);
    const auto gear = sidecar.has_value() ? sidecar->gear.trim().toLowerCase() : juce::String();

    const bool reverbLike = gear == "space" ? true
                          : gear == "cab"   ? false
                                            : engine->seconds > kShortIrMaxSeconds;

    engine->wetGain = normalization * (reverbLike ? 1.0f : kCabPadGain);

    Info info;
    info.filePath = wavFile.getFullPathName();
    info.name = wavFile.getFileNameWithoutExtension();
    info.channels = kernelChannels;
    info.sampleRate = fileRate;
    info.seconds = engine->seconds;
    info.reverbLike = reverbLike;
    info.gear = gear;
    info.gainDb = juce::Decibels::gainToDecibels (engine->wetGain);
    info.truncated = truncated;

    {
        const juce::SpinLock::ScopedLockType lock (impl->infoLock);
        impl->info = info;
    }

    impl->tailSeconds.store (engine->seconds, std::memory_order_release);
    impl->handoff.stage (std::move (engine));
    impl->irPresent.store (true, std::memory_order_release);
    return {};
}

void CabSim::clearImpulseResponse()
{
    const juce::ScopedLock build (impl->buildLock);
    collectGarbage();

    impl->handoff.requestRemove();
    impl->irPresent.store (false, std::memory_order_release);
    impl->tailSeconds.store (0.0, std::memory_order_release);

    const juce::SpinLock::ScopedLockType lock (impl->infoLock);
    impl->info = {};
}

void CabSim::collectGarbage()
{
    impl->handoff.collectGarbage();
}

bool CabSim::hasImpulseResponse() const noexcept
{
    return impl->irPresent.load (std::memory_order_acquire);
}

CabSim::Info CabSim::getInfo() const
{
    const juce::SpinLock::ScopedLockType lock (impl->infoLock);
    return impl->info;
}

CabSim::Image CabSim::getImage() const noexcept
{
    return (Image) impl->image.load (std::memory_order_relaxed);
}

double CabSim::getTailSeconds() const noexcept
{
    return impl->tailSeconds.load (std::memory_order_acquire);
}

void CabSim::applyStaging (bool audible)
{
    impl->handoff.begin (audible, [this] (Engine* engine) { impl->onSwap (engine); });
}

bool CabSim::isRunning() const noexcept
{
    return impl->handoff.isRunning();
}

void CabSim::setParameters (float lowCutHz, float highCutHz, float mix01)
{
    impl->lowCut.setCutoffFrequency (clampCutoff (lowCutHz, impl->hostSpec.sampleRate));
    impl->highCut.setCutoffFrequency (clampCutoff (highCutHz, impl->hostSpec.sampleRate));
    impl->mix.setTargetValue (juce::jlimit (0.0f, 1.0f, mix01));
}

void CabSim::process (juce::dsp::AudioBlock<float> block, bool allowStereo)
{
    auto& handoff = impl->handoff;
    auto& fade = handoff.fade;
    auto& mix = impl->mix;

    const auto numChannels = juce::jmin ((size_t) 2, block.getNumChannels());
    const auto numSamples = block.getNumSamples();

    if (numChannels == 0 || numSamples == 0)
        return;

    handoff.noteHeard();

    auto channels = block.getSubsetChannelBlock (0, numChannels);
    const auto slice = (size_t) impl->dry.getNumSamples();

    for (size_t offset = 0; offset < numSamples;)
    {
        const auto n = juce::jmin (numSamples - offset, slice);
        auto wet = channels.getSubBlock (offset, n);
        auto* engine = handoff.get();

        // The common case — IR settled, fully wet — needs no dry copy and no ramps.
        const bool plain = fade.isIdle() && ! mix.isSmoothing() && mix.getTargetValue() >= 1.0f;

        if (! plain)
            for (size_t ch = 0; ch < numChannels; ++ch)
                impl->dry.copyFrom ((int) ch, 0, wet.getChannelPointer (ch), (int) n);

        // Without an engine the "wet" signal is the input itself: that is what
        // closes before a first IR opens, and what opens after an IR is cleared.
        if (engine != nullptr)
        {
            impl->convolve (*engine, wet, allowStereo, fade);
            wet.multiplyBy (engine->wetGain);

            juce::dsp::ProcessContextReplacing<float> context (wet);
            impl->lowCut.process (context);
            impl->highCut.process (context);
        }

        if (! plain)
        {
            for (size_t i = 0; i < n; ++i)
            {
                const float m = mix.getNextValue();
                const float g = fade.next();

                for (size_t ch = 0; ch < numChannels; ++ch)
                {
                    auto* out = wet.getChannelPointer (ch);
                    const float d = impl->dry.getReadPointer ((int) ch)[i];

                    // Only the wet share rides the fade; the dry share of the mix
                    // holds. A swap at full wet therefore dips to silence and never
                    // lets the un-cabbed signal through.
                    out[i] = d * (1.0f - m) + out[i] * m * g;
                }
            }
        }

        if (fade.isSilent())
        {
            // An IR swap wins over a kernel switch: the new engine picks its own.
            if (handoff.hasPending())
                handoff.swapNow ([this] (Engine* fresh) { impl->onSwap (fresh); });
            else if (impl->kernelSwitchPending && engine != nullptr)
                impl->switchKernel (*engine, numChannels, allowStereo);

            impl->kernelSwitchPending = false;
            fade.startIn();
        }

        offset += n;
    }
}
} // namespace tubamp
