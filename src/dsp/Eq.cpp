#include "Eq.h"

#include <algorithm>
#include <cmath>

namespace tubamp
{
namespace
{
// Classic tone stack, verbatim from the base commit's src/dsp/FxBlocks.cpp:19-23
// (mirroring dsp::tone_stack::BasicNamToneStack).
constexpr float kBassHz = 150.0f, kMidHz = 425.0f, kTrebleHz = 1800.0f;
constexpr float kShelfQ = 0.707f;
constexpr float kMidQCut = 1.5f, kMidQBoost = 0.7f;
constexpr float kBassDbPerUnit = 4.0f, kMidDbPerUnit = 3.0f, kTrebleDbPerUnit = 2.0f;
constexpr float kToneCentre = 5.0f;

// Smoothing, shared by every type (docs/FX-ENGINES.md §2.5, §4.4.3, §4.4.4).
constexpr int kControlInterval = 16;          // [study: R1] coefficients every 16 samples
constexpr double kSmoothSeconds = 0.005;      // [study: R1; ver: V:65] cascaded 5 ms one-poles (two; Classic three)
constexpr double kClassicSettle = 1.0e-4;     // [study: R1] a Classic band stops moving within 1e-4 knob units
constexpr double kHandoverSeconds = 0.01;     // [choice: a Classic glide fades back to the float filter in 10 ms]
constexpr int kGlideKnot = 4;                 // [choice: a Classic glide's design every 4 samples, linear between]
constexpr double kMidBlendDb = 3.0;           // [choice: a Classic mid glide blends Q 1.5 -> 0.7 over +-3 dB]
constexpr double kSwitchSettleSeconds = 0.05; // [choice: an incoming type runs unheard this long before its fade]
constexpr double kSwitchFadeSeconds = 0.04;   // [§4.4.9 E18: the fade between types is 40 ms]
constexpr double kSwitchPowerSeconds = 0.01;  // [choice: two 10 ms one-poles average over a low E's period]
constexpr double kSwitchMaxGain = 2.0;        // [choice: level restoring stops at +6 dB (outputs in antiphase)]
constexpr double kSettle = 1.0e-5;            // [choice: new types settle 10x tighter than Classic, in their own
                                              //  units (knob, dB, octaves): the final snap is a step below -100 dB]
constexpr double kFlushBelow = 1.0e-15;       // [§2.6] recursive double state flushed to 0 once per block
constexpr double kStateLimit = 1.0e12;        // [choice: a state this large can only come from a bad sample; zeroed]
constexpr double kInputLimit = 1.0e6;         // [§2.2] fx::sanitise's bound: larger input is silenced (new types)
constexpr float kAbsurdInput = 1000.0f;       // [choice: +60 dBFS; above it the input is silenced for every type]
constexpr int kChunk = 64;                    // [choice: work-buffer length for the double-precision types]

// A gain control within this of 0 dB is 0 dB, so the section is an exact pass-through. The
// host hands a stepped range's default back with float noise from start + step * k (eq_level and
// the graphic bands read 1.79e-7 dB, eq_tilt 8.94e-8 dB; at most a few ulp of the range end,
// ~1e-6 dB at 12 dB), and the smallest deliberate move is one 0.1 dB step. 1e-3 dB sits a
// thousand times above the first and a hundred times below the second [choice].
constexpr double kNeutralDb = 1.0e-3;

constexpr double kPi = 3.141592653589793238462643383279502884;

// Parametric (§4.4.4).
constexpr double kBassShelfHz = 150.0, kTrebleShelfHz = 1800.0;  // [study: R2] = Classic
constexpr double kShelfQd = 0.707;            // [study: R2] = Classic's shelf Q
constexpr double kBassDb = 4.0, kMidDb = 3.0, kTrebleDb = 2.0;  // [study: R2] dB per knob unit, = Classic
constexpr double kCutK = 1.4142135623730951;  // [study: R2] k = sqrt(2): 12 dB/oct Butterworth
constexpr double kLowCutOffHz = 20.5;         // [§4.4.2] at or below: off
constexpr double kHighCutOffHz = 19999.0;     // [§4.4.2] at or above: off
constexpr double kHighCutMaxFs = 0.45;        // [§4.4.4] high cut at most 0.45 fs
constexpr double kMaxFs = 0.49;               // [§2.2] every frequency at most 0.49 fs before tan()
constexpr double kTiltPivotHz = 700.0;        // [study: R2, own constant] geometric centre of 80 Hz-6 kHz
constexpr double kBellRingFraction = 1.0 / 3.0;  // [choice] the bell gain's poles: a third of its decay time

// Matched bell (see matchedBell): the third match point sits a fraction c of the way from f0 to
// Nyquist, c = 0.75 up to f0 = 0.45 Nyquist, falling to 0.02 at 0.74 Nyquist, never above 20 kHz
// [own fit: minimax error against the analog bell over Q 0.5-8, 22.05-192 kHz].
constexpr double kMatchFrac = 0.75, kMatchMinFrac = 0.02;
constexpr double kMatchKnee = 0.45, kMatchFall = 2.5;
constexpr double kMatchTopHz = 20000.0;

// Graphic (§4.4.6).
constexpr int kGraphicBands = 7;
constexpr double kGraphicHz[kGraphicBands] { 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0 };  // [study: R5]
constexpr double kGraphicQ = 1.4142135623730951;  // [study: R5; Q is the study's own] one-octave half-gain width
constexpr double kGraphicScale = 1.0 / kGraphicQ;  // SvfSection's input scale: 1/Q, as Parametric's bell

// Stack (§4.4.5): Yeh & Smith, "Discretization of the '59 Fender Bassman tone stack", DAFx-06.
struct StackVoicing
{
    double r1, r2, r3, r4, c1, c2, c3;   // treble, bass, mid pots; slope resistor; treble, bass, mid caps
    double makeupDb;                      // minus the mean noon level over 80 Hz-5 kHz (log axis)
};

constexpr StackVoicing kVoicings[EqFx::numVoices] {
    { 250e3, 1e6,   25e3, 56e3,  250e-12, 20e-9,  20e-9, 8.9 },    // Tweed 5F6-A [Yeh & Smith]
    { 250e3, 250e3, 10e3, 100e3, 250e-12, 100e-9, 47e-9, 15.1 },   // Blackface AB763 [robrobinette.com]
    { 220e3, 1e6,   22e3, 33e3,  470e-12, 22e-9,  22e-9, 6.4 },    // British 1959/2203 [caps ToneStack.cc]
    { 1e6,   1e6,   10e3, 100e3, 50e-12,  22e-9,  22e-9, 13.6 },   // Top Boost AC30 [caps ToneStack.cc]
};                                                                 // makeup: [study: design_stack.tsv §3b]

// Audio pot law through (0, 0), (0.5, 0.3), (1, 1): b = (1/0.3 - 1)^2
// [study: design.py:242-246, 261, 264; ver: V:30 the taper applies to mid as well as bass].
constexpr double kTaperBase = (1.0 / 0.3 - 1.0) * (1.0 / 0.3 - 1.0);

// A pot at its end stop is this many ohms, not 0, so the nodal matrix stays regular: the response
// is then within 1e-5 dB of the closed form at the end stops [choice: 1 ohm was 0.005 dB off, 1e-6
// ohm loses precision to the 1e9 spread of conductances].
constexpr double kPotEndOhms = 1.0e-3;

//==============================================================================
/** NaN-proof clamp: setParameters() accepts anything, the engines only ever see finite,
    in-range values. */
float clampOr (float value, float lo, float hi, float fallback) noexcept
{
    return std::isfinite (value) ? juce::jlimit (lo, hi, value) : fallback;
}

double dbToGain (double db) noexcept { return std::pow (10.0, db / 20.0); }

/** Once per block for every recursive double state: denormal-range values to 0, and anything
    non-finite or absurd to 0 too, so a bad value cannot live in a loop [§2.2, §2.6]. */
double flushed (double x) noexcept
{
    const double a = std::abs (x);
    return a >= kFlushBelow && a < kStateLimit ? x : 0.0;   // false for NaN as well
}

/** Input to the double-precision types: NaN, infinities and absurd values become silence. */
double sanitisedInput (float x) noexcept
{
    const auto v = (double) x;
    return std::abs (v) < kInputLimit ? v : 0.0;   // false for NaN as well
}

/** Samples no instrument produces become silence before any type sees them: NaN, infinities
    and anything above kAbsurdInput. Everything else is untouched, which keeps Classic
    bit-identical. Classic's float filters ring on, unclipped, after an absurd finite sample:
    one of 1e30 kept the output above +12 dBFS for 173 ms (r2-tone N11; row E28). */
void replaceAbsurd (juce::dsp::AudioBlock<float>& block) noexcept
{
    const auto n = block.getNumSamples();

    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
    {
        auto* x = block.getChannelPointer (ch);

        for (size_t i = 0; i < n; ++i)
            if (! (std::abs (x[i]) <= kAbsurdInput))   // true for NaN as well
                x[i] = 0.0f;
    }
}

double neutralDb (float db) noexcept { return std::abs ((double) db) < kNeutralDb ? 0.0 : (double) db; }

/** A bass/mid/treble knob whose gain is within kNeutralDb of 0 dB reads as exactly 5. */
double neutralKnob (float knob, double dbPerUnit) noexcept
{
    return std::abs (dbPerUnit * ((double) knob - 5.0)) < kNeutralDb ? 5.0 : (double) knob;
}

/** (1 - a)^n, n a (1 - a)^n and n (n + 1)/2 a^2 (1 - a)^n for n = 0..16: the cascaded one-poles
    advanced n samples in closed form (e1' = r^n e1, e2' = r^n e2 + n a r^n e1,
    e3' = r^n e3 + n a r^n e2 + n (n + 1)/2 a^2 r^n e1, errors from the target). */
struct StepTable
{
    double r[kControlInterval + 1] {}, nar[kControlInterval + 1] {}, tri[kControlInterval + 1] {};

    void prepare (double sampleRate) noexcept
    {
        const double a = 1.0 - std::exp (-1.0 / (kSmoothSeconds * sampleRate));   // [study: R1]
        double rn = 1.0;

        for (int n = 0; n <= kControlInterval; ++n)
        {
            r[n] = rn;
            nar[n] = (double) n * a * rn;
            tri[n] = 0.5 * (double) n * (double) (n + 1) * a * a * rn;
            rn *= 1.0 - a;
        }
    }
};

/** One control through two cascaded one-poles, s1 += a (v - s1); s2 += a (s1 - s2) per
    sample [study: R1; ver: V:65], moved a whole control interval at a time; with `third`, a
    third one-pole after them, so the control's curvature is continuous as well. */
struct TwoPole
{
    double s1 = 0.0, s2 = 0.0, s3 = 0.0, target = 0.0;
    bool moving = false, third = false;

    double value() const noexcept { return third ? s3 : s2; }

    void snap (double v) noexcept
    {
        s1 = s2 = s3 = target = v;
        moving = false;
    }

