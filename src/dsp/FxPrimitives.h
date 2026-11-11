#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// Small, allocation-free-after-prepare building blocks shared by the reverb and delay
// engines (docs/REVERB-DELAY.md). Everything here is header-only, single-channel and
// float; the engines own the topology. None of it locks, allocates or touches the heap
// once prepare()/setSize() has run, so all of it is safe on the audio thread.
namespace tubamp::fx
{
inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 2.0f * kPi;

/** Replaces anything that is not a sane audio sample (NaN, infinity, or absurdly large)
    with silence. One bad input sample would otherwise live in a recirculating delay
    line until the next reset. */
inline float sanitise (float x) noexcept
{
    return std::abs (x) < 1.0e6f ? x : 0.0f; // false for NaN as well
}

/** Adds a constant far below audibility so recursive filters never idle in denormal
    range. The host's ScopedNoDenormals covers this on most paths, but the engines also
    run from the offline tools, which do not set it. */
inline constexpr float kAntiDenormal = 1.0e-20f;

/** Rounds up to the next power of two (minimum 2). */
inline int nextPow2 (int n) noexcept
{
    int p = 2;

    while (p < n)
        p <<= 1;

    return p;
}

/** Cheap, monotonic, odd soft clipper: x / (1 + |x|) family rescaled so the slope at 0
    is 1 and the asymptote is ±1. Much cheaper than tanh and smooth enough inside a
    feedback loop, where the filters around it hide the remaining harmonics. */
inline float softClip (float x) noexcept
{
    // Padé-style tanh approximation, exact to ~2e-3 over [-3, 3], clamped beyond.
    const float c = juce::jlimit (-3.0f, 3.0f, x);
    const float c2 = c * c;
    return c * (27.0f + c2) / (27.0f + 9.0f * c2);
}

//==============================================================================
/** Circular buffer with integer, linear and 4-point Hermite reads.

    `read (d)` returns the sample written `d` writes ago, 0 being the most recent
    write. A loop that reads before it writes therefore sees a delay of d + 1 samples.
    Fractional reads need 1 <= delay <= capacity(). */
class DelayBuffer
{
public:
    /** Allocates for at least `maxDelaySamples` of delay. Not for the audio thread. */
    void setSize (int maxDelaySamples)
    {
        const int size = nextPow2 (maxDelaySamples + 4);
        data.assign ((size_t) size, 0.0f);
        mask = size - 1;
        writePos = 0;
    }

    void clear() noexcept
    {
        std::fill (data.begin(), data.end(), 0.0f);
    }

    int capacity() const noexcept { return mask - 3; }

    /** Samples of storage, i.e. what clear() has to touch. */
    int storageSize() const noexcept { return mask + 1; }

    void write (float x) noexcept
    {
        writePos = (writePos + 1) & mask;
        data[(size_t) writePos] = x;
    }

    /** Sample written `delay` writes ago; 0 = the most recent write. */
    float read (int delay) const noexcept
    {
        return data[(size_t) ((writePos - delay) & mask)];
    }

    float readLinear (float delay) const noexcept
    {
        const int i = (int) delay;
        const float f = delay - (float) i;
        const float a = data[(size_t) ((writePos - i) & mask)];
        const float b = data[(size_t) ((writePos - i - 1) & mask)];
        return a + f * (b - a);
    }

    /** 4-point, 3rd-order Hermite (Catmull-Rom). Flat to much higher frequencies than
        linear interpolation, which matters for modulated lines: linear interpolation's
        fraction-dependent low-pass turns delay modulation into audible HF tremolo. */
    float readHermite (float delay) const noexcept
    {
        const int i = (int) delay;
        const float f = delay - (float) i;
        const float xm1 = data[(size_t) ((writePos - i + 1) & mask)];
        const float x0 = data[(size_t) ((writePos - i) & mask)];
        const float x1 = data[(size_t) ((writePos - i - 1) & mask)];
        const float x2 = data[(size_t) ((writePos - i - 2) & mask)];
        const float c = (x1 - xm1) * 0.5f;
        const float v = x0 - x1;
        const float w = c + v;
        const float a = w + v + (x2 - x0) * 0.5f;
        const float b = w + a;
        return (((a * f) - b) * f + c) * f + x0;
    }

private:
    std::vector<float> data { 0.0f, 0.0f };
    int mask = 1;
    int writePos = 0;
};

//==============================================================================
/** Schroeder allpass around a DelayBuffer: H(z) = (-g + z^-N) / (1 - g z^-N).
    `process (x, delay)` allows the delay to move (modulated diffusers). */
class Allpass
{
public:
    void setSize (int maxDelaySamples) { buffer.setSize (maxDelaySamples + 2); }
    void clear() noexcept { buffer.clear(); }

