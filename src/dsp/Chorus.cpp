#include "Chorus.h"

#include "FxPrimitives.h"

#include <algorithm>
#include <cmath>

namespace tubamp
{
namespace
{
// Shared machinery of modes 1-4 (docs/FX-ENGINES.md §4.5.4).

constexpr float kDelayMaxSeconds = 0.020f;   // D_max [choice: §4.5.4, D-12]: the 30-cent law holds down to 0.44 Hz
constexpr float kLineSeconds = 0.040f;       // [choice] >= the 34 ms §4.5.4 asks for (10 ms + D_max + Tri shimmer)
constexpr float kMinRateHz = 0.01f;          // ChorusParams::rateHz range [spec: §4.5.2]
constexpr float kMaxRateHz = 20.0f;
constexpr float kSmoothSeconds = 0.02f;      // [choice: §2.5]
constexpr int kControlInterval = 16;         // D and filter coefficients [spec: §4.5.4]
constexpr float kRoundMinHz = 8.0f;          // corner rounding fc = max(8 Hz, 20 rate) [study: shared design]
constexpr float kRoundPerHz = 20.0f;
// The swing moves at most 2^(30/1200) - 1 seconds per second, so its motion bends pitch by
// no more than the Wide ceiling, and it starts and stops moving through a kRoundMinHz
// one-pole [choice: at low rates the cents law swings up to 20 ms, and the 20 ms knob
// smoothing alone turned a depth step into a swoop of an octave or more].
constexpr float kSwingSlew = 0.0174796f;
constexpr float kMaxCutoffRatio = 0.45f;     // [spec: §4.5.4]
constexpr float kSvfDamping = 1.41421356f;   // Butterworth k = sqrt 2 [spec: §4.5.4]
constexpr float kFlushBelow = 1.0e-15f;      // [spec: §2.6]
constexpr float kToneOctaves = 2.0f;         // LP corner x 2^(2 (tone - 0.5)) [spec: §4.5.4]

// Wide [study: R1]
constexpr float kWideCentsMax = 30.0f;       // [fit: chorus/evidence/brigade_chorus.tsv, 29.3 cents at 2.859 Hz]
constexpr float kWideBaseSeconds[2] { 0.006f, 0.010f };
constexpr float kWidePair2Ratio = 1.41421356f;
constexpr float kWidePair2Gain = 0.7f;
constexpr float kWideNorm = 0.819f;          // 1 / sqrt (1 + 0.7^2) [choice: §4.5.5]
constexpr float kWideOffsetR = 0.25f;        // quadrature within a pair [study: R1]
constexpr float kWidePair2Offset = 0.5f;     // [choice: §4.5.5, pairs start half a cycle apart]
constexpr float kWideHpHz = 60.0f;
constexpr float kWideLpHz = 9000.0f;

// CE [study: R2; ver: V:46]
constexpr float kCeBaseSeconds = 0.00295f;   // 1.6 - 4.3 ms sweep [fit: brigade_chorus.tsv]
constexpr float kCeSweepSeconds = 0.0027f;
constexpr float kCeBend = 0.1f;              // [fit: brigade_chorus.tsv; ver: V:48]
constexpr float kCeTopRateHz = 2.86f;        // the pedal's top rate [study: brigade_chorus.tsv; ver: V:46]
constexpr float kCeOffsetR = 0.25f;          // [study: R2]
constexpr float kCeHpHz = 130.0f;            // [fit: study R2 to brigade_fr.tsv]
constexpr float kCeLp2Hz = 4600.0f;
constexpr float kCeLp1Hz = 9000.0f;

// Dimension [study: R3; ver: V:53]
constexpr float kDimBaseSeconds = 0.0085f;   // [study: Anwander]
constexpr float kDimSwingSeconds = 0.0025f;
// Above 0.5 Hz the swing shrinks as 1/f, holding the detune at its 0.5 Hz value (8.6 cents
// at depth 1) [choice: 0.5 Hz is the fastest Dimension D mode (Anwander); a fixed swing
// gave 182 cents at 10 Hz, the defect V:46 found in CE].
constexpr float kDimTopRateHz = 0.5f;
constexpr float kDimHpHz = 80.0f;            // [study: Anwander SDD-320]
constexpr float kDimLpHz = 10000.0f;         // [study: tagged guess]

// Tri [study: R4]
constexpr float kTriBaseSeconds = 0.007f;
constexpr float kTriCentsMax = 20.0f;
constexpr float kTriFastHz = 6.0f;
constexpr float kTriFastCents = 3.0f;
constexpr float kTriPairGain = 0.7f;
constexpr float kTriHpHz = 80.0f;
constexpr float kTriLpHz = 6000.0f;
constexpr float kCos120 = -0.5f;
constexpr float kSin120 = 0.866025404f;

// Classic keeps juce::dsp::Chorus, whose line reads at most 7 ms + 10 ms back.
constexpr float kClassicReachSeconds = 0.017f;

/** Delay swing that gives `cents` of detune for a modulator whose peak slope is
    `slopePerHz` * f * D (2 for a triangle, pi for a sine), capped at D_max. */
float centsSwing (float cents, float rateHz, float slopePerHz) noexcept
{
    const float k = std::exp2 (cents * (1.0f / 1200.0f)) - 1.0f;
    return std::min (k / (slopePerHz * rateHz), kDelayMaxSeconds);
}

/** The longest delay a mode can read, i.e. how much history its line needs before it is heard. */
float reachSeconds (int mode) noexcept
{
    switch ((ChorusFx::Mode) mode)
    {
        case ChorusFx::Mode::wide:      return kWideBaseSeconds[1] + kDelayMaxSeconds;
        case ChorusFx::Mode::ce:        return kCeBaseSeconds + 0.5f * kCeSweepSeconds;
        case ChorusFx::Mode::dimension: return kDimBaseSeconds + kDimSwingSeconds;
        case ChorusFx::Mode::tri:       return kTriBaseSeconds + kDelayMaxSeconds + 0.0001f; // + the 92 us shimmer
        case ChorusFx::Mode::classic:
        default:                        return kClassicReachSeconds;
    }
}

inline float wrap01 (float x) noexcept
{
    return x - std::floor (x);
}

inline float triangle (float phase) noexcept
{
    return 1.0f - std::abs (2.0f * wrap01 (phase) - 1.0f);
}

/** The BBD clock's bend: slope x(1 - b) at the short end, x(1 + b) at the long end. */
inline float bentTriangle (float phase) noexcept
{
    const float u = triangle (phase);
    return u * (1.0f + kCeBend * (u - 1.0f));
}

/** sin and cos of 2 pi t. Quadrant reduction, then Taylor terms on [-pi/4, pi/4]
    (error < 4e-7), far cheaper than two libm calls per sample. */
inline void sinCosTurns (float t, float& s, float& c) noexcept
{
    static constexpr float quadCos[4] { 1.0f, 0.0f, -1.0f, 0.0f };
    static constexpr float quadSin[4] { 0.0f, 1.0f, 0.0f, -1.0f };

    const float q = std::floor (t * 4.0f + 0.5f);
    const float x = (t - 0.25f * q) * fx::kTwoPi;
    const float x2 = x * x;
    const float s0 = x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f))));
    const float c0 = 1.0f + x2 * (-0.5f + x2 * (1.0f / 24.0f + x2 * (-1.0f / 720.0f + x2 * (1.0f / 40320.0f))));
    const int k = (int) q & 3;
    s = s0 * quadCos[k] + c0 * quadSin[k];
    c = c0 * quadCos[k] - s0 * quadSin[k];
}

