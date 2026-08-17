#pragma once

#include "PitchShifter.h"
#include "SpringDispersion.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cstdint>
#include <vector>

namespace tubamp
{
/**
    The reverb engine: an 8-line absorbent-allpass FDN in the Jot / Dahl-Jot lineage
    with per-mode constant tables, plus two alternate back ends (the Reverse window and
    the three dispersive springs).
    See docs/REVERB.md — this class is §3 of that document, and every constant below
    is either quoted from §4.1 or derived by the law the section names.

    Structure per §3.2, one sample at a time:

      r_i = cubicRead (line[i], writePos - lineSamples_i - mod_i)   4-pt Hermite
      a_i = allpass_i (r_i)        modulated Schroeder allpass INSIDE the loop
      s_i = jotDamp_i (a_i)        §3.4 zita Filt1, ratio form
      w_i = softClipAdaa (s_i) + 1e-20 §3.5 C1 clip, residual ADAA1 + staynormal
      y   = A * w                  normalized 8x8 Hadamard, 12 butterflies + 1/sqrt(8)
      line[i] <- y_i + inject_i

    Threading: prepare() is the only entry point that allocates and is called from
    prepareToPlay. configure(), reset(), resetStep(), setParameters(), setWetFade()
    and process() are audio-thread safe and allocation free.

    prepare() ALWAYS clears (§6.4) — renderChain's bit-identical contract depends on it,
    and every seeded RNG stream is re-seeded there so two renders match sample for sample.
*/
class ReverbEngine
{
public:
    /** Mode indices mirror `reverbAlgoChoices`, which is FROZEN (§4). Every entry is
        live from Stage 3 on: Spring shipped at Stage 2, Shimmer at Stage 3, and no DSP
        fallback path remains. */
    enum Mode
    {
        modeRoom = 0,
        modePlate = 1,
        modeHall = 2,
        modeSpring = 3,
        modeShimmer = 4,
        modeReverse = 5,
        numModes = 6
    };

    /** §6.1. One struct rather than a 17-argument setter. `freeze` is still deliberately
        absent: it belongs to Stage 4 and its parameter does not exist yet (user directive,
        2026-08-06). §6.1 is amended to match — a field with no parameter behind it is a
        field nothing can set, and this struct is internal, so each stage adds its own
        field with its own gate. Stage 3 adds the two shimmer fields under that rule. */
    struct Params
    {
        int   algo = 0;              // 0 = Room (§4)
        float size01 = 0.5f, damping01 = 0.5f, mix01 = 0.25f, width01 = 1.0f;
        float decayS = 2.0f, predelayMs = 0.0f, diffusion01 = 0.7f;
        float lowCutHz = 20.0f, highCutHz = 20000.0f;
        float mod01 = 0.35f, bassMult = 1.0f, erLevel01 = 0.5f;
        float color01 = 0.0f, tilt = 0.0f, duck01 = 0.0f;
        float shimmer01 = 0.0f;      // §3.11, 0 = fully off and bit-inert
        int   shimmerInterval = 2;   // §3.11 FROZEN 4, default +1 oct
        bool  snap = false;          // §6.4
    };

    /** Allocates every buffer, configures the current mode and clears. */
    void prepare (const juce::dsp::ProcessSpec& spec);

    /** Full synchronous clear. No allocation; safe on the audio thread but it touches
        the whole footprint at once — prefer resetStep() from the pendingReset drain. */
    void reset();

    /** Clears one delay line's worth of buffer per call and returns true on the last
        one (§6.3). The Reverse window line is split into tank-line-sized chunks so no
        single step exceeds the existing DelayFx bound. */
    bool resetStep();

    /** Rebuilds the mode tables (and, in Reverse, the tap table for `windowSeconds`).
        No allocation: a mode switch must never allocate (§6.2). Does not clear —
        the caller pairs this with reset()/resetStep(). */
    void configure (int algo, float windowSeconds);

    /** Audio thread, once per block. Computes coefficient targets only; the ramps are
        laid down in process() where the block length is known (§6.5). `algo` is
        ignored here — the configured mode is owned by configure() so that a mode
        change can be deferred (§6.2). */
    void setParameters (const Params& p);

    /** Wet-bus fade for the deferred mode change (§6.2), ramped across the block. */
    void setWetFade (float startGain, float endGain) noexcept;

    /** In place. 1 or 2 channels; the caller has already clamped the channel count. */
    void process (juce::dsp::AudioBlock<float> block);

    int   getConfiguredMode() const noexcept { return mode; }
    float getConfiguredWindow() const noexcept { return windowSeconds; }

