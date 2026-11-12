// fxbench adapter for the phaser voicings of the mod block (docs/FX-ENGINES.md §4.6).
// Keys: model rate depth mix color sync_phase on (0 skips process()), reset_on_enable (1: call
// reset() when on goes from 0 to 1, as the processor does for Phase 90 and Small Stone), and the
// test key freeze_u (hold the Phase 90 / Small Stone sweep at u in [-1, 1]; absent = running),
// start_reset (1: call reset() after the first block's parameters are set, so the engine starts as a
// voicing switched in does; without it Classic's LFO glides from juce's 1 Hz to the set rate).
// A simulated host: host_bpm (tempo; present = synced, it then sets rate and sync_phase),
// host_note (LFO cycles per beat, default 1), host_ppq (song position of the first block, beats),
// host_jitter (N: from the second block on, each block's reported position is off by a whole
// number of samples drawn uniformly from -N..N, not accumulated), host_drift (D: the song position
// advances 1 + D times as fast as the reported tempo says, as when the tempo parameter lags the
// host's own; default 0). The song position integrates the tempo between block starts as a
// straight line, as a host ramping its tempo does; the phase is position x note, so a host_note
// change is a jump.
// --trace writes, per block: the LFO phase in cycles of the block's last sample, the number of
// song-position jumps glided so far, and the simulated host's exact (unjittered) phase at the
// block's last sample. Use --block=1 for a per-sample trace. The legacy reference is the old
// Modulation block running its phaser.
#include "FxBench.h"
#include "legacy/LegacyFx.h"

#include "../../src/dsp/Phaser.h"

#include <cmath>
#include <cstdint>
#include <limits>

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

    void prepare (double rate, int maxBlock, int numChannels) override
    {
        sampleRate = rate;
        phaser.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
        hostStarted = started = false;
    }

    void reset() override
    {
        phaser.reset();
        hostStarted = started = false;
        lcg = 12345u;
    }

    void setParams (const fxbench::Args& a) override
    {
        // Like the processor: a bypassed block is neither updated nor run.
        const bool wasOn = on;
        on = a.get ("on", 1.0) > 0.5;

        if (! on)
            return;

        if (! wasOn && a.get ("reset_on_enable", 0.0) > 0.5)
            phaser.reset();

        tubamp::PhaserParams p;
        p.model = (int) a.get ("model", p.model);
        p.rateHz = (float) a.get ("rate", p.rateHz);
        p.depth = (float) a.get ("depth", p.depth);
        p.mix = (float) a.get ("mix", p.mix);
        p.color = a.get ("color", p.color ? 1.0 : 0.0) > 0.5;
        p.syncPhase = a.get ("sync_phase", p.syncPhase);

        if (a.has ("host_bpm"))
        {
            const double bpm = a.get ("host_bpm", 120.0), note = a.get ("host_note", 1.0);

            const bool first = ! hostStarted;

            if (first)
                beats = a.get ("host_ppq", 0.0);
            else
                beats += 0.5 * (lastBpm + bpm) / 60.0 * (double) lastBlock / sampleRate * (1.0 + drift);

            hostStarted = true;
            lastBpm = bpm;
            drift = a.get ("host_drift", 0.0);
            hostRate = bpm / 60.0 * note;
            hostPhase = beats * note;

            int offset = 0;
            const int range = (int) a.get ("host_jitter", 0.0);

            if (range > 0 && ! first)
            {
                lcg = lcg * 1664525u + 1013904223u;
                offset = (int) ((double) lcg / 4294967296.0 * (2 * range + 1)) - range;
            }

            p.rateHz = (float) hostRate;
            p.syncPhase = hostPhase + (double) offset * hostRate / sampleRate;
        }

        phaser.setParameters (p);

        if (! started && a.get ("start_reset", 0.0) > 0.5)
            phaser.reset();

        started = true;
        phaser.freezeSweep (a.get ("freeze_u", std::numeric_limits<double>::quiet_NaN()));
    }

    void process (juce::AudioBuffer<float>& io, int numSamples) override
    {
        lastBlock = numSamples;

        if (on)
            phaser.process (juce::dsp::AudioBlock<float> (io));
    }

    int traceChannels() const override { return 3; }

    void trace (float* const* dst, int numSamples) override
    {
        const double host = hostPhase + hostRate * (1.0 + drift) * (double) (numSamples - 1) / sampleRate;
        juce::FloatVectorOperations::fill (dst[0], (float) phaser.getLfoPhase(), numSamples);
        juce::FloatVectorOperations::fill (dst[1], (float) phaser.getSyncJumpCount(), numSamples);
        juce::FloatVectorOperations::fill (dst[2], (float) (host - std::floor (host)), numSamples);
    }

    std::unique_ptr<fxbench::Effect> makeLegacy() const override { return std::make_unique<LegacyModBench>(); }

private:
    tubamp::PhaserFx phaser;
    double sampleRate = 48000.0, beats = 0.0, lastBpm = 120.0, hostRate = 0.0, hostPhase = 0.0, drift = 0.0;
    int lastBlock = 0;
    uint32_t lcg = 12345u;
    bool on = true, hostStarted = false, started = false;
};

FXBENCH_REGISTER ("phaser", PhaserBench,
                  "model(0 Classic,1 Phase 90,2 Small Stone) rate depth mix color(0/1) sync_phase on(0/1) "
                  "reset_on_enable(0/1) freeze_u(test) start_reset(test) host_bpm host_note host_ppq host_jitter(samples) host_drift(fraction)");
} // namespace
