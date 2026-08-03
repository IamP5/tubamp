// End-to-end smoke test: drives the real TubampAudioProcessor (no editor) and
// verifies that loading a NAM model actually changes the processed audio, and that
// a user-arranged chain order processes cleanly and survives state round-trips.
//   tubamp_smoke <model.nam> [sampleRate]
#include <juce_events/juce_events.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "../src/PluginProcessor.h"

#include <cmath>
#include <cstdio>

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

    return modelPass && chainPass ? 0 : 1;
}