    void setTarget (double v) noexcept
    {
        if (! (juce::exactlyEqual (v, target) && juce::exactlyEqual (s1, v) && juce::exactlyEqual (s2, v)
               && juce::exactlyEqual (s3, v)))
        {
            target = v;
            moving = true;
        }
    }

    void advance (const StepTable& t, int n) noexcept { advance (t.r[n], t.nar[n], t.tri[n]); }

    /** The same step with the table's three entries for n samples given directly. */
    void advance (double r, double nar, double tri) noexcept
    {
        const double e1 = s1 - target, e2 = s2 - target, e3 = s3 - target;
        s1 = target + r * e1;
        s2 = target + r * e2 + nar * e1;
        s3 = third ? target + r * e3 + nar * e2 + tri * e1 : s2;
    }

    /** Snaps to the target once every stage is within eps of it. */
    void settle (double eps) noexcept
    {
        if (std::abs (s1 - target) <= eps && std::abs (s2 - target) <= eps && std::abs (s3 - target) <= eps)
            snap (target);
    }
};

//==============================================================================
// The double-precision core of the new types: Simper/Zavalishin trapezoidal SVF
// [study: R2, design.py:49-76], with (g, k, m0, m1, m2) designed every 16 samples from the
// smoothed controls and interpolated linearly per sample [study: R1/R2, ours_zipper.tsv].
struct SvfCoeffs
{
    double g = 0.0, k = 1.0, m0 = 1.0, m1 = 0.0, m2 = 0.0;

    bool isIdentity() const noexcept
    {
        return juce::exactlyEqual (m0, 1.0) && juce::exactlyEqual (m1, 0.0) && juce::exactlyEqual (m2, 0.0);
    }
};

double prewarp (double hz, double sampleRate) noexcept
{
    return std::tan (kPi * juce::jmin (hz, kMaxFs * sampleRate) / sampleRate);
}

// With A = 10^(dB/40), t = tan(pi f / fs) [study: R2, §4.4.4 table].
SvfCoeffs lowShelf (double sampleRate, double hz, double q, double db) noexcept
{
    const double A = std::pow (10.0, db / 40.0);
    const double k = 1.0 / q;
    return { prewarp (hz, sampleRate) / std::sqrt (A), k, 1.0, k * (A - 1.0), A * A - 1.0 };
}

SvfCoeffs highShelf (double sampleRate, double hz, double q, double db) noexcept
{
    const double A = std::pow (10.0, db / 40.0);
    const double k = 1.0 / q;
    return { prewarp (hz, sampleRate) * std::sqrt (A), k, A * A, k * (1.0 - A) * A, 1.0 - A * A };
}

SvfCoeffs plainBell (double sampleRate, double hz, double q, double db) noexcept
{
    const double A = std::pow (10.0, db / 40.0);
    const double k = 1.0 / (q * A);
    return { prewarp (hz, sampleRate), k, 1.0, k * (A * A - 1.0), 0.0 };
}

/** A biquad (b0 + b1/z + b2/z^2) / (a0 + a1/z + a2/z^2) as SVF parameters. Through
    s = (z - 1)/(z + 1) it is (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0), so g = sqrt(d0/d2),
    k = d1/(d2 g), m0 = n2/d2, m1 = n1/(d2 g) - m0 k, m2 = n0/d0 - m0. */
SvfCoeffs fromBiquad (double b0, double b1, double b2, double a0, double a1, double a2) noexcept
{
    const double d2 = a0 - a1 + a2, d1 = 2.0 * (a0 - a2), d0 = a0 + a1 + a2;
    const double n2 = b0 - b1 + b2, n1 = 2.0 * (b0 - b2), n0 = b0 + b1 + b2;
    const double g = std::sqrt (d0 / d2);
    const double k = d1 / (d2 * g);
    const double m0 = n2 / d2;
    return { g, k, m0, n1 / (d2 * g) - m0 * k, n0 / d0 - m0 };
}

//==============================================================================
// Classic: the base commit's ToneStackEQ (FxBlocks.cpp:126-193) moved verbatim, plus the
// two additions of §4.4.3: knob smoothing and an exact pass-through at 5/5/5.
class ClassicEq
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        smoothCoeff = 1.0 - std::exp (-1.0 / (kSmoothSeconds * spec.sampleRate));
        handoverSamples = juce::jmax (1, (int) std::lround (kHandoverSeconds * spec.sampleRate));
        bandT[0] = std::tan (kPi * (double) kBassHz / sampleRate);
        bandT[1] = std::tan (kPi * (double) kMidHz / sampleRate);
        bandT[2] = std::tan (kPi * (double) kTrebleHz / sampleRate);

        // Build flat coefficients for the new rate and mark the cached values as flat,
        // so the next setParameters() only rebuilds what actually differs.
        lastBass = lastMid = lastTreble = kToneCentre;
        using AC = juce::dsp::IIR::ArrayCoefficients<float>;
        bass.coeffs   = AC::makeLowShelf   (sampleRate, kBassHz,   kShelfQ,    1.0f);
        mid.coeffs    = AC::makePeakFilter (sampleRate, kMidHz,    kMidQBoost, 1.0f);
        treble.coeffs = AC::makeHighShelf  (sampleRate, kTrebleHz, kShelfQ,    1.0f);

        for (auto& g : glides)
            g = {};

        reset();
    }

    /** Clears the filters and snaps the smoothers on the next setTargets(). The coefficients
        and their cached knob values stay, as in the old class. */
    void reset()
    {
        resetFilters();
        snapOnNextUpdate = true;
        untilCheck = 0;
    }

    void setTargets (float bass010, float mid010, float treble010)
    {
        const float targets[3] { bass010, mid010, treble010 };
        const bool wasMoving = anyMoving();

        for (int band = 0; band < 3; ++band)
        {
            auto& s = knobs[band];

            if (snapOnNextUpdate)
            {
                s.snap ((double) targets[band]);

                if (glides[band].active)
                {
                    // Reset in the middle of a glide: the filter is not on a design of its own.
                    glides[band].active = false;
                    last (band) = targets[band];
                    design (band, targets[band], filter (band).coeffs);
                }
                else
                {
                    designIfChanged (band, targets[band]);
                }
            }
            else if (! s.moving && juce::approximatelyEqual (targets[band], last (band)))
            {
                // Float noise on a knob at rest: the coefficients stay, as in the old class. Any
                // larger change glides: snapping changes under 1e-4 at once, as this used to,
                // made slow automation at small blocks a series of steps that never counted as a
                // move (5.8e-5 per sample for 0 to 10 in 0.9 s at 192 kHz, block 1), and reaching
                // exactly 5/5/5 that way cleared a filter that was not flat: -62.6 dBFS above 1 kHz.
                s.snap ((double) targets[band]);
            }
            else
            {
                s.target = (double) targets[band];
                s.moving = true;
            }
        }

        if (! wasMoving)
            untilCheck = 0;

        snapOnNextUpdate = false;
        flatTargets = juce::exactlyEqual (bass010, kToneCentre) && juce::exactlyEqual (mid010, kToneCentre)
                      && juce::exactlyEqual (treble010, kToneCentre);
    }

    void process (juce::dsp::AudioBlock<float> block)
    {
        if (! anyMoving())
        {
            if (flatTargets)
            {
                // Exact flat: the three cookbook biquads are not a pass-through in float on arm64
                // (fused multiply-add [ver: V:20]), so at exactly 5/5/5 the audio is not touched.
                // Their state at flat is ~0, so clearing it on the way in loses nothing audible.
                if (! bypassed)
                {
                    resetFilters();
                    bypassed = true;
                }

                return;
            }

            // A static block is processed in a single call, as before (snapToZero acts per call).
            bypassed = false;
            runFilters (block);
            return;
        }

        bypassed = false;
        const int n = (int) block.getNumSamples();

        for (int pos = 0; pos < n;)
        {
            if (! anyMoving())
            {
                runFilters (block.getSubBlock ((size_t) pos, (size_t) (n - pos)));
                break;
            }

            if (untilCheck == 0)
                startSegment();

            const int len = juce::jmin (untilCheck, n - pos);
            const bool last16 = len == untilCheck;

            for (int band = 0; band < 3; ++band)
            {
                if (glides[band].active)
                    runGlide (band, block.getSubBlock ((size_t) pos, (size_t) len), last16);
                else
                    runBand (band, block.getSubBlock ((size_t) pos, (size_t) len));
            }

            untilCheck -= len;
            pos += len;

        }

        for (auto& g : glides)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                g.ic1[ch] = flushed (g.ic1[ch]);
                g.ic2[ch] = flushed (g.ic2[ch]);
            }
        }

    }

