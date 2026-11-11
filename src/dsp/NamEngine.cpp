#include "NamEngine.h"
#include "EngineHandoff.h"

#include <juce_core/juce_core.h>

// AudioDSPTools' ResamplingContainer::Reset() has a default argument that refers to
// DEFAULT_BLOCK_SIZE (an iPlug2 constant that ships nowhere in AudioDSPTools itself).
// We always pass the block size explicitly; this only has to exist for the header to parse.
#ifndef DEFAULT_BLOCK_SIZE
 #define DEFAULT_BLOCK_SIZE 1024
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <NAM/get_dsp.h>
#include <NAM/dsp.h>
#include <NAM/slimmable.h>

// AudioDSPTools' LanczosResampler.h refers to iplug::PI but has IPlugConstants.h
// commented out, so nothing declares it. Supply it before the include.
namespace iplug { static constexpr double PI = 3.14159265358979323846; }

#include <dsp/ResamplingContainer/ResamplingContainer.h>

namespace tubamp
{
namespace
{
// NAM_SAMPLE must be float so that model buffers alias JUCE's AudioBuffer<float>
// channel pointers directly (nam_core is built with NAM_SAMPLE_FLOAT=1).
static_assert (std::is_same_v<NAM_SAMPLE, float>,
               "tubamp requires nam_core to be compiled with NAM_SAMPLE_FLOAT=1");

constexpr double kAssumedModelSampleRate = 48000.0;
constexpr double kTargetLoudnessDb = -18.0;
constexpr int kResamplerMaxBlockSize = 2048; // conservative default before prepareToPlay

// The reference plugin's guards on what a .nam claims about itself
// (tone3000-plugin Processor.cpp, the per-block post-model gain). A file can say
// anything: a loudness of +40 or NaN would turn "normalize" into a 60 dB boost or a
// silent channel, so metadata outside these windows is treated as absent and the
// normalization gain itself never exceeds +-24 dB.
constexpr double kMinPlausibleLoudnessDb = -100.0;
constexpr double kMaxPlausibleLoudnessDb = 0.0;
constexpr double kMaxPlausibleLevelDbu = 60.0;
constexpr float kMaxNormalizationDb = 24.0f;

// While a swap fade is running the block is processed in slices this long, so the
// swap lands within a millisecond of the ramp reaching silence instead of at the
// next host block (which may be 85 ms away at 4096 samples).
constexpr int kFadeSlice = 32;

bool plausible (double value, double low, double high) noexcept
{
    return std::isfinite (value) && value >= low && value <= high;
}

/** True when every sample is a finite number. Tested on the bit pattern (exponent
    all ones = Inf/NaN) so no floating-point optimisation can fold the test away. */
bool allFinite (const float* data, int numSamples) noexcept
{
    std::uint32_t bad = 0;

    for (int n = 0; n < numSamples; ++n)
    {
        std::uint32_t bits;
        std::memcpy (&bits, data + n, sizeof (bits));
        bad |= (std::uint32_t) ((bits & 0x7f800000u) == 0x7f800000u);
    }

    return bad == 0;
}

/** Old .nam files don't encode a sample rate (report <= 0); the NAM convention is
    to assume 48 kHz for those. */
double getNamSampleRate (const std::unique_ptr<nam::DSP>& model)
{
    const double reported = model->GetExpectedSampleRate();
    return reported <= 0.0 ? kAssumedModelSampleRate : reported;
}

/** Parses the JSON object that follows a key whose closing quote sits at `from - 1`,
    or a void var when what follows is not an object (a same-named string value, a
    truncated file). String-aware brace matching, so a '{' inside a metadata string
    cannot unbalance the count. */
juce::var parseObjectAfterKey (const char* data, size_t size, size_t from)
{
    auto skipSpace = [data, size] (size_t i)
    {
        while (i < size && juce::CharacterFunctions::isWhitespace ((juce::juce_wchar) data[i]))
            ++i;

        return i;
    };

    size_t i = skipSpace (from);

    if (i >= size || data[i] != ':')
        return {};

    i = skipSpace (i + 1);

    if (i >= size || data[i] != '{')
        return {};

    const size_t start = i;
    int depth = 0;
    bool inString = false, escaped = false;

    for (; i < size; ++i)
    {
        const char c = data[i];

        if (inString)
        {
            if (escaped)        escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"')  inString = false;

            continue;
        }

        if (c == '"')       inString = true;
        else if (c == '{')  ++depth;
        else if (c == '}' && --depth == 0)
            return juce::JSON::parse (juce::String::fromUTF8 (data + start, (int) (i + 1 - start)));
    }

    return {};
}

/** Byte-scans a .nam for its *top-level* "metadata" object and parses only that.

    nam_core never hands the metadata over: get_dsp() lifts loudness and the two
    calibration levels out of it and drops the rest, so the gear fields have to be
    read from the file directly. Handing the whole document to juce::JSON instead
    would mean re-parsing megabytes of weights for a handful of strings, and A2
    container files repeat a "metadata" key inside every submodel — hence the depth
    tracking, which only accepts the object at depth 1.

    Returns a void var when the file has no top-level metadata. */
juce::var readTopLevelMetadata (const juce::File& namFile)
{
    const juce::MemoryMappedFile mapped (namFile, juce::MemoryMappedFile::readOnly);
    const auto* data = static_cast<const char*> (mapped.getData());
    const auto size = mapped.getSize();

    if (data == nullptr || size == 0)
        return {};

    int depth = 0;
    bool inString = false, escaped = false;
    size_t tokenStart = 0;

    for (size_t i = 0; i < size; ++i)
    {
        const char c = data[i];

        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (c == '\\')
            {
                escaped = true;
            }
            else if (c == '"')
            {
                inString = false;

                if (depth == 1 && i - tokenStart == 8
                    && std::memcmp (data + tokenStart, "metadata", 8) == 0)
                {
                    // A string value that happens to read "metadata" parses as void
                    // and the scan simply carries on to the real key.
                    if (auto metadata = parseObjectAfterKey (data, size, i + 1); ! metadata.isVoid())
                        return metadata;
                }
            }

            continue;
        }

        switch (c)
        {
            case '"':               inString = true; tokenStart = i + 1; break;
            case '{': case '[':     ++depth; break;
            case '}': case ']':     --depth; break;
            default:                break;
        }
    }

