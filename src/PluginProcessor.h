#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "Parameters.h"
#include "dsp/ChainOrder.h"
#include "dsp/NamEngine.h"
#include "dsp/FxBlocks.h"
#include "library/ModelLibrary.h"
#include "library/PresetManager.h"
#include "library/Tone3000Client.h"

#include <atomic>
#include <cstdint>
#include <functional>

namespace tubamp
{
/**
    User-buildable chain between two fixed endpoints:

      Input Trim -> [ ordered, user-arranged blocks ] -> DC Blocker -> Output Level

    The default order follows the official NAM plugin's ordering for the amp core
    (docs/research/nam-plugin-params.md §4):

      Gate trigger -> Comp -> Drive
        -> [ Amp In (+input calibration) -> NAM (mono) -> Gate gain -> Amp Out
             (+output-mode compensation) ]
        -> Cab IR -> Tone Stack -> Modulation -> Delay -> Reverb

    Blocks can be removed from the chain entirely or reordered by the user (see
    dsp/ChainOrder.h); "*_on" stays a separate, automatable per-block bypass. The order
    is state, not a parameter, and reaches the audio thread as one packed uint64.

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

    /** Current signal-chain order. Message thread only. */
    chain::Order getChainOrder() const { return uiChainOrder; }

    /** Publishes a new order lock-free to the audio thread and notifies listeners.
        Message thread only; duplicates/out-of-range ids are dropped. */
    void setChainOrder (const chain::Order& order);

    /** Fired on the message thread whenever the order changes — including after a
        state restore (setStateInformation / applyStateVar / preset load). Set and
        cleared by the editor, like library.onChanged. */
    std::function<void()> onChainChanged;

    juce::AudioProcessorValueTreeState apvts;
    NamEngine namEngine;
    ModelLibrary library;
    PresetManager presets;
    Tone3000Client tone3000;

    /** UI level metering (linear peak, decayed by UI). */
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };

private:
    void updateLatency();

    /** Any thread: makes the order visible to the audio thread (single atomic word). */
    void publishChainOrder (const chain::Order& order) noexcept;
    /** Message thread: publishes, updates the UI-facing copy, fires onChainChanged. */
    void adoptChainOrder (const chain::Order& order);

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

    // Raw APVTS value pointers, resolved once in the constructor: the audio thread
    // must never do a string lookup. Private detail, not part of the contract.
    struct ParamPtrs
    {
        std::atomic<float>* gateOn = nullptr;
        std::atomic<float>* compOn = nullptr;
        std::atomic<float>* driveOn = nullptr;
        std::atomic<float>* ampOn = nullptr;
        std::atomic<float>* cabOn = nullptr;
        std::atomic<float>* eqOn = nullptr;
        std::atomic<float>* modOn = nullptr;
        std::atomic<float>* delayOn = nullptr;
        std::atomic<float>* reverbOn = nullptr;

        std::atomic<float>* inputTrim = nullptr;
        std::atomic<float>* outputLevel = nullptr;

        std::atomic<float>* gateThreshold = nullptr;

        std::atomic<float>* compThreshold = nullptr;
        std::atomic<float>* compRatio = nullptr;
        std::atomic<float>* compAttack = nullptr;
        std::atomic<float>* compRelease = nullptr;
        std::atomic<float>* compMakeup = nullptr;

        std::atomic<float>* driveGain = nullptr;
        std::atomic<float>* driveTone = nullptr;
        std::atomic<float>* driveLevel = nullptr;

        std::atomic<float>* ampInput = nullptr;
        std::atomic<float>* ampOutput = nullptr;
        std::atomic<float>* ampOutMode = nullptr;
        std::atomic<float>* ampCalInput = nullptr;
        std::atomic<float>* ampCalLevel = nullptr;

        std::atomic<float>* cabLowCut = nullptr;
        std::atomic<float>* cabHighCut = nullptr;

        std::atomic<float>* eqBass = nullptr;
        std::atomic<float>* eqMid = nullptr;
        std::atomic<float>* eqTreble = nullptr;

        std::atomic<float>* modType = nullptr;
        std::atomic<float>* modRate = nullptr;
        std::atomic<float>* modDepth = nullptr;
        std::atomic<float>* modMix = nullptr;

        std::atomic<float>* delayTime = nullptr;
        std::atomic<float>* delayFeedback = nullptr;
        std::atomic<float>* delayMix = nullptr;

        std::atomic<float>* reverbSize = nullptr;
        std::atomic<float>* reverbDamping = nullptr;
        std::atomic<float>* reverbMix = nullptr;
    };

    ParamPtrs pp;

    // Guards deferred (callAsync) work against destruction of this processor.
    std::shared_ptr<bool> aliveFlag { std::make_shared<bool> (true) };

    // The order the audio thread runs: one packed word, swapped atomically, decoded
    // once per processBlock. uiChainOrder is the message-thread mirror behind
    // getChainOrder() and is never touched from the audio thread.
    std::atomic<uint64_t> packedChain { chain::pack (chain::defaultOrder()) };
    chain::Order uiChainOrder { chain::defaultOrder() };

    // chain blocks
    NoiseGate gate;
    juce::dsp::Compressor<float> compressor;
    juce::SmoothedValue<float> compMakeupLin { 1.0f };
    Drive drive;
    CabSim cab;
    ToneStackEQ eq;
    DcBlocker dcBlocker;
    Modulation modulation;
    DelayFx delay;
    ReverbFx reverbFx;
    juce::SmoothedValue<float> inputTrimLin { 1.0f }, outputLevelLin { 1.0f }, ampInLin { 1.0f }, ampOutLin { 1.0f };

    juce::AudioBuffer<float> monoScratch;

    // Written on the message thread; also read by getStateInformation, which hosts
    // may call from a save/worker thread. juce::String is copy-on-write and not safe
    // to read while another thread reassigns it, hence the lock (never touched by
    // the audio thread).
    mutable juce::CriticalSection pathLock;
    juce::String loadedModelPath, loadedIrPath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TubampAudioProcessor)
};
} // namespace tubamp