    float process (float x, int delay, float g) noexcept
    {
        const float delayed = buffer.read (delay - 1);
        const float v = x + g * delayed;
        buffer.write (v);
        return delayed - g * v;
    }

private:
    DelayBuffer buffer;
};

//==============================================================================
/** One-pole low-pass, y += a (x - y). */
class OnePole
{
public:
    void reset (float value = 0.0f) noexcept { z = value; }

    void setCutoff (float hz, float sampleRate) noexcept
    {
        a = 1.0f - std::exp (-kTwoPi * juce::jlimit (1.0f, sampleRate * 0.49f, hz) / sampleRate);
    }

    void setCoefficient (float coefficient) noexcept { a = coefficient; }

    float lowpass (float x) noexcept
    {
        z += a * (x - z) + kAntiDenormal;
        return z;
    }

private:
    float a = 1.0f, z = 0.0f;
};

//==============================================================================
/** Topology-preserving one-pole (trapezoidal) with simultaneous LP/HP outputs. Unlike
    OnePole its cutoff is exact up to Nyquist and it stays well-behaved when the cutoff
    is modulated per sample. */
class TptOnePole
{
public:
    void reset() noexcept { s = 0.0f; }

    void setCutoff (float hz, float sampleRate) noexcept
    {
        const float g = std::tan (kPi * juce::jlimit (1.0f, sampleRate * 0.49f, hz) / sampleRate);
        G = g / (1.0f + g);
    }

    float lowpass (float x) noexcept
    {
        const float v = (x - s) * G;
        const float y = v + s;
        s = y + v + kAntiDenormal;
        return y;
    }

    float highpass (float x) noexcept { return x - lowpass (x); }

private:
    float G = 1.0f, s = 0.0f;
};

//==============================================================================
/** Transposed direct-form II biquad with RBJ cookbook designs. */
class Biquad
{
public:
    void reset() noexcept { z1 = z2 = 0.0f; }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2 + kAntiDenormal;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void setLowpass (float hz, float q, float sampleRate) noexcept
    {
        const auto [cs, alpha] = prewarp (hz, q, sampleRate);
        set ((1.0f - cs) * 0.5f, 1.0f - cs, (1.0f - cs) * 0.5f, 1.0f + alpha, -2.0f * cs, 1.0f - alpha);
    }

    void setHighpass (float hz, float q, float sampleRate) noexcept
    {
        const auto [cs, alpha] = prewarp (hz, q, sampleRate);
        set ((1.0f + cs) * 0.5f, -(1.0f + cs), (1.0f + cs) * 0.5f, 1.0f + alpha, -2.0f * cs, 1.0f - alpha);
    }

    void setPeak (float hz, float q, float gainDb, float sampleRate) noexcept
    {
        const auto [cs, alpha] = prewarp (hz, q, sampleRate);
        const float A = std::pow (10.0f, gainDb / 40.0f);
        set (1.0f + alpha * A, -2.0f * cs, 1.0f - alpha * A, 1.0f + alpha / A, -2.0f * cs, 1.0f - alpha / A);
    }

private:
    struct Warp { float cs, alpha; };

    static Warp prewarp (float hz, float q, float sampleRate) noexcept
    {
        const float w = kTwoPi * juce::jlimit (5.0f, sampleRate * 0.49f, hz) / sampleRate;
        return { std::cos (w), std::sin (w) / (2.0f * juce::jmax (0.05f, q)) };
    }

    void set (float nb0, float nb1, float nb2, float na0, float na1, float na2) noexcept
    {
        const float inv = 1.0f / na0;
        b0 = nb0 * inv; b1 = nb1 * inv; b2 = nb2 * inv;
        a1 = na1 * inv; a2 = na2 * inv;
    }

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;
};

//==============================================================================
/** Frequency-dependent loop attenuation for one recirculating delay: a broadband gain
    with first-order low and high shelves on top, so a loop of `delaySeconds` decays by
    60 dB in `t60Mid` at mid frequencies, `t60Mid * lowRatio` below `lowHz` and
    `t60Mid * highRatio` above `highHz` (Jot's absorptive-delay design, with shelves
    instead of a single one-pole so bass and treble decay are independent).

    Structure per sample: y = gMid * (x + (gLow/gMid - 1) * LP_low(x) + (gHigh/gMid - 1) * HP_high(x)). */
class DecayFilter
{
public:
    void reset() noexcept
    {
        low.reset();
        high.reset();
    }

