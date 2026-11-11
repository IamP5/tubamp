#include "Drive.h"

#include "FxPrimitives.h"

#include <cmath>
#include <complex>

namespace tubamp
{
namespace
{
//==============================================================================
// Classic: src/dsp/FxBlocks.cpp (bf81728) lines 15-16, 32-39 and 45-123, moved verbatim
// (docs/FX-ENGINES.md §2.3). Do not touch a single expression here: --classic-null and
// the golden renders compare it bit for bit with the frozen reference.
//==============================================================================
constexpr float kSmoothingSeconds = 0.02f;
constexpr float kMinCutoffHz = 20.0f;

float clampCutoff (float hz, double sampleRate)
{
    const auto nyquistLimit = (float) (sampleRate * 0.49);
    return juce::jlimit (kMinCutoffHz, juce::jmax (kMinCutoffHz + 1.0f, nyquistLimit), hz);
}

class ClassicDrive
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;

        // 2x (2^1) polyphase IIR: cheapest option with negligible latency, which keeps the
        // reported plugin latency equal to the NAM resampler's.
        oversampling = std::make_unique<juce::dsp::Oversampling<float>> (
            spec.numChannels, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        oversampling->initProcessing (spec.maximumBlockSize);

        toneFilter.prepare (spec);
        toneFilter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        toneFilter.setCutoffFrequency (clampCutoff (4000.0f, sampleRate));

        gainLin.reset (sampleRate, (double) kSmoothingSeconds);
        levelLin.reset (sampleRate, (double) kSmoothingSeconds);

        reset();
    }

    void reset()
    {
        if (oversampling != nullptr)
            oversampling->reset();

        toneFilter.reset();
        gainLin.setCurrentAndTargetValue (gainLin.getTargetValue());
        levelLin.setCurrentAndTargetValue (levelLin.getTargetValue());
    }

    void setParameters (float gainDb, float toneHz, float levelDb)
    {
        gainLin.setTargetValue (juce::Decibels::decibelsToGain (gainDb));
        levelLin.setTargetValue (juce::Decibels::decibelsToGain (levelDb));
        toneFilter.setCutoffFrequency (clampCutoff (toneHz, sampleRate));
    }

    void process (juce::dsp::AudioBlock<float> block)
    {
        if (oversampling == nullptr || block.getNumSamples() == 0)
            return;

        auto upBlock = oversampling->processSamplesUp (block);

        const auto factor = (size_t) oversampling->getOversamplingFactor();
        const auto numOsSamples = upBlock.getNumSamples();
        const auto numChannels = upBlock.getNumChannels();

        float currentGain = gainLin.getCurrentValue();

        for (size_t i = 0; i < numOsSamples; ++i)
        {
            // The smoother runs at the base rate, so only advance it once per host sample.
            if (i % factor == 0)
                currentGain = gainLin.getNextValue();

            for (size_t ch = 0; ch < numChannels; ++ch)
            {
                auto* data = upBlock.getChannelPointer (ch);
                data[i] = std::tanh (data[i] * currentGain);
            }
        }

        oversampling->processSamplesDown (block);

        juce::dsp::ProcessContextReplacing<float> context (block);
        toneFilter.process (context);

        const auto numSamples = block.getNumSamples();
        const auto numOutChannels = block.getNumChannels();

        for (size_t i = 0; i < numSamples; ++i)
        {
            const float level = levelLin.getNextValue();

            for (size_t ch = 0; ch < numOutChannels; ++ch)
                block.getChannelPointer (ch)[i] *= level;
        }
    }

