#include "Drive.h"

#include <algorithm>
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

// V_REF, the same for all three pedals since each takes the guitar straight at its input:
// 0 dBFS = 1 V, so a full strum peaking at -12 dBFS is 0.25 V and quiet picking at -30 dBFS
// is 32 mV, the few hundred millivolts a passive pickup delivers [choice; study R1].
constexpr double kVoltsPerFullScale = 1.0;
constexpr double kInputLimitVolts = 1000.0;   // [choice] keeps any finite input finite downstream
constexpr double kInputHighPassHz = 15.9;     // [circuit: TS-808 input coupling, Yeh DAFx-07]
constexpr float kParamSmoothSeconds = 0.02f;
// [choice] a glide ends within 1e-6 (1 + |target|) of its target: -120 dB of the knob's span.
constexpr double kGlideTolerance = 1.0e-6;
constexpr int kChunk = 32;                    // base samples per pass while the knobs are still
constexpr int kMaxFactor = 8;
constexpr int kHalleySteps = 3;               // see DiodeClipper::evaluateMagnitude
constexpr double kClipStep = 5.0e-4;          // [choice] see PedalDrive::updateDrive

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
// [choice] The knob sets the stage's 1 kHz gain in dB linearly over the pot's whole range,
// 0 dB (knob 0) to 62 dB (knob 36): Rd = Z (G^d - 1), Z = |Zg| at 1 kHz, G = 1 + 100k / Z.
// Through the A taper half the knob was already 42 dB, so from a sixth of its travel up the
// output moved 1.5 dB for 36 dB of input; now the middle is 31 dB, where -30 dBFS (32 mV)
// sits at the diodes' knee and a full strum is clipped hard.
constexpr double kRatKnobHz = 1000.0;

// Big Muff Pi: J.L., "A case study: re-engineering the Big Muff Pi" (2014,
// guitarscience.net/papers/bigmuff.pdf) Fig. 1 and eq. (14): every gain stage is
// K = -Z2 / (Rs + R1), Z2 = RB || CF. Input booster RB11 470k, RG1 39k, CF1 470pF (12.05,
// 720 Hz); clipping stages RB21/RB31 470k, RG2/RG3 10k, CF2/CF3 470pF (47, 720 Hz) with the
// 1N914 pair across RB (through CR 1uF, which settles the limits symmetric, Fig. 8);
// CB2 1uF into RG2 and CO2 1uF into RG3 (15.9 Hz). The sustain pot ("SUSTAIN B100K",
// docs.pedalpcb.com/project/MuffinFuzz-PedalPCB.pdf) is a divider whose wiper feeds RG2,
// so its Thevenin resistance is part of Rs in eq. (14).
constexpr double kMuffFeedbackR = 470.0e3, kMuffFeedbackC = 470.0e-12;
constexpr double kMuffBoostRg = 39.0e3, kMuffStageRg = 10.0e3;
constexpr double kMuffCouplingHz = 1.0 / (kTwoPi * 1.0e-6 * 10.0e3);
constexpr double kMuffSustainPot = 100.0e3;
// The transistors, same paper: RC 47k, RE 100R, base divider RB 470k / 100k, 9 V, beta about
// 200 (§2: "a current gain of around 200 is suitable"), VBE 0.65 V. Eq. (20) then puts every
// collector at 4.00 V with IC = 99.8 uA, so rpi = beta VT / IC = 51.8 k and gm rpi = beta.
constexpr double kMuffRc = 47.0e3, kMuffBeta = 200.0, kMuffIc = 99.8e-6, kMuffVt = 0.02585;
constexpr double kMuffRpi = kMuffBeta * kMuffVt / kMuffIc;
// Eq. (10) without the simplification to K: return ratio T = alpha / (1 + Z2 / RC), alpha =
// beta / (1 + rpi / Rs), and eq. (9) A = K T / (1 + T). With Z2 = RB || CF this stays first
// order: A(s) = alpha RB / Rs / ((1 + alpha + RB / RC) + (1 + alpha) s RB CF). The loading
// takes 1.0 dB (booster) and 2.5 dB (clipping stages) off the ideal gains and lifts their
// 720 Hz poles to 803 and 937 Hz.
double muffAlpha (double rs) noexcept { return kMuffBeta / (1.0 + kMuffRpi / rs); }
// Even harmonics. gm follows IC, and the collector current moves by -vc / RC for a collector
// swing vc, so with the 4.69 V across RC the loop gain falls toward the supply (5.0 V of
// headroom) and rises toward ground (3.8 V): dT / T = (rpi / Rs) / (1 + rpi / Rs) dIC / IC,
// and the closed-loop gain moves by 1 / (1 + T) of that. Per volt of collector swing:
// sigma = (rpi / Rs) / (1 + rpi / Rs) / ((1 + T) IC RC) = 0.0454 / V for a clipping stage
// (Rs = 10k, T = 2.94). Integrated, dvc / du = 1 - sigma vc gives vc - sigma vc^2 / 2 to
// second order; the diodes keep |vc| below a volt, where the next term is negligible.
double muffSigma (double rs) noexcept
{
    const double x = kMuffRpi / rs, t = muffAlpha (rs) / (1.0 + kMuffFeedbackR / kMuffRc);
    return x / (1.0 + x) / ((1.0 + t) * kMuffIc * kMuffRc);
}
// [choice] The knob sets the sustain divider in dB: floor (1 - d)^2, -78 dB at knob 0 to the
// full pot at knob 36 (-44 / -19.5 / -4.9 dB at the quarters). Two clipping stages of 31 dB
// each saturate on anything the first one passes, so the pedal follows the pick only where
// the divider keeps a note below the second stage's knee, near -60 dB for -30 dBFS; the
// floor puts a full strum there at knob 0. The middle has to stay fully saturated, where
// acceptance D14 reads the tone stack's scoop through the clipped stages (THD > 70 % and the
// 900 Hz dip, both at -18 dBFS), which leaves it under 1 dB of dynamics.
constexpr double kMuffSustainFloorDb = -78.0, kMuffSustainCurve = 2.0;
// Tone stack. The paper's values (RT 27k, CT 10nF, CB 4.7nF, RB 27k, 500k pot, its Fig. 14)
// dip only 3.4 dB below the 220 Hz-5 kHz line at 900 Hz with the pot centred. Used here
// instead [choice]: the 1975 "Ram's Head" stack, R8 39k, C8 10nF, C9 4nF, R5 22k (LP 408 Hz,
// HP 1809 Hz, "big scoop of the mids", coda-effects.com/p/big-muff-tonestack-dealing-with-
// mids.html), 100k linear tone pot (muzique.com/lab/dtone.htm), which dips 6.1 dB.
constexpr double kMuffRt = 39.0e3, kMuffCt = 10.0e-9, kMuffCb = 4.0e-9, kMuffRb = 22.0e3;
constexpr double kMuffTonePot = 100.0e3;
constexpr double kMuffToneEnd = 0.005;        // [choice] pot end resistance, as the TS tone
// The tone stack drives the output stage's bias divider, RB41 470k || RB42 100k (Fig. 1),
// through CB4 1 uF: a 1.9 Hz high-pass that takes away the DC the stages' asymmetry makes.
constexpr double kMuffToneLoad = 470.0e3 * 100.0e3 / 570.0e3;
constexpr double kMuffOutputHz = 1.0 / (kTwoPi * 1.0e-6 * kMuffToneLoad);
// The Screamer and the Distortion leave through an output capacitor too, which takes away the
// DC their clippers make from an asymmetric waveform (-38 dBFS from a guitar-like stack without
// it). TS-808: C7 1 uF and R14 1 k into the 100 k Level pot, whose far end sits on the bias rail
// [circuit: cushychicken.github.io/ltspice-tube-screamer, its tube_screamer.asc]: 1.58 Hz.
constexpr double kTsOutputHz = 1.0 / (kTwoPi * 1.0e-6 * (1.0e3 + 100.0e3));
// RAT: 1 uF from the JFET buffer into the 100 k Volume pot [circuit: the Runt2020 BOM above,
// C9 1uF and VOL 100kA, placed as in the RAT's output buffer]: 1.59 Hz. The buffer's own gate
// coupling, 22 nF into 1 M (C12, R8: 7.2 Hz), is left out [choice]: it would take 0.14 dB off
// 40 Hz, and the output stage is otherwise folded into drive_level.
constexpr double kRatOutputHz = 1.0 / (kTwoPi * 1.0e-6 * 100.0e3);

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

