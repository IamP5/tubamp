#include "Eq.h"

#include "FxPrimitives.h"

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
constexpr double kSmoothSeconds = 0.005;      // [study: R1; ver: V:65] two cascaded 5 ms one-poles
constexpr double kClassicSettle = 1.0e-4;     // [study: R1] a Classic band stops moving within 1e-4 knob units
constexpr double kSettle = 1.0e-5;            // [choice: new types settle 10x tighter than Classic, in their own
                                              //  units (knob, dB, octaves): the final snap is a step below -100 dB]
constexpr double kFlushBelow = 1.0e-15;       // [§2.6] recursive double state flushed to 0 once per block
constexpr int kChunk = 64;                    // [choice: work-buffer length for the double-precision types]

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
constexpr double kBlendLowHz = 1500.0;        // [study: R3, blend width tagged guess] plain bell below
constexpr double kBlendHighHz = 2500.0;       // [study: R3] Orfanidis bell above
constexpr double kOrfanidisMaxDw = 0.98 * kPi;  // [§4.4.4, design.py:126-139] bandwidth guard

// Graphic (§4.4.6).
constexpr int kGraphicBands = 7;
constexpr double kGraphicHz[kGraphicBands] { 100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0 };  // [study: R5]
constexpr double kGraphicQ = 1.4142135623730951;  // [study: R5; Q is the study's own] one-octave half-gain width

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

//==============================================================================
/** NaN-proof clamp: setParameters() accepts anything, the engines only ever see finite,
    in-range values. */
float clampOr (float value, float lo, float hi, float fallback) noexcept
{
    return std::isfinite (value) ? juce::jlimit (lo, hi, value) : fallback;
}

double dbToGain (double db) noexcept { return std::pow (10.0, db / 20.0); }

double flushed (double x) noexcept { return std::abs (x) < kFlushBelow ? 0.0 : x; }

/** (1 - a)^n and n a (1 - a)^n for n = 0..16: the two cascaded one-poles advanced n
    samples in closed form (e1' = r^n e1, e2' = r^n e2 + n a r^n e1, errors from the target). */
struct StepTable
{
    double r[kControlInterval + 1] {}, nar[kControlInterval + 1] {};

    void prepare (double sampleRate) noexcept
    {
        const double a = 1.0 - std::exp (-1.0 / (kSmoothSeconds * sampleRate));   // [study: R1]
        double rn = 1.0;

        for (int n = 0; n <= kControlInterval; ++n)
        {
            r[n] = rn;
            nar[n] = (double) n * a * rn;
            rn *= 1.0 - a;
        }
    }
};

/** One control through two cascaded one-poles, s1 += a (v - s1); s2 += a (s1 - s2) per
    sample [study: R1; ver: V:65], moved a whole control interval at a time. */
struct TwoPole
{
    double s1 = 0.0, s2 = 0.0, target = 0.0;
    bool moving = false;

    void snap (double v) noexcept
    {
        s1 = s2 = target = v;
        moving = false;
    }

    void setTarget (double v) noexcept
    {
        if (! (juce::exactlyEqual (v, target) && juce::exactlyEqual (s1, v) && juce::exactlyEqual (s2, v)))
        {
            target = v;
            moving = true;
        }
    }

    void advance (const StepTable& t, int n) noexcept
    {
        const double e1 = s1 - target, e2 = s2 - target;
        s1 = target + t.r[n] * e1;
        s2 = target + t.r[n] * e2 + t.nar[n] * e1;
    }

    /** Snaps to the target once both stages are within eps of it. */
    void settle (double eps) noexcept
    {
        if (std::abs (s1 - target) <= eps && std::abs (s2 - target) <= eps)
            snap (target);
    }
};

