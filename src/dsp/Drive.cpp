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
constexpr int kHalleySteps = 3;               // see DiodeClipper::evaluateMagnitude
constexpr double kAdaaMinStep = 1.0e-6;       // volts

// 1N4148/1N914, the standard SPICE model D1N4148 (IS=2.52n N=1.752) at 25.85 mV thermal
// voltage; all three pedals use this diode family (docs/FX-ENGINES.md §4.3.4).
constexpr double kDiodeIs = 2.52e-9;
constexpr double kDiodeA = 1.752 * 0.02585;

// JUCE's maximum-quality polyphase IIR half-bands, low-frequency delay in base samples
// (juce::dsp::Oversampling::getLatencyInSamples, which prepare() checks against these).
constexpr double kHalfBandLatency2x = 3.137320995;
constexpr double kHalfBandLatency4x = 4.432672501;

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
constexpr double kRatGbw = 2.3e6;             // [fit: study R5, harness-only evidence (V:97); revisit by ear]
constexpr double kRatFilterFixed = 1.5e3, kRatFilterPot = 100.0e3, kRatFilterC = 3.3e-9;

// Big Muff Pi, early version: guitarscience.net/papers/bigmuff.pdf Fig. 1 (RG1 39k,
// RB 470k, CF 470pF, RG2/RG3 10k, coupling 1uF; tone RT 27k, CT 10nF, CB 4.7nF, RB 27k).
// Tone ("mix") pot 100 k: "a pair of filters whose output is combined through the 100k mix
// pot" (muzique.com/lab/dtone.htm). Sustain pot 100 k linear ("SUSTAIN B100K",
// docs.pedalpcb.com/project/MuffinFuzz-PedalPCB.pdf), a divider whose wiper feeds RG2 into
// the next stage's virtual-ground base (Fig. 1), so the divider is loaded by RG2.
constexpr double kMuffBoostGain = 470.0e3 / 39.0e3;
constexpr double kMuffStageGain = 470.0e3 / 10.0e3;
constexpr double kMuffFeedbackR = 470.0e3, kMuffFeedbackC = 470.0e-12;
constexpr double kMuffCouplingHz = 1.0 / (kTwoPi * 1.0e-6 * 10.0e3);
constexpr double kMuffSustainPot = 100.0e3, kMuffSustainLoad = 10.0e3;
constexpr double kMuffRt = 27.0e3, kMuffCt = 10.0e-9, kMuffCb = 4.7e-9, kMuffRb = 27.0e3;
constexpr double kMuffTonePot = 100.0e3;

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
/** Antiparallel diode pair behind a resistance R: u = v + k sinh (v / a), k = 2 Is R,
    solved for v per sample and anti-aliased to first order through the exact
    antiderivative of v(u) (docs/FX-ENGINES.md §4.3.4). An optional rail clamps u before
    the diodes; the clamp is folded into the same antiderivative. */
class DiodeClipper
{
public:
    void setSeriesResistance (double ohms) noexcept
    {
        k = 2.0 * kDiodeIs * ohms;
        kOverA = k / kDiodeA;

        if (rail > 0.0)
            evaluateMagnitude (rail, railV, railF);
    }

    void setRail (double volts) noexcept
    {
        rail = volts;

        if (rail > 0.0)
            evaluateMagnitude (rail, railV, railF);
    }

    void reset() noexcept
    {
        for (auto& s : state)
            s = {};
    }

    /** Re-evaluates the stored previous input on the current curve, so a change of R does
        not put a step into the next difference quotient. */
    void refresh() noexcept
    {
        for (auto& s : state)
            evaluate (s.u, s.v, s.F);
    }

    /** The stateless part: v(u) and F(u) for a run of inputs. Separate from finish() so
        the solves, which are independent of each other, can overlap in the CPU. */
    void solve (const double* u, double* v, double* F, int count) const noexcept
    {
        for (int m = 0; m < count; ++m)
            evaluate (u[m], v[m], F[m]);
    }

