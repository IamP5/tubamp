#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

// The phaser voicings of the mod block (docs/FX-ENGINES.md §4.6). One engine, three models:
//
//   Classic      juce::dsp::Phaser (6 stages, sine), as before (model 0, frozen; color ignored).
//   Phase 90     four stages; color off = script logo (no feedback), on = block logo.
//   Small Stone  color off / on = the pedal's Color switch.
namespace tubamp
{
/** Everything a phaser needs for one block; the Modulation dispatcher fills it. */
struct PhaserParams
{
    int model = 1;             // PhaserFx::Model
    float rateHz = 1.0f;       // resolved by the dispatcher [0.01, 20]
    float depth = 0.4f;        // Classic only
    float mix = 0.35f;         // Classic only
    bool color = true;         // Phase 90: block; Small Stone: Color
    double syncPhase = -1.0;   // >= 0: raw LFO phase at block start (song-position lock)
};

class PhaserFx
{
public:
    /** Order matches the mod_phaser_model choice list and is FROZEN: reordering it silently
        repoints every saved session. Append only. */
    enum class Model { classic = 0, phase90 = 1, smallStone = 2 };
    static constexpr int numModels = 3;
    static constexpr int numTypes = numModels;

    PhaserFx();
    ~PhaserFx();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    /** Audio thread, once per block before process(). */
    void setParameters (const PhaserParams& params);
    void process (juce::dsp::AudioBlock<float> block);

    int getLatencySamples() const noexcept { return 0; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PhaserFx)
};

// Where mod_rate starts for each model, indexed by PhaserFx::Model (docs/FX-ENGINES.md
// §2.7.4). Phase 90 and Small Stone start at Guitar Rig's noon (1.071 / 0.316 Hz, rounded
// as typeDefaults.ts has them). Classic's entry equals the frozen default.
inline constexpr float kPhaserRateDefault[PhaserFx::numModels] { 1.0f, 1.07f, 0.32f };
} // namespace tubamp
