#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

// The phaser voicings of the mod block (docs/FX-ENGINES.md §4.6). One engine, three models:
//
//   Classic      juce::dsp::Phaser (6 stages, sine), as before (model 0, frozen; color ignored).
//   Phase 90     four stages; color off = script logo (no feedback), on = block logo.
//   Small Stone  color off / on = the pedal's Color switch.
//
// Phase 90 and Small Stone are docs/research/fx/phaser/voicings.md, matched to its numeric
// reference prototype/phaser_ref.py. They read rate, color and syncPhase only: depth and mix
// are pinned to the stock pedal.
namespace tubamp
{
/** Everything a phaser needs for one block; the Modulation dispatcher fills it. */
struct PhaserParams
{
    int model = 1;             // PhaserFx::Model; out-of-range values clamp
    float rateHz = 1.0f;       // resolved by the dispatcher [0.01, 20] (Classic: [0.01, 100] as before)
    float depth = 0.4f;        // Classic only
    float mix = 0.35f;         // Classic only
    bool color = true;         // Phase 90: block; Small Stone: Color
    double syncPhase = -1.0;   // >= 0: LFO phase in cycles at this block's first sample (song-position lock)
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
    /** Clears every state (filters, feedback, LFO to phase 0 = the sweep bottom, gate smoothing,
        sync glide and lock, any model switch in flight, parameter smoothers): the next block is
        a fresh engine's. The processor calls it when a Phase 90 / Small Stone block is switched
        back on. */
    void reset();
    /** Audio thread, once per block before process(). */
    void setParameters (const PhaserParams& params);
    void process (juce::dsp::AudioBlock<float> block);

    int getLatencySamples() const noexcept { return 0; }

    /** Test hooks for tubamp_fxbench, not for the plugin. freezeSweep holds the Phase 90 /
        Small Stone sweep at `position` in [-1, 1] (-1 = bottom; Phase 90: after the gate
        smoothing); NaN releases it. getLfoPhase is the LFO phase, in cycles, of the last
        sample processed. getSyncJumpCount counts the song-position jumps glided since
        prepare(). */
    void freezeSweep (double position) noexcept;
    double getLfoPhase() const noexcept;
    int getSyncJumpCount() const noexcept;

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
