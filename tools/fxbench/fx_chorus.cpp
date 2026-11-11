// fxbench adapter for the chorus voicings of the mod block (docs/FX-ENGINES.md §4.5).
// Keys: mode rate depth mix width tone sync_phase solo on (0 skips process()) reset (a change of value calls
// reset() before that block's parameters, as the processor does when a bypassed block comes back). The legacy
// reference is the old Modulation block running its chorus.
#include "FxBench.h"
#include "legacy/LegacyFx.h"

#include "../../src/dsp/Chorus.h"

namespace
{
class LegacyModBench : public fxbench::Effect
{
public:
    const char* typeKey() const override { return "mode"; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override
    {
        modulation.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
    }

    void reset() override { modulation.reset(); }

    void setParams (const fxbench::Args& a) override
    {
        // Like the processor: a bypassed block is neither updated nor run.
        on = a.get ("on", 1.0) > 0.5;

        if (! on)
            return;

        modulation.setParameters (tubamp::legacy::Modulation::Type::chorus, (float) a.get ("rate", 1.0),
                                  (float) a.get ("depth", 0.4), (float) a.get ("mix", 0.35));
    }

    void process (juce::AudioBuffer<float>& io, int) override
    {
        if (on)
            modulation.process (juce::dsp::AudioBlock<float> (io));
    }

private:
    tubamp::legacy::Modulation modulation;
    bool on = true;
};

class ChorusBench : public fxbench::Effect
{
public:
    const char* typeKey() const override { return "mode"; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override
    {
        chorus.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
    }

    void reset() override
    {
        chorus.reset();
        lastReset = 0.0;
    }

    void setParams (const fxbench::Args& a) override
    {
        // Like the processor: a bypassed block is neither updated nor run.
        on = a.get ("on", 1.0) > 0.5;

        if (! on)
            return;

        const double resetKey = a.get ("reset", 0.0);

        if (! juce::exactlyEqual (resetKey, lastReset))
        {
            lastReset = resetKey;
            chorus.reset();
        }

        tubamp::ChorusParams p;
        p.mode = (int) a.get ("mode", p.mode);
        p.rateHz = (float) a.get ("rate", p.rateHz);
        p.depth = (float) a.get ("depth", p.depth);
        p.mix = (float) a.get ("mix", p.mix);
        p.width = (float) a.get ("width", p.width);
        p.tone = (float) a.get ("tone", p.tone);
        p.syncPhase = a.get ("sync_phase", p.syncPhase);
        chorus.setParameters (p);
        chorus.setDebugSolo ((int) a.get ("solo", -1.0));
    }

    void process (juce::AudioBuffer<float>& io, int) override
    {
        if (on)
            chorus.process (juce::dsp::AudioBlock<float> (io));
    }

    std::unique_ptr<fxbench::Effect> makeLegacy() const override { return std::make_unique<LegacyModBench>(); }

private:
    tubamp::ChorusFx chorus;
    double lastReset = 0.0;
    bool on = true;
};

FXBENCH_REGISTER ("chorus", ChorusBench,
                  "mode(0 Classic,1 Wide,2 CE,3 Dimension,4 Tri) rate depth mix width tone sync_phase solo(-1 = off) on(0/1) "
                  "reset(a change calls reset())");
} // namespace
