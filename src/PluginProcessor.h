#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "Parameters.h"
#include "dsp/ChainOrder.h"
#include "dsp/NamEngine.h"
#include "dsp/FxBlocks.h"
#include "dsp/FxHost.h"
#include "dsp/FxCatalog.h"
#include "library/ModelLibrary.h"
#include "library/PresetManager.h"
#include "library/Tone3000Client.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace tubamp
{
static_assert (params::maxInstances == chain::maxInstancesPerKind,
               "the parameter pool and the block-instance pool must be the same size");

/**
    User-buildable chain between two fixed endpoints:

      Input Trim -> [ ordered, user-arranged blocks ] -> DC Blocker -> Output Level

    A fresh instance runs the amp alone; every other block is added by the user.
    chain::classicOrder() — what the plugin used to start with, and what a factory
    preset is built on — follows the official NAM plugin's ordering for the amp core
    (docs/research/nam-plugin-params.md §4):

      Gate trigger -> Comp -> Drive
        -> [ Amp In (+input calibration) -> NAM (mono) -> Gate gain -> Amp EQ
             -> Amp Out (+output-mode compensation) ]
        -> Cab IR -> Tone Stack -> Modulation -> Delay -> Reverb

    Blocks can be removed from the chain entirely or reordered by the user, and six of
    the kinds can appear up to three times, each instance with its own parameters (see
    dsp/ChainOrder.h); "*_on" stays a separate, automatable per-block bypass. The order
    is state, not a parameter, and reaches the audio thread as one packed 128-bit word.

    The gate is split the way the reference plugin splits it: the trigger only measures
    the signal at the gate's own position, and the reduction it computes is applied
    downstream of the model when the amp comes later in the chain (otherwise it lands
    right where the gate sits).

    Mono-collapse before NAM (models are mono), stereo expansion afterwards.
    Latency = NamEngine resampler latency, reported via setLatencySamples.
*/
class TubampAudioProcessor : public juce::AudioProcessor,
                             private juce::AudioProcessorValueTreeState::Listener,
                             private juce::AsyncUpdater
{
public:
    TubampAudioProcessor();
    ~TubampAudioProcessor() override;

    // --- AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "tubamp"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    // Full state: APVTS + model path + IR path (+ preset name)
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // --- tubamp API (message thread)
    /** Loads a .nam model (background-thread work done internally, synchronous
        call ok from the message thread). Empty return = success. */
    juce::String loadModel (const juce::File& namFile);
    void clearModel();
    juce::String getLoadedModelPath() const { return loadedModelPath; }

    juce::String loadIr (const juce::File& wavFile);
    void clearIr();
    juce::String getLoadedIrPath() const { return loadedIrPath; }

    /** State as var for A/B slots + presets (params + model + IR + chain order). */
    juce::var captureStateVar();
    void applyStateVar (const juce::var& state);

    /** Current signal-chain order. Any thread (read under chainLock). */
    chain::Order getChainOrder() const;

    /** How the order is split into rows on the board: one length per row, summing to
        the order's size. Empty means "auto" — the editor wraps the chain itself. */
    std::vector<int> getChainRows() const;

    /** Publishes a new order lock-free to the audio thread and notifies listeners.
        Message thread only; duplicates/out-of-range ids are dropped, and rows that do
        not describe the surviving order are replaced by "auto". */
    void setChainOrder (const chain::Order& order, const std::vector<int>& rows);

    /** Fired on the message thread whenever the order or its rows change — including
        after a state restore (setStateInformation / applyStateVar / preset load).
        Listeners re-read both. Set and cleared by the editor, like library.onChanged. */
    std::function<void()> onChainChanged;

    //==============================================================================
    // --- external AudioUnit slots (message thread)

    /** Everything the UI needs about one slot. */
    struct FxSlotInfo
    {
        /** PluginDescription::fileOrIdentifier of the assigned plugin, empty when the
            slot is unoccupied. */
        juce::String identifier;
        juce::String name;
        juce::String manufacturer;
        /** A plugin is assigned to this slot. */
        bool occupied = false;
        /** Assigned but not runnable — usually the plugin is not installed on this
            machine. Its saved state is kept and re-saved untouched so that opening
            the project on the machine that has it loses nothing. */
        bool missing = false;
        /** An instance is actually processing (assigned, instantiated, prepared). */
        bool live = false;
        /** Instantiation is in flight. */
        bool loading = false;
        int latencySamples = 0;
        juce::String error;
    };

    FxSlotInfo getFxSlotInfo (int slot) const;

    /** Assigns a plugin to a slot and instantiates it asynchronously. Fires
        onFxSlotChanged once when the request is accepted and again on completion. */
    void loadFxPlugin (int slot, const juce::String& identifier);

    /** Empties a slot, discarding its plugin and saved state. */
    void clearFxPlugin (int slot);

    /** The live instance of a slot, for opening its own editor. Message thread; the
        pointer is valid until the next load/clear of that slot. */
    juce::AudioProcessor* getFxInstance (int slot);

    /** Fired on the message thread whenever a slot's contents or status change. */
    std::function<void (int slot)> onFxSlotChanged;

    /** Fired immediately before a slot's instance is retired, so the editor can close
        any window showing it — an AudioProcessor must outlive its editor. Every
        mutation path (UI, preset load, A/B, host state restore) reaches this. */
    std::function<void (int slot)> onFxSlotRetiring;

    FxCatalog fxCatalog;

    /** Last editor size, so reopening the window restores it. Deliberately part of the
        plugin's own state and NOT of a preset: a preset is a sound, and recalling one
        should never resize the user's window. Zero means "never set". */
    juce::Point<int> getEditorSize() const;
    void setEditorSize (juce::Point<int> size);

    juce::AudioProcessorValueTreeState apvts;
    NamEngine namEngine;
    FxHost fxHost;
    ModelLibrary library;
    PresetManager presets;
    Tone3000Client tone3000;

    /** UI level metering (linear peak, decayed by UI). */
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };

private:
    void updateLatency();

    /** Total latency for a given topology: the NAM resampler plus every loaded FX
        slot in the path. Audio-thread safe (atomics only). */
    int computeWantedLatency (const std::array<bool, chain::numBlockTypes>& present,
                              bool ampOn) const noexcept;

    /** Any thread: updates the message-thread mirror and makes the order visible to the
        audio thread (single atomic word). Order and rows move as one pair. */
    void publishChainOrder (const chain::Order& order, const std::vector<int>& rows);
    /** Message thread: publishes, re-reports latency, fires onChainChanged. Never
        holds chainLock across the callback. */
    void adoptChainOrder (const chain::Order& order, const std::vector<int>& rows);

    /** Audio thread: silences one block instance that is re-entering the chain, so it
        cannot replay what it was holding when it left. Allocation-free. */
    void resetBlockInstance (chain::BlockId id) noexcept;

    // The two amp gains fold the model-metadata compensation into the knob value before
    // it is converted to linear, exactly as the reference plugin's _SetInputGain() /
    // _SetOutputGain() do. Audio-thread safe (atomics all the way down).
    float ampInputDb() const noexcept;
    float ampOutputDb() const noexcept;

    // amp_slim: parameterChanged can arrive on the audio thread, and
    // SlimmableModel::SetSlimmableSize is explicitly not real-time safe, so the value
    // is parked here and applied from handleAsyncUpdate() on the message thread.
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;
    std::atomic<float> pendingSlim { 0.0f };

    //==============================================================================
    // --- external AudioUnit slots

    /** What the message thread knows about a slot. The description and the state blob
        survive the instance: a slot whose plugin is missing keeps both so that saving
        the project again does not silently discard the user's settings. */
    struct FxRecord
    {
        juce::PluginDescription desc;
        juce::MemoryBlock state;
        bool occupied = false;
        bool missing = false;
        bool loading = false;
        juce::String error;

        /** Bumped by every mutation. An in-flight instantiation whose epoch no longer
            matches has been superseded (rapid A/B, preset thrash) and discards
            itself instead of landing in the slot. */
        uint32_t epoch = 0;
    };

    std::array<FxRecord, chain::numFxSlots> fxRecords;

    /** Guards fxRecords against getStateInformation, which hosts may call from a save
        thread. Never touched by the audio thread. */
    mutable juce::CriticalSection fxLock;

    /** Set when prepareToPlay saw a configuration change that invalidated the hosted
        instances; drained by handleAsyncUpdate on the message thread. */
    std::atomic<bool> fxRebuildPending { false };

    /** Guards editorSize, which getStateInformation may read off the message thread. */
    mutable juce::CriticalSection editorSizeLock;
    juce::Point<int> editorSize;

    /** Message thread: (re)instantiates the plugin assigned to a slot from its record,
        restoring `state` into it. No-op for an unoccupied slot. */
    void instantiateFxSlot (int slot);

    /** Message thread: pulls the live instance's state into the record's blob. Called
        before serializing and before rebuilding. */
    void refreshFxState (int slot);
    void refreshAllFxState();

    /** Message thread: applies a parsed set of slot records (state restore, preset,
        A/B) and kicks the instantiations. An absent/empty set clears every slot. */
    void applyFxRecords (const std::array<FxRecord, chain::numFxSlots>& incoming);

    /** One serialization shape for both containers: getStateInformation appends the
        tree as a child, captureStateVar (presets, A/B) stores its XML string. */
    juce::ValueTree fxSlotsTree() const;
    std::array<FxRecord, chain::numFxSlots> parseFxSlotsTree (const juce::ValueTree& tree) const;

    void notifyFxSlotChanged (int slot);

    // Raw APVTS value pointers, resolved once in the constructor: the audio thread
    // must never do a string lookup. Private detail, not part of the contract.
    //
    // A duplicable kind's pointers are arrays indexed by chain::instanceOf(id); index 0
    // is the kind's original, frozen parameter id.
    struct ParamPtrs
    {
        std::atomic<float>* gateOn = nullptr;
        std::atomic<float>* ampOn = nullptr;
        std::atomic<float>* cabOn = nullptr;
        std::atomic<float>* fxOn[chain::numFxSlots] { nullptr, nullptr, nullptr };

        std::atomic<float>* inputTrim = nullptr;
        std::atomic<float>* outputLevel = nullptr;

        std::atomic<float>* gateThreshold = nullptr;

        std::atomic<float>* compOn[params::maxInstances] {};
        std::atomic<float>* compThreshold[params::maxInstances] {};
        std::atomic<float>* compRatio[params::maxInstances] {};
        std::atomic<float>* compAttack[params::maxInstances] {};
        std::atomic<float>* compRelease[params::maxInstances] {};
        std::atomic<float>* compMakeup[params::maxInstances] {};

        std::atomic<float>* driveOn[params::maxInstances] {};
        std::atomic<float>* driveGain[params::maxInstances] {};
        std::atomic<float>* driveTone[params::maxInstances] {};
        std::atomic<float>* driveLevel[params::maxInstances] {};

        std::atomic<float>* ampInput = nullptr;
        std::atomic<float>* ampOutput = nullptr;
        std::atomic<float>* ampOutMode = nullptr;
        std::atomic<float>* ampCalInput = nullptr;
        std::atomic<float>* ampCalLevel = nullptr;
        std::atomic<float>* ampEqOn = nullptr;
        std::atomic<float>* ampEqBass = nullptr;
        std::atomic<float>* ampEqMid = nullptr;
        std::atomic<float>* ampEqTreble = nullptr;

        std::atomic<float>* cabLowCut = nullptr;
        std::atomic<float>* cabHighCut = nullptr;

        std::atomic<float>* eqOn[params::maxInstances] {};
        std::atomic<float>* eqBass[params::maxInstances] {};
        std::atomic<float>* eqMid[params::maxInstances] {};
        std::atomic<float>* eqTreble[params::maxInstances] {};

        std::atomic<float>* modOn[params::maxInstances] {};
        std::atomic<float>* modType[params::maxInstances] {};
        std::atomic<float>* modRate[params::maxInstances] {};
        std::atomic<float>* modDepth[params::maxInstances] {};
        std::atomic<float>* modMix[params::maxInstances] {};

        std::atomic<float>* delayOn[params::maxInstances] {};
        std::atomic<float>* delayTime[params::maxInstances] {};
        std::atomic<float>* delayFeedback[params::maxInstances] {};
        std::atomic<float>* delayMix[params::maxInstances] {};

        std::atomic<float>* reverbOn[params::maxInstances] {};
        std::atomic<float>* reverbSize[params::maxInstances] {};
        std::atomic<float>* reverbDamping[params::maxInstances] {};
        std::atomic<float>* reverbMix[params::maxInstances] {};
    };

    ParamPtrs pp;

    // Guards deferred (callAsync) work against destruction of this processor.
    std::shared_ptr<bool> aliveFlag { std::make_shared<bool> (true) };

    // The order the audio thread runs: one packed word, stored whole and decoded once
    // per processBlock, so a reader can never see a half-written chain.
    std::atomic<chain::Packed> packedChain { chain::pack (chain::defaultOrder()) };

    // The message-thread-facing mirror of the same chain, plus the row layout the audio
    // thread has no use for. Guarded because hosts save and restore state from their own
    // threads, and order and rows must never be read as a mismatched pair. A LEAF lock:
    // nothing is called while it is held (onChainChanged least of all).
    mutable juce::CriticalSection chainLock;
    chain::Order uiChainOrder { chain::defaultOrder() };
    std::vector<int> uiChainRows;

    // chain blocks — one object per instance, all prepared eagerly in prepareToPlay
    // regardless of what the current order contains.
    NoiseGate gate;
    std::array<juce::dsp::Compressor<float>, params::maxInstances> compressor;
    std::array<juce::SmoothedValue<float>, params::maxInstances> compMakeupLin { {
        juce::SmoothedValue<float> { 1.0f }, juce::SmoothedValue<float> { 1.0f },
        juce::SmoothedValue<float> { 1.0f } } };
    std::array<Drive, params::maxInstances> drive;
    CabSim cab;
    std::array<ToneStackEQ, params::maxInstances> eq;
    /** The amp block's own tone stack, between the model's gated output and amp-out. */
    ToneStackEQ ampEq;
    DcBlocker dcBlocker;
    std::array<Modulation, params::maxInstances> modulation;
    std::array<DelayFx, params::maxInstances> delay;
    std::array<ReverbFx, params::maxInstances> reverbFx;
    juce::SmoothedValue<float> inputTrimLin { 1.0f }, outputLevelLin { 1.0f }, ampInLin { 1.0f }, ampOutLin { 1.0f };

    juce::AudioBuffer<float> monoScratch;

    /** What the previous block ran, for spotting a block re-entering the chain.
        Audio thread only. */
    std::array<bool, chain::numBlockTypes> prevPresent {};

    /** Re-entry resets waiting to be drained, a bounded few per callback: clearing a
        2-second delay line is megabytes of memset at high sample rates, and a preset
        switch can re-enter half a dozen blocks in one block. A block whose reset is
        still pending processes as bypassed (silent, never stale). Audio thread only. */
    std::array<bool, chain::numBlockTypes> pendingReset {};

    // Written on the message thread; also read by getStateInformation, which hosts
    // may call from a save/worker thread. juce::String is copy-on-write and not safe
    // to read while another thread reassigns it, hence the lock (never touched by
    // the audio thread).
    mutable juce::CriticalSection pathLock;
    juce::String loadedModelPath, loadedIrPath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TubampAudioProcessor)
};
} // namespace tubamp
