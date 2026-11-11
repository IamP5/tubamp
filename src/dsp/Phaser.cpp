#include "Phaser.h"

#include "FxPrimitives.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>

namespace tubamp
{
namespace
{
// Tunings for the phaser engine (docs/FX-ENGINES.md §4.6). The Phase 90 and Small Stone
// constants are docs/research/fx/phaser/voicings.md, as prototype/phaser_ref.py uses them.

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr float kParamSmoothSeconds = 0.02f;
constexpr double kStateFloor = 1.0e-15;        // recursive states below this are flushed once per block
constexpr int kSwitchPiece = 16;               // samples per run while a model switch is in flight
constexpr double kSwitchSpanSeconds = 0.045;   // 20 ms out, the reset, 20 ms in, with margin
constexpr double kGlideSeconds = 0.010;        // a song-position jump glides the sweep over this [choice: voicings.md]
constexpr double kGlideThreshold = 1.0e-6;     // sweep-position steps below this are taken as they come [choice: voicings.md]

// Discretisation (voicings.md, "Shared building block").
constexpr int kTableN = 256;                   // coefficient table intervals over the sweep position [choice]
constexpr double kMatchCap = 0.25;             // highest matching frequency of pairCoefficients, of fs [choice]
constexpr double kNotchEps = 1.0e-4;           // half step in ln f of the notch condition [choice]
constexpr double kT8 = 0.414213562373095048801688724209698079; // tan(pi/8)

// Phase 90 (P90_*).
constexpr double kP90C = 47.0e-9;              // F, allpass capacitor [circuit]
constexpr double kP90RFix = 22.0e3;            // ohm, in parallel with each JFET leg [circuit]
constexpr double kP90RS = 916.8;               // ohm, series limit of the leg at the sweep top [fit]
constexpr double kP90R1 = 1777.7;              // ohm, channel resistance at mid-sweep [fit]
constexpr double kP90Gamma = 0.8952;           // channel slope per unit of u: R_ch = R1 e^(-gamma u) [fit]
constexpr double kP90Knee = 3.876;             // sharpness m of the series limit [fit]
constexpr double kP90Beta = 0.3957;            // block logo: op-amp feedback R / feedback R [fit]
constexpr double kP90Kappa = -0.007;           // LFO curvature [fit]
constexpr double kP90GateHz = 3.542;           // one-pole smoothing between LFO and JFET gates [fit]
constexpr double kP90FbLagHz = 14746.0;        // block logo: lag in the feedback-resistor path [fit]
constexpr double kP90Dry = 0.5, kP90Wet = 0.5; // [guess]
constexpr double kP90ScriptMakeup = 1.365, kP90BlockMakeup = 1.189; // +2.7 / +1.5 dB [choice]

// Small Stone (SS_*).
constexpr double kSsKappa = 0.702;              // LFO curvature [fit]

struct StoneVoicing
{
    double loHz, hiHz, feedback, dry, wet, makeup;
};

constexpr StoneVoicing kStoneOff { 347.7, 8931.5, 0.0, 0.75, 0.50, 1.059 };    // [fit], make-up +0.5 dB [choice]
constexpr StoneVoicing kStoneOn { 99.4, 3068.3, 0.7548, 0.1318, 0.4395, 1.0 }; // [fit], make-up 0 dB [choice]

// Color network, Hz: two poles shared by both branches, the feedback branch's zero (its
// second zero is at DC) and the input branch's two zeros [fit].
constexpr double kStonePole1Hz = 49.8, kStonePole2Hz = 267.9;
constexpr double kStoneFbZeroHz = 12.0;
constexpr double kStoneInZeroAHz = 12.7, kStoneInZeroBHz = 862.1;

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
// Coefficient design (voicings.md, "Shared building block"; prototype stage_g and its helpers).
// The voicings are analog transfer functions. In them `s` is the Laplace variable and g a
// one-pole's break: s = j f with g in Hz (analog), or s = j tan(pi f / fs) with g the TPT
// coefficient (the trapezoidal map on the unit circle). prepare() solves, per sweep position,
// the TPT coefficients whose digital notches land on the analog ones and tabulates them.

using Complex = std::complex<double>;

Complex allpassOf (Complex s, double g) { return (s - g) / (s + g); }
Complex lowpassOf (Complex s, double g) { return g / (g + s); }

struct Coefficients
{
    double g[4] {};
    double lag = 0.0;
};

Complex blockResponse (Complex s, const double* g, double gl)
{
    const Complex a34 = allpassOf (s, g[2]) * allpassOf (s, g[3]);
    const Complex s2 = ((1.0 + kP90Beta) * s - g[1]) / (s + g[1]);
    return 0.5 + 0.5 * allpassOf (s, g[0]) * s2 * a34 / (1.0 + kP90Beta * lowpassOf (s, gl) * a34);
}

Complex stoneOnResponse (Complex s, const double* g, double gn1, double gn2)
{
    const double ka = 1.0 - kStoneInZeroAHz / kStonePole1Hz, kb = 1.0 - kStoneInZeroBHz / kStonePole2Hz;
    const double kf = 1.0 - kStoneFbZeroHz / kStonePole2Hz;
    const Complex p4 = allpassOf (s, g[0]) * allpassOf (s, g[1]) * allpassOf (s, g[2]) * allpassOf (s, g[3]);
    const Complex ha = (1.0 - ka * lowpassOf (s, gn1)) * (1.0 - kb * lowpassOf (s, gn2));
    const Complex fb = (1.0 - lowpassOf (s, gn1)) * (1.0 - kf * lowpassOf (s, gn2));
    return kStoneOn.dry + kStoneOn.wet * ha * p4 / (1.0 - kStoneOn.feedback * fb * p4);
}

/** Zero where |h| has a minimum or maximum at f: ln|h|^2 a step above f minus a step below. */
template <typename Response>
double notchCondition (const Response& h, double f)
{
    return 2.0 * (std::log (std::abs (h (f * std::exp (kNotchEps)))) - std::log (std::abs (h (f * std::exp (-kNotchEps)))));
}

/** Newton's method with a forward-difference Jacobian and a backtracking line search, as the
    prototype's newton(): the step, scaled down as a whole until no unknown moves by more than
    0.2, is halved until the residual's sum of squares falls (at most 30 times). */
template <int N, typename Residual>
bool solveNewton (const Residual& residual, double (&x)[N], double step = 1.0e-7)
{
    constexpr int iterations = 40;
    constexpr double tol = 1.0e-12, clip = 0.2;
    double r[N], rs[N], jac[N][N + 1], xs[N], dx[N];

    const auto measure = [] (const double (&v)[N], double& maxAbs, double& sumSq)
    {
        maxAbs = sumSq = 0.0;

        for (double e : v)
        {
            if (! std::isfinite (e))
                return false;

            maxAbs = std::max (maxAbs, std::abs (e));
            sumSq += e * e;
        }

        return true;
    };

    double maxAbs, sumSq;
    residual (x, r);

    for (int it = 0; it < iterations; ++it)
    {
        if (! measure (r, maxAbs, sumSq))
            return false;

        if (maxAbs < tol)
            return true;

        for (int j = 0; j < N; ++j)
        {
            std::copy (std::begin (x), std::end (x), std::begin (xs));
            xs[j] += step;
            residual (xs, rs);

            for (int i = 0; i < N; ++i)
                jac[i][j] = (rs[i] - r[i]) / step;
        }

        for (int i = 0; i < N; ++i)
            jac[i][N] = r[i];

        // Gaussian elimination with partial pivoting.
        for (int c = 0; c < N; ++c)
        {
            int p = c;

            for (int i = c + 1; i < N; ++i)
                if (std::abs (jac[i][c]) > std::abs (jac[p][c]))
                    p = i;

            if (jac[p][c] == 0.0 || ! std::isfinite (jac[p][c]))
                return false;

            for (int k = 0; k <= N; ++k)
                std::swap (jac[c][k], jac[p][k]);

            for (int i = c + 1; i < N; ++i)
            {
                const double f = jac[i][c] / jac[c][c];

                for (int k = c; k <= N; ++k)
                    jac[i][k] -= f * jac[c][k];
            }
        }

        double big = 0.0;

        for (int i = N - 1; i >= 0; --i)
        {
            double v = jac[i][N];

            for (int k = i + 1; k < N; ++k)
                v -= jac[i][k] * dx[k];

            dx[i] = v / jac[i][i];
            big = std::max (big, std::abs (dx[i]));
        }

        if (! std::isfinite (big))
            return false;

        if (big > clip)
            for (auto& d : dx)
                d *= clip / big;

        bool improved = false;

        for (int h = 0; h < 30 && ! improved; ++h)
        {
            for (int i = 0; i < N; ++i)
                xs[i] = x[i] - dx[i];

            residual (xs, rs);
            double nMax, nSum;
            improved = measure (rs, nMax, nSum) && nSum < sumSq;

            if (! improved)
                for (auto& d : dx)
                    d *= 0.5;
        }

        if (! improved)
            return false;

        std::copy (std::begin (xs), std::end (xs), std::begin (x));
        std::copy (std::begin (rs), std::end (rs), std::begin (r));
    }

    return measure (r, maxAbs, sumSq) && maxAbs < 1.0e3 * tol;
}

/** Frequency of the minimum of |h| reached by Newton from f0, or a negative value if there is
    none in [lo, hi]. */
template <typename Response>
double analogNotch (const Response& h, double f0, double lo, double hi)
{
    double x[1] { std::log (f0) };
    const bool ok = solveNewton<1> ([&h] (const double (&v)[1], double (&r)[1]) { r[0] = notchCondition (h, std::exp (v[0])); },
                                    x, 1.0e-5);
    const double f = std::exp (x[0]);

    if (! ok || f < lo || f > hi || notchCondition (h, f * 1.001) <= 0.0)
        return -1.0;

    return f;
}

/** TPT coefficient of a one-pole with analog break fBreak whose response is exact at fMatch. */
double warpedCoefficient (double fBreak, double fs, double fMatch)
{
    fMatch = std::min (fMatch, 0.49 * fs);
    return std::tan (kPi * fMatch / fs) * fBreak / fMatch;
}

/** Coefficients ga < gb of two one-pole allpasses, each used twice, whose four-stage chain has
    the phase of four analog stages at fc exactly at fa and at fb (fb capped at kMatchCap fs).
    With c = 1/g the condition at one frequency is linear in c_a + c_b and c_a c_b. */
void pairCoefficients (double fc, double fs, double fa, double fb, double& ga, double& gb)
{
    fb = std::min (fb, kMatchCap * fs);

    if (fb > 1.0001 * fa)
    {
        double row[2][3];
        const double f[2] { fa, fb };

        for (int k = 0; k < 2; ++k)
        {
            const double q = f[k] / fc, t = std::tan (kPi * f[k] / fs);
            row[k][0] = t * (1.0 - q * q);
            row[k][1] = 2.0 * q * t * t;
            row[k][2] = 2.0 * q;
        }

        const double det = row[0][0] * row[1][1] - row[1][0] * row[0][1];

        if (det != 0.0)
        {
            const double s = (row[0][2] * row[1][1] - row[1][2] * row[0][1]) / det;
            const double p = (row[0][0] * row[1][2] - row[1][0] * row[0][2]) / det;
            const double r = std::sqrt (std::max (s * s - 4.0 * p, 0.0));

            if (p > 0.0 && s > r && std::isfinite (s))
            {
                ga = 2.0 / (s + r);
                gb = 2.0 / (s - r);
                return;
            }
        }
    }

    ga = gb = warpedCoefficient (fc, fs, fa);
}

enum class Design { script, block, stoneOff, stoneOn };

/** The prototype's stage_g(): TPT coefficients at analog break frequency fc. `solution` holds
    the neighbouring table entry's solved unknowns: the solve starts there and, where it does not
    converge, keeps them (continuation along the sweep); it is updated for the next entry. */
Coefficients designStages (Design design, double fc, double fs, double (&solution)[3])
{
    Coefficients c;
    const auto digital = [fs] (double f) { return Complex (0.0, std::tan (kPi * f / fs)); };
    const auto analog = [] (double f) { return Complex (0.0, f); };
    double ga, gb;

    if (design == Design::script || design == Design::stoneOff || design == Design::stoneOn)
    {
        pairCoefficients (fc, fs, kT8 * fc, fc / kT8, ga, gb);

        if (design == Design::stoneOn)
        {
            const double gn1 = std::tan (kPi * kStonePole1Hz / fs), gn2 = std::tan (kPi * kStonePole2Hz / fs);
            const double ag[4] { fc, fc, fc, fc };
            const auto ha = [&] (double f) { return stoneOnResponse (analog (f), ag, kStonePole1Hz, kStonePole2Hz); };
            const double n1 = analogNotch (ha, 0.42 * fc, 0.3 * fc, 0.8 * fc);
            double scale = 0.0;

            if (n1 > 0.0 && n1 * std::exp (kNotchEps) < 0.45 * fs)
            {
                double x[1] { solution[0] };
                const bool ok = solveNewton<1> ([&] (const double (&v)[1], double (&r)[1])
                {
                    const double k = std::exp (v[0]);
                    const double g[4] { ga * k, gb * k, ga * k, gb * k };
                    r[0] = notchCondition ([&] (double f) { return stoneOnResponse (digital (f), g, gn1, gn2); }, n1);
                }, x);

                scale = ok ? x[0] : solution[0];
            }

            solution[0] = scale;
            ga *= std::exp (scale);
            gb *= std::exp (scale);
        }

        c.g[0] = c.g[2] = ga;
        c.g[1] = c.g[3] = gb;
        return c;
    }

    const double ag[4] { fc, fc, fc, fc };
    const auto ha = [&] (double f) { return blockResponse (analog (f), ag, kP90FbLagHz); };
    const double n1 = analogNotch (ha, 0.45 * fc, 0.3 * fc, 0.65 * fc);
    const double n2 = analogNotch (ha, 2.07 * fc, 1.4 * fc, 3.0 * fc);

    if (n1 <= 0.0)
    {
        std::fill (std::begin (solution), std::end (solution), 0.0);
        const double g = warpedCoefficient (fc, fs, kT8 * fc);
        c.g[0] = c.g[1] = c.g[2] = c.g[3] = g;
        c.lag = warpedCoefficient (kP90FbLagHz, fs, kT8 * fc);
        return c;
    }

    const double gs = warpedCoefficient (fc, fs, n1);
    c.lag = warpedCoefficient (kP90FbLagHz, fs, n1);
    pairCoefficients (fc, fs, n1, n2 > 0.0 ? n2 : fc / kT8, ga, gb);
    c.g[0] = c.g[1] = gs;
    c.g[2] = ga;
    c.g[3] = gb;

    if (n2 <= 0.0 || n2 * std::exp (kNotchEps) >= 0.45 * fs)
    {
        std::fill (std::begin (solution), std::end (solution), 0.0);
        return c;
    }

    const double depth = std::log (std::abs (ha (n1)));
    const auto gains = [&] (const double (&v)[3], double* g)
    {
        g[0] = gs * std::exp (v[0]);
        g[1] = gs;
        g[2] = ga * std::exp (v[1] + v[2]);
        g[3] = gb * std::exp (v[1] - v[2]);
    };
    double x[3] { solution[0], solution[1], solution[2] };
    const bool ok = solveNewton<3> ([&] (const double (&v)[3], double (&r)[3])
    {
        double g[4];
        gains (v, g);
        const auto hd = [&] (double f) { return blockResponse (digital (f), g, c.lag); };
        r[0] = notchCondition (hd, n1);
        r[1] = notchCondition (hd, n2);
        r[2] = std::log (std::abs (hd (n1))) - depth;
    }, x);

    if (ok)
        std::copy (std::begin (x), std::end (x), std::begin (solution));

    gains (solution, c.g);
    return c;
}

/** Relaxation-oscillator triangle in [-1, 1] whose halves are RC charge curves
    (prototype rc_triangle); phase 0 is the bottom. */
double rcTriangle (double p, double kappa, double norm) noexcept
{
    const bool rising = p < 0.5;
    const double w = rising ? 2.0 * p : 2.0 * p - 1.0;
    const double s = (1.0 - std::exp (-kappa * w)) * norm;
    return rising ? 2.0 * s - 1.0 : 1.0 - 2.0 * s;
}

/** Leg R(u) = (RS^m + (R1 e^(-gamma u))^m)^(1/m) in parallel with the fixed resistor, with the
    stage capacitor: fc = (1/R_fix + 1/R(u)) / (2 pi C). */
double phase90BreakHz (double u) noexcept
{
    const double leg = std::pow (std::pow (kP90RS, kP90Knee) + std::pow (kP90R1 * std::exp (-kP90Gamma * u), kP90Knee), 1.0 / kP90Knee);
    return (1.0 / kP90RFix + 1.0 / leg) / (2.0 * kPi * kP90C);
}

double stoneBreakHz (const StoneVoicing& v, double w) noexcept
{
    return std::sqrt (v.loHz * v.hiHz) * std::exp (0.5 * std::log (v.hiHz / v.loHz) * w);
}

//==============================================================================
/** Phase 90 and Small Stone: four TPT one-pole allpass stages whose coefficients follow the
    sweep position through a table built in prepare(); feedback loops are solved per sample in
    closed form, without a unit delay. */
class PedalPhaser
{
public:
    enum class Voicing { script, block, stoneOff, stoneOn };