private:
    using Coeffs = juce::dsp::IIR::Coefficients<float>;

    /** One band as the old ProcessorDuplicator<IIR::Filter<float>> ran it: the statements of
        juce_IIRFilter_Impl.h (processInternal, order 2) in the same order, so the same results
        bit for bit (rows E1, E2), but with the transposed direct form II state in reach, which
        the JUCE class keeps private and a glide has to read and write. */
    struct Biquad
    {
        Coeffs coeffs;
        float lv[2][2] {};   // per channel: lv1, lv2

        void reset() noexcept
        {
            for (auto& s : lv)
                s[0] = s[1] = 0.0f;
        }

        void process (juce::dsp::AudioBlock<float> block) noexcept
        {
            const auto numSamples = block.getNumSamples();
            const auto* c = coeffs.getRawCoefficients();

            for (size_t ch = 0; ch < std::min<size_t> (2, block.getNumChannels()); ++ch)
            {
                auto* src = block.getChannelPointer (ch);
                auto* dst = src;
                auto b0 = c[0];
                auto b1 = c[1];
                auto b2 = c[2];
                auto a1 = c[3];
                auto a2 = c[4];

                auto lv1 = lv[ch][0];
                auto lv2 = lv[ch][1];

                for (size_t i = 0; i < numSamples; ++i)
                {
                    auto input = src[i];
                    auto output = (input * b0) + lv1;
                    dst[i] = output;

                    lv1 = (input * b1) - (output* a1) + lv2;
                    lv2 = (input * b2) - (output* a2);
                }

                juce::dsp::util::snapToZero (lv1); lv[ch][0] = lv1;
                juce::dsp::util::snapToZero (lv2); lv[ch][1] = lv2;
            }
        }
    };

    /** A knob through three cascaded one-poles of 5 ms, advanced per sample. Two (the study's
        sketch) cannot meet -80 dBFS above 1 kHz under a low E: with a filter that follows the
        knob faithfully, bass 10 to 0 leaves -69 dBFS and a mid move across 5 -74 dBFS, the
        spectrum of the smoothing itself (the old direct form only passed the bass because its
        state lags the coefficients). Three leave -98 and -90, with the same 5 ms per pole. */
    struct ThreePole
    {
        double s[3] {}, target = 0.0;
        bool moving = false;

        void snap (double v) noexcept
        {
            s[0] = s[1] = s[2] = target = v;
            moving = false;
        }

        double step (double a) noexcept
        {
            s[0] += a * (target - s[0]);
            s[1] += a * (s[0] - s[1]);
            s[2] += a * (s[1] - s[2]);
            return s[2];
        }

        bool settled() const noexcept
        {
            return std::abs (target - s[0]) <= kClassicSettle && std::abs (target - s[1]) <= kClassicSettle
                   && std::abs (target - s[2]) <= kClassicSettle;
        }
    };

    /** A band while its knob moves. The float direct form cannot follow a moving design: its
        state is a sum of products of the old coefficients, re-weighted by every new set, and
        its designs every 16 samples jitter by ~1 % in low-frequency gain at 192 kHz (a 150 Hz
        shelf's poles sit 2e-5 from z = 1 in float): -68 dBFS above 1 kHz on a treble step under
        a low E at 44.1 kHz, -52 on a mid move across 5, -79.6 on a bass step at 192 kHz. So a
        moving band runs as a double TPT state-variable section (states: integrator outputs),
        designed in closed form every 4 samples from the knob smoothed per sample (P_d: Simper's
        shelves and bell are the cookbook filters exactly). It takes over from the float filter exactly:
        the SVF starts on the float design (P_f) with the state that gives the same free
        response, and the offset P_f - P_d fades out as the knob travels. At the end it runs on
        alongside the float filter, started on design(target) with the free response the SVF
        has, and fades over to it in 10 ms: the two are the same filter but for the float
        design's error, which is never switched in at once. */
    struct Glide
    {
        SvfCoeffs cur, dFrom;           // what the SVF runs now; P_f - P_d where the leg began
        SvfCoeffs from, to;             // the knots cur runs between
        double ic1[2] {}, ic2[2] {};
        double value = kToneCentre;     // the knob value cur stands for
        double legFrom = kToneCentre, legTo = kToneCentre;
        int handLeft = 0;               // samples of the fade to the float filter still to run
        int knotPos = 0;
        bool active = false;
    };

    bool anyMoving() const noexcept { return knobs[0].moving || knobs[1].moving || knobs[2].moving; }

    Biquad& filter (int band) noexcept { return band == 0 ? bass : band == 1 ? mid : treble; }

    float& last (int band) noexcept { return band == 0 ? lastBass : band == 1 ? lastMid : lastTreble; }

    void resetFilters()
    {
        bass.reset();
        mid.reset();
        treble.reset();
    }

    void runFilters (juce::dsp::AudioBlock<float> block)
    {
        bass.process (block);
        mid.process (block);
        treble.process (block);
    }

    void runBand (int band, juce::dsp::AudioBlock<float> block) { filter (band).process (block); }

    /** P_d: the band's design at v in closed form. The frozen mid switches from Q 1.5 to Q 0.7
        at 0 dB: the bell is flat there whatever its Q, but its slope against the knob is not, so
        a glide through it bends the note's level and phase in one sample. Even with the switch
        exactly on the flat point, a mid step across 5 under a low E left up to -72 dBFS above
        1 kHz, depending on the note's phase. A glide therefore blends Q across +-3 dB (mid 4 to
        6) with a smoothstep; outside it, and at rest anywhere, the frozen design holds. */
    SvfCoeffs designDouble (int band, double v) const noexcept
    {
        const double perUnit = band == 0 ? kBassDbPerUnit : band == 1 ? kMidDbPerUnit : kTrebleDbPerUnit;
        const double db = perUnit * (v - (double) kToneCentre);
        const double rootA = std::exp (db * (std::log (10.0) / 80.0)), A = rootA * rootA, t = bandT[band];

        if (band == 1)
        {
            const double u = juce::jlimit (0.0, 1.0, (db + kMidBlendDb) / (2.0 * kMidBlendDb));
            const double q = kMidQCut + (kMidQBoost - kMidQCut) * u * u * (3.0 - 2.0 * u);
            const double k = 1.0 / (q * A);
            return { t, k, 1.0, k * (A * A - 1.0), 0.0 };
        }

        const double k = 1.0 / (double) kShelfQ;

        if (band == 0)
            return { t / rootA, k, 1.0, k * (A - 1.0), A * A - 1.0 };

        return { t * rootA, k, A * A, k * (1.0 - A) * A, 1.0 - A * A };
    }

    /** P_f: a float design as the same filter in SVF form. */
    static SvfCoeffs fromFloat (const Coeffs& c) noexcept
    {
        const float* r = c.getRawCoefficients();
        return fromBiquad (r[0], r[1], r[2], 1.0, r[3], r[4]);
    }

    static SvfCoeffs minus (const SvfCoeffs& a, const SvfCoeffs& b) noexcept
    {
        return { a.g - b.g, a.k - b.k, a.m0 - b.m0, a.m1 - b.m1, a.m2 - b.m2 };
    }

    static SvfCoeffs lerp (const SvfCoeffs& a, const SvfCoeffs& b, double w) noexcept
    {
        const auto l = [w] (double x, double y) { return x + (y - x) * w; };
        return { l (a.g, b.g), l (a.k, b.k), l (a.m0, b.m0), l (a.m1, b.m1), l (a.m2, b.m2) };
    }

    /** base + a + (b - a) w */
    static SvfCoeffs blend (const SvfCoeffs& base, const SvfCoeffs& a, const SvfCoeffs& b, double w) noexcept
    {
        const auto l = [w] (double x, double y) { return x + (y - x) * w; };
        return { base.g + l (a.g, b.g), base.k + l (a.k, b.k), base.m0 + l (a.m0, b.m0), base.m1 + l (a.m1, b.m1),
                 base.m2 + l (a.m2, b.m2) };
    }

    /** A leg runs from the value cur stands for to the knob's target; the offset of cur from
        the closed form there fades out on the way. */
    void startLeg (int band)
    {
        auto& g = glides[band];
        g.legFrom = g.value;
        g.legTo = knobs[band].target;
        g.dFrom = minus (g.cur, designDouble (band, g.legFrom));
    }

    /** The free response over the next two samples, given the SVF state. */
    static void svfFree (const SvfCoeffs& c, double ic1, double ic2, double& y0, double& y1) noexcept
    {
        const double a1 = 1.0 / (1.0 + c.g * (c.g + c.k)), a2 = c.g * a1, a3 = c.g * a2;
        double v1 = a1 * ic1 - a2 * ic2, v2 = ic2 + a2 * ic1 - a3 * ic2;
        y0 = c.m1 * v1 + c.m2 * v2;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        v1 = a1 * ic1 - a2 * ic2;
        v2 = ic2 + a2 * ic1 - a3 * ic2;
        y1 = c.m1 * v1 + c.m2 * v2;
    }

    /** Float direct form -> SVF: the SVF state with the same free response. A flat band has
        none, and starts from zero. */
    void enterGlide (int band)
    {
        auto& g = glides[band];
        auto& f = filter (band);
        const float* c = f.coeffs.getRawCoefficients();
        g.cur = fromFloat (f.coeffs);
        g.value = (double) last (band);

        double F[2][2];   // free response (rows: sample 0, 1) per unit of ic1, ic2
        svfFree (g.cur, 1.0, 0.0, F[0][0], F[1][0]);
        svfFree (g.cur, 0.0, 1.0, F[0][1], F[1][1]);
        const double det = F[0][0] * F[1][1] - F[0][1] * F[1][0];
        const bool ok = det != 0.0 && std::isfinite (det);

        for (int ch = 0; ch < 2; ++ch)
        {
            const double lv1 = f.lv[ch][0], lv2 = f.lv[ch][1];
            const double y0 = lv1, y1 = lv2 - (double) c[3] * lv1;
            g.ic1[ch] = ok ? (F[1][1] * y0 - F[0][1] * y1) / det : 0.0;
            g.ic2[ch] = ok ? (F[0][0] * y1 - F[1][0] * y0) / det : 0.0;
        }

        g.active = true;
        g.knotPos = 0;
        startLeg (band);
    }

    /** Starts the float filter on design(target) with the direct-form state that gives the
        SVF's free response, for the fade over to it. */
    void startHandover (int band)
    {
        auto& g = glides[band];
        auto& f = filter (band);
        design (band, (float) g.value, f.coeffs);
        const double a1 = f.coeffs.getRawCoefficients()[3];

        for (int ch = 0; ch < 2; ++ch)
        {
            double y0, y1;
            svfFree (g.cur, g.ic1[ch], g.ic2[ch], y0, y1);
            f.lv[ch][0] = (float) y0;
            f.lv[ch][1] = (float) (y1 + a1 * y0);
        }

        g.handLeft = handoverSamples;
    }

    // While a band moves, it is checked once per 16 samples: a band whose knob starts moving
    // takes over from its float filter, and one whose smoother has settled hands back to it.
    void startSegment()
    {
        for (int band = 0; band < 3; ++band)
        {
            auto& g = glides[band];

            if (! knobs[band].moving)
                continue;

            if (! g.active)
                enterGlide (band);
            else if (g.handLeft == 0 && ! juce::exactlyEqual (knobs[band].target, g.legTo))
                startLeg (band);
        }

        untilCheck = kControlInterval;
    }

    void runGlide (int band, juce::dsp::AudioBlock<float> block, bool endsSegment)
    {
        auto& g = glides[band];
        auto& s = knobs[band];
        const int n = (int) block.getNumSamples();
        const int channels = (int) std::min<size_t> (2, block.getNumChannels());
        float handed[2][kControlInterval];

        if (g.handLeft > 0)
        {
            // The float filter runs on its own copy of the input; the SVF holds design(target).
            float* ptrs[2] { handed[0], handed[1] };

            for (int ch = 0; ch < channels; ++ch)
                std::copy (block.getChannelPointer ((size_t) ch), block.getChannelPointer ((size_t) ch) + n, handed[ch]);

            filter (band).process (juce::dsp::AudioBlock<float> (ptrs, (size_t) channels, (size_t) n));
        }

        for (int i = 0; i < n; ++i)
        {
            if (g.handLeft == 0)
            {
                if (g.knotPos == 0)
                {
                    // The smoother runs per sample; the design follows it every kGlideKnot
                    // samples, interpolated linearly in between.
                    double v = g.value;

                    for (int k = 0; k < kGlideKnot; ++k)
                        v = s.step (smoothCoeff);

                    const double span = g.legTo - g.legFrom;
                    const double w = std::abs (span) > 1.0e-12 ? juce::jlimit (0.0, 1.0, (v - g.legFrom) / span) : 1.0;
                    g.from = g.cur;
                    g.to = blend (designDouble (band, v), g.dFrom, { 0.0, 0.0, 0.0, 0.0, 0.0 }, w);
                    g.value = v;
                }

                g.knotPos = (g.knotPos + 1) % kGlideKnot;
                g.cur = g.knotPos == 0 ? g.to : lerp (g.from, g.to, (double) g.knotPos / (double) kGlideKnot);
            }

            const auto& c = g.cur;
            const double a1 = 1.0 / (1.0 + c.g * (c.g + c.k)), a2 = c.g * a1, a3 = c.g * a2;
            const double fade = g.handLeft > 0 ? 0.5 - 0.5 * std::cos (kPi * (double) (handoverSamples - g.handLeft + 1)
                                                                        / (double) handoverSamples)
                                               : 0.0;

            for (int ch = 0; ch < channels; ++ch)
            {
                float* x = block.getChannelPointer ((size_t) ch) + i;
                const double in = *x, v3 = in - g.ic2[ch];
                const double v1 = a1 * g.ic1[ch] + a2 * v3, v2 = g.ic2[ch] + a2 * g.ic1[ch] + a3 * v3;
                g.ic1[ch] = 2.0 * v1 - g.ic1[ch];
                g.ic2[ch] = 2.0 * v2 - g.ic2[ch];
                const double y = c.m0 * in + c.m1 * v1 + c.m2 * v2;
                *x = g.handLeft > 0 ? (float) (y + fade * ((double) handed[ch][i] - y)) : (float) y;
            }

            if (g.handLeft > 0 && --g.handLeft == 0)
            {
                // Faded over: the float filter goes on alone, on the rest of this chunk too.
                for (int ch = 0; ch < channels; ++ch)
                    std::copy (handed[ch] + i + 1, handed[ch] + n, block.getChannelPointer ((size_t) ch) + i + 1);

                g.active = false;
                last (band) = (float) g.value;

                if (juce::exactlyEqual (s.target, g.value))
                    s.snap (s.target);

                return;
            }
        }

        // Settled: the SVF holds the target's closed form and the fade to the float filter starts.
        if (endsSegment && g.handLeft == 0 && s.settled() && juce::exactlyEqual (g.legTo, s.target))
        {
            g.value = s.target;
            g.cur = designDouble (band, g.value);
            startHandover (band);
        }
    }

    /** The old class redesigned only when a knob moved by more than float rounding; a static
        session keeps exactly the coefficients it had. */
    void designIfChanged (int band, float value)
    {
        if (! juce::approximatelyEqual (value, last (band)))
        {
            last (band) = value;
            design (band, value, filter (band).coeffs);
        }
    }

    // design (v): the exact expressions of the old ToneStackEQ::setParameters, so a static
    // session gets the old coefficients bit for bit. Coefficient maths allocates a new
    // Coefficients object with the Coefficients<float> factories, so use the
    // allocation-free ArrayCoefficients factories here instead.
    void design (int band, float value, Coeffs& into) const
    {
        using AC = juce::dsp::IIR::ArrayCoefficients<float>;

        if (band == 0)
        {
            const float bass010 = value;
            const float gainDb = kBassDbPerUnit * (bass010 - kToneCentre);
            into = AC::makeLowShelf (
                sampleRate, kBassHz, kShelfQ, juce::Decibels::decibelsToGain (gainDb));
        }
        else if (band == 1)
        {
            const float mid010 = value;
            const float gainDb = kMidDbPerUnit * (mid010 - kToneCentre);
            // Wider when cutting, narrower when boosting — boosting at Q 1.5 sounds honky.
            const float q = gainDb < 0.0f ? kMidQCut : kMidQBoost;
            into = AC::makePeakFilter (
                sampleRate, kMidHz, q, juce::Decibels::decibelsToGain (gainDb));
        }
        else
        {
            const float treble010 = value;
            const float gainDb = kTrebleDbPerUnit * (treble010 - kToneCentre);
            into = AC::makeHighShelf (
                sampleRate, kTrebleHz, kShelfQ, juce::Decibels::decibelsToGain (gainDb));
        }
    }

    Biquad bass, mid, treble;
    double sampleRate = 44100.0;
    float lastBass = 1e9f, lastMid = 1e9f, lastTreble = 1e9f;

    double smoothCoeff = 0.1, bandT[3] {};
    int handoverSamples = 1;
    ThreePole knobs[3];
    Glide glides[3];
    int untilCheck = 0;
    bool snapOnNextUpdate = true, flatTargets = false, bypassed = false;
};