//==============================================================================
// Classic: the base commit's ToneStackEQ (FxBlocks.cpp:126-193) moved verbatim, plus the
// two additions of §4.4.3: knob smoothing and an exact pass-through at 5/5/5.
class ClassicEq
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        steps.prepare (spec.sampleRate);

        bass.prepare (spec);
        mid.prepare (spec);
        treble.prepare (spec);

        // Build flat coefficients for the new rate and mark the cached values as flat,
        // so the next setParameters() only rebuilds what actually differs.
        lastBass = lastMid = lastTreble = kToneCentre;
        using AC = juce::dsp::IIR::ArrayCoefficients<float>;
        *bass.state   = AC::makeLowShelf   (sampleRate, kBassHz,   kShelfQ,    1.0f);
        *mid.state    = AC::makePeakFilter (sampleRate, kMidHz,    kMidQBoost, 1.0f);
        *treble.state = AC::makeHighShelf  (sampleRate, kTrebleHz, kShelfQ,    1.0f);

        // Gives the ramp targets their storage now, so designing into them never allocates.
        for (int band = 0; band < 3; ++band)
            design (band, kToneCentre, next[band]);

        for (auto& r : ramps)
            r = {};

        reset();
    }

    /** Clears the filters and snaps the smoothers on the next setTargets(). The coefficients
        and their cached knob values stay, as in the old class. */
    void reset()
    {
        bass.reset();
        mid.reset();
        treble.reset();
        snapOnNextUpdate = true;
        untilDesign = 0;
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
                s.snap (targets[band]);

                if (ramps[band].active)
                {
                    // Reset in the middle of a ramp: the live coefficients are in between.
                    ramps[band].active = false;
                    last (band) = targets[band];
                    design (band, targets[band], *filter (band).state);
                }
                else
                {
                    designIfChanged (band, targets[band]);
                }
            }
            else if (! s.moving && std::abs ((double) targets[band] - s.s2) <= kClassicSettle)
            {
                s.snap (targets[band]);
                designIfChanged (band, targets[band]);
            }
            else
            {
                s.target = targets[band];
                s.moving = true;
            }
        }

        if (! wasMoving)
            untilDesign = 0;

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
                    bass.reset();
                    mid.reset();
                    treble.reset();
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

            if (untilDesign == 0)
                startSegment();

            const int len = juce::jmin (untilDesign, n - pos);
            const int done = kControlInterval - untilDesign;

            for (int band = 0; band < 3; ++band)
            {
                if (ramps[band].active)
                    runRamp (band, block, pos, len, done);
                else
                    runBand (band, block.getSubBlock ((size_t) pos, (size_t) len));
            }

            untilDesign -= len;
            pos += len;

            if (untilDesign == 0)
                endSegment();
        }
    }

