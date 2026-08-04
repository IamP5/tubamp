// Headless end-to-end check of the external-AU slot machinery: enumerate installed
// effects, load one into a real TubampAudioProcessor's chain, run audio through it and
// verify that it is actually in the path, that state round-trips, and that clearing it
// restores bit-exact pass-through.
//
//   tubamp_fxprobe                 list installed AUv2 effects
//   tubamp_fxprobe <identifier>    load that plugin into slot 0 and exercise it
//   tubamp_fxprobe --all           try every installed effect (slow; finds bad actors)
//   tubamp_fxprobe --editors       report each plugin's own editor size
//
// This is the only automated coverage of the code paths that need a real AudioUnit;
// tubamp_smoke deliberately builds without hosting.
#include <juce_events/juce_events.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "../src/PluginProcessor.h"

#include <cmath>
#include <cstdio>

namespace
{
using tubamp::chain::BlockId;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;

/** Every measured render is this many blocks, and the test tone completes a whole
    number of cycles in exactly that span. Both properties are needed for two renders
    to be comparable sample-by-sample: continuous phase alone would have consecutive
    windows landing on different parts of the cycle, and a per-render phase reset would
    ring the always-on DC blocker at every boundary. 47 cycles per window puts the tone
    at 220.3125 Hz — near enough to the 220 Hz the other tests use. */
constexpr int kRenderBlocks = 20;
constexpr int kWindowSamples = kRenderBlocks * kBlockSize;
constexpr double kToneHz = kSampleRate * 47.0 / (double) kWindowSamples;

int failures = 0;

bool expect (bool condition, const char* what)
{
    std::printf ("  %s  %s\n", condition ? "ok  " : "FAIL", what);

    if (! condition)
        ++failures;

    return condition;
}

/** Continuous across every render() in a run — see kToneHz. */
juce::int64 sinePhase = 0;

void fillSine (juce::AudioBuffer<float>& buffer)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* data = buffer.getWritePointer (ch);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
            data[i] = 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * kToneHz
                                                * (double) (sinePhase + i) / kSampleRate);
    }

    sinePhase += buffer.getNumSamples();
}

/** Runs `numBlocks` of sine through the processor, returning the concatenated output
    of channel 0 and clearing `allFinite` if anything non-finite appears. */
std::vector<float> render (tubamp::TubampAudioProcessor& proc, int numBlocks, bool& allFinite)
{
    juce::AudioBuffer<float> buffer (2, kBlockSize);
    juce::MidiBuffer midi;
    std::vector<float> out;
    out.reserve ((size_t) (numBlocks * kBlockSize));

    for (int block = 0; block < numBlocks; ++block)
    {
        fillSine (buffer);
        proc.processBlock (buffer, midi);

        for (int i = 0; i < kBlockSize; ++i)
        {
            const float sample = buffer.getReadPointer (0)[i];

            if (! std::isfinite (sample))
                allFinite = false;

            out.push_back (sample);
        }
    }

    return out;
}

double rms (const std::vector<float>& samples, size_t skip)
{
    double sum = 0.0;
    size_t count = 0;

    for (size_t i = skip; i < samples.size(); ++i, ++count)
        sum += (double) samples[i] * samples[i];

    return count > 0 ? std::sqrt (sum / (double) count) : 0.0;
}

/** Pumps the message loop until `done` or the timeout expires. Plugin instantiation is
    asynchronous and completes on the message thread, which a console app has to run
    by hand. */
bool pump (const std::function<bool()>& done, int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

    while (! done())
    {
        if (juce::Time::getMillisecondCounter() > deadline)
            return false;

        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    }

    return true;
}

/** Creates each plugin's own editor and reports the size it asks for. This answers one
    question: could a hosted editor be embedded in tubamp's fixed 1120x700 window
    instead of a separate one? The dock area available for it is 1120 x 596 (the 48 px
    header and 56 px footer are fixed). */
