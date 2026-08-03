#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "ChainOrder.h"

#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace tubamp
{
/**
    The three external-AudioUnit slots (chain::BlockId::fx1..fx3).

    Each slot hosts one third-party plugin instance somewhere in the user-arranged
    chain. An empty slot is a bit-exact pass-through, which is what makes it safe for
    the chain order to be published to the audio thread long before the instances
    behind it exist (state restore relies on exactly that).

    Threading contract — this is the load-bearing part:

      - process() / applyStaging() / latencyFor() are AUDIO THREAD only.
        applyStaging() must run before the first process() of each block.
      - setInstance() / clear() / prepare() / collectGarbage() / everything else is
        MESSAGE THREAD only.
      - Instances are handed to the audio thread through a single-slot atomic
        exchange (the NamEngine staging pattern, dsp/NamEngine.cpp). Ownership moves
        with the exchange, so the two threads never touch the same object.

    Two AudioUnit-specific rules the generic pattern does not imply:

      1. A hosted instance is NEVER destroyed on the audio thread. JUCE's AU wrapper
         posts an AUDeleter and blocks on it when the destructor runs off the message
         thread (juce_AudioUnitPluginFormatImpl.h:876-886), which would stall render.
         retire() therefore only parks pointers; collectGarbage() deletes them. On
         garbage overflow we leak deliberately rather than delete in the wrong place.
      2. A hosted instance is NEVER prepared from our prepareToPlay(). Our own AU
         wrapper calls prepareToPlay while holding the callback lock on the
         kAudioUnitProperty_OfflineRender path (juce_audio_plugin_client_AU_1.mm:794-809,
         which Logic hits on every bounce/freeze), and AudioUnitInitialize takes a
         CoreAudio mutex the audio thread already holds — a textbook ABBA deadlock.
         prepare() only records the configuration; slots whose instance was prepared
         for a different configuration pass audio through until the message thread
         has rebuilt them.

    The message thread may read the most recently staged instance of a slot (to
    serialize its state). That is safe because an instance only becomes garbage when
    a *successor* is staged, which only the message thread does — so the newest
    instance of a slot is never concurrently being destroyed.
*/
class FxHost
{
public:
    FxHost();
    ~FxHost();

    static constexpr int numSlots = chain::numFxSlots;

    /** A slot's instance together with everything preallocated for it. Built and
        destroyed on the message thread only; handed to the audio thread whole. */
    struct Prepared;

    //==============================================================================
    // --- audio thread

    /** Adopts anything staged and parks what it replaces. Call once per block,
        before the first process(). */
    void applyStaging() noexcept;

    /** Runs slot `index` over `buffer` in place. A slot with no usable instance is a
        pass-through; a bypassed slot with a latent instance is delayed by that
        instance's latency so that reported latency does not move when the (fully
        automatable) bypass parameter is toggled. */
    void process (int index, juce::AudioBuffer<float>& buffer, int numSamples,
                  bool enabled, juce::AudioPlayHead* playHead) noexcept;

    /** Latency of the instance currently live in the slot, 0 when empty. Independent
        of the bypass parameter by design — see process(). */
    int latencyFor (int index) const noexcept;

    //==============================================================================
    // --- message thread

    /** Records the host configuration. Deliberately does NOT touch instances (see the
        class comment); returns true when the change invalidates prepared instances,
        i.e. when the caller must rebuild them.

        Block-size changes alone never invalidate anything: process() chunks to the
        block size each instance was actually prepared for. */
    bool prepare (double sampleRate, int numChannels, int maxBlockSize);

    double getSampleRate() const noexcept  { return hostSampleRate; }
    int getNumChannels() const noexcept    { return hostChannels; }

    /** True when the slot holds an instance that was prepared for the current
        configuration (i.e. it is actually processing). */
    bool isLive (int index) const noexcept;

    /** Negotiates `instance`'s bus layout against the current configuration, prepares
        it, restores `stateToRestore` into it and publishes it into the slot, retiring
        whatever was there. Returns an empty string on success, or a message describing
        why the plugin cannot run in this chain.

        Message thread, no locks held: this calls prepareToPlay on a hosted AudioUnit,
        which must never happen with the callback lock taken. */
    juce::String loadInstance (int index, std::unique_ptr<juce::AudioProcessor> instance,
                               const juce::MemoryBlock* stateToRestore);

    /** Stages removal of the slot's instance. Fires onSlotRetiring() first. */
    void clear (int index);

    /** Destroys instances parked by the audio thread. Safe to call at any time on the
        message thread; called automatically before every slot mutation. */
    void collectGarbage();

    /** The instance most recently staged into the slot, or nullptr. Message thread
        only, and only valid until the next setInstance()/clear() for that slot —
        long enough to serialize state. */
    juce::AudioProcessor* peekInstance (int index) const noexcept;

    /** Fired on the message thread immediately before a slot's instance is retired,
        so an open editor window for it can be closed first: an AudioProcessor must
        outlive its editor. Every mutation path (UI, preset load, A/B, host state
        restore) goes through the same two entry points above, so this is the single
        chokepoint. */
    std::function<void (int index)> onSlotRetiring;

private:
    struct Slot;

    /** Builds a Prepared for `instance`: bus negotiation, prepareToPlay, state
        restore, scratch/delay allocation. nullptr with `error` set on refusal. */
    std::unique_ptr<Prepared> makePrepared (std::unique_ptr<juce::AudioProcessor> instance,
                                            const juce::MemoryBlock* stateToRestore,
                                            juce::String& error) const;

    /** Publishes a prepared instance into the slot, retiring whatever was there. */
    void setInstance (int index, std::unique_ptr<Prepared> prepared);

    std::array<std::unique_ptr<Slot>, numSlots> slots;

    double hostSampleRate = 48000.0;
    int hostChannels = 2;
    int hostBlockSize = 512;

    /** Bumped whenever a configuration change invalidates prepared instances. The
        audio thread bypasses any instance whose stamp does not match. */
    std::atomic<int> configEpoch { 0 };

    static bool validIndex (int index) noexcept
    {
        return index >= 0 && index < numSlots;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxHost)
};
} // namespace tubamp
