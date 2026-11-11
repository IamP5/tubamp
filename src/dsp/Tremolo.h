#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

// The tremolo voicings of the mod block (docs/FX-ENGINES.md §4.7). One engine, five voices:
//
//   Classic   sine gain LFO with the dry/wet mix, as before (voice 0, frozen).
//   Smooth    shaped LFO (wave, skew), real depth law with make-up, 120 Hz gain smoother.
//   Optical   lamp pulse into a two-rate photocell lag, -20 dB floor at full depth.
//   Bias      depth falls as the playing gets louder (push-pull bias model).
//   Harmonic  low and high bands modulated in antiphase.
namespace tubamp
{
/** Everything a tremolo needs for one block; the Modulation dispatcher fills it. */
struct TremoloParams
{
    int voice = 1;             // TremoloFx::Voice
    float rateHz = 1.0f;       // resolved by the dispatcher [0.01, 20]
    float depth = 0.4f;        // [0, 1]
    float mix = 0.35f;         // Classic only
    float width = 1.0f;        // [0, 1] spread = 180 deg * width
    float tone = 0.5f;         // [0, 1] Harmonic crossover
    float shape = 0.5f;        // [0, 1] Smooth skew/duty, Bias knee
    float wave = 0.0f;         // [0, 1] Smooth waveform
    double syncPhase = -1.0;   // >= 0: LFO phase at block start (song-position lock)
};

class TremoloFx
{
public:
    /** Order matches the mod_trem_voice choice list and is FROZEN: reordering it silently
        repoints every saved session. Append only. */
    enum class Voice { classic = 0, smooth = 1, optical = 2, bias = 3, harmonic = 4 };
    static constexpr int numVoices = 5;
    static constexpr int numTypes = numVoices;

    TremoloFx();
    ~TremoloFx();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    /** Audio thread, once per block before process(). */
    void setParameters (const TremoloParams& params);
    void process (juce::dsp::AudioBlock<float> block);

    int getLatencySamples() const noexcept { return 0; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TremoloFx)
};

// Where three knobs start for each voice, indexed by TremoloFx::Voice (docs/FX-ENGINES.md
// §2.7.4). Classic's entries equal the frozen defaults (its width is unused).

/** mod_rate in Hz. */
inline constexpr float kTremRateDefault[TremoloFx::numVoices]  { 1.0f, 4.0f, 5.0f, 5.0f, 4.0f };
/** mod_depth. */
inline constexpr float kTremDepthDefault[TremoloFx::numVoices] { 0.4f, 0.6f, 0.7f, 0.75f, 0.8f };
/** mod_width. */
inline constexpr float kTremWidthDefault[TremoloFx::numVoices] { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
} // namespace tubamp
