#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>
#include <optional>

#include "Parameters.h"

namespace nam { class DSP; }

namespace tubamp
{
/**
    Owns the NAM model lifecycle: background loading, resampling to the host rate,
    real-time-safe staged swap into the audio thread, output-mode gain compensation
    and the A2 slimmable-size control.

    Swapping is click-free, the way the reference TONE3000 plugin does it: the model
    that is playing keeps playing while the new one loads, then the block's output
    closes over 25 ms, the swap happens on the silent sample and the new model opens
    over another 25 ms (EngineHandoff.h). The same goes for a first model (the
    unprocessed signal closes, the model opens) and for removing one. A swap nobody
    can hear — block bypassed, stream starting — skips the ramps.

    Threading contract:
      - loadModel(), clearModel(), collectGarbage() and setSlimSize() are message-thread
        only, and must not run concurrently with each other.
      - loadModel() performs file IO, JSON parsing and prewarm synchronously, then stages
        the result. Never call it from processBlock.
      - process()/applyStaging()/isRunning() are audio-thread only. applyStaging()
        must be the first call each block: it starts the swap fade (or swaps at once
        when nothing is audible). process() finishes the swap on the sample the fade
        reaches silence and parks the old model in a garbage slot (released later on
        the message thread via collectGarbage()).
      - prepare() is called from prepareToPlay (audio thread stopped).

    The engine processes MONO in place: callers collapse the input to channel 0
    before process() and duplicate afterwards (all shipped .nam models are mono).
    Stereo is two engines, one per channel — nam_core shares nothing between
    instances, so each owns its own weights (docs/STEREO.md §1).
*/
class NamEngine
{
public:
    NamEngine();
    ~NamEngine();

    struct ModelInfo
    {
        juce::String filePath;      // absolute path of the loaded .nam
        juce::String name;          // display name (file basename)
        double sampleRate = 48000.0;
        std::optional<double> loudnessDb;   // metadata.loudness if present
        std::optional<double> inputLevelDbu;
        std::optional<double> outputLevelDbu;

        // metadata.gear_type, lower_snake and with TONE3000's "T3K-Null" placeholder
        // folded to empty. Empty means "the capture doesn't say" — most captures
        // trained outside TONE3000's exporter carry no gear fields at all.
        juce::String gearType;

        // True when gearType describes a rig captured through a cabinet (amp_cab,
        // amp_mic, the pedal_* variants, a bare cab). Loading an IR on top of one
        // of those stacks a second cabinet, which the UI warns about.
        bool includesCab = false;
    };

    /** Synchronously loads and stages a model. Returns an error string on failure,
        empty on success. Call from a background/message thread only. */
    juce::String loadModel (const juce::File& namFile);

    /** Request removal of the current model (pass-through afterwards). */
    void clearModel();

    /** Audio-thread, once per block before anything else: act on a staged model or
        a pending clear. `audible` says whether the block this engine belongs to is
        in the signal path; a swap there is faded (and happens later, inside
        process()), anywhere else it is done right here. Returns true when the live
        model changed in this call. */
    bool applyStaging (bool audible);

    /** Audio-thread: run the model on buffer channel 0 (mono), numSamples frames.
        No-op pass-through when no model is loaded. Non-finite samples are replaced
        by silence on the way in and on the way out.

        Returns the offset in this block at which the live model changed behind a
        fade, or -1. The block is silent at that sample, which makes it the one
        place where the caller can switch a gain that belongs to the model (the
        output-mode compensation) without ramping it across two models. */
    int process (float* monoChannel, int numSamples);

    /** Audio-thread only: the engine has something to do in the signal path this
        block — a live model, or the tail of a swap fade. A caller that skips
        process() for a model-less engine (the amp2 block) gates on this, not on
        hasLiveModel(), so a cleared model still gets to fade out. */
    bool isRunning() const noexcept;

