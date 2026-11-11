// fxbench adapter for the tremolo voicings of the mod block (docs/FX-ENGINES.md §4.7).
// Keys: voice rate depth mix width tone shape wave sync_phase on (0 skips process()). The legacy reference is
// the old Modulation block running its tremolo.
#include "FxBench.h"
#include "legacy/LegacyFx.h"

#include "../../src/dsp/Tremolo.h"

namespace
{
class LegacyModBench : public fxbench::Effect
{
public:
    const char* typeKey() const override { return "voice"; }

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

        modulation.setParameters (tubamp::legacy::Modulation::Type::tremolo, (float) a.get ("rate", 1.0),
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

class TremoloBench : public fxbench::Effect
{
public:
    const char* typeKey() const override { return "voice"; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override
    {
        tremolo.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
    }

    void reset() override { tremolo.reset(); }

    void setParams (const fxbench::Args& a) override
    {
        // Like the processor: a bypassed block is neither updated nor run.
        on = a.get ("on", 1.0) > 0.5;

        if (! on)
            return;

        tubamp::TremoloParams p;
        p.voice = (int) a.get ("voice", p.voice);
        p.rateHz = (float) a.get ("rate", p.rateHz);
        p.depth = (float) a.get ("depth", p.depth);
        p.mix = (float) a.get ("mix", p.mix);
        p.width = (float) a.get ("width", p.width);
        p.tone = (float) a.get ("tone", p.tone);
        p.shape = (float) a.get ("shape", p.shape);
        p.wave = (float) a.get ("wave", p.wave);
        p.syncPhase = a.get ("sync_phase", p.syncPhase);
        tremolo.setParameters (p);
    }

    void process (juce::AudioBuffer<float>& io, int) override
    {
        if (on)
            tremolo.process (juce::dsp::AudioBlock<float> (io));
    }

    std::unique_ptr<fxbench::Effect> makeLegacy() const override { return std::make_unique<LegacyModBench>(); }

private:
    tubamp::TremoloFx tremolo;
    bool on = true;
};

FXBENCH_REGISTER ("tremolo", TremoloBench,
                  "voice(0 Classic,1 Smooth,2 Optical,3 Bias,4 Harmonic) rate depth mix width tone shape wave sync_phase on(0/1)");
} // namespace