//==============================================================================
/** The bell of Parametric's mid and every Graphic band: the analog prototype
    (s^2 + (A/Q) s + 1) / (s^2 + s/(A Q) + 1) matched after Vicanek, "Matched Second Order
    Digital Filters" (2016), peaking EQ. The poles are the analog poles by impulse invariance;
    the numerator is the minimum-phase one whose magnitude equals the analog curve at DC, at f0
    and at a third frequency fm. Vicanek's own third conditions (zero slope at f0, or the gain at
    Nyquist) leave 1.1 and 1.6 dB at Q 0.5 and 22.05 kHz; fm between f0 and Nyquist (kMatch*)
    keeps every bell from Q 0.5 to 8 within 0.7 dB of the analog curve up to min(20 kHz, 0.45 fs)
    at every rate from 22.05 kHz, where the bilinear bell with Orfanidis decramping above 2.5 kHz
    was 1.9 dB off at 44.1 kHz and 13.8 dB at 22.05 kHz (check_eq E20). A cut is the boost
    turned upside down, which is exact for this prototype. */
SvfCoeffs matchedBell (double sampleRate, double hz, double q, double db) noexcept
{
    const double f0 = juce::jmin (hz, kMaxFs * sampleRate), nyquist = 0.5 * sampleRate;
    const double G = dbToGain (std::abs (db)), A = std::sqrt (G);
    const double w0 = 2.0 * kPi * f0 / sampleRate, zeta = 1.0 / (2.0 * A * q);

    const double decay = std::exp (-zeta * w0);
    const double a1 = zeta <= 1.0 ? -2.0 * decay * std::cos (std::sqrt (1.0 - zeta * zeta) * w0)
                                  : -2.0 * decay * std::cosh (std::sqrt (zeta * zeta - 1.0) * w0);
    const double a2 = decay * decay;

    // |H(w)|^2 = (B0 p0 + B1 p1 + B2 p2) / (A0 p0 + A1 p1 + A2 p2) with p1 = sin^2(w/2),
    // p0 = 1 - p1, p2 = 4 p0 p1; B0 = A0 is the match at DC.
    const double dc = 1.0 + a1 + a2, ny = 1.0 - a1 + a2;
    const double A0 = dc * dc, A1 = ny * ny, A2 = -4.0 * a2;
    const auto phis = [] (double w, double& p0, double& p1, double& p2)
    {
        const double s = std::sin (0.5 * w);
        p1 = s * s;
        p0 = 1.0 - p1;
        p2 = 4.0 * p0 * p1;
    };

    const double c = juce::jlimit (kMatchMinFrac, kMatchFrac, kMatchFrac - kMatchFall * (f0 / nyquist - kMatchKnee));
    const double fm = juce::jmin (f0 + c * (nyquist - f0), juce::jmax (kMatchTopHz, f0 + kMatchMinFrac * (nyquist - f0)));
    const double x = fm / f0, re = 1.0 - x * x, wide = A * x / q, narrow = x / (A * q);
    const double analogFm = (re * re + wide * wide) / (re * re + narrow * narrow);

    double p0, p1, p2, pm0, pm1, pm2;
    phis (w0, p0, p1, p2);
    phis (2.0 * kPi * fm / sampleRate, pm0, pm1, pm2);
    const double r1 = G * G * (A0 * p0 + A1 * p1 + A2 * p2) - A0 * p0;
    const double r2 = analogFm * (A0 * pm0 + A1 * pm1 + A2 * pm2) - A0 * pm0;
    const double det = p1 * pm2 - p2 * pm1;
    const double B1 = (r1 * pm2 - p2 * r2) / det, B2 = (p1 * r2 - pm1 * r1) / det;

    const double rootB1 = std::sqrt (juce::jmax (0.0, B1));
    const double W = 0.5 * (dc + rootB1);
    const double b0 = 0.5 * (W + std::sqrt (juce::jmax (0.0, W * W + B2)));
    const double b1 = 0.5 * (dc - rootB1);
    const double b2 = -B2 / (4.0 * b0);

    SvfCoeffs out = db >= 0.0 ? fromBiquad (b0, b1, b2, 1.0, a1, a2) : fromBiquad (1.0, a1, a2, b0, b1, b2);

    // Never reached over the parameter ranges (checked on a 740 000-design grid, 22.05-192 kHz),
    // but a degenerate design must not reach the audio.
    if (! (std::isfinite (out.g) && std::isfinite (out.k) && std::isfinite (out.m0) && std::isfinite (out.m1)
           && std::isfinite (out.m2) && out.g > 0.0 && out.k > 0.0))
        return plainBell (sampleRate, f0, q, db);

    // At exactly 0 dB the section is an exact pass-through; g and k stay those of the design, so
    // a gain ramp away from 0 dB starts from the same poles.
    if (juce::exactlyEqual (db, 0.0))
        out = { out.g, out.k, 1.0, 0.0, 0.0 };

    return out;
}

/** 12 dB/oct Butterworth cut, crossfaded from the identity by `amount` so the end stops can
    switch it out without a step: high-pass m = (1, -k a, -a), low-pass m = (1 - a, 0, a). */
SvfCoeffs cut (double sampleRate, double hz, bool highPass, double amount) noexcept
{
    if (highPass)
        return { prewarp (hz, sampleRate), kCutK, 1.0, -kCutK * amount, -amount };

    return { prewarp (hz, sampleRate), kCutK, 1.0 - amount, 0.0, amount };
}

//==============================================================================
/** One SVF section, two channels of state. The core is fed s x and mixed with m1/s and m2/s:
    the same filter whatever s is, but s decides what the states hold, and that is what a move
    inherits. Fed x, a bell holds x/k at its resonance (k its damping), so a fast Q move rescaled
    the band-pass term at once while the state lagged: +11.6 dB for Q 8 to 0.5 at +15 dB (row
    E25a). Fed k x it holds x there whatever k is, but off the resonance its states scale with k,
    and a bell's gain moves k too: a Q 8 bell stepped from -15 to +15 dB overshot +5.4 dB under a
    note a sixth of an octave off its centre, and a band moving far above a low note dragged the
    k x its low-pass held, a click of -58 dBFS (r3-engines EQ 1, 2). A bell is fed x/Q: a Q move
    still finds the resonance right (the states hold A x there, A the root of the bell's gain), a
    gain move leaves the state alone, and so does a band far from the note. Shelves and cuts have
    a fixed k and are fed k x. */
class SvfSection
{
public:
    /** Cut filters are skipped (state zeroed) while they are an identity; bells and shelves
        keep their state running so a gain move starts from a warm filter. */
    bool skipsWhenIdentity = false;

    void reset() noexcept
    {
        ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.0;
    }

    /** After reset() only: the state is already clear. */
    void snapTo (const SvfCoeffs& c, double scale) noexcept
    {
        cur = target = c;
        curS = targetS = sA = sB = scale;
        rampLeft = 0;
        scaleMoving = skipped = false;
        finishRamp();
    }