    return {};
}

/** TONE3000's exporter writes "T3K-Null" wherever the uploader left a field blank,
    and its catalog spells the value "amp-cab" where the NAM trainer writes
    "amp_cab". Both spellings collapse onto the trainer's. */
juce::String normaliseGearType (const juce::String& raw)
{
    const auto value = raw.trim().toLowerCase().replaceCharacter ('-', '_');

    if (value == "t3k_null" || value == "null" || value == "none")
        return {};

    return value;
}

/** True for the gear types whose capture already ran through a speaker: amp_cab,
    amp_mic, the pedal_amp_* variants, a full rig, or a bare cab. */
bool gearIncludesCab (const juce::String& gearType)
{
    return gearType.contains ("cab") || gearType.contains ("mic") || gearType.contains ("full_rig");
}

/** Port of the official plugin's ResamplingNAM: wraps a nam::DSP running at its own
    native rate and transparently resamples host audio in and out of it.

    Deviation from the reference: process() chunks oversized blocks instead of
    throwing, because throwing on the audio thread is not an option. */
class ResamplingNAM : public nam::DSP
{
public:
    ResamplingNAM (std::unique_ptr<nam::DSP> encapsulated, double expectedSampleRate)
        : nam::DSP (encapsulated->NumInputChannels(),
                    encapsulated->NumOutputChannels(),
                    expectedSampleRate),
          mEncapsulated (std::move (encapsulated))
    {
        mBlockProcessFunc = [this] (NAM_SAMPLE** in, NAM_SAMPLE** out, int n)
        {
            mEncapsulated->process (in, out, n);
        };

        if (mEncapsulated->HasLoudness())    SetLoudness (mEncapsulated->GetLoudness());
        if (mEncapsulated->HasInputLevel())  SetInputLevel (mEncapsulated->GetInputLevel());
        if (mEncapsulated->HasOutputLevel()) SetOutputLevel (mEncapsulated->GetOutputLevel());

        ResamplingNAM::Reset (expectedSampleRate, kResamplerMaxBlockSize);
    }

    void prewarm() override { mEncapsulated->prewarm(); }

