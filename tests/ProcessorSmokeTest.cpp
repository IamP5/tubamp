// End-to-end smoke test: drives the real TubampAudioProcessor (no editor) and
// verifies that loading a NAM model actually changes the processed audio, and that
// a user-arranged chain order (including duplicated block instances and rows)
// processes cleanly and survives state round-trips.
//   tubamp_smoke <model.nam> [sampleRate]
#include <juce_events/juce_events.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "../src/PluginProcessor.h"
#include "../src/dsp/Chorus.h"
#include "../src/dsp/Compressor.h"
#include "../src/dsp/Drive.h"
#include "../src/dsp/Eq.h"
#include "../src/dsp/Gate.h"
#include "../src/dsp/Phaser.h"
#include "../src/dsp/Tremolo.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <complex>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <tuple>
#include <vector>

#include <chrono>
#include <thread>

#include <pthread.h>

//==============================================================================
// Allocation guard for the real-time checks, the counter tubamp_fxbench --rt-check uses:
// every global operator new/delete, plus (on macOS) libmalloc's logger hook, which also
// sees malloc/free that never go through operator new. It counts only on the thread that
// armed it, so the message thread and the model loader cannot trip it.
namespace
{
std::atomic<bool> allocArmed { false };
std::atomic<pthread_t> allocThread {};
std::atomic<long> allocCount { 0 };

inline void noteAllocation() noexcept
{
    if (allocArmed.load (std::memory_order_relaxed)
        && pthread_equal (pthread_self(), allocThread.load (std::memory_order_relaxed)) != 0)
        allocCount.fetch_add (1, std::memory_order_relaxed);
}

void* allocateAligned (std::size_t size, std::size_t alignment) noexcept
{
    void* p = nullptr;
    alignment = std::max (alignment, sizeof (void*));
    return posix_memalign (&p, alignment, size != 0 ? size : 1) == 0 ? p : nullptr;
}

void* allocateOrThrow (std::size_t size)
{
    noteAllocation();

    if (void* p = std::malloc (size != 0 ? size : 1))
        return p;

    throw std::bad_alloc();
}

void* allocateAlignedOrThrow (std::size_t size, std::align_val_t alignment)
{
    noteAllocation();

    if (void* p = allocateAligned (size, (std::size_t) alignment))
        return p;

    throw std::bad_alloc();
}

void release (void* p) noexcept
{
    if (p != nullptr)
        noteAllocation();

    std::free (p);
}
} // namespace

void* operator new (std::size_t size)                                   { return allocateOrThrow (size); }
void* operator new[] (std::size_t size)                                 { return allocateOrThrow (size); }
void* operator new (std::size_t size, const std::nothrow_t&) noexcept   { noteAllocation(); return std::malloc (size != 0 ? size : 1); }
void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept { noteAllocation(); return std::malloc (size != 0 ? size : 1); }
void* operator new (std::size_t size, std::align_val_t a)               { return allocateAlignedOrThrow (size, a); }
void* operator new[] (std::size_t size, std::align_val_t a)             { return allocateAlignedOrThrow (size, a); }
void* operator new (std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept   { noteAllocation(); return allocateAligned (size, (std::size_t) a); }
void* operator new[] (std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept { noteAllocation(); return allocateAligned (size, (std::size_t) a); }
void operator delete (void* p) noexcept                                          { release (p); }
void operator delete[] (void* p) noexcept                                        { release (p); }
void operator delete (void* p, std::size_t) noexcept                             { release (p); }
void operator delete[] (void* p, std::size_t) noexcept                           { release (p); }
void operator delete (void* p, const std::nothrow_t&) noexcept                   { release (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept                 { release (p); }
void operator delete (void* p, std::align_val_t) noexcept                        { release (p); }
void operator delete[] (void* p, std::align_val_t) noexcept                      { release (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept           { release (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept         { release (p); }
void operator delete (void* p, std::align_val_t, const std::nothrow_t&) noexcept { release (p); }
void operator delete[] (void* p, std::align_val_t, const std::nothrow_t&) noexcept { release (p); }

#if JUCE_MAC
// libmalloc calls this (when set) for every allocation and free in every zone. Bit 2 =
// allocate, bit 4 = deallocate (realloc: both).
typedef void (malloc_logger_t) (uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                uintptr_t result, uint32_t numHotFramesToSkip);
extern "C" malloc_logger_t* malloc_logger;

namespace
{
void allocMallocLogger (uint32_t type, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t)
{
    if ((type & 6u) != 0)
        noteAllocation();
}
} // namespace
#endif

namespace
{
/** Counts every allocation and free the calling thread makes while one is alive. */
struct AllocationGuard
{
    AllocationGuard()
    {
       #if JUCE_MAC
        malloc_logger = allocMallocLogger;
       #endif
        allocThread.store (pthread_self(), std::memory_order_relaxed);
        allocArmed.store (true, std::memory_order_relaxed);
    }

    ~AllocationGuard()
    {
        allocArmed.store (false, std::memory_order_relaxed);
       #if JUCE_MAC
        malloc_logger = nullptr;
       #endif
    }

    AllocationGuard (const AllocationGuard&) = delete;
    AllocationGuard& operator= (const AllocationGuard&) = delete;
};
} // namespace

namespace
{
using tubamp::chain::BlockId;

bool expect (bool condition, const char* what)
{
    std::printf ("  %s  %s\n", condition ? "ok  " : "FAIL", what);
    return condition;
}

/** Runs a sine through the processor and returns the RMS of the settled tail.
    When allFinite is given it is cleared if any produced sample is not finite. */
double processSineRms (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize,
                       bool* allFinite = nullptr)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    double sumSquares = 0.0;
    long count = 0;

    for (int block = 0; block < 60; ++block)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);

            for (int i = 0; i < blockSize; ++i)
                data[i] = 0.3f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                   * 220.0 * (block * blockSize + i) / sampleRate);
        }

        proc.processBlock (buffer, midi);

        if (allFinite != nullptr)
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                for (int i = 0; i < blockSize; ++i)
                    if (! std::isfinite (buffer.getReadPointer (ch)[i]))
                        *allFinite = false;

        if (block >= 30) // skip smoothing/prewarm transients
        {
            auto* data = buffer.getReadPointer (0);

            for (int i = 0; i < blockSize; ++i, ++count)
                sumSquares += (double) data[i] * data[i];
        }
    }

    return std::sqrt (sumSquares / (double) count);
}

/** Same as processSineRms, but reports each channel's RMS separately -- used to prove
    two lanes hard-panned to opposite channels really do carry different content. */
void processSineRmsPerChannel (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize,
                               double& rmsL, double& rmsR, bool* allFinite = nullptr)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    double sumL = 0.0, sumR = 0.0;
    long count = 0;

    for (int block = 0; block < 60; ++block)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);

            for (int i = 0; i < blockSize; ++i)
                data[i] = 0.3f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                   * 220.0 * (block * blockSize + i) / sampleRate);
        }

        proc.processBlock (buffer, midi);

        if (allFinite != nullptr)
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                for (int i = 0; i < blockSize; ++i)
                    if (! std::isfinite (buffer.getReadPointer (ch)[i]))
                        *allFinite = false;

        if (block >= 30)
        {
            auto* l = buffer.getReadPointer (0);
            auto* r = buffer.getReadPointer (1);

            for (int i = 0; i < blockSize; ++i, ++count)
            {
                sumL += (double) l[i] * l[i];
                sumR += (double) r[i] * r[i];
            }
        }
    }

    rmsL = std::sqrt (sumL / (double) count);
    rmsR = std::sqrt (sumR / (double) count);
}

/** processSineRms's mono-host counterpart -- a mono buffer, not a stereo one, to match
    what withMonoHost's processor actually expects. */
double processMonoSineRms (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize,
                           bool* allFinite = nullptr)
{
    juce::AudioBuffer<float> buffer (1, blockSize);
    juce::MidiBuffer midi;
    double sumSquares = 0.0;
    long count = 0;

    for (int block = 0; block < 60; ++block)
    {
        auto* data = buffer.getWritePointer (0);

        for (int i = 0; i < blockSize; ++i)
            data[i] = 0.3f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                               * 220.0 * (block * blockSize + i) / sampleRate);

        proc.processBlock (buffer, midi);

        if (allFinite != nullptr)
            for (int i = 0; i < blockSize; ++i)
                if (! std::isfinite (buffer.getReadPointer (0)[i]))
                    *allFinite = false;

        if (block >= 30)
        {
            auto* data2 = buffer.getReadPointer (0);

            for (int i = 0; i < blockSize; ++i, ++count)
                sumSquares += (double) data2[i] * data2[i];
        }
    }

    return std::sqrt (sumSquares / (double) count);
}

void setBoolParam (tubamp::TubampAudioProcessor& proc, const char* paramId, bool value)
{
    if (auto* p = proc.apvts.getParameter (paramId))
        p->setValueNotifyingHost (value ? 1.0f : 0.0f);
}

void setFloatParam (tubamp::TubampAudioProcessor& proc, const char* paramId, float rawValue)
{
    if (auto* p = proc.apvts.getParameter (paramId))
        p->setValueNotifyingHost (p->convertTo0to1 (rawValue));
}

/** The latency a host reads once the message thread has had its turn: it is reported from
    there (a timer hands over what the audio thread applies), never from processBlock. */
int reportedLatency (tubamp::TubampAudioProcessor& proc)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (60);
    return proc.getLatencySamples();
}

/** Current choice index of a parameter, read the way the audio thread reads it. */
int choiceIndex (tubamp::TubampAudioProcessor& proc, const char* paramId)
{
    auto* raw = proc.apvts.getRawParameterValue (paramId);
    return raw != nullptr ? juce::roundToInt (raw->load()) : -1;
}

/** Pure chain::-level assertions: no processor involved. */
bool runParseTests()
{
    std::printf ("chain parsing:\n");
    namespace chain = tubamp::chain;
    bool ok = true;

    ok &= expect (chain::defaultOrder() == chain::Order { BlockId::amp },
                  "defaultOrder() is the amp alone (R1)");
    ok &= expect (chain::classicOrder() == chain::Order { BlockId::gate, BlockId::comp, BlockId::drive,
                                                          BlockId::amp, BlockId::cab, BlockId::eq,
                                                          BlockId::mod, BlockId::delay, BlockId::reverb },
                  "classicOrder() is the nine built-in blocks");

    ok &= expect (chain::parseOrder ("garbage").empty(), "parseOrder(\"garbage\") is empty, no fallback");
    ok &= expect (chain::parseOrder (chain::emptyChainToken).empty(), "parseOrder(\"-\") is empty");
    ok &= expect (chain::parseOrderOrLegacy ("") == chain::classicOrder(),
                  "parseOrderOrLegacy(\"\") falls back to classicOrder() -- persisted bytes with no token list");
    ok &= expect (chain::parseOrderOrLegacy (chain::emptyChainToken).empty(),
                  "parseOrderOrLegacy(\"-\") stays empty -- a deliberately-emptied chain is not legacy state");

    // maxChainLength-entry order (the chain length cap) round-trips through the widened
    // 128-bit word. numBlockTypes (28) now exceeds maxChainLength (24) by design --
    // split/lane2/mix/amp2 decoupled the cap from the id space (docs/SPLIT.md §1), so a
    // full order of every block instance no longer fits in one chain and this uses the
    // first maxChainLength block ids instead of "every block instance".
    chain::Order full;
    for (int i = 0; i < chain::maxChainLength; ++i)
        full.push_back (chain::blockInfos[(size_t) i].id);

    ok &= expect ((int) full.size() == chain::maxChainLength,
                  "the full order covers the chain length cap");
    ok &= expect (chain::numBlockTypes > chain::maxChainLength,
                  "...and the id space is now bigger than the cap (decoupled by design)");

    const auto packedFull = chain::pack (full);
    std::array<BlockId, chain::maxChainLength> decodedFull {};
    const int decodedFullCount = chain::unpackTo (packedFull, decodedFull);

    ok &= expect (decodedFullCount == chain::maxChainLength
                      && std::equal (full.begin(), full.end(), decodedFull.begin()),
                  "maxChainLength-entry order round-trips pack -> unpackTo (widened to __int128)");
    ok &= expect (chain::unpack (packedFull) == full, "... and pack -> unpack");

    return ok;
}

/** Split-path structure (docs/SPLIT.md §1): pure chain::-level parse/sanitize
    assertions, no processor involved. */
bool runSplitStructureTests()
{
    std::printf ("split structure:\n");
    namespace chain = tubamp::chain;
    bool ok = true;

    // 1. a well-formed split/lane2/mix triple survives sanitizeStructure untouched, and
    // findStructure locates all three at their real positions.
    const chain::Order valid { BlockId::gate, BlockId::split, BlockId::comp, BlockId::lane2,
                               BlockId::amp2, BlockId::mix, BlockId::cab };
    const auto sanitisedValid = chain::sanitizeStructure (valid);
    ok &= expect (sanitisedValid == valid, "a well-formed split/lane2/mix triple round-trips untouched");

    const auto structure = chain::findStructure (sanitisedValid);
    ok &= expect (structure.valid() && structure.splitAt == 1 && structure.lane2At == 3
                      && structure.mixAt == 5,
                  "findStructure locates split/lane2/mix at their real positions");

    // 2. a dangling split (no lane2, no mix anywhere) flattens to plain serial.
    const chain::Order danglingSplit { BlockId::gate, BlockId::split, BlockId::amp };
    ok &= expect (chain::sanitizeStructure (danglingSplit) == chain::Order { BlockId::gate, BlockId::amp },
                  "a dangling split with no lane2/mix flattens to plain serial");

    // 3. mix ahead of split flattens too (the triple is out of order).
    const chain::Order mixBeforeSplit { BlockId::mix, BlockId::gate, BlockId::split,
                                        BlockId::lane2, BlockId::amp };
    ok &= expect (chain::sanitizeStructure (mixBeforeSplit) == chain::Order { BlockId::gate, BlockId::amp },
                  "mix ahead of split flattens to plain serial (structural ids stripped)");

    // 4. a missing lane2 (split ... mix with nothing in between) also flattens.
    const chain::Order noLane2 { BlockId::split, BlockId::amp, BlockId::mix };
    ok &= expect (chain::sanitizeStructure (noLane2) == chain::Order { BlockId::amp },
                  "split...mix with no lane2 flattens to plain serial");

    // 5. amp2 is NOT structural -- it survives a flatten wherever it sat.
    const chain::Order amp2InDangling { BlockId::split, BlockId::amp2, BlockId::amp };
    ok &= expect (chain::sanitizeStructure (amp2InDangling) == chain::Order { BlockId::amp2, BlockId::amp },
                  "amp2 survives flattening -- only split/lane2/mix are stripped");

    return ok;
}

/** A never-persisted processor pins R1: it must run (and save) with only the amp. */
bool runFreshInstanceTest (double sampleRate, int blockSize)
{
    std::printf ("fresh instance:\n");
    bool ok = true;

    tubamp::TubampAudioProcessor freshProc;
    freshProc.prepareToPlay (sampleRate, blockSize);

    ok &= expect (tubamp::chain::toString (freshProc.getChainOrder()) == "amp",
                  "a fresh instance's in-memory order is exactly \"amp\"");

    juce::MemoryBlock blob;
    freshProc.getStateInformation (blob);
    auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), (int) blob.getSize());

    ok &= expect (xml != nullptr && xml->getStringAttribute ("chainOrder") == "amp",
                  "a fresh instance's saved state carries chainOrder \"amp\"");

    return ok;
}

/** Chain order: publish -> process -> save/restore, including legacy state that
    predates the chainOrder property and the per-order row layout. */
bool runChainOrderTests (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    std::printf ("chain order:\n");

    namespace chain = tubamp::chain;
    const auto classicText = chain::toString (chain::classicOrder());
    bool ok = true;

    // Drive ahead of the compressor, reverb ahead of the delay, gate removed entirely.
    const chain::Order custom { BlockId::drive, BlockId::comp, BlockId::amp,
                               BlockId::cab,    BlockId::eq,   BlockId::mod,
                               BlockId::reverb, BlockId::delay };
    const auto customText = chain::toString (custom);
    const std::vector<int> customRows { 4, 4 };

    int changeNotifications = 0;
    proc.onChainChanged = [&changeNotifications] { ++changeNotifications; };

    proc.setChainOrder (custom, customRows);
    ok &= expect (changeNotifications == 1, "setChainOrder fires onChainChanged");
    ok &= expect (chain::toString (proc.getChainOrder()) == customText,
                  "getChainOrder reflects the published order");
    ok &= expect (proc.getChainRows() == customRows, "getChainRows reflects a valid, matching row layout");

    setBoolParam (proc, params::driveOn, true); // off by default; exercise the moved block

    bool finite = true;
    const double rms = processSineRms (proc, sampleRate, blockSize, &finite);
    ok &= expect (finite, "custom order produces only finite samples");
    ok &= expect (std::isfinite (rms) && rms > 1.0e-5, "custom order produces non-silent output");

    // --- binary state round-trip (host save/load)
    juce::MemoryBlock blob;
    proc.getStateInformation (blob);

    proc.setChainOrder (chain::defaultOrder(), {});
    changeNotifications = 0;
    proc.setStateInformation (blob.getData(), (int) blob.getSize());
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

    ok &= expect (chain::toString (proc.getChainOrder()) == customText,
                  "order survives getStateInformation -> setStateInformation");
    ok &= expect (proc.getChainRows() == customRows, "rows survive getStateInformation -> setStateInformation");
    ok &= expect (changeNotifications >= 1, "state restore fires onChainChanged");

    // --- legacy state: same blob with the chainOrder property stripped out. The rows
    // property ("4,4") survives untouched, so this also proves a stale row layout that
    // no longer matches the recovered order falls back to auto rather than being kept.
    if (auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), (int) blob.getSize()))
    {
        xml->removeAttribute ("chainOrder");

        juce::MemoryBlock legacyBlob;
        juce::AudioProcessor::copyXmlToBinary (*xml, legacyBlob);

        proc.setChainOrder (custom, customRows);
        proc.setStateInformation (legacyBlob.getData(), (int) legacyBlob.getSize());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        ok &= expect (chain::toString (proc.getChainOrder()) == classicText,
                      "legacy state without chainOrder restores classicOrder(), not just the amp");
        ok &= expect (proc.getChainRows().empty(),
                      "...and a stale row layout that no longer matches falls back to auto");
    }
    else
    {
        ok &= expect (false, "legacy state blob could not be rebuilt");
    }

    // --- preset / A-B slot round-trip (PresetManager goes through these two)
    proc.setChainOrder (custom, customRows);
    const auto slotState = proc.captureStateVar();

    proc.setChainOrder (chain::defaultOrder(), {});
    proc.applyStateVar (slotState);
    ok &= expect (chain::toString (proc.getChainOrder()) == customText,
                  "order survives captureStateVar -> applyStateVar");
    ok &= expect (proc.getChainRows() == customRows, "rows survive captureStateVar -> applyStateVar");

    if (auto* obj = slotState.getDynamicObject())
    {
        obj->removeProperty ("chainOrder"); // a preset saved before the chain was arrangeable
        proc.setChainOrder (custom, customRows);
        proc.applyStateVar (slotState);

        ok &= expect (chain::toString (proc.getChainOrder()) == classicText,
                      "legacy preset without chainOrder restores classicOrder(), not just the amp");
        ok &= expect (proc.getChainRows().empty(), "...with the stale rows falling back to auto too");
    }

    // --- a deliberately-empty chain must round-trip, not revert to the default
    proc.setChainOrder ({}, {});
    const auto emptyState = proc.captureStateVar();

    proc.setChainOrder (custom, customRows);
    proc.applyStateVar (emptyState);
    ok &= expect (proc.getChainOrder().empty(),
                  "empty chain survives captureStateVar -> applyStateVar");
    ok &= expect (proc.getChainRows().empty(), "...with empty rows alongside it");

    proc.onChainChanged = nullptr;
    proc.setChainOrder (chain::defaultOrder(), {});
    setBoolParam (proc, params::driveOn, false);

    return ok;
}

/** Block-instance model: duplicated kinds, per-order row validation and the
    chainOrderV2 forward-compat split. */
bool runInstanceModelTests (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    std::printf ("block instances:\n");

    namespace chain = tubamp::chain;
    bool ok = true;

    // --- three compressor instances in one chain
    const chain::Order instanced { BlockId::gate, BlockId::comp, BlockId::comp2,
                                   BlockId::comp3, BlockId::amp,  BlockId::cab };
    const auto instancedText = chain::toString (instanced);
    const std::vector<int> instancedRows { 3, 3 };

    proc.setChainOrder (instanced, instancedRows);
    ok &= expect (chain::toString (proc.getChainOrder()) == instancedText,
                  "a chain with comp, comp2 and comp3 is accepted as-is");
    ok &= expect (proc.getChainRows() == instancedRows, "its valid rows are kept verbatim");

    bool finite = true;
    const double rms = processSineRms (proc, sampleRate, blockSize, &finite);
    ok &= expect (finite, "three compressor instances produce only finite samples");
    ok &= expect (std::isfinite (rms) && rms > 1.0e-5, "...and non-silent output");

    // --- state round-trip carries the instance tokens and rows
    juce::MemoryBlock blob;
    proc.getStateInformation (blob);

    proc.setChainOrder (chain::defaultOrder(), {});
    proc.setStateInformation (blob.getData(), (int) blob.getSize());
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

    ok &= expect (chain::toString (proc.getChainOrder()) == instancedText
                      && proc.getChainRows() == instancedRows,
                  "instance tokens and rows survive getStateInformation -> setStateInformation");

    // --- preset (captureStateVar/applyStateVar) round-trip
    proc.setChainOrder (instanced, instancedRows);
    const auto presetState = proc.captureStateVar();

    proc.setChainOrder (chain::defaultOrder(), {});
    proc.applyStateVar (presetState);

    ok &= expect (chain::toString (proc.getChainOrder()) == instancedText
                      && proc.getChainRows() == instancedRows,
                  "instance tokens and rows survive captureStateVar -> applyStateVar");

    // --- A/B slot round-trip (PresetManager wraps the same two calls)
    proc.setChainOrder (instanced, instancedRows);
    proc.presets.captureToSlot (0);

    proc.setChainOrder (chain::defaultOrder(), {});
    proc.presets.recallSlot (0);

    ok &= expect (chain::toString (proc.getChainOrder()) == instancedText
                      && proc.getChainRows() == instancedRows,
                  "instance tokens and rows survive an A/B slot round-trip");

    // --- invalid rows fall back to auto, never to a mangled layout
    proc.setChainOrder (instanced, { 0, 9 });
    ok &= expect (proc.getChainRows().empty(), "a zero-length row falls back to auto");

    proc.setChainOrder (instanced, { -1, 10 });
    ok &= expect (proc.getChainRows().empty(), "a negative-length row falls back to auto");

    proc.setChainOrder (instanced, { 2, 2 }); // instanced.size() == 6, this sums to 4
    ok &= expect (proc.getChainRows().empty(), "rows that do not sum to the order size fall back to auto");

    // --- chainOrderV2: an order with no v1-known id writes "-" to chainOrder and the
    // real order to chainOrderV2; the reader prefers chainOrderV2 whenever it is there.
    const chain::Order allNewInstances { BlockId::comp2, BlockId::eq3, BlockId::reverb2 };
    proc.setChainOrder (allNewInstances, {});

    juce::MemoryBlock v2Blob;
    proc.getStateInformation (v2Blob);
    auto xml = juce::AudioProcessor::getXmlFromBinary (v2Blob.getData(), (int) v2Blob.getSize());

    const bool v2XmlParsed = expect (xml != nullptr,
                                     "an all-instance-2/3 order still produces a parseable state blob");
    ok &= v2XmlParsed;

    if (v2XmlParsed)
    {
        ok &= expect (xml->getStringAttribute ("chainOrder") == chain::emptyChainToken,
                      "...chainOrder is the empty-chain sentinel (no v1-known id survives)");

        const auto v2Text = xml->getStringAttribute ("chainOrderV2");
        ok &= expect (chain::parseOrder (v2Text) == allNewInstances,
                      "...and chainOrderV2 carries the real order");

        // A stale-looking v1 token alongside the real v2 payload: the reader must
        // prefer chainOrderV2, never fall back to (or be fooled by) chainOrder.
        xml->setAttribute ("chainOrder", "amp");
        juce::MemoryBlock craftedBlob;
        juce::AudioProcessor::copyXmlToBinary (*xml, craftedBlob);

        proc.setChainOrder (chain::defaultOrder(), {});
        proc.setStateInformation (craftedBlob.getData(), (int) craftedBlob.getSize());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        ok &= expect (chain::toString (proc.getChainOrder()) == chain::toString (allNewInstances),
                      "setStateInformation prefers chainOrderV2 over a stale chainOrder");
    }

    proc.setChainOrder (chain::defaultOrder(), {});
    return ok;
}

/** Runs the reference sine through the processor as it currently stands, appending
    every produced sample to `captured` when that is non-null. */
void runSine (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize,
              int numBlocks, std::vector<float>* captured)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;

    for (int block = 0; block < numBlocks; ++block)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);

            for (int i = 0; i < blockSize; ++i)
                data[i] = 0.3f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                   * 220.0 * (block * blockSize + i) / sampleRate);
        }

        proc.processBlock (buffer, midi);

        if (captured != nullptr)
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto* data = buffer.getReadPointer (ch);
                captured->insert (captured->end(), data, data + blockSize);
            }
    }
}

/** Renders the reference sine through `order` and returns every produced sample.
    prepareToPlay resets the chain first, so two renders of the same order come out
    bit-identical and an exact comparison between two orders is meaningful. (The one
    block that does NOT reset bit-exactly is the NAM amp — see runFxSlotTests.) */
std::vector<float> renderChain (tubamp::TubampAudioProcessor& proc,
                                const tubamp::chain::Order& order,
                                double sampleRate, int blockSize, int numBlocks = 40)
{
    proc.setChainOrder (order, {});
    proc.prepareToPlay (sampleRate, blockSize);

    std::vector<float> captured;
    captured.reserve ((size_t) numBlocks * 2 * (size_t) blockSize);
    runSine (proc, sampleRate, blockSize, numBlocks, &captured);

    return captured;
}

/** Index of the first sample that differs, or -1 when the two renders are identical.
    Deliberately an exact float comparison, not a tolerance: an empty FX slot must not
    touch the buffer at all, so anything other than bit-identical output is a bug. */
long firstDifference (const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size())
        return 0;

    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return (long) i;

    return -1;
}

/** expect() for two renders, printing the offending sample when they differ. */
bool expectIdentical (const std::vector<float>& a, const std::vector<float>& b, const char* what)
{
    const long index = firstDifference (a, b);

    if (index >= 0 && a.size() == b.size())
        std::printf ("        first mismatch at sample %ld of %ld: %.9g vs %.9g\n",
                     index, (long) a.size(), (double) a[(size_t) index], (double) b[(size_t) index]);
    else if (index >= 0)
        std::printf ("        render lengths differ: %ld vs %ld\n", (long) a.size(), (long) b.size());

    return expect (index < 0, what);
}

/** Amp EQ (R2): the tone stack that lives inside the amp block, on the mono path
    between the model and amp-out. Exercised with no model loaded -- NamEngine::process
    is then a pass-through, so the sine reaching the EQ is unfiltered and any change is
    the EQ's alone. */
bool runAmpEqTests (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    std::printf ("amp eq:\n");

    namespace chain = tubamp::chain;
    bool ok = true;
    const chain::Order ampOnly { BlockId::amp };

    setBoolParam (proc, params::ampEqOn, true);
    setFloatParam (proc, params::ampEqBass, 5.0f);
    setFloatParam (proc, params::ampEqMid, 5.0f);
    setFloatParam (proc, params::ampEqTreble, 5.0f);
    const auto flat = renderChain (proc, ampOnly, sampleRate, blockSize);

    setFloatParam (proc, params::ampEqBass, 10.0f);
    const auto boosted = renderChain (proc, ampOnly, sampleRate, blockSize);

    ok &= expect (firstDifference (flat, boosted) >= 0,
                  "boosting amp EQ bass changes the amp block's output");

    setFloatParam (proc, params::ampEqBass, 5.0f);

    // Off and on-at-flat are the same audio (FX-ENGINES.md D2 a: the tone stack at exactly
    // 5/5/5 does not touch the buffer). Compared without a model: a resampling model does
    // not reset bit-exactly between two renders (see renderChain).
    {
        tubamp::TubampAudioProcessor fresh;
        setBoolParam (fresh, params::ampEqOn, false);
        const auto off = renderChain (fresh, ampOnly, sampleRate, blockSize);
        setBoolParam (fresh, params::ampEqOn, true);
        const auto flatOn = renderChain (fresh, ampOnly, sampleRate, blockSize);
        ok &= expectIdentical (off, flatOn, "amp EQ off and on-at-flat are bit-identical");

        setFloatParam (fresh, params::ampEqTreble, 5.1f);
        const auto near = renderChain (fresh, ampOnly, sampleRate, blockSize);
        ok &= expect (firstDifference (off, near) >= 0, "...while 5/5/5.1 runs the filters (control)");
    }

    setFloatParam (proc, params::ampEqMid, 5.0f);
    setFloatParam (proc, params::ampEqTreble, 5.0f);
    proc.setChainOrder (chain::defaultOrder(), {});
    proc.prepareToPlay (sampleRate, blockSize);

    return ok;
}

/** Factory presets (PresetManager::createFactoryPresetsIfMissing) stage classicOrder()
    around every capture, so none of them should freeze in as just the amp. Runs against
    its own processor -- a fresh instance with no model/IR loaded, matching what a real
    first-run capture looks like -- rather than the shared `proc`, which may already
    have a model path that has no business inside a factory preset. */
bool runFactoryPresetTests (double sampleRate, int blockSize)
{
    std::printf ("factory presets:\n");

    namespace chain = tubamp::chain;
    bool ok = true;

    tubamp::TubampAudioProcessor freshProc;
    freshProc.prepareToPlay (sampleRate, blockSize);
    freshProc.presets.createFactoryPresetsIfMissing();

    const auto classicText = chain::toString (chain::classicOrder());
    const juce::StringArray factoryNames { "Init", "Clean + Room", "Crunch Drive", "Ambient Lead" };

    for (const auto& name : factoryNames)
    {
        juce::File found;

        for (const auto& info : freshProc.presets.getPresets())
            if (info.name == name)
                found = info.file;

        char what[128];
        std::snprintf (what, sizeof (what), "\"%s\" factory preset exists on disk", name.toRawUTF8());

        if (! expect (found.existsAsFile(), what))
        {
            ok = false;
            continue;
        }

        const auto result = freshProc.presets.loadPreset (found);
        std::snprintf (what, sizeof (what), "\"%s\" loads cleanly", name.toRawUTF8());
        ok &= expect (result.wasOk(), what);

        const auto orderText = chain::toString (freshProc.getChainOrder());

        std::snprintf (what, sizeof (what), "\"%s\" chainOrder is the classic nine", name.toRawUTF8());
        ok &= expect (orderText == classicText, what);

        std::snprintf (what, sizeof (what), "\"%s\" chainOrder is not just \"amp\"", name.toRawUTF8());
        ok &= expect (orderText != "amp", what);

        // Written with every engine type at Classic (docs/FX-ENGINES.md §8 D-2), or by a
        // build without the types and pinned on load: Classic either way.
        bool classicTypes = choiceIndex (freshProc, params::gateMode) == 0
                         && choiceIndex (freshProc, params::compType) == 0
                         && choiceIndex (freshProc, params::driveType) == 0
                         && choiceIndex (freshProc, params::eqType) == 0
                         && choiceIndex (freshProc, params::modChorusMode) == 0
                         && choiceIndex (freshProc, params::modPhaserModel) == 0
                         && choiceIndex (freshProc, params::modTremVoice) == 0;

        std::snprintf (what, sizeof (what), "\"%s\" runs every engine on its Classic type", name.toRawUTF8());
        ok &= expect (classicTypes, what);
    }

    return ok;
}

/** Hammers getStateInformation from a non-message thread, the way a host autosave
    does, and checks every blob it gets back is parseable. */
class StateSaverThread final : public juce::Thread
{
public:
    explicit StateSaverThread (tubamp::TubampAudioProcessor& p)
        : juce::Thread ("smoke-state-saver"), proc (p) {}

    void run() override
    {
        while (! threadShouldExit() && saves < 40)
        {
            juce::MemoryBlock blob;
            proc.getStateInformation (blob);

            auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), (int) blob.getSize());

            // hasAttribute is the whole predicate: "-" is the legal, deliberate
            // empty-chain sentinel, not a save gone wrong (review catch).
            if (xml == nullptr || ! xml->hasTagName ("TUBAMP") || ! xml->hasAttribute ("chainOrder"))
            {
                bad = true;
                return;
            }

            ++saves;
        }
    }

    tubamp::TubampAudioProcessor& proc;
    std::atomic<int> saves { 0 };
    std::atomic<bool> bad { false };
};

/** Publishes a rotating full-width order into a plain atomic word on its own thread --
    the same "one whole-value store" contract packedChain has in the processor, but
    decoupled from TubampAudioProcessor so the encoding itself can be hammered directly
    without reaching into a private member. */
class ChainWriterThread final : public juce::Thread
{
public:
    explicit ChainWriterThread (std::atomic<tubamp::chain::Packed>& target)
        : juce::Thread ("smoke-chain-writer"), packed (target) {}

    void run() override
    {
        namespace chain = tubamp::chain;
        int spin = 0;

        while (! threadShouldExit())
        {
            chain::Order order;

            for (int i = 0; i < chain::maxChainLength; ++i)
                order.push_back ((BlockId) ((i + spin) % chain::numBlockTypes));

            packed.store (chain::pack (order), std::memory_order_relaxed);
            ++spin;
        }
    }

    std::atomic<tubamp::chain::Packed>& packed;
};

/** Two-thread publish + reader hammer on the widened 128-bit packed word: every decode
    the reader observes must stay within bounds, whatever half-formed spin of the writer
    it happens to catch (it can never catch a torn value -- the atomic makes that
    impossible -- but the decode's own clamps are what this proves). */
bool runConcurrentPackingTest()
{
    std::printf ("concurrent packing:\n");

    namespace chain = tubamp::chain;
    bool ok = true;

    std::atomic<chain::Packed> packed { chain::pack (chain::defaultOrder()) };
    ChainWriterThread writer (packed);
    writer.startThread();

    bool boundsHeld = true;
    std::array<BlockId, chain::maxChainLength> decoded {};

    for (int i = 0; i < 200000 && boundsHeld; ++i)
    {
        const int count = chain::unpackTo (packed.load (std::memory_order_relaxed), decoded);

        if (count < 0 || count > chain::numBlockTypes)
            boundsHeld = false;

        for (int n = 0; n < count && boundsHeld; ++n)
            if ((int) decoded[(size_t) n] < 0 || (int) decoded[(size_t) n] >= chain::numBlockTypes)
                boundsHeld = false;
    }

    const bool stopped = writer.stopThread (2000);
    ok &= expect (stopped, "the writer thread stops cleanly");
    ok &= expect (boundsHeld, "every concurrent decode stays within count <= 24 and ids < 24");

    return ok;
}

/** External AudioUnit slots. This target is built without JUCE_PLUGINHOST_AU, so no
    real plugin can ever be instantiated here — everything below deliberately covers
    the empty-slot behaviour the rest of the design rests on. */