    void prepare (double sampleRate)
    {
        invSampleRate = 1.0 / sampleRate;
        glideSamples = std::max (1, (int) std::lround (kGlideSeconds * sampleRate));

        p90Norm = 1.0 / (1.0 - std::exp (-kP90Kappa));
        stoneNorm = 1.0 / (1.0 - std::exp (-kSsKappa));
        gateCoeff = 1.0 - std::exp (-2.0 * kPi * kP90GateHz / sampleRate);

        const Design designs[4] { Design::script, Design::block, Design::stoneOff, Design::stoneOn };

        for (size_t v = 0; v < 4; ++v)
        {
            double solution[3] {};

            // From the sweep bottom up, each solve starting at the last.
            for (size_t i = 0; i <= (size_t) kTableN; ++i)
            {
                const double w = -1.0 + 2.0 * (double) i / kTableN;
                const double fc = v < 2 ? phase90BreakHz (w) : stoneBreakHz (v == 2 ? kStoneOff : kStoneOn, w);
                const auto c = designStages (designs[v], fc, sampleRate, solution);

                for (size_t k = 0; k < 4; ++k)
                    tables[v][i][k] = c.g[k] / (1.0 + c.g[k]);

                tables[v][i][4] = c.lag / (1.0 + c.lag);
            }
        }

        const auto tpt = [sampleRate] (double hz) { const double g = std::tan (kPi * hz / sampleRate); return g / (1.0 + g); };
        poleG1 = tpt (kStonePole1Hz);
        poleG2 = tpt (kStonePole2Hz);
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
        glideOffset = 0.0;
        glideLeft = 0;
        phasePrimed = false;
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
        const bool phase90 = voicing == Voicing::script || voicing == Voicing::block;

        if (syncPhase >= 0.0)
        {
            // A song-position jump is honoured, but the sweep glides from where it was to the new
            // phase's position instead of stepping the break frequency.
            if (phasePrimed)
            {
                const double kappa = phase90 ? kP90Kappa : kSsKappa, norm = phase90 ? p90Norm : stoneNorm;
                glideOffset = rcTriangle (phase, kappa, norm) + pendingGlide() - rcTriangle (syncPhase, kappa, norm);
                glideLeft = std::abs (glideOffset) > kGlideThreshold ? glideSamples : 0;
            }

            phase = syncPhase;
        }

        phasePrimed = true;

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

    using Table = std::array<std::array<double, 5>, kTableN + 1>;

    /** Raised-cosine remainder of the glide offset at the current sample. */
    double pendingGlide() const noexcept
    {
        return glideLeft > 0 ? glideOffset * (0.5 - 0.5 * std::cos (kPi * glideLeft / glideSamples)) : 0.0;
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

        if (glideLeft > 0)
        {
            sweep += pendingGlide();
            --glideLeft;
        }

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

    /** Linear interpolation of the voicing's table at sweep position w (prototype lookup). */
    static void lookup (const Table& table, double w, double* G, int count) noexcept
    {
        double t = (w + 1.0) * (0.5 * kTableN);
        t = t >= 0.0 ? std::min (t, (double) kTableN) : 0.0; // false for NaN as well
        const int i = std::min ((int) t, kTableN - 1);
        const double fr = t - i;

        for (int k = 0; k < count; ++k)
            G[k] = table[(size_t) i][(size_t) k] + fr * (table[(size_t) i + 1][(size_t) k] - table[(size_t) i][(size_t) k]);
    }

    template <bool block>
    void runPhase90 (float* const* data, int numChannels, int numSamples) noexcept
    {
        constexpr double beta = kP90Beta;
        const auto& table = tables[block ? 1 : 0];
        const double makeup = block ? kP90BlockMakeup : kP90ScriptMakeup;

        for (int i = 0; i < numSamples; ++i)
        {
            double G[5];
            lookup (table, nextSweep<true>(), G, block ? 5 : 4);
            const double a3 = 1.0 - 2.0 * G[2], a4 = 1.0 - 2.0 * G[3];
            const double hold3 = -2.0 * (1.0 - G[2]), hold4 = -2.0 * (1.0 - G[3]);
            const double lagG = block ? G[4] : 0.0;
            const double loopInv = block ? 1.0 / (1.0 + beta * lagG * a3 * a4) : 1.0;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto& c = channels[ch];
                auto& st = c.stage;
                const double x = (double) data[ch][i];

                // Stage 1: A(x) = x - 2 lp, the op-amp stage (-1 at DC, +1 at HF).
                double v = (x - st[0]) * G[0];
                double lp = v + st[0];
                st[0] = lp + v;
                const double v1 = x - 2.0 * lp;

                v = (v1 - st[1]) * G[1];
                lp = v + st[1];
                st[1] = lp + v;
                double u;

                if constexpr (block)
                {
                    // The feedback resistor loads stage 2's inverting input (gain 2 + beta) and
                    // closes a loop over stages 3 and 4 through the lag one-pole.
                    const double s2 = (1.0 + beta) * v1 - (2.0 + beta) * lp;
                    const double y4 = (a4 * (a3 * (s2 - beta * (1.0 - lagG) * c.lag) + hold3 * st[2]) + hold4 * st[3]) * loopInv;
                    v = (y4 - c.lag) * lagG;
                    lp = v + c.lag;
                    c.lag = lp + v;
                    u = s2 - beta * lp;
                }
                else
                {
                    u = v1 - 2.0 * lp;
                }

                v = (u - st[2]) * G[2];
                lp = v + st[2];
                st[2] = lp + v;
                const double y3 = u - 2.0 * lp;

                v = (y3 - st[3]) * G[3];
                lp = v + st[3];
                st[3] = lp + v;

                data[ch][i] = (float) (makeup * (kP90Dry * x + kP90Wet * (y3 - 2.0 * lp)));
            }
        }
    }

    template <bool color>
    void runStone (float* const* data, int numChannels, int numSamples) noexcept
    {
        const auto& voicing = color ? kStoneOn : kStoneOff;
        const auto& table = tables[color ? 3 : 2];
        const double fb = voicing.feedback, dry = voicing.makeup * voicing.dry, wet = voicing.makeup * voicing.wet;
        const double g1 = poleG1, g2 = poleG2;
        // The feedback branch F is affine in y4 for the current sample: F = fbGain * y4 + bF.
        const double fbGain = (1.0 - fbShelf * g2) * (1.0 - g1);
        const double fbHold = fbShelf * (1.0 - g2);

        for (int i = 0; i < numSamples; ++i)
        {
            double G[4], p[4], hold[4];
            lookup (table, nextSweep<false>(), G, 4);

            for (int k = 0; k < 4; ++k)
            {
                p[k] = 2.0 * G[k] - 1.0;
                hold[k] = 2.0 * (1.0 - G[k]);
            }

            const double p4 = p[0] * p[1] * p[2] * p[3];
            // fb * fbGain * |p4| <= 0.76, so the divisor stays above 0.24.
            const double loopInv = color ? 1.0 / (1.0 - fb * fbGain * p4) : 1.0;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto& c = channels[ch];
                auto& st = c.stage;
                const double x = (double) data[ch][i];
                double u = x;

                if constexpr (color)
                {
                    // The chain is y4 = p4 u + K, K the four states pushed through it.
                    double K = 0.0;

                    for (int k = 0; k < 4; ++k)
                        K = p[k] * K + hold[k] * st[k];

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
                    const double v = (u - st[k]) * G[k];
                    const double lp = v + st[k];
                    st[k] = lp + v;
                    u = 2.0 * lp - u;
                }

                data[ch][i] = (float) (dry * x + wet * u);
            }
        }
    }

    Channel channels[2];
    Table tables[4] {};
    double invSampleRate = 1.0 / 48000.0;
    double phase = 0.0, lastPhase = 0.0, gate = 0.0, frozen = std::numeric_limits<double>::quiet_NaN();
    double glideOffset = 0.0;
    int glideLeft = 0, glideSamples = 480;
    bool gatePrimed = false, phasePrimed = false;

    double p90Norm = 1.0, stoneNorm = 1.0, gateCoeff = 1.0;
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
    impl->requestedKey = -1;
    impl->switchSamplesLeft = 0;
    impl->pedalWasReset = false;
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

    // A non-finite input sample would live on in the filter states (for good, in Classic's
    // juce::dsp::Phaser): it is replaced by silence before anything sees it. Finite input is
    // untouched, so Classic stays bit-identical.
    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
    {
        auto* d = block.getChannelPointer (ch);

        for (size_t i = 0; i < numSamples; ++i)
            if (! std::isfinite (d[i]))
                d[i] = 0.0f;
    }

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