    /** Audio thread. Mono in/out (input[0], output[0]) — may alias. */
    void process (NAM_SAMPLE** input, NAM_SAMPLE** output, const int numFrames) override
    {
        const int chunk = std::max (1, mMaxExternalBlockSize);

        for (int offset = 0; offset < numFrames;)
        {
            const int n = std::min (numFrames - offset, chunk);
            NAM_SAMPLE* in[1]  = { input[0] + offset };
            NAM_SAMPLE* out[1] = { output[0] + offset };

            if (! needToResample())
                mEncapsulated->process (in, out, n);
            else
                mResampler->ProcessBlock (in, out, n, mBlockProcessFunc);

            offset += n;
        }
    }

    int getLatency() const { return mLatency; }

    /** Never call from the audio thread: resets the encapsulated model, which prewarms
        (allocates and runs the network). */
    void Reset (const double sampleRate, const int maxBlockSize) override
    {
        mExpectedSampleRate = sampleRate;
        mMaxExternalBlockSize = maxBlockSize;

        // Rebuilt from scratch, never Reset() in place: the container's Reset takes an
        // early-out when rate and block size are unchanged that clears its Lanczos
        // buffers but skips the silence pre-warm, so a same-spec re-prepare would start
        // the next render from a different resampler state than the first (audible as a
        // first-block glitch at 44.1k; bit-exact renders only at the model's own rate).
        mResampler.emplace (getEncapsulatedSampleRate());
        mResampler->Reset (sampleRate, maxBlockSize);
        mLatency = needToResample() ? measureLatency (sampleRate, maxBlockSize) : 0;

        const double upRatio = sampleRate / getEncapsulatedSampleRate();
        const int maxEncapsulatedBlockSize = (int) std::ceil ((double) maxBlockSize / upRatio);
        mEncapsulated->Reset (sampleRate, std::max (1, maxEncapsulatedBlockSize));
    }

    double getEncapsulatedSampleRate() const { return getNamSampleRate (mEncapsulated); }

private:
    /** The delay of the resampler pair, measured: a pulse through a scratch container
        whose encapsulated "model" is the identity, peak to peak in host samples.

        The container's own GetLatency() is the amount of silence it primes itself
        with, which is the delay before the *first* sample comes out — it leaves out
        the group delay of the two Lanczos kernels, a few samples depending on the
        rate ratio. Reporting the smaller number leaves the plugin that much late
        against every other track, and inside a split it leaves this lane that much
        late against the other one, which is a comb filter (the reference plugin
        measures for the same reason: measureChainBoundaryLatency, Processor.cpp).

        The pulse is a Gaussian a few samples wide at the lower of the two rates
        rather than a single sample: a one-sample impulse sits above the lower rate's
        Nyquist, and at an integer ratio the decimating resampler either passes it
        whole or drops it, depending on which phase it lands on. */
    int measureLatency (double sampleRate, int maxBlockSize) const
    {
        const int block = std::max (1, maxBlockSize);

        ::dsp::ResamplingContainer<NAM_SAMPLE, 1, 12> probe (getEncapsulatedSampleRate());
        probe.Reset (sampleRate, block);

        const double sigma = 6.0 * std::max (1.0, sampleRate / getEncapsulatedSampleRate());
        const int centre = (int) std::ceil (8.0 * sigma);
        const int needed = 2 * centre + probe.GetLatency() + 64;
        const int total = (needed / block + 1) * block;

        std::vector<NAM_SAMPLE> input ((size_t) total, (NAM_SAMPLE) 0), output ((size_t) total, (NAM_SAMPLE) 0);

        for (int n = 0; n < 2 * centre; ++n)
        {
            const double x = ((double) n - (double) centre) / sigma;
            input[(size_t) n] = (NAM_SAMPLE) std::exp (-0.5 * x * x);
        }

        const std::function<void (NAM_SAMPLE**, NAM_SAMPLE**, int)> identity =
            [] (NAM_SAMPLE** in, NAM_SAMPLE** out, int n) { std::copy_n (in[0], n, out[0]); };

        for (int offset = 0; offset < total; offset += block)
        {
            NAM_SAMPLE* in[1]  = { input.data() + offset };
            NAM_SAMPLE* out[1] = { output.data() + offset };
            probe.ProcessBlock (in, out, block, identity);
        }

        int peak = 0;

        for (int n = 1; n < total; ++n)
            if (output[(size_t) n] > output[(size_t) peak])
                peak = n;

        // Parabola through the peak and its neighbours: the true maximum of a smooth
        // pulse lies between samples, and rounding the refined position is what
        // makes the result the nearest whole sample rather than the larger one.
        double refined = (double) peak;

        if (peak > 0 && peak < total - 1)
        {
            const double l = output[(size_t) peak - 1], c = output[(size_t) peak], r = output[(size_t) peak + 1];
            const double curvature = l - 2.0 * c + r;

            if (curvature < 0.0)
                refined += 0.5 * (l - r) / curvature;
        }

        return std::max (0, (int) std::lround (refined - (double) centre));
    }