bool runFxSlotTests (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    std::printf ("fx slots:\n");

    namespace chain = tubamp::chain;
    bool ok = true;

    // --- 8. a fresh (never-loaded) slot reports nothing at all
    for (int slot = 0; slot < chain::numFxSlots; ++slot)
    {
        const auto info = proc.getFxSlotInfo (slot);
        char what[96];
        std::snprintf (what, sizeof (what), "slot %d starts unoccupied, not live, no error", slot);
        ok &= expect (! info.occupied && ! info.live && ! info.loading && ! info.missing
                          && info.error.isEmpty() && info.identifier.isEmpty()
                          && info.latencySamples == 0,
                      what);
    }

    ok &= expect (proc.getFxInstance (0) == nullptr && proc.getFxInstance (1) == nullptr
                      && proc.getFxInstance (2) == nullptr,
                  "no slot has a live instance without a host that can instantiate one");

    // --- 1. packing / stringifying a full 12-entry order, fx slots at head/middle/tail
    const chain::Order withoutFx { BlockId::gate, BlockId::comp, BlockId::drive, BlockId::amp,
                                   BlockId::cab,  BlockId::eq,   BlockId::mod,   BlockId::delay,
                                   BlockId::reverb };
    const chain::Order withFx { BlockId::fx1,  BlockId::gate, BlockId::comp,  BlockId::drive,
                                BlockId::amp,  BlockId::fx2,  BlockId::cab,   BlockId::eq,
                                BlockId::mod,  BlockId::delay, BlockId::reverb, BlockId::fx3 };

    ok &= expect ((int) withFx.size() == chain::numV1BlockIds,
                  "the fx test order uses every one of the twelve v1 block ids");

    const auto packed = chain::pack (withFx);
    std::array<BlockId, chain::maxChainLength> decoded {};
    const int decodedCount = chain::unpackTo (packed, decoded);

    ok &= expect (decodedCount == (int) withFx.size()
                      && std::equal (withFx.begin(), withFx.end(), decoded.begin()),
                  "12-entry order with fx slots survives pack -> unpackTo");
    ok &= expect (chain::unpack (packed) == withFx, "... and pack -> unpack");

    const auto withFxText = chain::toString (withFx);
    ok &= expect (withFxText.contains ("fx1") && withFxText.contains ("fx2")
                      && withFxText.contains ("fx3"),
                  "toString emits the fx1/fx2/fx3 tokens");
    ok &= expect (chain::parseOrder (withFxText) == withFx,
                  "12-entry order with fx slots survives toString -> parseOrder");

    // --- 2. the legacy-state contract: no fx tokens anywhere near classicOrder(), and
    // a fresh instance's default carries none either.
    bool defaultHasFx = false;

    for (auto id : chain::defaultOrder())
        defaultHasFx |= chain::isFxSlot (id);

    ok &= expect (! defaultHasFx, "defaultOrder() contains no fx slots");

    bool classicHasFx = false;

    for (auto id : chain::classicOrder())
        classicHasFx |= chain::isFxSlot (id);

    ok &= expect (! classicHasFx, "classicOrder() contains no fx slots");
    ok &= expect (chain::classicOrder() == withoutFx, "classicOrder() is exactly the nine built-in blocks");
    ok &= expect (chain::parseOrderOrLegacy ("") == withoutFx,
                  "parseOrderOrLegacy(\"\") still falls back to the nine built-in blocks");

    // --- 3. an id this build does not know must be DROPPED, never clamped onto fx3
    {
        chain::Order bogus { BlockId::gate, (BlockId) 99, BlockId::amp };
        proc.setChainOrder (bogus, {});

        const auto sanitised = proc.getChainOrder();
        bool anyFx = false;

        for (auto id : sanitised)
            anyFx |= chain::isFxSlot (id);

        ok &= expect (sanitised == chain::Order { BlockId::gate, BlockId::amp },
                      "sanitiseOrder drops an out-of-range id");
        ok &= expect (! anyFx, "... rather than clamping it onto fx3");
    }

    // --- 5. latency (checked before the renders, which call prepareToPlay)
    proc.setChainOrder (withoutFx, {});
    const int latencyWithoutFx = proc.getLatencySamples();
    proc.setChainOrder (withFx, {});
    const int latencyWithFx = proc.getLatencySamples();

    std::printf ("  latency: withoutFx=%d withFx=%d\n", latencyWithoutFx, latencyWithFx);
    ok &= expect (latencyWithFx == latencyWithoutFx,
                  "empty fx slots do not change the reported latency");

    // Repeat at a rate the model has to be resampled to, so the baseline latency is
    // non-zero and the comparison above is not just 0 == 0.
    {
        const double otherRate = sampleRate == 44100.0 ? 48000.0 : 44100.0;
        proc.prepareToPlay (otherRate, blockSize);

        proc.setChainOrder (withoutFx, {});
        const int resampledWithoutFx = proc.getLatencySamples();
        proc.setChainOrder (withFx, {});
        const int resampledWithFx = proc.getLatencySamples();

        std::printf ("  latency @%.0f: withoutFx=%d withFx=%d\n",
                     otherRate, resampledWithoutFx, resampledWithFx);
        ok &= expect (resampledWithFx == resampledWithoutFx,
                      "... including when the NAM resampler contributes latency");

        proc.prepareToPlay (sampleRate, blockSize);
    }

    // --- 4. an empty slot must be a bit-exact pass-through
    //
    // The NAM amp is deliberately absent from the two chains compared here. It is the
    // one block whose prepareToPlay is not a bit-reproducible reset: when the model
    // has to be resampled to the host rate (any rate other than the model's own), a
    // decaying remnant of the previous render survives into the next one, so two
    // renders of the *same* chain already differ by ~1e-6. That is a NamEngine
    // property, unrelated to the fx slots, and including it here would only make this
    // assertion measure it. Every other block resets exactly, and the "control"
    // assertion below proves it before the comparison is trusted.
    const chain::Order renderNoFx { BlockId::gate, BlockId::comp, BlockId::drive, BlockId::cab,
                                    BlockId::eq,   BlockId::mod,  BlockId::delay, BlockId::reverb };
    const chain::Order renderFx   { BlockId::fx1,  BlockId::gate, BlockId::comp,  BlockId::drive,
                                    BlockId::cab,  BlockId::fx2,  BlockId::eq,    BlockId::mod,
                                    BlockId::delay, BlockId::reverb, BlockId::fx3 };

    const auto baseline = renderChain (proc, renderNoFx, sampleRate, blockSize);
    const auto control  = renderChain (proc, renderNoFx, sampleRate, blockSize);

    // Control: two identical renders must already match exactly, otherwise the
    // comparison below would be measuring the harness rather than the fx slots.
    ok &= expectIdentical (baseline, control,
                           "control: the same chain rendered twice is bit-identical");

    // Sensitivity: the identical-render checks below prove nothing unless the same
    // comparison detects a chain that really did change.
    chain::Order minusReverb = renderNoFx;
    minusReverb.pop_back();

    ok &= expect (firstDifference (baseline, renderChain (proc, minusReverb, sampleRate, blockSize)) >= 0,
                  "sensitivity: dropping a real block from the chain does change the audio");

    ok &= expectIdentical (baseline, renderChain (proc, renderFx, sampleRate, blockSize),
                           "empty fx slots leave the audio bit-identical (exact float compare)");

    // A second arrangement: three empty slots back-to-back at the very end.
    chain::Order trailingFx = renderNoFx;
    trailingFx.insert (trailingFx.end(), { BlockId::fx1, BlockId::fx2, BlockId::fx3 });

    ok &= expectIdentical (baseline, renderChain (proc, trailingFx, sampleRate, blockSize),
                           "three trailing empty fx slots leave the audio bit-identical");

    // The full 12-block chain (fx slots *and* the NAM amp) still has to run cleanly;
    // it just cannot be compared sample-for-sample, for the reason above.
    {
        proc.setChainOrder (withFx, {});
        proc.prepareToPlay (sampleRate, blockSize);

        bool finite = true;
        const double rms = processSineRms (proc, sampleRate, blockSize, &finite);

        ok &= expect (finite && std::isfinite (rms) && rms > 1.0e-5,
                      "the full 12-block chain incl. fx slots produces finite, non-silent audio");
    }

    // --- 6. state round-trips carrying fx tokens in the order
    const auto withFxText2 = chain::toString (withFx);
    proc.setChainOrder (withFx, {});

    juce::MemoryBlock blob;
    proc.getStateInformation (blob);

    proc.setChainOrder (chain::defaultOrder(), {});
    proc.setStateInformation (blob.getData(), (int) blob.getSize());
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

    ok &= expect (chain::toString (proc.getChainOrder()) == withFxText2,
                  "fx tokens survive getStateInformation -> setStateInformation");

    proc.setChainOrder (withFx, {});
    const auto slotState = proc.captureStateVar();

    proc.setChainOrder (chain::defaultOrder(), {});
    proc.applyStateVar (slotState);

    ok &= expect (chain::toString (proc.getChainOrder()) == withFxText2,
                  "fx tokens survive captureStateVar -> applyStateVar");

    // --- 7. legacy tolerance: state written before the fx slots existed
    if (auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), (int) blob.getSize()))
    {
        if (auto* fxChild = xml->getChildByName ("FXSLOTS"))
            xml->removeChildElement (fxChild, true);

        ok &= expect (xml->getChildByName ("FXSLOTS") == nullptr,
                      "legacy blob really has no FXSLOTS child");

        juce::MemoryBlock legacyBlob;
        juce::AudioProcessor::copyXmlToBinary (*xml, legacyBlob);

        proc.setChainOrder (chain::defaultOrder(), {});
        proc.setStateInformation (legacyBlob.getData(), (int) legacyBlob.getSize());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        bool slotsClean = true;

        for (int slot = 0; slot < chain::numFxSlots; ++slot)
            slotsClean &= ! proc.getFxSlotInfo (slot).occupied;

        ok &= expect (chain::toString (proc.getChainOrder()) == withFxText2 && slotsClean,
                      "state with no FXSLOTS child restores cleanly");
    }
    else
    {
        ok &= expect (false, "legacy fx state blob could not be rebuilt");
    }

    if (auto* obj = slotState.getDynamicObject())
    {
        obj->removeProperty ("fxSlots"); // a preset saved before the fx slots existed

        proc.setChainOrder (chain::defaultOrder(), {});
        proc.applyStateVar (slotState);

        bool slotsClean = true;

        for (int slot = 0; slot < chain::numFxSlots; ++slot)
            slotsClean &= ! proc.getFxSlotInfo (slot).occupied;

        ok &= expect (chain::toString (proc.getChainOrder()) == withFxText2 && slotsClean,
                      "preset var with no fxSlots property restores cleanly");
    }
    else
    {
        ok &= expect (false, "captureStateVar did not produce an object");
    }

    // --- 9. host autosave: getStateInformation off the message thread while the
    // audio thread is rendering.
    {
        proc.setChainOrder (withFx, {});
        proc.prepareToPlay (sampleRate, blockSize);

        StateSaverThread saver (proc);
        saver.startThread();

        runSine (proc, sampleRate, blockSize, 200, nullptr);

        const bool stopped = saver.stopThread (4000);

        std::printf ("  off-thread saves: %d\n", saver.saves.load());
        ok &= expect (stopped, "off-thread getStateInformation finishes without hanging");
        ok &= expect (saver.saves.load() > 0 && ! saver.bad.load(),
                      "off-thread getStateInformation produces valid, parseable state");
    }

    proc.setChainOrder (chain::defaultOrder(), {});
    proc.prepareToPlay (sampleRate, blockSize);

    return ok;
}

/** Swaps the processor's main bus to mono (in and out), runs `run`, then restores a
    stereo layout and re-prepares. Used by the dual-NAM and delay-mode tests below to
    exercise the mono-host fallback that docs/STEREO.md requires for both. */
template <typename Fn>
bool withMonoHost (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize, Fn&& run)
{
    auto monoLayout = proc.getBusesLayout();
    monoLayout.inputBuses.set (0, juce::AudioChannelSet::mono());
    monoLayout.outputBuses.set (0, juce::AudioChannelSet::mono());

    const bool monoSupported = proc.setBusesLayout (monoLayout);

    if (monoSupported)
    {
        proc.prepareToPlay (sampleRate, blockSize);
        run();
    }

    auto stereoLayout = proc.getBusesLayout();
    stereoLayout.inputBuses.set (0, juce::AudioChannelSet::stereo());
    stereoLayout.outputBuses.set (0, juce::AudioChannelSet::stereo());
    proc.setBusesLayout (stereoLayout);
    proc.prepareToPlay (sampleRate, blockSize);

    return monoSupported;
}

/** Renders a mono sine through `proc` (which must already be on a mono bus layout)
    and reports whether every produced sample stayed finite. */
bool processMonoSineFinite (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    juce::AudioBuffer<float> buffer (1, blockSize);
    juce::MidiBuffer midi;
    bool finite = true;

    for (int block = 0; block < 20; ++block)
    {
        auto* data = buffer.getWritePointer (0);

        for (int i = 0; i < blockSize; ++i)
            data[i] = 0.3f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                               * 220.0 * (block * blockSize + i) / sampleRate);

        proc.processBlock (buffer, midi);

        for (int i = 0; i < blockSize; ++i)
            if (! std::isfinite (buffer.getReadPointer (0)[i]))
                finite = false;
    }

    return finite;
}

/** Split-path structure (docs/SPLIT.md): running a chain with no split/lane2/mix
    tokens in it must be exactly the single-pass path the chain always ran -- the one
    thing there is no pre-split binary left to diff against any more, so this proves
    the structureless path stays fully deterministic and self-consistent (the same
    "control" guarantee the fx-slot tests lean on), plus that every split_mode
    processes cleanly on both host layouts and the X-Over lanes really do sum flat. */
bool runSplitPathTests (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    std::printf ("split path:\n");

    namespace chain = tubamp::chain;
    bool ok = true;

    // --- structureless regression: a classic (no split) chain is still bit-identical
    // run to run, i.e. nothing from the lane machinery leaks into the plain path. The
    // amp block is deliberately excluded (same reasoning as the fx-slot "control"
    // render): when the model needs resampling to the host rate, a decaying remnant of
    // the previous render survives into the next one, so two renders of a chain
    // containing it are not bit-exact for a reason that has nothing to do with lanes.
    chain::Order classicNoAmp;

    for (auto id : chain::classicOrder())
        if (id != BlockId::amp)
            classicNoAmp.push_back (id);

    const auto classic1 = renderChain (proc, classicNoAmp, sampleRate, blockSize);
    const auto classic2 = renderChain (proc, classicNoAmp, sampleRate, blockSize);
    ok &= expectIdentical (classic1, classic2,
                           "a structureless (classic) chain renders bit-identically run to run "
                           "-- the lane machinery leaves it untouched");

    // --- every split_mode is finite, with empty lanes (structure only), on both hosts.
    const chain::Order structureOnly { BlockId::split, BlockId::lane2, BlockId::mix };

    for (int mode = 0; mode < 3; ++mode)
    {
        proc.setChainOrder (structureOnly, {});
        proc.prepareToPlay (sampleRate, blockSize);
        setFloatParam (proc, params::splitMode, (float) mode);

        bool finite = true;
        const double rms = processSineRms (proc, sampleRate, blockSize, &finite);

        char what[96];
        std::snprintf (what, sizeof (what), "split mode %d is only finite samples (stereo host)", mode);
        ok &= expect (finite, what);
        ok &= expect (std::isfinite (rms), "...and a sane RMS");

        proc.setChainOrder (structureOnly, {});
        setFloatParam (proc, params::splitMode, (float) mode);

        bool monoFinite = false;
        const bool monoRan = withMonoHost (proc, sampleRate, blockSize, [&]
        {
            monoFinite = processMonoSineFinite (proc, sampleRate, blockSize);
        });

        std::snprintf (what, sizeof (what), "split mode %d is only finite samples (mono host)", mode);
        ok &= expect (! monoRan || monoFinite, what);
    }

    // --- X-Over lanes sum flat at the unity mixer (LR4 property): on a mono host the
    // mixer's pans are inert, so at the default levels (0 dB, mix on) low + high should
    // reconstruct the dry signal's amplitude almost exactly.
    double dryRms = 0.0;
    withMonoHost (proc, sampleRate, blockSize, [&]
    {
        proc.setChainOrder ({}, {});
        dryRms = processMonoSineRms (proc, sampleRate, blockSize);
    });

    double xoverRms = 0.0;
    withMonoHost (proc, sampleRate, blockSize, [&]
    {
        proc.setChainOrder (structureOnly, {});
        setFloatParam (proc, params::splitMode, 2.0f); // X-Over
        xoverRms = processMonoSineRms (proc, sampleRate, blockSize);
    });

    std::printf ("  X-Over sum-flat: dryRms=%.6f xoverRms=%.6f ratio=%.4f\n",
                 dryRms, xoverRms, dryRms > 0.0 ? xoverRms / dryRms : 0.0);
    ok &= expect (dryRms > 0.0 && xoverRms > 0.0
                      && xoverRms > dryRms * 0.9 && xoverRms < dryRms * 1.1,
                  "X-Over split lanes summed back at the unity mixer reconstruct ~unity gain "
                  "(LR4 low+high sums flat)");

    proc.setChainOrder (chain::defaultOrder(), {});
    proc.prepareToPlay (sampleRate, blockSize);

    return ok;
}

/** Dual-NAM split lanes (docs/SPLIT.md): amp2 in a lane, alongside amp in the other,
    replaces the old amp_stereo toggle -- two independently-placeable chain blocks
    instead of a special-cased stereo amp. Reuses the same .nam file for both engines:
    this is not about the two sounding different on their own, only that the lane path
    runs cleanly, amp2 falls back to pass-through with no B model, and a hard-panned
    mixer really does route different content to L and R. */
bool runDualAmpLaneTests (tubamp::TubampAudioProcessor& proc, const juce::File& modelFile,
                          double sampleRate, int blockSize)
{
    std::printf ("dual amp lanes:\n");

    namespace chain = tubamp::chain;
    bool ok = true;

    const chain::Order dualAmp { BlockId::split, BlockId::amp, BlockId::lane2,
                                 BlockId::amp2, BlockId::mix };

    // --- amp2 with no B model loaded: gates on hasLiveModel(), so it is a pass-through
    // and the split/mix structure still has to run cleanly around it.
    proc.clearModelB();
    proc.setChainOrder (dualAmp, {});
    proc.prepareToPlay (sampleRate, blockSize);

    // Params are process-global and the split-modes suite runs first: without pinning
    // the split back to Copy, lane B would receive only the X-Over high band of the
    // 220 Hz test sine — near-silence that has nothing to do with the amps.
    setFloatParam (proc, params::splitMode, 0.0f);

    ok &= expect (! proc.namEngineB.hasModel(), "engine B starts with no model");

    bool finiteNoB = true;
    const double rmsNoB = processSineRms (proc, sampleRate, blockSize, &finiteNoB);
    ok &= expect (finiteNoB && std::isfinite (rmsNoB),
                  "amp in lane A, amp2 with no B model in lane B: finite, non-crashing output");

    // --- load engine B: both lanes now carry a live NAM engine.
    const auto errorB = proc.loadModelB (modelFile);
    ok &= expect (errorB.isEmpty(), "loadModelB accepts a .nam file");
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    ok &= expect (proc.namEngineB.hasModel(), "engine B reports a model after loadModelB");

    bool finiteBoth = true;
    const double rmsBoth = processSineRms (proc, sampleRate, blockSize, &finiteBoth);
    ok &= expect (finiteBoth, "both lanes loaded (stereo host): only finite samples");
    ok &= expect (std::isfinite (rmsBoth) && rmsBoth > 1.0e-6, "...and non-silent output");

    // --- hard-panned mixer yields different L/R: lane A (amp) hard left, lane B (amp2)
    // hard right, so whatever reaches L came through engine A and whatever reaches R came
    // through engine B.
    //
    // The two lanes are driven APART by amp2's own output knob rather than by relying on
    // the paths' inherent differences (amp is loudness-compensated and tone-stacked,
    // amp2 is neither): those differences are real but their SIZE depends on the model
    // file -- a capture without loudness metadata run through a flat 5/5/5 tone stack
    // leaves only ~10% between the channels, which is not a threshold any assertion can
    // stand on. +6 dB on amp2_output is a post-model gain (docs/SPLIT.md §3), so it
    // doubles lane B for every model there is.
    setFloatParam (proc, params::mixAPan, -1.0f);
    setFloatParam (proc, params::mixBPan, 1.0f);
    setFloatParam (proc, params::amp2Output, 6.0f);

    double rmsL = 0.0, rmsR = 0.0;
    bool finiteHardPan = true;
    processSineRmsPerChannel (proc, sampleRate, blockSize, rmsL, rmsR, &finiteHardPan);

    std::printf ("  hard pan: rmsL=%.6f rmsR=%.6f\n", rmsL, rmsR);
    ok &= expect (finiteHardPan, "hard-panned dual amp lanes stay finite");
    ok &= expect (rmsL > 1.0e-6 && rmsR > 1.0e-6, "...and both channels are non-silent");
    ok &= expect (rmsR - rmsL > 0.2 * juce::jmax (rmsL, rmsR),
                  "hard panning routes each lane to its own channel: +6 dB on amp2_output "
                  "lifts the right (lane B) clear of the left (lane A)");

    setFloatParam (proc, params::amp2Output, 0.0f);
    setFloatParam (proc, params::mixAPan, 0.0f);
    setFloatParam (proc, params::mixBPan, 0.0f);

    // --- a mono host still runs both engines cleanly (lanes still exist; pans go inert).
    proc.setChainOrder (dualAmp, {});
    const bool monoRan = withMonoHost (proc, sampleRate, blockSize, [&]
    {
        ok &= expect (processMonoSineFinite (proc, sampleRate, blockSize),
                      "a mono host runs the dual-amp lanes and stays finite");
    });
    ok &= expect (monoRan, "the processor accepts a mono bus layout for the dual-amp check");

    proc.clearModelB();
    proc.setChainOrder (chain::defaultOrder(), {});
    proc.prepareToPlay (sampleRate, blockSize);

    return ok;
}

/** Lane latency (docs/SPLIT.md): reported latency is the serial section plus the
    slower lane, and the mixer holds the faster lane back so the two line up where
    they meet. Uses the resampled amp's own latency as the "slow lane" (same trick the
    fx-slot latency test uses to get a known non-zero figure) against an empty, 0-cost
    lane B rather than a mocked fx plugin -- this build has no host to instantiate a
    real one anyway. */
bool runSplitLatencyTests (tubamp::TubampAudioProcessor& proc, const juce::File& modelFile,
                           double sampleRate, int blockSize)
{
    std::printf ("split latency:\n");

    namespace chain = tubamp::chain;
    bool ok = true;

    // Unlike the fx-slot latency test (which only checks two figures agree, so either
    // one being coincidentally 0 is harmless), this test needs a genuinely non-zero
    // latency to align lanes against. "the other of 44100/48000" isn't good enough --
    // whichever of those the model's own native rate happens to be, running the smoke
    // test at ITS non-native rate would flip `otherRate` onto the model's native rate
    // and collapse latency back to 0. A rate well away from both is safe regardless.
    const double resampleRate = 96000.0;
    proc.prepareToPlay (resampleRate, blockSize);

    const auto error = proc.loadModel (modelFile);
    ok &= expect (error.isEmpty(), "loadModel accepts the model at the resampled rate");
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

    proc.setChainOrder (chain::Order { BlockId::amp }, {});
    const int serialLatency = proc.getLatencySamples();
    ok &= expect (serialLatency > 0, "the resampled amp reports non-zero latency serially (control)");

    // --- serial + max(lane) reporting: amp alone in lane A, lane B empty (0 latency).
    const chain::Order laneOrder { BlockId::split, BlockId::amp, BlockId::lane2, BlockId::mix };
    proc.setChainOrder (laneOrder, {});
    const int laneLatency = proc.getLatencySamples();

    ok &= expect (laneLatency == serialLatency,
                  "split[amp, empty] latency == serial(0) + max(laneA, laneB(0)) == the amp's own latency");

    // --- lane compensation delays the shorter lane: Copy split so lane B carries an
    // exact, undelayed copy of the input; lane A carries the same signal through the
    // resampled amp (latency == laneLatency). Without compensation lane B's raw copy
    // would land in the mixed output right at sample 0; with it, lane B is held back by
    // laneLatency samples so the two arrive together.
    setFloatParam (proc, params::splitMode, 0.0f); // Copy
    proc.prepareToPlay (resampleRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    buffer.clear();
    buffer.setSample (0, 0, 1.0f);
    buffer.setSample (1, 0, 1.0f);

    std::vector<float> outL;
    const int numBlocksToCapture = 1 + (laneLatency + blockSize * 4) / blockSize;

    for (int block = 0; block < numBlocksToCapture; ++block)
    {
        proc.processBlock (buffer, midi);

        for (int i = 0; i < blockSize; ++i)
            outL.push_back (buffer.getSample (0, i));

        buffer.clear();
    }

    const float earlyMag = std::abs (outL[0]);
    float peakLater = 0.0f;
    int peakIndex = -1;

    for (int i = 4; i < (int) outL.size(); ++i)
    {
        if (std::abs (outL[i]) > peakLater)
        {
            peakLater = std::abs (outL[i]);
            peakIndex = i;
        }
    }

    std::printf ("  lane latency=%d, sample[0]=%.6g, peak later=%.6g at %d\n",
                 laneLatency, (double) earlyMag, (double) peakLater, peakIndex);

    ok &= expect (earlyMag < 0.1f,
                  "lane B's raw impulse does not leak through unaligned at sample 0 "
                  "-- compensateLaneLatency held it back to match lane A");
    ok &= expect (peakLater > earlyMag,
                  "...and the aligned energy shows up later, once both lanes arrive together");

    setFloatParam (proc, params::splitMode, 0.0f);
    proc.setChainOrder (chain::defaultOrder(), {});
    proc.prepareToPlay (sampleRate, blockSize);

    return ok;
}

/** Split/mix structure survives state round-trips exactly like a plain reordering
    does, and amp_stereo (docs/STEREO.md §1, superseded by docs/SPLIT.md) is gone from
    the param tree entirely. */
bool runSplitStateTests (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    std::printf ("split state:\n");

    namespace chain = tubamp::chain;
    bool ok = true;

    ok &= expect (proc.apvts.getParameter ("amp_stereo") == nullptr,
                  "amp_stereo is gone from the param tree");

    const chain::Order structured { BlockId::gate, BlockId::split, BlockId::amp, BlockId::lane2,
                                    BlockId::amp2, BlockId::mix, BlockId::cab };
    const auto structuredText = chain::toString (structured);

    proc.setChainOrder (structured, {});
    ok &= expect (chain::toString (proc.getChainOrder()) == structuredText,
                  "a well-formed split/mix structure is accepted as-is");

    juce::MemoryBlock blob;
    proc.getStateInformation (blob);

    proc.setChainOrder (chain::defaultOrder(), {});
    proc.setStateInformation (blob.getData(), (int) blob.getSize());
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

    ok &= expect (chain::toString (proc.getChainOrder()) == structuredText,
                  "structure survives getStateInformation -> setStateInformation");

    proc.setChainOrder (structured, {});
    const auto presetState = proc.captureStateVar();

    proc.setChainOrder (chain::defaultOrder(), {});
    proc.applyStateVar (presetState);

    ok &= expect (chain::toString (proc.getChainOrder()) == structuredText,
                  "structure survives captureStateVar -> applyStateVar");

    // --- an order longer than chain::maxChainLength may not leave a DECAPITATED
    // structure behind. There are more block ids (28) than the chain can hold (24), so
    // an over-long order is reachable, and the length clamp knows nothing about the
    // grammar: dropping `mix` off the end would publish `..., split, ..., lane2` into
    // the mirror, the packed word and the saved state -- exactly what docs/SPLIT.md §1
    // says can never happen. The truncated chain flattens to serial instead.
    {
        chain::Order overlong;

        for (int i = 0; i < chain::maxChainLength - 2; ++i)
            overlong.push_back ((BlockId) i); // ordinary ids only: 0..21, none structural

        overlong.push_back (BlockId::split);
        overlong.push_back (BlockId::lane2);
        overlong.push_back (BlockId::mix); // lands at index 24 -- past the cap

        proc.setChainOrder (overlong, {});

        const auto published = proc.getChainOrder();
        bool anyStructural = false;

        for (auto id : published)
            anyStructural |= id == BlockId::split || id == BlockId::lane2 || id == BlockId::mix;

        ok &= expect ((int) published.size() <= chain::maxChainLength,
                      "an over-long order is clamped to chain::maxChainLength");
        ok &= expect (chain::findStructure (published).valid(),
                      "...and never publishes a structure the length clamp broke");
        ok &= expect (! anyStructural,
                      "...the decapitated triple flattens to serial rather than dangling");
    }

    proc.setChainOrder (chain::defaultOrder(), {});
    proc.prepareToPlay (sampleRate, blockSize);

    return ok;
}

/** modelPathB (docs/STEREO.md §1): a sibling of modelPath threaded through all four
    serialization sites (getStateInformation/setStateInformation, captureStateVar/
    applyStateVar), guarded like loadedModelPath by pathLock. An absent key -- state
    or a preset written before the stereo amp existed -- must load engine B as empty,
    never stuck on whatever path B carried before the restore. */
bool runModelPathBStateTests (tubamp::TubampAudioProcessor& proc, const juce::File& modelFile,
                              double sampleRate, int blockSize)
{
    std::printf ("modelPathB state:\n");

    namespace chain = tubamp::chain;
    bool ok = true;

    proc.setChainOrder (chain::Order { BlockId::amp }, {});
    proc.prepareToPlay (sampleRate, blockSize);

    const auto errorB = proc.loadModelB (modelFile);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    ok &= expect (errorB.isEmpty() && proc.getLoadedModelPathB() == modelFile.getFullPathName(),
                  "loadModelB records loadedModelPathB");

    // --- binary state round-trip (host save/load)
    juce::MemoryBlock blob;
    proc.getStateInformation (blob);

    proc.clearModelB();
    ok &= expect (proc.getLoadedModelPathB().isEmpty(), "clearModelB empties loadedModelPathB");

    proc.setStateInformation (blob.getData(), (int) blob.getSize());
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

    ok &= expect (proc.getLoadedModelPathB() == modelFile.getFullPathName(),
                  "modelPathB survives getStateInformation -> setStateInformation");

    // --- absent key: an old-format blob with no modelPathB attribute loads engine B
    // as empty, not stuck on the path just restored above.
    if (auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), (int) blob.getSize()))
    {
        xml->removeAttribute ("modelPathB");

        juce::MemoryBlock legacyBlob;
        juce::AudioProcessor::copyXmlToBinary (*xml, legacyBlob);

        proc.setStateInformation (legacyBlob.getData(), (int) legacyBlob.getSize());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

        ok &= expect (proc.getLoadedModelPathB().isEmpty(),
                      "state with no modelPathB attribute restores engine B as empty");
    }
    else
    {
        ok &= expect (false, "modelPathB state blob could not be rebuilt");
    }

    // --- preset (captureStateVar/applyStateVar) round-trip
    proc.loadModelB (modelFile);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    const auto slotState = proc.captureStateVar();

    proc.clearModelB();
    proc.applyStateVar (slotState);
    ok &= expect (proc.getLoadedModelPathB() == modelFile.getFullPathName(),
                  "modelPathB survives captureStateVar -> applyStateVar");

    // --- absent key in a preset var (saved before the stereo amp existed)
    if (auto* obj = slotState.getDynamicObject())
    {
        obj->removeProperty ("modelPathB");
        proc.applyStateVar (slotState);

        ok &= expect (proc.getLoadedModelPathB().isEmpty(),
                      "a preset var with no modelPathB property restores engine B as empty");
    }
    else
    {
        ok &= expect (false, "captureStateVar did not produce an object");
    }

    proc.clearModelB();
    proc.setChainOrder (chain::defaultOrder(), {});
    proc.prepareToPlay (sampleRate, blockSize);

    return ok;
}

/** Stereo delay modes (docs/STEREO.md §2): all three delay_mode choices must process
    stereo and mono hosts cleanly, and mode 0 (Stereo, the frozen default) must stay
    exactly the old per-channel-independent delay at the frozen defaults (ratio 100,
    width 1) -- verified against two channels run through their own independent
    DelayFx instances, which is what the single shared-line code did before the
    stereo modes existed (see the `channels != 2` guard in DelayFx::process). */
bool runDelayModeTests (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    std::printf ("delay modes:\n");

    namespace chain = tubamp::chain;
    bool ok = true;
    const chain::Order delayOnly { BlockId::delay };

    // --- each mode processes a stereo host cleanly
    for (int mode = 0; mode < 3; ++mode)
    {
        proc.setChainOrder (delayOnly, {});
        proc.prepareToPlay (sampleRate, blockSize);
        setBoolParam (proc, params::delayOn, true);
        setFloatParam (proc, params::delayMode, (float) mode);
        setFloatParam (proc, params::delayRatio, 150.0f);
        setFloatParam (proc, params::delayWidth, 0.6f);

        bool finite = true;
        const double rms = processSineRms (proc, sampleRate, blockSize, &finite);

        char what[96];
        std::snprintf (what, sizeof (what), "delay mode %d is only finite samples (stereo host)", mode);
        ok &= expect (finite, what);
        ok &= expect (std::isfinite (rms), "...and a sane RMS");
    }

    // --- each mode processes a mono host cleanly (DelayFx collapses every mode onto
    // the single-line path off the host channel count, not the mode)
    for (int mode = 0; mode < 3; ++mode)
    {
        proc.setChainOrder (delayOnly, {});
        setFloatParam (proc, params::delayMode, (float) mode);

        bool finite = false;
        const bool monoRan = withMonoHost (proc, sampleRate, blockSize, [&]
        {
            finite = processMonoSineFinite (proc, sampleRate, blockSize);
        });

        char what[96];
        std::snprintf (what, sizeof (what), "delay mode %d is only finite samples (mono host)", mode);
        ok &= expect (! monoRan || finite, what);
    }

    setFloatParam (proc, params::delayRatio, 100.0f);
    setFloatParam (proc, params::delayWidth, 1.0f);

    // --- mode 0 (Stereo) regression guard against a fixed impulse
    {
        const juce::dsp::ProcessSpec stereoSpec { sampleRate, (juce::uint32) blockSize, 2 };
        const juce::dsp::ProcessSpec monoSpec { sampleRate, (juce::uint32) blockSize, 1 };

        tubamp::DelayFx stereoDelay, monoDelayL, monoDelayR;

        stereoDelay.prepare (stereoSpec);
        monoDelayL.prepare (monoSpec);
        monoDelayR.prepare (monoSpec);

        // Digital with nothing that couples or moves the channels: no modulation (the
        // two sides run a quarter cycle apart) and no ducking (it follows both).
        tubamp::DelayParams dp;
        dp.type = (int) tubamp::DelayFx::Type::digital;
        dp.mod = 0.0f;
        dp.duck = 0.0f;
        dp.timeMs = 280.0f;
        dp.feedback = 0.4f;
        dp.mix = 0.35f;
        dp.mode = 0; // Stereo
        dp.ratioPct = 100.0f;
        dp.width = 1.0f;

        stereoDelay.setParameters (dp);
        monoDelayL.setParameters (dp);
        monoDelayR.setParameters (dp);

        const int numBlocks = 8;
        std::vector<float> stereoL, stereoR, monoL, monoR;

        for (int block = 0; block < numBlocks; ++block)
        {
            juce::AudioBuffer<float> stereoBuf (2, blockSize);
            juce::AudioBuffer<float> monoBufL (1, blockSize), monoBufR (1, blockSize);
            stereoBuf.clear();
            monoBufL.clear();
            monoBufR.clear();

            // A fixed, deterministic impulse pair (not meant to be musical) that
            // exercises the feedback tail on both channels, offset from each other.
            if (block == 0)
            {
                stereoBuf.setSample (0, 0, 1.0f);
                stereoBuf.setSample (1, 37, 0.8f);
                monoBufL.setSample (0, 0, 1.0f);
                monoBufR.setSample (0, 37, 0.8f);
            }

            stereoDelay.process (juce::dsp::AudioBlock<float> (stereoBuf));
            monoDelayL.process (juce::dsp::AudioBlock<float> (monoBufL));
            monoDelayR.process (juce::dsp::AudioBlock<float> (monoBufR));

            for (int i = 0; i < blockSize; ++i)
            {
                stereoL.push_back (stereoBuf.getSample (0, i));
                stereoR.push_back (stereoBuf.getSample (1, i));
                monoL.push_back (monoBufL.getSample (0, i));
                monoR.push_back (monoBufR.getSample (0, i));
            }
        }

        ok &= expectIdentical (stereoL, monoL,
                               "mode 0 left channel matches an independent mono render (regression guard)");
        ok &= expectIdentical (stereoR, monoR,
                               "mode 0 right channel matches an independent mono render (regression guard)");
    }

    // --- Dual mode at the corners of the range: time 2000 ms x ratio 200% asks for a
    // 4 s right-hand line. Sizing the delay line for 2 s used to clamp it there, which
    // collapsed R onto L's time and made the top half of the ratio range inert.
    {
        const juce::dsp::ProcessSpec stereoSpec { sampleRate, (juce::uint32) blockSize, 2 };

        tubamp::DelayFx delayFx;
        delayFx.prepare (stereoSpec);
        tubamp::DelayParams dp;
        dp.type = (int) tubamp::DelayFx::Type::digital;
        dp.mod = 0.0f;
        dp.duck = 0.0f;
        dp.timeMs = 2000.0f;
        dp.feedback = 0.0f;
        dp.mix = 1.0f;
        dp.mode = 2; // Dual
        dp.ratioPct = 200.0f;
        dp.width = 1.0f;
        delayFx.setParameters (dp);

        // Half a second of silence first, so every smoother has settled before the
        // impulse and the echo lands at the target time.
        juce::AudioBuffer<float> buf (2, blockSize);
        const int settleBlocks = (int) std::ceil (sampleRate * 0.5 / blockSize);

        for (int block = 0; block < settleBlocks; ++block)
        {
            buf.clear();
            delayFx.process (juce::dsp::AudioBlock<float> (buf));
        }

        // One impulse on R, then long enough to hear a 4 s echo.
        const int tailBlocks = (int) std::ceil (sampleRate * 4.5 / blockSize);
        int peakIndex = -1;
        float peak = 0.0f;

        for (int block = 0; block < tailBlocks; ++block)
        {
            buf.clear();

            if (block == 0)
                buf.setSample (1, 0, 1.0f);

            delayFx.process (juce::dsp::AudioBlock<float> (buf));

            for (int i = 0; i < blockSize; ++i)
            {
                const int index = block * blockSize + i;
                const float magnitude = std::abs (buf.getSample (1, i));

                // Skip the dry impulse itself (mix is 1, but the interpolator leaks a
                // little of it at index 0 on some rates).
                if (index > 64 && magnitude > peak)
                {
                    peak = magnitude;
                    peakIndex = index;
                }
            }
        }

        const int expectedIndex = (int) std::lround (sampleRate * 4.0);
        const int tolerance = (int) std::lround (sampleRate * 0.01); // 10 ms

        ok &= expect (peak > 0.1f, "Dual mode at 2000 ms x 200% produces a right-channel echo");
        ok &= expect (std::abs (peakIndex - expectedIndex) <= tolerance,
                      "...and it lands at 4 s, not clamped to the 2 s line length");

        if (std::abs (peakIndex - expectedIndex) > tolerance)
            std::printf ("  (right echo at sample %d, expected ~%d)\n", peakIndex, expectedIndex);
    }

    proc.setChainOrder (chain::defaultOrder(), {});
    proc.prepareToPlay (sampleRate, blockSize);

    return ok;
}

/** Engine types (docs/REVERB-DELAY.md §1): a fresh instance starts on Digital and Plate
    with duck and damping at those types' own starting points, the type ids round-trip
    through both restore paths -- the host's ValueTree blob and the preset / A/B var --
    and state that predates them takes the defaults. */
bool runEngineTypeStateTests (double sampleRate, int blockSize)
{
    std::printf ("engine type state:\n");
    bool ok = true;

    const int digital = (int) tubamp::DelayFx::Type::digital;
    const int plate = (int) tubamp::ReverbFx::Type::plate;

    const auto allDefault = [&] (tubamp::TubampAudioProcessor& p)
    {
        bool defaults = true;

        for (int k = 0; k < params::maxInstances; ++k)
        {
            defaults &= choiceIndex (p, params::delayTypeIds[k]) == digital;
            defaults &= choiceIndex (p, params::reverbTypeIds[k]) == plate;
        }

        return defaults;
    };

    tubamp::TubampAudioProcessor fresh;
    fresh.prepareToPlay (sampleRate, blockSize);
    ok &= expect (allDefault (fresh), "a fresh instance has every delay_type = Digital and reverb_type = Plate");

    bool startingPoints = true;

    for (int k = 0; k < params::maxInstances; ++k)
    {
        const auto startsAt = [&fresh] (const char* id, float expected)
        {
            return juce::approximatelyEqual (fresh.apvts.getRawParameterValue (id)->load(), expected);
        };

        startingPoints &= startsAt (params::delayDuckIds[k], tubamp::kDelayDuckDefault[digital]);
        startingPoints &= startsAt (params::delayColorIds[k], tubamp::kDelayColorDefault[digital]);
        startingPoints &= startsAt (params::delayHighCutIds[k], tubamp::kDelayHighCutDefault[digital]);
        startingPoints &= startsAt (params::reverbDampingIds[k], tubamp::kReverbDampingDefault[plate]);
        startingPoints &= startsAt (params::reverbDecayIds[k], tubamp::kReverbDecayDefault[plate]);
    }

    ok &= expect (startingPoints, "knobs with a per-type starting point default to the default type's");

    // A distinctive non-default value on every type id, so a round-trip that silently
    // fell back to the defaults cannot pass.
    const int delayPick[params::maxInstances]  { (int) tubamp::DelayFx::Type::tape,
                                                 (int) tubamp::DelayFx::Type::diffuse,
                                                 (int) tubamp::DelayFx::Type::lofi };
    const int reverbPick[params::maxInstances] { (int) tubamp::ReverbFx::Type::hall,
                                                 (int) tubamp::ReverbFx::Type::spring,
                                                 (int) tubamp::ReverbFx::Type::ambient };

    const auto setPicks = [&] (tubamp::TubampAudioProcessor& p)
    {
        for (int k = 0; k < params::maxInstances; ++k)
        {
            setFloatParam (p, params::delayTypeIds[k], (float) delayPick[k]);
            setFloatParam (p, params::reverbTypeIds[k], (float) reverbPick[k]);
        }
    };

    const auto picksHeld = [&] (tubamp::TubampAudioProcessor& p)
    {
        bool held = true;

        for (int k = 0; k < params::maxInstances; ++k)
        {
            held &= choiceIndex (p, params::delayTypeIds[k]) == delayPick[k];
            held &= choiceIndex (p, params::reverbTypeIds[k]) == reverbPick[k];
        }

        return held;
    };

    const auto setAllDefault = [&] (tubamp::TubampAudioProcessor& p)
    {
        for (int k = 0; k < params::maxInstances; ++k)
        {
            setFloatParam (p, params::delayTypeIds[k], (float) digital);
            setFloatParam (p, params::reverbTypeIds[k], (float) plate);
        }
    };

    // --- host state (getStateInformation / setStateInformation)
    {
        tubamp::TubampAudioProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);

        setPicks (proc);
        juce::MemoryBlock blob;
        proc.getStateInformation (blob);

        setAllDefault (proc);
        proc.setStateInformation (blob.getData(), (int) blob.getSize());
        ok &= expect (picksHeld (proc), "host state carrying the type ids round-trips them");

        // The same blob with the six type PARAM children removed is what a session
        // saved before the types existed looks like.
        auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), (int) blob.getSize());
        auto root = xml != nullptr ? juce::ValueTree::fromXml (*xml) : juce::ValueTree();
        auto paramTree = root.getChildWithName (proc.apvts.state.getType());

        for (int k = 0; k < params::maxInstances; ++k)
        {
            paramTree.removeChild (paramTree.getChildWithProperty ("id", params::delayTypeIds[k]), nullptr);
            paramTree.removeChild (paramTree.getChildWithProperty ("id", params::reverbTypeIds[k]), nullptr);
        }

        juce::MemoryBlock olderBlob;

        if (auto olderXml = root.createXml())
            juce::AudioProcessor::copyXmlToBinary (*olderXml, olderBlob);

        ok &= expect (paramTree.isValid() && olderBlob.getSize() > 0, "host state built without the type ids");

        setPicks (proc);
        proc.setStateInformation (olderBlob.getData(), (int) olderBlob.getSize());
        ok &= expect (allDefault (proc), "host state without the type ids restores Digital and Plate");
    }

    // --- preset / A/B var (captureStateVar / applyStateVar)
    {
        tubamp::TubampAudioProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);

        setPicks (proc);
        const auto modern = proc.captureStateVar();

        setAllDefault (proc);
        proc.applyStateVar (modern);
        ok &= expect (picksHeld (proc), "a preset carrying the type ids round-trips them");

        auto older = proc.captureStateVar();
        auto* paramValues = older.getProperty ("params", juce::var()).getDynamicObject();

        if (paramValues != nullptr)
            for (int k = 0; k < params::maxInstances; ++k)
            {
                paramValues->removeProperty (juce::Identifier (params::delayTypeIds[k]));
                paramValues->removeProperty (juce::Identifier (params::reverbTypeIds[k]));
            }

        ok &= expect (paramValues != nullptr, "preset built without the type ids");

        setPicks (proc);
        proc.applyStateVar (older);
        ok &= expect (allDefault (proc), "a preset without the type ids restores Digital and Plate");
    }

    return ok;
}

