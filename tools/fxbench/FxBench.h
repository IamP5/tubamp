#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

// tubamp_fxbench's registration interface (docs/FX-ENGINES.md §5.3). Frozen once the
// engine engineers start: each tools/fxbench/fx_<effect>.cpp implements one Effect and
// registers it with FXBENCH_REGISTER; main.cpp owns everything else.
namespace fxbench
{
/** Parameter values for one block, after the timeline has been applied. */
struct Args
{
    std::map<std::string, double> values;
    double get (const char* key, double fallback) const;
    bool has (const char* key) const;
};

class Effect
{
public:
    virtual ~Effect() = default;
    virtual const char* typeKey() const = 0;          // "type", "mode", "model", "voice"
    virtual void prepare (double sampleRate, int maxBlock, int numChannels) = 0;
    virtual void reset() = 0;
    virtual void setParams (const Args&) = 0;          // every block, before process()
    virtual void process (juce::AudioBuffer<float>& io, int numSamples) = 0;
    virtual int latency() const { return 0; }
    virtual int traceChannels() const { return 0; }   // > 0 if --trace is supported
    virtual void trace (float* const* dst, int numSamples) { juce::ignoreUnused (dst, numSamples); }
    virtual std::string meterRow() const { return {}; } // one TSV row per block for --meters
    /** The frozen pre-change implementation, for --classic-null (fed the same Args). */
    virtual std::unique_ptr<Effect> makeLegacy() const { return nullptr; }
};

struct Registration
{
    const char* name;   // "gate", "comp", "drive", "eq", "chorus", "phaser", "tremolo"
    const char* help;   // the effect's keys, printed by --help
    std::unique_ptr<Effect> (*make)();
};

bool registerEffect (const Registration&);   // called from a static initialiser

//==============================================================================
/** The standard test input `gen:music` (docs/FX-ENGINES.md §5.3.5), shared by
    tubamp_fxbench and tubamp_golden; tools/fxbench/fxcheck.py music() is the same
    algorithm in numpy. Everything is computed in double and rounded to float once.

      0.0-2.0 s  plucked notes 82.41 Hz (at 0.0 s), 110 Hz (0.6 s), 196 Hz (1.2 s)
      2.0-3.0 s  the three notes together, plus white noise at -30 dBFS RMS
      3.0-4.0 s  1 kHz sine, -20 dBFS peak, 5 ms linear fade in and out
      4.0-4.5 s  white noise at -30 dBFS RMS
      4.5-6.0 s  silence

    A note is harmonics 1-4 at 1 / 0.6 / 0.4 / 0.25, a 5 ms linear attack, then
    exp(-t / 0.4 s), cut by a 5 ms linear fade ending at its section's end (2.0 s for the
    single notes, 3.0 s for the chord). The right channel's notes are the left's 3 ms
    later. All notes together are scaled so their peak over both channels is -6 dBFS.
    Noise: LCG x = x * 1664525 + 1013904223 mod 2^32 from seed 12345, u = x / 2^32 * 2 - 1,
    amplitude 10^(-30/20) * sqrt(3); two values are drawn per noise frame (left, then
    right) whatever the channel count, so the left channel never depends on it.
    Frames past 6 s are zero; a mono buffer gets the left channel. */
inline void fillMusic (juce::AudioBuffer<float>& buffer, double sampleRate)
{
    const int numFrames = buffer.getNumSamples();
    const int numChannels = juce::jmin (2, buffer.getNumChannels());
    buffer.clear();

    if (numFrames <= 0 || numChannels <= 0 || sampleRate <= 0.0)
        return;

    struct Note { double start, end; double hz[3]; int count; };
    static constexpr Note notes[] {
        { 0.0, 2.0, { 82.41, 0.0, 0.0 }, 1 },
        { 0.6, 2.0, { 110.0, 0.0, 0.0 }, 1 },
        { 1.2, 2.0, { 196.0, 0.0, 0.0 }, 1 },
        { 2.0, 3.0, { 82.41, 110.0, 196.0 }, 3 },
    };
    static constexpr double harmonics[4] { 1.0, 0.6, 0.4, 0.25 };
    constexpr double twoPi = 6.283185307179586476925286766559;
    constexpr double attack = 0.005, decay = 0.4, release = 0.005;

    const int total = juce::jmin (numFrames, (int) std::llround (6.0 * sampleRate));
    const auto frameAt = [sampleRate] (double seconds) { return (int) std::llround (seconds * sampleRate); };

    // Notes first, in double, so they can be normalised to their own peak.
    juce::HeapBlock<double> noteSignal ((size_t) total * 2, true);
    double peak = 0.0;

    for (int ch = 0; ch < 2; ++ch)
    {
        const double offset = ch == 1 ? 0.003 : 0.0;

        for (int i = 0; i < total; ++i)
        {
            const double t = (double) i / sampleRate - offset;
            double sum = 0.0;

            for (const auto& note : notes)
            {
                const double tau = t - note.start;

                if (tau < 0.0 || t >= note.end)
                    continue;

                double env = tau < attack ? tau / attack : std::exp (-(tau - attack) / decay);
                env *= juce::jmin (1.0, (note.end - t) / release);

                for (int n = 0; n < note.count; ++n)
                    for (int h = 0; h < 4; ++h)
                        sum += harmonics[h] * std::sin (twoPi * note.hz[n] * (double) (h + 1) * tau) * env;
            }

            noteSignal[(size_t) (ch * total + i)] = sum;
            peak = juce::jmax (peak, std::abs (sum));
        }
    }

    const double noteScale = peak > 0.0 ? std::pow (10.0, -6.0 / 20.0) / peak : 0.0;
    const double noiseAmp = std::pow (10.0, -30.0 / 20.0) * std::sqrt (3.0);
    const double sineAmp = std::pow (10.0, -20.0 / 20.0);
    const int noiseA0 = frameAt (2.0), noiseA1 = frameAt (3.0);
    const int sine0 = frameAt (3.0), sine1 = frameAt (4.0);
    const int noiseB0 = frameAt (4.0), noiseB1 = frameAt (4.5);
    const int fadeFrames = juce::jmax (1, frameAt (0.005));
    uint32_t lcg = 12345u;

    for (int i = 0; i < total; ++i)
    {
        double value[2] { noteSignal[(size_t) i] * noteScale, noteSignal[(size_t) (total + i)] * noteScale };

        if ((i >= noiseA0 && i < noiseA1) || (i >= noiseB0 && i < noiseB1))
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                lcg = lcg * 1664525u + 1013904223u;
                value[ch] += noiseAmp * ((double) lcg / 4294967296.0 * 2.0 - 1.0);
            }
        }

        if (i >= sine0 && i < sine1)
        {
            const int k = i - sine0;
            const double fade = juce::jmin (1.0, juce::jmin ((double) (k + 1), (double) (sine1 - i)) / (double) fadeFrames);
            const double s = sineAmp * fade * std::sin (twoPi * 1000.0 * (double) k / sampleRate);
            value[0] += s;
            value[1] += s;
        }

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.setSample (ch, i, (float) value[ch]);
    }
}
} // namespace fxbench

#define FXBENCH_REGISTER(NAME, CLASS, HELP)                                              \
    static const bool fxbenchRegistered_##CLASS = fxbench::registerEffect (               \
        { NAME, HELP, [] () -> std::unique_ptr<fxbench::Effect> { return std::make_unique<CLASS>(); } })
