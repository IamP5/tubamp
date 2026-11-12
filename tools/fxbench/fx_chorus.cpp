// fxbench adapter for the chorus voicings of the mod block (docs/FX-ENGINES.md §4.5).
// Keys: mode rate depth mix width tone sync_phase solo on (0 skips process()) reset (a change of value calls
// reset() before that block's parameters, as the processor does when a bypassed block comes back), and a host
// play head (host_*, below). The legacy reference is the old Modulation block running its chorus.
#include "FxBench.h"
#include "legacy/LegacyFx.h"

#include "../../src/dsp/Chorus.h"

#include <cmath>

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

    void reset() override
    {
        modulation.reset();
        lastReset = 0.0;
    }

    void setParams (const fxbench::Args& a) override
    {
        // Like the processor: a bypassed block is neither updated nor run.
        on = a.get ("on", 1.0) > 0.5;

        if (! on)
            return;

        // The reset key calls the legacy block's own reset(), as ChorusBench calls ChorusFx's:
        // --classic-null then compares Classic after a reset with the old chorus after its own.
        const double resetKey = a.get ("reset", 0.0);

        if (! juce::exactlyEqual (resetKey, lastReset))
        {
            lastReset = resetKey;
            modulation.reset();
        }

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
    double lastReset = 0.0;
    bool on = true;
};

class ChorusBench : public fxbench::Effect
{
public:
    const char* typeKey() const override { return "mode"; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override
    {
        chorus.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
        traceBuffer.setSize (3, maxBlock);
        traceBuffer.clear();
        fs = sampleRate;
    }

    void reset() override
    {
        chorus.reset();
        lastReset = 0.0;
        ppq = 0.0;
        seed = 12345u;
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

        // Host play head (test hook): host_bpm > 0 runs a song position that advances by each
        // block's tempo and resolves rate and sync phase as the Modulation dispatcher does, for
        // host_beats beats per LFO cycle. host_jitter reports the position off by up to that
        // many samples (uniform, per block); host_loop_len > 0 wraps it back by that many beats
        // once it reaches host_loop_end; host_play 0 is a stopped transport.
        bpm = a.get ("host_bpm", 0.0);

        if (bpm > 0.0)
        {
            const double beats = a.get ("host_beats", 1.0);
            const double loopLen = a.get ("host_loop_len", 0.0);

            if (loopLen > 0.0 && ppq >= a.get ("host_loop_end", 0.0))
                ppq -= loopLen;

            seed = seed * 1664525u + 1013904223u;
            const double u = (double) (seed >> 8) / (double) (1u << 24) * 2.0 - 1.0;
            const double reported = std::max (0.0, ppq + a.get ("host_jitter", 0.0) * u * bpm / (60.0 * fs));
            const double cycles = reported / beats;
            p.rateHz = (float) (bpm / (60.0 * beats));
            p.syncPhase = a.get ("host_play", 1.0) > 0.5 ? cycles - std::floor (cycles) : -1.0;
        }

        chorus.setParameters (p);
        chorus.setDebugSolo ((int) a.get ("solo", -1.0));
    }

    void process (juce::AudioBuffer<float>& io, int numSamples) override
    {
        if (! on)
            return;

        chorus.setDebugTrace (traceBuffer.getArrayOfWritePointers());
        chorus.process (juce::dsp::AudioBlock<float> (io));
        ppq += (double) numSamples * bpm / (60.0 * fs);
    }

    int traceChannels() const override { return 3; }

    void trace (float* const* dst, int numSamples) override
    {
        for (int ch = 0; ch < 3; ++ch)
            if (dst[ch] != nullptr)
                juce::FloatVectorOperations::copy (dst[ch], traceBuffer.getReadPointer (ch), numSamples);
    }

    std::string meterRow() const override
    {
        const auto c = chorus.getDebugCounts();
        return std::to_string (c.syncJumps) + "\t" + std::to_string (c.knobFades);
    }

    std::unique_ptr<fxbench::Effect> makeLegacy() const override { return std::make_unique<LegacyModBench>(); }

private:
    tubamp::ChorusFx chorus;
    juce::AudioBuffer<float> traceBuffer;
    double lastReset = 0.0, fs = 48000.0, ppq = 0.0, bpm = 0.0;
    juce::uint32 seed = 12345u;
    bool on = true;
};

FXBENCH_REGISTER ("chorus", ChorusBench,
                  "mode(0 Classic,1 Wide,2 CE,3 Dimension,4 Tri) rate depth mix width tone sync_phase solo(-1 = off) on(0/1) "
                  "reset(a change calls reset()) host_bpm host_beats host_jitter host_play host_loop_len host_loop_end "
                  "(a play head: rate and sync_phase from tempo and note value); --trace: the soloed voice's tap delays "
                  "and crossfade stage; --meters: sync jumps, knob crossfades");
} // namespace