/** Trapezoidal one-pole gain G = g / (1 + g). */
float tptGain (float hz, float sampleRate) noexcept
{
    const float g = std::tan (fx::kPi * std::min (hz, kMaxCutoffRatio * sampleRate) / sampleRate);
    return g / (1.0f + g);
}

struct WetCoefficients
{
    float hp = 1.0f;                  // TPT one-pole G
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f; // TPT SVF (Zavalishin / Simper)
    float lp1 = 1.0f;                 // CE's extra one-pole

    void set (float hpHz, float lpHz, float lp1Hz, float sampleRate) noexcept
    {
        hp = tptGain (hpHz, sampleRate);
        const float g = std::tan (fx::kPi * std::min (lpHz, kMaxCutoffRatio * sampleRate) / sampleRate);
        a1 = 1.0f / (1.0f + g * (g + kSvfDamping));
        a2 = g * a1;
        a3 = g * a2;
        lp1 = lp1Hz > 0.0f ? tptGain (lp1Hz, sampleRate) : 1.0f;
    }
};

/** The wet band-limit in front of a line: HP1 -> LP2 Butterworth (-> LP1 for CE). */
struct WetFilter
{
    float hpState = 0.0f, ic1 = 0.0f, ic2 = 0.0f, lp1State = 0.0f;

    void clear() noexcept { hpState = ic1 = ic2 = lp1State = 0.0f; }

    float process (float x, const WetCoefficients& c) noexcept
    {
        const float v = (x - hpState) * c.hp;
        const float low = v + hpState;
        hpState = low + v;
        const float hp = x - low;

        const float v3 = hp - ic2;
        const float v1 = c.a1 * ic1 + c.a2 * v3;
        const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v2;
    }