private:
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
                                                  juce::dsp::IIR::Coefficients<float>>;
    using Coeffs = juce::dsp::IIR::Coefficients<float>;

    /** A band's coefficients moving from `from` to `next` over one 16-sample segment. */
    struct Ramp
    {
        float from[5] {};
        float value = kToneCentre;
        bool active = false, settles = false;
    };

    bool anyMoving() const noexcept { return knobs[0].moving || knobs[1].moving || knobs[2].moving; }

    Filter& filter (int band) noexcept { return band == 0 ? bass : band == 1 ? mid : treble; }

    float& last (int band) noexcept { return band == 0 ? lastBass : band == 1 ? lastMid : lastTreble; }

    void runFilters (juce::dsp::AudioBlock<float> block)
    {
        juce::dsp::ProcessContextReplacing<float> context (block);
        bass.process (context);
        mid.process (context);
        treble.process (context);
    }

    void runBand (int band, juce::dsp::AudioBlock<float> block)
    {
        juce::dsp::ProcessContextReplacing<float> context (block);
        filter (band).process (context);
    }

    // While a band moves, design (s2) is computed once per 16 samples, one segment ahead,
    // and the five coefficients are interpolated linearly in between, one sample at a time
    // through the band's own filters. Stepping them every 16 samples, as §4.4.3 sketches,
    // leaves -43 dBFS clicks on a treble move under a low note (the direct-form b0 path
    // steps the output); per-sample interpolation brings that to -80 dBFS.
    void startSegment()
    {
        for (int band = 0; band < 3; ++band)
        {
            auto& s = knobs[band];
            auto& r = ramps[band];

            if (! s.moving)
                continue;

            s.advance (steps, kControlInterval);
            r.settles = std::abs (s.target - s.s1) <= kClassicSettle && std::abs (s.target - s.s2) <= kClassicSettle;
            r.value = r.settles ? (float) s.target : (float) s.s2;
            design (band, r.value, next[band]);

            const float* live = filter (band).state->getRawCoefficients();
            std::copy (live, live + 5, r.from);
            r.active = true;
        }

        untilDesign = kControlInterval;
    }

    void runRamp (int band, juce::dsp::AudioBlock<float> block, int pos, int len, int done)
    {
        auto& r = ramps[band];
        float* live = filter (band).state->getRawCoefficients();
        const float* to = next[band].getRawCoefficients();

        for (int i = 0; i < len; ++i)
        {
            const int k = done + i + 1;

            if (k == kControlInterval)
            {
                std::copy (to, to + 5, live);
            }
            else
            {
                const float w = (float) k / (float) kControlInterval;

                for (int c = 0; c < 5; ++c)
                    live[c] = r.from[c] + (to[c] - r.from[c]) * w;
            }

            runBand (band, block.getSubBlock ((size_t) (pos + i), 1));
        }
    }

    void endSegment()
    {
        for (int band = 0; band < 3; ++band)
        {
            auto& r = ramps[band];

            if (! r.active)
                continue;

            r.active = false;
            last (band) = r.value;

            if (r.settles)
                knobs[band].snap (knobs[band].target);
        }
    }

    /** The old class redesigned only when a knob moved by more than float rounding; a static
        session keeps exactly the coefficients it had. */
    void designIfChanged (int band, float value)
    {
        if (! juce::approximatelyEqual (value, last (band)))
        {
            last (band) = value;
            design (band, value, *filter (band).state);
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

    Filter bass, mid, treble;
    double sampleRate = 44100.0;
    float lastBass = 1e9f, lastMid = 1e9f, lastTreble = 1e9f;

    StepTable steps;
    TwoPole knobs[3];
    Coeffs next[3];
    Ramp ramps[3];
    int untilDesign = 0;
    bool snapOnNextUpdate = true, flatTargets = false, bypassed = false;
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

    /** a + w (b - a): identical sets blend to themselves exactly, so a 0 dB band stays (1, 0, 0). */
    static SvfCoeffs blend (const SvfCoeffs& a, const SvfCoeffs& b, double w) noexcept
    {
        return { a.g + w * (b.g - a.g), a.k + w * (b.k - a.k), a.m0 + w * (b.m0 - a.m0),
                 a.m1 + w * (b.m1 - a.m1), a.m2 + w * (b.m2 - a.m2) };
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

/** Orfanidis' prescribed-Nyquist-gain bell (G1 = the analog gain at Nyquist, Vicanek
    eq. 17 there [ver: V:31]) as SVF parameters: in the bilinear s-plane it is
    (G1 s^2 + B s + W2) / (s^2 + A s + W2), so g = sqrt(W2), k = A/g, m = (G1, (B - G1 A)/g,
    1 - G1) [study: R3, design.py:126-139]. */
SvfCoeffs orfanidisBell (double sampleRate, double hz, double q, double db) noexcept
{
    const double f = juce::jmin (hz, kMaxFs * sampleRate);

    if (std::abs (db) < 1.0e-6)
        return { prewarp (f, sampleRate), 1.0 / q, 1.0, 0.0, 0.0 };

    const double G = dbToGain (db), GB = std::sqrt (G);
    const double G2 = G * G, GB2 = GB * GB;
    const double w0 = 2.0 * kPi * f / sampleRate, Dw = w0 / q;
    const double F = std::abs (G2 - GB2), G00 = std::abs (G2 - 1.0), F00 = std::abs (GB2 - 1.0);
    const double p = w0 * w0 - kPi * kPi;
    const double bw = F00 * kPi * kPi * Dw * Dw / F;
    const double G1 = std::sqrt ((p * p + G2 * bw) / (p * p + bw));
    const double G01 = std::abs (G2 - G1), G11 = juce::jmax (1.0e-300, std::abs (G2 - G1 * G1));
    const double F01 = std::abs (GB2 - G1), F11 = juce::jmax (1.0e-300, std::abs (GB2 - G1 * G1));
    const double tw = std::tan (w0 / 2.0);
    const double W2 = std::sqrt (G11 / G00) * tw * tw;
    const double DW = (1.0 + std::sqrt (F00 / F11) * W2) * std::tan (juce::jmin (Dw, kOrfanidisMaxDw) / 2.0);
    const double C = F11 * DW * DW - 2.0 * W2 * (F01 - std::sqrt (F00 * F11));
    const double D = 2.0 * W2 * (G01 - std::sqrt (G00 * G11));
    const double A = std::sqrt ((C + D) / F), B = std::sqrt ((G2 * C + GB2 * D) / F);
    const double g = std::sqrt (W2);
    const SvfCoeffs c { g, A / g, G1, (B - G1 * A) / g, 1.0 - G1 };

    // Never reached over the parameter ranges (checked on a 290 000-design grid, 22.05-192 kHz),
    // but a degenerate design must not reach the audio.
    if (! (std::isfinite (c.g) && std::isfinite (c.k) && std::isfinite (c.m0) && std::isfinite (c.m1)
           && std::isfinite (c.m2) && c.g > 0.0 && c.k > 0.0))
        return plainBell (sampleRate, f, q, db);

    return c;
}

/** The bell used by Parametric's mid and every Graphic band: plain below 1.5 kHz, Orfanidis
    above 2.5 kHz, the two parameter sets crossfaded linearly in f between [study: R3]. */
SvfCoeffs bell (double sampleRate, double hz, double q, double db) noexcept
{
    if (hz <= kBlendLowHz)
        return plainBell (sampleRate, hz, q, db);

    if (hz >= kBlendHighHz)
        return orfanidisBell (sampleRate, hz, q, db);

    return SvfCoeffs::blend (plainBell (sampleRate, hz, q, db), orfanidisBell (sampleRate, hz, q, db),
                             (hz - kBlendLowHz) / (kBlendHighHz - kBlendLowHz));
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
/** One SVF section, two channels of state. */
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
    void snapTo (const SvfCoeffs& c) noexcept
    {
        cur = target = c;
        rampLeft = 0;
        skipped = false;
        finishRamp();
    }

    void rampTo (const SvfCoeffs& c) noexcept
    {
        target = c;

        if (skipped && c.isIdentity())
        {
            cur = c;
            return;
        }

        skipped = false;
        rampLeft = kControlInterval;
        const double s = 1.0 / (double) kControlInterval;
        inc = { (c.g - cur.g) * s, (c.k - cur.k) * s, (c.m0 - cur.m0) * s, (c.m1 - cur.m1) * s, (c.m2 - cur.m2) * s };
    }

    bool isRamping() const noexcept { return rampLeft > 0; }

    template <int NC>
    void process (double (&buf)[2][kChunk], int n) noexcept
    {
        if (skipped)
            return;

        if (rampLeft > 0)
        {
            processRamp<NC> (buf, n);
            return;
        }

        const double a1 = sa1, a2 = sa2, a3 = sa3;

        if (identity)
        {
            for (int i = 0; i < n; ++i)
                for (int ch = 0; ch < NC; ++ch)
                    tick (ch, buf[ch][i], a1, a2, a3);

            return;
        }

        const double m0 = cur.m0, m1 = cur.m1, m2 = cur.m2;

        for (int i = 0; i < n; ++i)
        {
            for (int ch = 0; ch < NC; ++ch)
            {
                const double x = buf[ch][i];
                double v1, v2;
                tick (ch, x, a1, a2, a3, v1, v2);
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
    void tick (int ch, double x, double a1, double a2, double a3) noexcept
    {
        double v1, v2;
        tick (ch, x, a1, a2, a3, v1, v2);
    }

    void tick (int ch, double x, double a1, double a2, double a3, double& v1, double& v2) noexcept
    {
        const double v3 = x - ic2[ch];
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
                }
                else
                {
                    cur.g += inc.g;
                    cur.k += inc.k;
                    cur.m0 += inc.m0;
                    cur.m1 += inc.m1;
                    cur.m2 += inc.m2;
                }

                computeA (cur, ra1, ra2, ra3);
            }

            for (int ch = 0; ch < NC; ++ch)
            {
                const double x = buf[ch][i];
                double v1, v2;
                tick (ch, x, ra1, ra2, ra3, v1, v2);
                buf[ch][i] = cur.m0 * x + cur.m1 * v1 + cur.m2 * v2;
            }
        }

        if (rampLeft == 0)
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
        identity = cur.isIdentity();

        if (identity && skipsWhenIdentity && ! skipped)
        {
            skipped = true;
            reset();
        }
    }

    SvfCoeffs cur, target, inc;
    double sa1 = 1.0, sa2 = 0.0, sa3 = 0.0, ra1 = 1.0, ra2 = 0.0, ra3 = 0.0;
    double ic1[2] {}, ic2[2] {};
    int rampLeft = 0;
    bool identity = true, skipped = false;
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
        y1[0] = flushed (y1[0]);
        y1[1] = flushed (y1[1]);
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
                    work[ch][i] = (double) io[ch][pos + i];

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
        v[cBass] = p.bass;
        v[cMid] = p.mid;
        v[cTreble] = p.treble;
        v[cMidHz] = std::log2 ((double) p.midFreqHz);
        v[cMidQ] = std::log2 ((double) p.midQ);
        v[cLowHz] = std::log2 ((double) p.lowCutHz);
        v[cLowOn] = p.lowCutHz > kLowCutOffHz ? 1.0 : 0.0;
        v[cHighHz] = std::log2 (juce::jmin ((double) p.highCutHz, kHighCutMaxFs * sampleRate));
        v[cHighOn] = p.highCutHz < kHighCutOffHz ? 1.0 : 0.0;
        v[cTilt] = p.tiltDb;
        v[cLevel] = p.levelDb;

        if (snapOnNextUpdate)
        {
            snapOnNextUpdate = false;

            for (int c = 0; c < numControls; ++c)
                controls[c].snap (v[c]);

            for (int s = 0; s < numSections; ++s)
                sections[s].snapTo (designSection (s));

            tilt.snapTo (controls[cTilt].s2);
            level.snapTo (dbToGain (controls[cLevel].s2));
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
                s.advance (steps, kControlInterval);
                s.settle (kSettle);
            }
        }

        if (moved[cLowHz] || moved[cLowOn])
            sections[lowCut].rampTo (designSection (lowCut));

        if (moved[cBass])
            sections[bassShelf].rampTo (designSection (bassShelf));

        if (moved[cMid] || moved[cMidHz] || moved[cMidQ])
            sections[midBell].rampTo (designSection (midBell));

        if (moved[cTreble])
            sections[trebleShelf].rampTo (designSection (trebleShelf));

        if (moved[cHighHz] || moved[cHighOn])
            sections[highCut].rampTo (designSection (highCut));

        if (moved[cTilt])
            tilt.rampTo (controls[cTilt].s2);

        if (moved[cLevel])
            level.rampTo (dbToGain (controls[cLevel].s2));
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

    SvfCoeffs designSection (int s) const noexcept
    {
        const auto knobDb = [this] (int c, double dbPerUnit) { return dbPerUnit * (controls[c].s2 - 5.0); };

        switch (s)
        {
            case lowCut:      return cut (sampleRate, std::exp2 (controls[cLowHz].s2), true, controls[cLowOn].s2);
            case bassShelf:   return lowShelf (sampleRate, kBassShelfHz, kShelfQd, knobDb (cBass, kBassDb));
            case midBell:     return bell (sampleRate, std::exp2 (controls[cMidHz].s2), std::exp2 (controls[cMidQ].s2),
                                           knobDb (cMid, kMidDb));
            case trebleShelf: return highShelf (sampleRate, kTrebleShelfHz, kShelfQd, knobDb (cTreble, kTrebleDb));
            case highCut:     return cut (sampleRate, std::exp2 (controls[cHighHz].s2), false, controls[cHighOn].s2);
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
                gains[b].snap (p.graphicDb[b]);
                bands[b].snapTo (bell (sampleRate, kGraphicHz[b], kGraphicQ, gains[b].s2));
            }

            levelDb.snap (p.levelDb);
            level.snapTo (dbToGain (levelDb.s2));
            return;
        }

        for (int b = 0; b < kGraphicBands; ++b)
            gains[b].setTarget (p.graphicDb[b]);

        levelDb.setTarget (p.levelDb);
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
                bands[b].rampTo (bell (sampleRate, kGraphicHz[b], kGraphicQ, s.s2));
            }
        }

        if (levelDb.moving)
        {
            levelDb.advance (steps, kControlInterval);
            levelDb.settle (kSettle);
            level.rampTo (dbToGain (levelDb.s2));
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
// Stack (§4.4.5): the Yeh-Smith third-order passive tone stack, bilinear (c = 2 fs), one
// transposed direct form II section in double. Coefficients are designed every 16 samples
// from the smoothed knobs, one segment ahead, and interpolated linearly per sample: stepped
// every 16 samples (as §4.4.5 has it) a treble or mid step under a low note clicks at
// -40 to -50 dBFS through the direct b0 path; interpolated it stays below -80 dBFS.
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
        for (auto& z : state)
            z[0] = z[1] = 0.0;

        rampLeft = 0;
        grid.resetGrid();
        snapOnNextUpdate = true;
    }

    void setVoice (int v) noexcept { voice = juce::jlimit (0, EqFx::numVoices - 1, v); }

    void setTargets (const EqParams& p) noexcept
    {
        const double v[3] { p.treble, p.mid, p.bass };

        if (snapOnNextUpdate)
        {
            snapOnNextUpdate = false;

            for (int k = 0; k < 3; ++k)
                knobs[k].snap (v[k]);

            levelDb.snap (p.levelDb);
            design (cur);
            rampLeft = 0;
            level.snapTo (dbToGain (levelDb.s2));
            return;
        }

        for (int k = 0; k < 3; ++k)
            knobs[k].setTarget (v[k]);

        levelDb.setTarget (p.levelDb);
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

            for (int c = 0; c < 7; ++c)
                inc[c] = (target[c] - cur[c]) / (double) kControlInterval;
        }

        if (levelDb.moving)
        {
            levelDb.advance (steps, kControlInterval);
            levelDb.settle (kSettle);
            level.rampTo (dbToGain (levelDb.s2));
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
        for (auto& z : state)
        {
            z[0] = flushed (z[0]);
            z[1] = flushed (z[1]);
        }
    }

    void process (juce::dsp::AudioBlock<float> block) noexcept { grid.run (*this, block); }

private:
    // Coefficients: b0, b1, b2, b3, a1, a2, a3 (a0 = 1), makeup folded into the b's.
    using Coeffs = double[7];

    template <int NC>
    void renderStatic (double (&buf)[2][kChunk], int n) noexcept
    {
        const double b0 = cur[0], b1 = cur[1], b2 = cur[2], b3 = cur[3], a1 = cur[4], a2 = cur[5], a3 = cur[6];

        for (int ch = 0; ch < NC; ++ch)
        {
            double z1 = state[0][ch], z2 = state[1][ch], z3 = state[2][ch];
            double* x = buf[ch];

            for (int i = 0; i < n; ++i)
            {
                const double in = x[i];
                const double y = b0 * in + z1;
                z1 = b1 * in - a1 * y + z2;
                z2 = b2 * in - a2 * y + z3;
                z3 = b3 * in - a3 * y;
                x[i] = y;
            }

            state[0][ch] = z1;
            state[1][ch] = z2;
            state[2][ch] = z3;
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
                    std::copy (target, target + 7, cur);
                else
                    for (int c = 0; c < 7; ++c)
                        cur[c] += inc[c];
            }

            for (int ch = 0; ch < NC; ++ch)
            {
                const double in = buf[ch][i];
                const double y = cur[0] * in + state[0][ch];
                state[0][ch] = cur[1] * in - cur[4] * y + state[1][ch];
                state[1][ch] = cur[2] * in - cur[5] * y + state[2][ch];
                state[2][ch] = cur[3] * in - cur[6] * y;
                buf[ch][i] = y;
            }
        }
    }

    static double taper (double x) noexcept { return (std::pow (kTaperBase, x) - 1.0) / (kTaperBase - 1.0); }

    /** Yeh & Smith eqs. 1-2, transcribed from design.py:174-192 (yeh_coeffs, checked against
        an independent nodal solve to 0.000000 dB [study: design_stack.tsv]), then the
        bilinear transform of design.py:234-239. */
    void design (Coeffs& out) const noexcept
    {
        const auto& v = kVoicings[voice];
        const double t = knobs[0].s2 / 10.0, m = taper (knobs[1].s2 / 10.0), l = taper (knobs[2].s2 / 10.0);
        const double R1 = v.r1, R2 = v.r2, R3 = v.r3, R4 = v.r4, C1 = v.c1, C2 = v.c2, C3 = v.c3;

        const double b1 = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
        const double b2 = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                        + m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                        + l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4)
                        + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3)
                        + (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
        const double b3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4)
                        - m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4)
                        + m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4)
                        + t * C1 * C2 * C3 * R1 * R3 * R4 - t * m * C1 * C2 * C3 * R1 * R3 * R4
                        + t * l * C1 * C2 * C3 * R1 * R2 * R4;
        const double a0 = 1.0;
        const double a1 = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
        const double a2 = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                        + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3)
                        - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                        + l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4)
                        + (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3
                           + C1 * C3 * R3 * R4 + C2 * C3 * R3 * R4);
        const double a3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4)
                        - m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4)
                        + m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3 - C1 * C2 * C3 * R1 * R3 * R4)
                        + l * C1 * C2 * C3 * R1 * R2 * R4 + C1 * C2 * C3 * R1 * R3 * R4;

        const double c = 2.0 * sampleRate, c2 = c * c, c3 = c2 * c;
        const double B0 = -b1 * c - b2 * c2 - b3 * c3;
        const double B1 = -b1 * c + b2 * c2 + 3.0 * b3 * c3;
        const double B2 = b1 * c + b2 * c2 - 3.0 * b3 * c3;
        const double B3 = b1 * c - b2 * c2 + b3 * c3;
        const double A0 = -a0 - a1 * c - a2 * c2 - a3 * c3;
        const double A1 = -3.0 * a0 - a1 * c + a2 * c2 + 3.0 * a3 * c3;
        const double A2 = -3.0 * a0 + a1 * c + a2 * c2 - 3.0 * a3 * c3;
        const double A3 = -a0 + a1 * c - a2 * c2 + a3 * c3;

        const double gain = dbToGain (v.makeupDb) / A0;
        out[0] = B0 * gain;
        out[1] = B1 * gain;
        out[2] = B2 * gain;
        out[3] = B3 * gain;
        out[4] = A1 / A0;
        out[5] = A2 / A0;
        out[6] = A3 / A0;
    }

    double sampleRate = 48000.0;
    int voice = (int) EqFx::Voice::british;
    StepTable steps;
    TwoPole knobs[3], levelDb;   // knobs: treble, mid, bass
    LevelStage level;
    Coeffs cur {}, target {}, inc {};
    double state[3][2] {};
    int rampLeft = 0;
    GridRunner<StackEq> grid;
    bool snapOnNextUpdate = true;
};

