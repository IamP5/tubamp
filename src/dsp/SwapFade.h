#pragma once

#include <algorithm>
#include <cmath>

namespace tubamp
{
/**
    The gain ramp around replacing a DSP engine while audio runs.

    Swapping a NAM model or an impulse response between two blocks is a step in the
    waveform: the old engine's last sample and the new engine's first have nothing to
    do with each other, and the step is a click whose size is whatever the signal
    happened to be. The reference plugin mutes the engine's own output over 25 ms,
    swaps while it is silent and opens it again over another 25 ms — linear both ways
    (its kWetFadeSeconds, ChainBlock.h). This is that ramp and nothing else: who owns
    the engines, and what "swap" means, stays with the caller.

    Audio thread only. A caller drives it as

        idle ── requestOut() ──► out ── (gain reaches 0: caller swaps) ── startIn() ──► in ──► idle

    and multiplies its engine's output by next() (or applies apply()) while not idle.
*/
class SwapFade
{
public:
    /** One way. The whole splice is twice this plus whatever is left of the slice in
        which the ramp lands. */
    static constexpr double seconds = 0.025;

    enum class Phase { idle, out, in };

    void prepare (double sampleRate) noexcept
    {
        step = (float) (1.0 / std::max (1.0, seconds * sampleRate));
        snapOpen();
    }

    Phase phase() const noexcept        { return current; }
    bool isIdle() const noexcept        { return current == Phase::idle; }
    float gain() const noexcept         { return level; }

    /** Out ramp finished: the engine is silent and may be replaced. */
    bool isSilent() const noexcept      { return current == Phase::out && level <= 0.0f; }

    /** Start (or continue) closing from wherever the gain is now. */
    void requestOut() noexcept          { current = Phase::out; }

    /** Open again from wherever the gain is now — 0 after a completed out ramp. */
    void startIn() noexcept             { current = level >= 1.0f ? Phase::idle : Phase::in; }

    /** Open from silence: for an engine that replaces nothing audible. */
    void startInFromSilence() noexcept  { level = 0.0f; current = Phase::in; }

    /** No ramp at all: unity, idle. For swaps nobody can hear. */
    void snapOpen() noexcept            { level = 1.0f; current = Phase::idle; }

    /** The gain for the next sample, advancing the ramp. */
    float next() noexcept
    {
        switch (current)
        {
            case Phase::out:
                level = std::max (0.0f, level - step);
                break;

            case Phase::in:
                level = std::min (1.0f, level + step);

                if (level >= 1.0f)
                    current = Phase::idle;

                break;

            case Phase::idle:
            default:
                break;
        }

        return level;
    }

    /** Multiplies `numChannels` channels by the same ramp over `numSamples`. */
    void apply (float* const* channels, int numChannels, int numSamples) noexcept
    {
        for (int n = 0; n < numSamples; ++n)
        {
            const float g = next();

            for (int ch = 0; ch < numChannels; ++ch)
                channels[ch][n] *= g;
        }
    }

    void apply (float* mono, int numSamples) noexcept
    {
        float* channels[1] = { mono };
        apply (channels, 1, numSamples);
    }

private:
    Phase current = Phase::idle;
    float level = 1.0f;
    float step = 1.0f / 1200.0f; // 25 ms at 48 kHz until prepare() says otherwise
};
} // namespace tubamp
