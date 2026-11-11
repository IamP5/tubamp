// fxbench adapter for the tremolo voicings of the mod block (docs/FX-ENGINES.md §4.7).
// Keys: voice rate depth mix width tone shape wave sync_phase on (0 skips process()). Switching on back to 1
// on voices 1-4 calls reset() between setParameters() and process(), as the processor does when the block
// is re-enabled. The legacy reference is the old Modulation block running its tremolo. --trace writes the gain each channel gets (voices 1-4;
// Harmonic: the low band's), through TremoloFx::setTraceBuffers.
#include "FxBench.h"
#include "legacy/LegacyFx.h"

#include "../../src/dsp/Tremolo.h"

#include <vector>

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

        for (auto& t : traced)
            t.assign ((size_t) juce::jmax (1, maxBlock), 0.0f);

        tremolo.setTraceBuffers (traced[0].data(), traced[1].data());
    }

    void reset() override
    {
        tremolo.reset();
        wasOn = true;
        resetPending = false;
    }

    void setParams (const fxbench::Args& a) override
    {
        // --bench times setParams too, so walk only the keys that were given: nine map
        // lookups per block would cost more than the Classic voice itself.
        tubamp::TremoloParams p;
        on = true;

        for (const auto& [key, value] : a.values)
        {
            if (key == "voice")            p.voice = (int) value;
            else if (key == "rate")        p.rateHz = (float) value;
            else if (key == "depth")       p.depth = (float) value;
            else if (key == "mix")         p.mix = (float) value;
            else if (key == "width")       p.width = (float) value;
            else if (key == "tone")        p.tone = (float) value;
            else if (key == "shape")       p.shape = (float) value;
            else if (key == "wave")        p.wave = (float) value;
            else if (key == "sync_phase")  p.syncPhase = value;
            else if (key == "on")          on = value > 0.5;
        }

        // Like the processor: a bypassed block is neither updated nor run.
        if (on)
            tremolo.setParameters (p);

        if (on && ! wasOn && p.voice != 0)
            resetPending = true;

        wasOn = on;
    }

    void process (juce::AudioBuffer<float>& io, int) override
    {
        if (! on)
            return;

        if (resetPending)
        {
            resetPending = false;
            tremolo.reset();
        }

        tremolo.process (juce::dsp::AudioBlock<float> (io));
    }

    int traceChannels() const override { return 2; }

    void trace (float* const* dst, int numSamples) override
    {
        const auto n = (size_t) juce::jmin (numSamples, (int) traced[0].size());

        for (int ch = 0; ch < 2; ++ch)
            if (dst[ch] != nullptr)
                std::copy (traced[ch].begin(), traced[ch].begin() + (std::ptrdiff_t) n, dst[ch]);
    }

    std::unique_ptr<fxbench::Effect> makeLegacy() const override { return std::make_unique<LegacyModBench>(); }

private:
    tubamp::TremoloFx tremolo;
    std::vector<float> traced[2];
    bool on = true, wasOn = true, resetPending = false;
};

FXBENCH_REGISTER ("tremolo", TremoloBench,
                  "voice(0 Classic,1 Smooth,2 Optical,3 Bias,4 Harmonic) rate depth mix width tone shape wave sync_phase on(0/1)");
} // namespace