    /** Audio-thread only: a model is in the path right now (it may be fading). */
    bool hasModelInPath() const noexcept;

    /** For an owner that leaves the signal completely alone while the engine has no
        model — the amp2 block: untouched stereo, no gains, no latency — instead of
        running it through process() like the amp block does. Such an owner calls
        fadeUnprocessed() for the model-less side of a swap, and process() then keeps
        the rest of a block silent once it has dropped the model, so the two paths
        (mono, gained / stereo, untouched) only ever meet at silence. Set once,
        before audio starts. */
    void setBypassedWhenEmpty (bool bypassed) noexcept;

    /** Audio-thread: the swap fade over the unprocessed channels of a
        bypassed-when-empty owner, for a block in which isRunning() is true and
        hasModelInPath() is false. Closes the signal before a first model, opens it
        again after a removed one. Returns true when the model was adopted in this
        call: the rest of the block is silent and the model opens through process()
        from the next block on. */
    bool fadeUnprocessed (float* const* channels, int numChannels, int numSamples);

    /** Blocks whose model output had to be silenced because it was not finite.
        Diagnostic; any thread. */
    int getFaultCount() const noexcept;

    /** prepareToPlay: reconfigures the resampler for the host rate/block size.
        Safe to call with a live model (audio is stopped during prepareToPlay). */
    void prepare (double sampleRate, int maxBlockSize);

    /** Message-thread: destroy any model parked by applyStaging(). */
    void collectGarbage();

    /** Any thread: true once loadModel() has staged a model, false after clearModel().
        This is the *UI's* question — "is a model loaded?" — and it answers yes before
        the audio thread has adopted the staged model. Do not gate DSP on it. */
    bool hasModel() const noexcept;

    /** Audio-thread query: true while a model is live on the audio thread. Published
        only by applyStaging() (and dropped by clearModel(), the fail-safe direction),
        so it never claims a model the audio thread has not adopted — which is what the
        dual-NAM stereo gate needs (docs/STEREO.md §1). */
    bool hasLiveModel() const noexcept;
    /** Resampler latency in samples at the host rate (0 when rates match), measured
        through the resampler pair rather than taken from its priming length. */
    int getLatencySamples() const noexcept;
    /** Info for the currently *live or staged* model (UI display). */
    ModelInfo getModelInfo() const;

    /** Audio-thread-safe: dB to add to the Amp Output knob for the given mode, a port
        of the official _SetOutputGain() (NeuralAmpModeler.cpp:701-729).

          Raw        0
          Normalized -18 - modelLoudness      (only when the model reports loudness)
          Calibrated modelOutputLevel - calibrationLevelDbu
                                              (only when the model reports an output level)

        Returns 0 when there is no live model or the mode's metadata is missing or
        implausible (non-finite, loudness outside -100..0 dB, a level beyond +-60 dBu),
        which makes the knob behave exactly as in Raw. The normalization gain is
        limited to +-24 dB, as in the TONE3000 plugin. */
    float getOutputCompensationDb (params::OutputMode mode, float calibrationLevelDbu) const noexcept;

    /** Audio-thread-safe: dB to add to the Amp Input knob, a port of the official
        _SetInputGain() (NeuralAmpModeler.cpp:690-699). Returns
        (calibrationLevelDbu - modelInputLevel) when `calibrateInput` is set and the
        model reports an input level, 0 otherwise. */
    float getInputCalibrationDb (bool calibrateInput, float calibrationLevelDbu) const noexcept;

    /** Message thread: A2 "slimmable" size, 0 = cheapest, 1 = full quality. Stored and
        applied to the live model now and to every model loaded afterwards.
        SetSlimmableSize() is thread-safe but not real-time safe, so this must never be
        reached from processBlock. */
    void setSlimSize (double size01);

    /** True when the loaded model implements nam::SlimmableModel (UI gating). */
    bool isSlimmable() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NamEngine)
};
} // namespace tubamp
