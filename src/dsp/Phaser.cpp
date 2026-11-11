#include "Phaser.h"

#include "FxPrimitives.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tubamp
{
namespace
{
// Tunings for the phaser engine (docs/FX-ENGINES.md §4.6). The Phase 90 and Small Stone
// constants are docs/research/fx/phaser/voicings.md, as prototype/phaser_ref.py uses them.

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr float kParamSmoothSeconds = 0.02f;
constexpr double kMaxBreakFraction = 0.49;     // of fs, before every tan() [guess: voicings.md]
constexpr double kStateFloor = 1.0e-15;        // recursive states below this are flushed once per block
constexpr int kSwitchPiece = 16;               // samples per run while a model switch is in flight
constexpr double kSwitchSpanSeconds = 0.045;   // 20 ms out, the reset, 20 ms in, with margin

// Phase 90 (P90_*).
constexpr double kP90C = 47.0e-9;              // F, allpass capacitor [circuit]
constexpr double kP90RFix = 22.0e3;            // ohm, in parallel with each JFET leg [circuit]
constexpr double kP90RS = 933.7;               // ohm, series limit of the leg at the sweep top [fit]
constexpr double kP90R1 = 1786.1;              // ohm, channel resistance at mid-sweep [fit]
constexpr double kP90Gamma = 0.8915;           // channel slope per unit of u: R_ch = R1 e^(-gamma u) [fit]
constexpr double kP90Knee = 3.892;             // sharpness m of the series limit [fit]
constexpr double kP90Beta = 0.3957;            // block logo: op-amp feedback R / feedback R [fit]
constexpr double kP90Kappa = -0.002;           // LFO curvature [fit]
constexpr double kP90GateHz = 3.535;           // one-pole smoothing between LFO and JFET gates [fit]
constexpr double kP90FbLagHz = 11736.0;        // block logo: lag in the feedback-resistor path [fit]
constexpr double kP90Dry = 0.5, kP90Wet = 0.5; // [guess]

// Small Stone (SS_*).
constexpr double kSsKappa = 0.70;              // LFO curvature [fit]

struct StoneVoicing
{
    double loHz, hiHz, feedback, dry, wet;
};

constexpr StoneVoicing kStoneOff { 355.0, 8300.0, 0.0, 0.75, 0.50 };      // [fit]
constexpr StoneVoicing kStoneOn { 99.0, 3133.5, 0.7553, 0.1266, 0.4289 }; // [fit]

// Color network, Hz: two poles shared by both branches, the feedback branch's zero (its
// second zero is at DC) and the input branch's two zeros [fit].
constexpr double kStonePole1Hz = 53.0, kStonePole2Hz = 266.0;
constexpr double kStoneFbZeroHz = 18.3;
constexpr double kStoneInZeroAHz = 16.5, kStoneInZeroBHz = 885.2;

/** Flushes a recursive state that has decayed to nothing, and drops one that is not a
    sane number, so a bad sample cannot live in a loop past the end of its block. */
inline void settle (double& state) noexcept
{
    const double a = std::abs (state);
    state = a < 1.0e6 && a >= kStateFloor ? state : 0.0; // false for NaN as well
}

//==============================================================================
/** The phaser of the old Modulation block, moved verbatim (FxBlocks.cpp at bf81728). */
class ClassicPhaser
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        maxBlockSize = (int) spec.maximumBlockSize;

        phaser.prepare (spec);

        phaser.setCentreFrequency (600.0f);
        phaser.setFeedback (0.0f);

        reset();
    }

    void reset()
    {
        phaser.reset();
    }

    void setParameters (float rateHz, float depth01, float mix01)
    {
        rate = juce::jlimit (0.01f, 100.0f, rateHz);
        depth = juce::jlimit (0.0f, 1.0f, depth01);
        mix = juce::jlimit (0.0f, 1.0f, mix01);

        phaser.setRate (rate);
        phaser.setDepth (depth);
        phaser.setMix (mix);
    }

    void process (juce::dsp::AudioBlock<float> block)
    {
        // juce::dsp::Phaser's buffers are sized for prepare()'s block; a longer block is
        // run in pieces rather than overrunning them.
        const auto total = block.getNumSamples();
        const auto step = (size_t) juce::jmax (1, maxBlockSize);

        for (size_t start = 0; start < total; start += step)
        {
            auto piece = block.getSubBlock (start, juce::jmin (step, total - start));
            juce::dsp::ProcessContextReplacing<float> context (piece);
            phaser.process (context);
        }
    }

private:
    juce::dsp::Phaser<float> phaser;
    float rate = 1.0f, depth = 0.5f, mix = 0.5f;
    int maxBlockSize = 0;
};

