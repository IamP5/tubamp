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
//
// Song-position lock (syncPhase >= 0, voices 1-4). Each block the host's phase is compared
// with its own previous phase advanced by the previous block's rate. A difference above
// max (0.01 cycle, 2 ms of LFO time) is a jump (transport start, locate, a loop that is not a
// whole number of cycles, a note-value change, sync switched on): the LFO moves to the new
// phase once, its shape crossfaded over 10 ms of samples whatever the host block size, and a
// second jump inside that fade starts from the blend heard at that moment. Anything smaller is
// host jitter or the residue of a tempo change: the LFO runs free at the tempo's rate and its
// own error against the host is pulled in by a rate nudge of error / 150 ms, at most 3 %.
// Released, the LFO keeps its phase and its rate glides from the tempo's to the knob's.
//
// A voice switch runs both voices on the one LFO, so the new voice continues the same cycle,
// and crossfades their outputs over 40 ms; the incoming voice joins its own steady trajectory
// (gains snapped to their targets, Optical's photocell set to its periodic state). Classic
// takes part with its own LFO, the phase carried across (its dip is at 0.75, the others' at 0).
//
// Bias reacts to the signal by design: a note that lands on the dip is cut by up to 4.7 dB for
// about 6.5 ms (depth 0.75, 196 Hz at -6 dBFS) while the 5 ms follower catches up, as the study's
// envelope does (R5). A DC offset is a fault: no AC-coupled amp lets it reach the bias. The
// detector is AC-coupled at 10 Hz, so DC 0.01 under a -40 dBFS tone now leaves the depth at
// 14.4 dB instead of lowering it to 11.7 dB.
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
    /** Clears every state. Voices 1-4 then start with their knobs at the last values sent
        and their gain at unity, gliding into the tremolo, so the processor can call this
        when the block is switched back on. Without sync the LFO starts where the voice's
        gain is at its crest (Smooth at phase s, Bias at 0.5, Optical and Harmonic at 0), so
        the first note is heard at full level; with sync the host's phase rules. */
    void reset();
    /** Audio thread, once per block before process(). */
    void setParameters (const TremoloParams& params);
    /** A sample beyond 1e6 in magnitude, like a non-finite one, is silenced on the way in;
        everything below passes untouched (Classic bit for bit). */
    void process (juce::dsp::AudioBlock<float> block);

    int getLatencySamples() const noexcept { return 0; }

    /** Test hook for tubamp_fxbench --trace, never used by the plugin. While set, each
        process() of voices 1-4 writes the gain it applies to each channel (Harmonic: the
        low band's) from index 0; a mono block writes the left gain to both. The buffers
        must hold maximumBlockSize samples. Classic leaves them untouched. */
    void setTraceBuffers (float* left, float* right) noexcept;

    /** Test hooks for tubamp_fxbench, never used by the plugin: the number of song-position
        jumps crossfaded since prepare() or reset(), and the LFO phase of voices 1-4 at the
        start of the next block, before that block's sync lock. */
    int getSyncJumpCount() const noexcept;
    double getLfoPhase() const noexcept;

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