    /** Range clamp only — every mode of §4 is shipped, so nothing falls back any more.
        Kept because it is the chokepoint the whole codebase already asks "which machine
        does this algo index actually render?", and it still owns the clamp. */
    static int resolveMode (int algo) noexcept;
    /** Reverse's window: the decay ceiling mechanism of §3.3 applied to §3.12's 1.5 s. */
    static float windowSecondsFor (float decaySeconds) noexcept;
    /** Per-mode decay ceiling of §4.1, enforced as a parameter clamp (§3.3). */
    static float decayCeilingFor (int resolvedMode) noexcept;

private:
    static constexpr int   kNumLines = 8;
    static constexpr int   kNumDiffusers = 4;
    static constexpr int   kNumErTaps = 24;
    static constexpr int   kNumRevTaps = 64;
    static constexpr int   kNumWalkers = 8;
    static constexpr int   kNumTankTaps = 7;
    static constexpr int   kNumSpreadSections = 3;
    static constexpr int   kNumSprings = 3;     // §3.9, 3 springs into 3 active lines
    static constexpr int   kGuard = 4;          // §3.3 mirrored guard region

    static constexpr float kMaxDelaySeconds = 0.36f;      // §3.3
    static constexpr float kMaxAllpassSeconds = 0.12f;    // §6.7
    static constexpr float kMaxPredelaySeconds = 0.25f;
    static constexpr float kMaxErSeconds = 0.09f;
    static constexpr float kReverseWindowSeconds = 1.5f;  // §3.12
    static constexpr float kMaxSizeScale = 1.5f;          // widest sizeMax across modes

    /** Linear ramp of a per-block coefficient across the block (§6.5). */
    struct Ramp
    {
        float cur = 0.0f, inc = 0.0f;
        inline float next() noexcept { cur += inc; return cur; }
        inline void setTarget (float target, float invN) noexcept { inc = (target - cur) * invN; }
        inline void snap (float target) noexcept { cur = target; inc = 0.0f; }
    };

    /** 2-multiply Schroeder allpass with a fractional, optionally ramped delay. */
    struct Allpass
    {
        float* buf = nullptr;
        int len = 0, write = 0;
        Ramp delay;
    };

    /** Second-order allpass section for the frequency-dependent L/R spread (§3.8). */
    struct Ap2
    {
        float a1 = 0.0f, a2 = 0.0f;
        float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
        inline float process (float x) noexcept
        {
            const float y = a2 * x + a1 * x1 + x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            return y;
        }
        void clear() noexcept { x1 = x2 = y1 = y2 = 0.0f; }
    };

    struct LineState
    {
        // Geometry, in fractional samples, ramped across the block.
        Ramp lineDelay, apDelay;
        float apG = 0.0f;
        float apRatio = 0.22f;

        // zita Filt1 (§3.4): ratio form, all four coefficients ramped.
        Ramp gMid, gLo, wLo, wHi;
        float sLo = 0.0f, sHi = 0.0f;

        // Colour axis 1: in-loop bandwidth ceiling (§5.1).
        float colorLp = 0.0f;

        // §3.5 residual-ADAA memory: previous pre-clip sample (AMENDED 2026-08-07).
        float clipPrev = 0.0f;

        // Layer A random walk (§3.6).
        std::uint32_t rng = 0;
        float walkPos = 0.0f, walkInc = 0.0f, walkTarget = 0.0f;
        int   walkCount = 0;
        float rateJitter = 1.0f, depthJitter = 1.0f;   // (0.6+0.8r) and (0.85+0.3r')
        float walkDepth = 0.0f;                        // samples, both depth laws applied
        int   walkSegment = 1;                         // samples per segment

        // Layer B quadrature (§3.6): one shared oscillator, per-line phase offset.
        float phaseCos = 1.0f, phaseSin = 0.0f;        // cos/sin of 2*pi*k/8
        float depthB = 0.0f;
        bool  layerB = false;
    };

    struct ErTap
    {
        int   delaySamples = 0;
        float gainL = 0.0f, gainR = 0.0f, gainMono = 0.0f;
    };

    struct RevTap
    {
        float delaySamples = 0.0f;
        float gainL = 0.0f, gainR = 0.0f, gainMono = 0.0f;
        int   walker = 0;
    };

    struct Walker
    {
        std::uint32_t rng = 0;
        float pos = 0.0f, inc = 0.0f;
        int   count = 0;
        float rateJitter = 1.0f, depthJitter = 1.0f;
        float depth = 0.0f;
        int   segment = 1;
    };

    void layOutBuffers();
    void buildModeTables();
    void rebuildErTaps();
    void rebuildReverseTaps();
    void clearAll();
    void resetState();
    void updateBlockRamps (int numSamples);
    void processTank (float* const* data, int numChannels, int numSamples);
    void processReverse (float* const* data, int numChannels, int numSamples);
    void processSpring (float* const* data, int numChannels, int numSamples);
    float dwell (float x, float& preState, float& deState, float drive) noexcept;
    void advanceWalk (LineState& line) noexcept;
    void advanceWalker (Walker& w) noexcept;
    float processAllpass (Allpass& ap, float x, float g) noexcept;
    void frontEnd (float inL, float inR, float& a, float& b) noexcept;
    void applyDiffusers (float& a, float& b) noexcept;
    void outputBus (float wetL, float wetR, float dryL, float dryR,
                    bool stereo, float& outL, float& outR) noexcept;