/** The switcher key: Stack's voice is a second choice inside the type (§2.4). */
int switchKey (int type, int voice) noexcept
{
    return type * 16 + (type == (int) EqFx::Type::stack ? voice : 0);
}
} // namespace

//==============================================================================
struct EqFx::Impl
{
    ClassicEq classic;
    ParametricEq parametric;
    StackEq stack;
    GraphicEq graphic;
    fx::EngineSwitcher switcher;
    EqParams params;
    int lastRequested = 0, switchLeft = 0, switchSpan = 0, gridPhase = 0, maxBlock = 0, maxChannels = 0;
    bool snapEngine = true;

    void resetEngine (int key) noexcept
    {
        switch ((Type) (key / 16))
        {
            case Type::classic:    classic.reset(); break;
            case Type::parametric: parametric.reset(); break;
            case Type::stack:      stack.reset(); stack.setVoice (key % 16); break;
            case Type::graphic:    graphic.reset(); break;
        }
    }

    void run (juce::dsp::AudioBlock<float> block, int requested)
    {
        // A type or voice change fades to the dry input, resets the incoming engine while
        // silent and fades back in; while steady the switcher does not touch the audio.
        // The incoming engine is reset only on the 16-sample grid (the switcher holds the dry
        // input until then), so the switch does not depend on the host block size. A block
        // larger than prepare() promised cannot be held: the switcher completes it outright.
        const bool onGrid = gridPhase == 0 || switchLeft <= 0 || (int) block.getNumSamples() > maxBlock
                            || (int) block.getNumChannels() > maxChannels;
        const int key = switcher.begin (block, requested, [this, onGrid] (int k, bool)
        {
            if (! onGrid)
                return false;

            resetEngine (k);
            return true;
        });

        if (! switcher.isHolding())
        {
            switch ((Type) (key / 16))
            {
                case Type::classic:
                    classic.setTargets (params.bass, params.mid, params.treble);
                    classic.process (block);
                    break;

                case Type::parametric:
                    parametric.setTargets (params);
                    parametric.process (block);
                    break;

                case Type::stack:
                    stack.setTargets (params);
                    stack.process (block);
                    break;

                case Type::graphic:
                    graphic.setTargets (params);
                    graphic.process (block);
                    break;
            }
        }

        switcher.end (block);
    }
};