    bool needToResample() const
    {
        return std::abs (GetExpectedSampleRate() - getEncapsulatedSampleRate()) > 1.0e-9;
    }

    std::unique_ptr<nam::DSP> mEncapsulated;
    std::optional<::dsp::ResamplingContainer<NAM_SAMPLE, 1, 12>> mResampler;
    int mMaxExternalBlockSize = kResamplerMaxBlockSize;
    int mLatency = 0;
    std::function<void (NAM_SAMPLE**, NAM_SAMPLE**, int)> mBlockProcessFunc;
};
} // namespace

//==============================================================================
struct NamEngine::Impl
{
    // The model in the signal path, the one waiting to replace it, the ones it has
    // replaced, and the fade between them (EngineHandoff.h).
    EngineHandoff<ResamplingNAM> handoff;

    std::atomic<int> latencySamples { 0 };

    // Two different questions, deliberately two flags:
    //   modelPresent     — "the message thread has a model loaded or staged" (UI).
    //   liveModelPresent — "the audio thread has adopted one" (DSP gating).
    // The stereo amp path keys off the second: between loadModel() staging a model and
    // the next applyStaging(), the first is already true while impl->live is still
    // null, and a block that turned the stereo path on there would pass R through dry.
    std::atomic<bool> modelPresent { false };
    std::atomic<bool> liveModelPresent { false };

    // Metadata of the model that is *live on the audio thread*, refreshed by
    // applyStaging(). Written on the audio thread, read there and by the UI.
    std::atomic<bool> hasLoudness { false };
    std::atomic<float> loudnessDb { 0.0f };
    std::atomic<bool> hasInputLevel { false };
    std::atomic<float> inputLevelDbu { 0.0f };
    std::atomic<bool> hasOutputLevel { false };
    std::atomic<float> outputLevelDbu { 0.0f };

    // --- message thread only ---------------------------------------------------
    // The slimmable interface of the most recently staged model. It aliases the
    // *encapsulated* nam::DSP, not the ResamplingNAM wrapper, so it has to be cast
    // before the model is wrapped.
    //
    // Lifetime invariant: every path that can destroy the object it points at runs on
    // the message thread and either re-points it (loadModel) or nulls it (clearModel).
    // Retired models only ever die in collectGarbage(), which is message-thread work
    // too, and a model can only be retired after a newer one was staged — which
    // already re-pointed this. So it is never left dangling.
    nam::SlimmableModel* slimmable = nullptr;
    double slimSize = 0.0;
    std::atomic<bool> slimmablePresent { false };

    double hostSampleRate = kAssumedModelSampleRate;
    int maxBlockSize = kResamplerMaxBlockSize;

    // Serialises everything that builds, stages or re-prepares an engine. Loads are
    // message-thread work, but prepareToPlay arrives on whatever thread the host
    // likes: without this it could take a freshly staged engine back, re-prepare it
    // and stage it again right over a newer one the message thread put there in
    // between. Never taken on the audio thread; the only lock taken under it is the
    // info spin lock, so it cannot be part of a cycle with a host's own locks.
    juce::CriticalSection buildLock;

    juce::SpinLock infoLock;
    ModelInfo info;

    std::atomic<int> faultCount { 0 };

    // See NamEngine::setBypassedWhenEmpty().
    bool bypassedWhenEmpty = false;

    /** Audio thread: the model on `numSamples` of mono audio, in place. Without a
        model the audio is left alone. Non-finite samples never reach the model (they
        would poison a recurrent state for good) and never leave it. */
    void runLive (float* mono, int numSamples) noexcept
    {
        auto* live = handoff.get();

        if (live == nullptr)
            return;

        if (! allFinite (mono, numSamples))
            juce::FloatVectorOperations::clear (mono, numSamples);

        float* channels[1] = { mono };
        live->process (channels, channels, numSamples);

        if (! allFinite (mono, numSamples))
        {
            juce::FloatVectorOperations::clear (mono, numSamples);
            faultCount.fetch_add (1, std::memory_order_relaxed);
        }
    }

