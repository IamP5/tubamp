// fxbench adapter for the gate (docs/FX-ENGINES.md §4.1). Keys:
//   mode threshold release hold range sc_hpf delay deferred on lag
// on=0 neither measures nor applies (the processor's gate_on off). deferred=1 applies the
// curve the way the processor does when the amp follows the gate: to the channels' mono
// mean, which is then copied back to every channel. delay=N is setCurveDelay; lag=N delays
// that mono audio by N samples first (deferred only), standing in for the amp's latency.
// --trace writes the applied gain (linear), read by applying the curve to a buffer of ones.
#include "FxBench.h"
#include "legacy/LegacyFx.h"

#include "../../src/dsp/Gate.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace
{
/** processAmpMono's collapse (PluginProcessor.cpp), so deferred renders match the plugin. */
void collapseToMono (juce::AudioBuffer<float>& io, int numSamples, float* mono)
{
    const int numChannels = io.getNumChannels();

    if (numChannels == 1)
    {
        juce::FloatVectorOperations::copy (mono, io.getReadPointer (0), numSamples);
        return;
    }

    const float scale = 1.0f / (float) numChannels;

    for (int n = 0; n < numSamples; ++n)
    {
        float sum = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
            sum += io.getReadPointer (ch)[n];

        mono[n] = sum * scale;
    }
}

void expandFromMono (juce::AudioBuffer<float>& io, int numSamples, const float* mono)
{
    for (int ch = 0; ch < io.getNumChannels(); ++ch)
        juce::FloatVectorOperations::copy (io.getWritePointer (ch), mono, numSamples);
}

class LegacyGateBench : public fxbench::Effect
{
public:
    const char* typeKey() const override { return "mode"; }

    void prepare (double sampleRate, int maxBlock, int) override
    {
        gate.prepare (sampleRate, maxBlock);
        gate.reset();
        mono.assign ((size_t) maxBlock, 0.0f);
        probe.assign ((size_t) maxBlock, 1.0f);
    }

    void reset() override { gate.reset(); }

    void setParams (const fxbench::Args& a) override
    {
        threshold = (float) a.get ("threshold", -80.0);
        deferred = a.get ("deferred", 0.0) > 0.5;
        on = a.get ("on", 1.0) > 0.5;
    }

    void process (juce::AudioBuffer<float>& io, int numSamples) override
    {
        juce::dsp::AudioBlock<float> block (io);
        std::fill (probe.begin(), probe.begin() + numSamples, 1.0f);

        if (! on)
        {
            if (deferred)
            {
                collapseToMono (io, numSamples, mono.data());
                expandFromMono (io, numSamples, mono.data());
            }

            return;
        }

        gate.measure (block, numSamples, threshold);

        gate.apply (probe.data(), numSamples);

        if (! deferred)
        {
            gate.apply (block, numSamples);
            return;
        }

        collapseToMono (io, numSamples, mono.data());
        gate.apply (mono.data(), numSamples);
        expandFromMono (io, numSamples, mono.data());
    }

    int traceChannels() const override { return 1; }
    void trace (float* const* dst, int numSamples) override { std::copy (probe.begin(), probe.begin() + numSamples, dst[0]); }

private:
    tubamp::legacy::NoiseGate gate;
    std::vector<float> mono, probe;
    float threshold = -80.0f;
    bool deferred = false, on = true;
};

class GateBench : public fxbench::Effect
{
public:
    const char* typeKey() const override { return "mode"; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override
    {
        gate.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
        gate.reset();
        mono.assign ((size_t) maxBlock, 0.0f);
        probe.assign ((size_t) maxBlock, 1.0f);
        lagLine.assign ((size_t) (tubamp::GateFx::kMaxCurveDelaySamples + maxBlock + 1), 0.0f);
        lagWrite = 0;
    }

    void reset() override
    {
        gate.reset();
        std::fill (lagLine.begin(), lagLine.end(), 0.0f);
        lagWrite = 0;
    }

    void setParams (const fxbench::Args& a) override
    {
        tubamp::GateParams p;
        p.mode = (int) a.get ("mode", p.mode);
        p.thresholdDb = (float) a.get ("threshold", p.thresholdDb);
        p.releaseMs = (float) a.get ("release", p.releaseMs);
        p.holdMs = (float) a.get ("hold", p.holdMs);
        p.rangeDb = (float) a.get ("range", p.rangeDb);
        p.scHpfHz = (float) a.get ("sc_hpf", p.scHpfHz);
        gate.setParameters (p);
        gate.setCurveDelay ((int) a.get ("delay", 0.0));
        deferred = a.get ("deferred", 0.0) > 0.5;
        on = a.get ("on", 1.0) > 0.5;
        lag = juce::jlimit (0, tubamp::GateFx::kMaxCurveDelaySamples, (int) a.get ("lag", 0.0));
    }

    void process (juce::AudioBuffer<float>& io, int numSamples) override
    {
        juce::dsp::AudioBlock<float> block (io);
        traced = numSamples;

        if (on)
            gate.measure (block, numSamples);

        if (! deferred)
        {
            if (on)
                gate.apply (block, numSamples);

            return;
        }

        collapseToMono (io, numSamples, mono.data());
        delayMono (numSamples);

        if (on)
            gate.apply (mono.data(), numSamples);

        expandFromMono (io, numSamples, mono.data());
    }

    int traceChannels() const override { return 1; }

    // The applied gain, read through the same overload process() used: apply() changes no
    // state, so probing after the audio is equivalent and keeps the probe out of --bench.
    void trace (float* const* dst, int numSamples) override
    {
        std::fill (probe.begin(), probe.begin() + numSamples, 1.0f);

        if (on && numSamples == traced)
        {
            if (deferred)
            {
                gate.apply (probe.data(), numSamples);
            }
            else
            {
                float* channels[1] { probe.data() };
                gate.apply (juce::dsp::AudioBlock<float> (channels, 1, (size_t) numSamples), numSamples);
            }
        }

        std::copy (probe.begin(), probe.begin() + numSamples, dst[0]);
    }

    std::string meterRow() const override
    {
        char row[96];
        std::snprintf (row, sizeof (row), "%d\t%.4f\t%.4f", gate.getState(), (double) gate.getGainReductionDb(),
                       (double) gate.getDetectorLevelDb());
        return row;
    }

    std::unique_ptr<fxbench::Effect> makeLegacy() const override { return std::make_unique<LegacyGateBench>(); }

private:
    /** lag=N stands in for the amp's latency on the deferred path: the mono audio reaching
        apply() is N samples late, as the NAM output is when the model resamples. */
    void delayMono (int numSamples)
    {
        if (lag == 0)
            return;

        const int size = (int) lagLine.size();

        for (int i = 0; i < numSamples; ++i)
        {
            lagLine[(size_t) lagWrite] = mono[(size_t) i];
            mono[(size_t) i] = lagLine[(size_t) ((lagWrite - lag + size) % size)];
            lagWrite = (lagWrite + 1) % size;
        }
    }

    tubamp::GateFx gate;
    std::vector<float> mono, probe, lagLine;
    int lagWrite = 0, lag = 0, traced = 0;
    bool deferred = false, on = true;
};

FXBENCH_REGISTER ("gate", GateBench,
                  "mode(0 Classic,1 Tight,2 Smooth) threshold release hold range sc_hpf delay deferred(0/1) on(0/1) "
                  "lag(deferred audio delay, samples); "
                  "--trace = applied gain (linear); --meters = state, GR dB, detector dBFS");
} // namespace
