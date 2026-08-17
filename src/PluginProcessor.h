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

    Mono-collapse before NAM (models are mono), stereo expansion afterwards — for both
    amp blocks: `amp` runs engine A with the amp tone stack, `amp2` runs engine B with
    its own in/out gains and no tone stack.

    The order may also contain ONE parallel region, `split, <lane A>, lane2, <lane B>,
    mix` (docs/SPLIT.md): lane A runs on the host buffer, lane B on laneScratch, and the
    mixer sums them back with per-lane level/pan and lane latency compensation. An order
    without that structure runs exactly the single serial pass it always did.

    Latency = the serial section's latency plus the slower lane's, reported via
    setLatencySamples.
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

    /** Engine B — the model the `amp2` block runs. Same contract as loadModel(); A and B
        are independent, clearing one never touches the other, and an empty B simply
        makes the amp2 block a pass-through. */
    juce::String loadModelB (const juce::File& namFile);
    void clearModelB();
    juce::String getLoadedModelPathB() const { return loadedModelPathB; }
    NamEngine::ModelInfo getModelInfoB() const { return namEngineB.getModelInfo(); }

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
    /** The amp2 block's engine. Idle (and costing nothing) until a model is loaded
        into it. */
    NamEngine namEngineB;
    FxHost fxHost;
    ModelLibrary library;
    PresetManager presets;
    Tone3000Client tone3000;

    /** UI level metering (linear peak, decayed by UI). */
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };

