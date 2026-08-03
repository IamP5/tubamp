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

    Threading contract:
      - loadModel(), clearModel(), collectGarbage() and setSlimSize() are message-thread
        only, and must not run concurrently with each other.
      - loadModel() performs file IO, JSON parsing and prewarm synchronously, then stages
        the result. Never call it from processBlock.
      - process()/applyStaging() are audio-thread only. applyStaging() must be the
        first call each block; it adopts a staged model and parks the old one in a
        garbage slot (released later on the message thread via collectGarbage()).
      - prepare() is called from prepareToPlay (audio thread stopped).

    The engine processes MONO in place: callers collapse the input to channel 0
    before process() and duplicate afterwards (all shipped .nam models are mono).
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
    };

    /** Synchronously loads and stages a model. Returns an error string on failure,
        empty on success. Call from a background/message thread only. */
    juce::String loadModel (const juce::File& namFile);

    /** Request removal of the current model (pass-through afterwards). */
    void clearModel();

    /** Audio-thread: adopt a freshly staged model / drop a cleared one.
        Returns true when the live model changed (caller updates latency). */
    bool applyStaging();

    /** Audio-thread: run the model on buffer channel 0 (mono), numSamples frames.
        No-op pass-through when no model is loaded. */
    void process (float* monoChannel, int numSamples);

    /** prepareToPlay: reconfigures the resampler for the host rate/block size.
        Safe to call with a live model (audio is stopped during prepareToPlay). */
    void prepare (double sampleRate, int maxBlockSize);

    /** Message-thread: destroy any model parked by applyStaging(). */
    void collectGarbage();

    bool hasModel() const noexcept;
    /** Resampler latency in samples at the host rate (0 when rates match). */
    int getLatencySamples() const noexcept;
    /** Info for the currently *live or staged* model (UI display). */
    ModelInfo getModelInfo() const;

    /** Audio-thread-safe: dB to add to the Amp Output knob for the given mode, a port
        of the official _SetOutputGain() (NeuralAmpModeler.cpp:701-729).

          Raw        0
          Normalized -18 - modelLoudness      (only when the model reports loudness)
          Calibrated modelOutputLevel - calibrationLevelDbu
                                              (only when the model reports an output level)

        Returns 0 when there is no live model or the mode's metadata is missing, which
        makes the knob behave exactly as in Raw. */
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