/** A parameter glide that arrives. Two one-poles of half the section default (§2.5) in
    series: it rises like the 20 ms one-pole (63 % at 21 ms) without the corner a one-pole
    puts at the start of every move, whose skirt reached -70 dBc above 3 kHz under a low note
    (Screamer tone step, 40-800 Hz noise). Close enough to its target it lands on it and
    reports settled, so the coefficients that follow it stop being recomputed: fx::Smoother in
    float stalls about 3e-5 short for good, and the Treble shelf kept being redesigned (23 %
    more CPU after a gain move). */
class Glide
{
public:
    void prepare (float seconds, double sampleRate, double initial) noexcept
    {
        a = 1.0 - std::exp (-1.0 / juce::jmax (1.0, 0.5 * (double) seconds * sampleRate));
        current = middle = target = initial;
    }

    void setTarget (double value) noexcept { target = value; }
    void snapToTarget() noexcept { current = middle = target; }
    bool isSettled() const noexcept { return juce::exactlyEqual (current, target) && juce::exactlyEqual (middle, target); }

    double next() noexcept
    {
        if (! isSettled())
        {
            middle += a * (target - middle);
            current += a * (middle - current);
            const double tolerance = kGlideTolerance * (1.0 + std::abs (target));

            if (std::abs (target - middle) <= tolerance && std::abs (target - current) <= tolerance)
                current = middle = target;
        }

        return current;
    }

private:
    double a = 1.0, current = 0.0, middle = 0.0, target = 0.0;
};

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

/** Second-order section from (n0 + n1 s + n2 s^2) / (d0 + d1 s + d2 s^2), d2 > 0, bilinear,
    built as a trapezoidal state-variable filter (V. Zavalishin, "The Art of VA Filter
    Design", ch. 4): its states are the two integrators', so retuning it while it runs moves
    the response smoothly. A direct form retuned every sample still throws a transient: a
    Screamer tone step under a 98 Hz note left -54 dBc above 3 kHz. */
struct Svf
{
    void setAnalog (double n0, double n1, double n2, double d0, double d1, double d2, double c) noexcept
    {
        const double w0 = std::sqrt (d0 / d2), k = d1 / std::sqrt (d0 * d2), g = w0 / c;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
        // H = (n0 LP + n1 w0 BP + n2 w0^2 HP) / d0, with HP = x - k BP - LP.
        const double hp = n2 * w0 * w0 / d0;
        m0 = hp;
        m1 = n1 * w0 / d0 - k * hp;
        m2 = n0 / d0 - hp;
    }

    double process (int ch, double x) noexcept
    {
        const double v3 = x - s2[ch];
        const double v1 = a1 * s1[ch] + a2 * v3;
        const double v2 = s2[ch] + a2 * s1[ch] + a3 * v3;
        s1[ch] = 2.0 * v1 - s1[ch];
        s2[ch] = 2.0 * v2 - s2[ch];
        return m0 * x + m1 * v1 + m2 * v2;
    }

