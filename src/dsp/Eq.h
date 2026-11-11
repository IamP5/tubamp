#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

// The EQ block and the amp block's tone stack (docs/FX-ENGINES.md §4.4). One engine,
// four types:
//
//   Classic     three float cookbook biquads as before (type 0), plus knob smoothing and
//               an exact pass-through at 5/5/5. The amp tone stack always runs Classic.
//   Parametric  double-precision TPT state-variable core: low cut, shelves, swept mid, tilt,
//               high cut, level.
//   Stack       interactive passive amp tone stack (Yeh-Smith), four voicings.
//   Graphic     seven octave bells, 100 Hz to 6.4 kHz.
namespace tubamp
{
/** Everything an EQ instance needs for one block, in the units Parameters.h documents. */
struct EqParams
{
    int type = 1;                 // EqFx::Type
    float bass = 5.0f, mid = 5.0f, treble = 5.0f;   // [0, 10], 5 = flat (Classic/Parametric), noon (Stack)
    float lowCutHz = 20.0f;       // [20, 400]; <= 20.5 = off
    float highCutHz = 20000.0f;   // [2000, 20000]; >= 19999 = off
    float midFreqHz = 425.0f;     // [100, 8000]
    float midQ = 0.7f;            // [0.5, 8]
    float tiltDb = 0.0f;          // [-6, 6]
    float levelDb = 0.0f;         // [-12, 12]
    int voice = 2;                // EqFx::Voice
    float graphicDb[7] {};        // [-12, 12] at 100, 200, 400, 800, 1600, 3200, 6400 Hz
};

class EqFx
{
public:
    /** Order matches the eq_type choice list and is FROZEN: reordering it silently
        repoints every saved session. Append only. */
    enum class Type { classic = 0, parametric = 1, stack = 2, graphic = 3 };
    static constexpr int numTypes = 4;

    /** Stack voicings; order matches the eq_voice choice list and is FROZEN. */
    enum class Voice { tweed = 0, blackface = 1, british = 2, topBoost = 3 };
    static constexpr int numVoices = 4;

    EqFx();
    ~EqFx();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    /** Audio thread, once per block before process(). */
    void setParameters (const EqParams& params);
    void process (juce::dsp::AudioBlock<float> block);

    int getLatencySamples() const noexcept { return 0; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqFx)
};
} // namespace tubamp