    void rampTo (const SvfCoeffs& c, double scale) noexcept
    {
        target = c;
        targetS = scale;

        if (skipped && c.isIdentity())
        {
            cur = c;
            curS = sA = sB = scale;
            return;
        }

        skipped = false;
        rampLeft = kControlInterval;
        const double s = 1.0 / (double) kControlInterval;
        inc = { (c.g - cur.g) * s, (c.k - cur.k) * s, (c.m0 - cur.m0) * s, (c.m1 - cur.m1) * s, (c.m2 - cur.m2) * s };
        incS = (scale - curS) * s;
        scaleMoving = scaleMoving || ! juce::exactlyEqual (scale, sB);
    }

    bool isRamping() const noexcept { return rampLeft > 0 || scaleMoving; }

    template <int NC>
    void process (double (&buf)[2][kChunk], int n) noexcept
    {
        if (skipped)
            return;

        if (isRamping())
        {
            processRamp<NC> (buf, n);
            return;
        }

        const double a1 = sa1, a2 = sa2, a3 = sa3, s = sB;

        if (identity)
        {
            for (int i = 0; i < n; ++i)
                for (int ch = 0; ch < NC; ++ch)
                    tick (ch, s * buf[ch][i], a1, a2, a3);

            return;
        }

        const double m0 = cur.m0, m1 = sm1, m2 = sm2;

        for (int i = 0; i < n; ++i)
        {
            for (int ch = 0; ch < NC; ++ch)
            {
                const double x = buf[ch][i];
                double v1, v2;
                tick (ch, s * x, a1, a2, a3, v1, v2);
                buf[ch][i] = m0 * x + m1 * v1 + m2 * v2;
            }
        }
    }

    void flush() noexcept
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            ic1[ch] = flushed (ic1[ch]);
            ic2[ch] = flushed (ic2[ch]);
        }
    }

private:
    // The input scale follows its knots through two one-poles of 8 samples. Ramped linearly, it
    // turned a low note into a 16-sample comb of kinks fed straight into the core: a Q step on a
    // 3 kHz bell rang it at -66 dBFS under a 41 Hz note at 44.1 kHz. The mix divides by the same
    // scale every sample, so the filter is the design's whatever the scale does.
    static constexpr double kScaleCoeff = 0.1175030974154046;   // 1 - exp(-1/8)
    static constexpr double kScaleSettle = 1.0e-10;             // relative; then snapped

    void tick (int ch, double u, double a1, double a2, double a3) noexcept
    {
        double v1, v2;
        tick (ch, u, a1, a2, a3, v1, v2);
    }

    void tick (int ch, double u, double a1, double a2, double a3, double& v1, double& v2) noexcept
    {
        const double v3 = u - ic2[ch];
        v1 = a1 * ic1[ch] + a2 * v3;
        v2 = ic2[ch] + a2 * ic1[ch] + a3 * v3;
        ic1[ch] = 2.0 * v1 - ic1[ch];
        ic2[ch] = 2.0 * v2 - ic2[ch];
    }

    template <int NC>
    void processRamp (double (&buf)[2][kChunk], int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            if (rampLeft > 0)
            {
                if (--rampLeft == 0)
                {
                    cur = target;
                    curS = targetS;
                }
                else
                {
                    cur.g += inc.g;
                    cur.k += inc.k;
                    cur.m0 += inc.m0;
                    cur.m1 += inc.m1;
                    cur.m2 += inc.m2;
                    curS += incS;
                }

                computeA (cur, ra1, ra2, ra3);
            }

            if (scaleMoving)
            {
                sA += kScaleCoeff * (curS - sA);
                sB += kScaleCoeff * (sA - sB);
            }

            const double s = sB, inv = 1.0 / s;

            for (int ch = 0; ch < NC; ++ch)
            {
                const double x = buf[ch][i];
                double v1, v2;
                tick (ch, s * x, ra1, ra2, ra3, v1, v2);
                buf[ch][i] = cur.m0 * x + (cur.m1 * v1 + cur.m2 * v2) * inv;
            }
        }

        if (rampLeft == 0 && scaleMoving && std::abs (sA - targetS) <= kScaleSettle * targetS
            && std::abs (sB - targetS) <= kScaleSettle * targetS)
        {
            sA = sB = targetS;
            scaleMoving = false;
        }

        if (rampLeft == 0 && ! scaleMoving)
            finishRamp();
    }

    static void computeA (const SvfCoeffs& c, double& a1, double& a2, double& a3) noexcept
    {
        a1 = 1.0 / (1.0 + c.g * (c.g + c.k));
        a2 = c.g * a1;
        a3 = c.g * a2;
    }

    void finishRamp() noexcept
    {
        computeA (cur, sa1, sa2, sa3);
        sm1 = cur.m1 / sB;
        sm2 = cur.m2 / sB;
        identity = cur.isIdentity();

        if (identity && skipsWhenIdentity && ! skipped)
        {
            skipped = true;
            reset();
        }
    }

    SvfCoeffs cur, target, inc;                  // as designed: the mix is divided by the scale
    double curS = 1.0, targetS = 1.0, incS = 0.0, sA = 1.0, sB = 1.0, sm1 = 0.0, sm2 = 0.0;
    double sa1 = 1.0, sa2 = 0.0, sa3 = 0.0, ra1 = 1.0, ra2 = 0.0, ra3 = 0.0;
    double ic1[2] {}, ic2[2] {};
    int rampLeft = 0;
    bool identity = true, skipped = false, scaleMoving = false;
};

/** First-order tilt around a pivot, T = 10^(tilt/20), gp = tan(pi fp / fs):
    y = ((T + gp) x + (gp - T) x1 - (T gp - 1) y1) / (1 + T gp): -tilt dB at DC, +tilt dB at
    Nyquist, 0 dB at the pivot [study: R2, design.py:325-335]. Skipped at exactly 0 dB. */
class TiltSection
{
public:
    void prepare (double sampleRate) noexcept { gp = std::tan (kPi * kTiltPivotHz / sampleRate); }

    void reset() noexcept
    {
        x1[0] = x1[1] = y1[0] = y1[1] = 0.0;
    }

    /** After reset() only: the state is already clear. */
    void snapTo (double tiltDb) noexcept
    {
        design (tiltDb, cur);
        target = cur;
        rampLeft = 0;
        skipped = false;
        finishRamp (tiltDb);
    }

    void rampTo (double tiltDb) noexcept
    {
        targetDb = tiltDb;
        design (tiltDb, target);

        if (skipped && juce::exactlyEqual (tiltDb, 0.0))
        {
            cur = target;
            return;
        }

        skipped = false;
        rampLeft = kControlInterval;
        const double s = 1.0 / (double) kControlInterval;
        inc = { (target.b0 - cur.b0) * s, (target.b1 - cur.b1) * s, (target.a1 - cur.a1) * s };
    }

    bool isRamping() const noexcept { return rampLeft > 0; }

    template <int NC>
    void process (double (&buf)[2][kChunk], int n) noexcept
    {
        if (skipped)
            return;

        for (int i = 0; i < n; ++i)
        {
            if (rampLeft > 0)
            {
                if (--rampLeft == 0)
                {
                    cur = target;
                }
                else
                {
                    cur.b0 += inc.b0;
                    cur.b1 += inc.b1;
                    cur.a1 += inc.a1;
                }
            }

            for (int ch = 0; ch < NC; ++ch)
            {
                const double x = buf[ch][i];
                const double y = cur.b0 * x + cur.b1 * x1[ch] - cur.a1 * y1[ch];
                x1[ch] = x;
                y1[ch] = y;
                buf[ch][i] = y;
            }
        }

        if (rampLeft == 0)
            finishRamp (targetDb);
    }

    void flush() noexcept
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            x1[ch] = flushed (x1[ch]);
            y1[ch] = flushed (y1[ch]);
        }
    }

private:
    struct Coeffs { double b0 = 1.0, b1 = 0.0, a1 = 0.0; };

    void design (double tiltDb, Coeffs& c) const noexcept
    {
        const double T = dbToGain (tiltDb);
        const double inv = 1.0 / (1.0 + T * gp);
        c = { (T + gp) * inv, (gp - T) * inv, (T * gp - 1.0) * inv };
    }

    void finishRamp (double tiltDb) noexcept
    {
        targetDb = tiltDb;

        if (juce::exactlyEqual (tiltDb, 0.0) && ! skipped)
        {
            skipped = true;
            reset();
        }
    }

    Coeffs cur, target, inc;
    double gp = 0.05, targetDb = 0.0;
    double x1[2] {}, y1[2] {};
    int rampLeft = 0;
    bool skipped = false;
};

/** Output level, interpolated per sample, skipped at exactly 0 dB. */
class LevelStage
{
public:
    void snapTo (double gain) noexcept
    {
        cur = target = gain;
        rampLeft = 0;
    }

    void rampTo (double gain) noexcept
    {
        target = gain;
        rampLeft = kControlInterval;
        inc = (target - cur) / (double) kControlInterval;
    }

    bool isRamping() const noexcept { return rampLeft > 0; }

    template <int NC>
    void process (double (&buf)[2][kChunk], int n) noexcept
    {
        if (rampLeft == 0)
        {
            if (juce::exactlyEqual (cur, 1.0))
                return;

            for (int i = 0; i < n; ++i)
                for (int ch = 0; ch < NC; ++ch)
                    buf[ch][i] *= cur;

            return;
        }

        for (int i = 0; i < n; ++i)
        {
            if (rampLeft > 0)
                cur = --rampLeft == 0 ? target : cur + inc;

            for (int ch = 0; ch < NC; ++ch)
                buf[ch][i] *= cur;
        }
    }

private:
    double cur = 1.0, target = 1.0, inc = 0.0;
    int rampLeft = 0;
};

//==============================================================================
/** Runs a double-precision engine over a float block in chunks that never cross a control
    tick while anything moves, keeping the tick phase across process() calls so the result
    does not depend on the block size. */
template <typename Engine>
class GridRunner
{
public:
    void resetGrid() noexcept { phase = 0; }

    void run (Engine& engine, juce::dsp::AudioBlock<float> block) noexcept
    {
        const auto channels = block.getNumChannels();

        if (channels >= 2)
            runChannels<2> (engine, block);
        else if (channels == 1)
            runChannels<1> (engine, block);

        engine.flush();
    }

private:
    template <int NC>
    void runChannels (Engine& engine, juce::dsp::AudioBlock<float>& block) noexcept
    {
        const int n = (int) block.getNumSamples();
        float* io[2] { block.getChannelPointer (0), NC > 1 ? block.getChannelPointer (1) : nullptr };

        for (int pos = 0; pos < n;)
        {
            int len;

            if (engine.isSettled())
            {
                len = juce::jmin (kChunk, n - pos);
            }
            else
            {
                if (phase == 0)
                    engine.tick();

                len = juce::jmin (kControlInterval - phase, n - pos);
            }

            phase = (phase + len) % kControlInterval;

            for (int ch = 0; ch < NC; ++ch)
                for (int i = 0; i < len; ++i)
                    work[ch][i] = sanitisedInput (io[ch][pos + i]);

            engine.template render<NC> (work, len);

            for (int ch = 0; ch < NC; ++ch)
                for (int i = 0; i < len; ++i)
                    io[ch][pos + i] = (float) work[ch][i];

            pos += len;
        }
    }

