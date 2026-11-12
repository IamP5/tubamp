#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

// The chorus voicings of the mod block (docs/FX-ENGINES.md §4.5). One engine, five modes:
//
//   Classic    juce::dsp::Chorus, one sine voice shared by both channels, as before (mode 0, frozen).
//   Wide       four voices in two quadrature pairs, triangle LFO, depth in cents.
//   CE         one bucket-brigade-style voice per channel, fixed 2.7 ms sweep, bent triangle.
//   Dimension  two antiphase lines fed from the mono sum, slow triangle (mono: line A alone).
//   Tri        three lines at 120 degrees, slow sine + fixed 6 Hz shimmer.
//
// The limit of a transition (rate, depth, a song-position jump): a change made by a tap
// crossfade slides each voice's phase by up to half a cycle in 30 ms, a pitch blip of up to
// 25 Hz (about +460 cents at 82 Hz, +43 at 1 kHz) that no fade shape removes, because gliding
// the same delay change as fast bends every note by as much. So a change glides, at most
// 4 cents beyond the settings' own detune, wherever that lags a stopped knob by under 80 ms or
// a moving one by under 100 ms, and is crossfaded otherwise, at most once per 250 ms while a
// knob keeps moving.
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
                               // Given every block while the song plays: each is compared with where the
                               // last one, moved on at rateHz, predicts; a small difference is pulled in
                               // slowly, a large one is a jump.
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
    /** Clears every line and starts every smoother at its target; modes 1-4 then pass the
        dry at once and fade the wet in as the lines refill. Call it when a bypassed block
        comes back, or the lines replay what they held when it was switched off. Classic is
        cleared completely too (line, oscillator, mixer): its output afterwards is exactly the
        frozen chorus started fresh. */
    void reset();
    /** Audio thread, once per block before process(). */
    void setParameters (const ChorusParams& params);
    /** A non-finite input sample, or one beyond +-1e6, is processed as 0 (and written back as 0). */
    void process (juce::dsp::AudioBlock<float> block);

    int getLatencySamples() const noexcept { return 0; }
    /** Test hook (fxbench solo=<voice>): routes one voice to both outputs at mix 1;
        -1 = off. */
    void setDebugSolo (int voice) noexcept;
    /** Test hook (fxbench --trace): while a voice is soloed, process() writes per sample the
        soloed voice's current tap delay, its outgoing tap delay (equal to the first outside
        a tap crossfade) in samples, and the crossfade stage (0 none, 1 outgoing tap alone,
        2 fading directly, 3 fading through the midpoint tap). Three channels of at least the
        block's length, or nullptr. */
    void setDebugTrace (float* const* channels) noexcept;

    struct DebugCounts
    {
        int syncJumps = 0;    // host song-position discontinuities taken as jumps
        int knobFades = 0;    // tap crossfades started for changes too large to glide
    };
    DebugCounts getDebugCounts() const noexcept;

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