//==============================================================================
/** Phase 90 and Small Stone: four TPT one-pole allpass stages sharing one break
    frequency, recomputed every sample from an RC-triangle LFO; feedback loops are solved
    per sample in closed form, without a unit delay. */
class PedalPhaser
{
public:
    enum class Voicing { script, block, stoneOff, stoneOn };

    void prepare (double sampleRate)
    {
        invSampleRate = 1.0 / sampleRate;
        piOverFs = kPi / sampleRate;
        maxBreakHz = kMaxBreakFraction * sampleRate;

        p90Norm = 1.0 / (1.0 - std::exp (-kP90Kappa));
        stoneNorm = 1.0 / (1.0 - std::exp (-kSsKappa));
        gateCoeff = 1.0 - std::exp (-2.0 * kPi * kP90GateHz / sampleRate);
        rsPowKnee = std::pow (kP90RS, kP90Knee);
        r1PowKnee = std::pow (kP90R1, kP90Knee);
        lagG = tptCoefficient (kP90FbLagHz);

        for (int k = 0; k < 2; ++k)
        {
            const auto& v = k == 0 ? kStoneOff : kStoneOn;
            stoneCentre[k] = std::sqrt (v.loHz * v.hiHz);
            stoneHalfLog[k] = 0.5 * std::log (v.hiHz / v.loHz);
        }

        poleG1 = tptCoefficient (kStonePole1Hz);
        poleG2 = tptCoefficient (kStonePole2Hz);
        inShelfA = 1.0 - kStoneInZeroAHz / kStonePole1Hz;
        inShelfB = 1.0 - kStoneInZeroBHz / kStonePole2Hz;
        fbShelf = 1.0 - kStoneFbZeroHz / kStonePole2Hz;

        rate.prepare (kParamSmoothSeconds, (float) sampleRate, 1.0f);
        reset();
    }

    void reset() noexcept
    {
        for (auto& c : channels)
            c = {};

        phase = lastPhase = 0.0;
        gate = 0.0;
        gatePrimed = false;
        rate.snapToTarget();
    }

    void setRate (float hz, bool snap) noexcept
    {
        rate.setTarget (hz);

        if (snap)
            rate.snapToTarget();
    }

    void freeze (double position) noexcept { frozen = position; }
    double getLastPhase() const noexcept { return lastPhase; }

    void process (juce::dsp::AudioBlock<float> block, Voicing voicing, double syncPhase) noexcept
    {
        if (syncPhase >= 0.0)
            phase = syncPhase;

        const int numChannels = juce::jmin (2, (int) block.getNumChannels());
        const int numSamples = (int) block.getNumSamples();
        float* data[2] { block.getChannelPointer (0), numChannels > 1 ? block.getChannelPointer (1) : nullptr };

        switch (voicing)
        {
            case Voicing::script:   runPhase90<false> (data, numChannels, numSamples); break;
            case Voicing::block:    runPhase90<true> (data, numChannels, numSamples); break;
            case Voicing::stoneOff: runStone<false> (data, numChannels, numSamples); break;
            case Voicing::stoneOn:  runStone<true> (data, numChannels, numSamples); break;
        }

        for (auto& c : channels)
        {
            for (auto& s : c.stage)
                settle (s);

            settle (c.lag);
            settle (c.inA);
            settle (c.inB);
            settle (c.fbA);
            settle (c.fbB);
        }
    }

private:
    struct Channel
    {
        double stage[4] {};
        double lag = 0.0;              // Phase 90 block: feedback lag
        double inA = 0.0, inB = 0.0;   // Small Stone Color: input branch
        double fbA = 0.0, fbB = 0.0;   // Small Stone Color: feedback branch
    };

    double tptCoefficient (double hz) const noexcept
    {
        const double g = std::tan (piOverFs * std::min (hz, maxBreakHz));
        return g / (1.0 + g);
    }

