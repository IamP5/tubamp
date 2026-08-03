// End-to-end smoke test: drives the real TubampAudioProcessor (no editor) and
// verifies that loading a NAM model actually changes the processed audio, and that
// a user-arranged chain order processes cleanly and survives state round-trips.
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

void setBoolParam (tubamp::TubampAudioProcessor& proc, const char* paramId, bool value)
{
    if (auto* p = proc.apvts.getParameter (paramId))
        p->setValueNotifyingHost (value ? 1.0f : 0.0f);
}

/** Chain order: publish -> process -> save/restore, including legacy state that
    predates the chainOrder property. */
bool runChainOrderTests (tubamp::TubampAudioProcessor& proc, double sampleRate, int blockSize)
{
    std::printf ("chain order:\n");

    const auto defaultOrder = tubamp::chain::toString (tubamp::chain::defaultOrder());
    bool ok = true;

    // Drive ahead of the compressor, reverb ahead of the delay, gate removed entirely.
    const tubamp::chain::Order custom { BlockId::drive, BlockId::comp, BlockId::amp,
                                        BlockId::cab,   BlockId::eq,   BlockId::mod,
                                        BlockId::reverb, BlockId::delay };
    const auto customText = tubamp::chain::toString (custom);

    int changeNotifications = 0;
    proc.onChainChanged = [&changeNotifications] { ++changeNotifications; };

    proc.setChainOrder (custom);
    ok &= expect (changeNotifications == 1, "setChainOrder fires onChainChanged");
    ok &= expect (tubamp::chain::toString (proc.getChainOrder()) == customText,
                  "getChainOrder reflects the published order");

    setBoolParam (proc, params::driveOn, true); // off by default; exercise the moved block

    bool finite = true;
    const double rms = processSineRms (proc, sampleRate, blockSize, &finite);
    ok &= expect (finite, "custom order produces only finite samples");
    ok &= expect (std::isfinite (rms) && rms > 1.0e-5, "custom order produces non-silent output");

    // --- binary state round-trip (host save/load)
    juce::MemoryBlock blob;
    proc.getStateInformation (blob);

    proc.setChainOrder (tubamp::chain::defaultOrder());
    changeNotifications = 0;
    proc.setStateInformation (blob.getData(), (int) blob.getSize());
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

    ok &= expect (tubamp::chain::toString (proc.getChainOrder()) == customText,
                  "order survives getStateInformation -> setStateInformation");
    ok &= expect (changeNotifications >= 1, "state restore fires onChainChanged");

    // --- legacy state: same blob with the property stripped out
    if (auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), (int) blob.getSize()))
    {
        xml->removeAttribute ("chainOrder");

        juce::MemoryBlock legacyBlob;
        juce::AudioProcessor::copyXmlToBinary (*xml, legacyBlob);

        proc.setChainOrder (custom);
        proc.setStateInformation (legacyBlob.getData(), (int) legacyBlob.getSize());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        ok &= expect (tubamp::chain::toString (proc.getChainOrder()) == defaultOrder,
                      "legacy state without chainOrder restores the default order");
    }
    else
    {
        ok &= expect (false, "legacy state blob could not be rebuilt");
    }

    // --- preset / A-B slot round-trip (PresetManager goes through these two)
    proc.setChainOrder (custom);
    const auto slotState = proc.captureStateVar();

    proc.setChainOrder (tubamp::chain::defaultOrder());
    proc.applyStateVar (slotState);
    ok &= expect (tubamp::chain::toString (proc.getChainOrder()) == customText,
                  "order survives captureStateVar -> applyStateVar");

    if (auto* obj = slotState.getDynamicObject())
    {
        obj->removeProperty ("chainOrder"); // a preset saved before the chain was arrangeable
        proc.setChainOrder (custom);
        proc.applyStateVar (slotState);

        ok &= expect (tubamp::chain::toString (proc.getChainOrder()) == defaultOrder,
                      "legacy preset without chainOrder restores the default order");
    }

    // --- a deliberately-empty chain must round-trip, not revert to the default
    proc.setChainOrder ({});
    const auto emptyState = proc.captureStateVar();

    proc.setChainOrder (custom);
    proc.applyStateVar (emptyState);
    ok &= expect (proc.getChainOrder().empty(),
                  "empty chain survives captureStateVar -> applyStateVar");

    proc.onChainChanged = nullptr;
    proc.setChainOrder (tubamp::chain::defaultOrder());
    setBoolParam (proc, params::driveOn, false);

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
    proc.setChainOrder (order);
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

            if (xml == nullptr || ! xml->hasTagName ("TUBAMP") || ! xml->hasAttribute ("chainOrder")
                || tubamp::chain::fromString (xml->getStringAttribute ("chainOrder")).empty())
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

    ok &= expect ((int) withFx.size() == chain::numBlockTypes,
                  "the fx test order uses every one of the 12 block types");

    const auto packed = chain::pack (withFx);
    std::array<BlockId, chain::numBlockTypes> decoded {};
    const int decodedCount = chain::unpackTo (packed, decoded);

    ok &= expect (decodedCount == (int) withFx.size()
                      && std::equal (withFx.begin(), withFx.end(), decoded.begin()),
                  "12-entry order with fx slots survives pack -> unpackTo");
    ok &= expect (chain::unpack (packed) == withFx, "... and pack -> unpack");

    const auto withFxText = chain::toString (withFx);
    ok &= expect (withFxText.contains ("fx1") && withFxText.contains ("fx2")
                      && withFxText.contains ("fx3"),
                  "toString emits the fx1/fx2/fx3 tokens");
    ok &= expect (chain::fromString (withFxText) == withFx,
                  "12-entry order with fx slots survives toString -> fromString");

    // --- 2. the legacy-state contract: no fx tokens anywhere near the default
    bool defaultHasFx = false;

    for (auto id : chain::defaultOrder())
        defaultHasFx |= chain::isFxSlot (id);

    ok &= expect (! defaultHasFx, "defaultOrder() contains no fx slots");
    ok &= expect (chain::defaultOrder() == withoutFx,
                  "defaultOrder() is exactly the nine built-in blocks");
    ok &= expect (chain::fromString ("") == withoutFx,
                  "fromString(\"\") still falls back to the nine built-in blocks");

    // --- 3. an id this build does not know must be DROPPED, never clamped onto fx3
    {
        chain::Order bogus { BlockId::gate, (BlockId) 99, BlockId::amp };
        proc.setChainOrder (bogus);

        const auto sanitised = proc.getChainOrder();
        bool anyFx = false;

        for (auto id : sanitised)
            anyFx |= chain::isFxSlot (id);

        ok &= expect (sanitised == chain::Order { BlockId::gate, BlockId::amp },
                      "sanitiseOrder drops an out-of-range id");
        ok &= expect (! anyFx, "... rather than clamping it onto fx3");
    }

    // --- 5. latency (checked before the renders, which call prepareToPlay)
    proc.setChainOrder (withoutFx);
    const int latencyWithoutFx = proc.getLatencySamples();
    proc.setChainOrder (withFx);
    const int latencyWithFx = proc.getLatencySamples();

    std::printf ("  latency: withoutFx=%d withFx=%d\n", latencyWithoutFx, latencyWithFx);
    ok &= expect (latencyWithFx == latencyWithoutFx,
                  "empty fx slots do not change the reported latency");

    // Repeat at a rate the model has to be resampled to, so the baseline latency is
    // non-zero and the comparison above is not just 0 == 0.
    {
        const double otherRate = sampleRate == 44100.0 ? 48000.0 : 44100.0;
        proc.prepareToPlay (otherRate, blockSize);

        proc.setChainOrder (withoutFx);
        const int resampledWithoutFx = proc.getLatencySamples();
        proc.setChainOrder (withFx);
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
        proc.setChainOrder (withFx);
        proc.prepareToPlay (sampleRate, blockSize);

        bool finite = true;
        const double rms = processSineRms (proc, sampleRate, blockSize, &finite);

        ok &= expect (finite && std::isfinite (rms) && rms > 1.0e-5,
                      "the full 12-block chain incl. fx slots produces finite, non-silent audio");
    }

    // --- 6. state round-trips carrying fx tokens in the order
    const auto withFxText2 = chain::toString (withFx);
    proc.setChainOrder (withFx);

    juce::MemoryBlock blob;
    proc.getStateInformation (blob);

    proc.setChainOrder (chain::defaultOrder());
    proc.setStateInformation (blob.getData(), (int) blob.getSize());
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

    ok &= expect (chain::toString (proc.getChainOrder()) == withFxText2,
                  "fx tokens survive getStateInformation -> setStateInformation");

    proc.setChainOrder (withFx);
    const auto slotState = proc.captureStateVar();

    proc.setChainOrder (chain::defaultOrder());
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

        proc.setChainOrder (chain::defaultOrder());
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

        proc.setChainOrder (chain::defaultOrder());
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
        proc.setChainOrder (withFx);
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

    proc.setChainOrder (chain::defaultOrder());
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

    const bool chainPass = runChainOrderTests (proc, sampleRate, blockSize);
    std::printf (chainPass ? "PASS: chain order processes and round-trips\n"
                           : "FAIL: chain order behaviour is wrong\n");

    const bool fxPass = runFxSlotTests (proc, sampleRate, blockSize);
    std::printf (fxPass ? "PASS: empty fx slots are inert and round-trip\n"
                        : "FAIL: fx slot behaviour is wrong\n");

    return modelPass && chainPass && fxPass ? 0 : 1;
}