    /** Audio thread, whenever the live model changed: publish what is live now. */
    void publish (ResamplingNAM* model)
    {
        latencySamples.store (model != nullptr ? model->getLatency() : 0, std::memory_order_release);

        // Published like the latency, and for the same reason: the audio thread gates
        // the dual-NAM stereo path on hasLiveModel(), so it has to describe the model
        // that is live here, not the one the message thread staged a moment ago.
        liveModelPresent.store (model != nullptr, std::memory_order_release);
        modelPresent.store (model != nullptr, std::memory_order_release);

        // Metadata that fails the plausibility window is published as absent, which
        // makes the mode that needs it behave as Raw (see the constants above).
        const bool loudness    = model != nullptr && model->HasLoudness()
                                 && plausible (model->GetLoudness(), kMinPlausibleLoudnessDb, kMaxPlausibleLoudnessDb);
        const bool inputLevel  = model != nullptr && model->HasInputLevel()
                                 && plausible (model->GetInputLevel(), -kMaxPlausibleLevelDbu, kMaxPlausibleLevelDbu);
        const bool outputLevel = model != nullptr && model->HasOutputLevel()
                                 && plausible (model->GetOutputLevel(), -kMaxPlausibleLevelDbu, kMaxPlausibleLevelDbu);

        loudnessDb.store (loudness ? (float) model->GetLoudness() : 0.0f, std::memory_order_relaxed);
        inputLevelDbu.store (inputLevel ? (float) model->GetInputLevel() : 0.0f, std::memory_order_relaxed);
        outputLevelDbu.store (outputLevel ? (float) model->GetOutputLevel() : 0.0f, std::memory_order_relaxed);

        // Published last: the "has" flags gate every read of the values above.
        hasLoudness.store (loudness, std::memory_order_release);
        hasInputLevel.store (inputLevel, std::memory_order_release);
        hasOutputLevel.store (outputLevel, std::memory_order_release);
    }
};

//==============================================================================
NamEngine::NamEngine() : impl (std::make_unique<Impl>()) {}
NamEngine::~NamEngine() = default;

juce::String NamEngine::loadModel (const juce::File& namFile)
{
    // Message/background thread: file IO, JSON parse, weight load and prewarm.
    const juce::ScopedLock build (impl->buildLock);

    if (! namFile.existsAsFile())
        return "Model file not found: " + namFile.getFullPathName();

    collectGarbage();

    std::unique_ptr<nam::DSP> model;

    try
    {
        model = nam::get_dsp (std::filesystem::path (namFile.getFullPathName().toStdString()));
    }
    catch (const std::exception& e)
    {
        return "Failed to load model: " + juce::String (e.what());
    }
    catch (...)
    {
        return "Failed to load model: unknown error";
    }

    if (model == nullptr)
        return "Failed to load model: no DSP returned";

    if (model->NumInputChannels() != 1 || model->NumOutputChannels() != 1)
        return "Only mono models are supported (this one is "
               + juce::String (model->NumInputChannels()) + " in / "
               + juce::String (model->NumOutputChannels()) + " out)";

    ModelInfo newInfo;
    newInfo.filePath = namFile.getFullPathName();
    newInfo.name = namFile.getFileNameWithoutExtension();
    newInfo.sampleRate = getNamSampleRate (model);

    if (model->HasLoudness())
        newInfo.loudnessDb = model->GetLoudness();

    if (model->HasInputLevel())
        newInfo.inputLevelDbu = model->GetInputLevel();

    if (model->HasOutputLevel())
        newInfo.outputLevelDbu = model->GetOutputLevel();

    // Second pass over the file, for the descriptive metadata get_dsp() discarded.
    // Failure here is never fatal: a capture that declares no gear just reads as
    // "unknown" and the UI stays silent about cabs.
    const auto metadata = readTopLevelMetadata (namFile);

    if (auto* fields = metadata.getDynamicObject())
    {
        newInfo.gearType = normaliseGearType (fields->getProperty ("gear_type").toString());
        newInfo.includesCab = gearIncludesCab (newInfo.gearType);
    }

    // The wrapper is not the slimmable object — the encapsulated model is. Take the
    // interface pointer now; ownership moves, the object itself does not.
    auto* slimmable = dynamic_cast<nam::SlimmableModel*> (model.get());

    std::unique_ptr<ResamplingNAM> wrapped;

    try
    {
        wrapped = std::make_unique<ResamplingNAM> (std::move (model), impl->hostSampleRate);
        wrapped->Reset (impl->hostSampleRate, impl->maxBlockSize);

        // After Reset (which prewarms), matching _StageModel in the reference plugin.
        if (slimmable != nullptr)
            slimmable->SetSlimmableSize (impl->slimSize);
    }
    catch (const std::exception& e)
    {
        return "Failed to prepare model: " + juce::String (e.what());
    }
    catch (...)
    {
        return "Failed to prepare model: unknown error";
    }

    {
        const juce::SpinLock::ScopedLockType lock (impl->infoLock);
        impl->info = newInfo;
    }

    // Anything the audio thread has not adopted yet is destroyed by stage(), right
    // here. That may be the model impl->slimmable currently aliases, so re-point it
    // in the same breath — nothing dereferences it in between.
    impl->handoff.stage (std::move (wrapped));
    impl->slimmable = slimmable;
    impl->slimmablePresent.store (slimmable != nullptr, std::memory_order_release);
    impl->modelPresent.store (true, std::memory_order_release);

    return {};
}

