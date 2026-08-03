#include "FxHost.h"

namespace tubamp
{
namespace
{
constexpr int kMaxAdaptChannels = 8;

/** Ring-buffer delay used to hold a bypassed slot at the latency its plugin
    reports, so that toggling the bypass parameter never moves the plugin's reported
    latency (see FxHost::process). Always written, only read while bypassed, so the
    delayed signal is already coherent the instant bypass engages. */
struct BypassDelay
{
    juce::AudioBuffer<float> ring;
    int writePos = 0;

    void resize (int numChannels, int lengthSamples)
    {
        length = juce::jmax (0, lengthSamples);
        writePos = 0;

        if (length > 0)
        {
            ring.setSize (juce::jmax (1, numChannels), length, false, true, true);
            ring.clear();
        }
        else
        {
            ring.setSize (0, 0, false, false, true);
        }
    }

    /** Pushes `numSamples` of input and, when `readBack`, replaces them with the
        samples that entered `length` frames ago. Allocation-free. */
    void run (juce::AudioBuffer<float>& buffer, int numSamples, bool readBack) noexcept
    {
        if (length <= 0 || ring.getNumSamples() <= 0)
            return;

        const int channels = juce::jmin (buffer.getNumChannels(), ring.getNumChannels());

        for (int n = 0; n < numSamples; ++n)
        {
            const int pos = (writePos + n) % length;

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* ringData = ring.getWritePointer (ch);
                auto* bufData = buffer.getWritePointer (ch);

                const float delayed = ringData[pos];
                ringData[pos] = bufData[n];

                if (readBack)
                    bufData[n] = delayed;
            }
        }

        writePos = (writePos + numSamples) % length;
    }

    int length = 0;
};
} // namespace

//==============================================================================
struct FxHost::Prepared
{
    std::unique_ptr<juce::AudioProcessor> processor;

    /** Configuration stamp this instance was prepared for; the audio thread bypasses
        it when the stamp no longer matches FxHost::configEpoch. */
    int configEpoch = 0;

    /** Channel count of our buffer that the instance was negotiated against. */
    int hostChannels = 0;

    /** Width the instance's MAIN bus was negotiated to: our width, or 1 when the
        plugin would only take mono. */
    int mainChannels = 0;

    /** Total channels the instance's own buffer must have — main plus any sidechain or
        aux bus it refused to give up. We never feed those, but we must still hand the
        instance a buffer wide enough for them: JUCE's AU host maps every enabled bus
        straight into the buffer it is given and only jasserts on overflow, so a buffer
        sized to OUR channel count is a read from a null channel pointer in a release
        build — an immediate crash inside AudioUnitRender. */
    int ioChannels = 0;

    /** Largest block the instance was prepared for; process() chunks to it. */
    int maxBlockSize = 0;

    int latencySamples = 0;

    /** The instance always runs on this, never on the host buffer. */
    juce::AudioBuffer<float> ioScratch;
    juce::MidiBuffer midiScratch;
    BypassDelay bypassDelay;
};

//==============================================================================
struct FxHost::Slot
{
    // --- audio thread only
    std::unique_ptr<FxHost::Prepared> live;

    // --- single-slot handoff: the message thread exchanges a prepared instance in,
    //     the audio thread exchanges it back out. Ownership travels with the swap.
    std::atomic<FxHost::Prepared*> staged { nullptr };
    std::atomic<bool> shouldRemove { false };

    // --- retired instances, parked by the audio thread, deleted by collectGarbage().
    //     Atomic slots, unlike the plain unique_ptr array NamEngine uses: three slots
    //     and several drain triggers make the unsynchronised version a real race
    //     rather than a theoretical one.
    static constexpr int numGarbageSlots = 6;
    std::array<std::atomic<FxHost::Prepared*>, numGarbageSlots> garbage {};

    // --- published to the audio thread / read by the UI
    std::atomic<int> latencySamples { 0 };
    std::atomic<bool> live_ { false };

    // --- message thread only: the newest staged instance. Never garbage (a slot only
    //     retires an instance when its successor is staged), so it stays readable.
    FxHost::Prepared* newest = nullptr;