/** Parameters in the layout before the gate/comp/drive/EQ/mod engines
    (tests/golden/params-base.tsv). Everything after them is a hint-4 addition. */
constexpr int kBaseParamCount = 166;

/** One engine type id of docs/FX-ENGINES.md §2.7.5 and the block instance it belongs to. */
struct EngineTypeId
{
    const char* id;
    BlockId block;
    int recommended;
};

/** Listed from the per-instance id arrays rather than from the processor's own pin table,
    so a table that lost an entry cannot pass. */
std::vector<EngineTypeId> engineTypeIds()
{
    namespace chain = tubamp::chain;
    std::vector<EngineTypeId> ids { { params::gateMode, BlockId::gate, (int) tubamp::GateFx::Mode::tight } };

    for (int k = 0; k < params::maxInstances; ++k)
    {
        ids.push_back ({ params::compTypeIds[k], chain::instanceId (BlockId::comp, k), (int) tubamp::CompFx::Type::clean });
        ids.push_back ({ params::driveTypeIds[k], chain::instanceId (BlockId::drive, k), (int) tubamp::DriveFx::Type::screamer });
        ids.push_back ({ params::eqTypeIds[k], chain::instanceId (BlockId::eq, k), (int) tubamp::EqFx::Type::parametric });
        ids.push_back ({ params::modChorusModeIds[k], chain::instanceId (BlockId::mod, k), (int) tubamp::ChorusFx::Mode::wide });
        ids.push_back ({ params::modPhaserModelIds[k], chain::instanceId (BlockId::mod, k), (int) tubamp::PhaserFx::Model::phase90 });
        ids.push_back ({ params::modTremVoiceIds[k], chain::instanceId (BlockId::mod, k), (int) tubamp::TremoloFx::Voice::smooth });
    }

    return ids;
}

float plainValueOf (const juce::RangedAudioParameter& p)
{
    return p.convertFrom0to1 (p.getValue());
}

/** Every parameter after the base layout, in layout order. */
std::vector<juce::RangedAudioParameter*> newParams (tubamp::TubampAudioProcessor& proc)
{
    std::vector<juce::RangedAudioParameter*> out;
    const auto& all = proc.getParameters();

    for (int i = kBaseParamCount; i < all.size(); ++i)
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (all[i]))
            out.push_back (ranged);

    return out;
}

/** Moves every new parameter off its default to a value that depends on its index, so a
    restore that fell back to defaults, or swapped two ids, cannot pass. */
void setNewParamsOffDefault (tubamp::TubampAudioProcessor& proc)
{
    int i = 0;

    for (auto* p : newParams (proc))
    {
        const float steps = (float) std::max (1, p->getNumSteps() - 1);
        float v = (float) ((i * 7 + 3) % 11 + 1) / 12.0f;

        if (p->getNumSteps() <= 32)
            v = std::round (v * steps) / steps;

        if (juce::approximatelyEqual (p->convertFrom0to1 (v), p->convertFrom0to1 (p->getDefaultValue())))
            v = p->getDefaultValue() > 0.5f ? 0.0f : 1.0f;

        p->setValueNotifyingHost (v);
        ++i;
    }
}

/** Snapshot of every parameter's plain value, indexed like getParameters(). */
std::vector<float> paramSnapshot (tubamp::TubampAudioProcessor& proc)
{
    std::vector<float> values;

    for (auto* raw : proc.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (raw))
            values.push_back (plainValueOf (*ranged));

    return values;
}

bool sameRange (const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to)
{
    if (a.size() < to || b.size() < to)
        return false;

    for (size_t i = from; i < to; ++i)
        if (a[i] != b[i])
            return false;

    return true;
}

bool inOrder (const tubamp::chain::Order& order, BlockId block)
{
    return std::find (order.begin(), order.end(), block) != order.end();
}

/** Records every value one parameter is set to, as the audio thread would see it. */
struct ValueRecorder final : juce::AudioProcessorParameter::Listener
{
    explicit ValueRecorder (juce::RangedAudioParameter& p) : param (p) { param.addListener (this); }
    ~ValueRecorder() override { param.removeListener (this); }

    void parameterValueChanged (int, float newValue) override { seen.push_back (param.convertFrom0to1 (newValue)); }
    void parameterGestureChanged (int, bool) override {}

    juce::RangedAudioParameter& param;
    std::vector<float> seen;
};

/** The legacy pin (docs/FX-ENGINES.md §2.7.5) on both restore paths: state written before
    the engine type ids existed restores every type id of a block in its chain at Classic,
    blocks outside the chain keep the recommended type, state that carries the ids
    round-trips them exactly, and the base layout is untouched throughout. */
bool runLegacyPinTests (double sampleRate, int blockSize)
{
    std::printf ("legacy pin:\n");

    namespace chain = tubamp::chain;
    bool ok = true;

    const auto typeIds = engineTypeIds();
    ok &= expect (typeIds.size() == 19, "19 engine type ids: gate_mode + comp/drive/eq type x3 + three mod voicings x3");

    // --- layout: 95 hint-4 parameters appended after the frozen 166
    {
        tubamp::TubampAudioProcessor fresh;
        const auto& all = fresh.getParameters();
        bool hints = all.size() == kBaseParamCount + 95;

        for (int i = 0; i < all.size(); ++i)
            if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (all[i]))
                hints &= (i < kBaseParamCount) == (p->getVersionHint() < 4);

        ok &= expect (hints, "261 parameters: the base 166 at hints 1-3, then 95 at hint 4");

        bool recommended = true;

        for (const auto& t : typeIds)
            recommended &= choiceIndex (fresh, t.id) == t.recommended;

        ok &= expect (recommended, "a fresh instance has every engine type id on the recommended type");
    }

    // gate, comp, comp3, drive2, eq, eq3, mod, mod2 in the chain; comp2, drive, drive3,
    // eq2, mod3 out of it — instance 1 of drive absent while instance 2 is present.
    const chain::Order order { BlockId::gate,  BlockId::comp, BlockId::comp3, BlockId::drive2, BlockId::amp,
                               BlockId::eq,    BlockId::eq3,  BlockId::mod,   BlockId::mod2 };

    const auto pinnedAsExpected = [&] (tubamp::TubampAudioProcessor& p, const chain::Order& restored)
    {
        bool pinned = true;

        for (const auto& t : typeIds)
            pinned &= choiceIndex (p, t.id) == (inOrder (restored, t.block) ? 0 : t.recommended);

        return pinned;
    };

    const auto newParamsAtDefault = [] (tubamp::TubampAudioProcessor& p, const std::vector<EngineTypeId>& except)
    {
        bool defaults = true;

        for (auto* param : newParams (p))
        {
            const bool isType = std::any_of (except.begin(), except.end(),
                                             [param] (const EngineTypeId& t) { return param->paramID == t.id; });

            if (! isType)
                defaults &= juce::approximatelyEqual (param->getValue(), param->getDefaultValue());
        }

        return defaults;
    };

    // The source rig: every new parameter off its default, non-default types included.
    tubamp::TubampAudioProcessor source;
    source.prepareToPlay (sampleRate, blockSize);
    source.setChainOrder (order, {});
    setNewParamsOffDefault (source);

    // Types neither Classic (0) nor recommended (1), varied per id.
    for (size_t i = 0; i < typeIds.size(); ++i)
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (source.apvts.getParameter (typeIds[i].id)))
            setFloatParam (source, typeIds[i].id, (float) (2 + (int) i % (choice->choices.size() - 2)));

    setFloatParam (source, params::gateThreshold, -55.0f);
    setFloatParam (source, params::compRatio, 7.5f);
    const auto sourceValues = paramSnapshot (source);
    const auto total = sourceValues.size();

    bool typesOff = true;

    for (const auto& t : typeIds)
        typesOff &= choiceIndex (source, t.id) != 0 && choiceIndex (source, t.id) != t.recommended;

    ok &= expect (typesOff, "the source rig has every type id on a non-default, non-Classic type");

    juce::MemoryBlock blob;
    source.getStateInformation (blob);

    // --- (c) host state from this layout round-trips every new parameter exactly
    {
        tubamp::TubampAudioProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);
        proc.setStateInformation (blob.getData(), (int) blob.getSize());

        ok &= expect (sameRange (paramSnapshot (proc), sourceValues, 0, total),
                      "host state from this layout round-trips all 261 parameters exactly");
    }

    // --- (a) host state from the old layout: the blob with all 95 new PARAM children
    // removed, which is what a session saved before them looks like.
    juce::MemoryBlock olderBlob;
    {
        auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), (int) blob.getSize());
        auto root = xml != nullptr ? juce::ValueTree::fromXml (*xml) : juce::ValueTree();
        auto paramTree = root.getChildWithName (source.apvts.state.getType());

        for (auto* p : newParams (source))
            paramTree.removeChild (paramTree.getChildWithProperty ("id", p->paramID), nullptr);

        ok &= expect (paramTree.isValid() && paramTree.getNumChildren() == kBaseParamCount,
                      "old-layout host state carries exactly the base 166 PARAM children");

        if (auto olderXml = root.createXml())
            juce::AudioProcessor::copyXmlToBinary (*olderXml, olderBlob);
    }

    {
        tubamp::TubampAudioProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);
        proc.setStateInformation (olderBlob.getData(), (int) olderBlob.getSize());

        ok &= expect (pinnedAsExpected (proc, order),
                      "old-layout host state: every type id of a block in the chain restores at Classic");
        ok &= expect (choiceIndex (proc, params::comp2Type) == (int) tubamp::CompFx::Type::clean
                          && choiceIndex (proc, params::driveType) == (int) tubamp::DriveFx::Type::screamer
                          && choiceIndex (proc, params::mod3ChorusMode) == (int) tubamp::ChorusFx::Mode::wide,
                      "(b) old-layout host state: blocks outside the chain keep the recommended type");
        ok &= expect (newParamsAtDefault (proc, typeIds), "old-layout host state: the other new parameters stay at their defaults");
        ok &= expect (sameRange (paramSnapshot (proc), sourceValues, 0, (size_t) kBaseParamCount),
                      "old-layout host state: all 166 existing parameters restore exactly");
        ok &= expect (proc.getChainOrder() == order, "old-layout host state: the chain restores as saved");

        // (b) Adding a block that was not in the restored chain starts it on the new type.
        auto grown = order;
        grown.push_back (BlockId::comp2);
        grown.push_back (BlockId::drive);
        proc.setChainOrder (grown, {});
        ok &= expect (choiceIndex (proc, params::comp2Type) == (int) tubamp::CompFx::Type::clean
                          && choiceIndex (proc, params::driveType) == (int) tubamp::DriveFx::Type::screamer,
                      "(b) a block added after the restore starts on the recommended type");
    }

    // The pin is written into the restored tree, so a pinned id goes straight to Classic:
    // the recommended type is never published, not even for one callback.
    {
        tubamp::TubampAudioProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);
        setFloatParam (proc, params::compType, (float) tubamp::CompFx::Type::fet);

        ValueRecorder recorder (*proc.apvts.getParameter (params::compType));
        proc.setStateInformation (olderBlob.getData(), (int) olderBlob.getSize());

        ok &= expect (recorder.seen.size() == 1 && recorder.seen.front() == 0.0f,
                      "old-layout host state: a pinned id moves to Classic without passing through the default");
    }

    // A session written before the chain was user-arrangeable has no chainOrder at all
    // and loads as the classic nine, so their type ids pin; the instances do not.
    {
        const char* const legacyXml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<TUBAMP modelPath=\"\" irPath=\"\">\n"
            "  <PARAMS>\n"
            "    <PARAM id=\"gate_on\" value=\"1.0\"/>\n"
            "    <PARAM id=\"gate_threshold\" value=\"-62.0\"/>\n"
            "    <PARAM id=\"comp_threshold\" value=\"-31.0\"/>\n"
            "    <PARAM id=\"mod_type\" value=\"2.0\"/>\n"
            "  </PARAMS>\n"
            "</TUBAMP>\n";

        juce::MemoryBlock legacyBlob;

        if (auto xml = juce::parseXML (juce::String (legacyXml)))
            juce::AudioProcessor::copyXmlToBinary (*xml, legacyBlob);

        tubamp::TubampAudioProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);
        proc.setStateInformation (legacyBlob.getData(), (int) legacyBlob.getSize());

        ok &= expect (legacyBlob.getSize() > 0 && proc.getChainOrder() == chain::classicOrder(),
                      "hand-written pre-chain host state loads as the classic order");
        ok &= expect (pinnedAsExpected (proc, chain::classicOrder()),
                      "hand-written pre-chain host state: classic blocks at Classic, instances 2-3 recommended");
        ok &= expect (choiceIndex (proc, params::modType) == 2
                          && juce::approximatelyEqual (proc.apvts.getRawParameterValue (params::gateThreshold)->load(), -62.0f),
                      "hand-written pre-chain host state: its own parameters restore");
    }

    // --- (e) the pin keys on absence only: a present id restores as saved, 0 or not,
    // in the chain or not.
    {
        tubamp::TubampAudioProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);
        proc.setChainOrder (order, {});

        setFloatParam (proc, params::gateMode, 0.0f);                                  // in chain, 0
        setFloatParam (proc, params::compType, (float) tubamp::CompFx::Type::opto);    // in chain, not 0
        setFloatParam (proc, params::comp2Type, 0.0f);                                 // outside, 0
        setFloatParam (proc, params::drive2Type, (float) tubamp::DriveFx::Type::fuzz); // in chain, not 0
        setFloatParam (proc, params::modTremVoice, (float) tubamp::TremoloFx::Voice::smooth); // in chain, = default

        juce::MemoryBlock explicitBlob;
        proc.getStateInformation (explicitBlob);
        const auto explicitVar = proc.captureStateVar();

        const auto heldExplicit = [] (tubamp::TubampAudioProcessor& p)
        {
            return choiceIndex (p, params::gateMode) == 0
                && choiceIndex (p, params::compType) == (int) tubamp::CompFx::Type::opto
                && choiceIndex (p, params::comp2Type) == 0
                && choiceIndex (p, params::drive2Type) == (int) tubamp::DriveFx::Type::fuzz
                && choiceIndex (p, params::modTremVoice) == (int) tubamp::TremoloFx::Voice::smooth;
        };

        const auto scramble = [] (tubamp::TubampAudioProcessor& p)
        {
            setFloatParam (p, params::gateMode, (float) tubamp::GateFx::Mode::smooth);
            setFloatParam (p, params::compType, (float) tubamp::CompFx::Type::squash);
            setFloatParam (p, params::comp2Type, (float) tubamp::CompFx::Type::fet);
            setFloatParam (p, params::drive2Type, (float) tubamp::DriveFx::Type::boost);
            setFloatParam (p, params::modTremVoice, (float) tubamp::TremoloFx::Voice::bias);
        };

        scramble (proc);
        proc.setStateInformation (explicitBlob.getData(), (int) explicitBlob.getSize());
        ok &= expect (heldExplicit (proc), "(e) host state: present type ids restore as saved (0, non-0, default; in chain or not)");

        scramble (proc);
        proc.applyStateVar (explicitVar);
        ok &= expect (heldExplicit (proc), "(e) preset: present type ids restore as saved (0, non-0, default; in chain or not)");
    }

    // --- (d) presets and A/B (captureStateVar / applyStateVar)
    const auto modern = source.captureStateVar();
    auto older = source.captureStateVar();

    if (auto* values = older.getProperty ("params", juce::var()).getDynamicObject())
        for (auto* p : newParams (source))
            values->removeProperty (juce::Identifier (p->paramID));

    {
        tubamp::TubampAudioProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);
        setNewParamsOffDefault (proc);

        proc.applyStateVar (modern);
        ok &= expect (sameRange (paramSnapshot (proc), sourceValues, 0, total),
                      "(d) a preset from this layout round-trips all 261 parameters exactly");

        // The processor still holds the source's non-default types: absent ids must not
        // leave them ringing, in or out of the chain.
        proc.applyStateVar (older);
        ok &= expect (pinnedAsExpected (proc, order),
                      "(d) old-layout preset: in-chain type ids at Classic, the rest at the recommended type");
        ok &= expect (newParamsAtDefault (proc, typeIds), "(d) old-layout preset: the other new parameters reset to their defaults");
        ok &= expect (sameRange (paramSnapshot (proc), sourceValues, 0, (size_t) kBaseParamCount),
                      "(d) old-layout preset: all 166 existing parameters restore exactly");

        setFloatParam (proc, params::eqType, (float) tubamp::EqFx::Type::graphic);
        ValueRecorder recorder (*proc.apvts.getParameter (params::eqType));
        proc.applyStateVar (older);
        ok &= expect (recorder.seen.size() == 1 && recorder.seen.front() == 0.0f,
                      "(d) old-layout preset: a pinned id moves to Classic without passing through the default");

        // A/B: a slot captured on this layout and recalled over other settings.
        proc.applyStateVar (modern);
        proc.presets.captureToSlot (0);
        setNewParamsOffDefault (proc);
        setFloatParam (proc, params::compType, 0.0f);
        proc.presets.recallSlot (0);
        ok &= expect (sameRange (paramSnapshot (proc), sourceValues, 0, total),
                      "(d) an A/B slot round-trips all 261 parameters exactly");
    }

    // A preset that predates the arrangeable chain loads as the classic nine and pins them.
    {
        auto* state = new juce::DynamicObject();
        auto* values = new juce::DynamicObject();
        values->setProperty ("comp_threshold", -33.0);
        state->setProperty ("version", 1);
        state->setProperty ("params", juce::var (values));

        tubamp::TubampAudioProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);
        setNewParamsOffDefault (proc);
        proc.applyStateVar (juce::var (state));

        ok &= expect (proc.getChainOrder() == chain::classicOrder() && pinnedAsExpected (proc, chain::classicOrder()),
                      "(d) a v1 preset without a chain: classic blocks at Classic, instances 2-3 recommended");
    }

    return ok;
}

/** Feeds `numChannels` of sine for one second, then silence, and returns how many
    seconds after the input stopped the output's 50 ms peak first fell below -60 dBFS
    (-1 if it never did within `limitSeconds`). Clears `finite` on any non-finite
    sample. The processor must already be prepared for `numChannels`. */
double secondsToSilence (tubamp::TubampAudioProcessor& proc, int numChannels, double sampleRate,
                         int blockSize, double limitSeconds, bool& finite)
{
    juce::AudioBuffer<float> buffer (numChannels, blockSize);
    juce::MidiBuffer midi;

    const int inputBlocks = (int) std::ceil (sampleRate / blockSize);
    const int tailBlocks = (int) std::ceil (sampleRate * limitSeconds / blockSize);
    const int windowSamples = (int) std::lround (sampleRate * 0.05);
    const float threshold = 0.001f; // -60 dBFS

    float windowPeak = 0.0f;
    int windowFill = 0;

    for (int block = 0; block < inputBlocks + tailBlocks; ++block)
    {
        const bool feeding = block < inputBlocks;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);

            for (int i = 0; i < blockSize; ++i)
                data[i] = feeding ? 0.3f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                             * 220.0 * (block * blockSize + i) / sampleRate)
                                  : 0.0f;
        }

        proc.processBlock (buffer, midi);

        for (int i = 0; i < blockSize; ++i)
        {
            float peak = 0.0f;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float v = buffer.getSample (ch, i);

                if (! std::isfinite (v))
                    finite = false;

                peak = std::max (peak, std::abs (v));
            }

            if (feeding)
                continue;

            windowPeak = std::max (windowPeak, peak);

            if (++windowFill == windowSamples)
            {
                if (windowPeak < threshold)
                    return (double) ((block - inputBlocks) * blockSize + i + 1) / sampleRate;

                windowPeak = 0.0f;
                windowFill = 0;
            }
        }
    }

    return -1.0;
}

/** Every delay type x mode and every reverb type, through the real processor on both
    host layouts, at the default controls: only finite samples, and silence again
    within a generous limit once the input stops (a runaway loop or a stuck denormal
    tail would never get there). */
bool runEngineRenderTests (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    std::printf ("engine renders:\n");

    namespace chain = tubamp::chain;
    bool ok = true;
    const double limitSeconds = 12.0;

    const auto check = [&] (const char* label, int numChannels, bool finite, double silentAfter)
    {
        char what[160];
        std::snprintf (what, sizeof (what), "%s is only finite samples (%s host)",
                       label, numChannels == 2 ? "stereo" : "mono");
        ok &= expect (finite, what);

        std::snprintf (what, sizeof (what), "%s returns below -60 dBFS within %.0f s (%s host)",
                       label, limitSeconds, numChannels == 2 ? "stereo" : "mono");
        ok &= expect (silentAfter >= 0.0, what);

        // A block that never ran would be "silent" at once; every engine at its
        // defaults leaves well over 100 ms of tail.
        std::snprintf (what, sizeof (what), "...and leaves a tail first (%.2f s)", silentAfter);
        ok &= expect (silentAfter > 0.1, what);
    };

    const auto renderBoth = [&] (const chain::Order& order, const char* label)
    {
        proc.setChainOrder (order, {});
        proc.prepareToPlay (sampleRate, blockSize);

        bool finite = true;
        const double stereoSilent = secondsToSilence (proc, 2, sampleRate, blockSize, limitSeconds, finite);
        check (label, 2, finite, stereoSilent);

        bool monoFinite = true;
        double monoSilent = 0.0;
        const bool monoRan = withMonoHost (proc, sampleRate, blockSize, [&]
        {
            monoSilent = secondsToSilence (proc, 1, sampleRate, blockSize, limitSeconds, monoFinite);
        });

        if (monoRan)
            check (label, 1, monoFinite, monoSilent);
    };

    setBoolParam (proc, params::delayOn, true);
    setBoolParam (proc, params::reverbOn, true);

    for (int type = 0; type < tubamp::DelayFx::numTypes; ++type)
    {
        for (int mode = 0; mode < 3; ++mode)
        {
            setFloatParam (proc, params::delayType, (float) type);
            setFloatParam (proc, params::delayMode, (float) mode);

            char label[96];
            std::snprintf (label, sizeof (label), "delay %s / mode %s",
                           params::delayTypeNames[type], params::delayModeChoices[mode].toRawUTF8());
            renderBoth ({ BlockId::delay }, label);
        }
    }

    for (int type = 0; type < tubamp::ReverbFx::numTypes; ++type)
    {
        setFloatParam (proc, params::reverbType, (float) type);

        char label[96];
        std::snprintf (label, sizeof (label), "reverb %s", params::reverbTypeNames[type]);
        renderBoth ({ BlockId::reverb }, label);
    }

    setFloatParam (proc, params::delayType, (float) tubamp::DelayFx::Type::digital);
    setFloatParam (proc, params::delayMode, 0.0f);
    setFloatParam (proc, params::reverbType, (float) tubamp::ReverbFx::Type::plate);
    proc.setChainOrder (chain::defaultOrder(), {});
    proc.prepareToPlay (sampleRate, blockSize);

    return ok;
}

/** A play head that reports nothing but a fixed tempo. */
class FixedTempoPlayHead final : public juce::AudioPlayHead
{
public:
    explicit FixedTempoPlayHead (double bpmToReport) : bpm (bpmToReport) {}

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        info.setIsPlaying (true);
        return info;
    }

    double bpm;
};

/** Sends one impulse through a delay-only chain and returns the sample index (from the
    impulse) of the strongest repeat, ignoring the dry impulse itself. */
int firstRepeatIndex (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize,
                      double maxSeconds)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;

    // Let the smoothed delay length settle on silence before the impulse.
    const int settleBlocks = (int) std::ceil (sampleRate * 0.5 / blockSize);

    for (int block = 0; block < settleBlocks; ++block)
    {
        buffer.clear();
        proc.processBlock (buffer, midi);
    }

    const int tailBlocks = (int) std::ceil (sampleRate * maxSeconds / blockSize);
    int peakIndex = -1;
    float peak = 0.0f;

    for (int block = 0; block < tailBlocks; ++block)
    {
        buffer.clear();

        if (block == 0)
        {
            buffer.setSample (0, 0, 1.0f);
            buffer.setSample (1, 0, 1.0f);
        }

        proc.processBlock (buffer, midi);

        for (int i = 0; i < blockSize; ++i)
        {
            const int index = block * blockSize + i;
            const float magnitude = std::abs (buffer.getSample (0, i));

            if (index > (int) (sampleRate * 0.005) && magnitude > peak)
            {
                peak = magnitude;
                peakIndex = index;
            }
        }
    }

    return peak > 0.01f ? peakIndex : -1;
}

/** Tempo sync (docs/REVERB-DELAY.md §1.1): with delay_sync on, a delay's
    first repeat lands on the note length at the play head's tempo (120 BPM when there
    is no play head), and delay_time is ignored. */
bool runDelaySyncTests (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    std::printf ("delay tempo sync:\n");

    namespace chain = tubamp::chain;
    bool ok = true;
    const int quarterIndex = 9; // "1/4" in delaySyncNames

    ok &= expect (juce::String (params::delaySyncNames[quarterIndex]) == "1/4",
                  "delay_sync choice 9 is 1/4");

    proc.setChainOrder ({ BlockId::delay }, {});
    setBoolParam (proc, params::delayOn, true);
    setFloatParam (proc, params::delayType, (float) tubamp::DelayFx::Type::digital);
    setFloatParam (proc, params::delayMode, 0.0f);
    setFloatParam (proc, params::delayTime, 300.0f); // must be ignored while synced
    setFloatParam (proc, params::delayFeedback, 0.0f);
    setFloatParam (proc, params::delayMix, 1.0f);
    setFloatParam (proc, params::delayModAmt, 0.0f);
    setFloatParam (proc, params::delayDuck, 0.0f);
    setFloatParam (proc, params::delaySync, (float) quarterIndex);

    const int tolerance = (int) std::lround (sampleRate * 0.005); // 5 ms

    const auto expectRepeatAt = [&] (double expectedSeconds, const char* what)
    {
        proc.prepareToPlay (sampleRate, blockSize);
        const int index = firstRepeatIndex (proc, sampleRate, blockSize, expectedSeconds + 0.5);
        const int expected = (int) std::lround (sampleRate * expectedSeconds);
        const bool hit = index >= 0 && std::abs (index - expected) <= tolerance;

        if (! hit)
            std::printf ("        first repeat at sample %d, expected ~%d\n", index, expected);

        ok &= expect (hit, what);
    };

    FixedTempoPlayHead playHead { 100.0 };
    proc.setPlayHead (&playHead);
    expectRepeatAt (0.6, "Digital, sync 1/4 at 100 BPM: first repeat at 600 ms (+-5 ms)");

    playHead.bpm = 150.0;
    expectRepeatAt (0.4, "Digital, sync 1/4 at 150 BPM: first repeat at 400 ms (+-5 ms)");

    proc.setPlayHead (nullptr);
    expectRepeatAt (0.5, "Digital, sync 1/4 with no play head: 120 BPM, first repeat at 500 ms (+-5 ms)");

    proc.setPlayHead (nullptr);

    for (const auto* id : { params::delayType, params::delayTime, params::delayFeedback,
                            params::delayMix, params::delayModAmt, params::delaySync })
        if (auto* p = proc.apvts.getParameter (id))
            p->setValueNotifyingHost (p->getDefaultValue());

    proc.setChainOrder (chain::defaultOrder(), {});
    proc.prepareToPlay (sampleRate, blockSize);

    return ok;
}
//==============================================================================
// Gate and compressor engines through the processor (docs/FX-ENGINES.md §6.1, §6.2).
// Classic's bit-identity with the old code is tubamp_golden --legacy-check's job; these
// prove the new modes really run, the deferred curve lines up with the resampled amp,
// switches stay inside the engines' fades, the instances are independent, the meters
// read sanely, and the wired path allocates nothing.

/** Runs `input` (mono, fed to both channels) through the processor as it stands and
    returns channel 0. `beforeBlock (startSample)` runs before each block. */
std::vector<float> renderSignal (tubamp::TubampAudioProcessor& proc, const std::vector<float>& input,
                                 int blockSize, const std::function<void (int)>& beforeBlock = {})
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> out;
    out.reserve (input.size());

    for (int start = 0; start < (int) input.size(); start += blockSize)
    {
        const int n = std::min (blockSize, (int) input.size() - start);

        if (beforeBlock)
            beforeBlock (start);

        float* channels[2] { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        juce::AudioBuffer<float> view (channels, 2, n);

        for (int ch = 0; ch < 2; ++ch)
            std::copy (input.begin() + start, input.begin() + start + n, channels[ch]);

        proc.processBlock (view, midi);
        out.insert (out.end(), channels[0], channels[0] + n);
    }

    return out;
}

std::vector<float> sineSignal (double sampleRate, double hz, float amplitude, int numSamples, int startAt = 0)
{
    std::vector<float> out ((size_t) numSamples, 0.0f);

    for (int i = startAt; i < numSamples; ++i)
        out[(size_t) i] = amplitude * (float) std::sin (2.0 * juce::MathConstants<double>::pi * hz * (i - startAt) / sampleRate);

    return out;
}

float peakOf (const std::vector<float>& x, size_t from, size_t to)
{
    float peak = 0.0f;

    for (size_t i = from; i < std::min (to, x.size()); ++i)
        peak = std::max (peak, std::abs (x[i]));

    return peak;
}

double rmsOf (const std::vector<float>& x, size_t from, size_t to)
{
    double sum = 0.0;
    to = std::min (to, x.size());

    for (size_t i = from; i < to; ++i)
        sum += (double) x[i] * x[i];

    return to > from ? std::sqrt (sum / (double) (to - from)) : 0.0;
}

double toDb (double linear) { return 20.0 * std::log10 (std::max (linear, 1.0e-12)); }

void resetParams (tubamp::TubampAudioProcessor& proc, std::initializer_list<const char*> ids)
{
    for (const auto* id : ids)
        if (auto* p = proc.apvts.getParameter (id))
            p->setValueNotifyingHost (p->getDefaultValue());
}

/** What the editor's timer does, minus the event: every dynamics meter read and reset. */
struct DynReading
{
    int gateState;
    float gateGr, gateLevel;
    std::array<float, params::maxInstances> compGr;
};

DynReading readDynMeters (tubamp::TubampAudioProcessor& proc)
{
    DynReading r {};
    r.gateState = proc.gateState.load();
    r.gateGr = proc.gateGrDb.exchange (0.0f);
    r.gateLevel = proc.gateLevelDb.exchange (tubamp::TubampAudioProcessor::kDynMeterFloorDb);

    for (int k = 0; k < params::maxInstances; ++k)
        r.compGr[(size_t) k] = proc.compGrDb[(size_t) k].exchange (0.0f);

    return r;
}

const std::initializer_list<const char*> kGateParamIds { params::gateOn, params::gateThreshold, params::gateMode,
                                                         params::gateRelease, params::gateHold, params::gateRange,
                                                         params::gateScHpf };

std::vector<const char*> compParamIds()
{
    std::vector<const char*> ids;

    for (int k = 0; k < params::maxInstances; ++k)
        for (const auto* id : { params::compOnIds[k], params::compThresholdIds[k], params::compRatioIds[k],
                                params::compAttackIds[k], params::compReleaseIds[k], params::compMakeupIds[k],
                                params::compTypeIds[k], params::compKneeIds[k], params::compMixIds[k],
                                params::compScHpfIds[k], params::compAutoMakeupIds[k] })
            ids.push_back (id);

    return ids;
}

void resetDynamicsParams (tubamp::TubampAudioProcessor& proc)
{
    resetParams (proc, kGateParamIds);

    for (const auto* id : compParamIds())
        resetParams (proc, { id });

    setBoolParam (proc, params::ampEqOn, true);
}