    double work[2][kChunk] {};
    int phase = 0;
};

//==============================================================================
// Parametric (§4.4.4): low cut -> bass shelf -> mid bell -> treble shelf -> tilt -> high cut
// -> level [study: R2].
class ParametricEq
{
public:
    void prepare (double rate) noexcept
    {
        sampleRate = rate;
        steps.prepare (rate);
        tilt.prepare (rate);
        sections[lowCut].skipsWhenIdentity = true;
        sections[highCut].skipsWhenIdentity = true;
        // The bell's gain and Q glide through a third one-pole, so their curvature does not jump
        // when a move starts: a Q step rang a 3 kHz bell at -54 dBFS above 1 kHz under a 41 Hz
        // note through two (row E29), and a -15 dB cut moved from Q 0.5 to 8 an octave above a
        // note overshot +1.0 dB (E25a). The mid frequency keeps two, as the other controls.
        controls[cMid].third = true;
        controls[cMidQ].third = true;
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : sections)
            s.reset();

        tilt.reset();
        grid.resetGrid();
        snapOnNextUpdate = true;
    }

    void setTargets (const EqParams& p) noexcept
    {
        double v[numControls];
        v[cBass] = neutralKnob (p.bass, kBassDb);
        v[cMid] = neutralKnob (p.mid, kMidDb);
        v[cTreble] = neutralKnob (p.treble, kTrebleDb);
        v[cMidHz] = std::log2 ((double) p.midFreqHz);
        v[cMidQ] = std::log2 ((double) p.midQ);
        v[cLowHz] = std::log2 ((double) p.lowCutHz);
        v[cLowOn] = (double) p.lowCutHz > kLowCutOffHz ? 1.0 : 0.0;
        v[cHighHz] = std::log2 (juce::jmin ((double) p.highCutHz, kHighCutMaxFs * sampleRate));
        v[cHighOn] = (double) p.highCutHz < kHighCutOffHz ? 1.0 : 0.0;
        v[cTilt] = neutralDb (p.tiltDb);
        v[cLevel] = neutralDb (p.levelDb);

        if (snapOnNextUpdate)
        {
            snapOnNextUpdate = false;

            for (int c = 0; c < numControls; ++c)
                controls[c].snap (v[c]);

            for (int s = 0; s < numSections; ++s)
                snapSection (s);

            tilt.snapTo (controls[cTilt].value());
            level.snapTo (dbToGain (controls[cLevel].value()));
            return;
        }

        for (int c = 0; c < numControls; ++c)
            controls[c].setTarget (v[c]);
    }

    bool isSettled() const noexcept
    {
        for (const auto& c : controls)
            if (c.moving)
                return false;

        for (const auto& s : sections)
            if (s.isRamping())
                return false;

        return ! tilt.isRamping() && ! level.isRamping();
    }

    void tick() noexcept
    {
        bool moved[numControls];

        for (int c = 0; c < numControls; ++c)
        {
            auto& s = controls[c];
            moved[c] = s.moving;

            if (s.moving)
            {
                if (c == cMid)
                    advanceBellGain();
                else
                    s.advance (steps, kControlInterval);

                s.settle (kSettle);
            }
        }

        if (moved[cLowHz] || moved[cLowOn])
            rampSection (lowCut);

        if (moved[cBass])
            rampSection (bassShelf);

        if (moved[cMid] || moved[cMidHz] || moved[cMidQ])
            rampSection (midBell);

        if (moved[cTreble])
            rampSection (trebleShelf);

        if (moved[cHighHz] || moved[cHighOn])
            rampSection (highCut);

        if (moved[cTilt])
            tilt.rampTo (controls[cTilt].value());

        if (moved[cLevel])
            level.rampTo (dbToGain (controls[cLevel].value()));
    }

    template <int NC>
    void render (double (&buf)[2][kChunk], int n) noexcept
    {
        sections[lowCut].process<NC> (buf, n);
        sections[bassShelf].process<NC> (buf, n);
        sections[midBell].process<NC> (buf, n);
        sections[trebleShelf].process<NC> (buf, n);
        tilt.process<NC> (buf, n);
        sections[highCut].process<NC> (buf, n);
        level.process<NC> (buf, n);
    }

    void flush() noexcept
    {
        for (auto& s : sections)
            s.flush();

        tilt.flush();
    }

    void process (juce::dsp::AudioBlock<float> block) noexcept { grid.run (*this, block); }

private:
    enum Control { cBass, cMid, cTreble, cMidHz, cMidQ, cLowHz, cLowOn, cHighHz, cHighOn, cTilt, cLevel, numControls };
    enum Section { lowCut, bassShelf, midBell, trebleShelf, highCut, numSections };

    /** The bell's gain moves no faster than a third of the bell's own decay time, 2 / (k w0) at
        the narrower of where it is and where it goes, and never faster than the other controls.
        Fed x/Q, a narrow bell swept from cut to boost under a note just off its centre still
        rang over its steady level whatever the state held: Q 8 at 100 Hz, -15 to +15 dB, a 92 Hz
        note, +2.0 dB through 5 ms poles, +1.5 dB for a 50 ms ramp, since the boost decays in
        60 ms. At most 20 ms per pole (100 Hz, Q 8, +15 dB): +0.5 dB; a bell at 1 kHz or of Q 2
        and below keeps the 5 ms poles [r3-engines EQ 1; choice: the third]. */
    void advanceBellGain() noexcept
    {
        auto& s = controls[cMid];
        const double hz = std::exp2 (controls[cMidHz].value()), q = std::exp2 (controls[cMidQ].value());
        const auto damping = [q] (double knob) { return std::exp (-kMidDb * (knob - 5.0) * (std::log (10.0) / 40.0)) / q; };
        const double ring = 2.0 / (juce::jmin (damping (s.value()), damping (s.target)) * 2.0 * kPi * hz);
        const double tau = juce::jmax (kSmoothSeconds, kBellRingFraction * ring);
        const double a = 1.0 - std::exp (-1.0 / (tau * sampleRate));
        const double r = std::pow (1.0 - a, (double) kControlInterval), n = (double) kControlInterval;
        s.advance (r, n * a * r, 0.5 * n * (n + 1.0) * a * a * r);
    }

    /** The input scale of SvfSection: 1/Q for the bell, k (fixed) for shelves and cuts. */
    double sectionScale (int s, const SvfCoeffs& c) const noexcept
    {
        return s == midBell ? std::exp2 (-controls[cMidQ].value()) : c.k;
    }

    void snapSection (int s) noexcept
    {
        const auto c = designSection (s);
        sections[s].snapTo (c, sectionScale (s, c));
    }

    void rampSection (int s) noexcept
    {
        const auto c = designSection (s);
        sections[s].rampTo (c, sectionScale (s, c));
    }

    SvfCoeffs designSection (int s) const noexcept
    {
        const auto knobDb = [this] (int c, double dbPerUnit) { return dbPerUnit * (controls[c].value() - 5.0); };

        switch (s)
        {
            case lowCut:      return cut (sampleRate, std::exp2 (controls[cLowHz].value()), true, controls[cLowOn].value());
            case bassShelf:   return lowShelf (sampleRate, kBassShelfHz, kShelfQd, knobDb (cBass, kBassDb));
            case midBell:     return matchedBell (sampleRate, std::exp2 (controls[cMidHz].value()), std::exp2 (controls[cMidQ].value()),
                                           knobDb (cMid, kMidDb));
            case trebleShelf: return highShelf (sampleRate, kTrebleShelfHz, kShelfQd, knobDb (cTreble, kTrebleDb));
            case highCut:     return cut (sampleRate, std::exp2 (controls[cHighHz].value()), false, controls[cHighOn].value());
            default:          break;
        }

        return {};
    }

    double sampleRate = 48000.0;
    StepTable steps;
    TwoPole controls[numControls];
    SvfSection sections[numSections];
    TiltSection tilt;
    LevelStage level;
    GridRunner<ParametricEq> grid;
    bool snapOnNextUpdate = true;
};

//==============================================================================
// Graphic (§4.4.6): seven constant-Q bells in series, then level.
class GraphicEq
{
public:
    void prepare (double rate) noexcept
    {
        sampleRate = rate;
        steps.prepare (rate);

        // A third one-pole as Parametric's bell: g1k6 stepped under a 98 Hz note boosted by the
        // other bands rang -77.9 dBFS above 1 kHz at 44.1 kHz through two, -84.5 through three (E29).
        for (auto& g : gains)
            g.third = true;

        reset();
    }

    void reset() noexcept
    {
        for (auto& s : bands)
            s.reset();

        grid.resetGrid();
        snapOnNextUpdate = true;
    }

    void setTargets (const EqParams& p) noexcept
    {
        if (snapOnNextUpdate)
        {
            snapOnNextUpdate = false;

            for (int b = 0; b < kGraphicBands; ++b)
            {
                gains[b].snap (neutralDb (p.graphicDb[b]));
                bands[b].snapTo (matchedBell (sampleRate, kGraphicHz[b], kGraphicQ, gains[b].value()), kGraphicScale);
            }

            levelDb.snap (neutralDb (p.levelDb));
            level.snapTo (dbToGain (levelDb.value()));
            return;
        }

        for (int b = 0; b < kGraphicBands; ++b)
            gains[b].setTarget (neutralDb (p.graphicDb[b]));

        levelDb.setTarget (neutralDb (p.levelDb));
    }

    bool isSettled() const noexcept
    {
        for (int b = 0; b < kGraphicBands; ++b)
            if (gains[b].moving || bands[b].isRamping())
                return false;

        return ! levelDb.moving && ! level.isRamping();
    }

    void tick() noexcept
    {
        for (int b = 0; b < kGraphicBands; ++b)
        {
            auto& s = gains[b];

            if (s.moving)
            {
                s.advance (steps, kControlInterval);
                s.settle (kSettle);
                bands[b].rampTo (matchedBell (sampleRate, kGraphicHz[b], kGraphicQ, s.value()), kGraphicScale);
            }
        }

        if (levelDb.moving)
        {
            levelDb.advance (steps, kControlInterval);
            levelDb.settle (kSettle);
            level.rampTo (dbToGain (levelDb.value()));
        }
    }

    template <int NC>
    void render (double (&buf)[2][kChunk], int n) noexcept
    {
        for (auto& s : bands)
            s.process<NC> (buf, n);

        level.process<NC> (buf, n);
    }

    void flush() noexcept
    {
        for (auto& s : bands)
            s.flush();
    }

    void process (juce::dsp::AudioBlock<float> block) noexcept { grid.run (*this, block); }

private:
    double sampleRate = 48000.0;
    StepTable steps;
    TwoPole gains[kGraphicBands], levelDb;
    SvfSection bands[kGraphicBands];
    LevelStage level;
    GridRunner<GraphicEq> grid;
    bool snapOnNextUpdate = true;
};