    /** Audio thread: park a retired instance. Never deletes — see FxHost.h rule 1. */
    void retire (std::unique_ptr<FxHost::Prepared> old) noexcept
    {
        if (old == nullptr)
            return;

        for (auto& slot : garbage)
        {
            FxHost::Prepared* expected = nullptr;

            if (slot.compare_exchange_strong (expected, old.get(), std::memory_order_acq_rel))
            {
                (void) old.release();
                return;
            }
        }

        // Every garbage slot full: the message thread has not drained in a very long
        // time. Leaking is the only correct action here — deleting an AudioUnit
        // instance on this thread blocks render on the message thread.
        jassertfalse;
        (void) old.release();
    }
};

//==============================================================================
FxHost::FxHost()
{
    for (auto& slot : slots)
        slot = std::make_unique<Slot>();
}

FxHost::~FxHost()
{
    for (auto& slot : slots)
    {
        slot->live.reset();
        delete slot->staged.exchange (nullptr, std::memory_order_acq_rel);
        slot->newest = nullptr;
    }

    collectGarbage();
}

//==============================================================================
void FxHost::applyStaging() noexcept
{
    for (auto& slot : slots)
    {
        if (auto* incoming = slot->staged.exchange (nullptr, std::memory_order_acq_rel))
        {
            slot->retire (std::move (slot->live));
            slot->live.reset (incoming);
        }
        else if (slot->shouldRemove.exchange (false, std::memory_order_acq_rel))
        {
            slot->retire (std::move (slot->live));
        }
    }
}

void FxHost::process (int index, juce::AudioBuffer<float>& buffer, int numSamples,
                      bool enabled, juce::AudioPlayHead* playHead) noexcept
{
    if (! validIndex (index) || numSamples <= 0)
        return;

    auto* prepared = slots[(size_t) index]->live.get();

    if (prepared == nullptr || prepared->processor == nullptr)
        return; // empty slot: bit-exact pass-through

    // Configuration moved under us (sample rate or channel count). The message thread
    // is rebuilding this instance; pass audio through until it does.
    if (prepared->configEpoch != configEpoch.load (std::memory_order_relaxed))
        return;

    const int channels = buffer.getNumChannels();

    // The host changed our width without a matching rebuild; pass through rather than
    // hand the instance a buffer shaped differently from the one it was prepared for.
    if (prepared->hostChannels != channels)
        return;

    // Always feed the compensation delay so bypass can engage without a gap; only
    // read from it while bypassed.
    prepared->bypassDelay.run (buffer, numSamples, ! enabled);

    if (! enabled)
        return;

    const int chunk = juce::jmax (1, prepared->maxBlockSize);

    const int mainChannels = prepared->mainChannels;
    auto& scratch = prepared->ioScratch;

    for (int offset = 0; offset < numSamples; offset += chunk)
    {
        const int count = juce::jmin (chunk, numSamples - offset);

        // --- our channels into the instance's main bus
        if (mainChannels == 1 && channels > 1)
        {
            // Mono-only plugin in a stereo chain: collapse in, fan back out.
            auto* mono = scratch.getWritePointer (0);
            const float scale = 1.0f / (float) channels;

            for (int n = 0; n < count; ++n)
            {
                float sum = 0.0f;

                for (int ch = 0; ch < channels; ++ch)
                    sum += buffer.getReadPointer (ch)[offset + n];

                mono[n] = sum * scale;
            }
        }
        else
        {
            for (int ch = 0; ch < mainChannels; ++ch)
                juce::FloatVectorOperations::copy (scratch.getWritePointer (ch),
                                                   buffer.getReadPointer (juce::jmin (ch, channels - 1)) + offset,
                                                   count);
        }

        // Sidechain/aux channels the plugin insisted on keeping are fed silence. We
        // advertise no sidechain, so silence is the honest input — but the channels
        // must exist and be defined, not merely be present.
        for (int ch = mainChannels; ch < prepared->ioChannels; ++ch)
            juce::FloatVectorOperations::clear (scratch.getWritePointer (ch), count);

        juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), prepared->ioChannels, 0, count);

        prepared->midiScratch.clear();
        prepared->processor->setPlayHead (playHead);
        prepared->processor->processBlock (view, prepared->midiScratch);

        // --- the instance's main bus back into our buffer
        for (int ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::copy (buffer.getWritePointer (ch) + offset,
                                               scratch.getReadPointer (juce::jmin (ch, mainChannels - 1)),
                                               count);
    }
}

