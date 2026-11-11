// fxbench adapter for the drive (docs/FX-ENGINES.md §4.3). Keys: type gain tone level on.
// on=0 skips process(), as the processor does for a bypassed block.
#include "FxBench.h"
#include "legacy/LegacyFx.h"

#include "../../src/dsp/Drive.h"

namespace
{
class LegacyDriveBench : public fxbench::Effect
{
public:
    const char* typeKey() const override { return "type"; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override
    {
        drive.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
    }

    void reset() override { drive.reset(); }

    void setParams (const fxbench::Args& a) override
    {
        // Like the processor: a bypassed block is neither updated nor run.
        on = a.get ("on", 1.0) > 0.5;

        if (! on)
            return;

        drive.setParameters ((float) a.get ("gain", 12.0), (float) a.get ("tone", 4000.0), (float) a.get ("level", 0.0));
    }

    void process (juce::AudioBuffer<float>& io, int) override
    {
        if (on)
            drive.process (juce::dsp::AudioBlock<float> (io));
    }

private:
    tubamp::legacy::Drive drive;
    bool on = true;
};

class DriveBench : public fxbench::Effect
{
public:
    const char* typeKey() const override { return "type"; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override
    {
        drive.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
    }

    void reset() override { drive.reset(); }

    void setParams (const fxbench::Args& a) override
    {
        // Like the processor: a bypassed block is neither updated nor run.
        on = a.get ("on", 1.0) > 0.5;

        if (! on)
            return;

        tubamp::DriveParams p;
        p.type = (int) a.get ("type", p.type);
        p.gainDb = (float) a.get ("gain", p.gainDb);
        p.toneHz = (float) a.get ("tone", p.toneHz);
        p.levelDb = (float) a.get ("level", p.levelDb);
        drive.setParameters (p);
    }

    void process (juce::AudioBuffer<float>& io, int) override
    {
        if (on)
            drive.process (juce::dsp::AudioBlock<float> (io));
    }

    int latency() const override { return drive.getLatencySamples(); }

    std::unique_ptr<fxbench::Effect> makeLegacy() const override { return std::make_unique<LegacyDriveBench>(); }

private:
    tubamp::DriveFx drive;
    bool on = true;
};

FXBENCH_REGISTER ("drive", DriveBench,
                  "type(0 Classic,1 Screamer,2 Distortion,3 Fuzz,4 Boost,5 Treble) gain tone level on(0/1); --latency");
} // namespace