    /** The audio is left exactly as it was; only the delay it always had is now reported
        (docs/FX-ENGINES.md §4.3.3, D2 b). */
    int getLatencySamples() const noexcept
    {
        return oversampling != nullptr ? (int) std::lround (oversampling->getLatencyInSamples()) : 0;
    }

private:
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    juce::dsp::StateVariableTPTFilter<float> toneFilter;
    juce::SmoothedValue<float> gainLin { 1.0f }, levelLin { 1.0f };
    double sampleRate = 44100.0;
};

//==============================================================================
// The circuit types (docs/FX-ENGINES.md §4.3.4-4.3.5).
//==============================================================================
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

constexpr double kVoltsPerFullScale = 1.0;    // V_REF [choice; study R1]
constexpr double kInputLimitVolts = 1000.0;   // [choice] keeps any finite input finite downstream
constexpr double kInputHighPassHz = 15.9;     // [circuit: TS-808 input coupling, Yeh DAFx-07]
constexpr float kParamSmoothSeconds = 0.02f;
constexpr int kControlInterval = 32;          // base samples between coefficient updates
constexpr int kMaxFactor = 8;
constexpr int kHalleySteps = 3;               // see DiodeClipper::evaluateMagnitude

// 1N4148/1N914, the standard SPICE model D1N4148 (IS=2.52n N=1.752) at 25.85 mV thermal
// voltage; all three pedals use this diode family (docs/FX-ENGINES.md §4.3.4).
constexpr double kDiodeIs = 2.52e-9;
constexpr double kDiodeA = 1.752 * 0.02585;

// Ibanez TS-808 clipping stage and tone/volume stage, as given in Yeh, Abel & Smith,
// "Simplified, physically-informed models of distortion and overdrive guitar effects
// pedals", DAFx-07 (ccrma.stanford.edu/~dtyeh/papers/yeh07_dafx_distortion.pdf).
// Drive pot 500 k audio taper: "The original circuit has a logarithmic 500k drive pot"
// (sandelinos.me/diy/tube-screamer, BOM "A500k").
constexpr double kTsR1 = 4.7e3, kTsCz = 47.0e-9, kTsCc = 51.0e-12;
constexpr double kTsR2Fixed = 51.0e3, kTsDrivePot = 500.0e3;
constexpr double kTsPreWarpHz = 720.0;
constexpr double kTsRs = 1.0e3, kTsCs = 0.22e-6, kTsRi = 10.0e3, kTsRf = 1.0e3;
constexpr double kTsTonePot = 20.0e3, kTsRz = 220.0, kTsCz2 = 0.22e-6;
// The TS tone pot is a "W" taper (same BOM: "W20k"); taken as linear over T [choice: the
// knob is already logarithmic in Hz], with 0.5 % end resistance so neither leg is zero.
constexpr double kTsToneEnd = 0.005;

// ProCo RAT, from the madbeanpedals "Runt2020" schematic and BOM ("based on the ProCo Rat",
// madbeanpedals.com/projects/_folders/1590A/pdf/Runt2020.pdf): R4 47R + C6 2u2 and
// R5 560R + C5 4u7 to ground, GAIN 100kA with C4 100pF across it (op-amp feedback), R6 1k
// from the op-amp output to D1/D2 1n914 to ground, CUT 100kA in series with R7 1k5 into
// C8 3n3, LM308 with C3 30pF. Gain 1 + 100k / (47 || 560) and the 32 kHz / 475 Hz filter
// ends agree with cushychicken.github.io/ltspice-proco-rat.
constexpr double kRatR1 = 47.0, kRatC1 = 2.2e-6, kRatR2 = 560.0, kRatC2 = 4.7e-6;
constexpr double kRatGainPot = 100.0e3, kRatCf = 100.0e-12;
constexpr double kRatClipSeries = 1.0e3;
constexpr double kRatRail = 4.5;              // [circuit: 9 V supply, op-amp biased at 4.5 V]
// [circuit: National LM108/LM208/LM308 data sheet (Dec 1994), LM308 "Open Loop Frequency
// Response": with the standard 30 pF compensation (C3 above) the gain crosses unity near
// 1 MHz, falling 6 dB/oct; "bandwidth and slew rate are proportional to 1/Cf"]
constexpr double kRatGbw = 1.0e6;
constexpr double kRatFilterFixed = 1.5e3, kRatFilterPot = 100.0e3, kRatFilterC = 3.3e-9;

// Big Muff Pi: J.L., "A case study: re-engineering the Big Muff Pi" (2014,
// guitarscience.net/papers/bigmuff.pdf) Fig. 1 and eq. (14): every gain stage is
// K = -Z2 / (Rs + R1), Z2 = RB || CF. Input booster RB11 470k, RG1 39k, CF1 470pF (12.05,
// 720 Hz); clipping stages RB21/RB31 470k, RG2/RG3 10k, CF2/CF3 470pF (47, 720 Hz) with the
// 1N914 pair across RB (through CR 1uF, which settles the limits symmetric, Fig. 8);
// CB2 1uF into RG2 and CO2 1uF into RG3 (15.9 Hz). The sustain pot ("SUSTAIN B100K",
// docs.pedalpcb.com/project/MuffinFuzz-PedalPCB.pdf) is a divider whose wiper feeds RG2,
// so its Thevenin resistance is part of Rs in eq. (14).
constexpr double kMuffBoostGain = 470.0e3 / 39.0e3;
constexpr double kMuffStageGain = 470.0e3 / 10.0e3;
constexpr double kMuffFeedbackR = 470.0e3, kMuffFeedbackC = 470.0e-12;
constexpr double kMuffCouplingHz = 1.0 / (kTwoPi * 1.0e-6 * 10.0e3);
constexpr double kMuffSustainPot = 100.0e3, kMuffSustainLoad = 10.0e3;
// The knob spans the sustain pot from where the divider is 36 dB down to full [choice: the
// knob is 36 dB wide, and the bottom 1.9 % of travel only fades the pedal out to silence].
constexpr double kMuffSustainMin = 0.018745;
// Tone stack. The paper's values (RT 27k, CT 10nF, CB 4.7nF, RB 27k, 500k pot, its Fig. 14)
// dip only 3.4 dB below the 220 Hz-5 kHz line at 900 Hz with the pot centred. Used here
// instead [choice]: the 1975 "Ram's Head" stack, R8 39k, C8 10nF, C9 4nF, R5 22k (LP 408 Hz,
// HP 1809 Hz, "big scoop of the mids", coda-effects.com/p/big-muff-tonestack-dealing-with-
// mids.html), 100k linear tone pot (muzique.com/lab/dtone.htm), which dips 6.1 dB.
constexpr double kMuffRt = 39.0e3, kMuffCt = 10.0e-9, kMuffCb = 4.0e-9, kMuffRb = 22.0e3;
constexpr double kMuffTonePot = 100.0e3;
constexpr double kMuffToneEnd = 0.005;        // [choice] pot end resistance, as the TS tone
// The tone stack drives the output stage's bias divider, RB41 470k || RB42 100k (Fig. 1).
constexpr double kMuffToneLoad = 470.0e3 * 100.0e3 / 570.0e3;

/** Audio-taper pot: 10 % of the resistance at mid travel (the common A-taper law). */
double audioTaper (double p) noexcept
{
    return (std::pow (81.0, p) - 1.0) / 80.0;
}

/** Bilinear constant c in s = c (1 - z^-1) / (1 + z^-1) that maps `hz` exactly. */
double warpedC (double hz, double rate) noexcept
{
    const double w = kTwoPi * juce::jlimit (1.0, 0.49 * rate, hz);
    return w / std::tan (w / (2.0 * rate));
}

/** Natural log to about 3e-8 for x >= 1: exponent bits plus a short atanh series. Only
    the clipper's Newton start uses it, where it costs nothing in the converged result. */
double roughLog (double x) noexcept
{
    const auto bits = juce::readUnaligned<juce::uint64> (&x);
    auto exponent = (int) ((bits >> 52) & 0x7ff) - 1023;
    const juce::uint64 mantissaBits = (bits & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
    auto m = juce::readUnaligned<double> (&mantissaBits);

    if (m > 1.4142135623730951)
    {
        m *= 0.5;
        ++exponent;
    }

    const double s = (m - 1.0) / (m + 1.0);
    const double s2 = s * s;
    return exponent * 0.6931471805599453 + 2.0 * s * (1.0 + s2 * (1.0 / 3.0 + s2 * (0.2 + s2 * (1.0 / 7.0))));
}

/** States that drift out of range or into denormals are cleared once per block. */
void tidy (double& state) noexcept
{
    const double a = std::abs (state);

    if (! (a >= 1.0e-15 && a < 1.0e12))
        state = 0.0;
}

//==============================================================================
/** First-order section from (n0 + n1 s) / (d0 + d1 s) by the bilinear transform; TDF II,
    double, one state per channel. */
struct FirstOrder
{
    void setAnalog (double n0, double n1, double d0, double d1, double c) noexcept
    {
        const double inv = 1.0 / (d0 + d1 * c);
        b0 = (n0 + n1 * c) * inv;
        b1 = (n0 - n1 * c) * inv;
        a1 = (d0 - d1 * c) * inv;
    }

    double process (int ch, double x) noexcept
    {
        const double y = b0 * x + z[ch];
        z[ch] = b1 * x - a1 * y;
        return y;
    }

    void reset() noexcept { z[0] = z[1] = 0.0; }
    void tidyStates() noexcept { tidy (z[0]); tidy (z[1]); }

    double b0 = 1.0, b1 = 0.0, a1 = 0.0;
    double z[2] {};
};

/** Second-order section from (n0 + n1 s + n2 s^2) / (d0 + d1 s + d2 s^2), bilinear. */
struct SecondOrder
{
    void setAnalog (double n0, double n1, double n2, double d0, double d1, double d2, double c) noexcept
    {
        const double cc = c * c;
        const double inv = 1.0 / (d0 + d1 * c + d2 * cc);
        b0 = (n0 + n1 * c + n2 * cc) * inv;
        b1 = 2.0 * (n0 - n2 * cc) * inv;
        b2 = (n0 - n1 * c + n2 * cc) * inv;
        a1 = 2.0 * (d0 - d2 * cc) * inv;
        a2 = (d0 - d1 * c + d2 * cc) * inv;
    }

    double process (int ch, double x) noexcept
    {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }

    void reset() noexcept { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
    void tidyStates() noexcept { tidy (z1[0]); tidy (z1[1]); tidy (z2[0]); tidy (z2[1]); }

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1[2] {}, z2[2] {};
};

//==============================================================================
/** Antiparallel diode pair behind a resistance R: u = g(v) = v + k sinh (v / a), k = 2 Is R,
    solved for v per sample and anti-aliased to second order through the exact
    antiderivatives of v(u) (one oversampled sample of delay). An optional rail clamps u
    before the diodes; the clamp is folded into the same antiderivatives. Second order
    rather than the spec's first: at full gain all three pedals drive the diodes so hard
    that the output edge is a step within a fraction of a sample, and a first-order kernel
    at 4x leaves its images at -46 dB (Distortion, 7.5 kHz) to -57 dB (1.2 kHz) below 20 kHz.

    Both antiderivatives are written so that their derivative with respect to v vanishes at
    the root (F1 = u v - G(v), F2 = u^2 v / 2 - u G(v) + B(v), G' = g, B' = g^2 / 2): an error
    e in the solved v moves them by O(e^2) only, which the divided differences need. */
class DiodeClipper
{
public:
    struct Point { double u = 0.0, v = 0.0, f1 = 0.0, f2 = 0.0; };

    void setSeriesResistance (double ohms) noexcept
    {
        k = 2.0 * kDiodeIs * ohms;
        kOverA = k / kDiodeA;

        if (rail > 0.0)
            evaluateMagnitude (rail, railPoint);
    }

    void setRail (double volts) noexcept
    {
        rail = volts;

        if (rail > 0.0)
            evaluateMagnitude (rail, railPoint);
    }

    void reset() noexcept
    {
        for (auto& s : state)
            s = {};
    }

    /** Re-evaluates the stored previous inputs on the current curve, so a change of R does
        not put a step into the next difference quotient. */
    void refresh() noexcept
    {
        for (auto& s : state)
        {
            evaluate (s.p1.u, s.p1);
            evaluate (s.p2.u, s.p2);
            s.mean1 = mean (s.p1, s.p2);
        }
    }

    /** The stateless part: v(u) and the antiderivatives for a run of inputs. Separate from
        finish() so the solves, which are independent of each other, can overlap in the CPU. */
    void solve (const double* u, Point* p, int count) const noexcept
    {
        for (int m = 0; m < count; ++m)
            evaluate (u[m], p[m]);
    }

    /** The anti-aliased output for the next point, in order. */
    double finish (int ch, const Point& p) noexcept
    {
        auto& s = state[ch];

        // y = 2 (D(u, u1) - D(u1, u2)) / (u - u2), D the mean of F1 over an interval: the
        // average of v(u) under a triangle spanning the last three inputs.
        const double m0 = mean (p, s.p1);
        const double span = p.u - s.p2.u;
        double y;

        if (std::abs (span) > tolerance (p.u, s.p2.u))
        {
            y = 2.0 * (m0 - s.mean1) / span;
        }
        else
        {
            // u2 -> u: the limit is 2 dD(u1, b)/db at b = u, = 2 (D(u1, u) - F1(u)) / (u1 - u).
            const double h = s.p1.u - p.u;
            y = std::abs (h) > tolerance (s.p1.u, p.u) ? 2.0 * (m0 - p.f1) / h : s.p1.v;
        }

        s.p2 = s.p1;
        s.p1 = p;
        s.mean1 = m0;
        return y;
    }

private:
    static double tolerance (double a, double b) noexcept
    {
        return 1.0e-5 * (1.0 + 0.5 * (std::abs (a) + std::abs (b)));
    }

    /** Mean of F1 over [b.u, a.u]: the divided difference of F2 where it is well conditioned,
        else the end-corrected trapezoid (F1' = v), exact to O(h^4). */
    double mean (const Point& a, const Point& b) const noexcept
    {
        const double h = a.u - b.u;

        if (std::abs (h) > tolerance (a.u, b.u))
            return (a.f2 - b.f2) / h;

        return 0.5 * (a.f1 + b.f1) - h * (a.v - b.v) * (1.0 / 12.0);
    }

    /** v >= 0, F1 and F2 for u >= 0. Three Halley steps from the upper bound
        min (u, a asinh (u / k)) reach double precision from 1e-6 to 1e5 V for every R used
        here. expm1 keeps cosh - 1 and sinh exact for small v. */
    void evaluateMagnitude (double u, Point& p) const noexcept
    {
        const double y = u / k;
        double v = juce::jmin (u, kDiodeA * roughLog (y + std::sqrt (y * y + 1.0)));
        double em = 0.0, step = 0.0;

        for (int i = 0; i < kHalleySteps; ++i)
        {
            // f, f' and f'' all scaled by e = exp (v / a), which cancels in the Halley step
            // and saves the division for exp (-v / a).
            em = std::expm1 (v * (1.0 / kDiodeA));
            const double e = 1.0 + em;
            const double f = e * (v - u) + 0.5 * k * em * (e + 1.0);
            const double f1 = e + 0.5 * kOverA * (e * e + 1.0);
            const double f2 = 0.5 * kOverA * (1.0 / kDiodeA) * em * (e + 1.0);
            step = 2.0 * f * f1 / (2.0 * f1 * f1 - f * f2);
            v -= step;
        }

        // exp (v / a) at the final v, from the last one to first order in the last step.
        em -= (1.0 + em) * step * (1.0 / kDiodeA);
        const double e = 1.0 + em;
        const double sh = 0.5 * em * (e + 1.0) / e;          // sinh (v / a)
        const double chm1 = 0.5 * em * em / e;               // cosh (v / a) - 1
        const double ch = 1.0 + chm1;
        const double ka = k * kDiodeA;

        const double g = v * v * 0.5 + ka * chm1;            // G(v)
        const double b = v * v * v * (1.0 / 6.0) + ka * (v * ch - kDiodeA * sh)
                       + 0.25 * k * k * (kDiodeA * sh * ch - v);
        p.u = u;
        p.v = v;
        p.f1 = u * v - g;
        p.f2 = 0.5 * u * u * v - u * g + b;
    }

    void evaluate (double u, Point& p) const noexcept
    {
        const double mag = std::abs (u);

        if (rail > 0.0 && mag > rail)
        {
            const double over = mag - rail;
            p.u = u;
            p.v = std::copysign (railPoint.v, u);
            p.f1 = railPoint.f1 + railPoint.v * over;
            p.f2 = std::copysign (railPoint.f2 + over * (railPoint.f1 + 0.5 * railPoint.v * over), u);
            return;
        }

        evaluateMagnitude (mag, p);
        p.u = u;
        p.v = std::copysign (p.v, u);
        p.f2 = std::copysign (p.f2, u);
    }

    struct State { Point p1, p2; double mean1 = 0.0; };

    double k = 1.0e-4, kOverA = 1.0e-4 / kDiodeA;
    double rail = 0.0;
    Point railPoint;
    State state[2];
};

//==============================================================================
int clippersOf (DriveFx::Type type) noexcept
{
    return type == DriveFx::Type::fuzz ? 2 : 1;
}

//==============================================================================
/** Screamer, Distortion and Fuzz: one engine, the type fixed between resets. */
class PedalDrive
{
public:
    static int factorFor (DriveFx::Type type, double sampleRate) noexcept
    {
        // Keeps the internal rate >= 176.4 kHz (docs/FX-ENGINES.md §4.3.4), >= 352.8 kHz for
        // Distortion: its 67 dB op-amp stage into a rail makes the edge a near-ideal step, and
        // at 4x even the second-order kernel leaves -67 dB of images for a 7.5 kHz tone.
        const int base = sampleRate < 80000.0 ? 4 : (sampleRate < 160000.0 ? 2 : 1);
        return type == DriveFx::Type::distortion ? 2 * base : base;
    }

    static int stagesFor (int factor) noexcept
    {
        return factor >= 8 ? 3 : (factor >= 4 ? 2 : (factor >= 2 ? 1 : 0));
    }

    /** JUCE's maximum-quality polyphase IIR half-band cascades, low-frequency delay in base
        samples (juce::dsp::Oversampling::getLatencyInSamples, which prepare() checks). */
    static double halfBandLatency (int factor) noexcept
    {
        static constexpr double delay[4] { 0.0, 3.137320995, 4.432672501, 4.951390743 };
        return delay[stagesFor (factor)];
    }

    /** Each clipper's second-order ADAA delays the signal by one oversampled sample. */
    static double clipperDelay (DriveFx::Type type, int factor) noexcept
    {
        return clippersOf (type) / (double) factor;
    }

    static int latencyFor (DriveFx::Type type, double sampleRate) noexcept
    {
        const int factor = factorFor (type, sampleRate);
        return (int) std::lround (halfBandLatency (factor) + clipperDelay (type, factor));
    }

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        const auto channels = (size_t) juce::jmax ((juce::uint32) 1, spec.numChannels);

        for (auto pedal : { DriveFx::Type::screamer, DriveFx::Type::distortion })
        {
            const int n = factorFor (pedal, sampleRate);
            auto& os = oversamplers[stagesFor (n)];
            os = std::make_unique<juce::dsp::Oversampling<float>> (
                channels, (size_t) stagesFor (n), juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
            os->initProcessing (spec.maximumBlockSize);
            jassert (std::abs ((double) os->getLatencyInSamples() - halfBandLatency (n)) < 1.0e-6);
        }

        const double cIn = warpedC (kInputHighPassHz, sampleRate);
        inputHighPass.setAnalog (0.0, 1.0 / (kTwoPi * kInputHighPassHz), 1.0, 1.0 / (kTwoPi * kInputHighPassHz), cIn);

        drive.prepare (kParamSmoothSeconds, (float) sampleRate, 0.0f);
        tone.prepare (kParamSmoothSeconds, (float) sampleRate, 0.5f);
        level.prepare (kParamSmoothSeconds, (float) sampleRate, 1.0f);

        setType (DriveFx::Type::screamer);
        reset();
    }

    void setType (DriveFx::Type newType) noexcept
    {
        type = newType;
        factor = factorFor (type, sampleRate);
        osRate = sampleRate * factor;
        oversampling = oversamplers[stagesFor (factor)].get();

        clip1.setRail (type == DriveFx::Type::distortion ? kRatRail : 0.0);
        clip1.setSeriesResistance (type == DriveFx::Type::fuzz ? kMuffFeedbackR
                                   : (type == DriveFx::Type::distortion ? kRatClipSeries : kTsR2Fixed));
        clip2.setSeriesResistance (kMuffFeedbackR);

        tsPreC = warpedC (kTsPreWarpHz, osRate);
        bilinearOs = 2.0 * osRate;

        // Fixed parts of Distortion and Fuzz.
        ratLeg1.setAnalog (0.0, kRatC1, 1.0, kRatR1 * kRatC1, bilinearOs);
        ratLeg2.setAnalog (0.0, kRatC2, 1.0, kRatR2 * kRatC2, bilinearOs);
        const double muffLowPass = 1.0 / (kTwoPi * kMuffFeedbackR * kMuffFeedbackC);
        const double cMuff = warpedC (muffLowPass, osRate);
        muffBoost.setAnalog (kMuffBoostGain, 0.0, 1.0, kMuffFeedbackR * kMuffFeedbackC, cMuff);
        muffStage1.setAnalog (kMuffStageGain, 0.0, 1.0, kMuffFeedbackR * kMuffFeedbackC, cMuff);
        muffStage2 = muffStage1;
        const double cCouple = warpedC (kMuffCouplingHz, osRate);
        muffCouple1.setAnalog (0.0, 1.0 / (kTwoPi * kMuffCouplingHz), 1.0, 1.0 / (kTwoPi * kMuffCouplingHz), cCouple);
        muffCouple2 = muffCouple1;
    }

    void setTargets (float d, float t, float levelGain) noexcept
    {
        drive.setTarget (d);
        tone.setTarget (t);
        level.setTarget (levelGain);
    }

    void snap() noexcept
    {
        drive.snapToTarget();
        tone.snapToTarget();
        level.snapToTarget();
    }

    void reset() noexcept
    {
        // All of them: the next block may start on another type without a further reset.
        for (auto& os : oversamplers)
            if (os != nullptr)
                os->reset();

        inputHighPass.reset();
        tsPre.reset();
        ratLeg1.reset();
        ratLeg2.reset();
        ratFeedback.reset();
        ratBandwidth.reset();
        muffBoost.reset();
        muffStage1.reset();
        muffStage2.reset();
        muffCouple1.reset();
        muffCouple2.reset();
        toneFilter.reset();
        clip1.reset();
        clip2.reset();
        cleanPrev[0] = cleanPrev[1] = 0.0;
        phase = 0;
        snap();
    }

    void process (juce::dsp::AudioBlock<float> block) noexcept
    {
        const int n = (int) block.getNumSamples();
        const int channels = juce::jmin (2, (int) block.getNumChannels());

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = block.getChannelPointer ((size_t) ch);

            for (int i = 0; i < n; ++i)
            {
                // Clamped, and NaN-safe: a NaN fails every comparison and becomes 0.
                const double raw = (double) data[i] * kVoltsPerFullScale;
                const double x = std::abs (raw) <= kInputLimitVolts ? raw
                               : (raw > 0.0 ? kInputLimitVolts : (raw < 0.0 ? -kInputLimitVolts : 0.0));
                data[i] = (float) inputHighPass.process (ch, x);
            }
        }

        auto up = oversampling->processSamplesUp (block);
        const int start = phase;

        switch (type)
        {
            case DriveFx::Type::distortion: runOversampled<DriveFx::Type::distortion> (up, n, channels, start); break;
            case DriveFx::Type::fuzz:       runOversampled<DriveFx::Type::fuzz> (up, n, channels, start); break;
            case DriveFx::Type::classic:
            case DriveFx::Type::screamer:
            case DriveFx::Type::boost:
            case DriveFx::Type::treble:     runOversampled<DriveFx::Type::screamer> (up, n, channels, start); break;
        }

        oversampling->processSamplesDown (block);

        for (int i = 0; i < n; ++i)
        {
            const double gain = (double) level.next() / kVoltsPerFullScale;

            for (int ch = 0; ch < channels; ++ch)
                block.getChannelPointer ((size_t) ch)[i] = (float) (block.getChannelPointer ((size_t) ch)[i] * gain);
        }

        phase = (start + n) & (kControlInterval - 1);

        inputHighPass.tidyStates();
        tsPre.tidyStates();
        ratLeg1.tidyStates();
        ratLeg2.tidyStates();
        ratFeedback.tidyStates();
        ratBandwidth.tidyStates();
        muffBoost.tidyStates();
        muffStage1.tidyStates();
        muffStage2.tidyStates();
        muffCouple1.tidyStates();
        muffCouple2.tidyStates();
        toneFilter.tidyStates();
    }

private:
    /** Runs the oversampled circuit in chunks that never straddle a coefficient update,
        each chunk in passes: the linear filters (cheap, sequential), then every clipper
        solve of the chunk (independent of each other), then the ADAA quotients. The tone
        stage runs here too, at N fs: at the base rate its bilinear warping would cost
        1.2-1.4 dB at 10 kHz (48 kHz), against 0.5 dB allowed by acceptance D8. */
    template <DriveFx::Type T>
    void runOversampled (juce::dsp::AudioBlock<float>& up, int n, int channels, int start) noexcept
    {
        for (int i = 0; i < n;)
        {
            const int pos = (start + i) & (kControlInterval - 1);
            const int len = juce::jmin (n - i, kControlInterval - pos);

            for (int j = 0; j < len; ++j)
            {
                const float d = drive.next();
                const float t = tone.next();

                if (j == 0 && pos == 0)
                {
                    updateDrive<T> ((double) d);
                    updateTone ((double) t);
                }
            }

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* data = up.getChannelPointer ((size_t) ch) + i * factor;
                const int count = len * factor;
                circuit<T> (ch, data, count);

                for (int m = 0; m < count; ++m)
                    data[m] = (float) toneFilter.process (ch, (double) data[m]);
            }

            i += len;
        }
    }

    template <DriveFx::Type T>
    void circuit (int ch, float* data, int count) noexcept
    {
        if constexpr (T == DriveFx::Type::screamer)
        {
            for (int m = 0; m < count; ++m)
                scratchU[m] = tsPre.process (ch, (double) data[m]);

            clip1.solve (scratchU, scratchP, count);

            for (int m = 0; m < count; ++m)
            {
                // Non-inverting stage: output = input + voltage across the feedback network.
                // The input is delayed by the one sample the ADAA adds.
                const double clean = cleanPrev[ch];
                cleanPrev[ch] = (double) data[m];
                data[m] = (float) (clean + clip1.finish (ch, scratchP[m]));
            }
        }
        else if constexpr (T == DriveFx::Type::distortion)
        {
            for (int m = 0; m < count; ++m)
            {
                const double x = (double) data[m];
                const double current = ratLeg1.process (ch, x) + ratLeg2.process (ch, x);
                scratchU[m] = ratBandwidth.process (ch, x + ratFeedback.process (ch, current));
            }

            clip1.solve (scratchU, scratchP, count);

            for (int m = 0; m < count; ++m)
                data[m] = (float) clip1.finish (ch, scratchP[m]);
        }
        else
        {
            // Booster, sustain divider, CB2 into RG2, stage 1.
            for (int m = 0; m < count; ++m)
                scratchU[m] = muffStage1.process (ch, muffCouple2.process (ch, muffBoost.process (ch, (double) data[m]) * sustainGain));

            clip1.solve (scratchU, scratchP, count);

            for (int m = 0; m < count; ++m)
                scratchU[m] = muffStage2.process (ch, muffCouple1.process (ch, clip1.finish (ch, scratchP[m])));

            clip2.solve (scratchU, scratchP, count);

            for (int m = 0; m < count; ++m)
                data[m] = (float) clip2.finish (ch, scratchP[m]);
        }
    }

    template <DriveFx::Type T>
    void updateDrive (double d) noexcept
    {
        if constexpr (T == DriveFx::Type::screamer)
        {
            // PRE(s) = (R2/R1) s R1 Cz / (1 + s R1 Cz) / (1 + s R2 Cc): the op-amp's G(s) - 1.
            const double r2 = kTsR2Fixed + kTsDrivePot * audioTaper (d);
            const double tz = kTsR1 * kTsCz, tc = r2 * kTsCc;
            tsPre.setAnalog (0.0, r2 * kTsCz, 0.0, 1.0, tz + tc, tz * tc, tsPreC);
            clip1.setSeriesResistance (r2);
            clip1.refresh();
        }
        else if constexpr (T == DriveFx::Type::distortion)
        {
            // G(s) = 1 + Zf (Y1 + Y2): the legs and Zf share one bilinear constant, so the
            // sum of sections is exactly the bilinear image of G. Zf's pole, and the
            // op-amp's, are kept below Nyquist so neither cancels its zero at z = -1.
            const double rd = kRatGainPot * audioTaper (d);
            const double cornerLimit = 0.45 * osRate;
            const double feedbackTau = juce::jmax (rd * kRatCf, 1.0 / (kTwoPi * cornerLimit));
            ratFeedback.setAnalog (rd, 0.0, 1.0, feedbackTau, bilinearOs);

            const std::complex<double> s (0.0, kTwoPi * 1000.0);
            const auto g1k = 1.0 + rd / (1.0 + s * rd * kRatCf)
                                 * (s * kRatC1 / (1.0 + s * kRatR1 * kRatC1) + s * kRatC2 / (1.0 + s * kRatR2 * kRatC2));
            const double bandwidthHz = juce::jmin (kRatGbw / std::abs (g1k), cornerLimit);
            ratBandwidth.setAnalog (1.0, 0.0, 1.0, 1.0 / (kTwoPi * bandwidthHz), bilinearOs);
        }
        else
        {
            // Linear pot over the knob's span, Thevenin resistance p (1 - p) Rpot in series
            // with RG2 into the virtual ground: eq. (14) with Rs = p (1 - p) Rpot.
            const double p = kMuffSustainMin + (1.0 - kMuffSustainMin) * d;
            sustainGain = p * kMuffSustainLoad / (kMuffSustainLoad + p * (1.0 - p) * kMuffSustainPot);
        }
    }

    void updateTone (double t) noexcept
    {
        switch (type)
        {
            case DriveFx::Type::distortion:
            {
                // The Filter pot (CUT 100kA, audio taper as in the schematic; the spec assumed
                // linear) adds resistance clockwise, so its rotation is 1 - T: T = 1 is bright.
                const double r = kRatFilterFixed + kRatFilterPot * audioTaper (1.0 - t);
                const double hz = juce::jmin (1.0 / (kTwoPi * r * kRatFilterC), 0.45 * osRate);
                toneFilter.setAnalog (1.0, 0.0, 0.0, 1.0, 1.0 / (kTwoPi * hz), 0.0, warpedC (hz, osRate));
                break;
            }

            case DriveFx::Type::fuzz:
            {
                // Low-pass leg RT/CT to node A, high-pass leg CB/RB to node B, the pot from A
                // (T = 0, bass) to B, the wiper loaded by RL. Node W eliminated, then A and B:
                // H = N(s) / D(s), both quadratic (each divided by the wiper node sum S).
                const double tp = juce::jlimit (kMuffToneEnd, 1.0 - kMuffToneEnd, t);
                const double gT = 1.0 / kMuffRt, gB = 1.0 / kMuffRb;
                const double ga = 1.0 / (tp * kMuffTonePot), gb = 1.0 / ((1.0 - tp) * kMuffTonePot);
                const double sum = ga + gb + 1.0 / kMuffToneLoad;
                const double m = ga * gb / sum;
                const double p0 = gT + ga - ga * ga / sum, q0 = gB + gb - gb * gb / sum;
                toneFilter.setAnalog (gT * (ga * q0 + gb * m), kMuffCb * (ga * gT + ga * m + gb * p0), gb * kMuffCt * kMuffCb,
                                      sum * (p0 * q0 - m * m), sum * (p0 * kMuffCb + q0 * kMuffCt), sum * kMuffCt * kMuffCb,
                                      bilinearOs);
                break;
            }

            case DriveFx::Type::classic:
            case DriveFx::Type::screamer:
            case DriveFx::Type::boost:
            case DriveFx::Type::treble:
            {
                const double tp = juce::jlimit (kTsToneEnd, 1.0 - kTsToneEnd, t);
                const double gl = 1.0 / (tp * kTsTonePot), gr = 1.0 / ((1.0 - tp) * kTsTonePot);
                const double g = gl + gr, gs = 1.0 / kTsRs + 1.0 / kTsRi, c = kTsCz2;
                const double n0 = g / kTsRs, n1 = c * (g * kTsRz + 1.0 + kTsRf * gr) / kTsRs;
                const double d0 = gs * g, d1 = gs * c * (g * kTsRz + 1.0) + kTsCs * g + gl * c;
                const double d2 = kTsCs * c * (g * kTsRz + 1.0);
                toneFilter.setAnalog (n0, n1, 0.0, d0, d1, d2, bilinearOs);
                break;
            }
        }
    }

    std::unique_ptr<juce::dsp::Oversampling<float>> oversamplers[4];   // by stage count
    juce::dsp::Oversampling<float>* oversampling = nullptr;
    DriveFx::Type type = DriveFx::Type::screamer;
    double sampleRate = 48000.0, osRate = 192000.0;
    double tsPreC = 1.0, bilinearOs = 1.0;
    double sustainGain = 0.0;
    double cleanPrev[2] {};
    int factor = 4, phase = 0;

    static constexpr int kScratch = kControlInterval * kMaxFactor;
    double scratchU[kScratch];
    DiodeClipper::Point scratchP[kScratch];

    FirstOrder inputHighPass;
    SecondOrder tsPre;
    FirstOrder ratLeg1, ratLeg2, ratFeedback, ratBandwidth;
    FirstOrder muffBoost, muffStage1, muffStage2, muffCouple1, muffCouple2;
    SecondOrder toneFilter;
    DiodeClipper clip1, clip2;
    fx::Smoother drive, tone, level;
};

//==============================================================================
/** Boost and Treble: linear, base rate, no latency. */
class CleanDrive
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        gain.prepare (kParamSmoothSeconds, (float) sampleRate, 1.0f);
        toneHz.prepare (kParamSmoothSeconds, (float) sampleRate, 4000.0f);
        level.prepare (kParamSmoothSeconds, (float) sampleRate, 1.0f);
        reset();
    }

    void setType (DriveFx::Type newType) noexcept { type = newType; }

    void setTargets (float gainLinear, float hz, float levelGain) noexcept
    {
        gain.setTarget (gainLinear);
        toneHz.setTarget (hz);
        level.setTarget (levelGain);
    }

    void snap() noexcept
    {
        gain.snapToTarget();
        toneHz.snapToTarget();
        level.snapToTarget();
    }

    void reset() noexcept
    {
        filter.reset();
        phase = 0;
        snap();
    }

    void process (juce::dsp::AudioBlock<float> block) noexcept
    {
        const int n = (int) block.getNumSamples();
        const int channels = juce::jmin (2, (int) block.getNumChannels());
        const bool boost = type == DriveFx::Type::boost;
        const int start = phase;

        for (int i = 0; i < n; ++i)
        {
            const double g = (double) gain.next();
            const double hz = (double) toneHz.next();
            const double out = (double) level.next() * (boost ? g : 1.0);

            const bool tick = ((start + i) & (kControlInterval - 1)) == 0;

            if (tick)
                setTone (hz, boost);

            // The shelf's gain lives in its coefficients: while the gain is still gliding they
            // follow it every sample, since a 32-sample staircase on a 36 dB move clicks.
            if (! boost && (tick || std::abs (g - (double) gain.getTarget()) > 1.0e-6 * g))
                setShelf (g);

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* data = block.getChannelPointer ((size_t) ch);
                data[i] = (float) (filter.process (ch, (double) data[i]) * out);
            }
        }

        phase = (start + n) & (kControlInterval - 1);
        filter.tidyStates();
    }