void NamEngine::clearModel()
{
    const juce::ScopedLock build (impl->buildLock);
    collectGarbage();

    impl->slimmable = nullptr;
    impl->slimmablePresent.store (false, std::memory_order_release);

    impl->handoff.requestRemove();
    impl->modelPresent.store (false, std::memory_order_release);

    // Dropped here rather than when the audio thread lets go of the model: false is
    // the fail-safe direction for everything that reads it off the audio thread
    // (latency, UI). The audio thread itself gates on isRunning(), which stays true
    // until the model has faded out.
    impl->liveModelPresent.store (false, std::memory_order_release);

    const juce::SpinLock::ScopedLockType lock (impl->infoLock);
    impl->info = {};
}

bool NamEngine::applyStaging (bool audible)
{
    using Begin = EngineHandoff<ResamplingNAM>::Begin;

    return impl->handoff.begin (audible, [this] (ResamplingNAM* model) { impl->publish (model); })
           == Begin::swapped;
}

int NamEngine::process (float* monoChannel, int numSamples)
{
    if (numSamples <= 0)
        return -1;

    auto& handoff = impl->handoff;
    auto& fade = handoff.fade;
    handoff.noteHeard();

    int swappedAt = -1;

    for (int offset = 0; offset < numSamples;)
    {
        float* x = monoChannel + offset;

        if (fade.isIdle())
        {
            impl->runLive (x, numSamples - offset);
            break;
        }

        // A fade is running: short slices, so the swap lands within a millisecond
        // of the ramp reaching silence.
        const int n = std::min (numSamples - offset, kFadeSlice);
        const bool closing = fade.phase() == SwapFade::Phase::out;

        impl->runLive (x, n);
        fade.apply (x, n);
        offset += n;

        if (closing && fade.isSilent())
        {
            handoff.swapNow ([this] (ResamplingNAM* model) { impl->publish (model); });
            fade.startIn();
            swappedAt = offset;

            // The model is gone and the owner does not run a model-less engine
            // through here: what comes next is its untouched signal, which opens
            // through fadeUnprocessed() from the next block. This path stays silent.
            if (impl->bypassedWhenEmpty && handoff.get() == nullptr)
            {
                juce::FloatVectorOperations::clear (monoChannel + offset, numSamples - offset);
                break;
            }
        }
    }

    return swappedAt;
}

bool NamEngine::fadeUnprocessed (float* const* channels, int numChannels, int numSamples)
{
    auto& handoff = impl->handoff;
    auto& fade = handoff.fade;
    handoff.noteHeard();

    for (int offset = 0; offset < numSamples;)
    {
        if (fade.isIdle())
            return false;

        const int n = std::min (numSamples - offset, kFadeSlice);
        const bool closing = fade.phase() == SwapFade::Phase::out;

        for (int i = 0; i < n; ++i)
        {
            const float g = fade.next();

            for (int ch = 0; ch < numChannels; ++ch)
                channels[ch][offset + i] *= g;
        }

        offset += n;

        if (closing && fade.isSilent())
        {
            handoff.swapNow ([this] (ResamplingNAM* model) { impl->publish (model); });
            fade.startIn();

            if (handoff.get() != nullptr)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    juce::FloatVectorOperations::clear (channels[ch] + offset, numSamples - offset);

                return true;
            }
        }
    }

    return false;
}