int FxHost::latencyFor (int index) const noexcept
{
    if (! validIndex (index))
        return 0;

    return slots[(size_t) index]->latencySamples.load (std::memory_order_relaxed);
}

//==============================================================================
bool FxHost::prepare (double sampleRate, int numChannels, int maxBlockSize)
{
    // Block size deliberately does not invalidate: process() chunks to whatever block
    // size each instance was actually prepared for, so a larger host buffer is handled
    // without touching the instance. Only rate and width force a rebuild.
    const bool invalidates = (! juce::approximatelyEqual (sampleRate, hostSampleRate))
                             || numChannels != hostChannels;

    hostSampleRate = sampleRate;
    hostChannels = numChannels;
    hostBlockSize = juce::jmax (32, maxBlockSize);

    if (invalidates)
        configEpoch.fetch_add (1, std::memory_order_acq_rel);

    return invalidates;
}

bool FxHost::isLive (int index) const noexcept
{
    if (! validIndex (index))
        return false;

    return slots[(size_t) index]->live_.load (std::memory_order_acquire);
}

juce::AudioProcessor* FxHost::peekInstance (int index) const noexcept
{
    if (! validIndex (index))
        return nullptr;

    auto* newest = slots[(size_t) index]->newest;
    return newest != nullptr ? newest->processor.get() : nullptr;
}

void FxHost::setInstance (int index, std::unique_ptr<Prepared> prepared)
{
    if (! validIndex (index))
        return;

    auto& slot = *slots[(size_t) index];

    if (slot.newest != nullptr && onSlotRetiring != nullptr)
        onSlotRetiring (index);

    collectGarbage();

    slot.latencySamples.store (prepared != nullptr ? prepared->latencySamples : 0,
                               std::memory_order_relaxed);
    slot.newest = prepared.get();

    // Published here rather than when the audio thread adopts the instance: "live"
    // means a prepared instance is committed to this slot, which is what the UI and
    // the latency report need to know. Waiting for adoption would leave a freshly
    // loaded plugin looking dead until the host next pulls a block, and a host with a
    // stopped transport may not pull one for a long time.
    slot.live_.store (true, std::memory_order_release);

    // Anything staged but not yet adopted has never been visible to the audio thread,
    // so deleting it here is safe and keeps the handoff single-slot.
    delete slot.staged.exchange (prepared.release(), std::memory_order_acq_rel);
}

juce::String FxHost::loadInstance (int index, std::unique_ptr<juce::AudioProcessor> instance,
                                   const juce::MemoryBlock* stateToRestore)
{
    if (! validIndex (index))
        return "Invalid slot";

    juce::String error;
    auto prepared = makePrepared (std::move (instance), stateToRestore, error);

    if (prepared == nullptr)
        return error.isNotEmpty() ? error : juce::String ("Could not prepare this plugin");

    setInstance (index, std::move (prepared));
    return {};
}

void FxHost::clear (int index)
{
    if (! validIndex (index))
        return;

    auto& slot = *slots[(size_t) index];

    if (slot.newest != nullptr && onSlotRetiring != nullptr)
        onSlotRetiring (index);

    collectGarbage();

    slot.newest = nullptr;
    slot.latencySamples.store (0, std::memory_order_relaxed);
    slot.live_.store (false, std::memory_order_release);

    delete slot.staged.exchange (nullptr, std::memory_order_acq_rel);
    slot.shouldRemove.store (true, std::memory_order_release);
}

void FxHost::collectGarbage()
{
    for (auto& slot : slots)
        for (auto& parked : slot->garbage)
            delete parked.exchange (nullptr, std::memory_order_acq_rel);
}

