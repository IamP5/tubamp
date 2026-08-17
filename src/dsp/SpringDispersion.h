#pragma once

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace tubamp
{
/**
    §3.9 spring dispersion — the cascade of STRETCHED first-order allpass sections that
    turns a plain delay line into a spring.

    Per section, with `D(z)` a fractional delay of `K` samples:

        A(z)     = [a1 + D(z)] / [1 + a1 * D(z)]
        D(z)     = A_fd(z) * z^-K1,   A_fd(z) = (a2 + z^-1) / (1 + a2 * z^-1)
        K        = fs / (2 * 4200)    ← derived from fs, NEVER a fixed integer
        K1       = round(K) - 1,      a2 solved from the leftover fraction

    `K` must come from `fs`. Fixing it (the first draft's `K = 5`) pins the chirp knee at
    `fs/2K`: 4.8 kHz at 48 kHz but 4.4 kHz at 44.1 and 9.6 kHz at 96 — the mode's identity
    would change with the host rate, in a design whose rule is that nothing is stored in
    samples. `K1 = round(K) - 1` keeps the leftover delay `d = K - K1` inside [0.5, 1.5],
    which is exactly the interval where the first-order allpass `a2 = (1-d)/(1+d)` is a
    well-conditioned fractional-delay filter, and it guarantees `K1 >= 1` at every real
    host rate so the section stays computable (the loop closes through `z^-K1`).

    `M = 100` sections (§3.9, capped from the unconstrained 318 — measured perceptually
    indistinguishable). Group delay per section runs from `K*(1-a1)/(1+a1)` at DC to
    `K*(1+a1)/(1-a1)` at the knee, i.e. 1.30 -> 25.2 samples at 48 kHz, so the cascade
    delays the knee region by ~52 ms at 48 kHz (100 * 25.2 samples) against 2.70 ms at DC
    (100 * 1.30). BOTH endpoints are on the `K` basis — the 46 ms an earlier draft quoted
    here is the same knee delay on the `K1` basis, and mixing the two bases in the sentence
    that defines these constants would mis-anchor a future `a1` re-fit. No gate reads
    either number; the gates read `lfDelaySamples()`, which is exact.
    That spread IS the boing. Above the knee the stretched response wraps
    (period `fs/K` = 8.4 kHz at 48 kHz) and the delay returns to its DC value, which is
    why §3.9 also puts a 2.8 kHz one-pole in the loop: it buries the repeats.

    Allocation happens only in prepare(). process() and clear() are audio-thread safe.
*/
class SpringDispersion
{
public:
    static constexpr int   kNumSections = 100;      // §3.9 M
    static constexpr float kA1 = 0.63f;             // §3.9, re-fit at the corrected K
    static constexpr float kChirpFcHz = 4200.0f;    // §3.9 chirp knee

    /** Allocates. Called from ReverbEngine::prepare() for every mode, so entering Spring
        never allocates (§6.2). */
    void prepare (double sampleRate)
    {
        const double k = juce::jmax (1.0, sampleRate / (2.0 * (double) kChirpFcHz));
        stretch = juce::jmax (1, (int) std::lround (k) - 1);

        // d = K - K1 in [0.5, 1.5] at every real rate; the clamp only guards absurd ones.
        const float d = juce::jlimit (0.1f, 2.0f, (float) k - (float) stretch);
        a2 = (1.0f - d) / (1.0f + d);
        totalK = (float) stretch + d;

        state.assign ((size_t) kNumSections * (size_t) (stretch + 2), 0.0f);
        write = 0;
    }

    void clear() noexcept
    {
        std::fill (state.begin(), state.end(), 0.0f);
        write = 0;
    }

    /** The cascade's group delay at DC, in samples: `M * K * (1-a1)/(1+a1)`. This is the
        delay the recirculation adds on top of `Td` at low frequency, so the loop-gain
        solve has to count it (129.7 samples = 2.70 ms at 48 kHz). */
    float lfDelaySamples() const noexcept
    {
        return (float) kNumSections * totalK * (1.0f - kA1) / (1.0f + kA1);
    }

    /** The stretched allpass wraps at `fs/K`; the useful chirp lives below `fs/2K`. */
    float chirpKneeHz (double sampleRate) const noexcept
    {
        return (float) (sampleRate / (2.0 * (double) totalK));
    }

    int stretchSamples() const noexcept { return stretch; }

    /** One sample through all M sections. No allocation, no branches in the inner loop. */
    inline float process (float x) noexcept
    {
        if (state.empty())
            return x;

        const int stride = stretch + 2;
        const int w = write;
        float* s = state.data();

        for (int m = 0; m < kNumSections; ++m, s += stride)
        {
            // D(z) = A_fd(z) * z^-K1. The ring slot holds v[n - K1]; A_fd is the
            // one-multiply first-order allpass, its two states parked past the ring.
            const float u = s[w];
            const float dOut = a2 * (u - s[stretch + 1]) + s[stretch];
            s[stretch] = u;
            s[stretch + 1] = dOut;

            // A(z) = (a1 + D)/(1 + a1*D), Schroeder form: v is what goes into the delay.
            const float v = x - kA1 * dOut;
            x = kA1 * v + dOut;
            s[w] = v;
        }

        if (++write >= stretch)
            write = 0;

        return x;
    }

private:
    // One flat block, section-major: [K1 ring slots][A_fd x1][A_fd y1] per section. At
    // 48 kHz that is 100 * 7 floats = 2.8 KB per spring, so the whole cascade stays in L1.
    std::vector<float> state;
    int   stretch = 1;          // K1
    int   write = 0;            // shared ring index, one advance per sample
    float a2 = 0.0f;            // A_fd coefficient, from frac(K)
    float totalK = 1.0f;        // K = K1 + d

    JUCE_LEAK_DETECTOR (SpringDispersion)
};
} // namespace tubamp