    void reset() noexcept { s1[0] = s1[1] = s2[0] = s2[1] = 0.0; }
    void tidyStates() noexcept { tidy (s1[0]); tidy (s1[1]); tidy (s2[0]); tidy (s2[1]); }

    double a1 = 1.0, a2 = 0.0, a3 = 0.0, m0 = 1.0, m1 = 0.0, m2 = 0.0;
    double s1[2] {}, s2[2] {};
};

/** One-pole low-pass as a trapezoidal integrator (same reference, ch. 3), the bilinear image
    of 1 / (1 + s / w); the high-pass is the input minus it. Retunes as smoothly as Svf. */
struct OnePole
{
    /** g = w / c: the integrator gain tan (w / 2 fs) for a bilinear prewarped at w. */
    void setWarped (double g) noexcept { gain = g / (1.0 + g); }

    double lowpass (int ch, double x) noexcept
    {
        const double v = (x - s[ch]) * gain;
        const double y = v + s[ch];
        s[ch] = y + v;
        return y;
    }

    void reset() noexcept { s[0] = s[1] = 0.0; }
    void tidyStates() noexcept { tidy (s[0]); tidy (s[1]); }

    double gain = 0.5;
    double s[2] {};
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

    /** v(u) of one fixed curve below `limit`, with its slope, as the start of the solve: a
        cubic Hermite through 1024 intervals lands within about 1e-8 V of the root, from where
        one Halley step reaches double precision instead of three from the bound. For a curve
        that never changes only (the Distortion's 1 kOhm below its rail, where the solve was
        most of the cost: 3.5 % at the low end of the knob, against a 3.5 % budget). */
    struct Table
    {
        static constexpr int kIntervals = 1024;
        double limit = 0.0, scale = 0.0;
        double v[kIntervals + 1] {}, slope[kIntervals + 1] {};   // slope in volts per interval
    };

    /** Fills `table` for the current resistance (no rail) up to `limit`. Not real-time. */
    void buildTable (Table& table, double limit) noexcept
    {
        const auto* kept = start;
        start = nullptr;
        table.limit = limit;
        table.scale = Table::kIntervals / limit;

        for (int i = 0; i <= Table::kIntervals; ++i)
        {
            Point p;
            evaluateMagnitude (i / table.scale, p);
            table.v[i] = p.v;
            table.slope[i] = 1.0 / (table.scale * (1.0 + kOverA * std::cosh (p.v / kDiodeA)));
        }

        start = kept;
    }