    /** The difference quotient (F(u) - F(u_prev)) / (u - u_prev), in order. */
    double finish (int ch, double u, double v, double F) noexcept
    {
        auto& s = state[ch];
        const double du = u - s.u;
        // Below the step the quotient is ill-conditioned; the mean of the two solutions is
        // v at the midpoint to second order.
        const double y = std::abs (du) > kAdaaMinStep ? (F - s.F) / du : 0.5 * (v + s.v);

        s.u = u;
        s.v = v;
        s.F = F;
        return y;
    }

private:
    /** v >= 0 and F(u) for u >= 0. Three Halley steps from the upper bound
        min (u, a asinh (u / k)) reach double precision from 1e-6 to 1e5 V for every R used
        here (checked against an 80-step Newton reference); the spec's four Newton steps
        leave up to 1e-5 V and cost more. At the root k sinh (v / a) = u - v, so
        k cosh (v / a) = sqrt (k^2 + (u - v)^2) gives F without another exp. */
    void evaluateMagnitude (double u, double& v, double& F) const noexcept
    {
        const double y = u / k;
        v = juce::jmin (u, kDiodeA * roughLog (y + std::sqrt (y * y + 1.0)));

        for (int i = 0; i < kHalleySteps; ++i)
        {
            // f, f' and f'' all scaled by e = exp (v / a), which cancels in the Halley step
            // and saves the division for exp (-v / a).
            const double e = std::exp (v * (1.0 / kDiodeA));
            const double ee = e * e;
            const double f = e * (v - u) + 0.5 * k * (ee - 1.0);
            const double f1 = e + 0.5 * kOverA * (ee + 1.0);
            const double f2 = 0.5 * kOverA * (1.0 / kDiodeA) * (ee - 1.0);
            v -= 2.0 * f * f1 / (2.0 * f1 * f1 - f * f2);
        }

        const double r = u - v;
        F = u * v - 0.5 * v * v - kDiodeA * (std::sqrt (k * k + r * r) - k);
    }

    void evaluate (double u, double& v, double& F) const noexcept
    {
        const double mag = std::abs (u);

        if (rail > 0.0 && mag > rail)
        {
            v = std::copysign (railV, u);
            F = railF + railV * (mag - rail);
            return;
        }

        double w;
        evaluateMagnitude (mag, w, F);
        v = std::copysign (w, u);
    }

    struct State { double u = 0.0, v = 0.0, F = 0.0; };

    double k = 1.0e-4, kOverA = 1.0e-4 / kDiodeA;
    double rail = 0.0, railV = 0.0, railF = 0.0;
    State state[2];
};

//==============================================================================
/** Screamer, Distortion and Fuzz: one engine, the type fixed between resets. */
class PedalDrive
{
public:
    static int factorFor (double sampleRate) noexcept
    {
        // Keeps the internal rate >= 176.4 kHz (docs/FX-ENGINES.md §4.3.4).
        return sampleRate < 80000.0 ? 4 : (sampleRate < 160000.0 ? 2 : 1);
    }

    static double halfBandLatency (int factor) noexcept
    {
        return factor == 4 ? kHalfBandLatency4x : (factor == 2 ? kHalfBandLatency2x : 0.0);
    }

    /** First-order ADAA delays each clipper by half an oversampled sample; Fuzz has two. */
    static double clipperDelay (DriveFx::Type type, int factor) noexcept
    {
        return (type == DriveFx::Type::fuzz ? 1.0 : 0.5) / (double) factor;
    }

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        factor = factorFor (sampleRate);
        osRate = sampleRate * factor;

        const auto stages = (size_t) (factor == 4 ? 2 : (factor == 2 ? 1 : 0));
        oversampling = std::make_unique<juce::dsp::Oversampling<float>> (
            juce::jmax ((juce::uint32) 1, spec.numChannels), stages,
            juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        oversampling->initProcessing (spec.maximumBlockSize);
        osLatency = (double) oversampling->getLatencyInSamples();
        jassert (std::abs (osLatency - halfBandLatency (factor)) < 1.0e-6);

        const double cIn = warpedC (kInputHighPassHz, sampleRate);
        inputHighPass.setAnalog (0.0, 1.0 / (kTwoPi * kInputHighPassHz), 1.0, 1.0 / (kTwoPi * kInputHighPassHz), cIn);

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

        drive.prepare (kParamSmoothSeconds, (float) sampleRate, 0.0f);
        tone.prepare (kParamSmoothSeconds, (float) sampleRate, 0.5f);
        level.prepare (kParamSmoothSeconds, (float) sampleRate, 1.0f);

        setType (DriveFx::Type::screamer);
        reset();
    }

    /** What the prepared oversampler reports, in base samples. */
    double getHalfBandLatency() const noexcept { return osLatency; }
    int getFactor() const noexcept { return factor; }