//==============================================================================
// Stack (§4.4.5): the passive tone stack of Yeh & Smith's Fig. 1 as a circuit, its three
// capacitors discretised with the trapezoidal rule (companion conductance 2 C fs). That is the
// bilinear transform (c = 2 fs) of their closed-form H(s) (row E12 checks it against design.py),
// but the state is the capacitor voltages, which a pot move leaves where they are. The transposed
// direct form it replaces kept the polynomial's internal sums: bass 0 to 10 with treble and mid
// at 10 under a 41 Hz note overshot the steady level by +15.5 dB; the circuit gives what the
// circuit gives, +2 dB, the same at 48 and 192 kHz (row E25).
//
// Nodes (design.py nodal_H): A = C1 / treble-pot top, O = treble wiper (output), B = treble-pot
// bottom / C2 / bass-pot top, C = slope resistor / C2 / C3, D = bass-pot bottom / mid-pot top,
// E = mid wiper / C3. Every 16 samples the network is solved for the map from (x, s1, s2, s3)
// to (y, v1, v2, v3), s the capacitors' trapezoidal states in volts (s' = 2 v - s), and the
// map is interpolated linearly per sample.
class StackEq
{
public:
    void prepare (double rate) noexcept
    {
        sampleRate = rate;
        steps.prepare (rate);
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : state)
            s[0] = s[1] = 0.0;

        rampLeft = 0;
        grid.resetGrid();
        snapOnNextUpdate = true;
    }

    void setVoice (int v) noexcept { voice = juce::jlimit (0, EqFx::numVoices - 1, v); }

    void setTargets (const EqParams& p) noexcept
    {
        const double v[3] { (double) p.treble, (double) p.mid, (double) p.bass };

        if (snapOnNextUpdate)
        {
            snapOnNextUpdate = false;

            for (int k = 0; k < 3; ++k)
                knobs[k].snap (v[k]);

            levelDb.snap (neutralDb (p.levelDb));
            design (cur);
            rampLeft = 0;
            level.snapTo (dbToGain (levelDb.value()));
            return;
        }

        for (int k = 0; k < 3; ++k)
            knobs[k].setTarget (v[k]);

        levelDb.setTarget (neutralDb (p.levelDb));
    }

    bool isSettled() const noexcept
    {
        return ! knobs[0].moving && ! knobs[1].moving && ! knobs[2].moving && ! levelDb.moving && ! level.isRamping()
               && rampLeft == 0;
    }

    void tick() noexcept
    {
        if (knobs[0].moving || knobs[1].moving || knobs[2].moving)
        {
            for (auto& k : knobs)
            {
                if (k.moving)
                {
                    k.advance (steps, kControlInterval);
                    k.settle (kSettle);
                }
            }

            design (target);
            rampLeft = kControlInterval;

            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    inc[r][c] = (target[r][c] - cur[r][c]) / (double) kControlInterval;
        }

        if (levelDb.moving)
        {
            levelDb.advance (steps, kControlInterval);
            levelDb.settle (kSettle);
            level.rampTo (dbToGain (levelDb.value()));
        }
    }

    template <int NC>
    void render (double (&buf)[2][kChunk], int n) noexcept
    {
        if (rampLeft > 0)
            renderRamp<NC> (buf, n);
        else
            renderStatic<NC> (buf, n);

        level.process<NC> (buf, n);
    }

    void flush() noexcept
    {
        for (auto& s : state)
        {
            s[0] = flushed (s[0]);
            s[1] = flushed (s[1]);
        }
    }

    void process (juce::dsp::AudioBlock<float> block) noexcept { grid.run (*this, block); }

private:
    // Row 0: y (makeup folded in); rows 1-3: 2 v1, 2 v2, 2 v3. Columns: x, s1, s2, s3.
    using Map = double[4][4];

    template <int NC>
    void renderStatic (double (&buf)[2][kChunk], int n) noexcept
    {
        const Map& m = cur;

        for (int ch = 0; ch < NC; ++ch)
        {
            double s1 = state[0][ch], s2 = state[1][ch], s3 = state[2][ch];
            double* x = buf[ch];

            for (int i = 0; i < n; ++i)
            {
                const double in = x[i];
                const double y = m[0][0] * in + m[0][1] * s1 + m[0][2] * s2 + m[0][3] * s3;
                const double n1 = m[1][0] * in + m[1][1] * s1 + m[1][2] * s2 + m[1][3] * s3 - s1;
                const double n2 = m[2][0] * in + m[2][1] * s1 + m[2][2] * s2 + m[2][3] * s3 - s2;
                const double n3 = m[3][0] * in + m[3][1] * s1 + m[3][2] * s2 + m[3][3] * s3 - s3;
                s1 = n1;
                s2 = n2;
                s3 = n3;
                x[i] = y;
            }

            state[0][ch] = s1;
            state[1][ch] = s2;
            state[2][ch] = s3;
        }
    }

    template <int NC>
    void renderRamp (double (&buf)[2][kChunk], int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            if (rampLeft > 0)
            {
                if (--rampLeft == 0)
                    std::copy (&target[0][0], &target[0][0] + 16, &cur[0][0]);
                else
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c)
                            cur[r][c] += inc[r][c];
            }

            for (int ch = 0; ch < NC; ++ch)
            {
                const double in = buf[ch][i], s1 = state[0][ch], s2 = state[1][ch], s3 = state[2][ch];
                buf[ch][i] = cur[0][0] * in + cur[0][1] * s1 + cur[0][2] * s2 + cur[0][3] * s3;

                for (int r = 1; r < 4; ++r)
                    state[r - 1][ch] = cur[r][0] * in + cur[r][1] * s1 + cur[r][2] * s2 + cur[r][3] * s3 - state[r - 1][ch];
            }
        }
    }

    static double taper (double x) noexcept { return (std::pow (kTaperBase, x) - 1.0) / (kTaperBase - 1.0); }

    /** Nodal analysis of the trapezoidal companion network: each capacitor is a conductance
        G = 2 C fs in parallel with a current source G s; the source is an ideal 1 V. */
    void design (Map& out) const noexcept
    {
        enum { nA, nO, nB, nC, nD, nE, numNodes };
        constexpr int src = -1, gnd = -2;
        const auto& v = kVoicings[voice];
        const double t = knobs[0].value() / 10.0, m = taper (knobs[1].value() / 10.0), l = taper (knobs[2].value() / 10.0);
        const double G[3] { 2.0 * v.c1 * sampleRate, 2.0 * v.c2 * sampleRate, 2.0 * v.c3 * sampleRate };
        const int capP[3] { src, nC, nC }, capQ[3] { nA, nB, nE };

        // Columns of rhs: the 1 V source, then each capacitor's state s (as G s injected into P).
        double Y[numNodes][numNodes] {}, rhs[numNodes][4] {};
        const auto conductance = [&] (int p, int q, double g)
        {
            for (const auto [a, b] : { std::pair { p, q }, std::pair { q, p } })
            {
                if (a < 0)
                    continue;

                Y[a][a] += g;

                if (b >= 0)
                    Y[a][b] -= g;
                else if (b == src)
                    rhs[a][0] += g;
            }
        };
        const auto resistor = [&] (int p, int q, double r) { conductance (p, q, 1.0 / juce::jmax (r, kPotEndOhms)); };

        for (int c = 0; c < 3; ++c)
        {
            conductance (capP[c], capQ[c], G[c]);

            if (capP[c] >= 0)
                rhs[capP[c]][1 + c] += G[c];

            rhs[capQ[c]][1 + c] -= G[c];
        }

        resistor (nA, nO, (1.0 - t) * v.r1);
        resistor (nO, nB, t * v.r1);
        resistor (src, nC, v.r4);
        resistor (nB, nD, l * v.r2);
        resistor (nD, nE, (1.0 - m) * v.r3);
        resistor (nE, gnd, m * v.r3);

        // Y is symmetric positive definite (every node reaches the source or ground), so
        // elimination without pivoting is stable.
        for (int k = 0; k < numNodes; ++k)
        {
            for (int r = k + 1; r < numNodes; ++r)
            {
                const double f = Y[r][k] / Y[k][k];

                for (int c = k; c < numNodes; ++c)
                    Y[r][c] -= f * Y[k][c];

                for (int c = 0; c < 4; ++c)
                    rhs[r][c] -= f * rhs[k][c];
            }
        }

        double node[numNodes][4];

        for (int r = numNodes - 1; r >= 0; --r)
        {
            for (int c = 0; c < 4; ++c)
            {
                double sum = rhs[r][c];

                for (int k = r + 1; k < numNodes; ++k)
                    sum -= Y[r][k] * node[k][c];

                node[r][c] = sum / Y[r][r];
            }
        }

        const double makeup = dbToGain (v.makeupDb);

        for (int c = 0; c < 4; ++c)
        {
            out[0][c] = makeup * node[nO][c];

            for (int k = 0; k < 3; ++k)
            {
                const double vp = capP[k] == src ? (c == 0 ? 1.0 : 0.0) : node[capP[k]][c];
                out[1 + k][c] = 2.0 * (vp - node[capQ[k]][c]);
            }
        }
    }

    double sampleRate = 48000.0;
    int voice = (int) EqFx::Voice::british;
    StepTable steps;
    TwoPole knobs[3], levelDb;   // knobs: treble, mid, bass
    LevelStage level;
    Map cur {}, target {}, inc {};
    double state[3][2] {};
    int rampLeft = 0;
    GridRunner<StackEq> grid;
    bool snapOnNextUpdate = true;
};

/** The switch key: Stack's voice is a second choice inside the type (§2.4). */
int switchKey (int type, int voice) noexcept
{
    return type * 16 + (type == (int) EqFx::Type::stack ? voice : 0);
}
} // namespace

//==============================================================================
// A type or voice change crossfades the outgoing engine's output into the incoming one's, both
// running. The incoming engine starts from reset on the 16-sample grid (so the switch does not
// depend on the host block size) and is fed, unheard, for kSwitchSettleSeconds, then the fade
// runs over kSwitchFadeSeconds with a raised cosine, which has no corner at either end. Fading
// through the dry input instead, with linear 20 ms ramps, ticked at -54.8 dBFS above 1 kHz and
// dipped 9.1 dB below both types' levels under a strong low boost (r2-tone N4; row E27).
struct EqFx::Impl
{
    enum Slot { classicSlot, parametricSlot, stackSlot0, stackSlot1, graphicSlot };

    ClassicEq classic;
    ParametricEq parametric;
    StackEq stacks[2];   // a voice change runs two Stacks at once
    GraphicEq graphic;
    EqParams params;
    int activeKey = 0, activeSlot = parametricSlot, incomingKey = 0, incomingSlot = parametricSlot;
    int settleLeft = 0, fadePos = 0, settleSamples = kControlInterval, fadeSamples = 1, gridPhase = 0;
    int turnPos = 0, turnLen = 0;           // turnLen > 0: the fade has turned round (see turnFade)
    double fadeW = 0.0, fadeStep = 0.0;     // the incoming weight now, and its last per-sample change
    double turnW = 0.0, turnV = 0.0, turnTo = 0.0;
    bool switching = false, snapEngine = true;
    float incomingInput[2][kControlInterval] {};
    double power[3][2] {}, powerCoeff = 0.1;   // <o o>, <i i>, <o i>, each through two one-poles