    /** The table must have been built for the resistance this clipper has; nullptr to stop. */
    void useTable (const Table* table) noexcept { start = table; }

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
        not put a step into the next difference quotient. R moves by at most about 1 % between
        two calls, so the old root is within a millivolt of the new one and a single Halley step
        from it lands within 1e-8 V (its error cubes); the full solve, three steps from the
        bound, ran here every base sample while the Screamer's drive moved. */
    void refresh() noexcept
    {
        for (auto& s : state)
        {
            reevaluate (s.p1);
            reevaluate (s.p2);
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
        double v;
        int steps = kHalleySteps;

        if (start != nullptr && u < start->limit)
        {
            const double x = u * start->scale;
            const int i = juce::jmin ((int) x, Table::kIntervals - 1);
            const double t = x - i, t2 = t * t, t3 = t2 * t;
            v = (2.0 * t3 - 3.0 * t2 + 1.0) * start->v[i] + (t3 - 2.0 * t2 + t) * start->slope[i]
              + (3.0 * t2 - 2.0 * t3) * start->v[i + 1] + (t3 - t2) * start->slope[i + 1];
            steps = 1;
        }
        else
        {
            const double y = u / k;
            v = juce::jmin (u, kDiodeA * roughLog (y + std::sqrt (y * y + 1.0)));
        }

        polishMagnitude (u, v, steps, p);
    }

    /** Halley steps from v toward the root for u >= 0, then F1 and F2 there. */
    void polishMagnitude (double u, double v, int steps, Point& p, bool fromOldRoot = false) const noexcept
    {
        double em = 0.0, step = 0.0;

        for (int i = 0; i < steps; ++i)
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

        // exp (v / a) at the final v, from the last one to first order in the last step: exact
        // after a full solve, whose last step is negligible. A single step from an old root is
        // up to 1 % of a, where first order would leave 5e-5 in sinh and cosh.
        if (! fromOldRoot)
            em -= (1.0 + em) * step * (1.0 / kDiodeA);
        else
            em = std::expm1 (v * (1.0 / kDiodeA));

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

    void reevaluate (Point& p) const noexcept
    {
        const double u = p.u, mag = std::abs (u);

        if (rail > 0.0 && mag > rail)
        {
            evaluate (u, p);
            return;
        }

        polishMagnitude (mag, std::abs (p.v), 1, p, true);
        p.u = u;
        p.v = std::copysign (p.v, u);
        p.f2 = std::copysign (p.f2, u);
    }

    struct State { Point p1, p2; double mean1 = 0.0; };

    double k = 1.0e-4, kOverA = 1.0e-4 / kDiodeA;
    double rail = 0.0;
    const Table* start = nullptr;
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

        const std::complex<double> s (0.0, kTwoPi * kRatKnobHz);
        ratKnobOhms = std::abs (1.0 / (s * kRatC1 / (1.0 + s * kRatR1 * kRatC1) + s * kRatC2 / (1.0 + s * kRatR2 * kRatC2)));

        DiodeClipper ratDiodes;
        ratDiodes.setSeriesResistance (kRatClipSeries);
        ratDiodes.buildTable (ratTable, kRatRail);

        drive.prepare (kParamSmoothSeconds, sampleRate, 0.0);
        tone.prepare (kParamSmoothSeconds, sampleRate, 0.5);
        level.prepare (kParamSmoothSeconds, sampleRate, 1.0);

        setType (DriveFx::Type::screamer);
        reset();
    }

    void setType (DriveFx::Type newType) noexcept
    {
        type = newType;
        factor = factorFor (type, sampleRate);
        osRate = sampleRate * factor;
        oversampling = oversamplers[stagesFor (factor)].get();

        clip1.useTable (nullptr);
        clip1.setRail (type == DriveFx::Type::distortion ? kRatRail : 0.0);
        clip1.setSeriesResistance (type == DriveFx::Type::fuzz ? kMuffFeedbackR
                                   : (type == DriveFx::Type::distortion ? kRatClipSeries : kTsR2Fixed));
        clip1.useTable (type == DriveFx::Type::distortion ? &ratTable : nullptr);
        clip2.setSeriesResistance (kMuffFeedbackR);

        tsPreC = warpedC (kTsPreWarpHz, osRate);
        bilinearOs = 2.0 * osRate;

        // Fixed parts of Distortion and Fuzz.
        ratLeg1.setAnalog (0.0, kRatC1, 1.0, kRatR1 * kRatC1, bilinearOs);
        ratLeg2.setAnalog (0.0, kRatC2, 1.0, kRatR2 * kRatC2, bilinearOs);
        setMuffStage (muffBoost, kMuffBoostRg);
        setMuffStage (muffStage2, kMuffStageRg);
        sigma2Half = 0.5 * muffSigma (kMuffStageRg);
        const double cCouple = warpedC (kMuffCouplingHz, osRate);
        muffCouple1.setAnalog (0.0, 1.0 / (kTwoPi * kMuffCouplingHz), 1.0, 1.0 / (kTwoPi * kMuffCouplingHz), cCouple);
        muffCouple2 = muffCouple1;

        const double outHz = type == DriveFx::Type::fuzz ? kMuffOutputHz
                           : (type == DriveFx::Type::distortion ? kRatOutputHz : kTsOutputHz);
        outputCoupling.setAnalog (0.0, 1.0 / (kTwoPi * outHz), 1.0, 1.0 / (kTwoPi * outHz), warpedC (outHz, sampleRate));
        stale = true;
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
        stale = true;
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
        outputCoupling.reset();
        ratFilter.reset();
        toneStage.reset();
        clip1.reset();
        clip2.reset();
        cleanPrev[0] = cleanPrev[1] = 0.0;
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

        switch (type)
        {
            case DriveFx::Type::distortion: runOversampled<DriveFx::Type::distortion> (up, n, channels); break;
            case DriveFx::Type::fuzz:       runOversampled<DriveFx::Type::fuzz> (up, n, channels); break;
            case DriveFx::Type::classic:
            case DriveFx::Type::screamer:
            case DriveFx::Type::boost:
            case DriveFx::Type::treble:     runOversampled<DriveFx::Type::screamer> (up, n, channels); break;
        }

        oversampling->processSamplesDown (block);

        for (int i = 0; i < n; ++i)
        {
            const double gain = level.next() / kVoltsPerFullScale;

            for (int ch = 0; ch < channels; ++ch)
            {
                auto& sample = block.getChannelPointer ((size_t) ch)[i];
                sample = (float) (outputCoupling.process (ch, (double) sample) * gain);
            }
        }

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
        outputCoupling.tidyStates();
        ratFilter.tidyStates();
        toneStage.tidyStates();
    }

private:
    /** Runs the oversampled circuit in passes: the linear filters (cheap, sequential), then
        every clipper solve of the pass (independent of each other), then the ADAA quotients.
        While a knob glides the coefficients follow it every base sample, which is a pass of
        one base sample; held for 32 samples they stepped audibly (-59 dBc under a 98 Hz note).
        Once the glides have landed nothing is recomputed. The tone stage runs here too, at
        N fs: at the base rate its bilinear warping would cost 1.2-1.4 dB at 10 kHz (48 kHz),
        against 0.5 dB allowed by acceptance D8. */
    template <DriveFx::Type T>
    void runOversampled (juce::dsp::AudioBlock<float>& up, int n, int channels) noexcept
    {
        for (int i = 0; i < n;)
        {
            int len = juce::jmin (n - i, kChunk);

            if (stale || ! drive.isSettled() || ! tone.isSettled())
            {
                follow<T>();
                len = 1;
            }

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* data = up.getChannelPointer ((size_t) ch) + i * factor;
                const int count = len * factor;
                circuit<T> (ch, data, count);

                if constexpr (T == DriveFx::Type::distortion)
                    for (int m = 0; m < count; ++m)
                        data[m] = (float) ratFilter.process (ch, (double) data[m]);
                else
                    for (int m = 0; m < count; ++m)
                        data[m] = (float) toneStage.process (ch, (double) data[m]);
            }

            i += len;
        }
    }