bool NamEngine::hasModelInPath() const noexcept
{
    return impl->handoff.get() != nullptr;
}

void NamEngine::setBypassedWhenEmpty (bool bypassed) noexcept
{
    impl->bypassedWhenEmpty = bypassed;
}

bool NamEngine::isRunning() const noexcept
{
    return impl->handoff.isRunning();
}

int NamEngine::getFaultCount() const noexcept
{
    return impl->faultCount.load (std::memory_order_relaxed);
}

void NamEngine::prepare (double sampleRate, int maxBlockSize)
{
    const juce::ScopedLock build (impl->buildLock);

    impl->hostSampleRate = sampleRate > 0.0 ? sampleRate : kAssumedModelSampleRate;
    impl->maxBlockSize = std::max (1, maxBlockSize);

    auto& handoff = impl->handoff;
    handoff.restart (impl->hostSampleRate);

    auto* live = handoff.get();

    if (live != nullptr)
        live->Reset (impl->hostSampleRate, impl->maxBlockSize);

    // A model staged but not yet adopted was prepared for the *previous* rate.
    // The audio thread is stopped during prepareToPlay, so it is safe to take it
    // back, re-Reset it (which prewarms — allocating, hence not audio-thread work)
    // and hand it over again.
    if (auto pending = handoff.takeStaged())
    {
        pending->Reset (impl->hostSampleRate, impl->maxBlockSize);
        handoff.stage (std::move (pending));
    }

    // Audio is stopped, so whatever is pending becomes the live model right here
    // instead of in the first block: the processor seeds its gain smoothers and
    // reports latency straight after this call, and both have to describe the model
    // that will be playing — a session restore stages its model before
    // prepareToPlay, and adopting it a block late would start the stream with the
    // output gain ramping in from the wrong value and the latency changing mid-way.
    if (! handoff.swapNow ([this] (ResamplingNAM* model) { impl->publish (model); }))
    {
        live = handoff.get();
        impl->latencySamples.store (live != nullptr ? live->getLatency() : 0, std::memory_order_release);
    }
}

void NamEngine::collectGarbage()
{
    impl->handoff.collectGarbage();
}

bool NamEngine::hasModel() const noexcept
{
    return impl->modelPresent.load (std::memory_order_acquire);
}

bool NamEngine::hasLiveModel() const noexcept
{
    return impl->liveModelPresent.load (std::memory_order_acquire);
}

int NamEngine::getLatencySamples() const noexcept
{
    return impl->latencySamples.load (std::memory_order_acquire);
}

NamEngine::ModelInfo NamEngine::getModelInfo() const
{
    const juce::SpinLock::ScopedLockType lock (impl->infoLock);
    return impl->info;
}

float NamEngine::getOutputCompensationDb (params::OutputMode mode,
                                          float calibrationLevelDbu) const noexcept
{
    switch (mode)
    {
        case params::OutputMode::normalized:
            if (impl->hasLoudness.load (std::memory_order_acquire))
                return juce::jlimit (-kMaxNormalizationDb, kMaxNormalizationDb,
                                     (float) kTargetLoudnessDb - impl->loudnessDb.load (std::memory_order_relaxed));

            return 0.0f;

        case params::OutputMode::calibrated:
            if (impl->hasOutputLevel.load (std::memory_order_acquire))
                return impl->outputLevelDbu.load (std::memory_order_relaxed) - calibrationLevelDbu;

            return 0.0f;

        case params::OutputMode::raw:
        default:
            return 0.0f;
    }
}

float NamEngine::getInputCalibrationDb (bool calibrateInput, float calibrationLevelDbu) const noexcept
{
    if (! calibrateInput || ! impl->hasInputLevel.load (std::memory_order_acquire))
        return 0.0f;

    return calibrationLevelDbu - impl->inputLevelDbu.load (std::memory_order_relaxed);
}

void NamEngine::setSlimSize (double size01)
{
    const juce::ScopedLock build (impl->buildLock);
    impl->slimSize = juce::jlimit (0.0, 1.0, size01);

    // Thread-safe but not real-time safe; message thread only, hence no staging dance.
    if (impl->slimmable != nullptr)
        impl->slimmable->SetSlimmableSize (impl->slimSize);
}

bool NamEngine::isSlimmable() const noexcept
{
    return impl->slimmablePresent.load (std::memory_order_acquire);
}
} // namespace tubamp