    static int slotFor (int key, int busySlot) noexcept
    {
        switch ((Type) (key / 16))
        {
            case Type::classic:    return classicSlot;
            case Type::parametric: return parametricSlot;
            case Type::stack:      return busySlot == stackSlot0 ? stackSlot1 : stackSlot0;
            case Type::graphic:    return graphicSlot;
        }

        return parametricSlot;
    }

    void resetSlot (int slot, int key) noexcept
    {
        switch (slot)
        {
            case classicSlot:    classic.reset(); break;
            case parametricSlot: parametric.reset(); break;
            case stackSlot0:
            case stackSlot1:     stacks[slot - stackSlot0].reset(); stacks[slot - stackSlot0].setVoice (key % 16); break;
            case graphicSlot:    graphic.reset(); break;
            default:             break;
        }
    }

    void runSlot (int slot, juce::dsp::AudioBlock<float> block)
    {
        switch (slot)
        {
            case classicSlot:
                classic.setTargets (params.bass, params.mid, params.treble);
                classic.process (block);
                break;

            case parametricSlot:
                parametric.setTargets (params);
                parametric.process (block);
                break;

            case stackSlot0:
            case stackSlot1:
                stacks[slot - stackSlot0].setTargets (params);
                stacks[slot - stackSlot0].process (block);
                break;

            case graphicSlot:
                graphic.setTargets (params);
                graphic.process (block);
                break;

            default:
                break;
        }
    }

    /** On the grid only: starts, retargets, reverses or cancels a switch. */
    void follow (int requested) noexcept
    {
        if (! switching)
        {
            if (requested != activeKey)
            {
                switching = true;
                startIncoming (requested);
            }

            return;
        }

        const bool goingBack = turnLen > 0 && juce::exactlyEqual (turnTo, 0.0);

        if (requested == incomingKey)
        {
            if (goingBack)
                turnFade (1.0);

            return;
        }

        if (requested == activeKey)
        {
            if (settleLeft > 0)
            {
                // Asked back before the incoming engine was heard: nothing to undo.
                switching = false;
                return;
            }

            // Asked back during the fade: both engines run, so the fade turns round where it is.
            if (! goingBack)
                turnFade (0.0);

            return;
        }

        // A third choice: an unheard incoming engine is replaced; a fade in progress finishes
        // first, and the next grid point starts a switch from its result.
        if (settleLeft > 0)
            startIncoming (requested);
    }

    void startIncoming (int key) noexcept
    {
        incomingKey = key;
        incomingSlot = slotFor (key, activeSlot);
        resetSlot (incomingSlot, key);
        settleLeft = settleSamples;
        fadePos = turnPos = turnLen = 0;
        fadeW = fadeStep = 0.0;

        for (auto& p : power)
            p[0] = p[1] = 0.0;
    }

    /** Turns a running fade towards `to` (0: back to the active engine, 1: on to the incoming
        one). Reversing the raised cosine's direction in place left a corner in the weight, a
        click of -45.6 dBFS above 1 kHz (r3-engines EQ 3); the weight now leaves on a cubic
        Hermite from where it is with the speed it has, and arrives at rest after one fade time. */
    void turnFade (double to) noexcept
    {
        turnW = fadeW;
        turnV = fadeStep * (double) fadeSamples;
        turnTo = to;
        turnPos = 0;
        turnLen = fadeSamples;
    }

    double nextWeight() noexcept
    {
        if (turnLen == 0)
            return 0.5 - 0.5 * std::cos (kPi * (double) ++fadePos / (double) fadeSamples);

        const double t = (double) ++turnPos / (double) turnLen, t2 = t * t, t3 = t2 * t;
        const double w = (2.0 * t3 - 3.0 * t2 + 1.0) * turnW + (t3 - 2.0 * t2 + t) * turnV + (3.0 * t2 - 2.0 * t3) * turnTo;
        return juce::jlimit (0.0, 1.0, w);
    }

    /** One piece of a switch, never across a grid point. The two outputs are mixed with raised-
        cosine weights scaled by G = a_t / a_mix, where a_t is the same blend of their RMS levels
        and a_mix the RMS their blend has, from running estimates of <o o>, <i i> and <o i>. G is
        1 for two outputs in phase and restores the level when they are not: Classic to
        Parametric on a guitar-like note under strong settings dipped 2.7 dB without it. */
    void runSwitch (juce::dsp::AudioBlock<float> block)
    {
        const int n = (int) block.getNumSamples();
        const int channels = (int) std::min<size_t> (2, block.getNumChannels());
        float* ptrs[2] { incomingInput[0], incomingInput[1] };

        for (int ch = 0; ch < channels; ++ch)
            std::copy (block.getChannelPointer ((size_t) ch), block.getChannelPointer ((size_t) ch) + n, incomingInput[ch]);

        runSlot (activeSlot, block);
        runSlot (incomingSlot, juce::dsp::AudioBlock<float> (ptrs, (size_t) channels, (size_t) n));

        for (int i = 0; i < n; ++i)
        {
            double oo = 0.0, ii = 0.0, oi = 0.0;

            for (int ch = 0; ch < channels; ++ch)
            {
                const double o = block.getChannelPointer ((size_t) ch)[i], x = incomingInput[ch][i];
                oo += o * o;
                ii += x * x;
                oi += o * x;
            }

            const double now[3] { oo, ii, oi };

            for (int k = 0; k < 3; ++k)
            {
                power[k][0] += powerCoeff * (now[k] - power[k][0]);
                power[k][1] += powerCoeff * (power[k][0] - power[k][1]);
            }

            if (settleLeft > 0)
                continue;

            const double w = nextWeight();
            fadeStep = w - fadeW;
            fadeW = w;
            const double po = juce::jmax (0.0, power[0][1]), pi = juce::jmax (0.0, power[1][1]), poi = power[2][1];
            const double mix = (1.0 - w) * (1.0 - w) * po + w * w * pi + 2.0 * w * (1.0 - w) * poi;
            const double target = (1.0 - w) * std::sqrt (po) + w * std::sqrt (pi);
            const double gain = mix > 1.0e-24 ? juce::jlimit (1.0, kSwitchMaxGain, target / std::sqrt (mix)) : 1.0;

            for (int ch = 0; ch < channels; ++ch)
            {
                float* out = block.getChannelPointer ((size_t) ch) + i;
                *out = (float) (gain * ((1.0 - w) * (double) *out + w * (double) incomingInput[ch][i]));
            }

            if (turnLen > 0 && turnPos == turnLen && juce::exactlyEqual (turnTo, 0.0))
            {
                // Back on the active engine: the rest of the block already holds its output.
                switching = false;
                return;
            }

            if (turnLen > 0 ? turnPos == turnLen : fadePos == fadeSamples)
            {
                for (int ch = 0; ch < channels; ++ch)
                    std::copy (incomingInput[ch] + i + 1, incomingInput[ch] + n, block.getChannelPointer ((size_t) ch) + i + 1);

                activeKey = incomingKey;
                activeSlot = incomingSlot;
                switching = false;
                return;
            }
        }

        settleLeft = juce::jmax (0, settleLeft - n);
    }
};

EqFx::EqFx() : impl (std::make_unique<Impl>()) {}
EqFx::~EqFx() = default;

void EqFx::prepare (const juce::dsp::ProcessSpec& spec)
{
    auto& s = *impl;
    s.classic.prepare (spec);
    s.parametric.prepare (spec.sampleRate);
    s.stacks[0].prepare (spec.sampleRate);
    s.stacks[1].prepare (spec.sampleRate);
    s.graphic.prepare (spec.sampleRate);
    s.settleSamples = kControlInterval * (int) std::ceil (kSwitchSettleSeconds * spec.sampleRate / kControlInterval);
    s.fadeSamples = juce::jmax (1, (int) std::lround (kSwitchFadeSeconds * spec.sampleRate));
    s.powerCoeff = 1.0 - std::exp (-1.0 / (kSwitchPowerSeconds * spec.sampleRate));
    s.snapEngine = true;
}

void EqFx::reset()
{
    auto& s = *impl;
    s.classic.reset();
    s.parametric.reset();
    s.stacks[0].reset();
    s.stacks[1].reset();
    s.graphic.reset();
    // Every engine is clean, so the next block starts on the requested type outright
    // rather than finishing a switch that was in flight.
    s.snapEngine = true;
}

void EqFx::setParameters (const EqParams& params)
{
    auto& p = impl->params;
    p.type = juce::jlimit (0, numTypes - 1, params.type);
    p.voice = juce::jlimit (0, numVoices - 1, params.voice);
    p.bass = clampOr (params.bass, 0.0f, 10.0f, 5.0f);
    p.mid = clampOr (params.mid, 0.0f, 10.0f, 5.0f);
    p.treble = clampOr (params.treble, 0.0f, 10.0f, 5.0f);
    p.lowCutHz = clampOr (params.lowCutHz, 20.0f, 400.0f, 20.0f);
    p.highCutHz = clampOr (params.highCutHz, 2000.0f, 20000.0f, 20000.0f);
    p.midFreqHz = clampOr (params.midFreqHz, 100.0f, 8000.0f, 425.0f);
    p.midQ = clampOr (params.midQ, 0.5f, 8.0f, 0.7f);
    p.tiltDb = clampOr (params.tiltDb, -6.0f, 6.0f, 0.0f);
    p.levelDb = clampOr (params.levelDb, -12.0f, 12.0f, 0.0f);

    for (int b = 0; b < kGraphicBands; ++b)
        p.graphicDb[b] = clampOr (params.graphicDb[b], -12.0f, 12.0f, 0.0f);
}

void EqFx::process (juce::dsp::AudioBlock<float> block)
{
    auto& s = *impl;
    const int requested = switchKey (s.params.type, s.params.voice);
    const int n = (int) block.getNumSamples();
    replaceAbsurd (block);

    if (s.snapEngine)
    {
        // Nothing has been heard since prepare() or reset(): start on the requested type.
        s.snapEngine = false;
        s.switching = false;
        s.activeKey = requested;
        s.activeSlot = Impl::slotFor (requested, -1);
        s.resetSlot (s.activeSlot, requested);
        s.gridPhase = 0;
    }

    if (! s.switching && requested == s.activeKey)
    {
        // Steady: one call per block, as Classic always had.
        s.runSlot (s.activeSlot, block);
        s.gridPhase = (s.gridPhase + n) % kControlInterval;
        return;
    }

    for (int pos = 0; pos < n;)
    {
        if (s.gridPhase == 0)
            s.follow (requested);

        const int len = juce::jmin (kControlInterval - s.gridPhase, n - pos);
        const auto piece = block.getSubBlock ((size_t) pos, (size_t) len);

        if (s.switching)
            s.runSwitch (piece);
        else
            s.runSlot (s.activeSlot, piece);

        s.gridPhase = (s.gridPhase + len) % kControlInterval;
        pos += len;
    }
}
} // namespace tubamp