    template <DriveFx::Type T>
    void follow() noexcept
    {
        const double d = drive.next(), t = tone.next();

        if (stale || ! juce::exactlyEqual (d, driveNow))
            updateDrive<T> (d);

        if (stale || ! juce::exactlyEqual (t, toneNow))
            updateTone (t);

        driveNow = d;
        toneNow = t;
        stale = false;
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
            // Booster, sustain divider, CB2 into RG2, stage 1. The stages invert, so the model's
            // v is minus the collector swing and vc - sigma vc^2 / 2 becomes v + sigma v^2 / 2.
            for (int m = 0; m < count; ++m)
                scratchU[m] = muffStage1.process (ch, muffCouple2.process (ch, muffBoost.process (ch, (double) data[m]) * sustainGain));

            clip1.solve (scratchU, scratchP, count);

            for (int m = 0; m < count; ++m)
            {
                const double v = clip1.finish (ch, scratchP[m]);
                scratchU[m] = muffStage2.process (ch, muffCouple1.process (ch, v * (1.0 + sigma1Half * v)));
            }

            clip2.solve (scratchU, scratchP, count);

            for (int m = 0; m < count; ++m)
            {
                const double v = clip2.finish (ch, scratchP[m]);
                data[m] = (float) (v * (1.0 + sigma2Half * v));
            }
        }
    }

    /** A Muff gain stage with its transistor's finite loop gain, eq. (9)-(10) (see muffAlpha),
        fed through Rs. */
    void setMuffStage (FirstOrder& stage, double rs) const noexcept
    {
        const double alpha = muffAlpha (rs);
        const double d0 = 1.0 + alpha + kMuffFeedbackR / kMuffRc, d1 = (1.0 + alpha) * kMuffFeedbackR * kMuffFeedbackC;
        stage.setAnalog (alpha * kMuffFeedbackR / rs, 0.0, d0, d1, warpedC (d0 / (kTwoPi * d1), osRate));
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

            // The diodes' curve follows R2 in steps of at most kClipStep of it, and lands on
            // it with the glide: re-solving the clipper's stored points every base sample was
            // most of what a moving drive cost (2.33 % against a 2.0 % budget). A step moves
            // v by a kClipStep at most, 23 uV, -90 dB under the clipped note.
            if (stale || std::abs (r2 - clipR) > kClipStep * clipR || drive.isSettled())
            {
                clip1.setSeriesResistance (r2);
                clip1.refresh();
                clipR = r2;
            }
        }
        else if constexpr (T == DriveFx::Type::distortion)
        {
            // G(s) = 1 + Zf (Y1 + Y2): the legs and Zf share one bilinear constant, so the
            // sum of sections is exactly the bilinear image of G. Zf's pole, and the
            // op-amp's, are kept below Nyquist so neither cancels its zero at z = -1.
            const double rd = ratKnobOhms * (std::pow (1.0 + kRatGainPot / ratKnobOhms, d) - 1.0);
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
            // The divider's wiper drives RG2 through its Thevenin resistance p (1 - p) Rpot,
            // which is part of Rs in eq. (14) and in the loop gain.
            const double p = std::pow (10.0, kMuffSustainFloorDb * std::pow (1.0 - d, kMuffSustainCurve) / 20.0);
            const double rs = kMuffStageRg + p * (1.0 - p) * kMuffSustainPot;
            sustainGain = p;
            setMuffStage (muffStage1, rs);
            sigma1Half = 0.5 * muffSigma (rs);
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
                ratFilter.setAnalog (1.0, 0.0, 1.0, 1.0 / (kTwoPi * hz), warpedC (hz, osRate));
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
                toneStage.setAnalog (gT * (ga * q0 + gb * m), kMuffCb * (ga * gT + ga * m + gb * p0), gb * kMuffCt * kMuffCb,
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
                toneStage.setAnalog (n0, n1, 0.0, d0, d1, d2, bilinearOs);
                break;
            }
        }
    }

    std::unique_ptr<juce::dsp::Oversampling<float>> oversamplers[4];   // by stage count
    juce::dsp::Oversampling<float>* oversampling = nullptr;
    DriveFx::Type type = DriveFx::Type::screamer;
    double sampleRate = 48000.0, osRate = 192000.0;
    double tsPreC = 1.0, bilinearOs = 1.0, ratKnobOhms = 1.0;
    double sustainGain = 0.0, sigma1Half = 0.0, sigma2Half = 0.0;
    double cleanPrev[2] {};
    double driveNow = 0.0, toneNow = 0.0, clipR = 0.0;
    int factor = 4;
    bool stale = true;

    static constexpr int kScratch = kChunk * kMaxFactor;
    double scratchU[kScratch];
    DiodeClipper::Point scratchP[kScratch];

    FirstOrder inputHighPass;
    Svf tsPre;
    FirstOrder ratLeg1, ratLeg2, ratFeedback, ratBandwidth, ratFilter;
    FirstOrder muffBoost, muffStage1, muffStage2, muffCouple1, muffCouple2;
    FirstOrder outputCoupling;   // the type's output capacitor: Fuzz CB4, Screamer C7, Distortion C9
    Svf toneStage;
    DiodeClipper clip1, clip2;
    DiodeClipper::Table ratTable;
    Glide drive, tone, level;
};

//==============================================================================
/** Boost and Treble: linear, base rate, no latency. */
class CleanDrive
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        gain.prepare (kParamSmoothSeconds, sampleRate, 1.0);
        warp.prepare (kParamSmoothSeconds, sampleRate, warpOf (4000.0));
        level.prepare (kParamSmoothSeconds, sampleRate, 1.0);
        reset();
    }

    void setType (DriveFx::Type newType) noexcept
    {
        type = newType;
        stale = true;
    }

    void setTargets (float gainLinear, float hz, float levelGain) noexcept
    {
        gain.setTarget (gainLinear);
        warp.setTarget (warpOf (hz));
        level.setTarget (levelGain);
    }

    void snap() noexcept
    {
        gain.snapToTarget();
        warp.snapToTarget();
        level.snapToTarget();
        stale = true;
    }

    void reset() noexcept
    {
        filter.reset();
        snap();
    }

    void process (juce::dsp::AudioBlock<float> block) noexcept
    {
        const int n = (int) block.getNumSamples();
        const int channels = juce::jmin (2, (int) block.getNumChannels());
        const bool boost = type == DriveFx::Type::boost;

        for (int i = 0; i < n; ++i)
        {
            // While a knob glides the filter follows it every sample (a 32-sample staircase
            // zippers at -77 dBc under a 98 Hz note); once it has landed nothing is redesigned.
            if (stale || ! gain.isSettled() || ! warp.isSettled())
                follow (boost);

            const double out = level.next();

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* data = block.getChannelPointer ((size_t) ch);
                const double x = juce::jlimit (-kInputLimitVolts, kInputLimitVolts, (double) data[i]);
                const double low = filter.lowpass (ch, x);
                // Treble: (1 + s / wz) / (1 + s / wp) = 1 + (G - 1) HP(wp), with the gain
                // outside the filter so a gain move is a smooth multiply.
                data[i] = (float) ((boost ? gainNow * low : x + (gainNow - 1.0) * (x - low)) * out);
            }
        }

        filter.tidyStates();
    }