    // --- geometry -----------------------------------------------------------
    double sampleRate = 44100.0;
    int mode = modeRoom;
    float windowSeconds = kReverseWindowSeconds;

    std::vector<float> pool;                       // one flat allocation (§3.3)
    std::array<float*, kNumLines> tankBuf {};
    std::array<float*, kNumLines> apBuf {};
    std::array<float*, 2> preBuf {};
    float* erBuf = nullptr;
    float* revBuf = nullptr;
    int tankLen = 0, apLen = 0, preLen = 0, erLen = 0, revLen = 0;
    int tankWrite = 0, apWrite = 0, preWrite = 0, erWrite = 0, revWrite = 0;

    std::array<std::array<Allpass, kNumDiffusers>, 2> diffuser {};   // [channel][stage]
    std::array<std::array<Allpass, 2>, 2> erAllpass {};              // [channel][stage]
    std::array<std::array<Allpass, 2>, 2> revAllpass {};             // [channel][stage]

    // --- per-line state -----------------------------------------------------
    std::array<LineState, kNumLines> lines {};
    std::array<ErTap, kNumErTaps> erTaps {};
    std::array<RevTap, kNumRevTaps> revTaps {};
    std::array<Walker, kNumWalkers> walkers {};

    // --- §3.9 Spring ---------------------------------------------------------
    // The three springs run in tankBuf[0..2] (0.36 s each, against a longest spring of
    // 61.7 ms x 1.5), so resetStep() already clears their delay lines with the tank's.
    // Only the dispersion cascades and the loop's own filter states live here.
    std::array<SpringDispersion, kNumSprings> springs {};
    float springHpState[kNumSprings] {};
    float springHpW = 0.0f;                        // 90 Hz rumble HP, fixed
    float springLfDelay = 0.0f;                    // dispersion group delay at DC, samples
    float dwellPreState[2] {}, dwellDeState[2] {}; // Dwell pre/de-emphasis shelves
    float dwellShelfW = 0.0f;
    Ramp  dwellDrive { 1.0f, 0.0f };               // reverb_diffusion as DWELL (§5.2)

    // --- §3.11 Shimmer -------------------------------------------------------
    // ONE shifter per reverb, in the Regen (in-loop) routing. Sized for every mode, like
    // the springs above, so entering Shimmer never allocates (§6.2); it lives outside
    // `pool` for the same reason they do — it is not a delay line with a tank's guard
    // region, and resetState() already owns clearing this class of state.
    PitchShifter shifter;
    Ramp  shimmerGain;                             // the §3.11-bounded in-loop gain
    float shimLpState = 0.0f;                      // in-loop LP, forced <= 4 kHz
    float shimLpW = 1.0f;
    bool  shimmerActive = false;                   // false => the shifter is not touched

    // --- shared modulation --------------------------------------------------
    float layerBCos = 1.0f, layerBSin = 0.0f;      // quadrature rotator state
    float layerBIncCos = 1.0f, layerBIncSin = 0.0f;
    // §3.6 Layer C (AMENDED 2026-08-07): one quadrature rotator per Givens pair, same
    // scheme as Layer B's — increments solved per block, advanced per sample in
    // processTank(), renormalised per block in process(). rotTheta is the block-rate
    // depth (rotMax · depthScale); 0 skips the layer outright, so reverb_mod = 0 keeps
    // the mixing matrix EXACTLY the static Hadamard, to the bit.
    float rotCos[4] { 1.0f, 0.0f, -1.0f, 0.0f };
    float rotSin[4] { 0.0f, 1.0f, 0.0f, -1.0f };
    float rotIncCos[4] { 1.0f, 1.0f, 1.0f, 1.0f };
    float rotIncSin[4] { 0.0f, 0.0f, 0.0f, 0.0f };
    float rotTheta = 0.0f;
    std::uint32_t commonRng = 0;                   // colour-axis correlated stream
    float commonTarget = 0.0f;
    int   commonCount = 0, commonSegment = 1;
    float colorDepth = 0.0f;                       // axis-1 filter mix, 0 = exactly neutral
    float colorRho = 0.0f;                         // stream correlation, 0 .. 0.5
    float colorRhoNorm = 1.0f;
    float colorQuant = 0.0f;                       // 0 = raw target, 1 = 1/32-sample steps
    float colorInterp = 0.0f;                      // axis-4 kernel blend, 0 = cubic exactly

