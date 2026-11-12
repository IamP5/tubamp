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

// Song position (voicings.md, "Engine behaviour outside the prototype").
constexpr double kJumpCycles = 0.01;           // a host phase step at least this large is a jump [choice: voicings.md]
constexpr double kJumpLfoSeconds = 0.002;      // ... and at least this much LFO time [choice: voicings.md]
constexpr double kLockSeconds = 0.15;          // time constant of the phase lock's proportional term [choice: voicings.md]
constexpr double kLockMaxNudge = 0.03;         // the proportional term bends the rate by at most this fraction [choice: voicings.md]
constexpr double kLockIntegralSeconds = 0.3;   // integral term: gain 1 / (kLockSeconds x this), damping 0.71 [choice: voicings.md]
constexpr double kLockMaxDrift = 0.10;         // the integral term follows a rate error of at most this fraction [choice: voicings.md]
constexpr double kJumpDriftRate = 0.15;        // a step a rate error this large could make since the last position is no jump [choice: voicings.md]
constexpr double kLockDeadCycles = 0.002;      // errors below this much of a cycle ... [choice: voicings.md]
constexpr double kLockDeadLfoSeconds = 0.0005; // ... or this much LFO time, whichever is less, are host jitter [choice: voicings.md]
// A jump glides the sweep over this many seconds: script, block, Small Stone off, on [choice: voicings.md].
constexpr double kGlideSeconds[4] { 0.010, 0.010, 0.010, 0.040 };

// Model and Color switches: the incoming voicing runs unheard from reset, waits for a moment the two
// will not cancel through, then a crossfade.
constexpr double kSwitchWarmSeconds = 0.030;   // [choice: voicings.md]
constexpr double kSwitchFadeSeconds = 0.040;   // [choice: voicings.md]
constexpr double kSwitchTurnSeconds = 0.080;   // ... and where it starts through the bridge: a held note turns by up to half a cycle [choice: voicings.md]
constexpr double kSwitchStatSeconds = 0.0025;  // each of the two one-poles of the power estimates [choice: voicings.md]
constexpr double kSwitchMinCorrelation = -0.5; // the fade starts where the two correlate at least this well [choice: FxBlocks.h PowerCrossfade]
constexpr double kSwitchWaitSeconds = 0.25;    // after the warm-up, waits this long for a moment that will stay good [choice: voicings.md]
constexpr double kSwitchLongestWaitSeconds = 0.28; // ... then for one good now, until this: request to fully new <= 0.39 s [choice: voicings.md]
// The two sweeps turn the outputs' phase difference on a held note at about kSwitchSweepTurn per Hz
// of LFO rate (the 90th percentile over voicing pairs, notes and moments; up to 18 degrees per ms just
// after the warm-up at 8 Hz), so a 40 ms fade that starts in phase can end in antiphase. Where the
// difference takes less than kSwitchFadeSeconds to turn by kSwitchHeldSpan at that rate (above 1.5 Hz),
// any fade lasts only that long, and no shorter than kSwitchShortestFadeSeconds; and on a held note
// (A B* this coherent, kSwitchHeldCoherence, once its shrinking by the smoothing of a turning phase
// is undone) the fade starts where the difference, extrapolated from its measured turn past the
// estimates' delay, stays within kSwitchHeldPhase of zero over the middle half of the fade
// [measured: docs/FX-ENGINES.md D-57].
constexpr double kSwitchSweepTurn = kPi / 180.0 * 1000.0;    // radians per second per Hz: 1 degree per ms per Hz
constexpr double kSwitchHeldSpan = kPi / 3.0;
constexpr double kSwitchShortestFadeSeconds = 0.005;
constexpr double kSwitchHeldCoherence = 0.75;
constexpr double kSwitchHeldPhase = kPi / 3.0;
constexpr double kSwitchEstimateDelaySeconds = 2.0 * 0.0025; // the two one-poles of kSwitchStatSeconds
constexpr double kSwitchMaxGain = 1.18;       // bound of the power correction in each band (+1.4 dB) [choice: voicings.md]
constexpr double kSwitchSplitHz = 1000.0;      // the two bands it corrects apart meet here [choice: FxBlocks.cpp kSplitHz]
constexpr double kSwitchPowerFloor = 1.0e-12;  // [choice: FxBlocks.cpp kPowerFloor]
// Where a band's two outputs correlate worse than kSwitchBridgeFrom, a bridge between them joins the
// fade, in full from kSwitchBridgeFull on [choice: voicings.md].
constexpr double kSwitchBridgeFrom = -0.2, kSwitchBridgeFull = -0.7;
// ... where both outputs are this coherent with the input, from none to all [choice: voicings.md].
constexpr double kSwitchBridgeCoherence[2] { 0.6, 0.9 };
constexpr double kSwitchTurnSmoothSeconds = 0.005; // each of the two one-poles through which the bridge follows its need [choice: voicings.md]
// The bridge's quadrature pair: coefficients a^2 of two chains of four allpasses in z^2, 90 +- 1
// degrees apart from 20 Hz to 20 kHz at 44.1 kHz [Niemitalo, "Hilbert transform" IIR pair].
constexpr double kQuadratureA[4] { 0.6923878 * 0.6923878, 0.9360654322959 * 0.9360654322959,
                                   0.9882295226860 * 0.9882295226860, 0.9987488452737 * 0.9987488452737 };
