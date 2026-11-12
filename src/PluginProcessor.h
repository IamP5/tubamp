#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "Parameters.h"
#include "dsp/ChainOrder.h"
#include "dsp/NamEngine.h"
#include "dsp/FxBlocks.h"
#include "dsp/Compressor.h"
#include "dsp/Drive.h"
#include "dsp/Eq.h"
#include "dsp/Gate.h"
#include "dsp/FxHost.h"
#include "dsp/FxCatalog.h"
#include "library/ModelLibrary.h"
#include "library/PresetManager.h"
#include "library/Tone3000Client.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace tubamp
{
static_assert (params::maxInstances == chain::maxInstancesPerKind,
               "the parameter pool and the block-instance pool must be the same size");

/** The legacy pin (docs/FX-ENGINES.md §2.7.5). A new type/mode/voicing id defaults to
    the recommended type, but state written before the id existed was made with Classic:
    when a restore lacks one of these ids and the block instance it belongs to is in the
    restored chain, the id is set to 0 instead of its default. Blocks outside the chain
    keep the default, so adding one later starts on the recommended type. Keyed on the
    id's absence, never on the state version. */
struct LegacyTypePin
{
    chain::BlockId kind;
    const char* const* ids; // instance-indexed, chain::numInstancesOf (kind) entries
};

inline constexpr LegacyTypePin kLegacyTypePins[] {
    { chain::BlockId::gate,  &params::gateMode },
    { chain::BlockId::comp,  params::compTypeIds },
    { chain::BlockId::drive, params::driveTypeIds },
    { chain::BlockId::eq,    params::eqTypeIds },
    { chain::BlockId::mod,   params::modChorusModeIds },
    { chain::BlockId::mod,   params::modPhaserModelIds },
    { chain::BlockId::mod,   params::modTremVoiceIds },
};

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
    setLatencySamples from the message thread only: the audio thread publishes what it applies
    (audioLatency) and a timer hands it to the host; with no audio running, the message thread
    works it out itself from the chain and the parameters.
*/
class TubampAudioProcessor : public juce::AudioProcessor,
                             private juce::AudioProcessorValueTreeState::Listener,
                             private juce::AsyncUpdater,
                             private juce::Timer
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

    /** captureStateVar()'s shape for a state other than the live one: every parameter at its
        default with `values` (id, plain value) on top, `order` as the chain with auto rows, and
        no model, IR or hosted plugin. Reads and changes no live state, so the factory presets
        are written from it. */
    juce::var makeStateVar (const chain::Order& order,
                            const std::vector<std::pair<juce::String, float>>& values) const;

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

    /** Dynamics metering (docs/FX-ENGINES.md §2.8), the same contract as the peaks: the
        audio thread max-accumulates, the editor reads with exchange. Gain reductions are
        dB >= 0, the gate's detector level is dBFS with this floor; gateState is a plain
        store of the last block's (0 closed, 1 hold, 2 open), -1 while the gate is absent,
        off or waiting for its re-entry reset. A compressor that does not run this block
        accumulates nothing, so it reads 0. */
    static constexpr float kDynMeterFloorDb = -120.0f;
    std::atomic<float> gateGrDb { 0.0f }, gateLevelDb { kDynMeterFloorDb };
    std::atomic<int> gateState { -1 };
    std::array<std::atomic<float>, params::maxInstances> compGrDb {};

private:
    /** Any thread: the chain, the state or a model changed. The timer reports the new figure at
        its next tick. */
    void updateLatency();

    /** The timer's tick (message thread), the one place setLatencySamples is called, once per
        tick at most and only when the figure changed: while audio runs, the audio thread's
        figure from a callback that saw the last edit; with none, the message thread's own. */
    void reportLatency();
    void timerCallback() override;

    /** DriveFx's type switch as it moves the drive's output in time (Drive.cpp, DriveFx::Impl::
        beginSwitch / processSwitch / crossfade), mirrored here because the engine tells only the
        latency of the type it is heading for. Fed the calls the engine gets, it gives the output
        delay at every sample: the outgoing type's latency, a rise slid in over the 10 ms the
        incoming type settles unheard, both at the larger latency through the 20 ms fade, a drop
        slid out over the 10 ms after it. Audio thread, allocation-free. */
    class DriveTimeline
    {
    public:
        void prepare (double sampleRate, int maximumBlockSize) noexcept;
        void reset() noexcept { snap = true; phase = Phase::steady; }
        /** One DriveFx::process() of `numSamples` with `type` set; `out` (or null) receives the
            output delay per sample. */
        void run (int type, int numSamples, float* out) noexcept;
        bool switching() const noexcept { return phase != Phase::steady; }
        /** The larger latency of a switch in progress, or the type's own. */
        int reach() const noexcept { return switching() ? common : latencyOf (active); }

    private:
        enum class Phase { steady, crossfade, slide };
        int latencyOf (int type) const noexcept { return DriveFx::latencyFor (type, sampleRate); }

        double sampleRate = 48000.0;
        int maxBlock = 512, settle = 8, fade = 8, slide = 8;
        int active = 0, position = 0, common = 0, extraOut = 0, extraIn = 0;
        Phase phase = Phase::steady;
        bool snap = true;
    };

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
                                      bool lanesRunning = true,
                                      const bool* waiting = nullptr,
                                      bool audioThread = false) const noexcept;

    /** What one chain entry delays the signal by, the term computeChainLatency sums and
        the gate's deferred curve is delayed by. Any thread (atomics only), except `waiting`
        and `audioThread`: the audio thread passes pendingReset, so a block that waits for its
        re-entry reset counts what it delays while it waits, and reads a drive's figure from
        what the drive applies (driveReported) rather than from its parameters. */
    int blockLatency (chain::BlockId id, bool ampOn, bool amp2On,
                      const bool* waiting = nullptr, bool audioThread = false) const noexcept;

    /** What the host is told: the serial sections plus the slower lane. */
    int computeWantedLatency (const chain::BlockId* order, int numBlocks,
                              bool ampOn, bool amp2On) const noexcept;

    /** Any thread: updates the message-thread mirror and makes the order visible to the
        audio thread (single atomic word). Order and rows move as one pair. */
    void publishChainOrder (const chain::Order& order, const std::vector<int>& rows);
    /** Message thread: publishes, re-reports latency, fires onChainChanged. Never
        holds chainLock across the callback. */
    void adoptChainOrder (const chain::Order& order, const std::vector<int>& rows);

    /** The play head, read once per callback. */
    struct HostTransport
    {
        double bpm = 120.0;        // 120 when the host reports no tempo (the delays' figure)
        double hostBpm = 0.0;      // the tempo as reported; 0 when none, not positive or not finite
        double ppqPosition = 0.0;  // quarter notes at the callback's first sample
        bool playing = false;      // the host is playing and reported ppqPosition
    };

    /** Audio thread: silences one block instance that is re-entering the chain, so it
        cannot replay what it was holding when it left. `snap`: a state restored while nothing
        is heard, so the block takes its on or off at once instead of fading in. Allocation-free. */
    void resetBlockInstance (chain::BlockId id, bool snap) noexcept;

    /** Any thread: what selects a block's engine type (type, voicing, colour), so a restore can
        tell a block it retyped. 0 for blocks without one. */
    int blockSignature (chain::BlockId id) const noexcept;

    /** Marks a whole-state apply (setStateInformation, applyStateVar) for its duration, and
        once more when it ends, for the audio thread (processBlock's restore branch). */
    struct RestoreScope
    {
        explicit RestoreScope (TubampAudioProcessor& p) : owner (p) { owner.stateRestoring.store (true, std::memory_order_release); }
        ~RestoreScope()
        {
            owner.stateRestored.store (true, std::memory_order_release);
            owner.stateRestoring.store (false, std::memory_order_release);
        }

        TubampAudioProcessor& owner;
    };

    /** Audio thread: block `id`'s engine processes this callback. True when it must start
        from its reset first: it holds audio from before it last stopped. Classic types too:
        a block never replays what it held when it went off. */
    bool engineStarts (chain::BlockId id) noexcept;

    /** Audio thread: runs ONE chain entry over the buffer it is handed. `buf` and
        `block` are two views of the same audio — the host buffer for a serial entry or
        a lane B entry's laneScratch view — which is what lets a block run in a lane
        without knowing it is in one. `monoScratchData` is the amp blocks' collapse
        scratch (null falls back to the target buffer's first channel). `transport` is the
        play head as processBlock read it once for every tempo-synced delay and mod block. */
    void processChainBlock (chain::BlockId id, juce::AudioBuffer<float>& buf,
                            juce::dsp::AudioBlock<float> block, int numSamples,
                            float* monoScratchData, bool gateOn, bool deferGateToAmp,
                            bool ampOn, bool amp2On, juce::AudioPlayHead* playHead,
                            const HostTransport& transport) noexcept;

    /** Audio thread: this block's engine parameters, read raw (the engines clamp). */
    GateParams gateParams() const noexcept;
    CompParams compParams (int instance) const noexcept;
    DriveParams driveParams (int instance) const noexcept;
    EqParams eqParams (int instance) const noexcept;
    ModParams modParams (int instance, const HostTransport& transport) const noexcept;

    /** Audio thread: collapse to mono -> engine -> deferred gate -> tone stack -> out
        gain -> expand, on whichever buffer `block` views. Both amp blocks run through
        here; only the amp block passes a tone stack or a deferred gate. */
    void processAmpMono (NamEngine& engine, juce::dsp::AudioBlock<float> block, int numSamples,
                         float* monoScratchData, juce::SmoothedValue<float>& inGain,
                         juce::SmoothedValue<float>& outGain, bool applyDeferredGate,
                         bool useToneStack) noexcept;

    /** Audio thread: the gate's gain curve over `block`, at the gate's own position or
        `deferred` onto the amp's mono output, through gateFader when the mode is not Classic
        (gateFading and gateFadeOn are this callback's, set by processBlock). */
    void applyGate (juce::dsp::AudioBlock<float> block, int numSamples, bool deferred) noexcept;

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

    /** Audio thread: lane A's delay minus lane B's at every sample of this block, into
        laneDelta: the lanes' blocks by blockLatency, a drive by what it applied sample by
        sample (driveApplied). */
    void computeLaneDelta (const chain::BlockId* order, const chain::Structure& structure,
                           int numSamples, bool ampOn, bool amp2On) noexcept;

    /** Audio thread: delays each lane by its share of the two lanes' latency difference
        (laneDelta), so the lanes are phase-aligned where they meet and the single latency
        the host is told about is true for both. A difference that moves smoothly (a drive's
        own slide) is followed exactly; a jump of up to kLaneSlideSamples slides over 10 ms
        from wherever the taps are; a larger one crossfades from what is heard to the new
        taps. Taps are read with 4-point Lagrange interpolation, exact at whole samples.
        Allocation-free — the rings are sized in prepareToPlay. */
    void compensateLaneLatency (juce::AudioBuffer<float>& laneA, juce::AudioBuffer<float>& laneB,
                                int numChannels, int numSamples) noexcept;

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
        std::atomic<float>* delayType[params::maxInstances] {};
        std::atomic<float>* delaySync[params::maxInstances] {};
        std::atomic<float>* delayLowCut[params::maxInstances] {};
        std::atomic<float>* delayHighCut[params::maxInstances] {};
        std::atomic<float>* delayModAmt[params::maxInstances] {};
        std::atomic<float>* delayColor[params::maxInstances] {};
        std::atomic<float>* delayDuck[params::maxInstances] {};

        std::atomic<float>* reverbOn[params::maxInstances] {};
        std::atomic<float>* reverbSize[params::maxInstances] {};
        std::atomic<float>* reverbDamping[params::maxInstances] {};
        std::atomic<float>* reverbMix[params::maxInstances] {};
        std::atomic<float>* reverbWidth[params::maxInstances] {};
        std::atomic<float>* reverbType[params::maxInstances] {};
        std::atomic<float>* reverbPreDelay[params::maxInstances] {};
        std::atomic<float>* reverbDecay[params::maxInstances] {};
        std::atomic<float>* reverbLowCut[params::maxInstances] {};
        std::atomic<float>* reverbHighCut[params::maxInstances] {};
        std::atomic<float>* reverbModAmt[params::maxInstances] {};

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

        // Gate, compressor, drive, EQ and mod engine parameters (docs/FX-ENGINES.md §2.7).
        std::atomic<float>* gateMode = nullptr;
        std::atomic<float>* gateRelease = nullptr;
        std::atomic<float>* gateHold = nullptr;
        std::atomic<float>* gateRange = nullptr;
        std::atomic<float>* gateScHpf = nullptr;

        std::atomic<float>* compType[params::maxInstances] {};
        std::atomic<float>* compKnee[params::maxInstances] {};
        std::atomic<float>* compMix[params::maxInstances] {};
        std::atomic<float>* compScHpf[params::maxInstances] {};
        std::atomic<float>* compAutoMakeup[params::maxInstances] {};

        std::atomic<float>* driveType[params::maxInstances] {};

        std::atomic<float>* eqType[params::maxInstances] {};
        std::atomic<float>* eqLowCut[params::maxInstances] {};
        std::atomic<float>* eqHighCut[params::maxInstances] {};
        std::atomic<float>* eqMidFreq[params::maxInstances] {};
        std::atomic<float>* eqMidQ[params::maxInstances] {};
        std::atomic<float>* eqTilt[params::maxInstances] {};
        std::atomic<float>* eqLevel[params::maxInstances] {};
        std::atomic<float>* eqVoice[params::maxInstances] {};
        std::atomic<float>* eqGraphic[params::maxInstances][params::numEqGraphicBands] {};

        std::atomic<float>* modChorusMode[params::maxInstances] {};
        std::atomic<float>* modPhaserModel[params::maxInstances] {};
        std::atomic<float>* modTremVoice[params::maxInstances] {};
        std::atomic<float>* modSync[params::maxInstances] {};
        std::atomic<float>* modWidth[params::maxInstances] {};
        std::atomic<float>* modTone[params::maxInstances] {};
        std::atomic<float>* modShape[params::maxInstances] {};
        std::atomic<float>* modWave[params::maxInstances] {};
        std::atomic<float>* modColor[params::maxInstances] {};
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

    /** The rate the blocks were last prepared at, for the drive's latency rule on any thread:
        getSampleRate() is only what the host last announced, and is 0 until it does. */
    std::atomic<double> preparedSampleRate { 44100.0 };

    // chain blocks — one object per instance, all prepared eagerly in prepareToPlay
    // regardless of what the current order contains.
    GateFx gate;
    std::array<CompFx, params::maxInstances> comp;
    std::array<DriveFx, params::maxInstances> drive;
    CabSim cab;
    std::array<EqFx, params::maxInstances> eq;
    /** The amp block's own tone stack, between the model's gated output and amp-out: a
        mono EqFx that always runs Classic. */
    EqFx ampEq;
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
    bool laneRingPrimed = false;

    /** The largest jump in the lanes' difference that slides rather than crossfades: a drive's
        latency (5 at most) or a Classic drive's 3 samples, never a model's or a hosted
        plugin's, whose slide would be heard as a pitch bend. */
    static constexpr double kLaneSlideSamples = 16.0;

    /** compensateLaneLatency's state. The taps' position is the signed difference (lane A's
        delay minus lane B's; lane B waits for a positive one, lane A for a negative one):
        `laneLast` is the previous sample's target, `laneOffset` what a slide still holds back
        of a jump, and up to two crossfade origins with their weights. Audio thread only. */
    std::vector<double> laneDelta;
    double laneLast = 0.0, laneOffset = 0.0, laneOffsetStart = 0.0;
    int laneSlideLength = 1, laneSlideDone = 1, laneFadeDone = 1;
    std::array<double, 2> laneFromPos {};
    std::array<double, 2> laneFromWeight {};
    int laneFromCount = 0;

    /** Per drive instance: its DriveTimeline, and two rows of driveDelays (engine delay, then
        the delay the block applied, per sample of the last callback), and the figure the audio
        thread counts for it (what it applies, a switch or slide taken at its larger end; a
        Classic drive by the old rule, 3 while drive_on and not waiting). Audio thread only. */
    std::array<DriveTimeline, params::maxInstances> driveTimeline;
    juce::AudioBuffer<float> driveDelays;
    std::array<int, params::maxInstances> driveReported {};

    // The latency hand-over (reportLatency). The audio thread stores, every callback, the
    // figure it applies with the latencyGeneration it read before decoding the chain, and
    // counts its callbacks; every change the latency depends on (the chain, the state, a model,
    // a latency parameter, on any thread) bumps the generation, so no figure computed before
    // a change is reported after it.
    std::atomic<std::uint64_t> audioLatency { 0 };
    std::atomic<std::uint32_t> latencyGeneration { 1 }, audioCallbacks { 0 };
    std::atomic<std::uint32_t> audioIdleMs { 250 };
    // Message thread only.
    std::uint32_t seenCallbacks = 0, lastAudioMs = 0, idleGeneration = 0;
    bool audioSeen = false;

    /** What the previous block ran, for spotting a block re-entering the chain.
        Audio thread only. */
    std::array<bool, chain::numBlockTypes> prevPresent {};

    /** Re-entry resets waiting to be drained, a bounded few per callback: clearing a
        4-second delay line is megabytes of memset at high sample rates, and a preset
        switch can re-enter half a dozen blocks in one block. A block whose reset is
        still pending processes as bypassed (silent, never stale). Audio thread only. */
    std::array<bool, chain::numBlockTypes> pendingReset {};

    /** A state is being applied / was applied since the last callback (RestoreScope). */
    std::atomic<bool> stateRestoring { false }, stateRestored { false };
    /** The last callback's output was not silence, so a restore lands on sound being heard.
        Audio thread only (and prepareToPlay). */
    bool outputSounding = false;
    /** blockSignature as of the last callback, per block. Audio thread only. */
    std::array<int, chain::numBlockTypes> lastSignature {};

    /** Per block: its engine processed last callback / this callback; it holds audio from
        before it last stopped. Audio thread only. */
    std::array<bool, chain::numBlockTypes> engineRan {}, engineRunsNow {}, engineStale {};

    /** How each block goes on and off (one per instance), Classic types included. */
    BypassFader gateFader;
    std::array<BypassFader, params::maxInstances> compFader, driveFader, eqFader, modFader;

    /** This callback's gate: whether it is in the chain (its gain goes through gateFader), and
        towards on or off. */
    bool gateFading = false, gateFadeOn = false;

    // Written on the message thread; also read by getStateInformation, which hosts
    // may call from a save/worker thread. juce::String is copy-on-write and not safe
    // to read while another thread reassigns it, hence the lock (never touched by
    // the audio thread).
    mutable juce::CriticalSection pathLock;
    juce::String loadedModelPath, loadedModelPathB, loadedIrPath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TubampAudioProcessor)
};
} // namespace tubamp