    /** Relaxation-oscillator triangle in [-1, 1] whose halves are RC charge curves
        (prototype rc_triangle); phase 0 is the bottom. */
    static double rcTriangle (double p, double kappa, double norm) noexcept
    {
        const bool rising = p < 0.5;
        const double w = rising ? 2.0 * p : 2.0 * p - 1.0;
        const double s = (1.0 - std::exp (-kappa * w)) * norm;
        return rising ? 2.0 * s - 1.0 : 1.0 - 2.0 * s;
    }

    /** The sweep position for this sample, advancing the LFO. */
    template <bool phase90>
    double nextSweep() noexcept
    {
        if (! std::isnan (frozen))
            return frozen;

        lastPhase = phase;
        double sweep = phase90 ? rcTriangle (phase, kP90Kappa, p90Norm) : rcTriangle (phase, kSsKappa, stoneNorm);
        phase += (double) rate.next() * invSampleRate;

        if (phase >= 1.0)
            phase -= 1.0;

        if constexpr (phase90)
        {
            // The gate smoothing starts on the first LFO value instead of rising from 0.
            if (! gatePrimed)
            {
                gate = sweep;
                gatePrimed = true;
            }

            gate += gateCoeff * (sweep - gate);
            sweep = gate;
        }

        return sweep;
    }

    /** Leg R(u) = (RS^m + (R1 e^(-gamma u))^m)^(1/m) in parallel with the fixed resistor,
        with the stage capacitor: fc = (1/R_fix + 1/R(u)) / (2 pi C). */
    double phase90BreakHz (double u) const noexcept
    {
        const double legConductance = std::pow (rsPowKnee + r1PowKnee * std::exp (-kP90Knee * kP90Gamma * u), -1.0 / kP90Knee);
        return (1.0 / kP90RFix + legConductance) / (2.0 * kPi * kP90C);
    }