    float processCe (float x, const WetCoefficients& c) noexcept
    {
        const float y = process (x, c);
        const float v = (y - lp1State) * c.lp1;
        const float out = v + lp1State;
        lp1State = out + v;
        return out;
    }

    void tidy() noexcept
    {
        for (float* s : { &hpState, &ic1, &ic2, &lp1State })
            *s = std::abs (*s) < kFlushBelow ? 0.0f : fx::sanitise (*s);
    }
};

struct Smoothed
{
    float current = 0.0f, target = 0.0f;

    float next (float a) noexcept
    {
        current += a * (target - current);
        return current;
    }

    void snap() noexcept { current = target; }

    /** Lands exactly on the target instead of creeping towards it in denormal steps. */
    void settle() noexcept
    {
        if (std::abs (target - current) < 1.0e-7f)
            current = target;
    }
};

//==============================================================================
/** Modes 1-4 on one pair of lines. Only one mode is heard at a time and a mode switch
    clears everything, so the lines, filters and LFO state are shared by all four. */
class ModernChorus
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = (float) newSampleRate;
        invSampleRate = 1.0 / newSampleRate;
        smoothA = 1.0f - std::exp (-1.0f / std::max (1.0f, kSmoothSeconds * sampleRate));
        swingRoundA = 1.0f - std::exp (-fx::kTwoPi * kRoundMinHz * (float) kControlInterval / sampleRate);

        for (auto& line : lines)
            line.setSize ((int) std::ceil (kLineSeconds * sampleRate) + 8);

        clear();
    }

    void clear() noexcept
    {
        for (auto& line : lines)
            line.clear();

        for (auto& f : filters)
            f.clear();

        phase[0] = phase[1] = 0.0;
        std::fill (std::begin (shape), std::end (shape), 0.0f);
        std::fill (std::begin (swing), std::end (swing), 0.0f);
        std::fill (std::begin (swingStep), std::end (swingStep), 0.0f);
        std::fill (std::begin (swingTarget), std::end (swingTarget), 0.0f);
        std::fill (std::begin (swingSlewed), std::end (swingSlewed), 0.0f);
        tick = 0;
        snapControl = true;
    }

    void setTargets (const ChorusParams& p) noexcept
    {
        rate.target = p.rateHz;
        depth.target = p.depth;
        mix.target = p.mix;
        width.target = p.width;
        tone.target = p.tone;
    }

    void snapSmoothers() noexcept
    {
        for (auto* s : { &rate, &depth, &mix, &width, &tone })
            s->snap();
    }

    void setPhase (double cycles) noexcept
    {
        phase[0] = cycles - std::floor (cycles);
    }

    static int reachSamples (int mode, double fs) noexcept
    {
        return (int) std::ceil (reachSeconds (mode) * fs) + 4;
    }

    /** Fills the lines with `block` without producing output, so that a mode fading in
        after a switch reads real history instead of the silence a cleared line holds. */
    void fill (const juce::dsp::AudioBlock<float>& block, int mode) noexcept
    {
        if (tick == 0)
            control (mode);

        const int n = (int) block.getNumSamples();
        const bool stereo = block.getNumChannels() > 1;
        const float* inL = block.getChannelPointer (0);
        const float* inR = stereo ? block.getChannelPointer (1) : inL;

        for (int i = 0; i < n; ++i)
        {
            switch ((ChorusFx::Mode) mode)
            {
                case ChorusFx::Mode::wide:
                    lines[0].write (filters[0].process (inL[i], coefs));
                    lines[1].write (filters[1].process (inR[i], coefs));
                    break;
                case ChorusFx::Mode::ce:
                    lines[0].write (filters[0].processCe (inL[i], coefs));
                    lines[1].write (filters[1].processCe (inR[i], coefs));
                    break;
                case ChorusFx::Mode::dimension:
                case ChorusFx::Mode::tri:
                case ChorusFx::Mode::classic:
                default:
                    lines[0].write (filters[0].process (0.5f * (inL[i] + inR[i]), coefs));
                    break;
            }
        }

        for (auto& f : filters)
            f.tidy();
    }

    void process (juce::dsp::AudioBlock<float>& block, int mode, int solo) noexcept
    {
        const bool stereo = block.getNumChannels() > 1;

        switch ((ChorusFx::Mode) mode)
        {
            case ChorusFx::Mode::wide:
                solo < 0 ? runWide<false> (block, stereo, 0) : runWide<true> (block, stereo, std::min (solo, 3));
                break;
            case ChorusFx::Mode::ce:
                solo < 0 ? runCe<false> (block, stereo, 0) : runCe<true> (block, stereo, std::min (solo, 1));
                break;
            case ChorusFx::Mode::dimension:
                solo < 0 ? runDimension<false> (block, stereo, 0) : runDimension<true> (block, stereo, std::min (solo, 1));
                break;
            case ChorusFx::Mode::tri:
                solo < 0 ? runTri<false> (block, stereo, 0) : runTri<true> (block, stereo, std::min (solo, 2));
                break;
            case ChorusFx::Mode::classic:
            default:
                break;
        }

        // A mono block only runs the left chain; keep the right one in step with it.
        if (! stereo)
            filters[1] = filters[0];
    }