constexpr double kQuadratureB[4] { 0.4021921162426 * 0.4021921162426, 0.8561710882420 * 0.8561710882420,
                                   0.9722909545651 * 0.9722909545651, 0.9952884791278 * 0.9952884791278 };

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
constexpr double kP90ScriptMakeup = 1.3152, kP90BlockMakeup = 1.3137; // +2.38 / +2.37 dB [choice: voicings.md, Level]

// Small Stone (SS_*).
constexpr double kSsKappa = 0.702;              // LFO curvature [fit]

struct StoneVoicing
{
    double loHz, hiHz, curve, feedback, dry, wet, makeup;
};

constexpr StoneVoicing kStoneOff { 326.4, 9086.3, 0.0128, 0.0, 0.5003, 0.7500, 1.0186 }; // [fit], make-up +0.16 dB [choice]
constexpr StoneVoicing kStoneOn { 98.5, 3105.4, 0.0, 0.7548, 0.1318, 0.4395, 0.9237 };    // [fit], make-up -0.69 dB [choice]
constexpr double kStoneOffHighpassHz = 44.2;    // Color off: the wet input's first-order highpass [fit]

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

/** x wrapped to [-0.5, 0.5). */
inline double wrapHalf (double x) noexcept
{
    return x - std::floor (x + 0.5);
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

/** Exponential in w, plus a curvature term that leaves the ends at loHz and hiHz. */
double stoneBreakHz (const StoneVoicing& v, double w) noexcept
{
    return std::sqrt (v.loHz * v.hiHz) * std::exp (0.5 * std::log (v.hiHz / v.loHz) * w + v.curve * (w * w - 1.0));
}

enum class Voicing { script = 0, block = 1, stoneOff = 2, stoneOn = 3 };

/** PedalPhaser::process: free running, synced with the host's phase at the run's first sample,
    or synced with nothing new (a run that does not start the host block). */
enum class Sync { off, update, hold };

/** What every PedalPhaser of one PhaserFx shares: the coefficient tables built in prepare(). */
struct PedalTables
{
    using Table = std::array<std::array<double, 5>, kTableN + 1>;

    void prepare (double sampleRate)
    {
        invSampleRate = 1.0 / sampleRate;
        p90Norm = 1.0 / (1.0 - std::exp (-kP90Kappa));
        stoneNorm = 1.0 / (1.0 - std::exp (-kSsKappa));
        gateCoeff = 1.0 - std::exp (-2.0 * kPi * kP90GateHz / sampleRate);

        for (size_t v = 0; v < 4; ++v)
            glideSamples[v] = std::max (1, (int) std::lround (kGlideSeconds[v] * sampleRate));

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
        highpassG = tpt (kStoneOffHighpassHz);
        inShelfA = 1.0 - kStoneInZeroAHz / kStonePole1Hz;
        inShelfB = 1.0 - kStoneInZeroBHz / kStonePole2Hz;
        fbShelf = 1.0 - kStoneFbZeroHz / kStonePole2Hz;
    }

    Table tables[4] {};
    double invSampleRate = 1.0 / 48000.0;
    double p90Norm = 1.0, stoneNorm = 1.0, gateCoeff = 1.0;
    double poleG1 = 0.0, poleG2 = 0.0, highpassG = 0.0, inShelfA = 0.0, inShelfB = 0.0, fbShelf = 0.0;
    int glideSamples[4] { 1, 1, 1, 1 };
};

//==============================================================================
/** Phase 90 and Small Stone: four TPT one-pole allpass stages whose coefficients follow the
    sweep position through the shared tables; feedback loops are solved per sample in closed
    form, without a unit delay. */
class PedalPhaser
{
public:
    void prepare (const PedalTables& sharedTables, double sampleRate)
    {
        t = &sharedTables;
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
        phasePrimed = synced = false;
        nudge = drift = 0.0;
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
    int getJumpCount() const noexcept { return jumps; }

    /** hostPhase: the host's LFO phase at the first sample (Sync::update); hostRate: the rate it
        advances at (the clamped rate parameter). */
    void process (juce::dsp::AudioBlock<float> block, Voicing voicing, Sync sync, double hostPhase, double hostRate) noexcept
    {
        const int numSamples = (int) block.getNumSamples();

        if (sync == Sync::update)
        {
            follow (hostPhase, hostRate, voicing);
        }
        else if (sync == Sync::off)
        {
            synced = false;
            nudge = drift = 0.0;
        }

        phasePrimed = true;

        const int numChannels = juce::jmin (2, (int) block.getNumChannels());
        float* data[2] { block.getChannelPointer (0), numChannels > 1 ? block.getChannelPointer (1) : nullptr };

        switch (voicing)
        {
            case Voicing::script:   runPhase90<false> (data, numChannels, numSamples); break;
            case Voicing::block:    runPhase90<true> (data, numChannels, numSamples); break;
            case Voicing::stoneOff: runStone<false> (data, numChannels, numSamples); break;
            case Voicing::stoneOn:  runStone<true> (data, numChannels, numSamples); break;
        }

        samplesSinceHost += numSamples;

        for (auto& c : channels)
        {
            for (auto& s : c.stage)
                settle (s);

            settle (c.lag);
            settle (c.highpass);
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
        double highpass = 0.0;         // Small Stone Color off: the wet input's highpass
        double inA = 0.0, inB = 0.0;   // Small Stone Color: input branch
        double fbA = 0.0, fbB = 0.0;   // Small Stone Color: feedback branch
    };

    /** A step in the host's own phase (transport start, locate, a loop that is not a whole number
        of cycles, a note-value change, sync switched on) is one jump, honoured with one glide. Any
        smaller difference, the engine's own drift, is pulled in by bending the rate; a difference
        within a few samples' worth (host jitter) is left alone. The lock is second order: a phase
        term, and an integral that learns a rate error the host does not report (a tempo parameter
        that lags the host's own), so a constant one leaves no error standing beyond the dead band,
        however large it is (within kLockMaxDrift). */
    void follow (double host, double hostRate, Voicing voicing) noexcept
    {
        if (! phasePrimed)
        {
            phase = host;
        }
        else if (! synced)
        {
            jump (host, hostRate, voicing);
        }
        else
        {
            const double since = (double) samplesSinceHost * t->invSampleRate;
            const double predicted = hostLast + hostRateLast * (1.0 + drift) * since;
            const double step = wrapHalf (host - predicted);

            if (std::abs (step) >= std::max ({ kJumpCycles, hostRate * kJumpLfoSeconds, kJumpDriftRate * hostRate * since }))
            {
                jump (host, hostRate, voicing);
            }
            else
            {
                // The dead band holds for both terms: host jitter then moves neither the LFO nor the
                // rate it has learnt, and a constant rate error is left at the band's edge.
                const double error = wrapHalf (host - phase);
                const double dead = std::min (kLockDeadCycles, hostRate * kLockDeadLfoSeconds);
                const double pull = std::max (0.0, std::abs (error) - dead) * (error < 0.0 ? -1.0 : 1.0);
                const double limit = kLockMaxNudge * hostRate;
                drift = juce::jlimit (-kLockMaxDrift, kLockMaxDrift,
                                      drift + pull * since / (kLockSeconds * kLockIntegralSeconds * hostRate));
                nudge = juce::jlimit (-limit, limit, pull / kLockSeconds) + drift * hostRate;
            }
        }

        synced = true;
        hostLast = host;
        hostRateLast = hostRate;
        samplesSinceHost = 0;
    }

    /** The phase is set to the host's; the sweep glides from where it is (a glide still running
        included) to the new phase's position with a raised cosine of fixed length. A rate error the
        lock has learnt stays: a jump does not change the host's tempo. */
    void jump (double host, double hostRate, Voicing voicing) noexcept
    {
        const bool phase90 = voicing == Voicing::script || voicing == Voicing::block;
        const double kappa = phase90 ? kP90Kappa : kSsKappa, norm = phase90 ? t->p90Norm : t->stoneNorm;
        const double from = rcTriangle (phase, kappa, norm) + pendingGlide();

        phase = host;
        glideOffset = from - rcTriangle (host, kappa, norm);
        glideSamples = t->glideSamples[(size_t) voicing];
        glideLeft = glideSamples;
        nudge = drift * hostRate;
        ++jumps;
    }

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
        double sweep = phase90 ? rcTriangle (phase, kP90Kappa, t->p90Norm) : rcTriangle (phase, kSsKappa, t->stoneNorm);
        phase += ((double) rate.next() + nudge) * t->invSampleRate;

        if (phase >= 1.0)
            phase -= 1.0;
        else if (phase < 0.0)
            phase += 1.0;

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

            gate += t->gateCoeff * (sweep - gate);
            sweep = gate;
        }

        return sweep;
    }

    /** Linear interpolation of the voicing's table at sweep position w (prototype lookup). */
    static void lookup (const PedalTables::Table& table, double w, double* G, int count) noexcept
    {
        double tt = (w + 1.0) * (0.5 * kTableN);
        tt = tt >= 0.0 ? std::min (tt, (double) kTableN) : 0.0; // false for NaN as well
        const int i = std::min ((int) tt, kTableN - 1);
        const double fr = tt - i;

        for (int k = 0; k < count; ++k)
            G[k] = table[(size_t) i][(size_t) k] + fr * (table[(size_t) i + 1][(size_t) k] - table[(size_t) i][(size_t) k]);
    }

    template <bool block>
    void runPhase90 (float* const* data, int numChannels, int numSamples) noexcept
    {
        constexpr double beta = kP90Beta;
        const auto& table = t->tables[block ? 1 : 0];
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
        const auto& table = t->tables[color ? 3 : 2];
        const double fb = voicing.feedback, dry = voicing.makeup * voicing.dry, wet = voicing.makeup * voicing.wet;
        const double g1 = t->poleG1, g2 = t->poleG2, gh = t->highpassG;
        const double inShelfA = t->inShelfA, inShelfB = t->inShelfB, fbShelf = t->fbShelf;
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
                double u;

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
                else
                {
                    const double v = (x - c.highpass) * gh;
                    const double lp = v + c.highpass;
                    c.highpass = lp + v;
                    u = x - lp;
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

    const PedalTables* t = nullptr;
    Channel channels[2];
    double phase = 0.0, lastPhase = 0.0, gate = 0.0, frozen = std::numeric_limits<double>::quiet_NaN();
    double glideOffset = 0.0, nudge = 0.0, drift = 0.0; // drift: the lock's integral, a fraction of the rate
    double hostLast = 0.0, hostRateLast = 0.0;
    int glideLeft = 0, glideSamples = 1, samplesSinceHost = 0, jumps = 0;
    bool gatePrimed = false, phasePrimed = false, synced = false;

    fx::Smoother rate;
};

/** 0 for Classic (it ignores color), else one key per voicing. */
int engineKey (const PhaserParams& p) noexcept
{
    return p.model == (int) PhaserFx::Model::classic ? 0 : p.model * 2 + (p.color ? 1 : 0);
}

/** Two outputs of one input 90 degrees apart (kQuadratureA one sample late against kQuadratureB).
    Each section is a first-order allpass in z^2, in transposed form: one state per sample parity. */
class QuadraturePair
{
public:
    void reset() noexcept
    {
        for (auto* st : { &stateA, &stateB })
            for (auto& section : *st)
                section[0] = section[1] = 0.0;

        late = 0.0;
        parity = 0;
    }

    void process (double x, double& i, double& q) noexcept
    {
        double a = x, b = x;

        for (size_t k = 0; k < 4; ++k)
        {
            double& sa = stateA[k][parity];
            const double ya = kQuadratureA[k] * a + sa;
            sa = kQuadratureA[k] * ya - a;
            a = ya;

            double& sb = stateB[k][parity];
            const double yb = kQuadratureB[k] * b + sb;
            sb = kQuadratureB[k] * yb - b;
            b = yb;
        }

        parity ^= 1;
        i = late;
        late = a;
        q = b;
    }

private:
    std::array<std::array<double, 2>, 4> stateA {}, stateB {};
    double late = 0.0;
    size_t parity = 0;
};

Voicing voicingFor (int key) noexcept
{
    switch (key)
    {
        case 2:  return Voicing::script;
        case 3:  return Voicing::block;
        case 4:  return Voicing::stoneOff;
        default: return Voicing::stoneOn;
    }
}
} // namespace

//==============================================================================
struct PhaserFx::Impl
{
    /** Runs `block`, which starts `offset` samples into the host block, on the voicing `key`
        (pedal instance `pedal` for the Phase 90 and Small Stone keys). */
    void runKey (int key, int pedal, juce::dsp::AudioBlock<float> block, size_t offset)
    {
        if (key == 0)
        {
            classic.process (block);
            return;
        }

        const double hostRate = (double) juce::jlimit (0.01f, 20.0f, params.rateHz);
        const Sync sync = params.syncPhase < 0.0 ? Sync::off : offset == 0 ? Sync::update : Sync::hold;
        pedals[pedal].process (block, voicingFor (key), sync, params.syncPhase, hostRate);
    }

    void startSwitch (int key)
    {
        incomingKey = key;
        incomingPedal = activeKey == 0 ? activePedal : 1 - activePedal;

        if (key == 0)
            classic.reset();
        else
            pedals[incomingPedal].reset();

        switchPos = waited = fadePos = 0;
        std::fill (&power[0][0], &power[0][0] + 8, 0.0);

        for (auto& band : bands)
            band = {};

        for (auto& low : lows)
            std::fill (std::begin (low), std::end (low), 0.0);

        for (auto& pairs : quadrature)
            for (auto& q : pairs)
                q.reset();

        correlationLag = 1.0;
        turnRate = lastCross[0] = lastCross[1] = 0.0;
    }

    // Short-term figures of one band, from analytic signals (a quadrature pair's outputs I, Q make
    // I - jQ, as Q leads I by 90 degrees) of the outgoing output A, the incoming one B and the input
    // pair's in-phase output X: |A|^2, |B|^2, Re A B*, A X*, B X*, |X|^2. A held note gives them no
    // ripple at twice its frequency, which products of the real signals have and which, through the
    // gains, would put sidebands beside the note.
    enum Stat { sAA, sBB, sAB, sAXr, sAXi, sBXr, sBXi, sXX, numStats };

    struct SwitchBand
    {
        double stat[2][numStats] {}; // two smoothing stages
        double side = 1.0;           // the bridge's side of the two
        double bridge = 0.0;         // its gain at the last sample
        double turn[2] {};           // how far the fade goes through it: two smoothing stages
    };

    /** The bridge of one band: cI I + cQ Q of the input's quadrature pair, of unit power, at the
        phase midway between the outgoing and the incoming outputs, so that it correlates equally
        (in antiphase: not at all) with both; crossA, crossB are its cross terms with the two; fit,
        in [0, 1], how well each output is one complex gain of the input (a held note, where a
        bridge can stand in for the note the two cancel on; not a chord through two phasers, whose
        partials turn apart). False where the input or either output carries nothing. */
    static bool bridgeOf (SwitchBand& band, double& cI, double& cQ, double& crossA, double& crossB, double& fit) noexcept
    {
        const double* st = band.stat[1];
        const double na = std::hypot (st[sAXr], st[sAXi]), nb = std::hypot (st[sBXr], st[sBXi]);

        if (st[sXX] < kSwitchPowerFloor || na * na < kSwitchPowerFloor * st[sXX] || nb * nb < kSwitchPowerFloor * st[sXX])
            return false;

        // The bisector w of the two directions is parallel to p, the normal of their difference,
        // which keeps a direction through antiphase, where w vanishes. The side of p is w's until
        // the bridge is heard; from then on it is kept, as the two may pass through antiphase and
        // the bridge must not jump to the other side.
        const double ea[2] { st[sAXr] / na, st[sAXi] / na }, eb[2] { st[sBXr] / nb, st[sBXi] / nb };
        const double w[2] { ea[0] + eb[0], ea[1] + eb[1] };
        const double pn[2] { eb[1] - ea[1], ea[0] - eb[0] };
        const double along = pn[0] * w[0] + pn[1] * w[1], np = std::hypot (pn[0], pn[1]);

        if (np < 1.0e-6)
            return false;

        // fit also falls to nothing as the two come to one phase, (|p| / 2)^2: there the bridge has
        // nothing to bridge, and p, reversing, would flip it to the other side.
        const double coherence = std::min (na * na / (st[sAA] * st[sXX]), nb * nb / (st[sBB] * st[sXX]));
        const double over = juce::jlimit (0.0, 1.0, (coherence - kSwitchBridgeCoherence[0]) / (kSwitchBridgeCoherence[1] - kSwitchBridgeCoherence[0]));
        fit = over * over * (3.0 - 2.0 * over) * 0.25 * np * np;

        if (band.bridge == 0.0 && std::abs (along) > 0.1)
            band.side = along > 0.0 ? 1.0 : -1.0;

        // The direction d is relative to I, and Q leads I by a quarter cycle: d0 I + d1 Q.
        const double norm = 1.0 / std::sqrt (st[sXX]);
        const double d[2] { band.side * pn[0] / np, band.side * pn[1] / np };
        cI = d[0] * norm;
        cQ = d[1] * norm;
        crossA = (st[sAXr] * d[0] + st[sAXi] * d[1]) * norm;
        crossB = (st[sBXr] * d[0] + st[sBXi] * d[1]) * norm;
        return true;
    }

    /** True where the fade about to start goes mostly through the bridge in the band that carries
        more of the two outputs: a held note the two cancel on. */
    bool turnsAtStart() noexcept
    {
        const auto& loud = bands[bands[1].stat[1][sAA] + bands[1].stat[1][sBB] > bands[0].stat[1][sAA] + bands[0].stat[1][sBB] ? 1 : 0];
        const double* st = loud.stat[1];
        const double pair = std::sqrt (st[sAA] * st[sBB]);
        const double rho = pair < kSwitchPowerFloor ? 1.0 : st[sAB] / pair;
        auto probe = loud;
        double cI, cQ, crossA, crossB, fit = 0.0;
        return rho < 0.5 * (kSwitchBridgeFrom + kSwitchBridgeFull) && bridgeOf (probe, cI, cQ, crossA, crossB, fit) && fit > 0.5;
    }

    /** A switch in flight: the outgoing voicing on one copy of the input, the incoming one on
        another. The incoming one is not heard for its first kSwitchWarmSeconds. Two phasers can sit
        near antiphase on one held note, where any crossfade of the two passes a null, so the fade
        waits (as FxBlocks.h PowerCrossfade does) for a moment where they correlate no worse than
        kSwitchMinCorrelation and, by the correlation's trend, will not fall below it before the fade
        is over; after kSwitchWaitSeconds the first moment that is good enough now, after
        kSwitchLongestWaitSeconds any. At a fast LFO the fade is shorter, and on a held note it waits
        instead for the moment the two will be in phase through the fade's middle (kSwitchSweepTurn).
        The fade runs on a cornerless ramp, in two bands: in a band
        whose two outputs still oppose each other, the bridge (bridgeOf) comes in and goes out with
        the ramp's arch, so the band passes from one to the other through the bridge instead of
        through the null; and each band is scaled so that the power of its sum, predicted from
        short-term powers and cross terms, follows the straight line in dB between the two outputs',
        the scale softly bounded at kSwitchMaxGain, where a large gain would itself be heard. */
    void processSwitch (juce::dsp::AudioBlock<float> block)
    {
        const int numChannels = juce::jmin (2, (int) block.getNumChannels());
        const auto total = block.getNumSamples();
        const auto capacity = (size_t) scratch.getNumSamples();
        const double slackRatio = 1.0 / (kSwitchMaxGain * kSwitchMaxGain - 1.0);
        const double sweepTurn = kSwitchSweepTurn / sampleRate * (double) juce::jlimit (0.01f, 20.0f, params.rateHz);
        const int rateLength = (int) juce::jlimit ((double) shortestFadeSamples, (double) turnSamples, kSwitchHeldSpan / sweepTurn);
        // Below about 1.5 Hz the fades keep their lengths and the start keeps the correlation rule:
        // there it already holds (D-57), and the phase rule waits too long for slow sweeps.
        const bool fast = rateLength < fadeSamples;
        const int plainLength = std::min (rateLength, fadeSamples), turnLength = fast ? rateLength : turnSamples;

        for (size_t start = 0; start < total; start += capacity)
        {
            const auto n = juce::jmin (capacity, total - start);
            auto sub = block.getSubBlock (start, n);

            if (incomingKey < 0)
            {
                runKey (activeKey, activePedal, sub, start);
                continue;
            }

            float* a[2] {}, *b[2] {}, *out[2] {};

            for (int ch = 0; ch < numChannels; ++ch)
            {
                a[ch] = scratch.getWritePointer (ch);
                b[ch] = scratch.getWritePointer (2 + ch);
                out[ch] = sub.getChannelPointer ((size_t) ch);
                juce::FloatVectorOperations::copy (a[ch], out[ch], (int) n);
                juce::FloatVectorOperations::copy (b[ch], out[ch], (int) n);
            }

            runKey (activeKey, activePedal, juce::dsp::AudioBlock<float> (a, (size_t) numChannels, n), start);
            runKey (incomingKey, incomingPedal, juce::dsp::AudioBlock<float> (b, (size_t) numChannels, n), start);

            for (size_t i = 0; i < n; ++i)
            {
                // Per band (low, high) and channel: A and B as heard; the pair outputs of A, of B and
                // of the input (the bridge's I and Q); and the pair outputs of that I (its analytic
                // signal, against which A and B are measured). out[ch][i] still holds the input.
                double v[2][10][2] {}, raw[4] {}, sums[2][numStats] {};

                // The pairs start from rest on signals already running: their inputs come in over
                // the warm-up, as a step would ring in their slow poles at DC and at Nyquist, and the
                // bridge carries the input pair's outputs to the output.
                const int pos = switchPos + (int) i;
                const double onset = pos < warmSamples ? 0.5 - 0.5 * std::cos (kPi * pos / warmSamples) : 1.0;

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    double whole[10] { a[ch][i], b[ch][i] };
                    quadrature[ch][0].process (onset * whole[0], whole[2], whole[3]);
                    quadrature[ch][1].process (whole[1], whole[4], whole[5]);
                    quadrature[ch][2].process (onset * out[ch][i], whole[6], whole[7]);
                    quadrature[ch][3].process (whole[6], whole[8], whole[9]);
                    raw[0] += whole[2] * whole[2] + whole[3] * whole[3];
                    raw[1] += whole[4] * whole[4] + whole[5] * whole[5];
                    raw[2] += whole[2] * whole[4] + whole[3] * whole[5];
                    raw[3] += whole[3] * whole[4] - whole[2] * whole[5];

                    // Complementary bands, low = one-pole low-pass and high = the rest.
                    for (int k = 0; k < 10; ++k)
                    {
                        lows[k][ch] += splitCoeff * (whole[k] - lows[k][ch]);
                        v[0][k][ch] = lows[k][ch];
                        v[1][k][ch] = whole[k] - lows[k][ch];
                    }

                    for (int m = 0; m < 2; ++m)
                    {
                        const double aI = v[m][2][ch], aQ = v[m][3][ch], bI = v[m][4][ch], bQ = v[m][5][ch];
                        const double xI = v[m][8][ch], xQ = v[m][9][ch];
                        const double terms[numStats] { aI * aI + aQ * aQ, bI * bI + bQ * bQ, aI * bI + aQ * bQ,
                                                       aI * xI + aQ * xQ, aI * xQ - aQ * xI,
                                                       bI * xI + bQ * xQ, bI * xQ - bQ * xI, xI * xI + xQ * xQ };

                        for (int k = 0; k < numStats; ++k)
                            sums[m][k] += terms[k];
                    }
                }

                for (int k = 0; k < 4; ++k)
                {
                    power[0][k] += powerCoeff * (raw[k] - power[0][k]);
                    power[1][k] += powerCoeff * (power[0][k] - power[1][k]);
                }

                for (int m = 0; m < 2; ++m)
                {
                    auto& st = bands[m].stat;

                    for (int k = 0; k < numStats; ++k)
                    {
                        st[0][k] += powerCoeff * (sums[m][k] - st[0][k]);
                        st[1][k] += powerCoeff * (st[0][k] - st[1][k]);
                    }
                }

                const double both = std::sqrt (power[1][0] * power[1][1]);
                const double correlation = both < kSwitchPowerFloor ? 1.0 : power[1][2] / both;

                // The trend: the difference from its own copy lagged by one more one-pole is the
                // slope times that time constant.
                correlationLag += powerCoeff * (correlation - correlationLag);

                // A held note: the outputs are one complex gain of each other, whose angle and its rate
                // of turn (smoothed as the estimates are) predict when the two will be in phase.
                const double step = std::atan2 (power[1][3] * lastCross[0] - power[1][2] * lastCross[1],
                                                power[1][2] * lastCross[0] + power[1][3] * lastCross[1]);
                turnRate += powerCoeff * (step - turnRate);
                lastCross[0] = power[1][2];
                lastCross[1] = power[1][3];
                // The two one-poles shrink A B* turning at w by 1 + (w tau)^2: undone, up to twice.
                const double turnTau = turnRate / powerCoeff;
                const double cross = std::hypot (power[1][2], power[1][3]) * std::min (2.0, 1.0 + turnTau * turnTau);
                const bool held = both >= kSwitchPowerFloor && cross >= kSwitchHeldCoherence * both;

                if (fadePos == 0)
                {
                    bool starts = pos >= warmSamples;

                    if (starts)
                    {
                        const bool goodNow = correlation >= kSwitchMinCorrelation;

                        if (held && fast)
                        {
                            const double phase = std::atan2 (power[1][3], power[1][2]);
                            const auto within = [&] (double ahead)
                            {
                                return std::abs (std::remainder (phase + turnRate * (delaySamples + ahead * plainLength), 2.0 * kPi)) <= kSwitchHeldPhase;
                            };
                            heldInPhase = within (0.25) && within (0.75);
                            starts = heldInPhase || (goodNow && waited >= waitSamples) || waited >= longestWaitSamples;
                        }
                        else
                        {
                            const double ahead = correlation + (correlation - correlationLag) * powerCoeff * fadeSamples;
                            const bool heading = correlation >= correlationLag || ahead >= 0.0;
                            heldInPhase = false;
                            starts = (goodNow && (heading || waited >= waitSamples)) || waited >= longestWaitSamples;
                        }

                        if (! starts)
                            ++waited;
                    }

                    if (! starts)
                    {
                        for (int ch = 0; ch < numChannels; ++ch)
                            out[ch][i] = a[ch][i];

                        continue;
                    }

                    fadeLength = ! heldInPhase && turnsAtStart() ? turnLength : plainLength;
                }

                if (++fadePos >= fadeLength)
                {
                    for (int ch = 0; ch < numChannels; ++ch)
                        out[ch][i] = b[ch][i];

                    continue;
                }

                const double ramp = fadePos / (double) fadeLength;
                // Smoother-step (corners down to the second derivative): a 40 ms exchange of two
                // different outputs on a held note stays 53 dB under the note from 80 Hz off it on,
                // where the plain smooth-step reaches -44 dB.
                const double r = ramp * ramp * ramp * (ramp * (6.0 * ramp - 15.0) + 10.0);
                // Gains halfway between equal amplitude and equal power (-4.5 dB at the middle): where
                // the power estimates lag (a pick attack inside the fade), two correlated outputs rise
                // 1.5 dB and two uncorrelated ones fall 1.5 dB, instead of 3 dB either way.
                const double gOutPlain = std::sqrt ((1.0 - r) * std::cos (0.5 * kPi * r));
                const double gInPlain = std::sqrt (r * std::sin (0.5 * kPi * r));
                // Through the bridge, a turn by theta: with B = -A, (1 + cos)/2 A + (1 - cos)/2 B +
                // sin R is cos A + sin R, of constant power, from A to B.
                const double theta = kPi * r;
                const double gOutTurn = 0.5 + 0.5 * std::cos (theta), gInTurn = 0.5 - 0.5 * std::cos (theta);
                const double arch = std::sin (theta);
                double g[2][4] {};

                // Each band on its own figures: two voicings' lows can cancel while their highs (a
                // pick's attack) add, and one broadband gain would lift the attack by the lows'
                // correction.
                for (int m = 0; m < 2; ++m)
                {
                    const double* st = bands[m].stat[1];
                    const double pOut = st[sAA], pIn = st[sBB], pCross = st[sAB];
                    const double want = (pOut + kSwitchPowerFloor) * std::exp (r * std::log ((pIn + kSwitchPowerFloor) / (pOut + kSwitchPowerFloor)));
                    const double pair = std::sqrt (pOut * pIn);
                    const double rho = pair < kSwitchPowerFloor ? 1.0 : pCross / pair;
                    // Without corners: a kink here is a kink in the band's gains, which a held note
                    // turns into sidebands.
                    const double over = juce::jlimit (0.0, 1.0, (kSwitchBridgeFrom - rho) / (kSwitchBridgeFrom - kSwitchBridgeFull));
                    const double need = over * over * (3.0 - 2.0 * over);
                    double cI = 0.0, cQ = 0.0, crossA = 0.0, crossB = 0.0, fit = 0.0;
                    auto& turns = bands[m].turn;
                    const bool bridged = (need > 0.0 || turns[1] > 1.0e-9) && bridgeOf (bands[m], cI, cQ, crossA, crossB, fit);

                    // The bridge and the gains' law follow the need no faster than two one-poles: the
                    // correlation of two phasers near resonance can swing in a few ms, and so fast a
                    // change of either is heard beside a held note. The fit is not smoothed: where it
                    // falls, the bridge's direction is no longer to be trusted.
                    turns[0] += turnCoeff * ((bridged ? need : 0.0) - turns[0]);
                    turns[1] += turnCoeff * (turns[0] - turns[1]);
                    const double turn = turns[1] * fit;
                    const double gBridge = bridged ? turn * arch * std::sqrt (want) : 0.0;

                    bands[m].bridge = gBridge;
                    const double gOut = gOutPlain + turn * (gOutTurn - gOutPlain);
                    const double gIn = gInPlain + turn * (gInTurn - gInPlain);
                    const double got = gOut * gOut * pOut + gIn * gIn * pIn + 2.0 * gOut * gIn * pCross
                                       + gBridge * (gBridge + 2.0 * (gOut * crossA + gIn * crossB));
                    const double slack = slackRatio * want + 1.0e-30;
                    const double scale = std::sqrt ((want + slack) / (std::max (0.0, got) + slack));
                    g[m][0] = scale * gOut;
                    g[m][1] = scale * gIn;
                    g[m][2] = scale * gBridge * cI;
                    g[m][3] = scale * gBridge * cQ;
                }

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    double y = 0.0;

                    for (int m = 0; m < 2; ++m)
                        y += g[m][0] * v[m][0][ch] + g[m][1] * v[m][1][ch] + g[m][2] * v[m][6][ch] + g[m][3] * v[m][7][ch];

                    out[ch][i] = (float) y;
                }
            }

            switchPos += (int) n;

            if (fadePos >= fadeLength)
            {
                activeKey = incomingKey;
                activePedal = incomingPedal;
                incomingKey = -1;
            }
        }
    }

    PedalTables tables;
    ClassicPhaser classic;
    PedalPhaser pedals[2];
    juce::AudioBuffer<float> scratch;        // outgoing L/R, incoming L/R
    PhaserParams params;
    double sampleRate = 48000.0, powerCoeff = 1.0, splitCoeff = 1.0, turnCoeff = 1.0, correlationLag = 1.0;
    double power[2][4] {};                   // two smoothing stages of: |A|^2, |B|^2, Re and Im of A B*
    double turnRate = 0.0, lastCross[2] {};  // the turn of A B* in radians per sample; A B* a sample ago
    SwitchBand bands[2];                     // low, high
    double lows[10][2] {};                   // the band split's low-passes (see processSwitch) per channel
    QuadraturePair quadrature[2][4];         // per channel: of A, of B, of the input, of the input's I
    int activeKey = 0, activePedal = 0;      // the voicing heard and its pedal instance
    int incomingKey = -1, incomingPedal = 1; // a switch is in flight while incomingKey >= 0
    int switchPos = 0, warmSamples = 1, fadeSamples = 1, turnSamples = 1, fadeLength = 1, waitSamples = 0, longestWaitSamples = 0;
    int waited = 0, fadePos = 0;             // samples waited after the warm-up; samples into the fade
    int shortestFadeSamples = 1;
    double delaySamples = 0.0;
    bool heldInPhase = false;
    bool snapEngine = true, snapParams = true;
};