/** Gate modes and compressor types through the processor, at the smoke test's rate. */
bool runGateCompEngineTests (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    std::printf ("gate and compressor engines:\n");

    bool ok = true;
    const auto samples = [sampleRate] (double seconds) { return (int) std::lround (seconds * sampleRate); };
    char what[200];

    // --- Tight mutes a palm-mute gap that Classic leaves open: 50 ms notes (1 kHz, -12 dBFS,
    // 10 ms raised-cosine edges) every 100 ms over a -60 dBFS RMS noise floor, threshold -40.
    // The soft edges and the high note keep the always-on DC blocker from ringing into the
    // gaps, which no gate in front of it could remove.
    {
        std::vector<float> chug ((size_t) samples (2.0));
        uint32_t seed = 12345u;

        for (size_t i = 0; i < chug.size(); ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            const float noise = ((float) (seed >> 8) / 8388608.0f - 1.0f) * 0.001f * std::sqrt (3.0f);
            const double t = (double) i / sampleRate;
            const double inPeriod = std::fmod (t, 0.1);
            const double envelope = inPeriod >= 0.05 ? 0.0
                                  : 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * std::min (1.0, std::min (inPeriod, 0.05 - inPeriod) / 0.01));
            chug[i] = noise + (float) (0.25 * envelope * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * t));
        }

        proc.setChainOrder ({ BlockId::gate }, {});
        setBoolParam (proc, params::gateOn, true);
        setFloatParam (proc, params::gateThreshold, -40.0f);

        const auto medianGapDb = [&] (int mode)
        {
            setFloatParam (proc, params::gateMode, (float) mode);
            proc.prepareToPlay (sampleRate, blockSize);
            const auto out = renderSignal (proc, chug, blockSize);
            std::vector<double> gaps;

            // The last 10 ms of each gap, from the fourth period on.
            for (int period = 3; period < 20; ++period)
            {
                const auto from = (size_t) samples (period * 0.1 + 0.09), to = (size_t) samples (period * 0.1 + 0.1);
                gaps.push_back (toDb (rmsOf (out, from, to) / rmsOf (chug, from, to)));
            }

            std::sort (gaps.begin(), gaps.end());
            return gaps[gaps.size() / 2];
        };

        const double classicGap = medianGapDb ((int) tubamp::GateFx::Mode::classic);
        const double tightGap = medianGapDb ((int) tubamp::GateFx::Mode::tight);
        const double smoothGap = medianGapDb ((int) tubamp::GateFx::Mode::smooth);
        std::printf ("  50 ms gaps, median gain: Classic %.1f dB, Tight %.1f dB, Smooth %.1f dB\n",
                     classicGap, tightGap, smoothGap);

        ok &= expect (classicGap > -6.0, "Classic leaves a 50 ms palm-mute gap open (> -6 dB)");
        ok &= expect (tightGap <= -30.0, "Tight mutes the same gap (<= -30 dB)");
        ok &= expect (smoothGap < classicGap - 3.0 && smoothGap > tightGap,
                      "Smooth sits between them (an expander, not a mute)");
    }

    // --- Clean compressor on a steady -10 dBFS tone: T -20, R 4, knee 6 puts the tone
    // 10 dB over, past the knee, so GR = 10 * (1 - 1/4) = 7.5 dB and the output peak
    // follows the static curve to -17.5 dBFS.
    {
        proc.setChainOrder ({ BlockId::comp }, {});
        setBoolParam (proc, params::compOn, true);
        setFloatParam (proc, params::compType, (float) tubamp::CompFx::Type::clean);
        setFloatParam (proc, params::compThreshold, -20.0f);
        setFloatParam (proc, params::compRatio, 4.0f);
        setFloatParam (proc, params::compKnee, 6.0f);
        setFloatParam (proc, params::compMakeup, 0.0f);
        proc.prepareToPlay (sampleRate, blockSize);

        const auto tone = sineSignal (sampleRate, 1000.0, std::pow (10.0f, -10.0f / 20.0f), samples (1.5));
        const int lastBlockStart = ((int) tone.size() - 1) / blockSize * blockSize;
        const auto out = renderSignal (proc, tone, blockSize, [&] (int start)
        {
            if (start == lastBlockStart)
                readDynMeters (proc);
        });
        const auto last = readDynMeters (proc);

        const double outDb = toDb (peakOf (out, (size_t) samples (1.0), out.size()));
        std::printf ("  Clean, -10 dBFS tone: GR meter %.2f dB, output peak %.2f dBFS\n", (double) last.compGr[0], outDb);
        ok &= expect (std::abs (last.compGr[0] - 7.5f) <= 0.3f, "Clean reports 7.5 +- 0.3 dB of gain reduction");
        ok &= expect (std::abs (outDb + 17.5) <= 0.3, "...and its output peak sits on the static curve (-17.5 +- 0.3 dBFS)");
        ok &= expect (last.compGr[1] == 0.0f && last.compGr[2] == 0.0f, "...while the absent instances read 0");
    }

    // --- Instances are independent: comp and comp2 on the same settings render the same
    // samples, and comp3's settings reach neither of them.
    {
        const auto configure = [&] (int k, int type, float threshold, float ratio, float makeup, float mix, float scHpf)
        {
            setBoolParam (proc, params::compOnIds[k], true);
            setFloatParam (proc, params::compTypeIds[k], (float) type);
            setFloatParam (proc, params::compThresholdIds[k], threshold);
            setFloatParam (proc, params::compRatioIds[k], ratio);
            setFloatParam (proc, params::compAttackIds[k], 2.0f);
            setFloatParam (proc, params::compReleaseIds[k], 200.0f);
            setFloatParam (proc, params::compMakeupIds[k], makeup);
            setFloatParam (proc, params::compKneeIds[k], 3.0f);
            setFloatParam (proc, params::compMixIds[k], mix);
            setFloatParam (proc, params::compScHpfIds[k], scHpf);
            setBoolParam (proc, params::compAutoMakeupIds[k], true);
        };

        configure (0, (int) tubamp::CompFx::Type::fet, -30.0f, 8.0f, 3.0f, 80.0f, 100.0f);
        configure (1, (int) tubamp::CompFx::Type::fet, -30.0f, 8.0f, 3.0f, 80.0f, 100.0f);
        configure (2, (int) tubamp::CompFx::Type::opto, -15.0f, 2.0f, 0.0f, 100.0f, 20.0f);

        const auto renderOne = [&] (BlockId id, DynReading& meters)
        {
            proc.setChainOrder ({ id }, {});
            proc.prepareToPlay (sampleRate, blockSize);
            std::vector<float> captured;
            runSine (proc, sampleRate, blockSize, 40, &captured);
            meters = readDynMeters (proc);
            return captured;
        };

        DynReading m1 {}, m2 {}, m3 {}, m2b {};
        const auto one = renderOne (BlockId::comp, m1);
        const auto two = renderOne (BlockId::comp2, m2);
        const auto three = renderOne (BlockId::comp3, m3);

        configure (2, (int) tubamp::CompFx::Type::squash, -50.0f, 20.0f, 12.0f, 50.0f, 300.0f);
        const auto twoAgain = renderOne (BlockId::comp2, m2b);

        ok &= expectIdentical (one, two, "comp and comp2 on the same settings render identical samples");
        ok &= expectIdentical (two, twoAgain, "comp3's settings do not reach comp2");
        ok &= expect (firstDifference (two, three) >= 0, "comp3 on its own settings renders differently");
        ok &= expect (m2.compGr[1] > 0.0f && m2.compGr[0] == 0.0f && m2.compGr[2] == 0.0f,
                      "comp2 alone moves only its own meter");
        ok &= expect (m3.compGr[2] > 0.0f && m3.compGr[2] != m2.compGr[1], "comp3 reports its own gain reduction");
    }

    resetDynamicsParams (proc);

    // --- Type switches mid-stream stay inside the engines' own fades.
    {
        const int window = std::max (1, samples (0.001));

        // Compressor: a sustained -10 dBFS tone through every type; the switcher fades the
        // effect out and back in over 20 + 20 ms, so no 1 ms window may step by more than
        // 3 dB (check_comp.py C22's bound).
        proc.setChainOrder ({ BlockId::comp }, {});
        setBoolParam (proc, params::compOn, true);
        setFloatParam (proc, params::compType, 0.0f);
        setFloatParam (proc, params::compMakeup, 0.0f);
        proc.prepareToPlay (sampleRate, blockSize);

        const auto loud = sineSignal (sampleRate, 1000.0, std::pow (10.0f, -10.0f / 20.0f), samples (1.6));
        static constexpr int compSequence[] { 1, 2, 3, 4, 0, 4, 1 };
        int nextSwitch = 0;
        const auto compOut = renderSignal (proc, loud, blockSize, [&] (int start)
        {
            if (nextSwitch < (int) std::size (compSequence) && start >= samples (0.2 * (nextSwitch + 1)))
                setFloatParam (proc, params::compType, (float) compSequence[nextSwitch++]);
        });

        double worstCompStepDb = 0.0;

        for (int w = samples (0.1) / window; (w + 2) * window <= (int) compOut.size(); ++w)
        {
            const auto a = toDb (peakOf (compOut, (size_t) (w * window), (size_t) ((w + 1) * window)));
            const auto b = toDb (peakOf (compOut, (size_t) ((w + 1) * window), (size_t) ((w + 2) * window)));
            worstCompStepDb = std::max (worstCompStepDb, std::abs (b - a));
        }

        std::snprintf (what, sizeof (what), "compressor type switches mid-note: largest 1 ms step %.2f dB (<= 3 dB)",
                       worstCompStepDb);
        ok &= expect (nextSwitch == (int) std::size (compSequence) && worstCompStepDb <= 3.0, what);

        // Gate: a steady tone 10 dB under the threshold, where the three modes disagree most
        // (Classic ~shut, Tight at range -20 dB, Smooth at -8 dB). The engine crossfades the
        // two curves linearly over 20 ms, so the level may move by at most 1/20 of the swing
        // per ms; an instant switch would jump the whole swing at once.
        proc.setChainOrder ({ BlockId::gate }, {});
        setBoolParam (proc, params::gateOn, true);
        setFloatParam (proc, params::gateThreshold, -40.0f);
        setFloatParam (proc, params::gateRange, -20.0f);
        setFloatParam (proc, params::gateMode, (float) tubamp::GateFx::Mode::tight);
        proc.prepareToPlay (sampleRate, blockSize);

        const float quietAmp = std::pow (10.0f, -50.0f / 20.0f);
        const auto quiet = sineSignal (sampleRate, 1000.0, quietAmp, samples (1.6));
        static constexpr int gateSequence[] { 2, 0, 1, 0, 2, 1 };
        nextSwitch = 0;
        const auto gateOut = renderSignal (proc, quiet, blockSize, [&] (int start)
        {
            if (nextSwitch < (int) std::size (gateSequence) && start >= samples (0.2 * (nextSwitch + 1)))
                setFloatParam (proc, params::gateMode, (float) gateSequence[nextSwitch++]);
        });

        float worstGateStep = 0.0f;
        const float firstPeak = peakOf (gateOut, (size_t) samples (0.15), (size_t) samples (0.2));
        float lowest = firstPeak, highest = firstPeak;

        for (int w = samples (0.1) / window; (w + 2) * window <= (int) gateOut.size(); ++w)
        {
            const float a = peakOf (gateOut, (size_t) (w * window), (size_t) ((w + 1) * window));
            const float b = peakOf (gateOut, (size_t) ((w + 1) * window), (size_t) ((w + 2) * window));
            worstGateStep = std::max (worstGateStep, std::abs (b - a));
            lowest = std::min (lowest, b);
            highest = std::max (highest, b);
        }

        const float widestSwing = highest - lowest;
        std::printf ("  gate mode switches: level swing %.3g, largest 1 ms step %.3g (fade bound %.3g)\n",
                     (double) widestSwing, (double) worstGateStep, (double) (quietAmp / 20.0f));
        ok &= expect (widestSwing > 0.3f * quietAmp, "the gate modes really disagree on the quiet tone (control)");
        ok &= expect (nextSwitch == (int) std::size (gateSequence) && worstGateStep <= 1.25f * quietAmp / 20.0f,
                      "gate mode switches move at most 1/20 of the full swing per ms (the 20 ms crossfade)");
    }

    resetDynamicsParams (proc);

    // --- Meters: sane while running, idle once the block stops running.
    {
        proc.setChainOrder ({ BlockId::gate, BlockId::comp }, {});
        setBoolParam (proc, params::gateOn, true);
        setFloatParam (proc, params::gateMode, (float) tubamp::GateFx::Mode::tight);
        setFloatParam (proc, params::gateThreshold, -40.0f);
        setBoolParam (proc, params::compOn, true);
        setFloatParam (proc, params::compType, (float) tubamp::CompFx::Type::clean);
        proc.prepareToPlay (sampleRate, blockSize);

        const auto tone = sineSignal (sampleRate, 1000.0, std::pow (10.0f, -10.0f / 20.0f), samples (0.5));
        renderSignal (proc, tone, blockSize);
        readDynMeters (proc);
        renderSignal (proc, std::vector<float> (tone.begin(), tone.begin() + blockSize), blockSize);
        const auto playing = readDynMeters (proc);

        std::printf ("  playing: gate state %d, GR %.2f dB, level %.2f dBFS; comp GR %.2f dB\n", playing.gateState,
                     (double) playing.gateGr, (double) playing.gateLevel, (double) playing.compGr[0]);
        ok &= expect (playing.gateState == 2 && playing.gateGr < 0.1f, "a note over the threshold reads open, no GR");
        ok &= expect (std::abs (playing.gateLevel + 10.0f) <= 1.0f, "...the detector level reads the note (-10 +- 1 dBFS)");
        ok &= expect (std::abs (playing.compGr[0] - 7.5f) <= 0.5f, "...and the compressor reads its gain reduction");

        renderSignal (proc, std::vector<float> ((size_t) samples (0.1), 0.0f), blockSize);
        readDynMeters (proc);
        renderSignal (proc, std::vector<float> ((size_t) blockSize, 0.0f), blockSize);
        const auto silent = readDynMeters (proc);

        std::printf ("  100 ms of silence: gate state %d, GR %.1f dB, level %.1f dBFS\n", silent.gateState,
                     (double) silent.gateGr, (double) silent.gateLevel);
        ok &= expect (silent.gateState == 0 && silent.gateGr == 90.0f && silent.gateLevel == -120.0f,
                      "100 ms after the note the gate reads closed, 90 dB of GR (mute), level at the floor");

        setBoolParam (proc, params::gateOn, false);
        setBoolParam (proc, params::compOn, false);
        renderSignal (proc, std::vector<float> (tone.begin(), tone.begin() + blockSize), blockSize);
        const auto bypassed = readDynMeters (proc);
        ok &= expect (bypassed.gateState == -1 && bypassed.gateGr == 0.0f && bypassed.gateLevel == -120.0f
                          && bypassed.compGr[0] == 0.0f,
                      "bypassed, both meters read idle (state -1, 0 dB, -120 dBFS, 0 dB)");

        setBoolParam (proc, params::gateOn, true);
        setBoolParam (proc, params::compOn, true);
        proc.setChainOrder ({}, {});
        renderSignal (proc, std::vector<float> (tone.begin(), tone.begin() + blockSize), blockSize);
        const auto absent = readDynMeters (proc);
        ok &= expect (absent.gateState == -1 && absent.gateGr == 0.0f && absent.gateLevel == -120.0f
                          && absent.compGr[0] == 0.0f,
                      "out of the chain, both meters read idle");
    }

    resetDynamicsParams (proc);
    proc.setChainOrder (tubamp::chain::defaultOrder(), {});
    proc.prepareToPlay (sampleRate, blockSize);

    return ok;
}

/** G16, the deferred curve's alignment on a resampling amp. A Tight gate in front of the
    amp measures the dry input and its curve lands on the model's output, which the
    resampler has delayed; the processor delays the curve by the latency it reports for
    that span. Three measurements, at 44.1 and 96 kHz against the model's own rate:

    - exact: with a muted gate the first non-zero output sample is an exact index, and the
      deferred curve must open precisely `reported latency` samples after the same gate
      opens on its own;
    - against the audio: the amp's true delay is the phase delay of a low tone through it,
      less the same at the model's own rate (where nothing resamples), and the curve may
      miss it only by what the reported latency itself misses;
    - against a gate placed after the amp: printed for reference, after removing the
      offset the model's own response puts between the two even at its own rate. */
bool runGateAlignmentTests (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    std::printf ("gate curve alignment on a resampling amp:\n");

    bool ok = true;
    char what[240];
    const double nativeRate = proc.namEngine.getModelInfo().sampleRate;
    const float amplitude = 0.3f;

    setBoolParam (proc, params::ampOn, true);
    setBoolParam (proc, params::ampEqOn, false);
    setBoolParam (proc, params::gateOn, true);
    setFloatParam (proc, params::gateMode, (float) tubamp::GateFx::Mode::tight);
    setFloatParam (proc, params::gateRange, -90.0f);

    const auto render = [&] (const tubamp::chain::Order& order, double rate, const std::vector<float>& input)
    {
        proc.setChainOrder (order, {});
        proc.prepareToPlay (rate, blockSize);
        return renderSignal (proc, input, blockSize);
    };

    const auto firstNonZero = [] (const std::vector<float>& x)
    {
        for (size_t i = 0; i < x.size(); ++i)
            if (x[i] != 0.0f)
                return (long) i;

        return -1L;
    };

    // Phase delay of the amp at 200 Hz, seconds, modulo the tone's period.
    const auto ampPhaseDelay = [&] (double rate)
    {
        const int n = (int) std::lround (rate);
        const auto tone = sineSignal (rate, 200.0, 0.01f, n);
        const auto out = render ({ BlockId::amp }, rate, tone);
        double inI = 0.0, inQ = 0.0, outI = 0.0, outQ = 0.0;

        for (int i = n / 2; i < n; ++i)
        {
            const double w = 2.0 * juce::MathConstants<double>::pi * 200.0 * i / rate;
            inI += tone[(size_t) i] * std::sin (w);
            inQ += tone[(size_t) i] * std::cos (w);
            outI += out[(size_t) i] * std::sin (w);
            outQ += out[(size_t) i] * std::cos (w);
        }

        return (std::atan2 (inQ, inI) - std::atan2 (outQ, outI)) / (2.0 * juce::MathConstants<double>::pi * 200.0);
    };

    // 250 ms of digital silence, then a 1 kHz tone from a zero crossing. Each gate opens at
    // a tenth of what it listens to: the input in front of the amp, the amp's output behind.
    struct Edges { long alone, deferred, after; int latency; };

    const auto edges = [&] (double rate, int onset)
    {
        const auto input = sineSignal (rate, 1000.0, amplitude, (int) std::lround (0.5 * rate), onset);
        const auto ampOnly = render ({ BlockId::amp }, rate, input);
        const float ampPeak = peakOf (ampOnly, (size_t) onset + 400, ampOnly.size());

        Edges e {};
        e.latency = proc.namEngine.getLatencySamples();
        setFloatParam (proc, params::gateThreshold, (float) toDb (0.1 * amplitude));
        e.alone = firstNonZero (render ({ BlockId::gate }, rate, input)) - onset;
        e.deferred = firstNonZero (render ({ BlockId::gate, BlockId::amp }, rate, input)) - onset;
        setFloatParam (proc, params::gateThreshold, (float) toDb (0.1 * ampPeak));
        e.after = firstNonZero (render ({ BlockId::amp, BlockId::gate }, rate, input)) - onset;
        return e;
    };

    const auto native = edges (nativeRate, (int) std::lround (0.25 * nativeRate));
    const double nativeDelay = ampPhaseDelay (nativeRate);
    const double modelOffset = (double) (native.after - native.deferred) / nativeRate;

    std::snprintf (what, sizeof (what), "at the model's own rate (%.0f Hz) nothing resamples and the curve is not delayed", nativeRate);
    ok &= expect (native.latency == 0 && native.deferred == native.alone, what);

    for (const double rate : { 44100.0, 96000.0 })
    {
        if (rate == nativeRate)
            continue;

        const auto e = edges (rate, (int) std::lround (0.25 * rate));
        const double period = 1.0 / 200.0;
        double trueDelay = ampPhaseDelay (rate) - nativeDelay;
        trueDelay -= period * std::round (trueDelay / period);
        const double trueSamples = trueDelay * rate;
        const double missed = trueSamples - (double) e.latency;
        const double afterResidual = (double) (e.after - e.deferred) - modelOffset * rate;

        std::printf ("  %.0f Hz: reported latency %d, true resampler delay %.2f samples; deferred curve opens at +%ld "
                     "(uncompensated +%ld); gate after the amp at +%ld, %.2f samples later than at the model's rate\n",
                     rate, e.latency, trueSamples, e.deferred, e.alone, e.after, afterResidual);

        std::snprintf (what, sizeof (what), "%.0f Hz: the model resamples (latency %d > 0, control)", rate, e.latency);
        ok &= expect (e.latency > 0, what);
        std::snprintf (what, sizeof (what), "%.0f Hz: the deferred Tight curve is delayed by exactly the reported latency", rate);
        ok &= expect (e.alone >= 0 && e.deferred == e.alone + e.latency, what);
        std::snprintf (what, sizeof (what),
                       "%.0f Hz: ...which meets the amp's audio within 4 samples (misses by %.2f: what the reported latency misses)",
                       rate, missed);
        ok &= expect (std::abs (missed) <= 4.0 && std::abs (afterResidual) <= 4.0, what);
    }

    resetParams (proc, kGateParamIds);
    setBoolParam (proc, params::ampEqOn, true);
    proc.setChainOrder (tubamp::chain::defaultOrder(), {});
    proc.prepareToPlay (sampleRate, blockSize);

    return ok;
}

/** The wired gate and compressor path allocates nothing: a guard counts every operator
    new/delete and malloc/free the audio thread makes inside processBlock, across mode and
    type switches, bypass toggles, the deferred apply onto the amp and re-entry resets. */
bool runDynamicsRtTests (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    std::printf ("gate and compressor real-time safety:\n");

    namespace chain = tubamp::chain;
    bool ok = true;

    {
        const long before = allocCount.load();
        juce::HeapBlock<float> probe;

        {
            const AllocationGuard guard;
            probe.malloc (64);
            auto boxed = std::make_unique<int> (1);
            juce::ignoreUnused (boxed);
        }

        ok &= expect (allocCount.load() - before >= 2, "the guard sees operator new and malloc on its thread (control)");
    }

    const chain::Order full { BlockId::gate, BlockId::comp, BlockId::comp2, BlockId::comp3, BlockId::amp };
    proc.setChainOrder (full, {});
    setBoolParam (proc, params::gateOn, true);
    setBoolParam (proc, params::ampOn, true);

    for (int k = 0; k < params::maxInstances; ++k)
    {
        setBoolParam (proc, params::compOnIds[k], true);
        setFloatParam (proc, params::compTypeIds[k], (float) k);
    }

    proc.prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    long allocations = 0;
    int armedBlocks = 0;

    for (int block = 0; block < 400; ++block)
    {
        // Every change happens on this (message) thread between blocks, as a host's would.
        if (block % 25 == 0 && block > 0)
        {
            const int step = block / 25;
            setFloatParam (proc, params::gateMode, (float) (step % tubamp::GateFx::numModes));
            setFloatParam (proc, params::compTypeIds[step % params::maxInstances], (float) (step % tubamp::CompFx::numTypes));
            setFloatParam (proc, params::gateThreshold, step % 2 == 0 ? -40.0f : -70.0f);
            setBoolParam (proc, params::compOnIds[(step + 1) % params::maxInstances], step % 3 != 0);

            if (step % 4 == 1)
                proc.setChainOrder ({ BlockId::comp2, BlockId::amp }, {});
            else if (step % 4 == 2)
                proc.setChainOrder (full, {});
        }

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < blockSize; ++i)
            {
                const int n = block * blockSize + i;
                const bool note = (n / (int) (sampleRate * 0.15)) % 2 == 0;
                buffer.setSample (ch, i, note ? 0.3f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * n / sampleRate)
                                              : 0.0f);
            }

        // The first blocks settle the latency report and warm the model.
        if (block < 20)
        {
            proc.processBlock (buffer, midi);
            continue;
        }

        const long before = allocCount.load();

        {
            const AllocationGuard guard;
            proc.processBlock (buffer, midi);
        }

        allocations += allocCount.load() - before;
        ++armedBlocks;
    }

    char what[200];
    std::snprintf (what, sizeof (what), "%d processBlock calls with gate, comp x3 and the amp: %ld allocations or frees",
                   armedBlocks, allocations);
    ok &= expect (allocations == 0, what);

    resetDynamicsParams (proc);
    proc.setChainOrder (chain::defaultOrder(), {});
    proc.prepareToPlay (sampleRate, blockSize);

    return ok;
}

//==============================================================================
// EQ and drive engines through the processor (docs/FX-ENGINES.md §6.3, §6.4).

float rawValue (tubamp::TubampAudioProcessor& proc, const char* id)
{
    return proc.apvts.getRawParameterValue (id)->load();
}

tubamp::DriveParams rawDriveParams (tubamp::TubampAudioProcessor& proc, int k)
{
    tubamp::DriveParams dp;
    dp.type    = (int) rawValue (proc, params::driveTypeIds[k]);
    dp.gainDb  = rawValue (proc, params::driveGainIds[k]);
    dp.toneHz  = rawValue (proc, params::driveToneIds[k]);
    dp.levelDb = rawValue (proc, params::driveLevelIds[k]);
    return dp;
}

tubamp::EqParams rawEqParams (tubamp::TubampAudioProcessor& proc, int k)
{
    tubamp::EqParams ep;
    ep.type      = (int) rawValue (proc, params::eqTypeIds[k]);
    ep.bass      = rawValue (proc, params::eqBassIds[k]);
    ep.mid       = rawValue (proc, params::eqMidIds[k]);
    ep.treble    = rawValue (proc, params::eqTrebleIds[k]);
    ep.lowCutHz  = rawValue (proc, params::eqLowCutIds[k]);
    ep.highCutHz = rawValue (proc, params::eqHighCutIds[k]);
    ep.midFreqHz = rawValue (proc, params::eqMidFreqIds[k]);
    ep.midQ      = rawValue (proc, params::eqMidQIds[k]);
    ep.tiltDb    = rawValue (proc, params::eqTiltIds[k]);
    ep.levelDb   = rawValue (proc, params::eqLevelIds[k]);
    ep.voice     = (int) rawValue (proc, params::eqVoiceIds[k]);

    for (int band = 0; band < params::numEqGraphicBands; ++band)
        ep.graphicDb[band] = rawValue (proc, params::eqGraphicIds[band][k]);

    return ep;
}

/** PluginProcessor.cpp's applySmoothedGain, verbatim. */
void applySmoothedGainLikeProcessor (juce::AudioBuffer<float>& buffer, int numSamples, juce::SmoothedValue<float>& smoothed)
{
    const float start = smoothed.getCurrentValue();
    const float end = smoothed.skip (numSamples);

    if (juce::approximatelyEqual (start, end))
        buffer.applyGain (0, numSamples, start);
    else
        buffer.applyGainRamp (0, numSamples, start, end);
}

/** Both channels of the processor's output and of the same input run, in lockstep, through
    `engine` wrapped in the processor's always-on stages exactly as processBlock runs them
    (input trim, then the engine, the DC blocker and the output level). `beforeBlock` changes
    parameters between blocks, as a host would; the engine reads them back raw. The caller
    sets the chain and calls prepareToPlay first. */
struct Lockstep
{
    std::vector<float> processor, engine;
};

Lockstep renderLockstep (tubamp::TubampAudioProcessor& proc, const std::vector<float>& input, double sampleRate,
                         int blockSize, const std::function<void (int)>& beforeBlock,
                         const std::function<void (juce::dsp::AudioBlock<float>)>& engine)
{
    juce::SmoothedValue<float> inputTrimLin { 1.0f }, outputLevelLin { 1.0f };

    for (auto* smoothed : { &inputTrimLin, &outputLevelLin })
        smoothed->reset (sampleRate, 0.02);

    inputTrimLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (rawValue (proc, params::inputTrim)));
    outputLevelLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (rawValue (proc, params::outputLevel)));

    tubamp::DcBlocker dc;
    dc.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });

    juce::AudioBuffer<float> procBuffer (2, blockSize), engineBuffer (2, blockSize);
    juce::MidiBuffer midi;
    Lockstep out;

    for (int start = 0; start < (int) input.size(); start += blockSize)
    {
        const int n = std::min (blockSize, (int) input.size() - start);

        if (beforeBlock)
            beforeBlock (start);

        float* pc[2] { procBuffer.getWritePointer (0), procBuffer.getWritePointer (1) };
        float* ec[2] { engineBuffer.getWritePointer (0), engineBuffer.getWritePointer (1) };
        juce::AudioBuffer<float> procView (pc, 2, n), engineView (ec, 2, n);

        for (int ch = 0; ch < 2; ++ch)
        {
            std::copy (input.begin() + start, input.begin() + start + n, pc[ch]);
            std::copy (input.begin() + start, input.begin() + start + n, ec[ch]);
        }

        proc.processBlock (procView, midi);

        const juce::ScopedNoDenormals noDenormals;
        juce::dsp::AudioBlock<float> block (engineView);
        inputTrimLin.setTargetValue (juce::Decibels::decibelsToGain (rawValue (proc, params::inputTrim)));
        applySmoothedGainLikeProcessor (engineView, n, inputTrimLin);
        engine (block);
        dc.process (block);
        outputLevelLin.setTargetValue (juce::Decibels::decibelsToGain (rawValue (proc, params::outputLevel)));
        applySmoothedGainLikeProcessor (engineView, n, outputLevelLin);

        for (int ch = 0; ch < 2; ++ch)
        {
            out.processor.insert (out.processor.end(), pc[ch], pc[ch] + n);
            out.engine.insert (out.engine.end(), ec[ch], ec[ch] + n);
        }
    }

    return out;
}

/** A small chord with some top end: 110, 440 and 2637 Hz. */
std::vector<float> chordSignal (double sampleRate, double seconds)
{
    std::vector<float> x ((size_t) std::lround (seconds * sampleRate));

    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) i / sampleRate, w = 2.0 * juce::MathConstants<double>::pi;
        x[i] = (float) (0.2 * std::sin (w * 110.0 * t) + 0.12 * std::sin (w * 440.0 * t) + 0.05 * std::sin (w * 2637.0 * t));
    }

    return x;
}

/** check_eq.py's zipper metric, with an 8th-order Butterworth high-pass at 1 kHz in place of
    its brick-wall FFT: the peak (dBFS) of what is left above 1 kHz between 0.45 and 1.1 s. */
double clickDb (const std::vector<float>& x, double sampleRate)
{
    static constexpr double qs[] { 0.50979558, 0.60134489, 0.89997622, 2.56291545 };
    std::vector<double> y (x.begin(), x.end());
    const double w = std::tan (juce::MathConstants<double>::pi * 1000.0 / sampleRate);

    for (const double q : qs)
    {
        const double norm = 1.0 / (1.0 + w / q + w * w);
        const double a1 = 2.0 * (w * w - 1.0) * norm, a2 = (1.0 - w / q + w * w) * norm;
        double z1 = 0.0, z2 = 0.0;

        for (auto& v : y)
        {
            const double out = norm * v + z1;
            z1 = -2.0 * norm * v - a1 * out + z2;
            z2 = norm * v - a2 * out;
            v = out;
        }
    }

    double peak = 0.0;

    for (size_t i = (size_t) (0.45 * sampleRate); i < std::min (y.size(), (size_t) (1.1 * sampleRate)); ++i)
        peak = std::max (peak, std::abs (y[i]));

    return toDb (peak);
}

/** Complex amplitude of an `hz` tone in x[from, to); the span should hold whole cycles. */
std::complex<double> toneAt (const std::vector<float>& x, double sampleRate, double hz, size_t from, size_t to)
{
    std::complex<double> sum;

    for (size_t i = from; i < to; ++i)
    {
        const double w = 2.0 * juce::MathConstants<double>::pi * hz * (double) i / sampleRate;
        sum += (double) x[i] * std::complex<double> (std::cos (w), -std::sin (w));
    }

    return sum * (2.0 / (double) (to - from));
}

/** juce::dsp::StateVariableTPTFilter's low-pass at its default resonance (Classic drive's
    tone filter): the prewarped bilinear transform of 1 / (s^2 + sqrt2 s + 1). */
std::complex<double> svfLowpass (double hz, double cutoff, double sampleRate)
{
    const double pi = juce::MathConstants<double>::pi;
    const double omega = std::tan (pi * hz / sampleRate) / std::tan (pi * cutoff / sampleRate);
    return 1.0 / std::complex<double> (1.0 - omega * omega, std::sqrt (2.0) * omega);
}

/** Boost's one-pole high cut (Drive.cpp CleanDrive), prewarped at its corner. */
std::complex<double> onePoleLowpass (double hz, double cutoff, double sampleRate)
{
    const double pi = juce::MathConstants<double>::pi;
    return 1.0 / std::complex<double> (1.0, std::tan (pi * hz / sampleRate) / std::tan (pi * cutoff / sampleRate));
}

/** Exact flat, knob-step clicks, and every EQ and drive type against the engine alone. */
bool runEqDriveEngineTests (double sampleRate, int blockSize)
{
    std::printf ("EQ and drive engines:\n");

    namespace chain = tubamp::chain;
    bool ok = true;
    char what[240];

    // --- exact flat (D2 a): Classic at exactly 5/5/5 hands the audio on bit for bit in every
    // EQ instance (the amp's tone stack: runAmpEqTests).
    {
        tubamp::TubampAudioProcessor fresh;
        const auto dry = renderChain (fresh, {}, sampleRate, blockSize);

        for (int k = 0; k < params::maxInstances; ++k)
        {
            setFloatParam (fresh, params::eqTypeIds[k], (float) tubamp::EqFx::Type::classic);
            const auto flat = renderChain (fresh, { chain::instanceId (BlockId::eq, k) }, sampleRate, blockSize);
            std::snprintf (what, sizeof (what), "eq instance %d: Classic at exactly 5/5/5 returns the input bit for bit", k + 1);
            ok &= expectIdentical (dry, flat, what);

            // Not bit for bit: eq_tilt and eq_level read their default 0 back from the parameter
            // as 8.9e-8 and 1.8e-7 dB (tests/golden/params-v4.tsv), which the engine applies.
            setFloatParam (fresh, params::eqTypeIds[k], (float) tubamp::EqFx::Type::parametric);
            const auto parametric = renderChain (fresh, { chain::instanceId (BlockId::eq, k) }, sampleRate, blockSize);
            double worst = 0.0;

            for (size_t i = 0; i < dry.size(); ++i)
                worst = std::max (worst, (double) std::abs (parametric[i] - dry[i]));

            std::snprintf (what, sizeof (what), "eq instance %d: Parametric at its parameter defaults is within %.1f dBFS of the "
                           "input (bound -140)", k + 1, toDb (worst));
            ok &= expect (toDb (worst) <= -140.0, what);
        }
    }

    // --- a knob step under a low note (82.41 Hz, -12 dBFS) is smoothed inside the engine:
    // through the processor it clicks no more than the engine alone does (D-4).
    {
        const auto note = sineSignal (sampleRate, 82.41, juce::Decibels::decibelsToGain (-12.0f), (int) std::lround (1.4 * sampleRate));
        const int stepAt = (int) std::lround (0.5 * sampleRate);

        struct KnobStep { const char* label; const char* eqId; const char* ampId; int band; float to; };
        static constexpr KnobStep knobSteps[] { { "bass 5 -> 10", params::eqBass, params::ampEqBass, 0, 10.0f },
                                                { "mid 5 -> 0", params::eqMid, params::ampEqMid, 1, 0.0f },
                                                { "treble 5 -> 10", params::eqTreble, params::ampEqTreble, 2, 10.0f } };

        for (const auto& s : knobSteps)
        {
            for (const bool amp : { false, true })
            {
                tubamp::TubampAudioProcessor fresh;
                setFloatParam (fresh, params::eqType, (float) tubamp::EqFx::Type::classic);
                fresh.setChainOrder ({ amp ? BlockId::amp : BlockId::eq }, {});
                fresh.prepareToPlay (sampleRate, blockSize);
                bool stepped = false;
                const auto out = renderSignal (fresh, note, blockSize, [&] (int start)
                {
                    if (! stepped && start >= stepAt)
                    {
                        setFloatParam (fresh, amp ? s.ampId : s.eqId, s.to);
                        stepped = true;
                    }
                });

                tubamp::EqFx alone;
                alone.prepare ({ sampleRate, (juce::uint32) blockSize, 1 });
                tubamp::EqParams ep;
                ep.type = (int) tubamp::EqFx::Type::classic;
                std::vector<float> engineOut (note);

                for (int start = 0; start < (int) engineOut.size(); start += blockSize)
                {
                    const int n = std::min (blockSize, (int) engineOut.size() - start);

                    if (start >= stepAt)
                        (s.band == 0 ? ep.bass : s.band == 1 ? ep.mid : ep.treble) = s.to;

                    float* p = engineOut.data() + start;
                    alone.setParameters (ep);
                    alone.process (juce::dsp::AudioBlock<float> (&p, 1, (size_t) n));
                }

                const double viaProcessor = clickDb (out, sampleRate), engineOnly = clickDb (engineOut, sampleRate);
                std::snprintf (what, sizeof (what), "%s Classic %s step: %.1f dBFS above 1 kHz through the processor, "
                               "%.1f dBFS from the engine alone (bound: engine + 1 dB, and -80)",
                               amp ? "amp tone stack" : "eq", s.label, viaProcessor, engineOnly);
                ok &= expect (viaProcessor <= engineOnly + 1.0 && viaProcessor <= -80.0, what);
            }
        }
    }

    // --- every type, through the processor, is what the engine alone makes of the same input
    // with the same parameters each block, bit for bit (knob steps and switches included).
    const auto input = chordSignal (sampleRate, 1.0);
    const auto at = [sampleRate] (double seconds) { return (int) std::lround (seconds * sampleRate); };
    const auto stepsAt = [&at] (std::vector<std::pair<double, std::function<void()>>> steps)
    {
        auto next = std::make_shared<size_t> (0);
        return [steps, next, at] (int start)
        {
            while (*next < steps.size() && start >= at (steps[*next].first))
                steps[(*next)++].second();
        };
    };

    const auto driveCase = [&] (const char* label, std::function<void (tubamp::TubampAudioProcessor&)> setup,
                                std::vector<std::pair<double, std::function<void()>>> steps,
                                tubamp::TubampAudioProcessor& fresh)
    {
        setup (fresh);
        fresh.setChainOrder ({ BlockId::drive }, {});
        fresh.prepareToPlay (sampleRate, blockSize);

        tubamp::DriveFx alone;
        alone.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });
        const auto r = renderLockstep (fresh, input, sampleRate, blockSize, stepsAt (std::move (steps)),
                                       [&] (juce::dsp::AudioBlock<float> block)
        {
            alone.setParameters (rawDriveParams (fresh, 0));
            alone.process (block);
        });

        std::snprintf (what, sizeof (what), "drive %s through the processor equals the engine alone", label);
        ok &= expectIdentical (r.processor, r.engine, what);
        ok &= expect (firstDifference (r.processor, input) >= 0, "...and it changes the signal (control)");
    };

    for (int type = 0; type < tubamp::DriveFx::numTypes; ++type)
    {
        tubamp::TubampAudioProcessor fresh;
        char label[96];
        std::snprintf (label, sizeof (label), "%s (gain, tone and level steps)", params::driveTypeNames[type]);
        driveCase (label, [type] (tubamp::TubampAudioProcessor& p)
        {
            setBoolParam (p, params::driveOn, true);
            setFloatParam (p, params::driveType, (float) type);
            setFloatParam (p, params::driveGain, 20.0f);
            setFloatParam (p, params::driveTone, 3000.0f);
            setFloatParam (p, params::driveLevel, -3.0f);
        },
        { { 0.3, [&fresh] { setFloatParam (fresh, params::driveGain, 30.0f); } },
          { 0.5, [&fresh] { setFloatParam (fresh, params::driveTone, 900.0f); } },
          { 0.7, [&fresh] { setFloatParam (fresh, params::driveLevel, 2.0f); } } }, fresh);
    }

    {
        tubamp::TubampAudioProcessor fresh;
        driveCase ("type switches Classic -> Screamer -> Boost -> Fuzz", [] (tubamp::TubampAudioProcessor& p)
        {
            setBoolParam (p, params::driveOn, true);
            setFloatParam (p, params::driveType, 0.0f);
        },
        { { 0.2, [&fresh] { setFloatParam (fresh, params::driveType, 1.0f); } },
          { 0.45, [&fresh] { setFloatParam (fresh, params::driveType, 4.0f); } },
          { 0.7, [&fresh] { setFloatParam (fresh, params::driveType, 3.0f); } } }, fresh);
    }

    const auto eqCase = [&] (const char* label, std::function<void (tubamp::TubampAudioProcessor&)> setup,
                             std::vector<std::pair<double, std::function<void()>>> steps,
                             tubamp::TubampAudioProcessor& fresh)
    {
        setup (fresh);
        fresh.setChainOrder ({ BlockId::eq }, {});
        fresh.prepareToPlay (sampleRate, blockSize);

        tubamp::EqFx alone;
        alone.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });
        const auto r = renderLockstep (fresh, input, sampleRate, blockSize, stepsAt (std::move (steps)),
                                       [&] (juce::dsp::AudioBlock<float> block)
        {
            alone.setParameters (rawEqParams (fresh, 0));
            alone.process (block);
        });

        std::snprintf (what, sizeof (what), "eq %s through the processor equals the engine alone", label);
        ok &= expectIdentical (r.processor, r.engine, what);
        ok &= expect (firstDifference (r.processor, input) >= 0, "...and it changes the signal (control)");
    };

    {
        tubamp::TubampAudioProcessor fresh;
        eqCase ("Parametric (every control set, mid freq and tilt steps)", [] (tubamp::TubampAudioProcessor& p)
        {
            setFloatParam (p, params::eqType, (float) tubamp::EqFx::Type::parametric);
            setFloatParam (p, params::eqBass, 7.0f);
            setFloatParam (p, params::eqMid, 3.5f);
            setFloatParam (p, params::eqTreble, 6.5f);
            setFloatParam (p, params::eqLowCut, 90.0f);
            setFloatParam (p, params::eqHighCut, 9000.0f);
            setFloatParam (p, params::eqMidFreq, 1200.0f);
            setFloatParam (p, params::eqMidQ, 2.0f);
            setFloatParam (p, params::eqTilt, 1.5f);
            setFloatParam (p, params::eqLevel, -2.0f);
        },
        { { 0.4, [&fresh] { setFloatParam (fresh, params::eqMidFreq, 3000.0f); } },
          { 0.6, [&fresh] { setFloatParam (fresh, params::eqTilt, -4.0f); } } }, fresh);
    }

    for (int voice = 0; voice < tubamp::EqFx::numVoices; ++voice)
    {
        tubamp::TubampAudioProcessor fresh;
        char label[96];
        std::snprintf (label, sizeof (label), "Stack %s (bass and level steps)", params::eqVoiceNames[voice]);
        eqCase (label, [voice] (tubamp::TubampAudioProcessor& p)
        {
            setFloatParam (p, params::eqType, (float) tubamp::EqFx::Type::stack);
            setFloatParam (p, params::eqVoice, (float) voice);
            setFloatParam (p, params::eqMid, 3.0f);
            setFloatParam (p, params::eqTreble, 7.0f);
        },
        { { 0.4, [&fresh] { setFloatParam (fresh, params::eqBass, 9.0f); } },
          { 0.6, [&fresh] { setFloatParam (fresh, params::eqLevel, 4.0f); } } }, fresh);
    }

    {
        tubamp::TubampAudioProcessor fresh;
        eqCase ("Graphic (seven bands, band steps)", [] (tubamp::TubampAudioProcessor& p)
        {
            setFloatParam (p, params::eqType, (float) tubamp::EqFx::Type::graphic);

            for (int band = 0; band < params::numEqGraphicBands; ++band)
                setFloatParam (p, params::eqGraphicIds[band][0], (float) (band % 2 == 0 ? 3 * band - 9 : 8 - 2 * band));
        },
        { { 0.4, [&fresh] { setFloatParam (fresh, params::eqGraphicIds[1][0], 12.0f); } },
          { 0.6, [&fresh] { setFloatParam (fresh, params::eqGraphicIds[5][0], -12.0f); } } }, fresh);
    }

    {
        tubamp::TubampAudioProcessor fresh;
        eqCase ("type and voice switches Classic -> Stack -> Stack Tweed -> Graphic", [] (tubamp::TubampAudioProcessor& p)
        {
            setFloatParam (p, params::eqType, (float) tubamp::EqFx::Type::classic);
            setFloatParam (p, params::eqBass, 8.0f);
        },
        { { 0.2, [&fresh] { setFloatParam (fresh, params::eqType, (float) tubamp::EqFx::Type::stack); } },
          { 0.45, [&fresh] { setFloatParam (fresh, params::eqVoice, 0.0f); } },
          { 0.7, [&fresh] { setFloatParam (fresh, params::eqType, (float) tubamp::EqFx::Type::graphic); } } }, fresh);
    }

    return ok;
}