    template <bool block>
    void runPhase90 (float* const* data, int numChannels, int numSamples) noexcept
    {
        constexpr double beta = kP90Beta;

        for (int i = 0; i < numSamples; ++i)
        {
            const double g = std::tan (piOverFs * std::min (phase90BreakHz (nextSweep<true>()), maxBreakHz));
            const double G = g / (1.0 + g);
            const double a = 1.0 - 2.0 * G;
            const double hold = -2.0 * (1.0 - G);
            const double loopInv = block ? 1.0 / (1.0 + beta * lagG * a * a) : 1.0;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto& c = channels[ch];
                auto& st = c.stage;
                const double x = (double) data[ch][i];

                // Stage 1: A(x) = x - 2 lp, the op-amp stage (-1 at DC, +1 at HF).
                double v = (x - st[0]) * G;
                double lp = v + st[0];
                st[0] = lp + v;
                const double v1 = x - 2.0 * lp;

                v = (v1 - st[1]) * G;
                lp = v + st[1];
                st[1] = lp + v;
                double u;

                if constexpr (block)
                {
                    // The feedback resistor loads stage 2's inverting input (gain 2 + beta) and
                    // closes a loop over stages 3 and 4 through the lag one-pole.
                    const double s2 = (1.0 + beta) * v1 - (2.0 + beta) * lp;
                    const double y4 = (a * a * (s2 - beta * (1.0 - lagG) * c.lag) + a * hold * st[2] + hold * st[3]) * loopInv;
                    v = (y4 - c.lag) * lagG;
                    lp = v + c.lag;
                    c.lag = lp + v;
                    u = s2 - beta * lp;
                }
                else
                {
                    u = v1 - 2.0 * lp;
                }

                v = (u - st[2]) * G;
                lp = v + st[2];
                st[2] = lp + v;
                const double y3 = u - 2.0 * lp;

                v = (y3 - st[3]) * G;
                lp = v + st[3];
                st[3] = lp + v;

                data[ch][i] = (float) (kP90Dry * x + kP90Wet * (y3 - 2.0 * lp));
            }
        }
    }

    template <bool color>
    void runStone (float* const* data, int numChannels, int numSamples) noexcept
    {
        const auto& voicing = color ? kStoneOn : kStoneOff;
        const double centre = stoneCentre[color ? 1 : 0], halfLog = stoneHalfLog[color ? 1 : 0];
        const double fb = voicing.feedback, dry = voicing.dry, wet = voicing.wet;
        const double g1 = poleG1, g2 = poleG2;
        // The feedback branch F is affine in y4 for the current sample: F = fbGain * y4 + bF.
        const double fbGain = (1.0 - fbShelf * g2) * (1.0 - g1);
        const double fbHold = fbShelf * (1.0 - g2);

        for (int i = 0; i < numSamples; ++i)
        {
            const double fc = centre * std::exp (halfLog * nextSweep<false>());
            const double g = std::tan (piOverFs * std::min (fc, maxBreakHz));
            const double G = g / (1.0 + g);
            const double p = 2.0 * G - 1.0;
            const double p4 = p * p * p * p;
            const double hold = 2.0 * (1.0 - G);
            // fb * fbGain * p4 <= 0.76, so the divisor stays above 0.24.
            const double loopInv = color ? 1.0 / (1.0 - fb * fbGain * p4) : 1.0;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto& c = channels[ch];
                auto& st = c.stage;
                const double x = (double) data[ch][i];
                double u = x;

                if constexpr (color)
                {
                    // The chain is y4 = p^4 u + K, K the four states pushed through it.
                    double K = 0.0;

                    for (int k = 0; k < 4; ++k)
                        K = p * K + hold * st[k];

                    double v = (x - c.inA) * g1;
                    double lp = v + c.inA;
                    c.inA = lp + v;
                    const double xa = x - inShelfA * lp;

                    v = (xa - c.inB) * g2;
                    lp = v + c.inB;
                    c.inB = lp + v;
                    const double xs = xa - inShelfB * lp;

                    const double bF = -fbGain * c.fbA - fbHold * c.fbB;
                    const double y4 = (p4 * (xs + fb * bF) + K) * loopInv;

                    v = (y4 - c.fbA) * g1;
                    lp = v + c.fbA;
                    c.fbA = lp + v;
                    const double h = y4 - lp;

                    v = (h - c.fbB) * g2;
                    lp = v + c.fbB;
                    c.fbB = lp + v;
                    u = xs + fb * (h - fbShelf * lp);
                }

                for (int k = 0; k < 4; ++k)
                {
                    const double v = (u - st[k]) * G;
                    const double lp = v + st[k];
                    st[k] = lp + v;
                    u = 2.0 * lp - u;
                }

                data[ch][i] = (float) (dry * x + wet * u);
            }
        }
    }

    Channel channels[2];
    double invSampleRate = 1.0 / 48000.0, piOverFs = kPi / 48000.0, maxBreakHz = 23520.0;
    double phase = 0.0, lastPhase = 0.0, gate = 0.0, frozen = std::numeric_limits<double>::quiet_NaN();
    bool gatePrimed = false;

    double p90Norm = 1.0, stoneNorm = 1.0, gateCoeff = 1.0, rsPowKnee = 1.0, r1PowKnee = 1.0, lagG = 0.5;
    double stoneCentre[2] {}, stoneHalfLog[2] {};
    double poleG1 = 0.0, poleG2 = 0.0, inShelfA = 0.0, inShelfB = 0.0, fbShelf = 0.0;

    fx::Smoother rate;
};

/** The switcher key: Classic ignores color; the other models fade on a color change too. */
int engineKey (const PhaserParams& p) noexcept
{
    return p.model == (int) PhaserFx::Model::classic ? 0 : p.model * 2 + (p.color ? 1 : 0);
}

PedalPhaser::Voicing voicingFor (int key) noexcept
{
    switch (key)
    {
        case 2:  return PedalPhaser::Voicing::script;
        case 3:  return PedalPhaser::Voicing::block;
        case 4:  return PedalPhaser::Voicing::stoneOff;
        default: return PedalPhaser::Voicing::stoneOn;
    }
}
} // namespace