PhaserFx::PhaserFx() : impl (std::make_unique<Impl>()) {}
PhaserFx::~PhaserFx() = default;

void PhaserFx::prepare (const juce::dsp::ProcessSpec& spec)
{
    auto& s = *impl;
    s.sampleRate = spec.sampleRate;
    s.warmSamples = (int) std::lround (kSwitchWarmSeconds * spec.sampleRate);
    s.fadeSamples = std::max (1, (int) std::lround (kSwitchFadeSeconds * spec.sampleRate));
    s.turnSamples = std::max (1, (int) std::lround (kSwitchTurnSeconds * spec.sampleRate));
    s.shortestFadeSamples = std::max (1, (int) std::lround (kSwitchShortestFadeSeconds * spec.sampleRate));
    s.delaySamples = kSwitchEstimateDelaySeconds * spec.sampleRate;
    s.waitSamples = (int) std::lround (kSwitchWaitSeconds * spec.sampleRate);
    s.longestWaitSamples = (int) std::lround (kSwitchLongestWaitSeconds * spec.sampleRate);
    s.powerCoeff = 1.0 - std::exp (-1.0 / (kSwitchStatSeconds * spec.sampleRate));
    s.splitCoeff = 1.0 - std::exp (-2.0 * kPi * kSwitchSplitHz / spec.sampleRate);
    s.turnCoeff = 1.0 - std::exp (-1.0 / (kSwitchTurnSmoothSeconds * spec.sampleRate));
    s.scratch.setSize (4, juce::jmax (1, (int) spec.maximumBlockSize), false, true, false);
    s.classic.prepare (spec);
    s.tables.prepare (spec.sampleRate);

    for (auto& p : s.pedals)
        p.prepare (s.tables, spec.sampleRate);

    s.incomingKey = -1;
    s.snapEngine = s.snapParams = true;
}