private:
    /** Once per kControlInterval samples: filter coefficients, the delay swings (reached
        linearly over the next interval) and the LFO corner rounding. */
    void control (int mode) noexcept
    {
        for (auto* s : { &rate, &depth, &mix, &width, &tone })
            s->settle();

        for (auto& f : filters)
            f.tidy();

        const float f = rate.current;
        const float depth2 = depth.current * depth.current;
        const float toneScale = std::exp2 (kToneOctaves * (tone.current - 0.5f));
        float target[2] { 0.0f, 0.0f };
        float secondRate = f;

        switch ((ChorusFx::Mode) mode)
        {
            case ChorusFx::Mode::wide:
                coefs.set (kWideHpHz, kWideLpHz * toneScale, 0.0f, sampleRate);
                secondRate = f * kWidePair2Ratio;
                target[0] = centsSwing (kWideCentsMax * depth2, f, 2.0f);
                target[1] = centsSwing (kWideCentsMax * depth2, secondRate, 2.0f);
                break;
            case ChorusFx::Mode::ce:
                coefs.set (kCeHpHz, kCeLp2Hz * toneScale, kCeLp1Hz * toneScale, sampleRate);
                // Above the pedal's top rate the swing shrinks as 1/f, so the detune stays at
                // its 2.86 Hz value instead of growing to 91 cents at 10 Hz (V:46).
                target[0] = kCeSweepSeconds * depth.current * std::min (1.0f, kCeTopRateHz / f);
                break;
            case ChorusFx::Mode::dimension:
                coefs.set (kDimHpHz, kDimLpHz * toneScale, 0.0f, sampleRate);
                target[0] = kDimSwingSeconds * depth.current * std::min (1.0f, kDimTopRateHz / f);
                break;
            case ChorusFx::Mode::tri:
                coefs.set (kTriHpHz, kTriLpHz * toneScale, 0.0f, sampleRate);
                secondRate = kTriFastHz;
                target[0] = centsSwing (kTriCentsMax * depth2, f, fx::kPi);
                target[1] = centsSwing (kTriFastCents, kTriFastHz, fx::kPi);
                break;
            case ChorusFx::Mode::classic:
            default:
                break;
        }

        const float lfoRate[2] { f, secondRate };

        for (int k = 0; k < 2; ++k)
        {
            const float corner = std::max (kRoundMinHz, kRoundPerHz * lfoRate[k]);
            roundA[k] = 1.0f - std::exp (-fx::kTwoPi * std::min (corner, kMaxCutoffRatio * sampleRate) / sampleRate);
            // The rounding one-pole delays the LFO's fundamental by atan (f / fc); reading the
            // shape that much ahead puts it back on the phase, so a synced LFO lands on the beat.
            lead[k] = std::atan (lfoRate[k] / corner) * (1.0f / fx::kTwoPi);

            const float samples = target[k] * sampleRate;

            if (snapControl)
            {
                swing[k] = swingTarget[k] = swingSlewed[k] = samples;
                swingStep[k] = 0.0f;
            }
            else
            {
                constexpr float maxStep = kSwingSlew * (float) kControlInterval; // samples per interval at any rate
                swingSlewed[k] = juce::jlimit (swingSlewed[k] - maxStep, swingSlewed[k] + maxStep, samples);
                swing[k] = swingTarget[k];
                swingTarget[k] += swingRoundA * (swingSlewed[k] - swingTarget[k]);

                if (std::abs (swingSlewed[k] - swingTarget[k]) < 1.0e-6f)
                    swingTarget[k] = swingSlewed[k];

                swingStep[k] = (swingTarget[k] - swing[k]) * (1.0f / (float) kControlInterval);
            }
        }

        if (snapControl)
        {
            primeShapes (mode);
            snapControl = false;
        }
    }

    /** Starts every rounding one-pole on its input, so nothing glides in after a reset. */
    void primeShapes (int mode) noexcept
    {
        const float p0 = (float) phase[0] + lead[0];
        const float p1 = (float) phase[1] + lead[1];

        switch ((ChorusFx::Mode) mode)
        {
            case ChorusFx::Mode::wide:
                shape[0] = triangle (p0);
                shape[1] = triangle (p0 + kWideOffsetR);
                shape[2] = triangle (p1 + kWidePair2Offset);
                shape[3] = triangle (p1 + kWidePair2Offset + kWideOffsetR);
                break;
            case ChorusFx::Mode::ce:
                shape[0] = bentTriangle (p0);
                shape[1] = bentTriangle (p0 + kCeOffsetR);
                break;
            case ChorusFx::Mode::dimension:
                shape[0] = triangle (p0);
                break;
            case ChorusFx::Mode::tri:
            {
                float s, c;
                sinCosTurns (wrap01 (p0), s, c);
                shape[0] = 0.5f - 0.5f * c;
                shape[1] = 0.5f - 0.5f * (kCos120 * c - kSin120 * s);
                shape[2] = 0.5f - 0.5f * (kCos120 * c + kSin120 * s);
                sinCosTurns (wrap01 (p1), s, c);
                shape[3] = 0.5f - 0.5f * c;
                shape[4] = 0.5f - 0.5f * (kCos120 * c - kSin120 * s);
                shape[5] = 0.5f - 0.5f * (kCos120 * c + kSin120 * s);
                break;
            }
            case ChorusFx::Mode::classic:
            default:
                break;
        }
    }

    /** Splits a block at the control grid, which keeps its place across process() calls,
        so the output does not depend on the block size. `body (start, length)` runs the
        samples of one interval. */
    template <typename Body>
    void forEachInterval (int numSamples, int mode, Body&& body) noexcept
    {
        int i = 0;

        while (i < numSamples)
        {
            if (tick == 0)
                control (mode);

            const int len = std::min (numSamples - i, kControlInterval - tick);
            body (i, len);
            i += len;
            tick = (tick + len) & (kControlInterval - 1);
        }
    }

    /** dry = min (1, 2 - 2m), wet = min (1, 2m) [choice: §4.5.4, dry at unity to the midpoint];
        width scales the wet side signal. */
    struct Output
    {
        float dry, wet, width;

        Output (Smoothed& mixS, Smoothed& widthS, float a) noexcept
        {
            const float m = mixS.next (a);
            dry = std::min (1.0f, 2.0f - 2.0f * m);
            wet = std::min (1.0f, 2.0f * m);
            width = widthS.next (a);
        }

        void write (float* outL, float* outR, int i, float xL, float xR, float wL, float wR, bool stereo) const noexcept
        {
            const float mid = 0.5f * (wL + wR);

            if (stereo)
            {
                const float side = 0.5f * width * (wL - wR);
                outL[i] = dry * xL + wet * (mid + side);
                outR[i] = dry * xR + wet * (mid - side);
            }
            else
            {
                outL[i] = dry * xL + wet * mid;
            }
        }
    };

    static void writeSolo (float* outL, float* outR, int i, float v, bool stereo) noexcept
    {
        outL[i] = v;

        if (stereo)
            outR[i] = v;
    }

    template <bool solo>
    void runWide (juce::dsp::AudioBlock<float>& block, bool stereo, int soloVoice) noexcept
    {
        float* outL = block.getChannelPointer (0);
        float* outR = stereo ? block.getChannelPointer (1) : outL;
        const float base0 = kWideBaseSeconds[0] * sampleRate;
        const float base1 = kWideBaseSeconds[1] * sampleRate;
        const int mode = (int) ChorusFx::Mode::wide;

        forEachInterval ((int) block.getNumSamples(), mode, [&] (int start, int len)
        {
            const float ra0 = roundA[0], ra1 = roundA[1];
            const float lead0 = lead[0], lead1 = lead[1];

            for (int i = start; i < start + len; ++i)
            {
                const float f = rate.next (smoothA);
                depth.next (smoothA);
                tone.next (smoothA);
                const Output out (mix, width, smoothA);

                const float xL = outL[i];
                const float xR = stereo ? outR[i] : xL;
                const float fl = filters[0].process (xL, coefs);
                lines[0].write (fl);
                lines[1].write (stereo ? filters[1].process (xR, coefs) : fl);

                const float p0 = (float) phase[0] + lead0;
                const float p1 = (float) phase[1] + lead1;
                shape[0] += ra0 * (triangle (p0) - shape[0]);
                shape[1] += ra0 * (triangle (p0 + kWideOffsetR) - shape[1]);
                shape[2] += ra1 * (triangle (p1 + kWidePair2Offset) - shape[2]);
                shape[3] += ra1 * (triangle (p1 + kWidePair2Offset + kWideOffsetR) - shape[3]);

                const float v0 = lines[0].readHermite (base0 + swing[0] * shape[0]);
                const float v1 = lines[1].readHermite (base0 + swing[0] * shape[1]);
                const float v2 = lines[0].readHermite (base1 + swing[1] * shape[2]);
                const float v3 = lines[1].readHermite (base1 + swing[1] * shape[3]);
                swing[0] += swingStep[0];
                swing[1] += swingStep[1];
                advance (f, f * kWidePair2Ratio);

                if constexpr (solo)
                {
                    const float v[4] { v0, v1, v2, v3 };
                    writeSolo (outL, outR, i, v[soloVoice], stereo);
                }
                else
                {
                    out.write (outL, outR, i, xL, xR, (v0 + kWidePair2Gain * v2) * kWideNorm,
                               (v1 + kWidePair2Gain * v3) * kWideNorm, stereo);
                }
            }
        });
    }

    template <bool solo>
    void runCe (juce::dsp::AudioBlock<float>& block, bool stereo, int soloVoice) noexcept
    {
        float* outL = block.getChannelPointer (0);
        float* outR = stereo ? block.getChannelPointer (1) : outL;
        const float base = kCeBaseSeconds * sampleRate;
        const int mode = (int) ChorusFx::Mode::ce;

        forEachInterval ((int) block.getNumSamples(), mode, [&] (int start, int len)
        {
            const float ra = roundA[0], lead0 = lead[0];

            for (int i = start; i < start + len; ++i)
            {
                const float f = rate.next (smoothA);
                depth.next (smoothA);
                tone.next (smoothA);
                const Output out (mix, width, smoothA);

                const float xL = outL[i];
                const float xR = stereo ? outR[i] : xL;
                const float fl = filters[0].processCe (xL, coefs);
                lines[0].write (fl);
                lines[1].write (stereo ? filters[1].processCe (xR, coefs) : fl);

                const float p = (float) phase[0] + lead0;
                shape[0] += ra * (bentTriangle (p) - shape[0]);
                shape[1] += ra * (bentTriangle (p + kCeOffsetR) - shape[1]);

                const float vL = lines[0].readHermite (base + swing[0] * (shape[0] - 0.5f));
                const float vR = lines[1].readHermite (base + swing[0] * (shape[1] - 0.5f));
                swing[0] += swingStep[0];
                advance (f, f);

                if constexpr (solo)
                    writeSolo (outL, outR, i, soloVoice == 0 ? vL : vR, stereo);
                else
                    out.write (outL, outR, i, xL, xR, vL, vR, stereo);
            }
        });
    }

    template <bool solo>
    void runDimension (juce::dsp::AudioBlock<float>& block, bool stereo, int soloVoice) noexcept
    {
        float* outL = block.getChannelPointer (0);
        float* outR = stereo ? block.getChannelPointer (1) : outL;
        const float base = kDimBaseSeconds * sampleRate;
        const int mode = (int) ChorusFx::Mode::dimension;

        forEachInterval ((int) block.getNumSamples(), mode, [&] (int start, int len)
        {
            const float ra = roundA[0], lead0 = lead[0];

            for (int i = start; i < start + len; ++i)
            {
                const float f = rate.next (smoothA);
                depth.next (smoothA);
                tone.next (smoothA);
                const Output out (mix, width, smoothA);

                const float xL = outL[i];
                const float xR = stereo ? outR[i] : xL;
                lines[0].write (filters[0].process (0.5f * (xL + xR), coefs));

                shape[0] += ra * (triangle ((float) phase[0] + lead0) - shape[0]);
                // Line B runs the inverted triangle (u_B = 1 - u_A).
                const float sweep = swing[0] * (2.0f * shape[0] - 1.0f);
                const float a = lines[0].readHermite (base + sweep);
                const float b = lines[0].readHermite (base - sweep);
                swing[0] += swingStep[0];
                advance (f, f);

                if constexpr (solo)
                    writeSolo (outL, outR, i, soloVoice == 0 ? a : b, stereo);
                else
                    out.write (outL, outR, i, xL, xR, a, b, stereo);
            }
        });
    }

    template <bool solo>
    void runTri (juce::dsp::AudioBlock<float>& block, bool stereo, int soloVoice) noexcept
    {
        float* outL = block.getChannelPointer (0);
        float* outR = stereo ? block.getChannelPointer (1) : outL;
        const float base = kTriBaseSeconds * sampleRate;
        const int mode = (int) ChorusFx::Mode::tri;

        forEachInterval ((int) block.getNumSamples(), mode, [&] (int start, int len)
        {
            const float ra0 = roundA[0], ra1 = roundA[1];
            const float lead0 = lead[0], lead1 = lead[1];

            for (int i = start; i < start + len; ++i)
            {
                const float f = rate.next (smoothA);
                depth.next (smoothA);
                tone.next (smoothA);
                const Output out (mix, width, smoothA);

                const float xL = outL[i];
                const float xR = stereo ? outR[i] : xL;
                lines[0].write (filters[0].process (0.5f * (xL + xR), coefs));

                float s, c;
                sinCosTurns (wrap01 ((float) phase[0] + lead0), s, c);
                shape[0] += ra0 * ((0.5f - 0.5f * c) - shape[0]);
                shape[1] += ra0 * ((0.5f - 0.5f * (kCos120 * c - kSin120 * s)) - shape[1]);
                shape[2] += ra0 * ((0.5f - 0.5f * (kCos120 * c + kSin120 * s)) - shape[2]);
                sinCosTurns (wrap01 ((float) phase[1] + lead1), s, c);
                shape[3] += ra1 * ((0.5f - 0.5f * c) - shape[3]);
                shape[4] += ra1 * ((0.5f - 0.5f * (kCos120 * c - kSin120 * s)) - shape[4]);
                shape[5] += ra1 * ((0.5f - 0.5f * (kCos120 * c + kSin120 * s)) - shape[5]);

                const float v0 = lines[0].readHermite (base + swing[0] * shape[0] + swing[1] * shape[3]);
                const float v1 = lines[0].readHermite (base + swing[0] * shape[1] + swing[1] * shape[4]);
                const float v2 = lines[0].readHermite (base + swing[0] * shape[2] + swing[1] * shape[5]);
                swing[0] += swingStep[0];
                swing[1] += swingStep[1];
                advance (f, kTriFastHz);

                if constexpr (solo)
                {
                    const float v[3] { v0, v1, v2 };
                    writeSolo (outL, outR, i, v[soloVoice], stereo);
                }
                else
                {
                    out.write (outL, outR, i, xL, xR, kTriPairGain * (v0 + v1), kTriPairGain * (v1 + v2), stereo);
                }
            }
        });
    }

    void advance (float rate0, float rate1) noexcept
    {
        phase[0] += (double) rate0 * invSampleRate;
        phase[1] += (double) rate1 * invSampleRate;

        if (phase[0] >= 1.0)
            phase[0] -= 1.0;

        if (phase[1] >= 1.0)
            phase[1] -= 1.0;
    }

    fx::DelayBuffer lines[2];
    WetFilter filters[2];
    WetCoefficients coefs;
    Smoothed rate, depth, mix, width, tone;

    // phase[0] is the master LFO (the one syncPhase sets); phase[1] is Wide's second pair
    // or Tri's 6 Hz shimmer.
    double phase[2] {};
    float shape[6] {};          // rounded LFO value per voice (Tri: 3 slow, then 3 fast)
    float swing[2] {}, swingStep[2] {}, swingTarget[2] {}, swingSlewed[2] {}; // samples
    float roundA[2] { 1.0f, 1.0f }, lead[2] {};

    float sampleRate = 48000.0f, smoothA = 1.0f, swingRoundA = 1.0f;
    double invSampleRate = 1.0 / 48000.0;
    int tick = 0;
    bool snapControl = true;
};