//==============================================================================
struct PhaserFx::Impl
{
    /** Runs `block` (which starts `offset` samples into the host block) on the requested
        engine through the switcher. */
    void run (juce::dsp::AudioBlock<float> block, int key, size_t offset)
    {
        const int active = switcher.begin (block, key, [this] (int k, bool)
        {
            if (k == 0)
            {
                classic.reset();
            }
            else
            {
                pedal.reset();
                pedalWasReset = true;
            }

            return true;
        });

        if (! switcher.isHolding())
        {
            if (active == 0)
            {
                classic.process (block);
            }
            else
            {
                // The song position is given for the host block's first sample; a pedal reset
                // further in picks it up where the transport is by then.
                double sync = -1.0;

                if (params.syncPhase >= 0.0 && (offset == 0 || pedalWasReset))
                {
                    sync = params.syncPhase + (double) offset * (double) juce::jlimit (0.01f, 20.0f, params.rateHz) / sampleRate;
                    sync -= std::floor (sync);
                }

                pedal.process (block, voicingFor (active), sync);
            }
        }

        pedalWasReset = false;
        switcher.end (block);
    }

    ClassicPhaser classic;
    PedalPhaser pedal;
    fx::EngineSwitcher switcher;
    PhaserParams params;
    double sampleRate = 48000.0;
    int requestedKey = -1, switchSamplesLeft = 0, switchSpan = 0;
    bool snapEngine = true, snapParams = true, pedalWasReset = false;
};

PhaserFx::PhaserFx() : impl (std::make_unique<Impl>()) {}
PhaserFx::~PhaserFx() = default;

void PhaserFx::prepare (const juce::dsp::ProcessSpec& spec)
{
    impl->sampleRate = spec.sampleRate;
    impl->switchSpan = (int) std::ceil (kSwitchSpanSeconds * spec.sampleRate) + kSwitchPiece;
    impl->classic.prepare (spec);
    impl->pedal.prepare (spec.sampleRate);
    impl->switcher.prepare (spec.sampleRate, (int) spec.maximumBlockSize, (int) spec.numChannels,
                            engineKey (impl->params));
    impl->snapEngine = impl->snapParams = true;
}

void PhaserFx::reset()
{
    impl->classic.reset();
    impl->pedal.reset();
    impl->snapEngine = impl->snapParams = true;
}

void PhaserFx::setParameters (const PhaserParams& params)
{
    auto& s = *impl;
    const PhaserParams defaults;

    s.params = params;
    s.params.model = juce::jlimit (0, numModels - 1, params.model);
    s.params.rateHz = std::isfinite (params.rateHz) ? params.rateHz : defaults.rateHz;
    s.params.depth = std::isfinite (params.depth) ? params.depth : defaults.depth;
    s.params.mix = std::isfinite (params.mix) ? params.mix : defaults.mix;
    s.params.syncPhase = std::isfinite (params.syncPhase) && params.syncPhase >= 0.0
                             ? params.syncPhase - std::floor (params.syncPhase)
                             : -1.0;

    if (s.params.model == (int) Model::classic)
        s.classic.setParameters (s.params.rateHz, s.params.depth, s.params.mix);

    s.pedal.setRate (juce::jlimit (0.01f, 20.0f, s.params.rateHz), s.snapParams);
    s.snapParams = false;
}

void PhaserFx::process (juce::dsp::AudioBlock<float> block)
{
    auto& s = *impl;
    const auto numSamples = block.getNumSamples();

    if (numSamples == 0)
        return;

    const int key = engineKey (s.params);

    if (s.snapEngine)
    {
        // Nothing has been heard since prepare() or reset(): start on the requested model.
        s.snapEngine = false;
        s.switcher.snapTo (key);
        s.requestedKey = key;
        s.switchSamplesLeft = 0;
    }

    if (key != s.requestedKey)
    {
        s.requestedKey = key;
        s.switchSamplesLeft = s.switchSpan;
    }

    if (s.switchSamplesLeft <= 0)
    {
        s.run (block, key, 0);
        return;
    }

    // While a switch is in flight the block runs in short pieces: the switcher resets the
    // incoming engine at the start of a run, so the fade back in then begins within one
    // piece of the fade out ending instead of waiting for the next host block.
    for (size_t start = 0; start < numSamples; start += (size_t) kSwitchPiece)
        s.run (block.getSubBlock (start, juce::jmin ((size_t) kSwitchPiece, numSamples - start)), key, start);

    s.switchSamplesLeft -= (int) numSamples;
}

void PhaserFx::freezeSweep (double position) noexcept { impl->pedal.freeze (position); }
double PhaserFx::getLfoPhase() const noexcept { return impl->pedal.getLastPhase(); }
} // namespace tubamp
