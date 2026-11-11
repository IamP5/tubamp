// fxbench adapter for the tremolo voicings of the mod block (docs/FX-ENGINES.md §4.7).
// Keys: voice rate depth mix width tone shape wave sync_phase on (0 skips process()). Switching on back to 1
// on voices 1-4 calls reset() between setParameters() and process(), as the processor does when the block
// is re-enabled. The legacy reference is the old Modulation block running its tremolo. --trace writes the gain each channel gets (voices 1-4;
// Harmonic: the low band's), through TremoloFx::setTraceBuffers.
//
// Scripted play head (test hook): with host_sync=1 the adapter stands in for the processor's dispatcher
// (FxBlocks.cpp resolveTiming): rate = host_bpm / (60 * host_beats) and, while host_play is 1, sync_phase =
// frac(ppq / host_beats) at each block start, the song position advancing by the block's tempo. A change of
// host_ppq moves the song position there (locate, loop, start position); host_jitter=J adds a pseudo-random
// whole number of samples in [-J, J] to each block's reported position. --meters writes per block the
// engine's song-position jump count and its LFO phase at block start minus the true (unjittered) host phase.
#include "FxBench.h"
#include "legacy/LegacyFx.h"

#include "../../src/dsp/Tremolo.h"

#include <cstdint>
#include <cstdio>
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
        fs = sampleRate;
        host = {};

        for (auto& t : traced)
            t.assign ((size_t) juce::jmax (1, maxBlock), 0.0f);

        tremolo.setTraceBuffers (traced[0].data(), traced[1].data());
    }

    void reset() override
    {
        tremolo.reset();
        wasOn = true;
        resetPending = false;
        host = {};
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
            else if (key == "host_sync")   host.sync = value > 0.5;
            else if (key == "host_bpm")    host.bpm = value;
            else if (key == "host_beats")  host.beats = value;
            else if (key == "host_play")   host.play = value > 0.5;
            else if (key == "host_jitter") host.jitter = (int) value;
            else if (key == "host_ppq" && ! juce::exactlyEqual (value, host.located))
                host.ppq = host.located = value;
        }

        host.truePhase = -1.0;

        if (host.sync)
        {
            const double hz = host.bpm / (60.0 * host.beats);
            p.rateHz = (float) juce::jlimit (0.01, 20.0, hz);
            p.syncPhase = -1.0;

            if (host.play && hz >= 0.01 && hz <= 20.0)
            {
                host.lcg = host.lcg * 1664525u + 1013904223u;
                const int j = host.jitter > 0 ? (int) ((host.lcg >> 8) % (std::uint32_t) (2 * host.jitter + 1)) - host.jitter : 0;
                const double reported = (host.ppq + (double) j * host.bpm / (60.0 * fs)) / host.beats;
                p.syncPhase = reported - std::floor (reported);
                host.truePhase = host.ppq / host.beats - std::floor (host.ppq / host.beats);
            }
        }

        // Like the processor: a bypassed block is neither updated nor run.
        if (on)
            tremolo.setParameters (p);

        if (on && ! wasOn && p.voice != 0)
            resetPending = true;

        wasOn = on;
    }

    void process (juce::AudioBuffer<float>& io, int numSamples) override
    {
        if (on)
        {
            if (resetPending)
            {
                resetPending = false;
                tremolo.reset();
            }

            if (host.truePhase >= 0.0)
            {
                const double d = tremolo.getLfoPhase() - host.truePhase;
                phaseError = d - std::floor (d + 0.5);
            }

            tremolo.process (juce::dsp::AudioBlock<float> (io));
        }

        // The transport runs whether the block is on or synced.
        if (host.play)
            host.ppq += host.bpm / 60.0 * (double) numSamples / fs;
    }

    std::string meterRow() const override
    {
        char row[64];
        std::snprintf (row, sizeof (row), "%d\t%.9f", tremolo.getSyncJumpCount(), phaseError);
        return row;
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

    struct Host
    {
        bool sync = false, play = true;
        double bpm = 120.0, beats = 1.0, ppq = 0.0, located = -1.0e300, truePhase = -1.0;
        int jitter = 0;
        std::uint32_t lcg = 12345u;
    };

    Host host;
    double fs = 48000.0, phaseError = 0.0;
};

FXBENCH_REGISTER ("tremolo", TremoloBench,
                  "voice(0 Classic,1 Smooth,2 Optical,3 Bias,4 Harmonic) rate depth mix width tone shape wave sync_phase on(0/1) "
                  "host_sync host_bpm host_beats host_play host_ppq host_jitter");
} // namespace