//==============================================================================
std::unique_ptr<FxHost::Prepared> FxHost::makePrepared (std::unique_ptr<juce::AudioProcessor> instance,
                                                        const juce::MemoryBlock* stateToRestore,
                                                        juce::String& error) const
{
    if (instance == nullptr)
    {
        error = "No plugin instance";
        return nullptr;
    }

    const int channels = juce::jmax (1, hostChannels);
    bool collapseToMono = false;

    // Set the main input and output to `wanted`. `disableAux` controls what happens to
    // any sidechain/aux buses: we would rather leave them at whatever the plugin chose
    // for itself, because a great many AudioUnits (most of Native Instruments' and
    // Plugin Alliance's, among others) publish channel configurations that simply have
    // no representation for a disabled aux bus, and asking for one gets the whole
    // layout refused. Disabling is only worth trying as a fallback.
    const auto tryLayout = [&instance] (int wanted, bool disableAux)
    {
        auto layout = instance->getBusesLayout();
        const auto set = juce::AudioChannelSet::canonicalChannelSet (wanted);

        if (layout.inputBuses.size() > 0)
            layout.inputBuses.getReference (0) = set;

        if (layout.outputBuses.size() > 0)
            layout.outputBuses.getReference (0) = set;

        if (disableAux)
        {
            for (int i = 1; i < layout.inputBuses.size(); ++i)
                layout.inputBuses.getReference (i) = juce::AudioChannelSet::disabled();

            for (int i = 1; i < layout.outputBuses.size(); ++i)
                layout.outputBuses.getReference (i) = juce::AudioChannelSet::disabled();
        }

        return instance->checkBusesLayoutSupported (layout) && instance->setBusesLayout (layout);
    };

    // Our width first, then mono. Nothing else: a plugin that accepts neither has no
    // sensible place in a strictly serial guitar chain, and silently running it at some
    // other width would be worse than refusing.
    const auto negotiate = [&tryLayout] (int wanted)
    {
        return tryLayout (wanted, false) || tryLayout (wanted, true);
    };

    if (! negotiate (channels))
    {
        if (channels == 1 || ! negotiate (1))
        {
            error = "This plugin does not support a "
                    + juce::String (channels == 1 ? "mono" : "stereo") + " layout";
            return nullptr;
        }

        collapseToMono = true;
    }

    auto prepared = std::make_unique<Prepared>();
    prepared->hostChannels = channels;
    prepared->mainChannels = collapseToMono ? 1 : channels;
    prepared->configEpoch = configEpoch.load (std::memory_order_relaxed);
    prepared->maxBlockSize = hostBlockSize;

    instance->setRateAndBufferSizeDetails (hostSampleRate, prepared->maxBlockSize);
    instance->prepareToPlay (hostSampleRate, prepared->maxBlockSize);

    // State goes in after prepare (plugins size their buffers there) but before the
    // latency read, because restoring a preset can change the reported latency — a
    // linear-phase mode, an oversampling factor, a lookahead setting.
    if (stateToRestore != nullptr && stateToRestore->getSize() > 0)
        instance->setStateInformation (stateToRestore->getData(), (int) stateToRestore->getSize());

    prepared->latencySamples = juce::jmax (0, instance->getLatencySamples());

    // Size the instance's buffer from what IT ended up with, not from what we asked
    // for. Plugins that would not give up a sidechain bus still have it enabled here,
    // and JUCE's AU host will index into those channels whether we feed them or not.
    prepared->ioChannels = juce::jmax (prepared->mainChannels,
                                       instance->getTotalNumInputChannels(),
                                       instance->getTotalNumOutputChannels());

    prepared->ioScratch.setSize (prepared->ioChannels, prepared->maxBlockSize, false, true, true);
    prepared->ioScratch.clear();

    // Generous: a MIDI-producing effect writes its output into this buffer on the
    // audio thread, and an undersized one would reallocate there.
    prepared->midiScratch.ensureSize (4096);
    prepared->bypassDelay.resize (channels, prepared->latencySamples);

    prepared->processor = std::move (instance);
    return prepared;
}
} // namespace tubamp
