#pragma once

#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <memory>

namespace tubamp
{
/**
    Impulse-response block: a cabinet (or a room) by convolution, with low/high cut on
    the convolved signal and a dry/wet mix.

    Loading and level follow the reference TONE3000 plugin, so the same .wav sits at
    the same level here as there (its ProcessorModelLoader.cpp / Processor.cpp):

      - At most 10 s of the file is used; leading and trailing samples below -80 dBFS
        are trimmed (juce::dsp::Convolution's own trim).
      - The kernel is NOT normalised by JUCE. Its gain is 1 / ||h|| — unit energy,
        measured over every channel of the file at a 48 kHz reference — and only ever
        attenuates (limited to [-48 dB, 0 dB]): a quiet IR is not boosted.
      - A cabinet-like IR (trimmed length up to 1 s, or tagged "cab" by TONE3000) gets
        a further fixed -18 dB on the convolved signal; a reverb-like one (longer, or
        tagged "space") does not.
      - A stereo file is applied as true stereo (L through its left kernel, R through
        its right) only where the path stays stereo. If an enabled amp follows, or the
        buffer is mono, the left kernel alone is used on every channel.
      - Zero latency. IRs up to a second run on a uniform partitioned convolver, longer
        ones on a two-stage one with an 8192-sample head. The convolver is prepared for
        at most 256 samples and fed in slices of that, so its cost does not grow with
        the host's block size.

    Swapping is click-free and never lets the un-cabbed signal through: the old IR's
    convolved signal closes over 25 ms, the convolvers swap, the new one opens over
    25 ms — the dry share of the mix holds throughout (EngineHandoff.h). A first IR
    and a cleared one take the same route, with the unprocessed signal as the thing
    that closes or opens.

    Threading contract:
      - loadImpulseResponse(), clearImpulseResponse() and collectGarbage() are
        message-thread only. Loading reads and decodes the file, builds the convolvers
        and runs them to a settled state before staging them.
      - applyStaging(), isRunning(), setParameters(), process() and reset() are
        audio-thread only (reset() also from prepareToPlay).
      - prepare() is called from prepareToPlay (audio stopped).
      - Everything else is safe from any thread.
*/
class CabSim
{
public:
    CabSim();
    ~CabSim();

    /** How the block applied its IR the last time it ran. */
    enum class Image
    {
        none,       // no IR
        stereo,     // true stereo: left and right kernels
        monoFile,   // the file has one channel
        monoPath    // stereo file, left kernel only: the path is mono from here on
    };

    struct Info
    {
        juce::String filePath;
        juce::String name;          // file basename
        int channels = 0;           // kernels available: 1 or 2
        double sampleRate = 0.0;    // of the file
        double seconds = 0.0;       // kernel length after trimming
        bool reverbLike = false;    // no cab pad (see the class comment)
        juce::String gear;          // TONE3000 gear tag from the file's sidecar, if any
        float gainDb = 0.0f;        // normalization + pad on the convolved signal
        bool truncated = false;     // the file ran past the 10 s limit
    };

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();

    /** Returns an error string, empty on success. The IR already playing keeps
        playing until the new one is ready, and after a failed load. */
    juce::String loadImpulseResponse (const juce::File& wavFile);
    void clearImpulseResponse();

    /** Destroys the convolvers the audio thread has retired. */
    void collectGarbage();

    /** True once an IR is loaded or staged, false after clearImpulseResponse(). The
        UI's question; the audio thread asks isRunning(). */
    bool hasImpulseResponse() const noexcept;

    /** Describes the IR that is loaded or staged. */
    Info getInfo() const;

    Image getImage() const noexcept;

    /** Length of the loaded kernel, for the host's tail report. */
    double getTailSeconds() const noexcept;

    /** Once per block, before process(). `audible`: the cab block is in the signal
        path, so a swap has to be faded rather than just done. */
    void applyStaging (bool audible);

    /** An IR is in the path, or a swap fade still has to finish. */
    bool isRunning() const noexcept;

    void setParameters (float lowCutHz, float highCutHz, float mix01);

    /** In place. `allowStereo` is false when something downstream folds the path to
        mono (an enabled amp), which makes true stereo pointless there. */
    void process (juce::dsp::AudioBlock<float> block, bool allowStereo);

private:
    struct Engine;
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CabSim)
};
} // namespace tubamp