    void setCrossovers (float lowHz, float highHz, float sampleRate) noexcept
    {
        low.setCutoff (lowHz, sampleRate);
        high.setCutoff (highHz, sampleRate);
    }

    /** `delaySeconds` is the total loop delay this filter sits in. */
    void setDecay (float delaySeconds, float t60Mid, float lowRatio, float highRatio) noexcept
    {
        const auto gainFor = [delaySeconds] (float t60)
        {
            // 10^(-3 * delay / t60): -60 dB after t60 seconds of recirculation.
            return std::pow (10.0f, -3.0f * delaySeconds / juce::jmax (0.01f, t60));
        };

        gMid = gainFor (t60Mid);
        lowExtra = gainFor (t60Mid * lowRatio) / gMid - 1.0f;
        highExtra = gainFor (t60Mid * highRatio) / gMid - 1.0f;
    }

    float process (float x) noexcept
    {
        const float lp = low.lowpass (x);
        const float hp = high.highpass (x);
        return gMid * (x + lowExtra * lp + highExtra * hp);
    }

private:
    TptOnePole low, high;
    float gMid = 0.5f, lowExtra = 0.0f, highExtra = 0.0f;
};

//==============================================================================
/** Sine LFO as a rotating phasor read through a parabolic sine approximation
    (max error 0.1%: inaudible as a delay modulator, far cheaper than std::sin). */
class SineLfo
{
public:
    void setRate (float hz, float sampleRate) noexcept { inc = hz / sampleRate; }
    void setPhase (float phase01) noexcept { phase = phase01 - std::floor (phase01); }

    /** Returns [-1, 1]. */
    float next() noexcept
    {
        phase += inc;

        if (phase >= 1.0f)
            phase -= 1.0f;

        return sineOf (phase);
    }

    /** The same oscillator read `offset01` of a cycle later, without advancing. */
    float peek (float offset01) const noexcept
    {
        float p = phase + offset01;
        p -= std::floor (p);
        return sineOf (p);
    }

    static float sineOf (float phase01) noexcept
    {
        // Parabola pair through the zero crossings and peaks, refined once.
        const float x = phase01 < 0.5f ? phase01 : phase01 - 1.0f; // [-0.5, 0.5)
        const float y = 8.0f * x * (1.0f - 2.0f * std::abs (x));
        return 0.225f * (y * std::abs (y) - y) + y;
    }

private:
    float phase = 0.0f, inc = 0.0f;
};

//==============================================================================
/** Band-limited random modulator: uniform noise through two cascaded one-poles, then
    normalised to roughly ±1. This is the "wander" that keeps a reverb tail from ever
    repeating — a periodic LFO alone produces an audible cyclic chorus on long decays. */
class RandomLfo
{
public:
    void seed (uint32_t s) noexcept { state = s != 0 ? s : 0x9E3779B9u; }

    void setRate (float hz, float sampleRate) noexcept
    {
        const float a = 1.0f - std::exp (-kTwoPi * hz / sampleRate);
        lp1.setCoefficient (a);
        lp2.setCoefficient (a);
        // Two one-poles on white noise leave variance ~ a/4 of the input's (for small a);
        // rescale so the output's RMS is ~0.5 regardless of rate.
        norm = 0.5f / std::sqrt (juce::jmax (1.0e-9f, a * 0.25f / 3.0f));
    }

    void reset() noexcept
    {
        lp1.reset();
        lp2.reset();
    }

    /** Roughly [-1, 1] (soft-limited). */
    float next() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const float white = (float) (int32_t) state * (1.0f / 2147483648.0f);
        return softClip (lp2.lowpass (lp1.lowpass (white)) * norm);
    }

private:
    uint32_t state = 0x9E3779B9u;
    OnePole lp1, lp2;
    float norm = 1.0f;
};

//==============================================================================
/** One-pole parameter smoother with a time constant in seconds. */
class Smoother
{
public:
    void prepare (float seconds, float sampleRate, float initial) noexcept
    {
        a = 1.0f - std::exp (-1.0f / juce::jmax (1.0f, seconds * sampleRate));
        current = target = initial;
    }

    void setTarget (float value) noexcept { target = value; }
    void snapToTarget() noexcept { current = target; }
    float getTarget() const noexcept { return target; }

