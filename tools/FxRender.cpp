// Offline renderer for the built-in reverb and delay blocks: drives one ReverbFx or
// DelayFx directly (no processor, no chain, no host) with a known stimulus and writes
// the result as a 32-bit float stereo WAV. It exists so the two engines can be
// measured — decay time per band, echo density, repeat spacing, modulation depth —
// and compared against reference plugins without a DAW in the loop.
//
//   tubamp_fxrender reverb out.wav [key=value ...]
//   tubamp_fxrender delay  out.wav [key=value ...]
//
// Common keys:   sr=48000 block=512 seconds=6 warmup=1 channels=2
//                stim=impulse | impulseL | burst:<hz>:<ms> | sine:<hz> | noise:<ms>
// Every other key is an engine parameter in the units Parameters.h documents:
//   reverb: type (0 Room, 1 Hall, 2 Plate, 3 Spring, 4 Ambient) size damping mix width
//           predelay decay lowcut highcut mod
//   delay:  type (0 Digital, 1 Analog, 2 Tape, 3 Lo-Fi, 4 Diffuse) time feedback mix mode
//           ratio width sync bpm lowcut highcut mod color duck
//           time2=<ms> at=<s>   (switch the delay time mid-render)
// mix defaults to 1 (wet only); everything else to the engine's own default.
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "../src/dsp/Delay.h"
#include "../src/dsp/Reverb.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace
{
using Args = std::map<std::string, std::string>;

float num (const Args& args, const char* key, float fallback)
{
    const auto it = args.find (key);
    return it != args.end() ? (float) std::atof (it->second.c_str()) : fallback;
}

std::string str (const Args& args, const char* key, const char* fallback)
{
    const auto it = args.find (key);
    return it != args.end() ? it->second : std::string (fallback);
}

/** Fills `buffer` (already cleared) with the stimulus, starting at sample 0. */
bool makeStimulus (const std::string& spec, juce::AudioBuffer<float>& buffer, double sampleRate)
{
    const int n = buffer.getNumSamples();
    const int channels = buffer.getNumChannels();
    const auto kind = spec.substr (0, spec.find (':'));
    juce::StringArray parts;
    parts.addTokens (juce::String (spec), ":", "");

    if (kind == "impulse" || kind == "impulseL")
    {
        for (int ch = 0; ch < channels; ++ch)
            if (kind == "impulse" || ch == 0)
                buffer.setSample (ch, 0, 0.5f);

        return true;
    }

    if (kind == "burst" && parts.size() >= 3)
    {
        const double hz = parts[1].getDoubleValue();
        const int len = juce::jmin (n, (int) (parts[2].getDoubleValue() * 0.001 * sampleRate));

        for (int i = 0; i < len; ++i)
        {
            const double window = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / juce::jmax (1, len - 1));
            const auto v = (float) (0.5 * window * std::sin (2.0 * juce::MathConstants<double>::pi * hz * i / sampleRate));

            for (int ch = 0; ch < channels; ++ch)
                buffer.setSample (ch, i, v);
        }

        return true;
    }

    if (kind == "sine" && parts.size() >= 2)
    {
        const double hz = parts[1].getDoubleValue();

        for (int i = 0; i < n; ++i)
        {
            const auto v = (float) (0.25 * std::sin (2.0 * juce::MathConstants<double>::pi * hz * i / sampleRate));

            for (int ch = 0; ch < channels; ++ch)
                buffer.setSample (ch, i, v);
        }

        return true;
    }

    if (kind == "noise" && parts.size() >= 2)
    {
        juce::Random random (0x7ba);
        const int len = juce::jmin (n, (int) (parts[1].getDoubleValue() * 0.001 * sampleRate));

        for (int i = 0; i < len; ++i)
        {
            const float v = 0.25f * (random.nextFloat() * 2.0f - 1.0f);

            for (int ch = 0; ch < channels; ++ch)
                buffer.setSample (ch, i, v);
        }

        return true;
    }

    return false;
}

bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    file.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());

    if (stream == nullptr)
        return false;

    // On success the writer takes the stream.
    const auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                            .withSampleRate (sampleRate)
                                                            .withNumChannels (buffer.getNumChannels())
                                                            .withBitsPerSample (32)
                                                            .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));

    if (writer == nullptr)
        return false;

    return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf ("usage: tubamp_fxrender reverb|delay out.wav [key=value ...]\n");
        return 2;
    }

    const std::string engine = argv[1];
    const juce::File outFile = juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]);
    Args args;

    for (int i = 3; i < argc; ++i)
    {
        const std::string token = argv[i];
        const auto eq = token.find ('=');

        if (eq == std::string::npos)
        {
            std::printf ("bad argument '%s' (expected key=value)\n", token.c_str());
            return 2;
        }

        args[token.substr (0, eq)] = token.substr (eq + 1);
    }

    const double sampleRate = num (args, "sr", 48000.0f);
    const int blockSize = (int) num (args, "block", 512.0f);
    const int channels = juce::jlimit (1, 2, (int) num (args, "channels", 2.0f));
    const int total = (int) (num (args, "seconds", 6.0f) * sampleRate);
    const int warmup = (int) (num (args, "warmup", 1.0f) * sampleRate);

    juce::AudioBuffer<float> signal (channels, total);
    signal.clear();

    if (! makeStimulus (str (args, "stim", "impulse"), signal, sampleRate))
    {
        std::printf ("unknown stim '%s'\n", str (args, "stim", "impulse").c_str());
        return 2;
    }

    const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) channels };
    tubamp::ReverbFx reverb;
    tubamp::DelayFx delay;

    // Defaults are the engines' own (the struct initialisers), except mix: a measurement
    // wants the wet signal alone unless it is the mix law being measured.
    tubamp::ReverbParams reverbParams;
    reverbParams.type = (int) num (args, "type", (float) tubamp::ReverbFx::Type::plate);
    reverbParams.size = num (args, "size", reverbParams.size);
    reverbParams.damping = num (args, "damping", reverbParams.damping);
    reverbParams.mix = num (args, "mix", 1.0f);
    reverbParams.width = num (args, "width", reverbParams.width);
    reverbParams.preDelayMs = num (args, "predelay", reverbParams.preDelayMs);
    reverbParams.decaySeconds = num (args, "decay", reverbParams.decaySeconds);
    reverbParams.lowCutHz = num (args, "lowcut", reverbParams.lowCutHz);
    reverbParams.highCutHz = num (args, "highcut", reverbParams.highCutHz);
    reverbParams.mod = num (args, "mod", reverbParams.mod);

    tubamp::DelayParams delayParams;
    delayParams.type = (int) num (args, "type", (float) delayParams.type);
    delayParams.timeMs = num (args, "time", delayParams.timeMs);
    delayParams.feedback = num (args, "feedback", delayParams.feedback);
    delayParams.mix = num (args, "mix", 1.0f);
    delayParams.mode = (int) num (args, "mode", 0.0f);
    delayParams.ratioPct = num (args, "ratio", delayParams.ratioPct);
    delayParams.width = num (args, "width", delayParams.width);
    delayParams.sync = (int) num (args, "sync", 0.0f);
    delayParams.bpm = (double) num (args, "bpm", 120.0f);
    delayParams.lowCutHz = num (args, "lowcut", delayParams.lowCutHz);
    delayParams.highCutHz = num (args, "highcut", delayParams.highCutHz);
    delayParams.mod = num (args, "mod", delayParams.mod);
    delayParams.color = num (args, "color", delayParams.color);
    delayParams.duck = num (args, "duck", 0.0f); // off unless it is ducking being measured

    // `time2=<ms> at=<seconds>` switches the delay time mid-render, to capture how a
    // time change sounds (repitch glide versus crossfade).
    const float time2 = num (args, "time2", -1.0f);
    const int switchAt = (int) (num (args, "at", 1.0f) * sampleRate);
    int rendered = 0;

    const auto applyParameters = [&]
    {
        if (engine == "reverb")
        {
            reverb.setParameters (reverbParams);
        }
        else
        {
            if (time2 > 0.0f && rendered >= switchAt)
                delayParams.timeMs = time2;

            delay.setParameters (delayParams);
        }
    };

    if (engine == "reverb")
        reverb.prepare (spec);
    else if (engine == "delay")
        delay.prepare (spec);
    else
    {
        std::printf ("unknown engine '%s'\n", engine.c_str());
        return 2;
    }

    const auto run = [&] (juce::AudioBuffer<float>& buffer)
    {
        for (int start = 0; start < buffer.getNumSamples(); start += blockSize)
        {
            const int len = juce::jmin (blockSize, buffer.getNumSamples() - start);
            juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), (size_t) channels,
                                                (size_t) start, (size_t) len);
            applyParameters();

            if (engine == "reverb")
                reverb.process (block);
            else
                delay.process (block);

            rendered += len;
        }
    };

    // Silence first, so every parameter smoother has settled before the stimulus lands.
    juce::AudioBuffer<float> silence (channels, warmup);
    silence.clear();
    run (silence);
    rendered = 0; // `at=` counts from the stimulus, not from the warm-up
    run (signal);

    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < total; ++i)
            if (! std::isfinite (signal.getSample (ch, i)))
            {
                std::printf ("FAIL: non-finite sample at ch %d index %d\n", ch, i);
                return 1;
            }

    if (! writeWav (outFile, signal, sampleRate))
    {
        std::printf ("could not write %s\n", outFile.getFullPathName().toRawUTF8());
        return 1;
    }

    std::printf ("wrote %s (%d ch, %d samples, peak %.4f)\n", outFile.getFullPathName().toRawUTF8(),
                 channels, total, (double) signal.getMagnitude (0, total));
    return 0;
}
