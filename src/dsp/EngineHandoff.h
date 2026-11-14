#pragma once

#include "SwapFade.h"

#include <array>
#include <atomic>
#include <memory>
#include <utility>

namespace tubamp
{
/**
    Hands a heavyweight DSP engine — a NAM model, a convolver — from the thread that
    built it to the audio thread, and the one it replaces back, without a lock and
    without a click.

    The message thread builds an engine completely (file IO, allocation, prewarm) and
    stage()s it, or asks for the live one to go with requestRemove(). The audio thread
    calls begin() at the top of every block and gets told what happened:

        nothing   no request pending.
        swapped   the engine was replaced right now, inaudibly — the stream is only
                  starting, or the owner's block is not in the signal path (bypassed,
                  out of the chain). No ramps.
        closing   the block is audible. The fade is closing on whatever the block
                  puts out now — the old engine's signal, or the unprocessed signal
                  of a block that had no engine. The owner keeps producing that
                  through the fade and calls swapNow() on the sample fade.isSilent()
                  turns true, then fade.startIn().

    Every audible transition therefore goes through silence: engine to engine, nothing
    to engine, engine to nothing. That is the reference TONE3000 plugin's splice
    (25 ms out, swap, 25 ms in, never exposing a half-loaded engine), done with an
    atomic exchange instead of its chain mutex — and it gives the owner one silent
    sample at which anything that belongs to the engine (a gain that compensates for
    its loudness, a filter's history) can change without being heard.

    Retired engines are parked and destroyed by collectGarbage() on the message
    thread: neither a model nor a convolver may be destroyed on the audio thread.
*/
template <typename Engine>
class EngineHandoff
{
public:
    EngineHandoff() = default;

    ~EngineHandoff()
    {
        delete staged.exchange (nullptr, std::memory_order_acq_rel);
        delete live;
        collectGarbage();
    }

    //==========================================================================
    // Message thread (one caller at a time).

    /** Offers a finished engine to the audio thread. An engine staged earlier that
        the audio thread has not picked up is destroyed here, on this thread. */
    void stage (std::unique_ptr<Engine> fresh)
    {
        // A pending removal must not eat the engine about to be staged.
        shouldRemove.store (false, std::memory_order_release);
        delete staged.exchange (fresh.release(), std::memory_order_acq_rel);
    }

    /** Asks the audio thread to drop its engine; cancels anything staged. */
    void requestRemove()
    {
        delete staged.exchange (nullptr, std::memory_order_acq_rel);
        shouldRemove.store (true, std::memory_order_release);
    }

    /** Takes back an engine the audio thread has not adopted, or nullptr. Only for
        prepareToPlay, where it is re-prepared for the new rate and stage()d again. */
    std::unique_ptr<Engine> takeStaged()
    {
        return std::unique_ptr<Engine> (staged.exchange (nullptr, std::memory_order_acq_rel));
    }

    /** Destroys the engines the audio thread has retired. */
    void collectGarbage()
    {
        for (auto& slot : garbage)
            delete slot.exchange (nullptr, std::memory_order_acq_rel);
    }

    //==========================================================================
    // prepareToPlay (audio stopped).

    /** The stream restarts: a fade caught mid-way has nothing left to splice, and the
        first block adopts whatever is pending without one. */
    void restart (double sampleRate) noexcept
    {
        fade.prepare (sampleRate);
        heard = false;
        freshStream = true;
        stalledBlocks = 0;
    }

    //==========================================================================
    // Audio thread (and prepareToPlay).

    Engine* get() const noexcept            { return live; }

    /** The owner ran its block through the fade this callback. */
    void noteHeard() noexcept               { heard = true; }

    /** Something to do this block: an engine, or a fade that is not finished. */
    bool isRunning() const noexcept         { return live != nullptr || ! fade.isIdle(); }

    enum class Begin { nothing, swapped, closing };

    /** Once per block, before the owner processes. `audible` says whether the
        owner's block is in the signal path — a swap there must be faded, anywhere
        else it is simply done. `onSwap (Engine* nowLive)` runs when the live engine
        changes inside this call. */
    template <typename OnSwap>
    Begin begin (bool audible, OnSwap&& onSwap)
    {
        const bool wasHeard = std::exchange (heard, false);
        const bool fresh = std::exchange (freshStream, false);

        const bool pending = staged.load (std::memory_order_acquire) != nullptr
                             || shouldRemove.load (std::memory_order_acquire);

        if (! pending)
        {
            stalledBlocks = 0;
            return Begin::nothing;
        }

        // "Remove" with no engine and nothing staged asks for what is already the
        // case (every restore clears the slots it has no file for). Treating it as
        // a transition would fade the block's untouched signal out and back in.
        if (live == nullptr && staged.load (std::memory_order_acquire) == nullptr)
        {
            shouldRemove.store (false, std::memory_order_release);
            stalledBlocks = 0;
            return Begin::nothing;
        }

        // A fade that is closing but that nobody is running through: the owner said
        // "audible" and then did not process (an oversized block, a path that was
        // skipped). Never leave the request hanging on a ramp that does not advance.
        const bool stalled = fade.phase() == SwapFade::Phase::out && ! wasHeard
                             && ++stalledBlocks >= maxStalledBlocks;

        if (fresh || ! audible || stalled)
        {
            stalledBlocks = 0;
            fade.snapOpen();
            swapNow (onSwap);
            return Begin::swapped;
        }

        if (wasHeard)
            stalledBlocks = 0;

        fade.requestOut();
        return Begin::closing;
    }

    /** A stage() or requestRemove() the audio thread has not acted on yet. */
    bool hasPending() const noexcept
    {
        return staged.load (std::memory_order_acquire) != nullptr
               || shouldRemove.load (std::memory_order_acquire);
    }

    /** Carries out the pending request: drop the live engine, adopt the staged one,
        or both. Returns true (after calling onSwap) when the live engine changed.
        Also for prepareToPlay, where audio is stopped and whatever is staged can
        simply become the live engine. */
    template <typename OnSwap>
    bool swapNow (OnSwap&& onSwap)
    {
        bool changed = false;

        if (shouldRemove.exchange (false, std::memory_order_acq_rel) && live != nullptr)
        {
            retire (std::exchange (live, nullptr));
            changed = true;
        }

        if (auto* fresh = staged.exchange (nullptr, std::memory_order_acq_rel))
        {
            retire (std::exchange (live, fresh));
            changed = true;
        }

        if (changed)
            onSwap (live);

        return changed;
    }

    SwapFade fade;

private:
    void retire (Engine* old) noexcept
    {
        if (old == nullptr)
            return;

        for (auto& slot : garbage)
        {
            Engine* expected = nullptr;

            if (slot.compare_exchange_strong (expected, old, std::memory_order_acq_rel))
                return;
        }

        // Every slot is full: collectGarbage() has not run across four swaps. Deleting
        // here would be a real-time violation, so the engine is leaked instead. The
        // owners collect before every load and clear (at most one swap can happen in
        // between), so this does not happen.
    }

    static constexpr int numGarbageSlots = 4;

    Engine* live = nullptr;                              // audio thread
    std::atomic<Engine*> staged { nullptr };             // single-slot handoff
    std::atomic<bool> shouldRemove { false };
    std::array<std::atomic<Engine*>, numGarbageSlots> garbage {};

    static constexpr int maxStalledBlocks = 2;

    bool heard = false;
    bool freshStream = true;
    int stalledBlocks = 0;
};
} // namespace tubamp