private:
    void setTone (double hz, bool boost) noexcept
    {
        const double fc = juce::jlimit (1.0, 0.49 * sampleRate, hz);
        wc = kTwoPi * fc;
        c = warpedC (fc, sampleRate);

        if (boost)
            filter.setAnalog (1.0, 0.0, 1.0, 1.0 / wc, c);
    }

    void setShelf (double g) noexcept
    {
        // (1 + s / wz) / (1 + s / wp), wz = wc / sqrt G, wp = wc sqrt G: 0 dB below, G above,
        // geometric centre at the knob, bilinear prewarped there (docs/FX-ENGINES.md §4.3.5).
        const double root = std::sqrt (g);
        filter.setAnalog (1.0, root / wc, 1.0, 1.0 / (wc * root), c);
    }

    DriveFx::Type type = DriveFx::Type::boost;
    double sampleRate = 48000.0, wc = 1.0, c = 1.0;
    int phase = 0;
    FirstOrder filter;
    fx::Smoother gain, toneHz, level;
};

bool isPedal (int type) noexcept
{
    return type == (int) DriveFx::Type::screamer || type == (int) DriveFx::Type::distortion
        || type == (int) DriveFx::Type::fuzz;
}

float finiteOr (float value, float fallback) noexcept
{
    return std::isfinite (value) ? value : fallback;
}
} // namespace