private:
    void follow (bool boost) noexcept
    {
        const double g = gain.next(), x = warp.next();
        const bool newTone = stale || ! juce::exactlyEqual (x, warpNow);

        // Treble: wz = wc / sqrt G, wp = wc sqrt G: 0 dB below, G above, geometric centre at
        // the knob, bilinear prewarped there (docs/FX-ENGINES.md §4.3.5).
        if (boost ? newTone : (newTone || ! juce::exactlyEqual (g, gainNow)))
            filter.setWarped (boost ? x : x * std::sqrt (g));

        gainNow = g;
        warpNow = x;
        stale = false;
    }

    /** The knob's corner as the bilinear integrator gain tan (pi fc / fs), what the filter takes:
        the tone glides in this form, so a tone move costs no tan() per sample. */
    double warpOf (double hz) const noexcept
    {
        return std::tan (kPi * juce::jlimit (1.0, 0.49 * sampleRate, hz) / sampleRate);
    }

    DriveFx::Type type = DriveFx::Type::boost;
    double sampleRate = 48000.0;
    double gainNow = 1.0, warpNow = 0.0;
    bool stale = true;
    OnePole filter;
    Glide gain, warp, level;
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

/** Replaces every NaN and infinity in the block with 0 and leaves every finite sample
    untouched; true if it found one. The scan is a sum of x * 0, which only a non-finite
    sample makes non-zero, so the clean case is one vectorisable pass. */
bool clearNonFinite (juce::dsp::AudioBlock<float> block) noexcept
{
    bool found = false;

    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
    {
        auto* data = block.getChannelPointer (ch);
        const auto n = block.getNumSamples();
        float probe = 0.0f;

        for (size_t i = 0; i < n; ++i)
            probe += data[i] * 0.0f;

        if (juce::exactlyEqual (probe, 0.0f))
            continue;

        found = true;

        for (size_t i = 0; i < n; ++i)
            if (! std::isfinite (data[i]))
                data[i] = 0.0f;
    }

    return found;
}

//==============================================================================
// Type switches. Both types run and their outputs are crossfaded, so the input itself is never
// heard: fading through it, as fx::EngineSwitcher does, let the guitar through 19 dB above the
// louder type when the level knob sat at -20 dB, and dipped 10 dB at the starting points.
// The incoming type starts from a reset and runs unheard for the settle time (BypassFader's,
// docs/FX-ENGINES.md §2.6), then the fade [choice: §2.4's 20 ms]. The two are lined up at the
// larger of their latencies; a rise in latency is slid in on the outgoing type while the
// incoming one settles, a drop is slid out of the incoming type after the fade.
constexpr double kSwitchSettleSeconds = 0.010;
constexpr double kSwitchFadeSeconds = 0.020;
constexpr double kSwitchSlideSeconds = 0.010;
// Equal-power gains, corrected by the measured powers and correlation so the short-term power
// moves on the straight line in dB between the two types' own (the chorus measures the same
// way, §4.5.4): equal power alone bumps 3 dB where the two outputs are alike and holds where
// they are not, and drive types are anywhere between. In dB because the types can sit 30 dB
// apart (Fuzz to Boost at gain 24): a target linear in power rose faster than the incoming
// gain, and the correction then doubled the outgoing Fuzz's edges. The powers are two cascaded
// one-poles, so a low note's ripple does not modulate the gains; the slack keeps the
// correction finite where two outputs nearly cancel; the floor (-120 dB) keeps the logarithms.
constexpr double kSwitchPowerSeconds = 0.004;
constexpr double kSwitchSlack = 0.1;
constexpr double kSwitchPowerFloor = 1.0e-12;

double smoothstep (double x) noexcept
{
    x = juce::jlimit (0.0, 1.0, x);
    return x * x * (3.0 - 2.0 * x);
}

/** A short delay on one type's output while a switch runs: a whole number of samples lines two
    types of different latency up, and a moving delay (4-point Lagrange, exact at whole samples)
    takes the difference in or out where only one of them is heard. */
class Aligner
{
public:
    static constexpr int kSize = 16;   // > the largest latency difference (5) plus the taps

    void clear() noexcept
    {
        for (auto& channel : ring)
            std::fill (std::begin (channel), std::end (channel), 0.0f);

        write = 0;
    }

    /** In place; delayAt (i) is the delay in samples at sample i of the block. */
    template <typename DelayAt>
    void process (juce::dsp::AudioBlock<float> block, DelayAt&& delayAt) noexcept
    {
        const int n = (int) block.getNumSamples();
        const int channels = juce::jmin (2, (int) block.getNumChannels());

        for (int i = 0; i < n; ++i)
        {
            write = (write + 1) & (kSize - 1);
            const double d = delayAt (i);

            for (int ch = 0; ch < channels; ++ch)
            {
                auto& sample = block.getChannelPointer ((size_t) ch)[i];
                ring[ch][write] = sample;
                sample = read (ch, d);
            }
        }
    }

private:
    float at (int ch, int delay) const noexcept { return ring[ch][(write - delay) & (kSize - 1)]; }