void measureEditors (tubamp::FxCatalog& catalog)
{
    constexpr int kAvailableW = 1120, kAvailableH = 596;

    const auto entries = catalog.enumerateEffects();
    int measured = 0, fits = 0, needsScroll = 0, noEditor = 0;

    for (const auto& entry : entries)
    {
        std::unique_ptr<juce::AudioProcessor> instance;
        bool done = false;

        catalog.createAsync (catalog.descriptionFor (entry.identifier), kSampleRate, kBlockSize,
                             [&] (std::unique_ptr<juce::AudioProcessor> created, const juce::String&)
                             {
                                 instance = std::move (created);
                                 done = true;
                             });

        if (! pump ([&done] { return done; }, 15000) || instance == nullptr)
            continue;

        if (! instance->hasEditor())
        {
            ++noEditor;
            std::printf ("  %-38s (no editor of its own)\n", entry.name.toRawUTF8());
            instance.reset();
            continue;
        }

        if (auto* editor = instance->createEditorIfNeeded())
        {
            const int w = editor->getWidth(), h = editor->getHeight();
            const bool fitsHere = w <= kAvailableW && h <= kAvailableH;

            ++measured;

            if (fitsHere)
                ++fits;
            else
                ++needsScroll;

            std::printf ("  %-38s %4d x %4d  %s%s\n", entry.name.toRawUTF8(), w, h,
                         fitsHere ? "fits" : "TOO BIG",
                         editor->isResizable() ? "  (resizable)" : "");

            instance->editorBeingDeleted (editor);
            delete editor;
        }

        instance.reset();
    }

    std::printf ("\nmeasured=%d  fit in %dx%d=%d  too big=%d  no editor=%d\n",
                 measured, kAvailableW, kAvailableH, fits, needsScroll, noEditor);
}

void listPlugins (const tubamp::FxCatalog& catalog)
{
    const auto entries = catalog.enumerateEffects();
    std::printf ("%d installed AUv2 effects\n\n", (int) entries.size());

    for (const auto& entry : entries)
        std::printf ("  %-52s %s - %s\n", entry.identifier.toRawUTF8(),
                     entry.manufacturer.toRawUTF8(), entry.name.toRawUTF8());
}

