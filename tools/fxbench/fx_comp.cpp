// fxbench adapter for the compressor (docs/FX-ENGINES.md §4.2). Keys:
//   type threshold ratio attack release makeup knee mix sc_hpf auto_makeup on
// on=0 calls skip() instead of process(), the way the processor runs a bypassed or
// absent comp (exercises the makeup smoother through skip sequences).
// --trace: one channel, the per-sample gain reduction in dB (>= 0, makeup excluded);
// 0 while skipped.
#include "FxBench.h"
#include "legacy/LegacyFx.h"

#include "../../src/dsp/Compressor.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace
{
class LegacyCompBench : public fxbench::Effect
{
public:
    const char* typeKey() const override { return "type"; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override
    {
        comp.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
    }

    void reset() override { comp.reset(); }

    void setParams (const fxbench::Args& a) override
    {
        comp.setParameters ((float) a.get ("threshold", -20.0), (float) a.get ("ratio", 4.0), (float) a.get ("attack", 5.0),
                            (float) a.get ("release", 120.0), (float) a.get ("makeup", 0.0));
        on = a.get ("on", 1.0) > 0.5;
    }

    void process (juce::AudioBuffer<float>& io, int numSamples) override
    {
        if (on)
            comp.process (juce::dsp::AudioBlock<float> (io));
        else
            comp.skip (numSamples);
    }

private:
    tubamp::legacy::LegacyComp comp;
    bool on = true;
};

class CompBench : public fxbench::Effect
{
public:
    const char* typeKey() const override { return "type"; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override
    {
        comp.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
        gainTrace.assign ((size_t) maxBlock, 0.0f);
    }

    void reset() override { comp.reset(); }

    void setParams (const fxbench::Args& a) override
    {
        tubamp::CompParams p;
        p.type = (int) a.get ("type", p.type);
        p.thresholdDb = (float) a.get ("threshold", p.thresholdDb);
        p.ratio = (float) a.get ("ratio", p.ratio);
        p.attackMs = (float) a.get ("attack", p.attackMs);
        p.releaseMs = (float) a.get ("release", p.releaseMs);
        p.makeupDb = (float) a.get ("makeup", p.makeupDb);
        p.kneeDb = (float) a.get ("knee", p.kneeDb);
        p.mixPct = (float) a.get ("mix", p.mixPct);
        p.scHpfHz = (float) a.get ("sc_hpf", p.scHpfHz);
        p.autoMakeup = a.get ("auto_makeup", 0.0) > 0.5;
        comp.setParameters (p);
        on = a.get ("on", 1.0) > 0.5;
    }

    void process (juce::AudioBuffer<float>& io, int numSamples) override
    {
        const bool traced = numSamples <= (int) gainTrace.size();

        if (traced)
            std::fill (gainTrace.begin(), gainTrace.begin() + numSamples, 0.0f);

        if (on)
        {
            comp.setGainTrace (traced ? gainTrace.data() : nullptr);
            comp.process (juce::dsp::AudioBlock<float> (io));
        }
        else
        {
            comp.skip (numSamples);
        }
    }

    int traceChannels() const override { return 1; }

    void trace (float* const* dst, int numSamples) override
    {
        if (numSamples <= (int) gainTrace.size())
            std::copy (gainTrace.begin(), gainTrace.begin() + numSamples, dst[0]);
    }

    std::string meterRow() const override
    {
        char row[32];
        std::snprintf (row, sizeof (row), "%.4f", (double) comp.getGainReductionDb());
        return row;
    }

    std::unique_ptr<fxbench::Effect> makeLegacy() const override { return std::make_unique<LegacyCompBench>(); }

private:
    tubamp::CompFx comp;
    std::vector<float> gainTrace;
    bool on = true;
};

FXBENCH_REGISTER ("comp", CompBench,
                  "type(0 Classic,1 Clean,2 Squash,3 FET,4 Opto) threshold ratio attack release makeup knee mix "
                  "sc_hpf auto_makeup(0/1) on(0 = skip); --meters = GR dB; --trace = per-sample GR dB");
} // namespace