/** The drive's oversampling latency (FX-ENGINES.md §2.9, §6.3, D-7): what the processor
    reports against the delay measured through it, the report following type and on/off
    changes, and a drive in one split lane summing with an empty lane without a comb. */
bool runDriveLatencyTests (double sampleRate, int blockSize)
{
    std::printf ("drive latency:\n");

    namespace chain = tubamp::chain;
    bool ok = true;
    char what[300];
    const double pi = juce::MathConstants<double>::pi;

    struct TypeSetting { float gain, tone, levelDb; };
    // The engineer's own measurement settings (check_drive.py D2/D3): small signal, so the
    // clippers are linear and only the filters and the oversampling move the phase.
    static constexpr TypeSetting settings[tubamp::DriveFx::numTypes] {
        { 0.0f, 12000.0f, -40.0f }, { 0.0f, 4000.0f, -60.0f }, { 0.0f, 4000.0f, -60.0f },
        { 18.0f, 4000.0f, -120.0f }, { 0.0f, 12000.0f, -40.0f }, { 0.0f, 3000.0f, -40.0f } };

    // Phase delay at 1 kHz of the drive's chain against the empty chain at the same rate (so
    // the trim, DC blocker and output level cancel), seconds, with the reported latency.
    const auto phaseDelay = [&] (int type, double rate, int& reported)
    {
        const auto& s = settings[type];
        const int n = (int) std::lround (rate);
        const auto tone = sineSignal (rate, 1000.0, (float) std::pow (10.0, s.levelDb / 20.0), n);

        tubamp::TubampAudioProcessor fresh;
        setBoolParam (fresh, params::driveOn, true);
        setFloatParam (fresh, params::driveType, (float) type);
        setFloatParam (fresh, params::driveGain, s.gain);
        setFloatParam (fresh, params::driveTone, s.tone);

        fresh.setChainOrder ({}, {});
        fresh.prepareToPlay (rate, blockSize);
        const auto dry = renderSignal (fresh, tone, blockSize);

        fresh.setChainOrder ({ BlockId::drive }, {});
        fresh.prepareToPlay (rate, blockSize);
        const auto wet = renderSignal (fresh, tone, blockSize);
        reported = fresh.getLatencySamples();

        auto h = toneAt (wet, rate, 1000.0, (size_t) n / 2, (size_t) n) / toneAt (dry, rate, 1000.0, (size_t) n / 2, (size_t) n);

        // Classic, Boost and Treble have analytic tone filters: divide them out, so what is
        // left is the oversampling (Treble at 0 dB is an exact identity).
        if (type == (int) tubamp::DriveFx::Type::classic)
            h /= svfLowpass (1000.0, s.tone, rate);
        else if (type == (int) tubamp::DriveFx::Type::boost)
            h /= onePoleLowpass (1000.0, s.tone, rate);

        return -std::arg (h) / (2.0 * pi * 1000.0);
    };

    // The circuit types' own filters have no closed form here. Their oversampling factor is the
    // same at 44.1 and 48 kHz (and at 88.2 and 96 kHz), so their delay in SAMPLES is too, while
    // the circuit's phase in SECONDS is the same analog filter at both: the difference of the
    // two phase delays is latency x (1/fa - 1/fb), and the circuit drops out.
    std::printf ("  type        rate    reported  measured  (samples)\n");

    for (int type = 0; type < tubamp::DriveFx::numTypes; ++type)
    {
        const bool circuit = type >= 1 && type <= 3;

        for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            int reported = -1, unused = -1;
            double measured;

            if (circuit)
            {
                const double partner = rate == 192000.0 ? 176400.0
                                     : rate == 96000.0  ? 88200.0
                                                        : (rate == 44100.0 ? 48000.0 : 44100.0);
                const double tauA = phaseDelay (type, rate, reported), tauB = phaseDelay (type, partner, unused);
                measured = (tauA - tauB) / (1.0 / rate - 1.0 / partner);
            }
            else
            {
                measured = phaseDelay (type, rate, reported) * rate;
            }

            const int expected = tubamp::DriveFx::latencyFor (type, rate);
            std::printf ("  %-10s %6.0f   %4d     %7.3f\n", params::driveTypeNames[type], rate, reported, measured);
            std::snprintf (what, sizeof (what), "%s at %.0f Hz: reported latency %d (the engine's %d) is the measured delay %.3f "
                           "within the half-sample rounding", params::driveTypeNames[type], rate, reported, expected, measured);
            ok &= expect (reported == expected && std::abs ((double) reported - measured) <= 0.5, what);
        }
    }

    // --- the report follows drive_type and drive_on, from the audio thread and from the
    // message thread's own path (setChainOrder -> updateLatency).
    {
        tubamp::TubampAudioProcessor fresh;
        setBoolParam (fresh, params::driveOn, true);
        setFloatParam (fresh, params::driveType, 0.0f);
        fresh.setChainOrder ({ BlockId::drive }, {});
        fresh.prepareToPlay (sampleRate, blockSize);
        ok &= expect (fresh.getLatencySamples() == 3, "Classic drive in the chain: prepareToPlay reports 3");

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;
        const auto latencyAfter = [&] (double seconds)
        {
            int done = 0;

            do
            {
                buffer.clear();
                fresh.processBlock (buffer, midi);
                done += blockSize;
            }
            while (done < (int) std::ceil (seconds * sampleRate));

            return reportedLatency (fresh);
        };
        const auto latencyAfterBlock = [&] { return latencyAfter (0.0); };

        // The drive's own switch runs both types for 30 ms lined up at the larger latency and slides
        // a drop out over the 10 ms after that: the figure is the larger one until then, the new
        // type's once it is over.
        bool follows = true, holds = true;
        int previous = 3;

        // The drive runs Classic before the first switch, so each step below is a real switch.
        latencyAfter (0.05);

        for (const int type : { 1, 4, 2, 5, 3, 0 })
        {
            setFloatParam (fresh, params::driveType, (float) type);
            const int during = latencyAfterBlock();
            const int after = latencyAfter (0.05);
            const int expected = tubamp::DriveFx::latencyFor (type, sampleRate);
            std::printf ("  drive_type -> %s: latency %d during the switch, %d after it\n", params::driveTypeNames[type], during, after);
            follows &= after == expected;
            holds &= during == juce::jmax (previous, expected);
            previous = expected;
        }

        ok &= expect (follows, "the reported latency follows every drive_type change once the drive's switch is over (50 ms)");
        ok &= expect (holds, "...and while it runs it is the larger of the two types' latencies, what the drive applies");

        setBoolParam (fresh, params::driveOn, false);
        ok &= expect (latencyAfterBlock() == 0, "drive_on off: latency 0 (only a drive that runs counts, like the amp)");
        setBoolParam (fresh, params::driveOn, true);
        ok &= expect (latencyAfterBlock() == 3, "drive_on back on: latency 3 again");

        fresh.setChainOrder ({}, {});
        ok &= expect (fresh.getLatencySamples() == 0, "setChainOrder without the drive reports 0 at once (message thread)");
        setFloatParam (fresh, params::driveType, 1.0f);
        fresh.setChainOrder ({ BlockId::drive }, {});
        std::snprintf (what, sizeof (what), "setChainOrder with a Screamer drive reports %d at once (message thread)",
                       tubamp::DriveFx::latencyFor (1, sampleRate));
        ok &= expect (fresh.getLatencySamples() == tubamp::DriveFx::latencyFor (1, sampleRate), what);

        setBoolParam (fresh, params::drive2On, true);
        setFloatParam (fresh, params::drive2Type, 0.0f);
        fresh.setChainOrder ({ BlockId::drive, BlockId::drive2 }, {});
        std::snprintf (what, sizeof (what), "two drives in series add: Screamer + Classic = %d (once the first drive's own "
                       "switch from Classic is over)", tubamp::DriveFx::latencyFor (1, sampleRate) + 3);
        ok &= expect (latencyAfter (0.05) == tubamp::DriveFx::latencyFor (1, sampleRate) + 3, what);
    }

    // --- the gate's deferred Tight curve waits for a drive between the gate and the amp, by
    // the same per-block rule (no model: the amp itself adds nothing).
    {
        tubamp::TubampAudioProcessor fresh;
        const float amplitude = 0.3f;
        const int onset = (int) std::lround (0.25 * sampleRate);
        const auto input = sineSignal (sampleRate, 1000.0, amplitude, (int) std::lround (0.5 * sampleRate), onset);

        setBoolParam (fresh, params::gateOn, true);
        setFloatParam (fresh, params::gateMode, (float) tubamp::GateFx::Mode::tight);
        setFloatParam (fresh, params::gateRange, -90.0f);
        setFloatParam (fresh, params::gateThreshold, (float) toDb (0.1 * amplitude));
        setBoolParam (fresh, params::driveOn, true);
        setFloatParam (fresh, params::driveType, 0.0f);
        setFloatParam (fresh, params::driveGain, 0.0f);

        const auto opensAt = [&] (const chain::Order& order)
        {
            fresh.setChainOrder (order, {});
            fresh.prepareToPlay (sampleRate, blockSize);
            const auto out = renderSignal (fresh, input, blockSize);

            for (size_t i = 0; i < out.size(); ++i)
                if (out[i] != 0.0f)
                    return (long) i - onset;

            return -1L;
        };

        const long plain = opensAt ({ BlockId::gate, BlockId::amp });
        const long withDrive = opensAt ({ BlockId::gate, BlockId::drive, BlockId::amp });
        std::snprintf (what, sizeof (what), "a Classic drive between the gate and the amp delays the deferred Tight curve by its "
                       "3 samples (opens at +%ld, +%ld without the drive)", withDrive, plain);
        ok &= expect (plain >= 0 && withDrive == plain + 3, what);
    }

    // --- D18: a Classic drive (gain 0, tone 12 kHz, -40 dBFS) in lane A, lane B empty, Copy
    // split, summed. Lane B now waits for the drive's 3 samples. Per tone: the phase of lane A
    // with the tone filter divided out against lane B's, as the comb |1 + e^-j(phiA - phiB)| / 2
    // two equal lanes would make; the same against an unwaited lane B is the comb the study found.
    for (const double rate : { 44100.0, 48000.0, 96000.0 })
    {
        static constexpr double freqs[] { 100, 500, 1000, 2000, 4000, 6000, 7000, 7400, 8000, 9000, 10000, 12000, 14000, 16000 };
        const int n = (int) std::lround (rate);
        std::vector<float> tones ((size_t) n, 0.0f);
        const float amplitude = juce::Decibels::decibelsToGain (-40.0f) / (float) std::size (freqs);

        for (int i = 0; i < n; ++i)
            for (const double f : freqs)
                tones[(size_t) i] += amplitude * (float) std::sin (2.0 * pi * f * i / rate);

        tubamp::TubampAudioProcessor fresh;
        setBoolParam (fresh, params::driveOn, true);
        setFloatParam (fresh, params::driveType, 0.0f);
        setFloatParam (fresh, params::driveGain, 0.0f);
        setFloatParam (fresh, params::driveTone, 12000.0f);
        setFloatParam (fresh, params::splitMode, 0.0f);

        const auto render = [&] (const chain::Order& order, bool laneB)
        {
            setBoolParam (fresh, params::splitOn, laneB);
            fresh.setChainOrder (order, {});
            fresh.prepareToPlay (rate, blockSize);
            return renderSignal (fresh, tones, blockSize);
        };

        const chain::Order lanes { BlockId::split, BlockId::drive, BlockId::lane2, BlockId::mix };
        const auto both = render (lanes, true);
        const int laneLatency = fresh.getLatencySamples();
        const auto laneA = render (lanes, false);
        const auto dry = render ({}, true);

        double worstTo10k = 0.0, worstUnaligned = 0.0, at16k = 0.0;

        for (const double f : freqs)
        {
            const auto a = toneAt (laneA, rate, f, (size_t) n / 2, (size_t) n) / svfLowpass (f, 12000.0, rate);
            const auto summed = toneAt (both, rate, f, (size_t) n / 2, (size_t) n);
            const auto b = summed - toneAt (laneA, rate, f, (size_t) n / 2, (size_t) n);
            const auto unwaited = toneAt (dry, rate, f, (size_t) n / 2, (size_t) n);
            const auto comb = [&a] (std::complex<double> other)
            {
                return toDb (std::abs (a / std::abs (a) + other / std::abs (other)) / 2.0);
            };

            if (f <= 10000.0)
                worstTo10k = std::min (worstTo10k, comb (b));

            worstUnaligned = std::min (worstUnaligned, comb (unwaited));

            if (f == 16000.0)
                at16k = comb (b);
        }

        std::snprintf (what, sizeof (what), "D18 at %.0f Hz: Classic drive in lane A, lane B empty: reported %d; the lanes sum "
                       "within %.3f dB of flat to 10 kHz (%.2f dB at 16 kHz; without the wait the comb reaches %.1f dB)",
                       rate, laneLatency, worstTo10k, at16k, worstUnaligned);
        ok &= expect (laneLatency == 3 && worstTo10k >= -0.5, what);

        if (rate != 96000.0)
            ok &= expect (worstUnaligned < -3.0, "...and the unwaited comb is there to find (control)");
    }

    return ok;
}

/** Instances 2 and 3 have their own engines and parameters. */
bool runEqDriveInstanceTests (double sampleRate, int blockSize)
{
    std::printf ("EQ and drive instances:\n");

    namespace chain = tubamp::chain;
    bool ok = true;
    tubamp::TubampAudioProcessor fresh;

    setFloatParam (fresh, params::drive2Type, 1.0f);
    setFloatParam (fresh, params::drive2Gain, 24.0f);
    setFloatParam (fresh, params::drive3Type, 0.0f);
    setFloatParam (fresh, params::eq2Type, (float) tubamp::EqFx::Type::stack);
    setFloatParam (fresh, params::eq2Bass, 8.0f);
    setFloatParam (fresh, params::eq3Type, (float) tubamp::EqFx::Type::graphic);
    setFloatParam (fresh, params::eq3G800, 9.0f);

    const auto drive2 = renderChain (fresh, { BlockId::drive2 }, sampleRate, blockSize);
    const auto eq2 = renderChain (fresh, { BlockId::eq2 }, sampleRate, blockSize);
    const auto drive3 = renderChain (fresh, { BlockId::drive3 }, sampleRate, blockSize);
    const auto eq3 = renderChain (fresh, { BlockId::eq3 }, sampleRate, blockSize);
    const auto all = renderChain (fresh, { BlockId::drive2, BlockId::drive3, BlockId::eq2, BlockId::eq3 }, sampleRate, blockSize);

    ok &= expect (fresh.getLatencySamples() == tubamp::DriveFx::latencyFor (1, sampleRate) + 3,
                  "drive2 (Screamer) and drive3 (Classic) each add their own latency");

    // Everything instance 3 reads, moved; instance 2's renders must not notice.
    setFloatParam (fresh, params::drive3Type, 3.0f);
    setFloatParam (fresh, params::drive3Gain, 36.0f);
    setFloatParam (fresh, params::drive3Tone, 900.0f);
    setFloatParam (fresh, params::eq3Type, (float) tubamp::EqFx::Type::parametric);
    setFloatParam (fresh, params::eq3Bass, 0.0f);
    setFloatParam (fresh, params::eq3MidFreq, 5000.0f);

    ok &= expectIdentical (drive2, renderChain (fresh, { BlockId::drive2 }, sampleRate, blockSize),
                           "drive2 renders the same whatever drive3's type and knobs are");
    ok &= expectIdentical (eq2, renderChain (fresh, { BlockId::eq2 }, sampleRate, blockSize),
                           "eq2 renders the same whatever eq3's type and knobs are");
    ok &= expect (firstDifference (drive3, renderChain (fresh, { BlockId::drive3 }, sampleRate, blockSize)) >= 0
                      && firstDifference (eq3, renderChain (fresh, { BlockId::eq3 }, sampleRate, blockSize)) >= 0,
                  "...while instance 3's own renders follow its parameters (control)");
    ok &= expect (firstDifference (drive2, drive3) >= 0 && firstDifference (eq2, eq3) >= 0 && firstDifference (all, eq2) >= 0,
                  "instances 2 and 3 at different settings render differently");

    return ok;
}

/** The EQ and drive path allocates nothing on the audio thread: types, voices, knobs,
    bypass and re-entry all move between guarded blocks, with the amp in the chain. */
bool runEqDriveRtTests (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    std::printf ("EQ and drive real-time safety:\n");

    namespace chain = tubamp::chain;
    bool ok = true;

    const chain::Order full { BlockId::drive, BlockId::drive2, BlockId::drive3, BlockId::amp,
                              BlockId::eq, BlockId::eq2, BlockId::eq3 };
    proc.setChainOrder (full, {});
    setBoolParam (proc, params::ampOn, true);

    for (int k = 0; k < params::maxInstances; ++k)
    {
        setBoolParam (proc, params::driveOnIds[k], true);
        setBoolParam (proc, params::eqOnIds[k], true);
    }

    proc.prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    long allocations = 0;
    int armedBlocks = 0, latencyChanges = 0, lastLatency = proc.getLatencySamples();

    for (int block = 0; block < 400; ++block)
    {
        if (block % 20 == 0 && block > 0)
        {
            const int step = block / 20;
            const int k = step % params::maxInstances;
            setFloatParam (proc, params::driveTypeIds[k], (float) (step % tubamp::DriveFx::numTypes));
            setFloatParam (proc, params::driveGainIds[k], (float) (6 * (step % 7)));
            setFloatParam (proc, params::eqTypeIds[k], (float) (step % tubamp::EqFx::numTypes));
            setFloatParam (proc, params::eqVoiceIds[k], (float) (step % tubamp::EqFx::numVoices));
            setFloatParam (proc, params::eqBassIds[k], step % 2 == 0 ? 9.0f : 5.0f);
            setFloatParam (proc, params::eqGraphicIds[step % params::numEqGraphicBands][k], step % 3 == 0 ? 10.0f : -6.0f);
            setBoolParam (proc, params::ampEqOn, step % 3 != 1);
            setFloatParam (proc, params::ampEqTreble, step % 2 == 0 ? 5.0f : 7.5f);
            setBoolParam (proc, params::driveOnIds[(step + 1) % params::maxInstances], step % 4 != 0);

            if (step % 5 == 2)
                proc.setChainOrder ({ BlockId::eq2, BlockId::amp }, {});
            else if (step % 5 == 3)
                proc.setChainOrder (full, {});
        }

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < blockSize; ++i)
                buffer.setSample (ch, i, 0.3f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 110.0
                                                                  * (block * blockSize + i) / sampleRate));

        if (block < 20)
        {
            proc.processBlock (buffer, midi);
            continue;
        }

        const long before = allocCount.load();

        {
            const AllocationGuard guard;
            proc.processBlock (buffer, midi);
        }

        allocations += allocCount.load() - before;
        ++armedBlocks;

        if (proc.getLatencySamples() != lastLatency)
        {
            lastLatency = proc.getLatencySamples();
            ++latencyChanges;
        }
    }

    char what[220];
    std::snprintf (what, sizeof (what), "%d processBlock calls with drive x3, eq x3 and the amp (%d latency changes reported "
                   "from the audio thread): %ld allocations or frees", armedBlocks, latencyChanges, allocations);
    ok &= expect (allocations == 0 && latencyChanges > 0, what);

    for (int k = 0; k < params::maxInstances; ++k)
        resetParams (proc, { params::driveOnIds[k], params::driveTypeIds[k], params::driveGainIds[k], params::eqTypeIds[k],
                             params::eqVoiceIds[k], params::eqBassIds[k], params::eqOnIds[k] });

    for (int band = 0; band < params::numEqGraphicBands; ++band)
        for (int k = 0; k < params::maxInstances; ++k)
            resetParams (proc, { params::eqGraphicIds[band][k] });

    resetParams (proc, { params::ampEqOn, params::ampEqTreble });
    proc.setChainOrder (chain::defaultOrder(), {});
    proc.prepareToPlay (sampleRate, blockSize);

    return ok;
}

//==============================================================================
// The mod block (docs/FX-ENGINES.md §6.5): Modulation dispatches mod_type to ChorusFx,
// PhaserFx and TremoloFx and resolves tempo sync. Classic's bit-identity with the old
// block, mod_type changes included, is tubamp_golden --legacy-check's job.

/** A play head with a tempo and, while playing, a song position the test moves on. */
class TransportPlayHead final : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        info.setIsPlaying (playing);
        info.setPpqPosition (ppq);
        return info;
    }

    void advance (int numSamples, double sampleRate) noexcept
    {
        if (playing)
            ppq += numSamples * bpm / (60.0 * sampleRate);
    }

    double bpm = 120.0, ppq = 0.0;
    bool playing = true;
};

/** The three mod engines alone, fed instance k's parameters raw with the rate and
    song-position phase the caller resolves; `type` overrides mod_type when >= 0. */
struct ModEnginesAlone
{
    ModEnginesAlone (double sampleRate, int blockSize)
    {
        const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, 2 };
        chorus.prepare (spec);
        phaser.prepare (spec);
        tremolo.prepare (spec);
    }

    void run (tubamp::TubampAudioProcessor& proc, int k, juce::dsp::AudioBlock<float> block, float rateHz,
              double syncPhase, int type = -1)
    {
        const auto v = [&proc, k] (const char* const* ids) { return rawValue (proc, ids[k]); };

        switch (type >= 0 ? type : (int) v (params::modTypeIds))
        {
            case 0:
                chorus.setParameters ({ (int) v (params::modChorusModeIds), rateHz, v (params::modDepthIds), v (params::modMixIds),
                                        v (params::modWidthIds), v (params::modToneIds), syncPhase });
                chorus.process (block);
                break;

            case 1:
                phaser.setParameters ({ (int) v (params::modPhaserModelIds), rateHz, v (params::modDepthIds),
                                        v (params::modMixIds), v (params::modColorIds) > 0.5f, syncPhase });
                phaser.process (block);
                break;

            default:
                tremolo.setParameters ({ (int) v (params::modTremVoiceIds), rateHz, v (params::modDepthIds), v (params::modMixIds),
                                         v (params::modWidthIds), v (params::modToneIds), v (params::modShapeIds),
                                         v (params::modWaveIds), syncPhase });
                tremolo.process (block);
                break;
        }
    }

    /** What every voicing runs at with sync Off: mod_rate, free-running. */
    void runFree (tubamp::TubampAudioProcessor& proc, int k, juce::dsp::AudioBlock<float> block, int type = -1)
    {
        run (proc, k, block, rawValue (proc, params::modRateIds[k]), -1.0, type);
    }

    void reset (int type)
    {
        if (type == 0)      chorus.reset();
        else if (type == 1) phaser.reset();
        else                tremolo.reset();
    }

    tubamp::ChorusFx chorus;
    tubamp::PhaserFx phaser;
    tubamp::TremoloFx tremolo;
};

/** One channel of a Lockstep render (which stores each block's left, then right, samples). */
std::vector<float> lockstepChannel (const std::vector<float>& x, int blockSize, int channel)
{
    std::vector<float> out;
    out.reserve (x.size() / 2);

    for (size_t start = 0; start < x.size();)
    {
        const size_t n = std::min ((size_t) blockSize, (x.size() - start) / 2);
        out.insert (out.end(), x.begin() + (long) (start + (size_t) channel * n), x.begin() + (long) (start + (size_t) (channel + 1) * n));
        start += 2 * n;
    }

    return out;
}

/** A sine with exactly `period` samples per cycle. */
std::vector<float> periodicSine (int period, float amplitude, int numSamples)
{
    std::vector<float> x ((size_t) numSamples);

    for (int i = 0; i < numSamples; ++i)
        x[(size_t) i] = amplitude * (float) std::sin (2.0 * juce::MathConstants<double>::pi * (double) (i % period) / period);

    return x;
}

/** check_phaser.py P7's chord: a 3:4:5 triad on one period per n = round(fs / 1000) samples,
    two harmonics each, -12 dBFS peak, so every n-sample window of the steady chord has the
    same level and a level step between adjacent windows shows at full size. */
std::vector<float> p7Chord (double sampleRate, double seconds, int& n)
{
    n = (int) std::lround (sampleRate / 1000.0);
    std::vector<double> x ((size_t) std::lround (seconds * sampleRate), 0.0);
    double peak = 0.0;

    for (size_t i = 0; i < x.size(); ++i)
    {
        for (int k = 3; k <= 5; ++k)
            for (int h = 1; h <= 2; ++h)
                x[i] += std::sin (2.0 * juce::MathConstants<double>::pi * k * h * (double) (i % (size_t) n) / n) / h;

        peak = std::max (peak, std::abs (x[i]));
    }

    std::vector<float> out (x.size());

    for (size_t i = 0; i < x.size(); ++i)
        out[i] = (float) (x[i] * juce::Decibels::decibelsToGain (-12.0) / peak);

    return out;
}

/** P7's metric: the largest level change (dB) between an n-sample window and the next, for
    windows starting in [from, to). */
double largestLevelStepDb (const std::vector<float>& y, int from, int to, int n)
{
    std::vector<double> sum (y.size() + 1, 0.0);

    for (size_t i = 0; i < y.size(); ++i)
        sum[i + 1] = sum[i] + (double) y[i] * y[i];

    const auto level = [&sum, n] (int i) { return 10.0 * std::log10 (std::max ((sum[(size_t) (i + n)] - sum[(size_t) i]) / n, 1.0e-20)); };
    double worst = 0.0;

    for (int i = std::max (0, from); i < std::min (to, (int) y.size() - 2 * n); ++i)
        worst = std::max (worst, std::abs (level (i + n) - level (i)));

    return worst;
}

/** Every voicing through the processor is the engine alone; tempo sync lands on the note
    and follows the host; instances are their own; switches stay inside the engines' bounds. */
