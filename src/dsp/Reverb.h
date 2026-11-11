#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

// The reverb block (docs/REVERB-DELAY.md §2). One shell, two engines, five types:
//
//   Room      )  four voicings of one lattice: twenty delay stages with a rotation
//   Hall      )  after each, inside a single feedback loop seconds long. Dense within
//   Plate     )  milliseconds, smooth with the modulation off, exact decay per band.
//   Ambient   )  They differ in loop length and in which stages are heard.
//   Spring    dispersive allpass chains in feedback loops: the drip of an amp's tank.
//
// The shell around both: pre-delay -> wet-only low/high cut -> engine -> M/S width ->
// constant-dry mix.
namespace tubamp
{
/** Everything a reverb instance needs for one block, in the units Parameters.h documents. */
struct ReverbParams
{
    int type = 0;                 // ReverbFx::Type; out-of-range values clamp
    float size = 0.5f;            // [0, 1]
    float damping = 0.5f;         // [0, 1] treble decay: 0 = as long as the mids, 1 = gone fast
                                  // (kReverbDampingDefault holds each type's starting point)
    float mix = 0.25f;            // [0, 1]
    float width = 1.0f;           // [0, 1] M/S width of the wet signal
    float preDelayMs = 10.0f;     // [0, 250]
    float decaySeconds = 1.6f;    // [0.2, 20] mid-band T60 (kReverbDecayDefault)
    float lowCutHz = 120.0f;      // [20, 1000] wet only
    float highCutHz = 8000.0f;    // [1000, 20000] wet only
    float mod = 0.35f;            // [0, 1] tail modulation
};

class ReverbFx
{
public:
    /** Order matches the reverb_type choice list and is FROZEN: reordering it silently
        repoints every saved session. Append only. */
    enum class Type { room = 0, hall = 1, plate = 2, spring = 3, ambient = 4 };
    static constexpr int numTypes = 5;

    ReverbFx();
    ~ReverbFx();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    /** Audio thread, once per block before process(). */
    void setParameters (const ReverbParams& params);
    void process (juce::dsp::AudioBlock<float> block);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbFx)
};

// Where two knobs start for each type, indexed by ReverbFx::Type. A parameter has one
// default, which is the default type's entry; the editor applies the others when the
// user picks a type (docs/REVERB-DELAY.md §2.4). Only knobs whose sensible range
// depends on the type are here: mix, width and pre-delay are the player's, and size and
// mod are already scaled per type inside the engine.

/** A room that rings for 1.6 s is a hall, and an ambient wash that lasts 1.6 s is not
    one: decay starts where each kind of space lives. */
inline constexpr float kReverbDecayDefault[ReverbFx::numTypes] {
    0.7f, // Room
    2.2f, // Hall
    1.6f, // Plate
    1.8f, // Spring: about what a full-size tank rings for
    4.5f  // Ambient
};

/** The damping knob means the same thing in every type, so the values differ: each is
    where the treble of that kind of space decays in the reference reverbs measured for
    this work. */
inline constexpr float kReverbDampingDefault[ReverbFx::numTypes] {
    0.65f, // Room: 8 kHz decays in ~0.45x the mid-band time
    0.65f, // Hall: ~0.55x
    0.50f, // Plate: ~0.65x, the brightest of the acoustic types
    0.50f, // Spring: the tank's own band limit does most of the work
    0.55f  // Ambient: ~0.8x, an airy tail
};
} // namespace tubamp