private:
    void updateLatency();

    /** Ring capacity for the lanes' latency compensation, and therefore the largest
        lane-to-lane difference that can actually be aligned. Only the DIFFERENCE is
        stored, but that difference is not always small: a hosted linear-phase EQ or a
        lookahead limiter in one lane reports tens of thousands of samples, so the ring
        is sized to swallow those rather than to the tens of samples a resampler costs.
        512 kB per lane, allocated once in prepareToPlay. */
    static constexpr int kLaneLatencyRingSamples = 65536;

    /** How much latency each section of an order carries. Lanes run side by side, so
        the two of them cost whatever the slower one costs — but only after everything
        serial has been paid for. */
    struct ChainLatency
    {
        int serial = 0, laneA = 0, laneB = 0;

        /** What the host is told. The lanes cost the slower one — but only as far as
            the compensation ring can actually hold the faster one back; beyond that the
            lanes cannot be aligned at all, and the honest figure is the one that is true
            for the lane that WAS compensated, not a promise the mixer did not keep. */
        int reported() const noexcept
        {
            const int shorter = juce::jmin (laneA, laneB);
            const int longer  = juce::jmax (laneA, laneB);

            return serial + shorter
                 + juce::jmin (longer - shorter, kLaneLatencyRingSamples - 1);
        }
    };

    /** The one lane-aware latency rule, shared by the audio thread (decoded order) and
        the message thread (the mirror under chainLock) so the two can never disagree.
        `lanesRunning` is the audio thread's own verdict on whether the split region can
        run this block (it also needs lane scratch big enough); false folds both lanes
        into the serial sum, which is what a flat pass actually costs. Audio-thread safe
        (atomics only). */
    ChainLatency computeChainLatency (const chain::BlockId* order, int numBlocks,
                                      bool ampOn, bool amp2On,
                                      bool lanesRunning = true) const noexcept;

    /** What the host is told: the serial sections plus the slower lane. */
    int computeWantedLatency (const chain::BlockId* order, int numBlocks,
                              bool ampOn, bool amp2On) const noexcept;

    /** Any thread: updates the message-thread mirror and makes the order visible to the
        audio thread (single atomic word). Order and rows move as one pair. */
    void publishChainOrder (const chain::Order& order, const std::vector<int>& rows);
    /** Message thread: publishes, re-reports latency, fires onChainChanged. Never
        holds chainLock across the callback. */
    void adoptChainOrder (const chain::Order& order, const std::vector<int>& rows);

    /** Audio thread: silences one block instance that is re-entering the chain, so it
        cannot replay what it was holding when it left. Allocation-free.

        Returns false when the instance is only PARTLY cleared and wants another call —
        the reverb clears one delay line per step (docs/REVERB.md §6.3) rather than
        memsetting megabytes inline. The drain loop re-arms pendingReset while that is
        the case, so a chunked clear costs one slot of kMaxResetsPerCallback per
        callback and never more. Every other block clears in one call and returns true. */
    bool resetBlockInstance (chain::BlockId id) noexcept;

    /** Any thread: arms docs/REVERB.md §6.4's snap flag on every reverb instance, so the
        next block each one runs adopts its parameters outright instead of sliding Size
        and crossfading the pre-delay pointer into them. For the state paths and
        prepareToPlay — resetBlockInstance arms only the instance it cleared. */
    void armReverbSnap() noexcept;

    /** Audio thread: runs ONE chain entry over the buffer it is handed. `buf` and
        `block` are two views of the same audio — the host buffer for a serial entry or
        a lane B entry's laneScratch view — which is what lets a block run in a lane
        without knowing it is in one. `monoScratchData` is the amp blocks' collapse
        scratch (null falls back to the target buffer's first channel). */
    void processChainBlock (chain::BlockId id, juce::AudioBuffer<float>& buf,
                            juce::dsp::AudioBlock<float> block, int numSamples,
                            float* monoScratchData, bool gateOn, bool deferGateToAmp,
                            bool ampOn, bool amp2On, juce::AudioPlayHead* playHead) noexcept;

    /** Audio thread: collapse to mono -> engine -> deferred gate -> tone stack -> out
        gain -> expand, on whichever buffer `block` views. Both amp blocks run through
        here; only the amp block passes a tone stack or a deferred gate. */
    void processAmpMono (NamEngine& engine, juce::dsp::AudioBlock<float> block, int numSamples,
                         float* monoScratchData, juce::SmoothedValue<float>& inGain,
                         juce::SmoothedValue<float>& outGain, bool applyDeferredGate,
                         bool useToneStack) noexcept;

    /** Audio thread: fills `lane` from `main` per split_mode, leaving lane A in `main`.
        Allocation-free; the X-Over filters are prepared in prepareToPlay. */
    void splitLanes (juce::AudioBuffer<float>& main, juce::AudioBuffer<float>& lane,
                     int numChannels, int numSamples) noexcept;

    /** Audio thread: sums `lane` back into `main` through the mixer's smoothed
        level/pan gains. Ramps are materialized into monoScratch (idle here) because a
        SmoothedValue must advance exactly numSamples per block, never once per
        channel. */
    void mixLanes (juce::AudioBuffer<float>& main, juce::AudioBuffer<float>& lane,
                   int numChannels, int numSamples) noexcept;

    /** Audio thread: delays each lane by its share of the two lanes' latency
        difference, so the lanes are phase-aligned where they meet and the single
        latency the host is told about is true for both. Allocation-free — the rings
        are sized in prepareToPlay. */
    void compensateLaneLatency (juce::AudioBuffer<float>& laneA, juce::AudioBuffer<float>& laneB,
                                int numChannels, int numSamples, int delayA, int delayB) noexcept;

    // The two amp gains fold the model-metadata compensation into the knob value before
    // it is converted to linear, exactly as the reference plugin's _SetInputGain() /
    // _SetOutputGain() do. Audio-thread safe (atomics all the way down).
    float ampInputDb() const noexcept;
    float ampOutputDb() const noexcept;
    float amp2InputDb() const noexcept;
    float amp2OutputDb() const noexcept;

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
        std::atomic<float>* delayMode[params::maxInstances] {};
        std::atomic<float>* delayRatio[params::maxInstances] {};
        std::atomic<float>* delayWidth[params::maxInstances] {};

        std::atomic<float>* reverbOn[params::maxInstances] {};
        std::atomic<float>* reverbSize[params::maxInstances] {};
        std::atomic<float>* reverbDamping[params::maxInstances] {};
        std::atomic<float>* reverbMix[params::maxInstances] {};
        std::atomic<float>* reverbWidth[params::maxInstances] {};
        std::atomic<float>* reverbAlgo[params::maxInstances] {};
        std::atomic<float>* reverbDecay[params::maxInstances] {};
        std::atomic<float>* reverbPredelay[params::maxInstances] {};
        std::atomic<float>* reverbDiffusion[params::maxInstances] {};
        std::atomic<float>* reverbLowCut[params::maxInstances] {};
        std::atomic<float>* reverbHighCut[params::maxInstances] {};
        std::atomic<float>* reverbMod[params::maxInstances] {};
        std::atomic<float>* reverbBassMult[params::maxInstances] {};
        std::atomic<float>* reverbErLevel[params::maxInstances] {};
        std::atomic<float>* reverbColor[params::maxInstances] {};
        std::atomic<float>* reverbTilt[params::maxInstances] {};
        std::atomic<float>* reverbDuck[params::maxInstances] {};
        std::atomic<float>* reverbShimmer[params::maxInstances] {};
        std::atomic<float>* reverbShimmerInterval[params::maxInstances] {};

        std::atomic<float>* amp2On = nullptr;
        std::atomic<float>* amp2Input = nullptr;
        std::atomic<float>* amp2Output = nullptr;

        std::atomic<float>* splitOn = nullptr;
        std::atomic<float>* splitMode = nullptr;
        std::atomic<float>* splitXover = nullptr;

        std::atomic<float>* mixOn = nullptr;
        std::atomic<float>* mixALevel = nullptr;
        std::atomic<float>* mixBLevel = nullptr;
        std::atomic<float>* mixAPan = nullptr;
        std::atomic<float>* mixBPan = nullptr;
        std::atomic<float>* mixBPhase = nullptr;
        std::atomic<float>* mixLevel = nullptr;
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
    /** The amp2 block's own gains — never the amp block's: two SmoothedValues cannot
        share one, each has to advance exactly numSamples per block. */
    juce::SmoothedValue<float> amp2InLin { 1.0f }, amp2OutLin { 1.0f };

    // The mixer's gains: one per lane per channel (level x equal-power pan, sign-flipped
    // for lane B's phase invert), plus the master. Their own smoothers for the same
    // reason as amp2's.
    std::array<juce::SmoothedValue<float>, 2> mixAGain { { juce::SmoothedValue<float> { 1.0f },
                                                           juce::SmoothedValue<float> { 1.0f } } };
    std::array<juce::SmoothedValue<float>, 2> mixBGain { { juce::SmoothedValue<float> { 1.0f },
                                                           juce::SmoothedValue<float> { 1.0f } } };
    juce::SmoothedValue<float> mixLevelLin { 1.0f };

    juce::AudioBuffer<float> monoScratch;

    /** Lane B's audio. Sized in prepareToPlay like monoScratch; processBlock takes a
        non-owning view of it with the host's channel count, so a lane block (FxHost
        above all) sees exactly the geometry it sees on the main buffer. */
    juce::AudioBuffer<float> laneScratch;

    // X-Over split: LR4 pair, lane A takes the lows and lane B the highs.
    juce::dsp::LinkwitzRileyFilter<float> splitLow, splitHigh;
    /** Nyquist guard for split_xover, from the last prepareToPlay. */
    float maxSplitCutoffHz = 20000.0f;

    // Latency-compensation rings for the two lanes: one 2-channel ring each, written and
    // read by compensateLaneLatency. Sized once in prepareToPlay, big enough that a delay
    // is never clamped in practice (a lane's excess latency is a resampler's or a hosted
    // plugin's, not thousands of samples). `primed` is false whenever the lanes did not
    // run last block, so re-entering them cannot replay what the rings were holding.
    std::array<juce::AudioBuffer<float>, 2> laneRing;
    int laneRingWrite = 0;
    std::array<int, 2> laneRingDelays { 0, 0 };
    bool laneRingPrimed = false;

    /** What the previous block ran, for spotting a block re-entering the chain.
        Audio thread only. */
    std::array<bool, chain::numBlockTypes> prevPresent {};

    /** Re-entry resets waiting to be drained, a bounded few per callback: clearing a
        4-second delay line is megabytes of memset at high sample rates, and a preset
        switch can re-enter half a dozen blocks in one block. A block whose reset is
        still pending processes as bypassed (silent, never stale). Audio thread only. */
    std::array<bool, chain::numBlockTypes> pendingReset {};

    /** docs/REVERB.md §6.4: "glide nothing on the next block" for one reverb instance.
        Size is slew-limited and the pre-delay pointer crossfades, which is right for a
        knob and wrong for a state change — a preset recall would rubber-band into its
        stored size. Set for one block after resetBlockInstance for that instance, at the
        end of applyStateVar and setStateInformation, and for the first block after
        prepareToPlay; consumed (and cleared) where the instance's parameters are read.
        Atomic because the two state paths run on the message thread and the host's own
        restore threads, while the consumer is the audio thread. */
    std::array<std::atomic<bool>, params::maxInstances> reverbSnap {};

    // Written on the message thread; also read by getStateInformation, which hosts
    // may call from a save/worker thread. juce::String is copy-on-write and not safe
    // to read while another thread reassigns it, hence the lock (never touched by
    // the audio thread).
    mutable juce::CriticalSection pathLock;
    juce::String loadedModelPath, loadedModelPathB, loadedIrPath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TubampAudioProcessor)
};
} // namespace tubamp