//==============================================================================
struct DriveFx::Impl
{
    void resetEngine (int type)
    {
        if (type == (int) Type::classic)
        {
            classic.reset();
        }
        else if (isPedal (type))
        {
            pedal.setType ((Type) type);
            pedal.reset();
        }
        else
        {
            clean.setType ((Type) type);
            clean.reset();
        }
    }

    void run (int type, juce::dsp::AudioBlock<float> block)
    {
        if (type == (int) Type::classic)
            classic.process (block);
        else if (isPedal (type))
            pedal.process (block);
        else
            clean.process (block);
    }

    ClassicDrive classic;
    PedalDrive pedal;
    CleanDrive clean;
    fx::EngineSwitcher switcher;
    DriveParams params;
    double sampleRate = 48000.0;
    int maxBlock = 512;
    bool snapEngine = true, snapParams = true;
};

DriveFx::DriveFx() : impl (std::make_unique<Impl>()) {}
DriveFx::~DriveFx() = default;

void DriveFx::prepare (const juce::dsp::ProcessSpec& spec)
{
    auto& s = *impl;
    s.sampleRate = spec.sampleRate;
    s.maxBlock = juce::jmax (1, (int) spec.maximumBlockSize);
    s.classic.prepare (spec);
    s.pedal.prepare (spec);
    s.clean.prepare (spec);
    s.switcher.prepare (spec.sampleRate, s.maxBlock, (int) spec.numChannels, s.params.type);
    s.snapEngine = true;
    s.snapParams = true;
}

