// fxbench adapter for the phaser voicings of the mod block (docs/FX-ENGINES.md §4.6).
// Keys: model rate depth mix color sync_phase on (0 skips process()). The legacy reference is the old
// Modulation block running its phaser.
#include "FxBench.h"
#include "legacy/LegacyFx.h"

#include "../../src/dsp/Phaser.h"

namespace
{
class LegacyModBench : public fxbench::Effect
{
public:
    const char* typeKey() const override { return "model"; }

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

        modulation.setParameters (tubamp::legacy::Modulation::Type::phaser, (float) a.get ("rate", 1.0),
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

class PhaserBench : public fxbench::Effect
{
public:
    const char* typeKey() const override { return "model"; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override
    {
        phaser.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
    }

    void reset() override { phaser.reset(); }

    void setParams (const fxbench::Args& a) override
    {
        // Like the processor: a bypassed block is neither updated nor run.
        on = a.get ("on", 1.0) > 0.5;

        if (! on)
            return;

        tubamp::PhaserParams p;
        p.model = (int) a.get ("model", p.model);
        p.rateHz = (float) a.get ("rate", p.rateHz);
        p.depth = (float) a.get ("depth", p.depth);
        p.mix = (float) a.get ("mix", p.mix);
        p.color = a.get ("color", p.color ? 1.0 : 0.0) > 0.5;
        p.syncPhase = a.get ("sync_phase", p.syncPhase);
        phaser.setParameters (p);
    }

    void process (juce::AudioBuffer<float>& io, int) override
    {
        if (on)
            phaser.process (juce::dsp::AudioBlock<float> (io));
    }

    std::unique_ptr<fxbench::Effect> makeLegacy() const override { return std::make_unique<LegacyModBench>(); }

private:
    tubamp::PhaserFx phaser;
    bool on = true;
};

FXBENCH_REGISTER ("phaser", PhaserBench,
                  "model(0 Classic,1 Phase 90,2 Small Stone) rate depth mix color(0/1) sync_phase on(0/1)");
} // namespace
