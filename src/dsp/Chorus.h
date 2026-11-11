#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

// The chorus voicings of the mod block (docs/FX-ENGINES.md §4.5). One engine, five modes:
//
//   Classic    juce::dsp::Chorus, one sine voice shared by both channels, as before (mode 0, frozen).
//   Wide       four voices in two quadrature pairs, triangle LFO, depth in cents.
//   CE         one bucket-brigade-style voice per channel, fixed 2.7 ms sweep, bent triangle.
//   Dimension  two antiphase lines fed from the mono sum, slow triangle.
//   Tri        three lines at 120 degrees, slow sine + fixed 6 Hz shimmer.
namespace tubamp
{
/** Everything a chorus needs for one block; the Modulation dispatcher fills it. */
struct ChorusParams
{
    int mode = 1;              // ChorusFx::Mode
    float rateHz = 1.0f;       // resolved by the dispatcher: mod_rate, or the synced rate [0.01, 20]
    float depth = 0.4f;        // [0, 1]
    float mix = 0.35f;         // [0, 1]
    float width = 1.0f;        // [0, 1]
    float tone = 0.5f;         // [0, 1]
    double syncPhase = -1.0;   // >= 0: LFO phase in cycles at this block's first sample (song position lock)
};

class ChorusFx
{
public:
    /** Order matches the mod_chorus_mode choice list and is FROZEN: reordering it silently
        repoints every saved session. Append only. */
    enum class Mode { classic = 0, wide = 1, ce = 2, dimension = 3, tri = 4 };
    static constexpr int numModes = 5;
    static constexpr int numTypes = numModes;

    ChorusFx();
    ~ChorusFx();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    /** Audio thread, once per block before process(). */
    void setParameters (const ChorusParams& params);
    void process (juce::dsp::AudioBlock<float> block);

    int getLatencySamples() const noexcept { return 0; }
    /** Test hook (fxbench solo=<voice>): routes one voice to both outputs at mix 1;
        -1 = off. */
    void setDebugSolo (int voice) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChorusFx)
};

// Where three knobs start for each mode, indexed by ChorusFx::Mode. A parameter has one
// default; the editor applies these when the user picks a mode (docs/FX-ENGINES.md
// §2.7.4). Classic's entries equal the frozen defaults; width is not moved.

/** mod_rate in Hz. */
inline constexpr float kChorusRateDefault[ChorusFx::numModes]  { 1.0f, 1.0f, 0.8f, 0.25f, 0.6f };
/** mod_depth. */
inline constexpr float kChorusDepthDefault[ChorusFx::numModes] { 0.4f, 0.4f, 0.7f, 0.8f, 0.5f };
/** mod_mix. */
inline constexpr float kChorusMixDefault[ChorusFx::numModes]   { 0.35f, 0.35f, 0.5f, 0.5f, 0.5f };
} // namespace tubamp