bool runModEngineTests (double sampleRate, int blockSize)
{
    std::printf ("mod engines:\n");

    namespace chain = tubamp::chain;
    bool ok = true;
    char what[300];
    const auto at = [sampleRate] (double seconds) { return (int) std::lround (seconds * sampleRate); };
    const auto stepsAt = [&at] (std::vector<std::pair<double, std::function<void()>>> steps)
    {
        auto next = std::make_shared<size_t> (0);
        return [steps, next, at] (int start)
        {
            while (*next < steps.size() && start >= at (steps[*next].first))
                steps[(*next)++].second();
        };
    };

    // --- every new voicing, with knob steps and voicing switches inside the engine, through
    // the processor is what the engine alone makes of the same input, bit for bit.
    {
        const auto input = chordSignal (sampleRate, 1.0);
        const auto modCase = [&] (const char* label, std::function<void (tubamp::TubampAudioProcessor&)> setup,
                                  std::vector<std::pair<double, std::function<void()>>> steps,
                                  tubamp::TubampAudioProcessor& fresh)
        {
            setup (fresh);
            fresh.setChainOrder ({ BlockId::mod }, {});
            fresh.prepareToPlay (sampleRate, blockSize);

            ModEnginesAlone alone (sampleRate, blockSize);
            const auto r = renderLockstep (fresh, input, sampleRate, blockSize, stepsAt (std::move (steps)),
                                           [&] (juce::dsp::AudioBlock<float> block) { alone.runFree (fresh, 0, block); });

            std::snprintf (what, sizeof (what), "mod %s through the processor equals the engine alone", label);
            ok &= expectIdentical (r.processor, r.engine, what);
            ok &= expect (firstDifference (r.processor, input) >= 0, "...and it changes the signal (control)");
        };

        for (int mode = 1; mode < tubamp::ChorusFx::numModes; ++mode)
        {
            tubamp::TubampAudioProcessor fresh;
            char label[120];
            std::snprintf (label, sizeof (label), "chorus %s (rate, depth, width and tone steps)", params::modChorusModeNames[mode]);
            modCase (label, [mode] (tubamp::TubampAudioProcessor& p)
            {
                setFloatParam (p, params::modType, 0.0f);
                setFloatParam (p, params::modChorusMode, (float) mode);
                setFloatParam (p, params::modRate, 0.7f);
                setFloatParam (p, params::modDepth, 0.6f);
                setFloatParam (p, params::modMix, 0.5f);
                setFloatParam (p, params::modWidth, 0.6f);
                setFloatParam (p, params::modTone, 0.3f);
            },
            { { 0.3, [&fresh] { setFloatParam (fresh, params::modRate, 2.5f); } },
              { 0.5, [&fresh] { setFloatParam (fresh, params::modDepth, 0.9f); } },
              { 0.7, [&fresh] { setFloatParam (fresh, params::modWidth, 1.0f); setFloatParam (fresh, params::modTone, 0.8f); } } },
            fresh);
        }

        {
            tubamp::TubampAudioProcessor fresh;
            modCase ("chorus mode switches Wide -> CE -> Tri -> Dimension", [] (tubamp::TubampAudioProcessor& p)
            {
                setFloatParam (p, params::modType, 0.0f);
                setFloatParam (p, params::modChorusMode, 1.0f);
            },
            { { 0.25, [&fresh] { setFloatParam (fresh, params::modChorusMode, 2.0f); } },
              { 0.5, [&fresh] { setFloatParam (fresh, params::modChorusMode, 4.0f); } },
              { 0.75, [&fresh] { setFloatParam (fresh, params::modChorusMode, 3.0f); } } }, fresh);
        }

        for (int model = 1; model < tubamp::PhaserFx::numModels; ++model)
        {
            for (const bool color : { false, true })
            {
                tubamp::TubampAudioProcessor fresh;
                char label[120];
                std::snprintf (label, sizeof (label), "phaser %s color %s (rate step; depth and mix moves ignored)",
                               params::modPhaserModelNames[model], color ? "on" : "off");
                modCase (label, [model, color] (tubamp::TubampAudioProcessor& p)
                {
                    setFloatParam (p, params::modType, 1.0f);
                    setFloatParam (p, params::modPhaserModel, (float) model);
                    setBoolParam (p, params::modColor, color);
                    setFloatParam (p, params::modRate, 0.5f);
                },
                { { 0.4, [&fresh] { setFloatParam (fresh, params::modRate, 3.0f); } },
                  { 0.6, [&fresh] { setFloatParam (fresh, params::modDepth, 0.9f); setFloatParam (fresh, params::modMix, 0.9f); } } },
                fresh);
            }
        }

        {
            tubamp::TubampAudioProcessor fresh;
            modCase ("phaser switches Phase 90 script -> block -> Small Stone on -> off", [] (tubamp::TubampAudioProcessor& p)
            {
                setFloatParam (p, params::modType, 1.0f);
                setFloatParam (p, params::modPhaserModel, 1.0f);
                setBoolParam (p, params::modColor, false);
            },
            { { 0.3, [&fresh] { setBoolParam (fresh, params::modColor, true); } },
              { 0.55, [&fresh] { setFloatParam (fresh, params::modPhaserModel, 2.0f); } },
              { 0.8, [&fresh] { setBoolParam (fresh, params::modColor, false); } } }, fresh);
        }

        for (int voice = 1; voice < tubamp::TremoloFx::numVoices; ++voice)
        {
            tubamp::TubampAudioProcessor fresh;
            char label[120];
            std::snprintf (label, sizeof (label), "tremolo %s (depth, wave, shape, width and tone steps)",
                           params::modTremVoiceNames[voice]);
            modCase (label, [voice] (tubamp::TubampAudioProcessor& p)
            {
                setFloatParam (p, params::modType, 2.0f);
                setFloatParam (p, params::modTremVoice, (float) voice);
                setFloatParam (p, params::modRate, 4.0f);
                setFloatParam (p, params::modDepth, 0.7f);
                setFloatParam (p, params::modWidth, 0.5f);
                setFloatParam (p, params::modShape, 0.3f);
                setFloatParam (p, params::modWave, 0.4f);
                setFloatParam (p, params::modTone, 0.6f);
            },
            { { 0.3, [&fresh] { setFloatParam (fresh, params::modDepth, 1.0f); } },
              { 0.5, [&fresh] { setFloatParam (fresh, params::modWave, 0.9f); setFloatParam (fresh, params::modShape, 0.7f); } },
              { 0.7, [&fresh] { setFloatParam (fresh, params::modWidth, 0.0f); setFloatParam (fresh, params::modTone, 0.2f); } } },
            fresh);
        }

        {
            tubamp::TubampAudioProcessor fresh;
            modCase ("tremolo voice switches Smooth -> Optical -> Bias -> Harmonic", [] (tubamp::TubampAudioProcessor& p)
            {
                setFloatParam (p, params::modType, 2.0f);
                setFloatParam (p, params::modTremVoice, 1.0f);
                setFloatParam (p, params::modRate, 4.0f);
            },
            { { 0.3, [&fresh] { setFloatParam (fresh, params::modTremVoice, 2.0f); } },
              { 0.55, [&fresh] { setFloatParam (fresh, params::modTremVoice, 3.0f); } },
              { 0.8, [&fresh] { setFloatParam (fresh, params::modTremVoice, 4.0f); } } }, fresh);
        }
    }

    // --- tempo sync. Through the processor each voicing equals the engine alone given the
    // note's rate and, while the host plays, the song position's phase frac(ppq / beats):
    // the engines' own checks (CH13, P5, T13) bound what an engine does with those. On top,
    // measured from the audio: the output repeats every 250 ms, and the tremolo's dips sit on
    // the eighth-note grid within T13's 2 ms.
    {
        const int eighth = 6;
        ok &= expect (juce::String (params::modSyncNames[eighth]) == "1/8" && tubamp::kModSyncBeats[eighth] == 0.5,
                      "mod_sync choice 6 is 1/8, half a beat per LFO cycle");

        const auto gcd = [] (int a, int b) { while (b != 0) { const int t = a % b; a = b; b = t; } return a; };
        const int p250 = at (0.25), p200 = at (0.2);
        // A carrier that repeats exactly within 250 and 200 ms, near 500 Hz.
        const int g = gcd (p250, p200);
        int carrier = g;

        for (int d = 1; d <= g; ++d)
            if (g % d == 0 && g / d <= 110)
            {
                carrier = g / d;
                break;
            }

        const auto input = periodicSine (carrier, 0.25f, at (3.0));

        struct GridLine { int sample; double bpm; };

        struct SyncRun
        {
            Lockstep r;
            std::vector<GridLine> grid;   // where the song position crosses each 1/8 note
        };

        const auto syncRun = [&] (tubamp::TubampAudioProcessor& fresh, TransportPlayHead* playHead, int syncIndex,
                                  std::vector<std::pair<double, double>> tempoSteps)
        {
            const double beats = tubamp::kModSyncBeats[syncIndex];
            fresh.setPlayHead (playHead);
            fresh.setChainOrder ({ BlockId::mod }, {});
            setFloatParam (fresh, params::modSync, (float) syncIndex);
            fresh.prepareToPlay (sampleRate, blockSize);

            ModEnginesAlone alone (sampleRate, blockSize);
            SyncRun run;
            int lastStart = 0;
            size_t nextTempo = 0;

            run.r = renderLockstep (fresh, input, sampleRate, blockSize, [&] (int start)
            {
                if (playHead == nullptr)
                    return;

                playHead->advance (start - lastStart, sampleRate);
                lastStart = start;

                while (nextTempo < tempoSteps.size() && start >= at (tempoSteps[nextTempo].first))
                    playHead->bpm = tempoSteps[nextTempo++].second;

                if (playHead->playing)
                {
                    const double perSample = playHead->bpm / (60.0 * sampleRate);
                    const double end = playHead->ppq + blockSize * perSample;

                    for (double line = std::ceil (playHead->ppq / beats) * beats; line < end; line += beats)
                        run.grid.push_back ({ start + (int) std::lround ((line - playHead->ppq) / perSample), playHead->bpm });
                }
            },
            [&] (juce::dsp::AudioBlock<float> block)
            {
                const double bpm = playHead != nullptr ? playHead->bpm : 120.0;
                const bool playing = playHead != nullptr && playHead->playing;
                const double cycles = playing ? playHead->ppq / beats : 0.0;
                alone.run (fresh, 0, block, (float) (bpm / (60.0 * beats)), playing ? cycles - std::floor (cycles) : -1.0);
            });

            fresh.setPlayHead (nullptr);
            return run;
        };

        /** rms (y[i] - y[i - lag]) re rms (y), over [1, 2.5] s, in dB. */
        const auto repeatResidualDb = [&] (const std::vector<float>& y, int lag)
        {
            double diff = 0.0, level = 0.0;

            for (int i = at (1.0); i < at (2.5); ++i)
            {
                diff += std::pow ((double) y[(size_t) i] - y[(size_t) (i - lag)], 2.0);
                level += (double) y[(size_t) i] * y[(size_t) i];
            }

            return 10.0 * std::log10 (std::max (diff, 1.0e-30) / std::max (level, 1.0e-30));
        };

        /** The tremolo's gain dips: the quietest one-carrier-period window around each guess. */
        const auto dipNear = [&] (const std::vector<float>& y, int guess, int halfSpan)
        {
            int best = guess;
            double quietest = 1.0e30;

            for (int i = std::max (carrier, guess - halfSpan); i < std::min ((int) y.size() - carrier, guess + halfSpan); ++i)
            {
                double e = 0.0;

                for (int j = i - carrier / 2; j < i - carrier / 2 + carrier; ++j)
                    e += (double) y[(size_t) j] * y[(size_t) j];

                if (e < quietest)
                {
                    quietest = e;
                    best = i;
                }
            }

            return best;
        };

        struct SyncVoicing { const char* label; int type; const char* voicingId; int voicing; };
        static constexpr SyncVoicing voicings[] { { "chorus CE", 0, params::modChorusMode, 2 },
                                                  { "phaser Phase 90 block", 1, params::modPhaserModel, 1 },
                                                  { "tremolo Smooth", 2, params::modTremVoice, 1 } };

        for (const auto& v : voicings)
        {
            for (const bool playing : { true, false })
            {
                tubamp::TubampAudioProcessor fresh;
                setFloatParam (fresh, params::modType, (float) v.type);
                setFloatParam (fresh, v.voicingId, (float) v.voicing);
                setFloatParam (fresh, params::modRate, 0.3f);   // must be ignored while synced
                setFloatParam (fresh, params::modDepth, 0.8f);
                setFloatParam (fresh, params::modMix, 1.0f);
                setFloatParam (fresh, params::modWidth, 0.0f);
                setBoolParam (fresh, params::modColor, true);

                TransportPlayHead playHead;
                playHead.playing = playing;
                playHead.ppq = 7.3;   // starts 0.6 of a cycle in
                const auto run = syncRun (fresh, &playHead, eighth, {});

                std::snprintf (what, sizeof (what), "%s, sync 1/8 at 120 BPM, host %s: the processor equals the engine alone at "
                               "4 Hz with %s", v.label, playing ? "playing" : "stopped",
                               playing ? "the song position's phase" : "a free-running LFO");
                ok &= expectIdentical (run.r.processor, run.r.engine, what);

                // Control: 0.1 % faster, the 250 ms repeat no longer holds.
                TransportPlayHead offHead;
                offHead.playing = playing;
                offHead.ppq = 7.3;
                offHead.bpm = 120.12;
                const auto off = syncRun (fresh, &offHead, eighth, {});

                const auto left = lockstepChannel (run.r.processor, blockSize, 0);
                const double residual = repeatResidualDb (left, p250);
                const double offResidual = repeatResidualDb (lockstepChannel (off.r.processor, blockSize, 0), p250);
                std::snprintf (what, sizeof (what), "...the output repeats every 250 ms: residual %.1f dB (bound -60; %.1f dB at "
                               "120.12 BPM, 0.1 %% off, control)", residual, offResidual);
                ok &= expect (residual <= -60.0 && offResidual > -60.0, what);

                if (v.type == 2)
                {
                    double worst = 0.0;
                    std::vector<int> dips;

                    if (playing)
                    {
                        for (const auto& line : run.grid)
                            if (line.sample >= at (0.5) && line.sample < at (2.8))
                            {
                                dips.push_back (dipNear (left, line.sample, at (0.1)));
                                worst = std::max (worst, std::abs (dips.back() - line.sample) / sampleRate);
                            }
                    }
                    else
                    {
                        for (int guess = at (0.5); guess < at (2.8); guess += p250)
                            dips.push_back (dipNear (left, guess, at (0.1)));
                    }

                    const double period = dips.size() > 1 ? (dips.back() - dips.front()) / sampleRate / (double) (dips.size() - 1) : 0.0;
                    std::snprintf (what, sizeof (what), "...%zu tremolo dips %.4f ms apart (250 +- 0.1 %%)%s", dips.size(), 1e3 * period,
                                   playing ? juce::String::formatted (", each within %.2f ms of its eighth note (bound 2)", 1e3 * worst).toRawUTF8() : "");
                    ok &= expect (dips.size() >= 8 && std::abs (period - 0.25) <= 0.25e-3 && worst <= 0.002, what);
                }
            }
        }

        // A tempo change is followed: 120 -> 150 BPM at 1 s, the 1/8 note becomes 200 ms.
        for (const bool playing : { true, false })
        {
            tubamp::TubampAudioProcessor fresh;
            setFloatParam (fresh, params::modType, 2.0f);
            setFloatParam (fresh, params::modTremVoice, 1.0f);
            setFloatParam (fresh, params::modDepth, 0.8f);
            setFloatParam (fresh, params::modWidth, 0.0f);

            TransportPlayHead playHead;
            playHead.playing = playing;
            playHead.ppq = 3.1;
            const auto run = syncRun (fresh, &playHead, eighth, { { 1.0, 150.0 } });

            std::snprintf (what, sizeof (what), "tremolo Smooth, sync 1/8, host %s, 120 -> 150 BPM at 1 s: the processor equals the "
                           "engine alone at 4 then 5 Hz", playing ? "playing" : "stopped");
            ok &= expectIdentical (run.r.processor, run.r.engine, what);

            const auto left = lockstepChannel (run.r.processor, blockSize, 0);
            std::vector<int> dips;
            double worst = 0.0;

            if (playing)
            {
                for (const auto& line : run.grid)
                    if (line.bpm == 150.0 && line.sample >= at (1.3) && line.sample < at (2.8))
                    {
                        dips.push_back (dipNear (left, line.sample, at (0.08)));
                        worst = std::max (worst, std::abs (dips.back() - line.sample) / sampleRate);
                    }
            }
            else
            {
                const int first = dipNear (left, at (1.4), p200 / 2);

                for (int guess = first; guess < at (2.8); guess += p200)
                    dips.push_back (dipNear (left, guess, at (0.08)));
            }

            const double period = dips.size() > 1 ? (dips.back() - dips.front()) / sampleRate / (double) (dips.size() - 1) : 0.0;
            std::snprintf (what, sizeof (what), "...after the change %zu dips %.4f ms apart (200 +- 0.1 %%)%s", dips.size(), 1e3 * period,
                           playing ? juce::String::formatted (", each within %.2f ms of the new grid (bound 2)", 1e3 * worst).toRawUTF8() : "");
            ok &= expect (dips.size() >= 6 && std::abs (period - 0.2) <= 0.2e-3 && worst <= 0.002, what);
        }

        // No play head: 120 BPM, free-running, like the delay's sync.
        {
            tubamp::TubampAudioProcessor fresh;
            setFloatParam (fresh, params::modType, 1.0f);
            setFloatParam (fresh, params::modPhaserModel, 2.0f);
            const auto run = syncRun (fresh, nullptr, eighth, {});
            ok &= expectIdentical (run.r.processor, run.r.engine,
                                   "phaser Small Stone, sync 1/8 with no play head: 120 BPM, 4 Hz free-running, as the engine alone");
        }

        // A note faster than the engines' 20 Hz cannot follow the song: 1/32 at 200 BPM is
        // 26.7 Hz, which runs free at 20 Hz.
        {
            tubamp::TubampAudioProcessor fresh;
            setFloatParam (fresh, params::modType, 2.0f);
            setFloatParam (fresh, params::modTremVoice, 1.0f);
            setFloatParam (fresh, params::modSync, 1.0f);
            fresh.setChainOrder ({ BlockId::mod }, {});
            fresh.prepareToPlay (sampleRate, blockSize);
            TransportPlayHead playHead;
            playHead.bpm = 200.0;
            fresh.setPlayHead (&playHead);

            ModEnginesAlone alone (sampleRate, blockSize);
            int lastStart = 0;
            const auto r = renderLockstep (fresh, input, sampleRate, blockSize,
                                           [&] (int start) { playHead.advance (start - lastStart, sampleRate); lastStart = start; },
                                           [&] (juce::dsp::AudioBlock<float> block) { alone.run (fresh, 0, block, 20.0f, -1.0); });
            fresh.setPlayHead (nullptr);
            ok &= expectIdentical (r.processor, r.engine, "tremolo Smooth, sync 1/32 at 200 BPM (26.7 Hz): 20 Hz free-running, "
                                                          "as the engine alone");
        }

        // Classic ignores sync and the song position: mod_rate, free-running, as before.
        {
            tubamp::TubampAudioProcessor fresh;
            setFloatParam (fresh, params::modType, 2.0f);
            setFloatParam (fresh, params::modTremVoice, 0.0f);
            setFloatParam (fresh, params::modRate, 2.0f);
            setFloatParam (fresh, params::modSync, (float) eighth);
            fresh.setChainOrder ({ BlockId::mod }, {});
            fresh.prepareToPlay (sampleRate, blockSize);
            TransportPlayHead playHead;
            playHead.ppq = 1.7;
            fresh.setPlayHead (&playHead);

            ModEnginesAlone alone (sampleRate, blockSize);
            int lastStart = 0;
            const auto r = renderLockstep (fresh, input, sampleRate, blockSize,
                                           [&] (int start) { playHead.advance (start - lastStart, sampleRate); lastStart = start; },
                                           [&] (juce::dsp::AudioBlock<float> block) { alone.runFree (fresh, 0, block); });
            fresh.setPlayHead (nullptr);
            ok &= expectIdentical (r.processor, r.engine, "tremolo Classic with sync 1/8 and the host playing: mod_rate, "
                                                          "free-running, as the engine alone");
        }
    }

    // --- instances 2 and 3 run their own engines from their own ids.
    {
        tubamp::TubampAudioProcessor fresh;
        setFloatParam (fresh, params::mod2Type, 2.0f);
        setFloatParam (fresh, params::mod2TremVoice, 3.0f);
        setFloatParam (fresh, params::mod2Depth, 0.75f);
        setFloatParam (fresh, params::mod2Shape, 0.4f);
        setFloatParam (fresh, params::mod2Width, 0.5f);
        setFloatParam (fresh, params::mod2Sync, 6.0f);
        setFloatParam (fresh, params::mod3Type, 0.0f);
        setFloatParam (fresh, params::mod3ChorusMode, 3.0f);
        setFloatParam (fresh, params::mod3Rate, 0.25f);
        setFloatParam (fresh, params::mod3Depth, 0.8f);
        setFloatParam (fresh, params::mod3Mix, 0.5f);
        setFloatParam (fresh, params::mod3Tone, 0.4f);

        const auto input = chordSignal (sampleRate, 1.0);
        const auto instanceRun = [&] (int k)
        {
            fresh.setChainOrder ({ chain::instanceId (BlockId::mod, k) }, {});
            fresh.prepareToPlay (sampleRate, blockSize);
            ModEnginesAlone alone (sampleRate, blockSize);
            return renderLockstep (fresh, input, sampleRate, blockSize, {}, [&] (juce::dsp::AudioBlock<float> block)
            {
                if (k == 1)
                    alone.run (fresh, 1, block, 4.0f, -1.0);   // 1/8 at the fallback 120 BPM
                else
                    alone.runFree (fresh, k, block);
            });
        };

        const auto mod2 = instanceRun (1), mod3 = instanceRun (2);
        ok &= expectIdentical (mod2.processor, mod2.engine, "mod2 (tremolo Bias, sync 1/8) runs from the mod2_* ids, as the engine alone");
        ok &= expectIdentical (mod3.processor, mod3.engine, "mod3 (chorus Dimension) runs from the mod3_* ids, as the engine alone");

        for (const auto* id : { params::mod3Type, params::mod3ChorusMode, params::mod3PhaserModel, params::mod3TremVoice,
                                params::mod3Rate, params::mod3Depth, params::mod3Mix, params::mod3Sync, params::mod3Width,
                                params::mod3Tone, params::mod3Shape, params::mod3Wave })
            setFloatParam (fresh, id, 1.0f);

        setBoolParam (fresh, params::mod3Color, false);

        ok &= expectIdentical (mod2.processor, instanceRun (1).processor, "mod2 renders the same whatever mod3's ids are");
        ok &= expect (firstDifference (mod3.processor, instanceRun (2).processor) >= 0
                          && firstDifference (mod2.processor, mod3.processor) >= 0,
                      "...while mod3's own render follows them (control)");
    }

    // --- switches on a sustained chord: inside an engine (its own 40 ms fade) and of mod_type
    // (the dispatcher's crossfade) neither steps the level by more than P7/T16's 3 dB.
    {
        int n = 0;
        const auto chord = p7Chord (sampleRate, 2.0, n);
        const int switchAt = at (1.0);

        struct Switch { const char* label; std::vector<std::pair<const char*, float>> before, after; };
        const std::vector<Switch> switches {
            { "Phase 90 script -> block", { { params::modType, 1 }, { params::modPhaserModel, 1 }, { params::modColor, 0 } },
              { { params::modColor, 1 } } },
            { "Phase 90 block -> Small Stone on", { { params::modType, 1 }, { params::modPhaserModel, 1 } },
              { { params::modPhaserModel, 2 } } },
            { "chorus Wide -> CE", { { params::modType, 0 }, { params::modChorusMode, 1 } }, { { params::modChorusMode, 2 } } },
            { "tremolo Smooth -> Optical", { { params::modType, 2 }, { params::modTremVoice, 1 }, { params::modRate, 4 },
                                             { params::modDepth, 0.5f }, { params::modWidth, 0 } },
              { { params::modTremVoice, 2 } } },
            // Classic chorus is left out: on this chord it steps 3.4 dB with no switch at all.
            { "mod_type Classic phaser -> Classic tremolo", { { params::modType, 1 }, { params::modPhaserModel, 0 },
                                                             { params::modTremVoice, 0 }, { params::modDepth, 0.5f },
                                                             { params::modMix, 0.5f } },
              { { params::modType, 2 } } },
            { "mod_type chorus Wide -> phaser Phase 90 block", { { params::modType, 0 }, { params::modChorusMode, 1 } },
              { { params::modType, 1 } } },
            { "mod_type phaser Phase 90 block -> tremolo Smooth", { { params::modType, 1 }, { params::modDepth, 0.5f },
                                                                   { params::modWidth, 0 } },
              { { params::modType, 2 } } },
            { "mod_type tremolo Smooth -> chorus CE", { { params::modType, 2 }, { params::modChorusMode, 2 },
                                                       { params::modDepth, 0.5f }, { params::modWidth, 0 } },
              { { params::modType, 0 } } } };

        for (const auto& s : switches)
        {
            const auto render = [&] (bool doSwitch)
            {
                tubamp::TubampAudioProcessor fresh;

                for (const auto& [id, value] : s.before)
                    setFloatParam (fresh, id, value);

                fresh.setChainOrder ({ BlockId::mod }, {});
                fresh.prepareToPlay (sampleRate, blockSize);
                bool switched = false;
                return renderSignal (fresh, chord, blockSize, [&] (int start)
                {
                    if (doSwitch && ! switched && start >= switchAt)
                    {
                        for (const auto& [id, value] : s.after)
                            setFloatParam (fresh, id, value);

                        switched = true;
                    }
                });
            };

            const auto switched = render (true), steady = render (false);
            const int from = switchAt - at (0.05), to = switchAt + at (0.15);
            const double step = largestLevelStepDb (switched, from, to, n), base = largestLevelStepDb (steady, from, to, n);
            std::snprintf (what, sizeof (what), "%s on the chord: largest 1 ms level step %.2f dB (bound 3; unswitched %.2f)",
                           s.label, step, base);
            ok &= expect (step <= 3.0 && firstDifference (switched, steady) >= 0, what);
        }
    }

    // --- the mod_type crossfade itself (FX-ENGINES.md D-5): the incoming engine is reset in
    // the block the change arrives in and runs from there, as the old block did, unheard for its
    // settle time while the outgoing one is heard alone, then a 40 ms PowerCrossfade hands over.
    {
        const int fade = tubamp::Modulation::crossfadeSamples (sampleRate);
        ok &= expect (fade == at (0.04), "a mod_type crossfade lasts 40 ms");

        const auto input = chordSignal (sampleRate, 1.0);
        struct TypeSwitch { const char* label; int fromType, toType; int chorusMode, tremVoice; };
        static constexpr TypeSwitch typeSwitches[] { { "Classic chorus -> Classic tremolo", 0, 2, 0, 0 },
                                                     { "chorus Wide -> tremolo Smooth", 0, 2, 1, 1 },
                                                     { "tremolo Optical -> chorus Tri", 2, 0, 4, 2 } };

        for (const auto& s : typeSwitches)
        {
            tubamp::TubampAudioProcessor fresh;
            setFloatParam (fresh, params::modType, (float) s.fromType);
            setFloatParam (fresh, params::modChorusMode, (float) s.chorusMode);
            setFloatParam (fresh, params::modTremVoice, (float) s.tremVoice);
            setFloatParam (fresh, params::modRate, 3.0f);
            setFloatParam (fresh, params::modDepth, 0.7f);
            fresh.setChainOrder ({ BlockId::mod }, {});
            fresh.prepareToPlay (sampleRate, blockSize);

            const int requestAt = at (0.4);
            int switchBlock = -1, blockStart = 0, hold = 0;
            bool fading = false;
            ModEnginesAlone alone (sampleRate, blockSize);
            juce::AudioBuffer<float> outgoing (2, blockSize);
            tubamp::PowerCrossfade crossfade;
            crossfade.prepare (sampleRate);

            const auto r = renderLockstep (fresh, input, sampleRate, blockSize, [&] (int start)
            {
                blockStart = start;

                if (switchBlock < 0 && start >= requestAt)
                {
                    setFloatParam (fresh, params::modType, (float) s.toType);
                    switchBlock = start;

                    tubamp::ModParams incoming;
                    incoming.type = s.toType;
                    incoming.chorusMode = s.chorusMode;
                    incoming.depth = rawValue (fresh, params::modDepth);
                    hold = tubamp::Modulation::crossfadeHoldSamples (incoming, sampleRate);
                }
            },
            [&] (juce::dsp::AudioBlock<float> block)
            {
                const int n = (int) block.getNumSamples();

                if (blockStart == switchBlock)
                {
                    alone.reset (s.toType);
                    crossfade.start (hold, fade, tubamp::Modulation::crossfadeWaitSamples (sampleRate));
                    fading = true;
                }

                if (! fading)
                {
                    alone.runFree (fresh, 0, block);
                    return;
                }

                juce::dsp::AudioBlock<float> old (outgoing.getArrayOfWritePointers(), 2, (size_t) n);
                old.copyFrom (block);
                alone.runFree (fresh, 0, old, s.fromType);
                alone.runFree (fresh, 0, block, s.toType);

                float* heard[2] { old.getChannelPointer (0), old.getChannelPointer (1) };
                const float* coming[2] { block.getChannelPointer (0), block.getChannelPointer (1) };

                for (int i = 0; i < n && fading; ++i)
                {
                    fading = ! crossfade.mix (heard, coming, 2, i);

                    for (size_t ch = 0; ch < 2; ++ch)
                        block.getChannelPointer (ch)[i] = old.getChannelPointer (ch)[i];
                }
            });

            std::snprintf (what, sizeof (what), "mod_type %s through the processor equals the engines alone with the incoming one "
                           "reset at the switch, the outgoing one heard alone for %d samples (or until the two stop cancelling), "
                           "then a %d-sample power-matched crossfade", s.label, hold, fade);
            ok &= expectIdentical (r.processor, r.engine, what);
        }
    }

    return ok;
}

/** Three mod blocks cycling through types, voicings, colours, sync and the transport,
    with bypass and re-entry, allocate nothing on the audio thread. */
bool runModRtTests (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    std::printf ("mod real-time safety:\n");

    namespace chain = tubamp::chain;
    bool ok = true;

    const chain::Order full { BlockId::mod, BlockId::mod2, BlockId::mod3 };
    proc.setChainOrder (full, {});
    TransportPlayHead playHead;
    proc.setPlayHead (&playHead);
    proc.prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    long allocations = 0;
    int armedBlocks = 0;

    for (int block = 0; block < 600; ++block)
    {
        if (block % 7 == 0 && block > 0)
        {
            const int step = block / 7;
            const int k = step % params::maxInstances;
            setFloatParam (proc, params::modTypeIds[k], (float) (step % tubamp::Modulation::numTypes));
            setFloatParam (proc, params::modChorusModeIds[k], (float) ((step / 3) % tubamp::ChorusFx::numModes));
            setFloatParam (proc, params::modPhaserModelIds[k], (float) ((step / 2) % tubamp::PhaserFx::numModels));
            setFloatParam (proc, params::modTremVoiceIds[k], (float) ((step / 3) % tubamp::TremoloFx::numVoices));
            setBoolParam (proc, params::modColorIds[k], step % 2 == 0);
            setFloatParam (proc, params::modSyncIds[k], (float) ((step * 5) % tubamp::kNumModSyncChoices));
            setFloatParam (proc, params::modRateIds[k], step % 2 == 0 ? 0.4f : 6.0f);
            setFloatParam (proc, params::modWidthIds[k], step % 3 == 0 ? 0.0f : 1.0f);
            setFloatParam (proc, params::modWaveIds[k], (float) (step % 3) * 0.5f);
            setBoolParam (proc, params::modOnIds[(k + 1) % params::maxInstances], step % 5 != 0);
            playHead.playing = step % 4 != 1;
            playHead.bpm = step % 3 == 0 ? 90.0 : 140.0;

            if (step % 6 == 2)
                proc.setChainOrder ({ BlockId::mod3, BlockId::mod }, {});
            else if (step % 6 == 3)
                proc.setChainOrder (full, {});
        }

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < blockSize; ++i)
                buffer.setSample (ch, i, 0.3f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0
                                                                  * (block * blockSize + i) / sampleRate));

        playHead.advance (blockSize, sampleRate);

        if (block < 20)
        {
            proc.processBlock (buffer, midi);
            continue;
        }

        const long before = allocCount.load();

        {
            const AllocationGuard guard;
            proc.processBlock (buffer, midi);
        }

        allocations += allocCount.load() - before;
        ++armedBlocks;
    }

    char what[220];
    std::snprintf (what, sizeof (what), "%d processBlock calls with mod x3 cycling types, voicings, colours, sync and the "
                   "transport: %ld allocations or frees", armedBlocks, allocations);
    ok &= expect (allocations == 0, what);

    proc.setPlayHead (nullptr);

    for (int k = 0; k < params::maxInstances; ++k)
        resetParams (proc, { params::modTypeIds[k], params::modChorusModeIds[k], params::modPhaserModelIds[k],
                             params::modTremVoiceIds[k], params::modColorIds[k], params::modSyncIds[k], params::modRateIds[k],
                             params::modWidthIds[k], params::modWaveIds[k], params::modOnIds[k] });

    proc.setChainOrder (chain::defaultOrder(), {});
    proc.prepareToPlay (sampleRate, blockSize);

    return ok;
}
//==============================================================================
// Processor-level robustness: non-finite input, block toggles, mod switches, lane latency
// changes, reported against applied latency, fresh installs and per-type starting points.

/** Both channels of a render, the host feeding `left` and `right`. */
struct StereoRender
{
    std::vector<float> l, r;
};

StereoRender renderStereo (tubamp::TubampAudioProcessor& proc, const std::vector<float>& left,
                           const std::vector<float>& right, int blockSize,
                           const std::function<void (int)>& beforeBlock = {})
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    StereoRender out;
    out.l.reserve (left.size());
    out.r.reserve (left.size());

    for (int start = 0; start < (int) left.size(); start += blockSize)
    {
        const int n = std::min (blockSize, (int) left.size() - start);

        if (beforeBlock)
            beforeBlock (start);

        float* channels[2] { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        juce::AudioBuffer<float> view (channels, 2, n);
        std::copy (left.begin() + start, left.begin() + start + n, channels[0]);
        std::copy (right.begin() + start, right.begin() + start + n, channels[1]);
        proc.processBlock (view, midi);
        out.l.insert (out.l.end(), channels[0], channels[0] + n);
        out.r.insert (out.r.end(), channels[1], channels[1] + n);
    }

    return out;
}

/** What is left above `cutoffHz` (8th-order Butterworth high-pass, clickDb's sections). */
std::vector<double> highBand (const std::vector<float>& x, double sampleRate, double cutoffHz)
{
    static constexpr double qs[] { 0.50979558, 0.60134489, 0.89997622, 2.56291545 };
    std::vector<double> y (x.begin(), x.end());
    const double w = std::tan (juce::MathConstants<double>::pi * cutoffHz / sampleRate);

    for (const double q : qs)
    {
        const double norm = 1.0 / (1.0 + w / q + w * w);
        const double a1 = 2.0 * (w * w - 1.0) * norm, a2 = (1.0 - w / q + w * w) * norm;
        double z1 = 0.0, z2 = 0.0;

        for (auto& v : y)
        {
            const double out = norm * v + z1;
            z1 = -2.0 * norm * v - a1 * out + z2;
            z2 = norm * v - a2 * out;
            v = out;
        }
    }

    return y;
}

double peakIn (const std::vector<double>& x, size_t from, size_t to)
{
    double peak = 0.0;

    for (size_t i = from; i < std::min (to, x.size()); ++i)
        peak = std::max (peak, std::abs (x[i]));

    return peak;
}

/** The pop metric: how far the high band (above 2 kHz) of a switched render peaks in
    [from, to) over the steady response, the higher of the two steady renders' (the block
    left on, the block left off) peaks within `margin` samples of that span, dBFS; -240 when
    it does not rise above it at all. The margin lets an engine that restarts from its reset
    sit anywhere inside the range its steady render moves through. */
double popExcessDb (const std::vector<double>& switched, const std::vector<double>& on,
                    const std::vector<double>& off, size_t from, size_t to, size_t margin)
{
    const size_t steadyFrom = from > margin ? from - margin : 0, steadyTo = to + margin;
    const double excess = peakIn (switched, from, to) - std::max (peakIn (on, steadyFrom, steadyTo),
                                                                  peakIn (off, steadyFrom, steadyTo));
    return excess > 0.0 ? toDb (excess) : -240.0;
}

/** A sustained E power chord (82.4, 123.5, 164.8 Hz, six harmonics each), -10 dBFS peak:
    everything below 1 kHz, so a step shows above 2 kHz. */
std::vector<float> powerChord (double sampleRate, double seconds)
{
    std::vector<float> x ((size_t) std::lround (seconds * sampleRate));
    static constexpr double roots[] { 82.41, 123.47, 164.81 };

    for (size_t i = 0; i < x.size(); ++i)
    {
        double v = 0.0;

        for (const double f : roots)
            for (int h = 1; h <= 6; ++h)
                v += std::sin (2.0 * juce::MathConstants<double>::pi * f * h * (double) i / sampleRate + 0.7 * h) / h;

        x[i] = (float) (0.047 * v);
    }

    return x;
}

/** One block type to exercise: its chain, its on id and the settings that select it. */
struct BlockCase
{
    std::string label;
    tubamp::chain::Order order;
    const char* onId;
    std::vector<std::pair<const char*, float>> settings;
    bool classic = false;
};

/** Every type of the five engine blocks on instance 1, plus one non-Classic type on
    instances 2 and 3, each alone in the chain and set away from neutral. */
std::vector<BlockCase> engineBlockCases()
{
    std::vector<BlockCase> cases;

    for (int m = 0; m < tubamp::GateFx::numModes; ++m)
        cases.push_back ({ std::string ("gate ") + params::gateModeNames[m], { BlockId::gate }, params::gateOn,
                           { { params::gateMode, (float) m }, { params::gateThreshold, -60.0f } }, m == 0 });

    // Deferred onto the amp's mono output (no model: the amp passes the audio through).
    cases.push_back ({ "gate Smooth, deferred", { BlockId::gate, BlockId::amp }, params::gateOn,
                       { { params::gateMode, (float) tubamp::GateFx::Mode::smooth }, { params::gateThreshold, -60.0f },
                         { params::ampEqOn, 0.0f } } });

    for (int k = 0; k < params::maxInstances; ++k)
        for (int t = 0; t < tubamp::CompFx::numTypes; ++t)
            if (k == 0 || t == 3)
                cases.push_back ({ "comp" + std::to_string (k + 1) + " " + params::compTypeNames[t],
                                   { tubamp::chain::instanceId (BlockId::comp, k) }, params::compOnIds[k],
                                   { { params::compTypeIds[k], (float) t }, { params::compThresholdIds[k], -30.0f },
                                     { params::compScHpfIds[k], 100.0f } }, t == 0 });

    for (int k = 0; k < params::maxInstances; ++k)
        for (int t = 0; t < tubamp::DriveFx::numTypes; ++t)
            if (k == 0 || t == 2 - k + 3)
                cases.push_back ({ "drive" + std::to_string (k + 1) + " " + params::driveTypeNames[t],
                                   { tubamp::chain::instanceId (BlockId::drive, k) }, params::driveOnIds[k],
                                   { { params::driveTypeIds[k], (float) t }, { params::driveGainIds[k], 18.0f } }, t == 0 });

    for (int k = 0; k < params::maxInstances; ++k)
        for (int t = 0; t < tubamp::EqFx::numTypes; ++t)
            if (k == 0 || t == k)
                cases.push_back ({ "eq" + std::to_string (k + 1) + " " + params::eqTypeNames[t],
                                   { tubamp::chain::instanceId (BlockId::eq, k) }, params::eqOnIds[k],
                                   { { params::eqTypeIds[k], (float) t }, { params::eqBassIds[k], 8.0f },
                                     { params::eqTrebleIds[k], 2.0f }, { params::eqLevelIds[k], 4.0f },
                                     { params::eqLowCutIds[k], 120.0f }, { params::eqHighCutIds[k], 5000.0f },
                                     { params::eqGraphicIds[2][k], 9.0f }, { params::eqGraphicIds[5][k], -9.0f } }, t == 0 });

    struct Voicing { int type; const char* const* ids; const char* const* names; int count; };
    const Voicing voicings[] { { 0, params::modChorusModeIds, params::modChorusModeNames, tubamp::ChorusFx::numModes },
                               { 1, params::modPhaserModelIds, params::modPhaserModelNames, tubamp::PhaserFx::numModels },
                               { 2, params::modTremVoiceIds, params::modTremVoiceNames, tubamp::TremoloFx::numVoices } };

    for (int k = 0; k < params::maxInstances; ++k)
        for (const auto& v : voicings)
            for (int i = 0; i < v.count; ++i)
                if (k == 0 || (i == 1 && v.type == k))
                    cases.push_back ({ "mod" + std::to_string (k + 1) + " " + v.names[i],
                                       { tubamp::chain::instanceId (BlockId::mod, k) }, params::modOnIds[k],
                                       { { params::modTypeIds[k], (float) v.type }, { v.ids[k], (float) i },
                                         { params::modDepthIds[k], 0.6f }, { params::modMixIds[k], 0.5f } }, i == 0 });

    return cases;
}

/** One non-finite sample (or a run) at the plugin input must not reach the output, on any
    channel or path: the block sees silence there instead, so the render equals the same input
    with those samples zeroed, bit for bit and from the first sample. */
bool runNonFiniteInputTests (double sampleRate, int blockSize)
{
    std::printf ("non-finite input:\n");
    bool ok = true;
    char what[300];

    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const auto chord = powerChord (sampleRate, 0.45);
    auto badL = chord, badR = chord, zeroL = chord, zeroR = chord;
    const size_t b = (size_t) std::lround (0.15 * sampleRate);

    const auto poison = [] (std::vector<float>& bad, std::vector<float>& zero, size_t from, size_t count, float value)
    {
        for (size_t i = from; i < from + count; ++i)
        {
            bad[i] = value;
            zero[i] = 0.0f;
        }
    };

    poison (badL, zeroL, b, 4, nan);
    poison (badL, zeroL, b + 20, 2, inf);
    poison (badL, zeroL, b + 40, 1, -inf);
    poison (badL, zeroL, b + 1000, 64, nan);
    poison (badR, zeroR, b + 10, 3, -inf);
    poison (badR, zeroR, b + 30, 1, nan);

    std::vector<BlockCase> cases { { "empty chain", {}, nullptr, {} } };

    for (auto& c : engineBlockCases())
        cases.push_back (std::move (c));

    for (int t = 0; t < tubamp::DelayFx::numTypes; ++t)
        cases.push_back ({ "delay type " + std::to_string (t), { BlockId::delay }, params::delayOn,
                           { { params::delayType, (float) t } } });

    for (int t = 0; t < tubamp::ReverbFx::numTypes; ++t)
        cases.push_back ({ "reverb type " + std::to_string (t), { BlockId::reverb }, params::reverbOn,
                           { { params::reverbType, (float) t } } });

    cases.push_back ({ "amp (no model) and cab", { BlockId::amp, BlockId::cab }, nullptr, {} });
    cases.push_back ({ "classic chain, Classic types", tubamp::chain::classicOrder(), nullptr,
                       { { params::gateMode, 0 }, { params::compType, 0 }, { params::driveType, 0 }, { params::eqType, 0 },
                         { params::modChorusMode, 0 }, { params::driveOn, 1 }, { params::delayOn, 1 }, { params::reverbOn, 1 },
                         { params::modOn, 1 } } });
    cases.push_back ({ "classic chain, new types", tubamp::chain::classicOrder(), nullptr,
                       { { params::driveOn, 1 }, { params::delayOn, 1 }, { params::reverbOn, 1 }, { params::modOn, 1 } } });
    cases.push_back ({ "split lanes", { BlockId::split, BlockId::drive, BlockId::eq, BlockId::lane2, BlockId::comp,
                                        BlockId::mod, BlockId::mix, BlockId::reverb },
                       nullptr, { { params::driveOn, 1 }, { params::modOn, 1 }, { params::reverbOn, 1 } } });
    cases.push_back ({ "split lanes, L/R", { BlockId::split, BlockId::gate, BlockId::lane2, BlockId::eq2, BlockId::mix },
                       nullptr, { { params::splitMode, 1 }, { params::eq2Type, 2 } } });

    int failing = 0;

    for (const auto& c : cases)
    {
        const auto render = [&] (const std::vector<float>& l, const std::vector<float>& r)
        {
            tubamp::TubampAudioProcessor fresh;

            for (const auto& [id, value] : c.settings)
                setFloatParam (fresh, id, value);

            if (c.onId != nullptr)
                setBoolParam (fresh, c.onId, true);

            fresh.setChainOrder (c.order, {});
            fresh.prepareToPlay (sampleRate, blockSize);
            return renderStereo (fresh, l, r, blockSize);
        };

        const auto bad = render (badL, badR), clean = render (zeroL, zeroR);
        long nonFinite = 0;

        for (size_t i = 0; i < bad.l.size(); ++i)
            nonFinite += (std::isfinite (bad.l[i]) ? 0 : 1) + (std::isfinite (bad.r[i]) ? 0 : 1);

        const bool same = firstDifference (bad.l, clean.l) < 0 && firstDifference (bad.r, clean.r) < 0;
        std::snprintf (what, sizeof (what), "%s: NaN/Inf bursts at the input: %ld non-finite output samples, %s the input "
                       "with them zeroed", c.label.c_str(), nonFinite, same ? "identical to" : "DIFFERENT from");
        const bool pass = nonFinite == 0 && same;
        failing += pass ? 0 : 1;
        ok &= expect (pass, what);
    }

    // A mono host: the one channel is sanitised too.
    {
        tubamp::TubampAudioProcessor fresh;
        setBoolParam (fresh, params::driveOn, true);
        setBoolParam (fresh, params::reverbOn, true);
        fresh.setChainOrder (tubamp::chain::classicOrder(), {});
        std::vector<float> monoBad, monoClean;

        withMonoHost (fresh, sampleRate, blockSize, [&]
        {
            const auto run = [&] (const std::vector<float>& in, std::vector<float>& out)
            {
                fresh.prepareToPlay (sampleRate, blockSize);
                juce::AudioBuffer<float> buffer (1, blockSize);
                juce::MidiBuffer midi;

                for (int start = 0; start < (int) in.size(); start += blockSize)
                {
                    const int n = std::min (blockSize, (int) in.size() - start);
                    float* channels[1] { buffer.getWritePointer (0) };
                    juce::AudioBuffer<float> view (channels, 1, n);
                    std::copy (in.begin() + start, in.begin() + start + n, channels[0]);
                    fresh.processBlock (view, midi);
                    out.insert (out.end(), channels[0], channels[0] + n);
                }
            };

            run (badL, monoBad);
            run (zeroL, monoClean);
        });

        const bool pass = ! monoBad.empty() && firstDifference (monoBad, monoClean) < 0;
        failing += pass ? 0 : 1;
        ok &= expect (pass, "mono host, classic chain: NaN/Inf at the input leave the render identical to the zeroed input");
    }

    std::printf ("  %d of %d non-finite cases fail\n", failing, (int) cases.size() + 1);
    return ok;
}

/** A gate, comp, drive, EQ or mod block of any type, switched off for a second and back on
    under a sustained chord, neither pops (high band at the edges no more than 60 dB below full
    scale over the steady on and off renders) nor, switched back on over silence, replays what
    it held when it went off (non-Classic: -60 dBFS; Classic, which kept its old hard switch and
    replayed up to +0.9 dBFS before it went through the same fader and reset: -100 dBFS). */
bool runBlockToggleTests (double sampleRate, int blockSize)
{
    std::printf ("block on/off:\n");
    bool ok = true;
    char what[300];

    const auto at = [sampleRate] (double seconds) { return (int) std::lround (seconds * sampleRate); };
    const auto chord = powerChord (sampleRate, 2.4);
    std::vector<float> stopping = powerChord (sampleRate, 2.2);
    std::fill (stopping.begin() + at (0.6), stopping.end(), 0.0f);

    const int offAt = at (0.8), onAt = at (1.8);
    int failing = 0, tested = 0;
    double worstPop = -240.0, worstStale = -240.0;

    for (const auto& c : engineBlockCases())
    {
        const auto render = [&] (const std::vector<float>& input, int offSample, int onSample, bool startOn)
        {
            tubamp::TubampAudioProcessor fresh;

            for (const auto& [id, value] : c.settings)
                setFloatParam (fresh, id, value);

            setBoolParam (fresh, c.onId, startOn);
            fresh.setChainOrder (c.order, {});
            fresh.prepareToPlay (sampleRate, blockSize);

            return renderSignal (fresh, input, blockSize, [&] (int start)
            {
                if (offSample >= 0 && start >= offSample && start < offSample + blockSize)
                    setBoolParam (fresh, c.onId, false);

                if (onSample >= 0 && start >= onSample && start < onSample + blockSize)
                    setBoolParam (fresh, c.onId, true);
            });
        };

        const int blockOff = (offAt / blockSize) * blockSize + (offAt % blockSize != 0 ? blockSize : 0);
        const int blockOn = (onAt / blockSize) * blockSize + (onAt % blockSize != 0 ? blockSize : 0);

        const auto on = highBand (render (chord, -1, -1, true), sampleRate, 2000.0);
        const auto off = highBand (render (chord, -1, -1, false), sampleRate, 2000.0);
        const auto toggled = highBand (render (chord, offAt, onAt, true), sampleRate, 2000.0);

        const auto margin = (size_t) at (0.2);
        const double popOff = popExcessDb (toggled, on, off, (size_t) (blockOff - at (0.001)), (size_t) (blockOff + at (0.06)), margin);
        const double popOn = popExcessDb (toggled, on, off, (size_t) (blockOn - at (0.001)), (size_t) (blockOn + at (0.06)), margin);

        const int staleOff = at (0.5), staleOn = at (1.5);
        const auto stale = render (stopping, staleOff, staleOn, true);
        float stalePeak = 0.0f;

        for (size_t i = (size_t) staleOn; i < stale.size(); ++i)
            stalePeak = std::max (stalePeak, std::abs (stale[i]));

        const double staleDb = toDb (stalePeak);

        if (c.classic)
        {
            std::snprintf (what, sizeof (what), "%s: off then on under a chord pops %.1f / %.1f dBFS over the steady high band "
                           "(bound -60); back on over silence %.1f dBFS (bound -100)", c.label.c_str(), popOff, popOn, staleDb);
            const bool pass = popOff <= -60.0 && popOn <= -60.0 && staleDb <= -100.0;
            failing += pass ? 0 : 1;
            ++tested;
            ok &= expect (pass, what);
            continue;
        }

        ++tested;
        worstPop = std::max ({ worstPop, popOff, popOn });
        worstStale = std::max (worstStale, staleDb);
        std::snprintf (what, sizeof (what), "%s: off then on under a chord pops %.1f / %.1f dBFS over the steady high band "
                       "(bound -60); back on over silence %.1f dBFS (bound -60)", c.label.c_str(), popOff, popOn, staleDb);
        const bool pass = popOff <= -60.0 && popOn <= -60.0 && staleDb <= -60.0;
        failing += pass ? 0 : 1;
        ok &= expect (pass, what);
    }

    std::printf ("  %d of %d block types fail; worst non-Classic pop %.1f dBFS, worst non-Classic replay %.1f dBFS\n",
                 failing, tested, worstPop, worstStale);

    // Not a switch: a reorder keeps every block's state (no reset, no fade), and a type change
    // while bypassed lands cleanly when the block comes back on.
    {
        tubamp::TubampAudioProcessor fresh;
        setBoolParam (fresh, params::driveOn, true);
        setFloatParam (fresh, params::driveType, (float) tubamp::DriveFx::Type::screamer);
        setFloatParam (fresh, params::eqType, (float) tubamp::EqFx::Type::parametric);
        setFloatParam (fresh, params::eqBass, 8.0f);
        fresh.setChainOrder ({ BlockId::drive, BlockId::eq }, {});
        fresh.prepareToPlay (sampleRate, blockSize);

        const auto moved = renderSignal (fresh, chord, blockSize, [&] (int start)
        {
            if (start >= at (0.5) && start < at (0.5) + blockSize)
                fresh.setChainOrder ({ BlockId::eq, BlockId::drive }, {});
        });

        tubamp::TubampAudioProcessor reference;
        setBoolParam (reference, params::driveOn, true);
        setFloatParam (reference, params::driveType, (float) tubamp::DriveFx::Type::screamer);
        setFloatParam (reference, params::eqType, (float) tubamp::EqFx::Type::parametric);
        setFloatParam (reference, params::eqBass, 8.0f);
        reference.setChainOrder ({ BlockId::drive, BlockId::eq }, {});
        reference.prepareToPlay (sampleRate, blockSize);
        const auto unmoved = renderSignal (reference, chord, blockSize);

        const long first = firstDifference (moved, unmoved);
        std::snprintf (what, sizeof (what), "a reorder that moves a block resets and fades nothing: identical up to the move "
                       "(first difference at %.4f s, move at 0.5 s)", (double) first / sampleRate);
        ok &= expect (first >= at (0.5) - blockSize, what);
    }

    {
        const auto render = [&] (bool changeWhileOff, bool backOn)
        {
            tubamp::TubampAudioProcessor fresh;
            setFloatParam (fresh, params::driveType, (float) tubamp::DriveFx::Type::distortion);
            setBoolParam (fresh, params::driveOn, true);
            fresh.setChainOrder ({ BlockId::drive }, {});
            fresh.prepareToPlay (sampleRate, blockSize);

            return renderSignal (fresh, chord, blockSize, [&] (int start)
            {
                if (start >= offAt && start < offAt + blockSize)
                    setBoolParam (fresh, params::driveOn, false);

                if (changeWhileOff && start >= at (1.2) && start < at (1.2) + blockSize)
                    setFloatParam (fresh, params::driveType, (float) tubamp::DriveFx::Type::fuzz);

                if (backOn && start >= onAt && start < onAt + blockSize)
                    setBoolParam (fresh, params::driveOn, true);
            });
        };

        tubamp::TubampAudioProcessor fuzzOn;
        setFloatParam (fuzzOn, params::driveType, (float) tubamp::DriveFx::Type::fuzz);
        setBoolParam (fuzzOn, params::driveOn, true);
        fuzzOn.setChainOrder ({ BlockId::drive }, {});
        fuzzOn.prepareToPlay (sampleRate, blockSize);
        const auto onBand = highBand (renderSignal (fuzzOn, chord, blockSize), sampleRate, 2000.0);
        const auto offBand = highBand (render (false, false), sampleRate, 2000.0);
        const auto changed = highBand (render (true, true), sampleRate, 2000.0);
        const int blockOn = (onAt / blockSize) * blockSize + (onAt % blockSize != 0 ? blockSize : 0);
        const double pop = popExcessDb (changed, onBand, offBand, (size_t) (blockOn - at (0.001)), (size_t) (blockOn + at (0.06)),
                                        (size_t) at (0.2));
        std::snprintf (what, sizeof (what), "Distortion -> Fuzz while bypassed, then on: pops %.1f dBFS (bound -60)", pop);
        ok &= expect (pop <= -60.0, what);
    }

    return ok;
}

