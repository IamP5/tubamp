// End-to-end smoke test: drives the real TubampAudioProcessor (no editor) and
// verifies that loading a NAM model actually changes the processed audio, and that
// a user-arranged chain order (including duplicated block instances and rows)
// processes cleanly and survives state round-trips.
//   tubamp_smoke <model.nam> [sampleRate]
#include <juce_events/juce_events.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "../src/PluginProcessor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <vector>

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

    setBoolParam (proc, params::ampEqOn, false);
    const auto bypassed = renderChain (proc, ampOnly, sampleRate, blockSize); // bass still 10, but off

    setFloatParam (proc, params::ampEqBass, 5.0f);
    setBoolParam (proc, params::ampEqOn, true);
    const auto flatAgain = renderChain (proc, ampOnly, sampleRate, blockSize);

    // Off and on-at-flat are close but NOT bit-exact: the biquad's coefficients at
    // "flat" settings do not collapse to an exact identity filter (review note).
    double maxAbsDiff = 0.0;
    for (size_t i = 0; i < std::min (bypassed.size(), flatAgain.size()); ++i)
        maxAbsDiff = std::max (maxAbsDiff, (double) std::abs (bypassed[i] - flatAgain[i]));

    std::printf ("  amp eq off vs on-at-flat: max abs diff = %.6g\n", maxAbsDiff);
    ok &= expect (maxAbsDiff < 0.02, "amp EQ off and on-at-flat are within tolerance of each other");

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

        stereoDelay.setParameters (280.0f, 0.4f, 0.35f, 0 /* Stereo */, 100.0f, 1.0f);
        monoDelayL.setParameters (280.0f, 0.4f, 0.35f, 0, 100.0f, 1.0f);
        monoDelayR.setParameters (280.0f, 0.4f, 0.35f, 0, 100.0f, 1.0f);

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
        delayFx.setParameters (2000.0f, 0.0f, 1.0f, 2 /* Dual */, 200.0f, 1.0f);

        // The delay length is smoothed over 150 ms from its prepared value; let it
        // settle on silence before the impulse so the echo lands at the target time.
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

    return modelPass && parsePass && splitStructurePass && freshPass && chainPass && instancePass
                   && ampEqPass && factoryPass && concurrentPass && fxPass
                   && modelPathBPass && delayModePass
                   && splitPathPass && dualAmpLanePass && splitLatencyPass && splitStatePass
               ? 0
               : 1;
}