void PhaserFx::reset()
{
    auto& s = *impl;
    s.classic.reset();

    for (auto& p : s.pedals)
        p.reset();

    s.incomingKey = -1;
    s.snapEngine = s.snapParams = true;
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

    for (auto& p : s.pedals)
        p.setRate (juce::jlimit (0.01f, 20.0f, s.params.rateHz), s.snapParams);

    s.snapParams = false;
}

void PhaserFx::process (juce::dsp::AudioBlock<float> block)
{
    auto& s = *impl;
    const auto numSamples = block.getNumSamples();

    if (numSamples == 0)
        return;

    // A sample no instrument can produce (non-finite, or beyond 1e6) would live on in the filter
    // states (for good, in Classic's juce::dsp::Phaser): it is replaced by silence before anything
    // sees it. Everything else is untouched, so Classic stays bit-identical.
    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
    {
        auto* d = block.getChannelPointer (ch);

        for (size_t i = 0; i < numSamples; ++i)
            d[i] = fx::sanitise (d[i]);
    }

    const int key = engineKey (s.params);

    if (s.snapEngine)
    {
        // Nothing has been heard since prepare() or reset(): start on the requested model.
        s.snapEngine = false;
        s.activeKey = key;
        s.activePedal = 0;
        s.incomingKey = -1;
    }

    if (s.incomingKey < 0 && key != s.activeKey)
    {
        s.startSwitch (key);
    }
    else if (s.incomingKey >= 0 && s.fadePos == 0 && key != s.incomingKey)
    {
        // Nothing of the incoming voicing has been heard yet: a request back is dropped, another
        // one takes its place.
        if (key == s.activeKey)
            s.incomingKey = -1;
        else
            s.startSwitch (key);
    }

    if (s.incomingKey < 0)
        s.runKey (s.activeKey, s.activePedal, block, 0);
    else
        s.processSwitch (block);
}

void PhaserFx::freezeSweep (double position) noexcept
{
    for (auto& p : impl->pedals)
        p.freeze (position);
}

double PhaserFx::getLfoPhase() const noexcept { return impl->pedals[impl->activePedal].getLastPhase(); }
int PhaserFx::getSyncJumpCount() const noexcept { return impl->pedals[0].getJumpCount() + impl->pedals[1].getJumpCount(); }
} // namespace tubamp