void DriveFx::reset()
{
    auto& s = *impl;
    s.classic.reset();
    s.pedal.reset();
    s.clean.reset();
    s.snapEngine = true;
    s.snapParams = true;
}

void DriveFx::setParameters (const DriveParams& params)
{
    auto& s = *impl;
    auto& p = s.params;
    p.type = juce::jlimit (0, numTypes - 1, params.type);
    p.gainDb = juce::jlimit (0.0f, 36.0f, finiteOr (params.gainDb, 12.0f));
    p.toneHz = juce::jlimit (500.0f, 12000.0f, finiteOr (params.toneHz, 4000.0f));
    p.levelDb = juce::jlimit (-24.0f, 12.0f, finiteOr (params.levelDb, 0.0f));

    s.classic.setParameters (p.gainDb, p.toneHz, p.levelDb);

    const float levelGain = juce::Decibels::decibelsToGain (p.levelDb);
    s.pedal.setTargets (p.gainDb / 36.0f, std::log (p.toneHz / 500.0f) / std::log (24.0f), levelGain);
    s.clean.setTargets (juce::Decibels::decibelsToGain (p.gainDb), p.toneHz, levelGain);

    if (s.snapParams)
    {
        s.snapParams = false;
        s.pedal.snap();
        s.clean.snap();
    }
}

