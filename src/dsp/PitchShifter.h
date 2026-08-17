#pragma once

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tubamp
{
/**
    §3.11 granular pitch shifter — the Shimmer voice.

    FOUR overlapping grains, not two. §3.11 is explicit about the count and about why:
    a 1–2 window shifter combs badly on a dense tail, and Costello's own shifter "uses
    randomization to avoid the comb filtering artifacts". So each grain also draws a
    fresh start offset from a seeded LCG every time its Hann window closes, which is
    exactly the moment the window is at zero and the redraw is inaudible.

    Geometry. One delay line; each grain reads it at a delay that ramps linearly, and
    the four ramps are Hann-crossfaded at phase offsets of 1/4:

        delay_g(p) = kMinDelaySamples + jitter_g + p * D        p in [0, 1)
        dp/dt      = (1 - r) / D                                r = pitch ratio
        w(p)       = 0.5 - 0.5*cos(2*pi*p)                      Hann

    `d(delay)/dt = 1 - r` is the definition of playing back at rate `r`, so the SPAN `D`
    is a constant and the grain length `D / |1 - r|` is what varies with the interval:
    at D = 50 ms that is 25 ms at +oct+5th, 50 ms at +1 oct and 100 ms at ±(oct/5th).
    Holding the span fixed rather than the grain length is what keeps the delay line
    small — one span plus one jitter range, not one grain length times the largest
    ratio — and it is also why an interval change is click-free: `p` is continuous and
    only its slope flips sign, so no grain's read position ever jumps.

    Sum of four Hann windows at 1/4 offsets is exactly 2 (the cosines cancel), so the
    output is halved for unity gain on coherent content. Decorrelated grains — which is
    what the jitter buys — sum in power instead, about -4.3 dB; the reverb's own shimmer
    gain owns the level, and §3.11's hard limiter (applied by the caller, after the
    in-loop lowpass) owns the ceiling.

    Threading: prepare() is the only entry point that allocates. clear(), activate(),
    setRatio() and process() are audio-thread safe and allocation free. Everything
    seeded is re-seeded in clear(), so two renders of the same input match sample for
    sample (§6.4).
*/
class PitchShifter
{
public:
    static constexpr int   kNumGrains = 4;              // §3.11, four overlapping grains
    static constexpr float kSpanSeconds = 0.050f;       // D, the delay ramp's span
    static constexpr float kJitterSeconds = 0.020f;     // randomized grain start range
    static constexpr float kMinDelaySamples = 4.0f;     // >= 3 for the cubic read, and it
                                                        // is what stops an in-loop caller
                                                        // closing a zero-delay loop
    static constexpr int   numIntervals = 4;

    /** §3.11 / §5: FROZEN four intervals, default index 2 (+1 oct). Ratios are
        2^(semitones/12) at -12, +7, +12 and +19 semitones. */
    static float ratioFor (int interval) noexcept
    {
        constexpr float kRatios[numIntervals] = { 0.5f, 1.4983071f, 2.0f, 2.9966142f };
        return kRatios[(size_t) juce::jlimit (0, numIntervals - 1, interval)];
    }

    /** Allocates. ReverbEngine calls this for every mode, so entering Shimmer never
        allocates (§6.2). */
    void prepare (double sampleRate)
    {
        const float fs = (float) (sampleRate > 0.0 ? sampleRate : 44100.0);

        spanSamples = juce::jmax (16.0f, kSpanSeconds * fs);
        jitterSamples = kJitterSeconds * fs;

        // The whole read range plus the cubic interpolator's own 4-sample reach.
        len = (int) std::ceil (kMinDelaySamples + spanSamples + jitterSamples) + 8;
        buffer.assign ((size_t) (len + kGuard), 0.0f);

        for (int i = 0; i <= kWindowSteps; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                                         * (float) i / (float) kWindowSteps);

        setRatio (ratio);
        clear();
    }

    /** Full clear plus a re-seed, so the shifter is part of §6.4's bit-identical
        contract rather than a source of drift between two renders. */
    void clear() noexcept
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        write = 0;
        rng = kSeed;

        for (int g = 0; g < kNumGrains; ++g)
        {
            grainPhase[(size_t) g] = (float) g / (float) kNumGrains;
            grainJitter[(size_t) g] = nextJitter();
        }

        warmup = 0;
    }

    /** Called on the inactive -> active edge. The caller stops feeding the line when the
        shimmer level is zero (that is what makes the mode bit-inert there), so the buffer
        holds whatever it held when the level last reached zero; replaying it would be an
        audible ghost. Muting the output for one buffer length costs O(1) and is the only
        alternative to a memset on the audio thread. */
    void activate() noexcept { warmup = len; }

    void setRatio (float r) noexcept
    {
        ratio = juce::jlimit (0.25f, 4.0f, r);
        phaseInc = (1.0f - ratio) / spanSamples;
    }

    /** One sample in, one sample out. The write happens AFTER the reads, so the shortest
        path through the shifter is kMinDelaySamples. */
    inline float process (float x) noexcept
    {
        if (buffer.empty())
            return 0.0f;

        float out = 0.0f;

        if (warmup <= 0)
        {
            for (int g = 0; g < kNumGrains; ++g)
            {
                const float p = grainPhase[(size_t) g];
                const float d = juce::jlimit (3.0f, (float) (len - 5),
                                              kMinDelaySamples + grainJitter[(size_t) g] + p * spanSamples);

                // Hann by table + linear interpolation: 4 cosines per sample would be the
                // most expensive thing in the reverb, and the residual error of a 1024-step
                // table on a function this smooth is under 1e-5.
                const float wf = p * (float) kWindowSteps;
                const int   wi = juce::jlimit (0, kWindowSteps - 1, (int) wf);
                const float wt = wf - (float) wi;
                const float win = window[(size_t) wi] + wt * (window[(size_t) wi + 1] - window[(size_t) wi]);

                out += win * readCubic (d);
            }

            out *= 0.5f;    // sum of four 1/4-offset Hann windows is exactly 2
        }
        else
        {
            --warmup;
        }

        for (int g = 0; g < kNumGrains; ++g)
        {
            float& p = grainPhase[(size_t) g];
            p += phaseInc;

            // |phaseInc| = |1-r|/D <= 3/(16 samples) at the absurd end of the clamps, so a
            // single fold is always enough; the jlimit is a float-drift guard, not a wrap.
            if (p >= 1.0f)      { p -= 1.0f; grainJitter[(size_t) g] = nextJitter(); }
            else if (p < 0.0f)  { p += 1.0f; grainJitter[(size_t) g] = nextJitter(); }

            p = juce::jlimit (0.0f, 0.9999999f, p);
        }

        // §3.3's mirrored guard, so the four interpolation taps are always contiguous.
        buffer[(size_t) write] = x;
        if (write < kGuard)
            buffer[(size_t) (len + write)] = x;

        if (++write >= len)
            write = 0;

        return out;
    }

private:
    static constexpr int kGuard = 4;
    static constexpr int kWindowSteps = 1024;
    static constexpr std::uint32_t kSeed = 0x3E7B12A5u;

    /** ReverbEngine's LCG, verbatim — one generator across the engine (§6.4). */
    inline float nextJitter() noexcept
    {
        rng = rng * 1664525u + 1013904223u;
        return (float) (rng >> 8) * (1.0f / 16777216.0f) * jitterSamples;
    }

    inline float readCubic (float delay) const noexcept
    {
        const float readPos = (float) write - delay;
        const int base = (int) std::floor (readPos);
        const float t = readPos - (float) base;

        int j = base - 1;
        if (j < 0) j += len;
        if (j >= len) j -= len;

        const float* b = buffer.data() + j;
        const float x0 = b[0], x1 = b[1], x2 = b[2], x3 = b[3];
        const float c0 = x1;
        const float c1 = 0.5f * (x2 - x0);
        const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
        const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }

    std::vector<float> buffer;
    std::array<float, (size_t) kWindowSteps + 1> window {};
    std::array<float, (size_t) kNumGrains> grainPhase {};
    std::array<float, (size_t) kNumGrains> grainJitter {};

    int   len = 0, write = 0, warmup = 0;
    float spanSamples = 1.0f, jitterSamples = 0.0f;
    float ratio = 2.0f, phaseInc = 0.0f;
    std::uint32_t rng = kSeed;

    JUCE_LEAK_DETECTOR (PitchShifter)
};
} // namespace tubamp