    float next() noexcept
    {
        current += a * (target - current);
        return current;
    }

private:
    float a = 1.0f, current = 0.0f, target = 0.0f;
};

//==============================================================================
/** Click-free switching between mutually exclusive engines inside one block.

    Running two engines side by side to crossfade them would double the cost of every
    type change and needs both to be warm. Instead the effect is faded out to the
    untouched input over a few milliseconds, the incoming engine is reset while nothing
    of it is audible, and the effect fades back in:

        const int type = switcher.begin (block, requestedType,
                                         [this] (int t, bool restart) { return resetEngine (t, restart); });
        if (! switcher.isHolding())
            ... run engine `type` in place on `block` ...
        switcher.end (block);

    `resetEngine (engine, restart)` returns true once the engine is clean. It may take
    several calls — one per block, `restart` true on the first — so that clearing a
    large engine is spread out instead of landing in one callback; while it is still
    working the switcher holds the output on the dry input and the engine must not run.

    `begin` keeps a copy of the input while a switch is in flight; outside a switch it
    costs one comparison. */
class EngineSwitcher
{
public:
    void prepare (double sampleRate, int maxBlockSize, int numChannels, int initialEngine)
    {
        dry.setSize (juce::jmax (1, numChannels), juce::jmax (1, maxBlockSize), false, true, false);
        step = 1.0f / juce::jmax (1.0f, 0.02f * (float) sampleRate);
        snapTo (initialEngine);
    }

    /** True while the incoming engine is still being reset: skip processing. */
    bool isHolding() const noexcept { return holding; }

    /** Forgets any switch in flight and makes `engine` current. The caller is
        responsible for that engine being clean. */
    void snapTo (int engine) noexcept
    {
        active = engine;
        gain = 1.0f;
        direction = 0;
        resetting = holding = fading = false;
    }

    template <typename Block, typename ResetEngine>
    int begin (const Block& block, int requested, ResetEngine&& resetEngine)
    {
        const auto n = (int) block.getNumSamples();
        const auto channels = (int) block.getNumChannels();
        const bool canFade = n <= dry.getNumSamples() && channels <= dry.getNumChannels();

        if (! canFade)
        {
            // A block larger than prepare() promised cannot be faded without touching
            // the heap, so a switch that is due or in flight is completed outright —
            // and end() must not reach for the (too small) dry copy.
            fading = false;

            if (requested != active || direction != 0)
            {
                bool restart = ! resetting || resetTarget != requested;

                while (! resetEngine (requested, restart))
                    restart = false;

                snapTo (requested);
            }

            return active;
        }

        if (requested != active)
        {
            if (direction >= 0)
                direction = -1;
        }
        else if (direction < 0 && ! resetting)
        {
            // Asked back to the engine that is still running before anything was
            // reset: there is nothing to switch, so just fade back in.
            direction = 1;
        }

        if (direction < 0 && gain <= 0.0f)
        {
            // Fully faded out: nothing of the outgoing engine is audible any more.
            const bool restart = ! resetting || resetTarget != requested;
            resetting = true;
            resetTarget = requested;

            if (resetEngine (requested, restart))
            {
                resetting = false;
                active = requested;
                direction = 1;
            }
        }

        holding = resetting;
        fading = direction != 0;

        if (fading)
            for (int ch = 0; ch < channels; ++ch)
                juce::FloatVectorOperations::copy (dry.getWritePointer (ch), block.getChannelPointer ((size_t) ch), n);

        return active;
    }

    template <typename Block>
    void end (Block& block) noexcept
    {
        if (! fading)
            return;

        const auto n = (int) block.getNumSamples();
        const auto channels = (int) block.getNumChannels();
        float g = gain;

        for (int i = 0; i < n; ++i)
        {
            g = juce::jlimit (0.0f, 1.0f, g + (float) direction * step);

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* data = block.getChannelPointer ((size_t) ch);
                const float in = dry.getReadPointer (ch)[i];
                data[i] = in + g * (data[i] - in);
            }
        }

        gain = g;

        if (direction > 0 && gain >= 1.0f)
            direction = 0;
    }

private:
    juce::AudioBuffer<float> dry;
    float gain = 1.0f, step = 0.001f;
    int active = 0, resetTarget = 0;
    int direction = 0; // -1 fading the effect out, +1 fading it in, 0 steady
    bool fading = false, resetting = false, holding = false;
};

} // namespace tubamp::fx
