#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

// The delay block (docs/REVERB-DELAY.md §3). One engine, five characters:
//
//   Digital   clean line, filtered and softly limited feedback, crossfaded time changes.
//   Analog    bucket-brigade character: bandwidth falls as the time grows, companded
//             soft saturation, time changes repitch.
//   Tape      wow and flutter, head-bump and gap-loss EQ, tape saturation, slow
//             capstan-style repitch on time changes.
//   Lo-Fi     early digital hardware: reduced sample rate and word length.
//   Diffuse   allpass diffusion inside the loop: every repeat smears further.
//
// All types share the routing modes (Stereo / Ping-Pong / Dual), the in-loop low and
// high cut, modulation, ducking and tempo sync.
namespace tubamp
{
/** Everything a delay instance needs for one block, in the units Parameters.h documents. */
struct DelayParams
{
    float timeMs = 420.0f;        // [20, 2000] left/base time when sync is Off
    float feedback = 0.35f;       // [0, 0.95]
    float mix = 0.25f;            // [0, 1]
    int mode = 0;                 // DelayFx::Mode
    float ratioPct = 100.0f;      // [25, 200] R time as % of L, Dual mode only
    float width = 1.0f;           // [0, 1] M/S width of the wet signal
    int type = 0;                 // DelayFx::Type; out-of-range values clamp
    int sync = 0;                 // index into kDelaySyncBeats; 0 = Off
    double bpm = 120.0;           // host tempo, used only when sync != 0
    float lowCutHz = 80.0f;       // [20, 1000] in the feedback loop
    float highCutHz = 7000.0f;    // [1000, 20000] in the feedback loop (kDelayHighCutDefault)
    float mod = 0.2f;             // [0, 1] time modulation depth
    float color = 0.0f;           // [0, 1] how much of the type's character (kDelayColorDefault)
    float duck = 0.25f;           // [0, 1] wet ducking under the input (kDelayDuckDefault)
};

/** Note lengths for delay_sync, in quarter-note beats; index 0 is "Off". The order
    matches the delay_sync choice list in Parameters.h and is FROZEN. Append only. */
inline constexpr float kDelaySyncBeats[] {
    0.0f,          // Off
    0.125f,        // 1/32
    1.0f / 6.0f,   // 1/16T
    0.25f,         // 1/16
    1.0f / 3.0f,   // 1/8T
    0.375f,        // 1/16.
    0.5f,          // 1/8
    2.0f / 3.0f,   // 1/4T
    0.75f,         // 1/8.
    1.0f,          // 1/4
    4.0f / 3.0f,   // 1/2T
    1.5f,          // 1/4.
    2.0f,          // 1/2
    3.0f,          // 1/2.
    4.0f           // 1/1
};
inline constexpr int kNumDelaySyncChoices = (int) (sizeof (kDelaySyncBeats) / sizeof (kDelaySyncBeats[0]));

class DelayFx
{
public:
    /** Order matches the delay_mode choice list and is FROZEN. */
    enum class Mode { stereo = 0, pingPong = 1, dual = 2 };

    /** Order matches the delay_type choice list and is FROZEN: reordering it silently
        repoints every saved session. Append only. */
    enum class Type { digital = 0, analog = 1, tape = 2, lofi = 3, diffuse = 4 };
    static constexpr int numTypes = 5;

    DelayFx();
    ~DelayFx();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    /** Audio thread, once per block before process(). */
    void setParameters (const DelayParams& params);
    void process (juce::dsp::AudioBlock<float> block);

    /** The base (left) delay time a parameter set resolves to: the knob when sync is
        Off, the note length at `bpm` otherwise, clamped to the knob's own range. */
    static float resolveTimeMs (const DelayParams& params) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayFx)
};

// Where three knobs start for each type, indexed by DelayFx::Type. A parameter has one
// default, which is the default type's entry; the editor applies the others when the
// user picks a type (docs/REVERB-DELAY.md §3.4). Only knobs whose meaning or sensible
// range depends on the type are here: time, feedback, mix and routing are the player's.

/** Colour is a different thing in every type, so it cannot share a starting point. */
inline constexpr float kDelayColorDefault[DelayFx::numTypes] {
    0.00f, // Digital: a clean line until asked otherwise
    0.40f, // Analog: bucket-brigade darkness and compander grit
    0.35f, // Tape: some age and saturation
    0.40f, // Lo-Fi: clearly reduced rate and word length
    0.50f  // Diffuse: repeats already smearing into each other
};

/** Analog and Tape darken every repeat on their own; a low in-loop high cut on top
    would darken them twice, so theirs starts well out of the way. */
inline constexpr float kDelayHighCutDefault[DelayFx::numTypes] {
    7000.0f,  // Digital
    12000.0f, // Analog
    12000.0f, // Tape
    7000.0f,  // Lo-Fi
    7000.0f   // Diffuse
};

/** The clearer and brighter a type's repeats, the more they compete with the playing
    they follow, so the more they are ducked; Diffuse, whose repeats become a continuous
    wash, gets the most. */
inline constexpr float kDelayDuckDefault[DelayFx::numTypes] {
    0.25f, // Digital: full-bandwidth repeats
    0.15f, // Analog: already dark, sits behind the dry signal
    0.15f, // Tape: likewise
    0.20f, // Lo-Fi
    0.40f  // Diffuse
};
} // namespace tubamp