float finiteOr (float value, float fallback) noexcept
{
    return std::isfinite (value) ? value : fallback;
}
} // namespace

//==============================================================================
struct ChorusFx::Impl
{
    juce::dsp::Chorus<float> classic;
    ModernChorus modern;
    fx::EngineSwitcher switcher;
    juce::AudioBuffer<float> warmup;   // Classic's throwaway output while its line refills
    ChorusParams params;
    double sampleRate = 48000.0;
    int maxBlock = 512, numChannels = 2, solo = -1, filled = 0;
    bool snapEngine = true, snapParams = true;

    /** The switcher's reset step for the incoming mode. Returns false (the switcher holds
        on dry) until that mode's line holds as much history as it can read, so the wet
        fades in from real signal instead of switching on out of a cleared line. */
    bool startMode (int mode, bool restart, const juce::dsp::AudioBlock<float>& block)
    {
        const int n = (int) block.getNumSamples();

        if (restart)
        {
            filled = 0;

            if (mode == (int) Mode::classic)
            {
                classic.reset();
            }
            else
            {
                modern.clear();
                modern.snapSmoothers();
            }
        }

        // Ready is decided before this block is written: once ready, the mode processes this
        // very block, and writing it here as well would put it into the line twice.
        if (filled >= ModernChorus::reachSamples (mode, sampleRate)
            || n > maxBlock || (int) block.getNumChannels() > numChannels)
            return true;

        if (mode == (int) Mode::classic)
        {
            const auto channels = block.getNumChannels();

            for (size_t ch = 0; ch < channels; ++ch)
                juce::FloatVectorOperations::copy (warmup.getWritePointer ((int) ch), block.getChannelPointer (ch), n);

            juce::dsp::AudioBlock<float> scratch (warmup.getArrayOfWritePointers(), channels, (size_t) n);
            juce::dsp::ProcessContextReplacing<float> context (scratch);
            classic.process (context);
        }
        else
        {
            modern.fill (block, mode);
        }

        filled += n;
        return false;
    }