    float read (int ch, double d) const noexcept
    {
        const int whole = (int) d;

        if (juce::exactlyEqual ((double) whole, d))
            return at (ch, whole);

        // Taps whole - 1 ... whole + 2, shifted up at the start where whole - 1 is in the future.
        const int b = juce::jmax (0, whole - 1);
        const double x = d - b;
        const double w0 = -(x - 1.0) * (x - 2.0) * (x - 3.0) * (1.0 / 6.0);
        const double w1 = x * (x - 2.0) * (x - 3.0) * 0.5;
        const double w2 = -x * (x - 1.0) * (x - 3.0) * 0.5;
        const double w3 = x * (x - 1.0) * (x - 2.0) * (1.0 / 6.0);
        return (float) (w0 * at (ch, b) + w1 * at (ch, b + 1) + w2 * at (ch, b + 2) + w3 * at (ch, b + 3));
    }

    float ring[2][kSize] {};
    int write = 0;
};
} // namespace

//==============================================================================
struct DriveFx::Impl
{
    /** One type in use, and which of the two instances of its engine runs it. */
    struct Voice
    {
        int type = (int) Type::screamer;
        int slot = 0;
    };

    enum class Phase { steady, crossfade, slide };

    // The engine instances, numbered: Classic 0, pedal 1 + slot, clean 3 + slot.
    static int engineOf (const Voice& v) noexcept
    {
        if (v.type == (int) Type::classic)
            return 0;

        return (isPedal (v.type) ? 1 : 3) + v.slot;
    }

    static int familyOf (int type) noexcept
    {
        return type == (int) Type::classic ? 0 : (isPedal (type) ? 1 : 2);
    }

    /** An engine that is heard (or about to be) keeps the knobs it had while the requested type
        is not its own: the outgoing type stays where it was, and the knobs that arrive with a
        new type (its starting points) reach the incoming one only. */
    bool holds (int engine) const noexcept
    {
        if (snapEngine)
            return false;

        return (engineOf (active) == engine && active.type != params.type)
            || (phase == Phase::crossfade && engineOf (incoming) == engine && incoming.type != params.type);
    }

    void resetVoice (const Voice& v)
    {
        if (v.type == (int) Type::classic)
        {
            classic.reset();
        }
        else if (isPedal (v.type))
        {
            pedal[v.slot].setType ((Type) v.type);
            pedal[v.slot].reset();
        }
        else
        {
            clean[v.slot].setType ((Type) v.type);
            clean[v.slot].reset();
        }
    }

    void run (const Voice& v, juce::dsp::AudioBlock<float> block)
    {
        if (v.type == (int) Type::classic)
        {
            // The frozen code has no guard of its own: a finite input near FLT_MAX overflows
            // its float half-bands and the infinity would live in them for good. Where its
            // output is finite it is passed on exactly as it is.
            classic.process (block);

            if (clearNonFinite (block))
                classic.reset();
        }
        else if (isPedal (v.type))
            pedal[v.slot].process (block);
        else
            clean[v.slot].process (block);
    }

    void beginSwitch (int channels)
    {
        incoming.type = params.type;
        incoming.slot = familyOf (incoming.type) == familyOf (active.type) ? 1 - active.slot : 0;
        resetVoice (incoming);

        if (channels > other.getNumChannels())
        {
            // More channels than prepare() promised: no room to run two types, so the switch
            // completes at once.
            active = incoming;
            return;
        }

        const int latencyOut = latencyFor (active.type, sampleRate);
        const int latencyIn = latencyFor (incoming.type, sampleRate);
        const int common = juce::jmax (latencyOut, latencyIn);
        extraOut = common - latencyOut;
        extraIn = common - latencyIn;
        alignOut.clear();
        alignIn.clear();
        std::fill (&power[0][0], &power[0][0] + 6, 0.0);
        phase = Phase::crossfade;
        position = 0;
    }

    void processSwitch (juce::dsp::AudioBlock<float> block)
    {
        const int n = (int) block.getNumSamples();

        if (phase == Phase::crossfade && (int) block.getNumChannels() > other.getNumChannels())
        {
            active = incoming;
            phase = Phase::steady;
        }

        for (int offset = 0; offset < n;)
        {
            if (phase == Phase::steady)
            {
                run (active, block.getSubBlock ((size_t) offset));
                break;
            }

            const int left = phase == Phase::crossfade ? settleSamples + fadeSamples - position : slideSamples - position;
            const int length = juce::jmin (n - offset, left);
            auto sub = block.getSubBlock ((size_t) offset, (size_t) length);

            if (phase == Phase::crossfade)
            {
                crossfade (sub);
            }
            else
            {
                run (active, sub);
                const int start = position;
                alignIn.process (sub, [this, start] (int i)
                {
                    return extraIn * (1.0 - smoothstep ((start + i + 1) / (double) slideSamples));
                });
            }

            position += length;
            offset += length;

            if (position >= (phase == Phase::crossfade ? settleSamples + fadeSamples : slideSamples))
            {
                position = 0;

                if (phase == Phase::crossfade)
                {
                    active = incoming;
                    phase = extraIn > 0 ? Phase::slide : Phase::steady;
                }
                else
                {
                    phase = Phase::steady;
                }
            }
        }
    }