    void setType (DriveFx::Type newType) noexcept
    {
        type = newType;
        clip1.setRail (type == DriveFx::Type::distortion ? kRatRail : 0.0);
        clip1.setSeriesResistance (type == DriveFx::Type::fuzz ? kMuffFeedbackR
                                   : (type == DriveFx::Type::distortion ? kRatClipSeries : kTsR2Fixed));
        clip2.setSeriesResistance (kMuffFeedbackR);
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
        if (oversampling != nullptr)
            oversampling->reset();

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

            clip1.solve (scratchU, scratchV, scratchF, count);

            for (int m = 0; m < count; ++m)
            {
                // Non-inverting stage: output = input + voltage across the feedback network.
                // The input is delayed by the same half sample the clipper's ADAA adds.
                const double x = (double) data[m];
                const double clean = 0.5 * (x + cleanPrev[ch]);
                cleanPrev[ch] = x;
                data[m] = (float) (clean + clip1.finish (ch, scratchU[m], scratchV[m], scratchF[m]));
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

            clip1.solve (scratchU, scratchV, scratchF, count);

            for (int m = 0; m < count; ++m)
                data[m] = (float) clip1.finish (ch, scratchU[m], scratchV[m], scratchF[m]);
        }
        else
        {
            for (int m = 0; m < count; ++m)
                scratchU[m] = muffStage1.process (ch, muffBoost.process (ch, (double) data[m])) * sustainGain;

            clip1.solve (scratchU, scratchV, scratchF, count);

            for (int m = 0; m < count; ++m)
            {
                const double y = clip1.finish (ch, scratchU[m], scratchV[m], scratchF[m]);
                scratchU[m] = muffStage2.process (ch, muffCouple1.process (ch, y));
            }

            clip2.solve (scratchU, scratchV, scratchF, count);

            for (int m = 0; m < count; ++m)
                data[m] = (float) muffCouple2.process (ch, clip2.finish (ch, scratchU[m], scratchV[m], scratchF[m]));
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
            // Linear divider with Thevenin resistance d (1 - d) Rpot, feeding RG2 into a
            // virtual ground: the stage gain RB / RG2 is scaled by RG2 / (RG2 + d (1 - d) Rpot).
            // (The spec's unloaded audio-taper divider is not what Fig. 1 or the BOM show.)
            sustainGain = d * kMuffSustainLoad / (kMuffSustainLoad + d * (1.0 - d) * kMuffSustainPot);
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
                // Passive LP/HP pair joined by the pot, wiper unloaded (docs/FX-ENGINES.md §4.3.5).
                const double gT = 1.0 / kMuffRt, gB = 1.0 / kMuffRb, gM = 1.0 / kMuffTonePot;
                const double nA0 = gT * gB + gT * gM, nA1 = kMuffCb * (gT + gM);
                const double nB0 = gM * gT, nB1 = kMuffCb * (gT + gM), nB2 = kMuffCt * kMuffCb;
                const double d0 = gT * gB + gT * gM + gB * gM;
                const double d1 = kMuffCt * (gB + gM) + kMuffCb * (gT + gM);
                const double d2 = kMuffCt * kMuffCb;
                toneFilter.setAnalog ((1.0 - t) * nA0 + t * nB0, (1.0 - t) * nA1 + t * nB1, t * nB2,
                                      d0, d1, d2, bilinearOs);
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

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    DriveFx::Type type = DriveFx::Type::screamer;
    double sampleRate = 48000.0, osRate = 192000.0, osLatency = 0.0;
    double tsPreC = 1.0, bilinearOs = 1.0;
    double sustainGain = 0.0;
    double cleanPrev[2] {};
    int factor = 4, phase = 0;

    static constexpr int kScratch = kControlInterval * 4;
    double scratchU[kScratch], scratchV[kScratch], scratchF[kScratch];

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

    return (int) std::lround (s.pedal.getHalfBandLatency() + PedalDrive::clipperDelay ((Type) type, s.pedal.getFactor()));
}

int DriveFx::latencyFor (int type, double sampleRate) noexcept
{
    type = juce::jlimit (0, numTypes - 1, type);

    if (type == (int) Type::classic)
        return (int) std::lround (kHalfBandLatency2x);

    if (! isPedal (type))
        return 0;

    const int factor = PedalDrive::factorFor (sampleRate);
    return (int) std::lround (PedalDrive::halfBandLatency (factor) + PedalDrive::clipperDelay ((Type) type, factor));
}
} // namespace tubamp