/** mod_type switches: the incoming engine starts from its reset (the chorus fading its own
    wet in), so no ordered pair of voicings ticks; voicing switches are each engine's own
    business (printed); tempo sync hands the engines every song-position jump and free-runs
    when the host has no usable tempo or is stopped. */
bool runModSwitchTests (double sampleRate, int blockSize)
{
    std::printf ("mod switches and sync:\n");
    bool ok = true;
    char what[300];

    const auto at = [sampleRate] (double seconds) { return (int) std::lround (seconds * sampleRate); };
    const auto tone = sineSignal (sampleRate, 220.0, juce::Decibels::decibelsToGain (-12.0f), at (0.7));
    const int switchAt = at (0.35);
    const int switchBlock = (switchAt / blockSize) * blockSize + (switchAt % blockSize != 0 ? blockSize : 0);

    struct Voicing { int type, index; const char* name; };
    std::vector<Voicing> voicings;

    for (int i = 0; i < tubamp::ChorusFx::numModes; ++i)    voicings.push_back ({ 0, i, params::modChorusModeNames[i] });
    for (int i = 0; i < tubamp::PhaserFx::numModels; ++i)   voicings.push_back ({ 1, i, params::modPhaserModelNames[i] });
    for (int i = 0; i < tubamp::TremoloFx::numVoices; ++i)  voicings.push_back ({ 2, i, params::modTremVoiceNames[i] });

    static constexpr const char* typeNames[] { "chorus", "phaser", "tremolo" };
    const char* const voicingIds[] { params::modChorusMode, params::modPhaserModel, params::modTremVoice };

    const auto render = [&] (const Voicing& a, const Voicing& b, bool doSwitch)
    {
        tubamp::TubampAudioProcessor fresh;
        setFloatParam (fresh, voicingIds[a.type], (float) a.index);
        setFloatParam (fresh, voicingIds[b.type], (float) b.index);
        setFloatParam (fresh, params::modType, (float) a.type);
        fresh.setChainOrder ({ BlockId::mod }, {});
        fresh.prepareToPlay (sampleRate, blockSize);

        return renderSignal (fresh, tone, blockSize, [&] (int start)
        {
            if (doSwitch && start == switchBlock)
            {
                setFloatParam (fresh, params::modType, (float) b.type);
                setFloatParam (fresh, voicingIds[b.type], (float) b.index);
            }
        });
    };

    std::vector<std::vector<double>> steady;

    for (const auto& v : voicings)
        steady.push_back (highBand (render (v, v, false), sampleRate, 2000.0));

    int failing = 0, pairs = 0;
    double worst = -240.0;

    for (size_t i = 0; i < voicings.size(); ++i)
        for (size_t j = 0; j < voicings.size(); ++j)
        {
            if (voicings[i].type == voicings[j].type)
                continue;

            const auto switched = highBand (render (voicings[i], voicings[j], true), sampleRate, 2000.0);
            const double tick = popExcessDb (switched, steady[i], steady[j], (size_t) (switchBlock - at (0.001)),
                                             (size_t) (switchBlock + at (0.1)), (size_t) at (0.2));
            ++pairs;
            worst = std::max (worst, tick);

            if (tick > -60.0)
            {
                ++failing;
                std::snprintf (what, sizeof (what), "mod_type %s %s -> %s %s ticks %.1f dBFS over the steady high band (bound -60)",
                               typeNames[voicings[i].type], voicings[i].name, typeNames[voicings[j].type], voicings[j].name, tick);
                ok &= expect (false, what);
            }
        }

    std::snprintf (what, sizeof (what), "every ordered pair of voicings across mod_type (%d pairs): worst tick %.1f dBFS over "
                   "the steady high band (bound -60), %d over", pairs, worst, failing);
    ok &= expect (failing == 0, what);

    // Voicing switches inside one type run inside the engines (the dispatcher hands the new
    // voicing to the running engine, which runModEngineTests proves bit for bit). Printed: the
    // tick and the largest 10 ms level excursion past both steady levels.
    for (size_t i = 0; i < voicings.size(); ++i)
        for (size_t j = 0; j < voicings.size(); ++j)
        {
            if (i == j || voicings[i].type != voicings[j].type)
                continue;

            tubamp::TubampAudioProcessor fresh;
            setFloatParam (fresh, params::modType, (float) voicings[i].type);
            setFloatParam (fresh, voicingIds[voicings[i].type], (float) voicings[i].index);
            setFloatParam (fresh, params::modMix, 1.0f);
            fresh.setChainOrder ({ BlockId::mod }, {});
            fresh.prepareToPlay (sampleRate, blockSize);
            const auto y = renderSignal (fresh, tone, blockSize, [&] (int start)
            {
                if (start == switchBlock)
                    setFloatParam (fresh, voicingIds[voicings[j].type], (float) voicings[j].index);
            });

            // The 10 ms levels of the switch against the range each voicing alone moves through.
            const auto rmsDb = [&] (const std::vector<float>& x, int from) { return toDb (rmsOf (x, (size_t) from, (size_t) (from + at (0.01)))); };
            const auto alone = [&] (const Voicing& v)
            {
                tubamp::TubampAudioProcessor single;
                setFloatParam (single, params::modType, (float) v.type);
                setFloatParam (single, voicingIds[v.type], (float) v.index);
                setFloatParam (single, params::modMix, 1.0f);
                single.setChainOrder ({ BlockId::mod }, {});
                single.prepareToPlay (sampleRate, blockSize);
                return renderSignal (single, tone, blockSize);
            };
            const auto first = alone (voicings[i]), second = alone (voicings[j]);
            double low = 1000.0, high = -1000.0, excursion = 0.0;

            for (int f = switchBlock - at (0.15); f + at (0.01) < (int) first.size(); f += at (0.005))
                for (const auto* x : { &first, &second })
                {
                    low = std::min (low, rmsDb (*x, f));
                    high = std::max (high, rmsDb (*x, f));
                }

            for (int f = switchBlock; f + at (0.01) < switchBlock + at (0.15); f += at (0.005))
                excursion = std::max ({ excursion, rmsDb (y, f) - high, low - rmsDb (y, f) });

            const auto hb = highBand (y, sampleRate, 2000.0);
            std::printf ("  (engine) %s %s -> %s: tick %.1f dBFS, level outside both steady levels by %.2f dB\n",
                         typeNames[voicings[i].type], voicings[i].name, voicings[j].name,
                         popExcessDb (hb, steady[i], steady[j], (size_t) switchBlock, (size_t) (switchBlock + at (0.1)),
                                      (size_t) at (0.2)), excursion);
        }

    // --- tempo sync: the dispatcher hands each engine the song position's phase every block,
    // jumps included, and free-runs at the synced rate when the transport stops or when the
    // host reports no usable tempo (<= 1 BPM: 120 BPM, and no phase to follow).
    {
        // A host tempo of 0 or below comes with a song position that does not move: the LFO must
        // run free at 120 BPM's rate, not lock to the frozen position (it crawled at 0.075 Hz).
        struct Scenario { const char* label; double bpm; };
        static constexpr Scenario scenarios[] { { "120 BPM with a locate, a loop, a stop and a restart", 120.0 },
                                                { "a host tempo of 0.5 BPM (120 BPM, free-running)", 0.5 },
                                                { "a host tempo of 0 with a frozen position (120 BPM, free-running)", 0.0 },
                                                { "a negative host tempo with a frozen position (120 BPM, free-running)", -120.0 } };

        for (const auto& s : scenarios)
            for (const auto& v : { Voicing { 0, 2, "CE" }, Voicing { 1, 2, "Small Stone" }, Voicing { 2, 1, "Smooth" } })
            {
                tubamp::TubampAudioProcessor fresh;
                TransportPlayHead playHead;
                playHead.bpm = s.bpm;
                setFloatParam (fresh, params::modType, (float) v.type);
                setFloatParam (fresh, voicingIds[v.type], (float) v.index);
                setFloatParam (fresh, params::modSync, 9.0f);   // 1/4
                fresh.setPlayHead (&playHead);
                fresh.setChainOrder ({ BlockId::mod }, {});
                fresh.prepareToPlay (sampleRate, blockSize);

                ModEnginesAlone alone (sampleRate, blockSize);
                const double beats = tubamp::kModSyncBeats[9];
                const double usableBpm = s.bpm > 1.0 ? s.bpm : 120.0;
                const float rate = (float) (usableBpm / (60.0 * beats));
                const auto input = chordSignal (sampleRate, 1.4);
                bool playingNow = true;
                double ppqNow = 0.0;

                const auto r = renderLockstep (fresh, input, sampleRate, blockSize, [&] (int start)
                {
                    if (start > 0 && s.bpm > 0.0)
                        playHead.advance (blockSize, sampleRate);

                    if (start >= at (0.3) && start < at (0.3) + blockSize) playHead.ppq = 7.3;
                    if (start >= at (0.6) && start < at (0.6) + blockSize) playHead.ppq = 2.0;
                    if (start >= at (0.8) && start < at (0.8) + blockSize) playHead.playing = false;
                    if (start >= at (1.0) && start < at (1.0) + blockSize) { playHead.playing = true; playHead.ppq = 0.0; }

                    playingNow = playHead.playing;
                    ppqNow = playHead.ppq;
                },
                [&] (juce::dsp::AudioBlock<float> block)
                {
                    double phase = -1.0;

                    if (playingNow && s.bpm > 1.0)
                    {
                        const double cycles = ppqNow / beats;
                        phase = cycles - std::floor (cycles);
                    }

                    alone.run (fresh, 0, block, rate, phase);
                });

                fresh.setPlayHead (nullptr);
                std::snprintf (what, sizeof (what), "%s, %s: the processor equals the engine handed every song-position "
                               "phase as it comes (or -1 to free-run)", v.name, s.label);
                ok &= expectIdentical (r.processor, r.engine, what);
            }
    }

    return ok;
}

/** A latency change in one lane (a drive switched on or off or changing type) never drops or
    steps the other lane: its alignment tap moves under a crossfade, not through a cleared ring. */
bool runLaneLatencyChangeTests (double sampleRate, int blockSize)
{
    std::printf ("lane latency changes:\n");
    bool ok = true;
    char what[300];

    const auto at = [sampleRate] (double seconds) { return (int) std::lround (seconds * sampleRate); };
    const int changeAt = ((at (0.8) + blockSize - 1) / blockSize) * blockSize;

    // At its crest where the change lands, so a dropped or skipped sample shows at full size.
    std::vector<float> tone ((size_t) at (1.6));

    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = 0.25f * (float) std::cos (2.0 * juce::MathConstants<double>::pi * 100.0 * ((double) i - changeAt) / sampleRate);

    const double steadyStep = 2.0 * juce::MathConstants<double>::pi * 100.0 * 0.25 / sampleRate;

    struct Change { const char* label; std::vector<std::pair<const char*, float>> before, after; };
    const std::vector<Change> changes {
        { "Classic drive on", { { params::driveType, 0 }, { params::driveOn, 0 } }, { { params::driveOn, 1 } } },
        { "Classic drive off", { { params::driveType, 0 }, { params::driveOn, 1 } }, { { params::driveOn, 0 } } },
        { "drive Boost -> Distortion", { { params::driveType, 4 }, { params::driveOn, 1 } }, { { params::driveType, 2 } } },
        { "drive Screamer -> Classic", { { params::driveType, 1 }, { params::driveOn, 1 } }, { { params::driveType, 0 } } } };

    for (const auto& c : changes)
    {
        tubamp::TubampAudioProcessor fresh;

        for (const auto& [id, value] : c.before)
            setFloatParam (fresh, id, value);

        setFloatParam (fresh, params::mixAPan, -1.0f);
        setFloatParam (fresh, params::mixBPan, 1.0f);
        fresh.setChainOrder ({ BlockId::split, BlockId::drive, BlockId::lane2, BlockId::mix }, {});
        fresh.prepareToPlay (sampleRate, blockSize);

        const int latencyBefore = fresh.getLatencySamples();
        int latencyAfter = -1;
        const auto out = renderStereo (fresh, tone, tone, blockSize, [&] (int start)
        {
            if (start >= changeAt && start < changeAt + blockSize)
                for (const auto& [id, value] : c.after)
                    setFloatParam (fresh, id, value);
        });
        latencyAfter = reportedLatency (fresh);

        double largest = 0.0;
        int zeroRun = 0, longestZeroRun = 0;

        for (size_t i = (size_t) at (0.5); i + 1 < out.r.size(); ++i)
        {
            largest = std::max (largest, (double) std::abs (out.r[i + 1] - out.r[i]));
            zeroRun = out.r[i] == 0.0f ? zeroRun + 1 : 0;
            longestZeroRun = std::max (longestZeroRun, zeroRun);
        }

        std::snprintf (what, sizeof (what), "%s in lane A (latency %d -> %d): lane B's largest sample step %.4f (a steady "
                       "100 Hz step is %.4f), longest run of silent samples %d", c.label, latencyBefore, latencyAfter,
                       largest, steadyStep, longestZeroRun);
        ok &= expect (latencyBefore != latencyAfter && largest <= 1.5 * steadyStep && longestZeroRun <= 1, what);
    }

    return ok;
}

/** The latency told to the host is the delay the audio gets: an impulse through every drive
    type in every slot, in series and in each lane of a split, at four rates. The lane without
    the drive is delayed by exactly the reported figure, and so is a non-Classic drive's input
    while it is bypassed (its latency stays counted, as an FX slot's does). A drive waiting for
    its re-entry reset neither delays nor counts. */
bool runLatencyAccountingTests (int blockSize)
{
    std::printf ("latency accounting:\n");
    bool ok = true;
    char what[300];
    int mismatches = 0, checks = 0;

    const auto impulseAt = [] (const std::vector<float>& y)
    {
        size_t best = 0;

        for (size_t i = 1; i < y.size(); ++i)
            if (std::abs (y[i]) > std::abs (y[best]))
                best = i;

        return (int) best;
    };

    for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int k = 0; k < params::maxInstances; ++k)
            for (int type = 0; type < tubamp::DriveFx::numTypes; ++type)
                for (const bool driveOn : { true, false })
                {
                    const auto drive = tubamp::chain::instanceId (BlockId::drive, k);
                    const int expected = (driveOn || type != 0) ? tubamp::DriveFx::latencyFor (type, rate) : 0;
                    std::vector<float> impulse ((size_t) blockSize * 8, 0.0f);
                    impulse[(size_t) blockSize * 2] = 0.001f;

                    for (int placement = 0; placement < 3; ++placement)
                    {
                        tubamp::TubampAudioProcessor fresh;
                        setFloatParam (fresh, params::driveTypeIds[k], (float) type);
                        setBoolParam (fresh, params::driveOnIds[k], driveOn);
                        setFloatParam (fresh, params::driveGainIds[k], 0.0f);
                        setFloatParam (fresh, params::mixAPan, -1.0f);
                        setFloatParam (fresh, params::mixBPan, 1.0f);

                        const tubamp::chain::Order order = placement == 0 ? tubamp::chain::Order { drive }
                                                         : placement == 1 ? tubamp::chain::Order { BlockId::split, drive, BlockId::lane2, BlockId::mix }
                                                                          : tubamp::chain::Order { BlockId::split, BlockId::lane2, drive, BlockId::mix };
                        fresh.setChainOrder (order, {});
                        fresh.prepareToPlay (rate, blockSize);
                        const int preparedReport = fresh.getLatencySamples();
                        const auto out = renderStereo (fresh, impulse, impulse, blockSize);
                        const int reported = fresh.getLatencySamples();

                        // In series, a bypassed drive's own delay is measured directly; with the drive
                        // running its circuit smears the impulse, so the engine's figure stands (and
                        // runDriveLatencyTests measures that one). In a lane, the empty lane is exact.
                        int applied = expected;

                        if (placement == 0 && ! driveOn)
                            applied = impulseAt (out.l) - blockSize * 2;
                        else if (placement == 1)
                            applied = impulseAt (out.r) - blockSize * 2;
                        else if (placement == 2)
                            applied = impulseAt (out.l) - blockSize * 2;

                        ++checks;
                        const bool pass = reported == expected && preparedReport == expected && applied == expected;

                        if (! pass)
                        {
                            ++mismatches;
                            std::snprintf (what, sizeof (what), "drive%d %s %s at %.0f Hz, %s: reported %d (prepare %d), "
                                           "applied %d, engine %d", k + 1, params::driveTypeNames[type], driveOn ? "on" : "off",
                                           rate, placement == 0 ? "series" : placement == 1 ? "lane A" : "lane B",
                                           reported, preparedReport, applied, expected);
                            ok &= expect (false, what);
                        }
                    }
                }

    std::snprintf (what, sizeof (what), "every drive type, slot, placement and rate (44.1/48/96/192 kHz), on and off: reported "
                   "latency = applied delay = the engine's (%d of %d differ)", mismatches, checks);
    ok &= expect (mismatches == 0, what);

    // Re-entry: three blocks come back in one callback and two resets are drained per callback,
    // so drive3 waits a callback, bypassed. A Classic one neither delays nor counts while it waits.
    for (const int type : { 0, 1 })
    {
        tubamp::TubampAudioProcessor fresh;
        setBoolParam (fresh, params::drive3On, true);
        setFloatParam (fresh, params::drive3Type, (float) type);
        fresh.setChainOrder ({}, {});
        fresh.prepareToPlay (48000.0, blockSize);

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;
        buffer.clear();
        fresh.processBlock (buffer, midi);

        fresh.setChainOrder ({ BlockId::comp, BlockId::eq, BlockId::drive3 }, {});
        buffer.clear();
        buffer.setSample (0, 0, 0.001f);
        buffer.setSample (1, 0, 0.001f);
        fresh.processBlock (buffer, midi);
        const int waiting = reportedLatency (fresh);
        int appliedWaiting = 0;

        for (int i = 0; i < blockSize; ++i)
            if (std::abs (buffer.getSample (0, i)) > std::abs (buffer.getSample (0, appliedWaiting)))
                appliedWaiting = i;

        buffer.clear();
        fresh.processBlock (buffer, midi);
        const int running = reportedLatency (fresh);
        const int engine = tubamp::DriveFx::latencyFor (type, 48000.0);
        const int expectedWaiting = type == 0 ? 0 : engine;

        std::snprintf (what, sizeof (what), "%s drive3 waiting for its re-entry reset: reported %d, applied %d (expected %d); "
                       "running: %d (engine %d)", params::driveTypeNames[type], waiting, appliedWaiting, expectedWaiting,
                       running, engine);
        ok &= expect (waiting == expectedWaiting && appliedWaiting == expectedWaiting && running == engine, what);
    }

    return ok;
}

/** The first instance on a machine without presets writes the factory presets without
    touching its own state, and comes up exactly as an instance that found them already there:
    every parameter at its default (the recommended types, not the legacy pin), the amp alone. */
bool runFreshInstallTests (double sampleRate, int blockSize)
{
    std::printf ("fresh install:\n");
    bool ok = true;
    char what[300];

    const auto home = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("tubamp-fresh-install-" + juce::String (juce::Time::getMillisecondCounter()));
    home.createDirectory();

    const char* const previous = std::getenv ("CFFIXED_USER_HOME");
    const std::string saved = previous != nullptr ? previous : "";
    setenv ("CFFIXED_USER_HOME", home.getFullPathName().toRawUTF8(), 1);

    const auto presetsDir = tubamp::ModelLibrary::getPresetsDir();
    ok &= expect (presetsDir.isAChildOf (home) && ! presetsDir.exists(), "the presets folder is redirected into an empty home");

    const auto differingFromDefault = [] (tubamp::TubampAudioProcessor& p, juce::StringArray& names)
    {
        for (auto* raw : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (raw))
                if (! juce::approximatelyEqual (ranged->getValue(), ranged->getDefaultValue()))
                    names.add (ranged->paramID);

        return names.size();
    };

    std::vector<float> firstValues, secondValues;
    {
        tubamp::TubampAudioProcessor first;
        first.prepareToPlay (sampleRate, blockSize);
        const int written = presetsDir.getNumberOfChildFiles (juce::File::findFiles, "*.json");
        juce::StringArray changed;
        const int count = differingFromDefault (first, changed);
        std::snprintf (what, sizeof (what), "the instance that writes the %d factory presets keeps every parameter at its default "
                       "(%d differ: %s), order \"%s\"", written, count, changed.joinIntoString (" ").toRawUTF8(),
                       tubamp::chain::toString (first.getChainOrder()).toRawUTF8());
        ok &= expect (written == 4 && count == 0 && tubamp::chain::toString (first.getChainOrder()) == "amp", what);
        firstValues = paramSnapshot (first);
    }

    {
        tubamp::TubampAudioProcessor second;
        second.prepareToPlay (sampleRate, blockSize);
        secondValues = paramSnapshot (second);
        ok &= expect (choiceIndex (second, params::gateMode) == (int) tubamp::GateFx::Mode::tight
                          && choiceIndex (second, params::compType) == (int) tubamp::CompFx::Type::clean
                          && choiceIndex (second, params::driveType) == (int) tubamp::DriveFx::Type::screamer,
                      "an instance that finds the presets comes up on the recommended types");
    }

    ok &= expect (firstValues == secondValues, "both come up identically, whether or not the presets folder existed");

    // The files themselves: every type Classic (D-2) and each preset's own settings, Ambient
    // Lead's drive included (every earlier install wrote it that way).
    {
        tubamp::TubampAudioProcessor reader;
        bool contents = true;

        for (const auto& [name, id, value] : std::vector<std::tuple<juce::String, const char*, float>> {
                 { "Init", params::driveOn, 0.0f }, { "Clean + Room", params::reverbMix, 0.35f },
                 { "Crunch Drive", params::driveGain, 18.0f }, { "Ambient Lead", params::driveOn, 1.0f },
                 { "Ambient Lead", params::delayMix, 0.35f }, { "Ambient Lead", params::modOn, 1.0f } })
        {
            const auto parsed = juce::JSON::parse (presetsDir.getChildFile (name + ".json"));
            const auto p = parsed.getProperty ("state", {}).getProperty ("params", {});
            contents &= std::abs ((float) (double) p.getProperty (id, -999.0) - value) < 1.0e-6f;
            contents &= (int) (double) p.getProperty (params::gateMode, -1.0) == 0
                        && (int) (double) p.getProperty (params::eqType, -1.0) == 0;
        }

        ok &= expect (contents, "the factory preset files carry their settings with every engine type at Classic");
    }

    if (saved.empty())
        unsetenv ("CFFIXED_USER_HOME");
    else
        setenv ("CFFIXED_USER_HOME", saved.c_str(), 1);

    home.deleteRecursively();
    return ok;
}

/** Knobs whose starting point depends on the type default to the default type's entry in the
    engine header, and the frozen drive knobs equal Classic's and Screamer's entries. */
bool runStartingPointTests()
{
    std::printf ("per-type starting points:\n");
    bool ok = true;

    tubamp::TubampAudioProcessor fresh;
    // A stepped range whose start is not 0 reads its default back a few ulp off (D-23).
    const auto startsAt = [&fresh] (const char* id, float expected)
    {
        return std::abs (fresh.apvts.getRawParameterValue (id)->load() - expected) <= 1.0e-5f * std::max (1.0f, std::abs (expected));
    };

    const int tight = (int) tubamp::GateFx::Mode::tight;
    ok &= expect (startsAt (params::gateRelease, tubamp::kGateReleaseDefault[tight])
                      && startsAt (params::gateHold, tubamp::kGateHoldDefault[tight])
                      && startsAt (params::gateScHpf, tubamp::kGateScHpfDefault[tight]),
                  "gate release, hold and sidechain HPF start at Tight's entries (75 Hz HPF)");

    bool comp = true, drive = true;
    const int classic = (int) tubamp::DriveFx::Type::classic, screamer = (int) tubamp::DriveFx::Type::screamer;

    for (int k = 0; k < params::maxInstances; ++k)
    {
        comp &= startsAt (params::compKneeIds[k], tubamp::kCompKneeDefault[(int) tubamp::CompFx::Type::clean]);

        for (const int t : { classic, screamer })
            drive &= startsAt (params::driveGainIds[k], tubamp::kDriveGainDefault[t])
                     && startsAt (params::driveToneIds[k], tubamp::kDriveToneDefault[t])
                     && startsAt (params::driveLevelIds[k], tubamp::kDriveLevelDefault[t]);
    }

    ok &= expect (comp, "comp knee starts at Clean's entry");
    ok &= expect (drive, "drive gain, tone and level start at Classic's and Screamer's entries");
    ok &= expect (juce::approximatelyEqual (tubamp::kDriveLevelDefault[(int) tubamp::DriveFx::Type::fuzz], 5.0f),
                  "the Fuzz level starting point is 5.0 dB");
    return ok;
}

//==============================================================================
// Processor round 2 (.audit/fx-verify/r2-processor.md sections 6 and 7).

/** Counts latency changes the processor announces, and on which thread. */
struct LatencyWatch final : juce::AudioProcessorListener
{
    std::atomic<int> fromMessageThread { 0 }, fromOtherThreads { 0 };

    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}

    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (! details.latencyChanged)
            return;

        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            ++fromMessageThread;
        else
            ++fromOtherThreads;
    }
};

/** 6.1, 6.6, two threads: setLatencySamples is never reached from the audio thread (it takes
    JUCE's listener lock and makes the AU wrapper post a message) nor from a host's restore
    thread; the message thread reports what the audio thread applies, and with no audio
    running works the figure out itself when a latency-changing parameter, the chain or the
    state changes. */
bool runLatencyThreadTests (double sampleRate, int blockSize)
{
    std::printf ("latency from the message thread:\n");
    bool ok = true;
    char what[300];

    // --- drive_on and drive_type automated on an audio thread of its own, every block, with the
    // message thread pumping as a host's would.
    {
        tubamp::TubampAudioProcessor fresh;
        LatencyWatch watch;
        fresh.addListener (&watch);
        setBoolParam (fresh, params::driveOn, true);
        setFloatParam (fresh, params::driveType, (float) tubamp::DriveFx::Type::boost);
        setBoolParam (fresh, params::drive2On, true);
        setFloatParam (fresh, params::drive2Type, (float) tubamp::DriveFx::Type::classic);
        setFloatParam (fresh, params::drive3Type, (float) tubamp::DriveFx::Type::fuzz);
        fresh.setChainOrder ({ BlockId::split, BlockId::drive, BlockId::lane2, BlockId::drive2, BlockId::mix, BlockId::drive3 }, {});
        fresh.prepareToPlay (sampleRate, blockSize);
        watch.fromMessageThread = 0;

        std::atomic<bool> done { false };
        std::atomic<long> allocations { 0 }, changes { 0 }, blocks { 0 };

        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            const auto chord = powerChord (sampleRate, (double) blockSize / sampleRate * 400.0);
            const int types[] { 4, 2, 1, 0, 5, 3 };

            for (int b = 0; b < 400; ++b)
            {
                // Host automation arrives on the audio thread, between callbacks.
                if (b % 2 == 0)
                    setBoolParam (fresh, params::drive2On, (b / 2) % 2 == 0);
                else
                    setFloatParam (fresh, params::driveType, (float) types[(b / 2) % 6]);

                setFloatParam (fresh, params::drive3On, (float) (b % 3 == 0));
                ++changes;

                for (int ch = 0; ch < 2; ++ch)
                    std::copy (chord.begin() + (long) b * blockSize, chord.begin() + (long) (b + 1) * blockSize,
                               buffer.getWritePointer (ch));

                allocCount.store (0);
                {
                    AllocationGuard guard;
                    fresh.processBlock (buffer, midi);
                }
                allocations += allocCount.load();
                ++blocks;
            }

            done = true;
        });

        while (! done)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);

        audio.join();
        const int settled = reportedLatency (fresh);
        fresh.removeListener (&watch);

        std::snprintf (what, sizeof (what), "drive_on and drive_type automated every block for %ld blocks on an audio thread: "
                       "%ld allocations there, %d latency changes announced from it, %d from the message thread",
                       blocks.load(), allocations.load(), watch.fromOtherThreads.load(), watch.fromMessageThread.load());
        ok &= expect (allocations == 0 && watch.fromOtherThreads == 0 && watch.fromMessageThread > 0, what);
        ok &= expect (settled == fresh.getLatencySamples() && settled >= 0, "...and the message thread hands the figure over");
    }

    // --- 6.6: no audio running. A latency-changing parameter moved on the message thread is
    // reported at once; moved on another thread, by the next timer tick.
    {
        tubamp::TubampAudioProcessor fresh;
        setBoolParam (fresh, params::driveOn, true);
        setFloatParam (fresh, params::driveType, (float) tubamp::DriveFx::Type::screamer);
        fresh.setChainOrder ({ BlockId::drive }, {});
        fresh.prepareToPlay (sampleRate, blockSize);
        const int screamer = fresh.getLatencySamples();

        setFloatParam (fresh, params::driveType, (float) tubamp::DriveFx::Type::boost);
        const int boost = fresh.getLatencySamples();

        setFloatParam (fresh, params::driveType, (float) tubamp::DriveFx::Type::classic);
        setBoolParam (fresh, params::driveOn, false);
        const int classicOff = fresh.getLatencySamples();

        std::thread other ([&] { setBoolParam (fresh, params::driveOn, true); });
        other.join();
        const int classicOnElsewhere = reportedLatency (fresh);

        std::snprintf (what, sizeof (what), "no audio running: Screamer %d, drive_type Boost %d at once, Classic off %d at once, "
                       "Classic on from another thread %d after a timer tick (expected %d, 0, 0, %d)", screamer, boost, classicOff,
                       classicOnElsewhere, tubamp::DriveFx::latencyFor (1, sampleRate), tubamp::DriveFx::latencyFor (0, sampleRate));
        ok &= expect (screamer == tubamp::DriveFx::latencyFor (1, sampleRate) && boost == 0 && classicOff == 0
                          && classicOnElsewhere == tubamp::DriveFx::latencyFor (0, sampleRate), what);
    }

    // --- a state restore on a host thread, while audio runs on another: no latency is
    // announced from either; the message thread reports the restored chain's.
    {
        tubamp::TubampAudioProcessor source;
        setBoolParam (source, params::driveOn, true);
        setFloatParam (source, params::driveType, (float) tubamp::DriveFx::Type::distortion);
        source.setChainOrder ({ BlockId::comp, BlockId::drive }, {});
        juce::MemoryBlock state;
        source.getStateInformation (state);

        tubamp::TubampAudioProcessor fresh;
        LatencyWatch watch;
        fresh.addListener (&watch);
        fresh.setChainOrder ({ BlockId::eq }, {});
        fresh.prepareToPlay (sampleRate, blockSize);

        std::atomic<bool> stop { false };
        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;

            while (! stop)
            {
                buffer.clear();
                fresh.processBlock (buffer, midi);
                std::this_thread::sleep_for (std::chrono::milliseconds (2));
            }
        });

        std::thread restore ([&] { fresh.setStateInformation (state.getData(), (int) state.getSize()); });
        restore.join();
        const int restored = reportedLatency (fresh);
        stop = true;
        audio.join();
        fresh.removeListener (&watch);

        std::snprintf (what, sizeof (what), "a state restored on a host thread while audio runs: %d latency changes announced off the "
                       "message thread; reported %d (the restored Distortion's %d)", watch.fromOtherThreads.load(), restored,
                       tubamp::DriveFx::latencyFor (2, sampleRate));
        ok &= expect (watch.fromOtherThreads == 0 && restored == tubamp::DriveFx::latencyFor (2, sampleRate), what);
    }

    return ok;
}

/** A -12 dBFS chord: the power chord, which has nothing above 1 kHz. */
std::vector<float> quieterChord (double sampleRate, double seconds)
{
    auto x = powerChord (sampleRate, seconds);

    for (auto& v : x)
        v *= juce::Decibels::decibelsToGain (-2.0f);

    return x;
}

double largestStep (const std::vector<float>& x, size_t from, size_t to)
{
    double largest = 0.0;

    for (size_t i = from; i + 1 < std::min (to, x.size()); ++i)
        largest = std::max (largest, (double) std::abs (x[i + 1] - x[i]));

    return largest;
}

/** 6.2: drive_type changed on a bypassed drive slides the bypass delay to its new length over
    10 ms instead of jumping it by the latency difference. */
bool runBypassedDriveTypeTests (double sampleRate, int blockSize)
{
    std::printf ("drive type changed while bypassed:\n");
    bool ok = true;
    char what[300];

    const auto at = [sampleRate] (double seconds) { return (int) std::lround (seconds * sampleRate); };
    const auto chord = quieterChord (sampleRate, 1.0);
    const int changeAt = ((at (0.5) + blockSize - 1) / blockSize) * blockSize;
    using T = tubamp::DriveFx::Type;
    const std::pair<T, T> changes[] { { T::boost, T::screamer }, { T::screamer, T::boost }, { T::classic, T::fuzz },
                                      { T::distortion, T::treble } };

    for (const auto& [from, to] : changes)
    {
        const auto render = [&] (bool change)
        {
            tubamp::TubampAudioProcessor fresh;
            setBoolParam (fresh, params::driveOn, false);
            setFloatParam (fresh, params::driveType, (float) from);
            fresh.setChainOrder ({ BlockId::drive }, {});
            fresh.prepareToPlay (sampleRate, blockSize);

            return renderSignal (fresh, chord, blockSize, [&] (int start)
            {
                if (change && start == changeAt)
                    setFloatParam (fresh, params::driveType, (float) to);
            });
        };

        const auto changed = render (true), steady = render (false);
        const auto band = highBand (changed, sampleRate, 5000.0);
        const double click = toDb (peakIn (band, (size_t) (changeAt - at (0.005)), (size_t) (changeAt + at (0.05))));
        const double step = largestStep (changed, (size_t) (changeAt - at (0.005)), (size_t) (changeAt + at (0.05)));
        const double steadyStep = largestStep (steady, (size_t) (changeAt - at (0.005)), (size_t) (changeAt + at (0.05)));

        std::snprintf (what, sizeof (what), "bypassed drive %s -> %s (delay %d -> %d) on a -12 dBFS chord: above 5 kHz %.1f dBFS "
                       "(bound -70); largest step %.5f against the steady %.5f (bound x1.05)", params::driveTypeNames[(int) from],
                       params::driveTypeNames[(int) to], from == T::classic ? 0 : tubamp::DriveFx::latencyFor ((int) from, sampleRate),
                       tubamp::DriveFx::latencyFor ((int) to, sampleRate), click, step, steadyStep);
        ok &= expect (click <= -70.0 && step <= 1.05 * steadyStep, what);
    }

    return ok;
}

/** 6.3: two lane-latency changes inside 10 ms, and a discrete parameter swept at block rate,
    move the waiting lane smoothly: every slide starts from where the tap is. */
