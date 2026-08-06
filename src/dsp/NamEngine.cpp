#include "NamEngine.h"

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
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <type_traits>

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

    int getLatency() const { return needToResample() ? mResampler->GetLatency() : 0; }

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

        const double upRatio = sampleRate / getEncapsulatedSampleRate();
        const int maxEncapsulatedBlockSize = (int) std::ceil ((double) maxBlockSize / upRatio);
        mEncapsulated->Reset (sampleRate, std::max (1, maxEncapsulatedBlockSize));
    }

    double getEncapsulatedSampleRate() const { return getNamSampleRate (mEncapsulated); }

private:
    bool needToResample() const
    {
        return std::abs (GetExpectedSampleRate() - getEncapsulatedSampleRate()) > 1.0e-9;
    }

    std::unique_ptr<nam::DSP> mEncapsulated;
    std::optional<dsp::ResamplingContainer<NAM_SAMPLE, 1, 12>> mResampler;
    int mMaxExternalBlockSize = kResamplerMaxBlockSize;
    std::function<void (NAM_SAMPLE**, NAM_SAMPLE**, int)> mBlockProcessFunc;
};
} // namespace

//==============================================================================
struct NamEngine::Impl
{
    ~Impl()
    {
        delete staged.exchange (nullptr, std::memory_order_acq_rel);
    }

    // Audio thread only.
    std::unique_ptr<ResamplingNAM> live;

    // Single-slot handoff: the loader thread exchanges a freshly built model in,
    // the audio thread exchanges it back out. Ownership is transferred by the
    // exchange itself, so loader and audio thread never touch the same object.
    std::atomic<ResamplingNAM*> staged { nullptr };
    std::atomic<bool> shouldRemove { false };

    // Retired models, parked by the audio thread and destroyed by collectGarbage().
    static constexpr int numGarbageSlots = 4;
    std::array<std::unique_ptr<ResamplingNAM>, numGarbageSlots> garbage;

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

    juce::SpinLock infoLock;
    ModelInfo info;

    /** Audio thread: park a retired model for later destruction on the message thread. */
    void retire (std::unique_ptr<ResamplingNAM> old)
    {
        if (old == nullptr)
            return;

        for (auto& slot : garbage)
        {
            if (slot == nullptr)
            {
                slot = std::move (old);
                return;
            }
        }

        // All slots full: collectGarbage() has not been called between four
        // consecutive swaps. Shouldn't happen (the processor collects on every
        // load/clear); destroying here would be a real-time violation, so leak
        // instead and complain in debug builds.
        jassertfalse;
        (void) old.release();
    }
};

//==============================================================================
NamEngine::NamEngine() : impl (std::make_unique<Impl>()) {}
NamEngine::~NamEngine() = default;

juce::String NamEngine::loadModel (const juce::File& namFile)
{
    // Message/background thread: file IO, JSON parse, weight load and prewarm.
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

    // A pending "clear" must not eat the model we are about to stage.
    impl->shouldRemove.store (false, std::memory_order_release);

    // Anything the audio thread has not adopted yet is ours to destroy, right here.
    // That may be the model impl->slimmable currently aliases, so re-point it in the
    // same breath — nothing dereferences it in between.
    delete impl->staged.exchange (wrapped.release(), std::memory_order_acq_rel);
    impl->slimmable = slimmable;
    impl->slimmablePresent.store (slimmable != nullptr, std::memory_order_release);
    impl->modelPresent.store (true, std::memory_order_release);

    return {};
}

void NamEngine::clearModel()
{
    collectGarbage();

    impl->slimmable = nullptr;
    impl->slimmablePresent.store (false, std::memory_order_release);

    delete impl->staged.exchange (nullptr, std::memory_order_acq_rel);
    impl->shouldRemove.store (true, std::memory_order_release);
    impl->modelPresent.store (false, std::memory_order_release);

    // Dropped here rather than at the next applyStaging(): false is the fail-safe
    // direction for the stereo gate, and it costs at most one block of stereo.
    impl->liveModelPresent.store (false, std::memory_order_release);

    const juce::SpinLock::ScopedLockType lock (impl->infoLock);
    impl->info = {};
}

bool NamEngine::applyStaging()
{
    bool changed = false;

    if (impl->shouldRemove.exchange (false, std::memory_order_acq_rel))
    {
        if (impl->live != nullptr)
        {
            impl->retire (std::move (impl->live));
            changed = true;
        }
    }

    if (auto* fresh = impl->staged.exchange (nullptr, std::memory_order_acq_rel))
    {
        impl->retire (std::move (impl->live));
        impl->live.reset (fresh);
        changed = true;
    }

    if (changed)
    {
        impl->latencySamples.store (impl->live != nullptr ? impl->live->getLatency() : 0,
                                    std::memory_order_release);

        // Published like the latency, and for the same reason: the audio thread gates
        // the dual-NAM stereo path on hasLiveModel(), so it has to describe the model
        // that is live here, not the one the message thread staged a moment ago.
        impl->liveModelPresent.store (impl->live != nullptr, std::memory_order_release);
        impl->modelPresent.store (impl->live != nullptr, std::memory_order_release);

        auto* live = impl->live.get();

        const bool loudness    = live != nullptr && live->HasLoudness();
        const bool inputLevel  = live != nullptr && live->HasInputLevel();
        const bool outputLevel = live != nullptr && live->HasOutputLevel();

        impl->loudnessDb.store (loudness ? (float) live->GetLoudness() : 0.0f,
                                std::memory_order_relaxed);
        impl->inputLevelDbu.store (inputLevel ? (float) live->GetInputLevel() : 0.0f,
                                   std::memory_order_relaxed);
        impl->outputLevelDbu.store (outputLevel ? (float) live->GetOutputLevel() : 0.0f,
                                    std::memory_order_relaxed);

        // Published last: the "has" flags gate every read of the values above.
        impl->hasLoudness.store (loudness, std::memory_order_release);
        impl->hasInputLevel.store (inputLevel, std::memory_order_release);
        impl->hasOutputLevel.store (outputLevel, std::memory_order_release);
    }

    return changed;
}

void NamEngine::process (float* monoChannel, int numSamples)
{
    if (impl->live == nullptr || numSamples <= 0)
        return; // pass-through

    float* channels[1] = { monoChannel };
    impl->live->process (channels, channels, numSamples);
}

void NamEngine::prepare (double sampleRate, int maxBlockSize)
{
    impl->hostSampleRate = sampleRate > 0.0 ? sampleRate : kAssumedModelSampleRate;
    impl->maxBlockSize = std::max (1, maxBlockSize);

    if (impl->live != nullptr)
        impl->live->Reset (impl->hostSampleRate, impl->maxBlockSize);

    // A model staged but not yet adopted was prepared for the *previous* rate.
    // The audio thread is stopped during prepareToPlay, so it is safe to take it
    // back, re-Reset it (which prewarms — allocating, hence not audio-thread work)
    // and hand it over again.
    if (auto* pending = impl->staged.exchange (nullptr, std::memory_order_acq_rel))
    {
        pending->Reset (impl->hostSampleRate, impl->maxBlockSize);
        delete impl->staged.exchange (pending, std::memory_order_acq_rel);
    }

    impl->latencySamples.store (impl->live != nullptr ? impl->live->getLatency() : 0,
                                std::memory_order_release);
}

void NamEngine::collectGarbage()
{
    for (auto& slot : impl->garbage)
        slot.reset();
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
                return (float) kTargetLoudnessDb - impl->loudnessDb.load (std::memory_order_relaxed);

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