bool probe (const juce::String& identifier, bool verbose)
{
    tubamp::TubampAudioProcessor proc;
    proc.setPlayConfigDetails (2, 2, kSampleRate, kBlockSize);
    proc.prepareToPlay (kSampleRate, kBlockSize);

    // A chain of nothing but the slot: whatever changes in the output is the plugin.
    proc.setChainOrder ({ BlockId::fx1 }, {});

    bool finite = true;

    // Warm the always-on DC blocker before taking the reference. It is a very
    // narrow highpass, so its settling time is far longer than a few blocks, and a
    // reference taken cold would differ from any later render for reasons that have
    // nothing to do with the slot under test.
    render (proc, kRenderBlocks * 10, finite);
    const auto dry = render (proc, kRenderBlocks, finite);

    // Control: two consecutive renders of the same settled chain. Anything the
    // pass-through check below reports must be larger than this, or it is measuring
    // the processor's own residual drift rather than the slot.
    const auto dryAgain = render (proc, kRenderBlocks, finite);
    double control = 0.0;

    for (size_t i = dry.size() / 2; i < dry.size(); ++i)
        control = juce::jmax (control, std::abs ((double) dry[i] - (double) dryAgain[i]));

    if (verbose)
        std::printf ("  control (dry vs dry) deviation = %g\n", control);

    proc.loadFxPlugin (0, identifier);

    const bool settled = pump ([&proc] { return ! proc.getFxSlotInfo (0).loading; }, 20000);

    const auto info = proc.getFxSlotInfo (0);

    if (! settled)
    {
        std::printf ("  FAIL  timed out loading %s\n", identifier.toRawUTF8());
        ++failures;
        return false;
    }

    if (! info.live)
    {
        std::printf ("  skip  %s (%s)\n", identifier.toRawUTF8(),
                     info.error.isNotEmpty() ? info.error.toRawUTF8() : "not live");
        return false;
    }

    if (verbose)
    {
        std::printf ("  loaded: %s - %s  latency=%d params=%d\n",
                     info.manufacturer.toRawUTF8(), info.name.toRawUTF8(),
                     info.latencySamples,
                     proc.getFxInstance (0) != nullptr ? proc.getFxInstance (0)->getParameters().size() : 0);
    }

    const auto wet = render (proc, kRenderBlocks * 2, finite);
    expect (finite, "hosted plugin produces only finite samples");
    expect (rms (wet, wet.size() / 2) > 0.0, "hosted plugin produces non-silent output");

    // Latency is reported while loaded regardless of the bypass parameter, so that
    // automating the bypass never moves the plugin's reported latency.
    const int loadedLatency = proc.getLatencySamples();

    if (auto* bypass = proc.apvts.getParameter ("fx1_on"))
    {
        bypass->setValueNotifyingHost (0.0f);
        bool stillFinite = true;
        render (proc, kRenderBlocks, stillFinite);
        expect (proc.getLatencySamples() == loadedLatency,
                "reported latency is unchanged by toggling the slot's bypass");
        expect (stillFinite, "bypassed slot produces only finite samples");
        bypass->setValueNotifyingHost (1.0f);
    }

    // State round-trip: the plugin and its settings must survive a save/restore.
    juce::MemoryBlock saved;
    proc.getStateInformation (saved);
    proc.clearFxPlugin (0);
    expect (! proc.getFxSlotInfo (0).occupied, "clearing the slot empties it");

    proc.setStateInformation (saved.getData(), (int) saved.getSize());
    pump ([&proc] { const auto s = proc.getFxSlotInfo (0); return s.occupied && ! s.loading; }, 20000);
    expect (proc.getFxSlotInfo (0).identifier == identifier,
            "the plugin is restored from saved state");

    // An emptied slot must put the chain back where it started. This compares the
    // settled tail with a tolerance rather than demanding bit-equality: the always-on
    // DC blocker is an IIR filter whose state has been driven by the plugin's output
    // in between, so the two runs only reconverge once it has settled. The strict
    // bit-exactness claim — that an empty slot never touches the buffer at all — is
    // tested properly in tubamp_smoke, where both runs share one warm processor.
    proc.clearFxPlugin (0);
    bool passFinite = true;
    const auto passthrough = render (proc, kRenderBlocks, passFinite);

    bool matches = passthrough.size() == dry.size();
    double worst = 0.0;

    for (size_t i = dry.size() / 2; matches && i < dry.size(); ++i)
        worst = juce::jmax (worst, std::abs ((double) dry[i] - (double) passthrough[i]));

    matches = matches && worst < 1.0e-6;

    if (! matches && verbose)
        std::printf ("        worst tail deviation = %g\n", worst);

    expect (matches, "an emptied slot returns the chain to dry (settled tail)");

    proc.releaseResources();
    return true;
}
} // namespace

int main (int argc, char* argv[])
{
    // Unbuffered: a hosted plugin can take the whole process down, and a buffered line
    // would take the name of the plugin that did it with them.
    std::setvbuf (stdout, nullptr, _IONBF, 0);

    juce::ScopedJuceInitialiser_GUI juceInit;

    tubamp::FxCatalog catalog;

    if (! tubamp::FxCatalog::isSupported())
    {
        std::printf ("This build has no plugin hosting support.\n");
        return 1;
    }

    if (argc < 2)
    {
        listPlugins (catalog);
        return 0;
    }

    const juce::String arg { argv[1] };

    if (arg == "--editors")
    {
        measureEditors (catalog);
        return 0;
    }

    if (arg == "--all")
    {
        const auto entries = catalog.enumerateEffects();
        int loaded = 0;

        for (const auto& entry : entries)
        {
            std::printf ("\n%s (%s)\n", entry.name.toRawUTF8(), entry.manufacturer.toRawUTF8());

            if (probe (entry.identifier, false))
                ++loaded;
        }

        std::printf ("\n%d/%d plugins loaded and processed\n", loaded, (int) entries.size());
    }
    else
    {
        probe (arg, true);
    }

    std::printf ("\n%s\n", failures == 0 ? "PASS: hosted AU slots behave" : "FAILURES ABOVE");
    return failures == 0 ? 0 : 1;
}