    void run (juce::dsp::AudioBlock<float> block, bool applySync)
    {
        const int mode = switcher.begin (block, params.mode, [this, &block] (int m, bool restart)
        {
            return startMode (m, restart, block);
        });

        if (! switcher.isHolding())
        {
            if (mode == (int) Mode::classic)
            {
                // FxBlocks.cpp Modulation::process, chorus branch (§4.5.3).
                juce::dsp::ProcessContextReplacing<float> context (block);
                classic.process (context);
            }
            else
            {
                if (applySync)
                    modern.setPhase (params.syncPhase);

                modern.process (block, mode, solo);
            }
        }

        switcher.end (block);
    }
};

ChorusFx::ChorusFx() : impl (std::make_unique<Impl>()) {}
ChorusFx::~ChorusFx() = default;

void ChorusFx::prepare (const juce::dsp::ProcessSpec& spec)
{
    auto& s = *impl;
    s.sampleRate = spec.sampleRate;
    s.maxBlock = juce::jmax (1, (int) spec.maximumBlockSize);
    s.numChannels = juce::jlimit (1, 2, (int) spec.numChannels);

    // FxBlocks.cpp Modulation::prepare, chorus parts (§4.5.3); reset() follows below.
    s.classic.prepare (spec);
    s.classic.setCentreDelay (7.0f);
    s.classic.setFeedback (0.0f);

    s.modern.prepare (spec.sampleRate);
    s.switcher.prepare (spec.sampleRate, s.maxBlock, s.numChannels, (int) Mode::wide);
    s.warmup.setSize (s.numChannels, s.maxBlock);
    reset();
}