    // --- front end ----------------------------------------------------------
    float dcInState[2] { 0.0f, 0.0f };
    float dcOutState[2] { 0.0f, 0.0f };
    float sendHpState[2] { 0.0f, 0.0f };
    float sendLpState[2] { 0.0f, 0.0f };
    Ramp dcInW, sendHpW, sendLpW;
    Ramp diffAmount;                               // 0.15 + 0.85 * diffusion
    float dcOutW = 0.0f;

    // Pre-delay, dual law (§6.5).
    float preDelayCur = 3.0f, preDelayOld = 3.0f, preDelayTarget = 3.0f;
    float preSlewPerSample = 0.0f;
    float xfadePos = 1.0f, xfadeInc = 0.0f;        // 1 = crossfade complete

    // --- output bus ---------------------------------------------------------
    std::array<std::array<Ap2, kNumSpreadSections>, 2> spread {};
    bool spreadOn = false;
    float tiltLpState[2] { 0.0f, 0.0f };
    float tiltW = 0.0f;

    // §3.4 tonal correction (AMENDED 2026-08-07): two wet-bus shelves that hand back the
    // per-band tail LEVEL the Filt1 solve moves along with the per-band decay TIME
    // (energy ∝ T60). Corners are the mode's own crossovers; gains are solved per block.
    float tcLoState[2] { 0.0f, 0.0f };
    float tcHiState[2] { 0.0f, 0.0f };
    Ramp tcLoGain { 1.0f, 0.0f }, tcHiGain { 1.0f, 0.0f };
    float tcLoW = 0.0f, tcHiW = 0.0f;
    float revColorLp = 0.0f;
    float revColorW = 1.0f;

    juce::SmoothedValue<float> mixWet { 0.25f }, widthAmount { 1.0f }, erLevelAmount { 0.5f };
    juce::SmoothedValue<float> tiltLowGain { 1.0f }, tiltHighGain { 1.0f }, duckAmount { 0.0f };
    Ramp wetFade;
    float wetFadeStart = 1.0f, wetFadeEnd = 1.0f;

    float duckEnv = 0.0f, duckAttack = 0.0f, duckRelease = 0.0f;

    // --- mode constants (refreshed by configure()) --------------------------
    float sizeMin = 0.4f, sizeMax = 1.4f;
    float sizeScaleCur = 1.0f, sizeScaleTarget = 1.0f, sizeSlewPerSample = 0.0f;
    float erToTank = 0.6f, kIn = 0.35f;
    // §3.2 decay-normalised injection (AMENDED 2026-08-07): gRef is the per-pass loop
    // gain at the 1.5 s reference decay, from the mode's MINIMUM nominal loop delay at
    // sizeScale 1.0 so it matches gMidMax's max-over-lines convention. The per-block
    // scale √(1−g²)/√(1−gRef²) rides every injection so line RMS — and with it the wet
    // level and the distance to §3.5's fixed clip knee — stops tracking reverb_decay.
    float gRef = 0.0f;
    Ramp  injectGain { 1.0f, 0.0f };
    float bassXoverHz = 450.0f, hfXoverHz = 4500.0f, hfMaxT60 = 1.0f;
    float tonalCorrect = 0.0f;                     // §3.4 tonal-correction amount, per mode
    float rotMax = 0.0f;                           // §3.6 Layer C depth, per mode
    float inSatBase = 0.15f, inSat = 0.15f;
    float modDepthMs = 0.35f, modRateHz = 0.6f;
    // §3.4 / §5.1 axis 4 (AMENDED 2026-08-07): |Hi(f_h)|_mean of the BLENDED read kernel
    // (1−c)·cubic + c·linear, tabulated over c ∈ [0, 1] at configure() and lerped per
    // block by `colorInterp` — the r_hi solve divides by the kernel the loop actually
    // reads through, which is what keeps §8's per-octave T60 gate honest at colour 1.
    // interpMagAtHf holds the current lerped value; entry 0 is the cubic kernel, so
    // colour 0 reproduces the pre-axis-4 solve to the bit.
    std::array<float, 17> interpMagTable {};
    float interpMagAtHf = 1.0f;                    // |Hi(f_h)|_mean at the live blend
    float colorLpBottomHz = 6000.0f;               // §5.1 axis-1 knob-top corner, per mode
    float colorInSatTop = 0.60f;                   // §5.1 axis-2 knob-top drive, per mode
    float colorInterpTop = 0.0f;                   // §5.1 axis-4 knob-top blend, per mode
    float colorLpW = 1.0f;
    std::array<float, kNumDiffusers> inDiffG { 0.75f, 0.75f, 0.625f, 0.625f };

    Params current;
    int clearStep = 0;
    bool prepared = false;

    JUCE_LEAK_DETECTOR (ReverbEngine)
};
} // namespace tubamp