    void crossfade (juce::dsp::AudioBlock<float> block)
    {
        const int n = (int) block.getNumSamples();
        const auto channels = block.getNumChannels();
        juce::dsp::AudioBlock<float> in (other.getArrayOfWritePointers(), channels, (size_t) n);
        in.copyFrom (block);

        run (active, block);
        run (incoming, in);

        const int start = position;

        if (extraOut > 0)
        {
            // Slid in while only the outgoing type is heard; it starts a few samples in so the
            // interpolator never reaches back before the switch.
            alignOut.process (block, [this, start] (int i)
            {
                return extraOut * smoothstep ((start + i - 4) / (double) (settleSamples - 4));
            });
        }

        if (extraIn > 0)
            alignIn.process (in, [this] (int) { return (double) extraIn; });

        for (int i = 0; i < n; ++i)
        {
            double a = 0.0, b = 0.0, ab = 0.0;

            for (size_t ch = 0; ch < channels; ++ch)
            {
                const double x = block.getChannelPointer (ch)[i], y = in.getChannelPointer (ch)[i];
                a += x * x;
                b += y * y;
                ab += x * y;
            }

            const double raw[3] { a, b, ab };

            for (int k = 0; k < 3; ++k)
            {
                power[0][k] += powerCoefficient * (raw[k] - power[0][k]);
                power[1][k] += powerCoefficient * (power[0][k] - power[1][k]);
            }

            const double r = smoothstep ((start + i + 1 - settleSamples) / (double) fadeSamples);

            if (r <= 0.0)
                continue;

            if (r >= 1.0)
            {
                for (size_t ch = 0; ch < channels; ++ch)
                    block.getChannelPointer (ch)[i] = in.getChannelPointer (ch)[i];

                continue;
            }

            const double gOut = std::cos (0.5 * kPi * r), gIn = std::sin (0.5 * kPi * r);
            const double powerOut = power[1][0], powerIn = power[1][1], cross = power[1][2];
            const double want = std::exp ((1.0 - r) * std::log (powerOut + kSwitchPowerFloor)
                                          + r * std::log (powerIn + kSwitchPowerFloor));
            const double got = gOut * gOut * powerOut + gIn * gIn * powerIn + 2.0 * gOut * gIn * cross;
            const double slack = kSwitchSlack * want + 1.0e-30;
            const double scale = std::sqrt ((want + slack) / (juce::jmax (0.0, got) + slack));

            for (size_t ch = 0; ch < channels; ++ch)
            {
                auto& out = block.getChannelPointer (ch)[i];
                out = (float) (scale * (gOut * out + gIn * in.getChannelPointer (ch)[i]));
            }
        }
    }

    ClassicDrive classic;
    PedalDrive pedal[2];
    CleanDrive clean[2];
    Voice active, incoming;
    Phase phase = Phase::steady;
    int position = 0, settleSamples = 1, fadeSamples = 1, slideSamples = 1;
    int extraOut = 0, extraIn = 0;
    double powerCoefficient = 1.0;
    double power[2][3] {};   // two smoothing stages of: outgoing power, incoming power, their product
    Aligner alignOut, alignIn;
    juce::AudioBuffer<float> other;   // the incoming type's copy of the input
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

    for (auto& p : s.pedal)
        p.prepare (spec);

    for (auto& c : s.clean)
        c.prepare (spec);

    s.other.setSize (juce::jmax (1, (int) spec.numChannels), s.maxBlock, false, true, false);
    const auto samples = [&s] (double seconds) { return juce::jmax (8, (int) std::lround (seconds * s.sampleRate)); };
    s.settleSamples = samples (kSwitchSettleSeconds);
    s.fadeSamples = samples (kSwitchFadeSeconds);
    s.slideSamples = samples (kSwitchSlideSeconds);
    s.powerCoefficient = 1.0 - std::exp (-1.0 / (kSwitchPowerSeconds * s.sampleRate));
    s.phase = Impl::Phase::steady;
    s.snapEngine = true;
    s.snapParams = true;
}

void DriveFx::reset()
{
    auto& s = *impl;
    s.classic.reset();

    for (auto& p : s.pedal)
        p.reset();

    for (auto& c : s.clean)
        c.reset();

    s.phase = Impl::Phase::steady;
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

    if (! s.holds (0))
        s.classic.setParameters (p.gainDb, p.toneHz, p.levelDb);

    const float levelGain = juce::Decibels::decibelsToGain (p.levelDb);
    const float d = p.gainDb / 36.0f, t = std::log (p.toneHz / 500.0f) / std::log (24.0f);
    const float gainLinear = juce::Decibels::decibelsToGain (p.gainDb);

    for (int slot = 0; slot < 2; ++slot)
    {
        if (! s.holds (1 + slot))
            s.pedal[slot].setTargets (d, t, levelGain);

        if (! s.holds (3 + slot))
            s.clean[slot].setTargets (gainLinear, p.toneHz, levelGain);
    }

    if (s.snapParams)
    {
        s.snapParams = false;

        for (int slot = 0; slot < 2; ++slot)
        {
            s.pedal[slot].snap();
            s.clean[slot].snap();
        }
    }
}

void DriveFx::process (juce::dsp::AudioBlock<float> block)
{
    auto& s = *impl;
    const auto total = block.getNumSamples();

    // Finite samples are not touched: Classic stays bit-identical.
    clearNonFinite (block);

    // A block larger than prepare() promised is run in pieces: the oversamplers and the
    // incoming type's copy of the input are sized for maximumBlockSize.
    for (size_t start = 0; start < total; start += (size_t) s.maxBlock)
    {
        auto piece = block.getSubBlock (start, juce::jmin ((size_t) s.maxBlock, total - start));

        if (s.snapEngine)
        {
            // Nothing has been heard since prepare() or reset(): start on the requested type.
            // Classic is not reset here, so it keeps the frozen code's start-up ramp.
            s.snapEngine = false;
            s.active = { s.params.type, 0 };
            s.phase = Impl::Phase::steady;

            if (isPedal (s.params.type))
                s.pedal[0].setType ((Type) s.params.type);
            else if (s.params.type != (int) Type::classic)
                s.clean[0].setType ((Type) s.params.type);
        }

        // A type asked for while a switch runs waits for it to finish (40 ms at most).
        if (s.phase == Impl::Phase::steady && s.params.type != s.active.type)
            s.beginSwitch ((int) piece.getNumChannels());

        if (s.phase == Impl::Phase::steady)
            s.run (s.active, piece);
        else
            s.processSwitch (piece);
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