void DriveFx::process (juce::dsp::AudioBlock<float> block)
{
    auto& s = *impl;
    const auto total = block.getNumSamples();

    // A block larger than prepare() promised is run in pieces: the oversamplers and the
    // switcher's dry copy are sized for maximumBlockSize.
    for (size_t start = 0; start < total; start += (size_t) s.maxBlock)
    {
        auto piece = block.getSubBlock (start, juce::jmin ((size_t) s.maxBlock, total - start));

        if (s.snapEngine)
        {
            // Nothing has been heard since prepare() or reset(): start on the requested type.
            // Classic is not reset here, so it keeps the frozen code's start-up ramp.
            s.snapEngine = false;
            s.switcher.snapTo (s.params.type);

            if (isPedal (s.params.type))
                s.pedal.setType ((Type) s.params.type);
            else if (s.params.type != (int) Type::classic)
                s.clean.setType ((Type) s.params.type);
        }

        const int type = s.switcher.begin (piece, s.params.type, [&s] (int t, bool)
        {
            s.resetEngine (t);
            return true;
        });

        if (! s.switcher.isHolding())
            s.run (type, piece);

        s.switcher.end (piece);
    }
}

int DriveFx::getLatencySamples() const noexcept
{
    const auto& s = *impl;
    const int type = s.params.type;

    if (type == (int) Type::classic)
        return s.classic.getLatencySamples();

    if (! isPedal (type))
        return 0;

    return PedalDrive::latencyFor ((Type) type, s.sampleRate);
}

int DriveFx::latencyFor (int type, double sampleRate) noexcept
{
    type = juce::jlimit (0, numTypes - 1, type);

    if (type == (int) Type::classic)
        return (int) std::lround (PedalDrive::halfBandLatency (2));   // Classic's own 2x stage

    if (! isPedal (type))
        return 0;

    return PedalDrive::latencyFor ((Type) type, sampleRate);
}
} // namespace tubamp