bool runLaneRetargetTests (const juce::File& modelFile, double sampleRate)
{
    std::printf ("lane latency changes in quick succession:\n");
    bool ok = true;
    char what[300];

    const int blockSize = 64;
    const auto at = [sampleRate] (double seconds) { return (int) std::lround (seconds * sampleRate); };
    const int changeAt = ((at (0.6) + blockSize - 1) / blockSize) * blockSize;
    const double pi = juce::MathConstants<double>::pi;

    // A sine at fs / 10 (the verifier's) and a 100 Hz one: lane B, which waits, must stay one.
    for (const double hz : { sampleRate / 10.0, 100.0 })
    {
        std::vector<float> tone ((size_t) at (1.4));

        for (size_t i = 0; i < tone.size(); ++i)
            tone[i] = 0.25f * (float) std::sin (2.0 * pi * hz * (double) i / sampleRate);

        const double steadyStep = 2.0 * 0.25 * std::sin (pi * hz / sampleRate);

        struct Case { std::string label; std::function<void (tubamp::TubampAudioProcessor&, int)> automate; bool needsModel; };
        const std::vector<Case> cases {
            { "drive_type Boost -> Distortion -> Boost 3 ms apart",
              [&] (tubamp::TubampAudioProcessor& p, int start)
              {
                  if (start == changeAt) setFloatParam (p, params::driveType, 2.0f);
                  if (start == changeAt + ((at (0.003) + blockSize - 1) / blockSize) * blockSize) setFloatParam (p, params::driveType, 4.0f);
              }, false },
            { "drive_type swept Boost / Distortion every block for 0.4 s",
              [&] (tubamp::TubampAudioProcessor& p, int start)
              {
                  if (start >= changeAt && start < changeAt + at (0.4))
                      setFloatParam (p, params::driveType, ((start - changeAt) / blockSize) % 2 == 0 ? 2.0f : 4.0f);
              }, false },
            { "Classic drive_on toggled every block for 0.4 s",
              [&] (tubamp::TubampAudioProcessor& p, int start)
              {
                  if (start == changeAt) setFloatParam (p, params::driveType, 0.0f);
                  if (start >= changeAt && start < changeAt + at (0.4))
                      setBoolParam (p, params::driveOn, ((start - changeAt) / blockSize) % 2 == 1);
              }, false },
            { "amp_on (resampled model) toggled every block for 0.4 s",
              [&] (tubamp::TubampAudioProcessor& p, int start)
              {
                  if (start >= changeAt && start < changeAt + at (0.4))
                      setBoolParam (p, params::ampOn, ((start - changeAt) / blockSize) % 2 == 1);
              }, true } };

        for (const auto& c : cases)
        {
            tubamp::TubampAudioProcessor fresh;
            setBoolParam (fresh, params::driveOn, true);
            setFloatParam (fresh, params::driveType, 4.0f);
            setFloatParam (fresh, params::mixAPan, -1.0f);
            setFloatParam (fresh, params::mixBPan, 1.0f);
            fresh.prepareToPlay (sampleRate, blockSize);

            if (c.needsModel)
            {
                if (fresh.loadModel (modelFile).isNotEmpty())
                    continue;

                juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
                fresh.setChainOrder ({ BlockId::split, BlockId::amp, BlockId::lane2, BlockId::mix }, {});
                setBoolParam (fresh, params::ampEqOn, false);
            }
            else
            {
                fresh.setChainOrder ({ BlockId::split, BlockId::drive, BlockId::lane2, BlockId::mix }, {});
            }

            fresh.prepareToPlay (sampleRate, blockSize);

            if (c.needsModel && fresh.namEngine.getLatencySamples() == 0)
                continue;   // the model runs at this rate: amp_on moves no latency

            const auto out = renderStereo (fresh, tone, tone, blockSize, [&] (int start) { c.automate (fresh, start); });
            const double largest = largestStep (out.r, (size_t) (changeAt - at (0.05)), (size_t) (changeAt + at (0.6)));
            int zeroRun = 0, longest = 0;

            for (size_t i = (size_t) (changeAt - at (0.05)); i < out.r.size(); ++i)
            {
                zeroRun = out.r[i] == 0.0f ? zeroRun + 1 : 0;
                longest = std::max (longest, zeroRun);
            }

            std::snprintf (what, sizeof (what), "%s in lane A, %.0f Hz: lane B's largest step %.4f (steady %.4f, bound x1.1), "
                           "longest silent run %d", c.label.c_str(), hz, largest, steadyStep, longest);
            ok &= expect (largest <= 1.1 * steadyStep && longest <= 1, what);
        }
    }

    return ok;
}

/** 6.4 (drive fixer): in a split with a drive in one lane, a type switch keeps the lanes in
    line while the drive runs both types and slides its latency: at most 1 sample apart for at
    most 10 ms, and no comb deeper than 1 dB below 8 kHz in the sum. */
bool runDriveSwitchLaneTests (double sampleRate)
{
    std::printf ("drive type switches in a split lane:\n");
    bool ok = true;
    char what[300];

    const int blockSize = 64;
    const auto at = [sampleRate] (double seconds) { return (int) std::lround (seconds * sampleRate); };
    const int changeAt = ((at (0.5) + blockSize - 1) / blockSize) * blockSize;
    const double pi = juce::MathConstants<double>::pi;
    static constexpr double tones[] { 1000, 2000, 3000, 4000, 4800, 6000, 7000, 8000 };
    const int window = at (0.01);   // whole cycles of every tone (multiples of 100 Hz)

    std::vector<float> input ((size_t) at (0.9), 0.0f);

    for (size_t i = 0; i < input.size(); ++i)
        for (const double f : tones)
            input[i] += 0.0025f * (float) std::sin (2.0 * pi * f * (double) i / sampleRate + f * 0.001);

    using T = tubamp::DriveFx::Type;
    const std::pair<T, T> switches[] { { T::boost, T::distortion }, { T::distortion, T::boost }, { T::treble, T::screamer },
                                       { T::screamer, T::classic }, { T::classic, T::treble }, { T::boost, T::classic },
                                       { T::classic, T::boost } };
    // Classic (gain 0, its low-pass at 12 kHz), Boost and Treble are close to pure delays at 1 kHz,
    // so the 1 kHz phase between the lanes is their misalignment; a circuit type's own phase there
    // (several samples at 192 kHz) is not, and for those the comb against the steady types is the
    // measure.
    const auto plain = [] (T type) { return type == T::classic || type == T::boost || type == T::treble; };

    const auto render = [&] (T from, T to, bool doSwitch)
    {
        tubamp::TubampAudioProcessor fresh;
        setBoolParam (fresh, params::driveOn, true);
        setFloatParam (fresh, params::driveType, (float) from);
        setFloatParam (fresh, params::driveGain, 0.0f);
        setFloatParam (fresh, params::driveTone, 12000.0f);
        setFloatParam (fresh, params::driveLevel, 0.0f);
        setFloatParam (fresh, params::mixAPan, -1.0f);
        setFloatParam (fresh, params::mixBPan, 1.0f);
        fresh.setChainOrder ({ BlockId::split, BlockId::drive, BlockId::lane2, BlockId::mix }, {});
        fresh.prepareToPlay (sampleRate, blockSize);

        return renderStereo (fresh, input, input, blockSize, [&] (int start)
        {
            if (doSwitch && start == changeAt)
                setFloatParam (fresh, params::driveType, (float) to);
        });
    };

    // Lane A's lag behind lane B (samples) from the 1 kHz phase, and per tone the sum against
    // the two lanes in phase, |a + b| / (|a| + |b|) in dB.
    constexpr size_t numTones = std::size (tones);
    const auto measure = [&] (const StereoRender& r, int from, double& lag, std::array<double, numTones>& comb)
    {
        const auto a = toneAt (r.l, sampleRate, 1000.0, (size_t) from, (size_t) (from + window));
        const auto b = toneAt (r.r, sampleRate, 1000.0, (size_t) from, (size_t) (from + window));
        lag = -std::arg (a / b) / (2.0 * pi * 1000.0) * sampleRate;

        for (size_t t = 0; t < numTones; ++t)
        {
            const auto fa = toneAt (r.l, sampleRate, tones[t], (size_t) from, (size_t) (from + window));
            const auto fb = toneAt (r.r, sampleRate, tones[t], (size_t) from, (size_t) (from + window));
            comb[t] = toDb (std::abs (fa + fb) / (std::abs (fa) + std::abs (fb)));
        }
    };

    for (const auto& [from, to] : switches)
    {
        // The drive's own circuit sits a fraction of a sample off its reported latency (Classic
        // 0.9 at 48 kHz, the circuits' filters more at 1 kHz), which steady renders show as a
        // shallow comb of their own: the switch is judged against the shallower of the two.
        double lagOld = 0.0, lagNew = 0.0;
        std::array<double, numTones> combOld {}, combNew {};
        measure (render (from, from, false), changeAt - at (0.1), lagOld, combOld);
        measure (render (to, to, false), changeAt - at (0.1), lagNew, combNew);

        const auto r = render (from, to, true);
        double worstComb = 0.0, worstAbsolute = 0.0, worstApart = 0.0;
        int apartWindows = 0;
        const int hop = at (0.001);

        for (int f = changeAt - window; f < changeAt + at (0.08); f += hop)
        {
            double lag = 0.0;
            std::array<double, numTones> comb {};
            measure (r, f, lag, comb);
            const double apart = std::min (std::abs (lag - lagOld), std::abs (lag - lagNew));
            worstApart = std::max (worstApart, apart);
            apartWindows += apart > 1.0 ? 1 : 0;

            for (size_t t = 0; t < numTones; ++t)
            {
                worstAbsolute = std::min (worstAbsolute, comb[t]);
                worstComb = std::min (worstComb, comb[t] - std::min (combOld[t], combNew[t]));
            }
        }

        const bool judgeApart = plain (from) && plain (to);
        std::snprintf (what, sizeof (what), "%s -> %s in lane A (latency %d -> %d): lanes apart at most %.2f samples, more than 1 "
                       "for %d ms (%s); comb below 8 kHz %.2f dB deeper than the steady types' (bound 1; %.2f dB absolute)",
                       params::driveTypeNames[(int) from], params::driveTypeNames[(int) to],
                       tubamp::DriveFx::latencyFor ((int) from, sampleRate), tubamp::DriveFx::latencyFor ((int) to, sampleRate),
                       worstApart, apartWindows, judgeApart ? "bound 10" : "a circuit's own phase, printed", -worstComb, worstAbsolute);
        ok &= expect ((! judgeApart || apartWindows <= 10) && worstComb >= -1.0, what);
    }

    return ok;
}

/** A plucked, decaying take: E2, A2, D3, G3, B3 and a low E chord, five harmonics each. */
std::vector<float> pluckedTake (double sampleRate, double seconds)
{
    std::vector<float> x ((size_t) std::lround (seconds * sampleRate), 0.0f);
    static constexpr double notes[] { 82.41, 110.0, 146.83, 196.0, 246.94, 82.41 };
    const double pi = juce::MathConstants<double>::pi;

    for (size_t k = 0; k < std::size (notes); ++k)
    {
        const auto onset = (size_t) std::lround ((0.05 + 0.12 * (double) k) * sampleRate);

        for (size_t i = onset; i < x.size(); ++i)
        {
            const double t = (double) (i - onset) / sampleRate;
            double v = 0.0;

            for (int h = 1; h <= 5; ++h)
                v += std::sin (2.0 * pi * notes[k] * h * t + 0.3 * h) * std::exp (-t * (2.0 + 1.5 * h)) / h;

            x[i] += (float) (0.08 * v);
        }
    }

    return x;
}

/** 6.5: a mod_type switch runs the incoming engine unheard until it settles and then hands
    over with a power-matched crossfade: every ordered pair of voicings across the three types,
    on held notes at 82, 196 and 440 Hz (-12 dBFS) and on a plucked take, stays within 1.5 dB
    above the higher steady level and 3 dB below the lower, and clicks at most -60 dBFS over the
    steady renders. Held notes, the verifier's measure (r2-processor 6.5): one-period levels
    against the band the two voicings' steady renders span over their whole steady part. The
    take's level follows its notes and each engine's own LFO, so it is held to the two engines
    alone in lockstep, 10 ms window by window (the outgoing one running on, the incoming one
    reset at the switch, as the dispatcher runs them). Printed for the held notes as well: that
    lockstep measure, which two effects in antiphase on one note cannot always meet, since no
    crossfade avoids the null between them. */
bool runModTypeLevelTests (double sampleRate)
{
    std::printf ("mod_type switch levels:\n");
    bool ok = true;
    char what[300];

    const int blockSize = 128;
    const auto at = [sampleRate] (double seconds) { return (int) std::lround (seconds * sampleRate); };
    const int switchAt = ((at (0.8) + blockSize - 1) / blockSize) * blockSize;
    const int length = at (1.6);

    struct Voicing { int type, index; const char* name; };
    std::vector<Voicing> voicings;

    for (int i = 0; i < tubamp::ChorusFx::numModes; ++i)    voicings.push_back ({ 0, i, params::modChorusModeNames[i] });
    for (int i = 0; i < tubamp::PhaserFx::numModels; ++i)   voicings.push_back ({ 1, i, params::modPhaserModelNames[i] });
    for (int i = 0; i < tubamp::TremoloFx::numVoices; ++i)  voicings.push_back ({ 2, i, params::modTremVoiceNames[i] });

    static constexpr const char* typeNames[] { "chorus", "phaser", "tremolo" };
    const char* const voicingIds[] { params::modChorusMode, params::modPhaserModel, params::modTremVoice };

    struct Signal { std::string label; std::vector<float> x; int period; bool held; };
    std::vector<Signal> signals;

    for (const double hz : { 82.41, 196.0, 440.0 })
        signals.push_back ({ juce::String (hz, 0).toStdString() + " Hz note",
                             sineSignal (sampleRate, hz, juce::Decibels::decibelsToGain (-12.0f), length),
                             (int) std::lround (sampleRate / hz), true });

    {
        // The take's notes arrive around the switch.
        auto take = pluckedTake (sampleRate, 1.0);
        std::vector<float> x ((size_t) length, 0.0f);
        std::copy (take.begin(), take.end(), x.begin() + (switchAt - at (0.45)));
        signals.push_back ({ "plucked take", x, at (0.01), false });
    }

    struct Renders { std::vector<float> switched, outgoing, incoming; };

    const auto render = [&] (const Voicing& a, const Voicing& b, bool doSwitch, const std::vector<float>& x)
    {
        tubamp::TubampAudioProcessor fresh;
        setFloatParam (fresh, voicingIds[a.type], (float) a.index);
        setFloatParam (fresh, voicingIds[b.type], (float) b.index);
        setFloatParam (fresh, params::modType, (float) a.type);
        setFloatParam (fresh, params::modMix, 0.5f);
        fresh.setChainOrder ({ BlockId::mod }, {});
        fresh.prepareToPlay (sampleRate, blockSize);

        ModEnginesAlone alone (sampleRate, blockSize);
        juce::AudioBuffer<float> buffers[3] { juce::AudioBuffer<float> (2, blockSize), juce::AudioBuffer<float> (2, blockSize),
                                              juce::AudioBuffer<float> (2, blockSize) };
        juce::MidiBuffer midi;
        Renders out;

        for (int start = 0; start < (int) x.size(); start += blockSize)
        {
            const int n = std::min (blockSize, (int) x.size() - start);

            if (doSwitch && start == switchAt)
            {
                setFloatParam (fresh, params::modType, (float) b.type);
                alone.reset (b.type);
            }

            juce::AudioBuffer<float> views[3];

            for (int r = 0; r < 3; ++r)
            {
                float* channels[2] { buffers[r].getWritePointer (0), buffers[r].getWritePointer (1) };
                views[r].setDataToReferTo (channels, 2, n);

                for (int ch = 0; ch < 2; ++ch)
                    std::copy (x.begin() + start, x.begin() + start + n, channels[ch]);
            }

            fresh.processBlock (views[0], midi);

            if (doSwitch)
            {
                juce::dsp::AudioBlock<float> outgoingBlock (views[1]), incomingBlock (views[2]);
                alone.runFree (fresh, 0, outgoingBlock, a.type);

                if (start >= switchAt)
                    alone.runFree (fresh, 0, incomingBlock, b.type);
                else
                    views[2].clear();

                out.outgoing.insert (out.outgoing.end(), views[1].getReadPointer (0), views[1].getReadPointer (0) + n);
                out.incoming.insert (out.incoming.end(), views[2].getReadPointer (0), views[2].getReadPointer (0) + n);
            }

            out.switched.insert (out.switched.end(), views[0].getReadPointer (0), views[0].getReadPointer (0) + n);
        }

        return out;
    };

    const auto rmsDb = [] (const std::vector<float>& y, int from, int n) { return toDb (rmsOf (y, (size_t) from, (size_t) (from + n))); };
    int failing = 0, pairs = 0;
    double worstAbove = -100.0, worstBelow = -100.0, worstClick = -240.0, lockAbove = -100.0, lockBelow = -100.0;

    for (const auto& sig : signals)
    {
        std::vector<std::vector<float>> steady;

        for (const auto& v : voicings)
            steady.push_back (render (v, v, false, sig.x).switched);

        for (size_t i = 0; i < voicings.size(); ++i)
            for (size_t j = 0; j < voicings.size(); ++j)
            {
                if (voicings[i].type == voicings[j].type)
                    continue;

                const auto r = render (voicings[i], voicings[j], true, sig.x);
                const int step = std::max (1, sig.period / 4);
                double low = 1000.0, high = -1000.0;

                if (sig.held)
                    for (int g = at (0.3); g + sig.period <= length - at (0.3); g += step)
                        for (const auto* y : { &steady[i], &steady[j] })
                        {
                            low = std::min (low, rmsDb (*y, g, sig.period));
                            high = std::max (high, rmsDb (*y, g, sig.period));
                        }

                double above = -100.0, below = -100.0, againstEnginesAbove = -100.0, againstEnginesBelow = -100.0;

                for (int f = switchAt - sig.period; f + sig.period < switchAt + at (0.5); f += step)
                {
                    const double level = rmsDb (r.switched, f, sig.period);

                    if (sig.held)
                    {
                        above = std::max (above, level - high);
                        below = std::max (below, low - level);
                    }

                    if (f >= switchAt)
                    {
                        const double a = rmsDb (r.outgoing, f, sig.period), b = rmsDb (r.incoming, f, sig.period);
                        againstEnginesAbove = std::max (againstEnginesAbove, level - std::max (a, b));
                        againstEnginesBelow = std::max (againstEnginesBelow, std::min (a, b) - level);
                    }
                }

                if (! sig.held)
                {
                    above = againstEnginesAbove;
                    below = againstEnginesBelow;
                }

                const auto hfSwitched = highBand (r.switched, sampleRate, 2000.0);
                const double click = sig.held
                    ? popExcessDb (hfSwitched, highBand (steady[i], sampleRate, 2000.0), highBand (steady[j], sampleRate, 2000.0),
                                   (size_t) (switchAt - at (0.005)), (size_t) (switchAt + at (0.3)), (size_t) at (0.5))
                    : popExcessDb (hfSwitched, highBand (r.outgoing, sampleRate, 2000.0), highBand (r.incoming, sampleRate, 2000.0),
                                   (size_t) switchAt, (size_t) (switchAt + at (0.3)), (size_t) at (0.002));
                ++pairs;
                worstAbove = std::max (worstAbove, above);
                worstBelow = std::max (worstBelow, below);
                worstClick = std::max (worstClick, click);
                if (sig.held)
                {
                    lockAbove = std::max (lockAbove, againstEnginesAbove);
                    lockBelow = std::max (lockBelow, againstEnginesBelow);
                }

                if (above > 1.5 || below > 3.0 || click > -60.0)
                {
                    ++failing;
                    std::snprintf (what, sizeof (what), "mod_type %s %s -> %s %s, %s: %.2f dB above the higher steady level (bound "
                                   "1.5), %.2f dB below the lower (bound 3), click %.1f dBFS over the steady renders (bound -60)",
                                   typeNames[voicings[i].type], voicings[i].name, typeNames[voicings[j].type], voicings[j].name,
                                   sig.label.c_str(), above, below, click);
                    ok &= expect (false, what);
                }
            }
    }

    std::snprintf (what, sizeof (what), "every ordered pair of voicings across mod_type on three held notes and a plucked take "
                   "(%d switches): worst %.2f dB above the higher steady level, %.2f dB below the lower, click %.1f dBFS; %d over",
                   pairs, worstAbove, worstBelow, worstClick, failing);
    ok &= expect (failing == 0, what);
    std::printf ("  (held notes against the two engines in lockstep, window by window: worst %.2f dB above the louder, %.2f dB "
                 "below the quieter)\n", lockAbove, lockBelow);
    return ok;
}

/** 7 / N14: one finite but absurd sample (1e6 or more) is silence at the input, before the
    trim, on every channel; and anything absurd made inside the chain is silenced at the output
    before the DC blocker, which recovers by the next block. Below 1e6 nothing is touched. */
bool runAbsurdSampleTests (double sampleRate, int blockSize)
{
    std::printf ("absurd samples:\n");
    bool ok = true;
    char what[300];

    const auto at = [sampleRate] (double seconds) { return (int) std::lround (seconds * sampleRate); };
    const auto chord = quieterChord (sampleRate, 1.5);
    const auto spikeAt = (size_t) at (0.3);

    const auto render = [&] (const std::vector<float>& l, const std::vector<float>& r,
                             const std::vector<std::pair<const char*, float>>& settings, const tubamp::chain::Order& order)
    {
        tubamp::TubampAudioProcessor fresh;

        for (const auto& [id, value] : settings)
            setFloatParam (fresh, id, value);

        fresh.setChainOrder (order, {});
        fresh.prepareToPlay (sampleRate, blockSize);
        return renderStereo (fresh, l, r, blockSize);
    };

    const auto nonFinite = [] (const StereoRender& y)
    {
        long count = 0;

        for (size_t i = 0; i < y.l.size(); ++i)
            count += (std::isfinite (y.l[i]) ? 0 : 1) + (std::isfinite (y.r[i]) ? 0 : 1);

        return count;
    };

    // At the input: 3e38 with the trim at +12 dB (which overflowed after the old sanitiser), and
    // exactly 1e6, on either channel; the render equals the chord with that sample zeroed.
    for (const bool empty : { true, false })
        for (const float spike : { 3.0e38f, -3.0e38f, 1.0e6f })
            for (const bool trim : { false, true })
            {
                auto bad = chord, zeroed = chord;
                bad[spikeAt] = spike;
                zeroed[spikeAt] = 0.0f;
                const std::vector<std::pair<const char*, float>> settings { { params::inputTrim, trim ? 12.0f : 0.0f },
                                                                            { params::modOn, 1.0f }, { params::modType, 1.0f },
                                                                            { params::modPhaserModel, 2.0f } };
                const auto order = empty ? tubamp::chain::Order {} : tubamp::chain::Order { BlockId::comp, BlockId::mod };
                const auto y = render (bad, chord, settings, order), z = render (zeroed, chord, settings, order);
                const auto y2 = render (chord, bad, settings, order), z2 = render (chord, zeroed, settings, order);
                const bool same = firstDifference (y.l, z.l) < 0 && firstDifference (y.r, z.r) < 0
                                  && firstDifference (y2.l, z2.l) < 0 && firstDifference (y2.r, z2.r) < 0;
                std::snprintf (what, sizeof (what), "%s: one input sample of %g, trim %s: %ld non-finite output samples, %s the "
                               "input with it zeroed", empty ? "empty chain" : "comp, Small Stone", (double) spike,
                               trim ? "+12 dB" : "0 dB", nonFinite (y) + nonFinite (y2), same ? "identical to" : "DIFFERENT from");
                ok &= expect (nonFinite (y) + nonFinite (y2) == 0 && same, what);
            }

    // Just below: passed on untouched (the render differs from the zeroed one, and is finite).
    {
        auto big = chord, zeroed = chord;
        big[spikeAt] = 9.9e5f;
        zeroed[spikeAt] = 0.0f;
        const auto y = render (big, big, {}, {}), z = render (zeroed, zeroed, {}, {});
        ok &= expect (nonFinite (y) == 0 && firstDifference (y.l, z.l) >= 0,
                      "an input sample of 9.9e5 is passed on (not zeroed) and the output stays finite");
    }

    // Made inside the chain: 9e5 passes the input rule, then +12 dB trim and 48 dB of makeup take
    // it to 1e9. Before the output stage zeroed it, the DC blocker rang at that size for seconds.
    {
        auto spiked = chord;
        spiked[spikeAt] = 9.0e5f;
        const std::vector<std::pair<const char*, float>> settings { { params::inputTrim, 12.0f }, { params::compOn, 1.0f },
                                                                    { params::compType, 0.0f }, { params::compMakeup, 48.0f },
                                                                    { params::compThreshold, 0.0f }, { params::compRatio, 1.0f } };
        // Ratio 1 makes the compressor memoryless, so a render whose input has that sample at 0 is
        // exactly what the output stage leaves: the DC blocker never saw the 1e9.
        auto zeroed = chord;
        zeroed[spikeAt] = 0.0f;
        const auto y = render (spiked, spiked, settings, { BlockId::comp }), z = render (zeroed, zeroed, settings, { BlockId::comp });
        const long first = firstDifference (y.l, z.l);

        std::snprintf (what, sizeof (what), "a value of ~1e9 made inside the chain: %ld non-finite output samples, peak %.3g; %s the "
                       "render with that sample silent (the DC blocker never rang)", nonFinite (y), (double) peakOf (y.l, 0, y.l.size()),
                       first < 0 ? "identical to" : "DIFFERENT from");
        ok &= expect (nonFinite (y) == 0 && first < 0 && firstDifference (y.r, z.r) < 0, what);
    }

    return ok;
}

/** 10: the output passes levels far above full scale unclipped and finite (a host handles float
    overs): the compressor's 48 dB of makeup and the phaser's Small Stone with Color on. */
bool runHeadroomTests (double sampleRate, int blockSize)
{
    std::printf ("headroom:\n");
    bool ok = true;
    char what[300];

    const auto at = [sampleRate] (double seconds) { return (int) std::lround (seconds * sampleRate); };
    const auto full = sineSignal (sampleRate, 196.0, 1.0f, at (1.0));

    struct Case { const char* label; tubamp::chain::Order order; std::vector<std::pair<const char*, float>> settings; float expectAbove; };
    const Case cases[] {
        { "compressor Classic, ratio 1, full makeup", { BlockId::comp },
          { { params::compOn, 1.0f }, { params::compType, 0.0f }, { params::compRatio, 1.0f }, { params::compThreshold, 0.0f },
            { params::compMakeup, 24.0f } }, 10.0f },
        { "phaser Small Stone, Color on", { BlockId::mod },
          { { params::modOn, 1.0f }, { params::modType, 1.0f }, { params::modPhaserModel, 2.0f }, { params::modColor, 1.0f } }, 1.0f } };

    for (const auto& c : cases)
    {
        tubamp::TubampAudioProcessor fresh;

        for (const auto& [id, value] : c.settings)
            setFloatParam (fresh, id, value);

        fresh.setChainOrder (c.order, {});
        fresh.prepareToPlay (sampleRate, blockSize);
        const auto y = renderStereo (fresh, full, full, blockSize);
        bool finite = true;

        for (const float v : y.l)
            finite &= std::isfinite (v);

        const float peak = peakOf (y.l, (size_t) at (0.5), y.l.size());
        std::snprintf (what, sizeof (what), "%s on a full-scale 196 Hz sine: output peak %+.1f dBFS, finite, not clipped", c.label,
                       toDb (peak));
        ok &= expect (finite && peak > c.expectAbove, what);
    }

    return ok;
}

} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: tubamp_smoke <model.nam> [sampleRate]\n");
        return 2;
    }

    const juce::ScopedJuceInitialiser_GUI juceInit;
    const juce::File modelFile { juce::String { argv[1] } };
    const double sampleRate = argc > 2 ? atof (argv[2]) : 48000.0;
    const int blockSize = 512;

    tubamp::TubampAudioProcessor proc;
    proc.prepareToPlay (sampleRate, blockSize);

    const double dryRms = processSineRms (proc, sampleRate, blockSize);

    const auto error = proc.loadModel (modelFile);

    if (error.isNotEmpty())
    {
        std::printf ("FAIL loadModel: %s\n", error.toRawUTF8());
        return 1;
    }

    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

    const double wetRms = processSineRms (proc, sampleRate, blockSize);
    const bool namActive = proc.namEngine.hasModel();
    const double delta = std::abs (wetRms - dryRms);

    const auto info = proc.namEngine.getModelInfo();

    std::printf ("sr=%.0f dryRMS=%.5f wetRMS=%.5f delta=%.5f hasModel=%d latency=%d\n",
                 sampleRate, dryRms, wetRms, delta, (int) namActive,
                 proc.namEngine.getLatencySamples());

    // Descriptive metadata: read out of the .nam directly (nam_core drops it), and
    // the reason the UI can flag a capture that already contains a cabinet.
    std::printf ("gearType=%s includesCab=%d\n",
                 info.gearType.isEmpty() ? "(none)" : info.gearType.toRawUTF8(),
                 (int) info.includesCab);

    const bool modelPass = namActive && delta > 1.0e-4;
    std::printf (modelPass ? "PASS: model audibly changes the signal\n"
                           : "FAIL: output did not change after model load\n");

    const bool parsePass = runParseTests();
    std::printf (parsePass ? "PASS: chain parsing is correct\n"
                           : "FAIL: chain parsing is wrong\n");

    const bool splitStructurePass = runSplitStructureTests();
    std::printf (splitStructurePass ? "PASS: split-path structure parses and sanitizes correctly\n"
                                    : "FAIL: split-path structure behaviour is wrong\n");

    const bool freshPass = runFreshInstanceTest (sampleRate, blockSize);
    std::printf (freshPass ? "PASS: a fresh instance starts with only the amp\n"
                           : "FAIL: a fresh instance does not start with only the amp\n");

    const bool chainPass = runChainOrderTests (proc, sampleRate, blockSize);
    std::printf (chainPass ? "PASS: chain order processes and round-trips\n"
                           : "FAIL: chain order behaviour is wrong\n");

    const bool instancePass = runInstanceModelTests (proc, sampleRate, blockSize);
    std::printf (instancePass ? "PASS: block instances process and round-trip\n"
                              : "FAIL: block instance behaviour is wrong\n");

    const bool ampEqPass = runAmpEqTests (proc, sampleRate, blockSize);
    std::printf (ampEqPass ? "PASS: amp EQ behaves correctly\n"
                           : "FAIL: amp EQ behaviour is wrong\n");

    const bool factoryPass = runFactoryPresetTests (sampleRate, blockSize);
    std::printf (factoryPass ? "PASS: factory presets carry a real chain order\n"
                             : "FAIL: factory preset behaviour is wrong\n");

    const bool concurrentPass = runConcurrentPackingTest();
    std::printf (concurrentPass ? "PASS: concurrent chain packing stays in bounds\n"
                                : "FAIL: concurrent chain packing went out of bounds\n");

    const bool fxPass = runFxSlotTests (proc, sampleRate, blockSize);
    std::printf (fxPass ? "PASS: empty fx slots are inert and round-trip\n"
                        : "FAIL: fx slot behaviour is wrong\n");

    const bool modelPathBPass = runModelPathBStateTests (proc, modelFile, sampleRate, blockSize);
    std::printf (modelPathBPass ? "PASS: modelPathB state round-trips correctly\n"
                                : "FAIL: modelPathB state behaviour is wrong\n");

    const bool delayModePass = runDelayModeTests (proc, sampleRate, blockSize);
    std::printf (delayModePass ? "PASS: stereo delay modes process and mode 0 matches the old path\n"
                               : "FAIL: stereo delay mode behaviour is wrong\n");

    const bool splitPathPass = runSplitPathTests (proc, sampleRate, blockSize);
    std::printf (splitPathPass ? "PASS: split-path processing is correct\n"
                               : "FAIL: split-path processing behaviour is wrong\n");

    const bool dualAmpLanePass = runDualAmpLaneTests (proc, modelFile, sampleRate, blockSize);
    std::printf (dualAmpLanePass ? "PASS: dual-NAM split lanes and fallback are correct\n"
                                 : "FAIL: dual-NAM split lane behaviour is wrong\n");

    const bool splitLatencyPass = runSplitLatencyTests (proc, modelFile, sampleRate, blockSize);
    std::printf (splitLatencyPass ? "PASS: split-path latency reporting and lane compensation are correct\n"
                                  : "FAIL: split-path latency behaviour is wrong\n");

    const bool splitStatePass = runSplitStateTests (proc, sampleRate, blockSize);
    std::printf (splitStatePass ? "PASS: split-path structure round-trips through state\n"
                                : "FAIL: split-path state behaviour is wrong\n");

    const bool engineStatePass = runEngineTypeStateTests (sampleRate, blockSize);
    std::printf (engineStatePass ? "PASS: engine type defaults and state round-trips are correct\n"
                                 : "FAIL: engine type state behaviour is wrong\n");

    const bool legacyPinPass = runLegacyPinTests (sampleRate, blockSize);
    std::printf (legacyPinPass ? "PASS: engine type ids pin to Classic for older state and round-trip otherwise\n"
                               : "FAIL: engine type legacy pin behaviour is wrong\n");

    const bool engineRenderPass = runEngineRenderTests (proc, sampleRate, blockSize);
    std::printf (engineRenderPass ? "PASS: every reverb/delay engine renders finite and decays to silence\n"
                                  : "FAIL: a reverb/delay engine render is wrong\n");

    const bool delaySyncPass = runDelaySyncTests (proc, sampleRate, blockSize);
    std::printf (delaySyncPass ? "PASS: delay tempo sync lands on the note length\n"
                               : "FAIL: delay tempo sync behaviour is wrong\n");

    const bool gateCompPass = runGateCompEngineTests (proc, sampleRate, blockSize);
    std::printf (gateCompPass ? "PASS: gate modes and compressor types run through the processor\n"
                              : "FAIL: gate or compressor engine behaviour is wrong\n");

    const bool gateAlignPass = runGateAlignmentTests (proc, sampleRate, blockSize);
    std::printf (gateAlignPass ? "PASS: the deferred gate curve lines up with the resampled amp\n"
                               : "FAIL: the deferred gate curve is misaligned\n");

    const bool dynRtPass = runDynamicsRtTests (proc, sampleRate, blockSize);
    std::printf (dynRtPass ? "PASS: the gate and compressor path allocates nothing\n"
                           : "FAIL: the gate and compressor path allocates\n");

    const bool eqDriveEnginePass = runEqDriveEngineTests (sampleRate, blockSize);
    std::printf (eqDriveEnginePass ? "PASS: EQ and drive types run through the processor as the engines do\n"
                                   : "FAIL: EQ or drive engine behaviour through the processor is wrong\n");

    const bool driveLatencyPass = runDriveLatencyTests (sampleRate, blockSize);
    std::printf (driveLatencyPass ? "PASS: drive latency is reported, measured and compensated\n"
                                  : "FAIL: drive latency behaviour is wrong\n");

    const bool eqDriveInstancePass = runEqDriveInstanceTests (sampleRate, blockSize);
    std::printf (eqDriveInstancePass ? "PASS: EQ and drive instances are independent\n"
                                     : "FAIL: EQ or drive instances interfere\n");

    const bool eqDriveRtPass = runEqDriveRtTests (proc, sampleRate, blockSize);
    std::printf (eqDriveRtPass ? "PASS: the EQ and drive path allocates nothing\n"
                               : "FAIL: the EQ and drive path allocates\n");

    const bool modEnginePass = runModEngineTests (sampleRate, blockSize);
    std::printf (modEnginePass ? "PASS: mod voicings, tempo sync and instances run through the processor as the engines do\n"
                               : "FAIL: mod engine behaviour through the processor is wrong\n");

    const bool modRtPass = runModRtTests (proc, sampleRate, blockSize);
    std::printf (modRtPass ? "PASS: the mod path allocates nothing\n"
                           : "FAIL: the mod path allocates\n");

    const bool nonFinitePass = runNonFiniteInputTests (sampleRate, blockSize);
    std::printf (nonFinitePass ? "PASS: non-finite input samples never reach the output\n"
                               : "FAIL: non-finite input reaches the output\n");

    const bool togglePass = runBlockToggleTests (sampleRate, blockSize);
    std::printf (togglePass ? "PASS: non-Classic blocks switch on and off without a pop or a replay\n"
                            : "FAIL: a block pops or replays when switched\n");

    const bool modSwitchPass = runModSwitchTests (sampleRate, blockSize);
    std::printf (modSwitchPass ? "PASS: mod_type switches do not tick and sync hands the engines every jump\n"
                               : "FAIL: mod switch or sync behaviour is wrong\n");

    const bool laneChangePass = runLaneLatencyChangeTests (sampleRate, blockSize);
    std::printf (laneChangePass ? "PASS: a latency change in one lane leaves the other lane whole\n"
                                : "FAIL: a latency change in one lane breaks the other\n");

    const bool accountingPass = runLatencyAccountingTests (blockSize);
    std::printf (accountingPass ? "PASS: reported latency is the delay applied\n"
                                : "FAIL: reported latency differs from the delay applied\n");

    const bool freshInstallPass = runFreshInstallTests (sampleRate, blockSize);
    std::printf (freshInstallPass ? "PASS: a fresh install writes its factory presets without touching the live state\n"
                                  : "FAIL: writing the factory presets changes the live state\n");

    const bool startingPointPass = runStartingPointTests();
    std::printf (startingPointPass ? "PASS: per-type starting points come from the engine headers\n"
                                   : "FAIL: per-type starting points are wrong\n");

    const bool latencyThreadPass = runLatencyThreadTests (sampleRate, blockSize);
    std::printf (latencyThreadPass ? "PASS: latency is reported from the message thread only, and follows changes with no audio running\n"
                                   : "FAIL: latency reporting threads or staleness are wrong\n");

    const bool bypassedTypePass = runBypassedDriveTypeTests (sampleRate, blockSize);
    std::printf (bypassedTypePass ? "PASS: a bypassed drive's type change slides its delay without a click\n"
                                  : "FAIL: a bypassed drive's type change clicks\n");

    const bool laneRetargetPass = runLaneRetargetTests (modelFile, sampleRate);
    std::printf (laneRetargetPass ? "PASS: lane latency changes in quick succession move the waiting lane smoothly\n"
                                  : "FAIL: lane latency changes in quick succession step the waiting lane\n");

    const bool driveLanePass = runDriveSwitchLaneTests (sampleRate);
    std::printf (driveLanePass ? "PASS: a drive type switch in a split keeps the lanes in line\n"
                               : "FAIL: a drive type switch in a split misaligns the lanes\n");

    const bool modLevelPass = runModTypeLevelTests (sampleRate);
    std::printf (modLevelPass ? "PASS: mod_type switches keep the level between the two types' and do not click\n"
                              : "FAIL: mod_type switches dip, bump or click\n");

    const bool absurdPass = runAbsurdSampleTests (sampleRate, blockSize);
    std::printf (absurdPass ? "PASS: absurd samples at the input or made inside the chain never reach the host\n"
                            : "FAIL: an absurd sample reaches the host or latches a state\n");

    const bool headroomPass = runHeadroomTests (sampleRate, blockSize);
    std::printf (headroomPass ? "PASS: levels above full scale leave the plugin unclipped and finite\n"
                              : "FAIL: levels above full scale are clipped or not finite\n");

    return modelPass && parsePass && splitStructurePass && freshPass && chainPass && instancePass
                   && ampEqPass && factoryPass && concurrentPass && fxPass
                   && modelPathBPass && delayModePass
                   && splitPathPass && dualAmpLanePass && splitLatencyPass && splitStatePass
                   && engineStatePass && legacyPinPass && engineRenderPass && delaySyncPass
                   && gateCompPass && gateAlignPass && dynRtPass
                   && eqDriveEnginePass && driveLatencyPass && eqDriveInstancePass && eqDriveRtPass
                   && modEnginePass && modRtPass
                   && nonFinitePass && togglePass && modSwitchPass && laneChangePass && accountingPass
                   && freshInstallPass && startingPointPass
                   && latencyThreadPass && bypassedTypePass && laneRetargetPass && driveLanePass && modLevelPass
                   && absurdPass && headroomPass
               ? 0
               : 1;
}
