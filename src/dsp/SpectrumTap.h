#pragma once

#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace tubamp
{
/**
    The latest samples of two aligned mono streams (the signal entering and leaving the
    watched EQ block), handed from the audio thread to the message thread for the
    spectrum analyser (docs/FX-ENGINES.md §2.8.1).

    Single producer (the audio thread: writePre, then writePost, for the same block),
    single consumer (the message thread: readLatest). The ring is allocated once, at
    construction; the audio side does no allocation, takes no lock and makes no system
    call. One 64-bit write position counts every sample ever written and is published
    with a release store after both streams hold the block, so whatever the reader sees
    behind that position is a complete, aligned pair.

    The read race. The writer never waits for the reader, so while the reader copies the
    newest N samples the writer may be overwriting the ring's oldest ones. It can only
    reach the samples being copied after writing capacity - N more samples (at least
    32768 with the largest FFT, High at 192 kHz), a sixth of a second of audio even
    there, during a copy that
    takes microseconds on the message thread; and if it ever did (a stalled message
    thread pre-empted mid-copy), the result is one analysis frame mixing samples about a
    ring length apart, never garbage. Each sample is a relaxed std::atomic<float>, so
    nothing tears and there is no data race in the language sense: on every target this
    builds for a relaxed float load or store is a plain move.
*/
class SpectrumTap
{
public:
    static constexpr int kCapacity = 1 << 16;

    SpectrumTap()
        : pre (std::make_unique<std::atomic<float>[]> ((size_t) kCapacity)),
          post (std::make_unique<std::atomic<float>[]> ((size_t) kCapacity))
    {
        static_assert (std::atomic<float>::is_always_lock_free);
        static_assert ((kCapacity & (kCapacity - 1)) == 0, "the index mask needs a power of two");

        for (int i = 0; i < kCapacity; ++i)
        {
            pre[(size_t) i].store (0.0f, std::memory_order_relaxed);
            post[(size_t) i].store (0.0f, std::memory_order_relaxed);
        }
    }

    //==========================================================================
    // Audio thread.

    /** The block entering the stage, mono-summed (the average of its channels). */
    void writePre (const juce::dsp::AudioBlock<float>& block) noexcept { write (pre.get(), block); }

    /** The same block leaving the stage; publishes both. Must follow writePre for a
        block of the same length. */
    void writePost (const juce::dsp::AudioBlock<float>& block) noexcept
    {
        write (post.get(), block);

        if (block.getNumChannels() == 0)
            return;

        const auto pos = writePos.load (std::memory_order_relaxed);
        writePos.store (pos + (uint64_t) block.getNumSamples(), std::memory_order_release);
    }

    //==========================================================================
    // Message thread.

    /** Copies the newest `numSamples` (at most kCapacity) of each stream, oldest first.
        Samples that were never written, or were written before the last discardOlder(),
        read as zero. Returns whether anything was written since the previous call. */
    bool readLatest (float* preOut, float* postOut, int numSamples) noexcept
    {
        numSamples = juce::jlimit (0, kCapacity, numSamples);
        const auto end = writePos.load (std::memory_order_acquire);
        const bool advanced = end != lastRead;
        lastRead = end;

        const auto start = end - (uint64_t) numSamples;   // wraps below 0 when end is small

        for (int i = 0; i < numSamples; ++i)
        {
            const auto index = start + (uint64_t) i;

            // `index > end` is the unsigned wrap: a position before the first sample.
            if (index > end || index < floor)
            {
                preOut[i] = postOut[i] = 0.0f;
                continue;
            }

            const auto slot = (size_t) (index & (uint64_t) (kCapacity - 1));
            preOut[i]  = pre [slot].load (std::memory_order_relaxed);
            postOut[i] = post[slot].load (std::memory_order_relaxed);
        }

        return advanced;
    }

    /** Everything written so far reads as zero from now on (the watched instance changed,
        so the ring holds another block's signal). Also forgets what was last read, so the
        next readLatest reports only samples written after this call as new. */
    void discardOlder() noexcept
    {
        floor = lastRead = writePos.load (std::memory_order_acquire);
    }

private:
    void write (std::atomic<float>* ring, const juce::dsp::AudioBlock<float>& block) noexcept
    {
        const auto numChannels = block.getNumChannels();
        const auto numSamples = block.getNumSamples();

        if (numChannels == 0 || numSamples == 0)
            return;

        // A block longer than the ring only leaves its tail behind, as it would anyway.
        size_t first = 0;

        if (numSamples > (size_t) kCapacity)
            first = numSamples - (size_t) kCapacity;

        // The writer is the only thread that moves writePos, so its own relaxed load is exact.
        const auto base = writePos.load (std::memory_order_relaxed);
        const float scale = 1.0f / (float) numChannels;

        for (size_t i = first; i < numSamples; ++i)
        {
            float sum = 0.0f;

            for (size_t ch = 0; ch < numChannels; ++ch)
                sum += block.getSample ((int) ch, (int) i);

            ring[(size_t) ((base + i) & (uint64_t) (kCapacity - 1))].store (sum * scale, std::memory_order_relaxed);
        }
    }

    std::unique_ptr<std::atomic<float>[]> pre, post;
    std::atomic<uint64_t> writePos { 0 };

    // Reader-only state.
    uint64_t lastRead = 0, floor = 0;
};
} // namespace tubamp