EqFx::EqFx() : impl (std::make_unique<Impl>()) {}
EqFx::~EqFx() = default;

void EqFx::prepare (const juce::dsp::ProcessSpec& spec)
{
    auto& s = *impl;
    s.classic.prepare (spec);
    s.parametric.prepare (spec.sampleRate);
    s.stack.prepare (spec.sampleRate);
    s.graphic.prepare (spec.sampleRate);
    s.switcher.prepare (spec.sampleRate, (int) spec.maximumBlockSize, (int) spec.numChannels,
                        switchKey (s.params.type, s.params.voice));
    // Fade out, a hold shorter than one grid step, fade in (EngineSwitcher: 20 ms each way).
    s.switchSpan = 2 * (int) std::ceil (0.02 * spec.sampleRate) + 3 * kControlInterval;
    s.maxBlock = (int) spec.maximumBlockSize;
    s.maxChannels = (int) spec.numChannels;
    s.snapEngine = true;
}

void EqFx::reset()
{
    auto& s = *impl;
    s.classic.reset();
    s.parametric.reset();
    s.stack.reset();
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

    if (s.snapEngine)
    {
        // Nothing has been heard since prepare() or reset(): start on the requested type.
        s.snapEngine = false;
        s.stack.setVoice (s.params.voice);
        s.switcher.snapTo (requested);
        s.lastRequested = requested;
        s.switchLeft = 0;
        s.gridPhase = 0;
    }

    if (requested != s.lastRequested)
    {
        s.lastRequested = requested;
        s.switchLeft = s.switchSpan;
    }

    if (s.switchLeft <= 0)
    {
        s.run (block, requested);
        s.gridPhase = (s.gridPhase + n) % kControlInterval;
        return;
    }

    // While a switch is in flight the block is cut on a 16-sample grid, so the incoming
    // engine is reset at the same sample whatever the host block size, and the dry hold
    // between the two fades stays under one grid step instead of up to a whole block.
    for (int pos = 0; pos < n;)
    {
        const int len = s.switchLeft > 0 ? juce::jmin (kControlInterval - s.gridPhase, n - pos) : n - pos;
        s.run (block.getSubBlock ((size_t) pos, (size_t) len), requested);
        s.gridPhase = (s.gridPhase + len) % kControlInterval;
        s.switchLeft -= len;
        pos += len;
    }
}
} // namespace tubamp
