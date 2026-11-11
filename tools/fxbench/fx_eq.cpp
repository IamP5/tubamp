// fxbench adapter for the EQ (docs/FX-ENGINES.md §4.4). Keys:
//   type bass mid treble lowcut highcut mid_freq mid_q tilt level voice
//   g100 g200 g400 g800 g1k6 g3k2 g6k4 on
// on=0 skips process(), as the processor does for a bypassed block.
#include "FxBench.h"
#include "legacy/LegacyFx.h"

#include "../../src/dsp/Eq.h"

namespace
{
class LegacyEqBench : public fxbench::Effect
{
public:
    const char* typeKey() const override { return "type"; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override
    {
        eq.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
    }

    void reset() override { eq.reset(); }

    void setParams (const fxbench::Args& a) override
    {
        // Like the processor: a bypassed block is neither updated nor run.
        on = a.get ("on", 1.0) > 0.5;

        if (! on)
            return;

        eq.setParameters ((float) a.get ("bass", 5.0), (float) a.get ("mid", 5.0), (float) a.get ("treble", 5.0));
    }

    void process (juce::AudioBuffer<float>& io, int) override
    {
        if (on)
            eq.process (juce::dsp::AudioBlock<float> (io));
    }

private:
    tubamp::legacy::ToneStackEQ eq;
    bool on = true;
};

class EqBench : public fxbench::Effect
{
public:
    const char* typeKey() const override { return "type"; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override
    {
        eq.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
    }

    void reset() override { eq.reset(); }

    void setParams (const fxbench::Args& a) override
    {
        // Like the processor: a bypassed block is neither updated nor run.
        on = a.get ("on", 1.0) > 0.5;

        if (! on)
            return;

        static constexpr const char* bands[7] { "g100", "g200", "g400", "g800", "g1k6", "g3k2", "g6k4" };
        tubamp::EqParams p;
        p.type = (int) a.get ("type", p.type);
        p.bass = (float) a.get ("bass", p.bass);
        p.mid = (float) a.get ("mid", p.mid);
        p.treble = (float) a.get ("treble", p.treble);
        p.lowCutHz = (float) a.get ("lowcut", p.lowCutHz);
        p.highCutHz = (float) a.get ("highcut", p.highCutHz);
        p.midFreqHz = (float) a.get ("mid_freq", p.midFreqHz);
        p.midQ = (float) a.get ("mid_q", p.midQ);
        p.tiltDb = (float) a.get ("tilt", p.tiltDb);
        p.levelDb = (float) a.get ("level", p.levelDb);
        p.voice = (int) a.get ("voice", p.voice);

        for (int b = 0; b < 7; ++b)
            p.graphicDb[b] = (float) a.get (bands[b], p.graphicDb[b]);

        eq.setParameters (p);
    }

    void process (juce::AudioBuffer<float>& io, int) override
    {
        if (on)
            eq.process (juce::dsp::AudioBlock<float> (io));
    }

    std::unique_ptr<fxbench::Effect> makeLegacy() const override { return std::make_unique<LegacyEqBench>(); }

private:
    tubamp::EqFx eq;
    bool on = true;
};

FXBENCH_REGISTER ("eq", EqBench,
                  "type(0 Classic,1 Parametric,2 Stack,3 Graphic) bass mid treble lowcut highcut mid_freq mid_q "
                  "tilt level voice(0 Tweed,1 Blackface,2 British,3 Top Boost) g100 g200 g400 g800 g1k6 g3k2 g6k4 on(0/1)");
} // namespace