void ChorusFx::reset()
{
    auto& s = *impl;
    s.classic.reset();
    s.modern.clear();
    s.filled = 0;
    s.snapEngine = true;
    s.snapParams = true;
}

void ChorusFx::setParameters (const ChorusParams& params)
{
    auto& s = *impl;
    auto& p = s.params;
    p.mode = juce::jlimit (0, numModes - 1, params.mode);

    if (p.mode == (int) Mode::classic)
    {
        // FxBlocks.cpp Modulation::setParameters, chorus branch, verbatim (§4.5.3).
        const float rate = juce::jlimit (0.01f, 100.0f, finiteOr (params.rateHz, 1.0f));
        const float depth = juce::jlimit (0.0f, 1.0f, finiteOr (params.depth, 0.0f));
        const float mix = juce::jlimit (0.0f, 1.0f, finiteOr (params.mix, 0.0f));
        s.classic.setRate (rate);
        s.classic.setDepth (depth);
        s.classic.setMix (mix);
    }

    p.rateHz = juce::jlimit (kMinRateHz, kMaxRateHz, finiteOr (params.rateHz, 1.0f));
    p.depth = juce::jlimit (0.0f, 1.0f, finiteOr (params.depth, 0.0f));
    p.mix = juce::jlimit (0.0f, 1.0f, finiteOr (params.mix, 0.0f));
    p.width = juce::jlimit (0.0f, 1.0f, finiteOr (params.width, 1.0f));
    p.tone = juce::jlimit (0.0f, 1.0f, finiteOr (params.tone, 0.5f));
    p.syncPhase = std::isfinite (params.syncPhase) ? params.syncPhase : -1.0;
    s.modern.setTargets (p);

    if (s.snapParams)
    {
        s.snapParams = false;
        s.modern.snapSmoothers();
    }
}

void ChorusFx::process (juce::dsp::AudioBlock<float> block)
{
    auto& s = *impl;
    const auto total = block.getNumSamples();

    if (total == 0)
        return;

    if (s.snapEngine)
    {
        // Nothing has been heard since prepare() or reset(): start on the requested mode.
        s.snapEngine = false;
        s.switcher.snapTo (s.params.mode);
    }

    // juce::dsp::Chorus and the switcher's dry copy are sized for the prepared block, so a
    // larger block runs in pieces; syncPhase belongs to the first of them.
    const auto step = (size_t) s.maxBlock;

    for (size_t start = 0; start < total; start += step)
        s.run (block.getSubBlock (start, juce::jmin (step, total - start)), start == 0 && s.params.syncPhase >= 0.0);
}

void ChorusFx::setDebugSolo (int voice) noexcept
{
    impl->solo = voice;
}
} // namespace tubamp
