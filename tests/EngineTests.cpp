// NAM / IR engine behaviour: everything that has to match the reference TONE3000
// plugin (levels, lengths, the stereo rule, click-free swaps) plus the session's
// embedded model/IR files. Part of tubamp_smoke; see ProcessorSmokeTest.cpp for main().
#include <juce_events/juce_events.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "../src/PluginProcessor.h"
#include "../src/library/ToneSidecar.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <vector>

namespace
{
using tubamp::chain::BlockId;
using Proc = tubamp::TubampAudioProcessor;

constexpr double kPi = juce::MathConstants<double>::pi;

bool expect (bool condition, const char* what)
{
    std::printf ("  %s  %s\n", condition ? "ok  " : "FAIL", what);
    return condition;
}

void setBool (Proc& proc, const char* id, bool value)
{
    if (auto* p = proc.apvts.getParameter (id))
        p->setValueNotifyingHost (value ? 1.0f : 0.0f);
}

void setFloat (Proc& proc, const char* id, float raw)
{
    if (auto* p = proc.apvts.getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (raw));
}

double toDb (double ratio) { return 20.0 * std::log10 (std::max (ratio, 1.0e-12)); }

bool writeWav (const juce::File& file, const std::vector<std::vector<float>>& channels,
               double sampleRate, int bits = 24)
{
    file.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());

    if (stream == nullptr)
        return false;

    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                      .withSampleRate (sampleRate)
                                                      .withNumChannels ((int) channels.size())
                                                      .withBitsPerSample (bits));

    if (writer == nullptr)
        return false;

    const int numFrames = (int) channels.front().size();
    juce::AudioBuffer<float> buffer ((int) channels.size(), numFrames);

    for (size_t ch = 0; ch < channels.size(); ++ch)
        buffer.copyFrom ((int) ch, 0, channels[ch].data(), numFrames);

    return writer->writeFromAudioSampleBuffer (buffer, 0, numFrames);
}

std::vector<float> impulseKernel (int length, float amplitude, int at = 0)
{
    std::vector<float> kernel ((size_t) length, 0.0f);
    kernel[(size_t) at] = amplitude;
    return kernel;
}

/** A continuous stereo sine through the processor: both channels carry the same
    signal, the phase runs on across calls, and each channel's output is appended. */
struct SineRun
{
    SineRun (Proc& p, double sr, int bs, float amp = 0.3f, double hz = 220.0)
        : proc (p), sampleRate (sr), blockSize (bs), amplitude (amp), frequency (hz) {}

    void run (int numBlocks)
    {
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        for (int b = 0; b < numBlocks; ++b)
        {
            for (int i = 0; i < blockSize; ++i, ++position)
            {
                const auto v = amplitude * (float) std::sin (2.0 * kPi * frequency * (double) position / sampleRate);
                buffer.setSample (0, i, v);
                buffer.setSample (1, i, v);
            }

            proc.processBlock (buffer, midi);
            left.insert (left.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
            right.insert (right.end(), buffer.getReadPointer (1), buffer.getReadPointer (1) + blockSize);
        }
    }

    /** Durations are in seconds, not blocks: 64-sample blocks last a third of a
        millisecond at 192 kHz, and a fade lasts 25 ms at any rate. */
    void runFor (double seconds)
    {
        run ((int) std::ceil (seconds * sampleRate / (double) blockSize));
    }

    /** Index of the first sample of the last `seconds` captured. */
    size_t tail (double seconds = 0.03) const
    {
        const auto n = (size_t) (seconds * sampleRate);
        return left.size() > n ? left.size() - n : 0;
    }

    void forget() { left.clear(); right.clear(); }

    static double rms (const std::vector<float>& x, size_t from = 0)
    {
        double sum = 0.0;

        for (size_t i = from; i < x.size(); ++i)
            sum += (double) x[i] * x[i];

        return x.size() > from ? std::sqrt (sum / (double) (x.size() - from)) : 0.0;
    }

    static double peak (const std::vector<float>& x, size_t from = 0)
    {
        double p = 0.0;

        for (size_t i = from; i < x.size(); ++i)
            p = std::max (p, (double) std::abs (x[i]));

        return p;
    }

    /** Largest step between neighbouring samples: what a click is made of. */
    static double maxStep (const std::vector<float>& x)
    {
        double step = 0.0;

        for (size_t i = 1; i < x.size(); ++i)
            step = std::max (step, (double) std::abs (x[i] - x[i - 1]));

        return step;
    }

    /** True when some run of `length` samples stays below `level`. Callers give the
        length as a time (a millisecond): a sine's own zero crossing is a handful of
        quiet samples too, and more of them the higher the rate. */
    static bool hasQuietRun (const std::vector<float>& x, double level, int length)
    {
        int run = 0;

        for (float v : x)
        {
            run = std::abs (v) < level ? run + 1 : 0;

            if (run >= length)
                return true;
        }

        return false;
    }

    Proc& proc;
    double sampleRate;
    int blockSize;
    float amplitude;
    double frequency;
    juce::int64 position = 0;
    std::vector<float> left, right;
};

/** A processor whose chain is just the cab, wide open, so what is measured is the
    convolution and its gain. */
void makeCabOnly (Proc& proc, double sampleRate, int blockSize)
{
    proc.setChainOrder ({ BlockId::cab }, {});
    setBool (proc, params::cabOn, true);
    setFloat (proc, params::cabLowCut, 20.0f);
    setFloat (proc, params::cabHighCut, 20000.0f);
    setFloat (proc, params::cabMix, 100.0f);
    proc.prepareToPlay (sampleRate, blockSize);
}

/** Settled RMS of a 1 kHz sine through the processor, channel 0. */
double settledRms (Proc& proc, double sampleRate, int blockSize, std::vector<float>* rightOut = nullptr)
{
    SineRun run (proc, sampleRate, blockSize, 0.3f, 1000.0);
    run.runFor (0.4);
    run.forget();
    run.runFor (0.1);

    if (rightOut != nullptr)
        *rightOut = run.right;

    return SineRun::rms (run.left);
}

//==============================================================================
bool runIrLevelTests (const juce::File& dir, double sampleRate, int blockSize)
{
    std::printf ("ir levels:\n");
    bool ok = true;

    Proc proc;
    makeCabOnly (proc, sampleRate, blockSize);

    // No IR: the cab is out of the path and the sine passes untouched.
    const double dryRms = settledRms (proc, sampleRate, blockSize);

    // --- a quiet cabinet IR: never boosted, padded by 18 dB.
    // Files are written at the host rate so no resampling sits between the kernel
    // and the expected numbers.
    const auto quiet = dir.getChildFile ("quiet.wav");
    ok &= expect (writeWav (quiet, { impulseKernel (2400, 0.5f) }, sampleRate), "wrote a test IR");
    ok &= expect (proc.loadIr (quiet).isEmpty(), "a mono .wav loads");

    const double quietRms = settledRms (proc, sampleRate, blockSize);
    const double quietDb = toDb (quietRms / dryRms);
    std::printf ("  quiet cab IR: %.2f dB re dry (want %.2f)\n", quietDb, toDb (0.5) - 18.0);
    ok &= expect (std::abs (quietDb - (toDb (0.5) - 18.0)) < 0.3,
                  "an IR below unit energy is not boosted: its own level, minus the 18 dB cab pad");

    auto info = proc.cab.getInfo();
    ok &= expect (info.channels == 1 && ! info.reverbLike && ! info.truncated,
                  "...and is reported as a one-channel cabinet IR");
    ok &= expect (proc.cab.getImage() == tubamp::CabSim::Image::monoFile, "...applied as a mono file");

    // --- a hot IR: cut down to unit energy (referred to 48 kHz), then padded.
    const std::vector<float> hotKernel = [] { std::vector<float> k (2400, 0.0f); k[0] = k[1] = k[2] = k[3] = 0.9f; return k; }();
    const auto hot = dir.getChildFile ("hot.wav");
    writeWav (hot, { hotKernel }, sampleRate);
    ok &= expect (proc.loadIr (hot).isEmpty(), "a hot IR loads");

    double re = 0.0, im = 0.0;

    for (int k = 0; k < 4; ++k)
    {
        re += 0.9 * std::cos (2.0 * kPi * 1000.0 * k / sampleRate);
        im -= 0.9 * std::sin (2.0 * kPi * 1000.0 * k / sampleRate);
    }

    const double response = std::sqrt (re * re + im * im);
    const double l2 = std::sqrt (4.0 * 0.81 * sampleRate / 48000.0);
    const double wantHotDb = toDb (response / l2) - 18.0;
    const double hotDb = toDb (settledRms (proc, sampleRate, blockSize) / dryRms);
    std::printf ("  hot cab IR: %.2f dB re dry (want %.2f)\n", hotDb, wantHotDb);
    ok &= expect (std::abs (hotDb - wantHotDb) < 0.3, "an IR above unit energy is normalized to it, then padded");

    // --- reverb-like: the same impulse with a tail past one second gets no pad.
    auto longKernel = impulseKernel ((int) (1.5 * sampleRate), 0.5f);
    longKernel.back() = 0.01f; // above the -80 dB trim, so the length survives
    const auto room = dir.getChildFile ("room.wav");
    writeWav (room, { longKernel }, sampleRate);
    const auto roomStarted = juce::Time::getMillisecondCounterHiRes();
    ok &= expect (proc.loadIr (room).isEmpty(), "a 1.5 s IR loads");
    std::printf ("  loading a 1.5 s mono IR took %.0f ms\n", juce::Time::getMillisecondCounterHiRes() - roomStarted);

    const double roomDb = toDb (settledRms (proc, sampleRate, blockSize) / dryRms);
    std::printf ("  1.5 s IR: %.2f dB re dry (want %.2f)\n", roomDb, toDb (0.5));
    ok &= expect (std::abs (roomDb - toDb (0.5)) < 0.3, "an IR longer than a second is reverb-like: no cab pad");
    ok &= expect (proc.cab.getInfo().reverbLike && proc.cab.getInfo().seconds > 1.0, "...and is reported as such");

    // --- TONE3000's gear tag overrides the length.
    tubamp::ToneSidecar tag;
    tag.toneId = 7;
    tag.gear = "space";
    ok &= expect (tag.write (quiet), "wrote a sidecar");
    proc.loadIr (quiet);
    const double taggedSpaceDb = toDb (settledRms (proc, sampleRate, blockSize) / dryRms);
    ok &= expect (std::abs (taggedSpaceDb - toDb (0.5)) < 0.3, "a short IR tagged \"space\" is treated as reverb-like");

    tag.gear = "cab";
    tag.write (room);
    proc.loadIr (room);
    const double taggedCabDb = toDb (settledRms (proc, sampleRate, blockSize) / dryRms);
    ok &= expect (std::abs (taggedCabDb - (toDb (0.5) - 18.0)) < 0.3, "a long IR tagged \"cab\" gets the cab pad");
    tubamp::ToneSidecar::remove (quiet);
    tubamp::ToneSidecar::remove (room);

    // --- dry/wet.
    proc.loadIr (quiet);
    setFloat (proc, params::cabMix, 0.0f);
    ok &= expect (std::abs (toDb (settledRms (proc, sampleRate, blockSize) / dryRms)) < 0.01, "mix 0 % is the dry signal");

    setFloat (proc, params::cabMix, 50.0f);
    const double halfDb = toDb (settledRms (proc, sampleRate, blockSize) / dryRms);
    const double wantHalfDb = toDb (0.5 + 0.5 * 0.5 * std::pow (10.0, -18.0 / 20.0));
    ok &= expect (std::abs (halfDb - wantHalfDb) < 0.2, "mix 50 % is half dry plus half convolved");
    setFloat (proc, params::cabMix, 100.0f);

    // --- the 10 s limit, and the tail the host is told about. A low file rate
    // keeps the fixture small; the limit is in seconds.
    auto endless = impulseKernel (12 * 8000, 0.5f);

    for (size_t i = 1; i < endless.size(); i += 400)
        endless[i] = 0.02f;

    const auto endlessFile = dir.getChildFile ("endless.wav");
    writeWav (endlessFile, { endless }, 8000.0, 16);
    ok &= expect (proc.loadIr (endlessFile).isEmpty(), "a 12 s IR loads");
    info = proc.cab.getInfo();
    std::printf ("  12 s file: kernel %.2f s, truncated=%d, tail=%.2f s\n",
                 info.seconds, (int) info.truncated, proc.getTailLengthSeconds());
    ok &= expect (info.truncated && info.seconds <= 10.01 && info.seconds > 9.0, "...and is cut at 10 s");
    ok &= expect (proc.getTailLengthSeconds() >= info.seconds, "the host's tail covers the loaded IR");

    // --- failures leave the IR that was playing in place.
    proc.loadIr (quiet);
    const auto junk = dir.getChildFile ("junk.wav");
    junk.replaceWithText ("this is not audio");
    ok &= expect (proc.loadIr (junk).isNotEmpty(), "junk bytes are refused with a message");
    ok &= expect (proc.loadIr (dir.getChildFile ("nope.wav")).isNotEmpty(), "a missing file is refused with a message");
    ok &= expect (proc.getLoadedIrPath() == quiet.getFullPathName() && proc.cab.hasImpulseResponse(),
                  "...and the previous IR stays loaded");

    // --- WAVs whose odd-sized data chunk lacks its pad byte (some catalog files).
    const auto padded = dir.getChildFile ("odd.wav");
    writeWav (padded, { impulseKernel (2401, 0.5f) }, sampleRate, 24);
    juce::MemoryBlock bytes;
    padded.loadFileAsData (bytes);
    const auto padless = dir.getChildFile ("odd-padless.wav");
    padless.replaceWithData (bytes.getData(), bytes.getSize() - 1);
    const auto padlessError = proc.loadIr (padless);

    if (padlessError.isNotEmpty())
        std::printf ("        %s\n", padlessError.toRawUTF8());

    ok &= expect (padlessError.isEmpty(), "a WAV with a pad-less odd data chunk loads");
    ok &= expect (std::abs (toDb (settledRms (proc, sampleRate, blockSize) / dryRms) - quietDb) < 0.3,
                  "...and sounds like its well-formed twin");

    return ok;
}

bool runIrStereoTests (const juce::File& dir, double sampleRate, int blockSize)
{
    std::printf ("ir stereo:\n");
    bool ok = true;

    Proc proc;
    makeCabOnly (proc, sampleRate, blockSize);
    const double dryRms = settledRms (proc, sampleRate, blockSize);

    // Left kernel +0.5, right kernel -0.25: the two are told apart by level and sign.
    const auto stereo = dir.getChildFile ("stereo.wav");
    writeWav (stereo, { impulseKernel (2400, 0.5f), impulseKernel (2400, -0.25f) }, sampleRate);
    const auto loadStarted = juce::Time::getMillisecondCounterHiRes();
    ok &= expect (proc.loadIr (stereo).isEmpty(), "a stereo .wav loads");
    std::printf ("  loading a 50 ms stereo IR took %.0f ms\n", juce::Time::getMillisecondCounterHiRes() - loadStarted);
    ok &= expect (proc.cab.getInfo().channels == 2, "...with two kernels");

    // Both kernels count towards the energy, referred to 48 kHz: at high rates this
    // pair is above unit energy and is cut (attenuation only, never a boost).
    const double norm = std::min (1.0, 1.0 / std::sqrt ((0.25 + 0.0625) * sampleRate / 48000.0));
    const double pad = norm * std::pow (10.0, -18.0 / 20.0);
    std::vector<float> right;
    const double leftRms = settledRms (proc, sampleRate, blockSize, &right);
    const double rightRms = SineRun::rms (right);
    std::printf ("  true stereo: L %.2f dB, R %.2f dB re dry\n", toDb (leftRms / dryRms), toDb (rightRms / dryRms));
    ok &= expect (std::abs (toDb (leftRms / dryRms) - toDb (0.5 * pad)) < 0.3
                  && std::abs (toDb (rightRms / dryRms) - toDb (0.25 * pad)) < 0.3,
                  "with nothing after the cab, L runs through the left kernel and R through the right");
    ok &= expect (proc.cab.getImage() == tubamp::CabSim::Image::stereo, "...reported as true stereo");

    // An enabled amp after the cab folds the path to mono. With true stereo the fold
    // would be (0.5 - 0.25) / 2; with the left kernel on both channels it is 0.5.
    proc.setChainOrder ({ BlockId::cab, BlockId::amp }, {});
    setBool (proc, params::ampOn, true);
    setFloat (proc, params::ampInput, 0.0f);
    setFloat (proc, params::ampOutput, 0.0f);
    proc.prepareToPlay (sampleRate, blockSize);

    const double foldedDb = toDb (settledRms (proc, sampleRate, blockSize) / dryRms);
    std::printf ("  cab before amp: %.2f dB re dry (left kernel %.2f, true-stereo fold %.2f)\n",
                 foldedDb, toDb (0.5 * pad), toDb (0.125 * pad));
    ok &= expect (std::abs (foldedDb - toDb (0.5 * pad)) < 0.3,
                  "with an amp after the cab, the left kernel is used on both channels");
    ok &= expect (proc.cab.getImage() == tubamp::CabSim::Image::monoPath, "...reported as mono because of the path");

    // A bypassed amp folds nothing.
    setBool (proc, params::ampOn, false);
    settledRms (proc, sampleRate, blockSize);
    ok &= expect (proc.cab.getImage() == tubamp::CabSim::Image::stereo, "a bypassed amp after the cab does not count");
    setBool (proc, params::ampOn, true);

    return ok;
}

bool runIrBlockSizeTests (const juce::File& dir, double sampleRate)
{
    std::printf ("ir block size:\n");
    bool ok = true;

    // A dense 60 ms kernel and a noise input: nothing in either lines up with a
    // partition size by accident.
    juce::Random random (1234);
    std::vector<float> kernel ((size_t) (0.06 * sampleRate));

    for (size_t i = 0; i < kernel.size(); ++i)
        kernel[i] = 0.2f * (random.nextFloat() * 2.0f - 1.0f) * std::exp (-6.0f * (float) i / (float) kernel.size());

    const auto file = dir.getChildFile ("dense.wav");
    writeWav (file, { kernel }, sampleRate);

    const int total = 24000;
    std::vector<float> input ((size_t) total);

    for (auto& v : input)
        v = 0.2f * (random.nextFloat() * 2.0f - 1.0f);

    const auto render = [&] (int blockSize)
    {
        Proc proc;
        proc.setChainOrder ({ BlockId::cab }, {});
        setBool (proc, params::cabOn, true);
        proc.loadIr (file);
        proc.prepareToPlay (sampleRate, blockSize);

        std::vector<float> out;
        juce::MidiBuffer midi;

        for (int offset = 0; offset < total;)
        {
            const int n = std::min (blockSize, total - offset);
            juce::AudioBuffer<float> buffer (2, n);
            buffer.copyFrom (0, 0, input.data() + offset, n);
            buffer.copyFrom (1, 0, input.data() + offset, n);
            proc.processBlock (buffer, midi);
            out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + n);
            offset += n;
        }

        return out;
    };

    const auto reference = render (64);
    const double peak = SineRun::peak (reference);
    ok &= expect (peak > 1.0e-4, "the reference render is not silent");

    for (int blockSize : { 256, 512, 1000, 4096 })
    {
        const auto other = render (blockSize);
        double worst = 0.0;

        for (size_t i = 0; i < reference.size(); ++i)
            worst = std::max (worst, (double) std::abs (reference[i] - other[i]));

        std::printf ("  block %d vs 64: max diff %.2e (peak %.3f)\n", blockSize, worst, peak);
        ok &= expect (worst <= peak * 1.0e-4, "the convolved signal does not depend on the host block size");
    }

    return ok;
}

bool runIrSwapTests (const juce::File& dir, double sampleRate)
{
    std::printf ("ir swap:\n");
    bool ok = true;
    const int blockSize = 64;

    Proc proc;
    makeCabOnly (proc, sampleRate, blockSize);

    const auto first = dir.getChildFile ("swap-a.wav");
    const auto second = dir.getChildFile ("swap-b.wav");
    writeWav (first, { impulseKernel (2400, 0.5f) }, sampleRate);
    writeWav (second, { impulseKernel (2400, 0.4f) }, sampleRate);

    // The dry sine is 16 times hotter than the convolved one, so a single sample of
    // it getting through, or a step, stands out.
    SineRun run (proc, sampleRate, blockSize, 0.5f, 220.0);
    const double drySlope = 0.5 * 2.0 * kPi * 220.0 / sampleRate;
    const double fadeSlope = 1.0 / (0.025 * sampleRate);

    // Long enough for the DC blocker's start-up transient to be gone at any rate.
    run.runFor (1.0);
    ok &= expect (std::abs (SineRun::peak (run.left, run.tail()) - 0.5) < 0.01,
                  "no IR: the cab passes the dry signal");

    // --- first IR into an empty block: the dry signal closes, the IR opens.
    run.forget();
    ok &= expect (proc.loadIr (first).isEmpty(), "IR A loads while audio runs");
    run.runFor (0.35);
    const double wetPeak = SineRun::peak (run.left, run.tail());
    std::printf ("  first load: settled peak %.4f, largest step %.5f (dry slope %.5f)\n",
                 wetPeak, SineRun::maxStep (run.left), drySlope);
    ok &= expect (std::abs (wetPeak - 0.5 * 0.5 * std::pow (10.0, -18.0 / 20.0)) < 0.003, "...and settles on the convolved level");
    ok &= expect (SineRun::maxStep (run.left) <= 1.2 * drySlope + 0.5 * fadeSlope, "...without a step in the waveform");

    // --- IR to IR: the convolved signal closes, swaps, opens. Never the dry signal.
    run.forget();
    ok &= expect (proc.loadIr (second).isEmpty(), "IR B loads while IR A plays");
    run.runFor (0.35);
    const double swapPeak = SineRun::peak (run.left);
    const double settled = SineRun::peak (run.left, run.tail());
    std::printf ("  A -> B: peak during swap %.4f (steady %.4f), settled %.4f\n", swapPeak, wetPeak, settled);
    ok &= expect (swapPeak <= 1.25 * wetPeak, "swapping IRs never exposes the un-cabbed signal");
    ok &= expect (SineRun::hasQuietRun (run.left, 0.02 * wetPeak, (int) (0.001 * sampleRate)), "...the swap happens in silence");
    ok &= expect (std::abs (settled - wetPeak * 0.8) < 0.002, "...and settles on the new IR");
    ok &= expect (SineRun::maxStep (run.left) <= 1.2 * drySlope * (wetPeak / 0.5) + wetPeak * fadeSlope * 1.5,
                  "...without a step in the waveform");
    ok &= expect (proc.cab.getInfo().name == "swap-b", "the loaded IR is reported");

    // --- clearing: the convolved signal closes, the dry signal opens.
    run.forget();
    proc.clearIr();
    run.runFor (0.35);
    ok &= expect (std::abs (SineRun::peak (run.left, run.tail()) - 0.5) < 0.01, "clearing the IR returns to the dry signal");
    ok &= expect (SineRun::maxStep (run.left) <= 1.2 * drySlope + 0.5 * fadeSlope, "...without a step in the waveform");
    ok &= expect (! proc.cab.hasImpulseResponse() && proc.getLoadedIrPath().isEmpty(), "...and the block reports no IR");

    // --- a bypassed cab swaps at once: nothing to fade, nothing left pending.
    proc.loadIr (first);
    run.runFor (0.15);
    setBool (proc, params::cabOn, false);
    run.run (4);
    proc.loadIr (second);
    run.run (2);
    setBool (proc, params::cabOn, true);
    run.forget();
    run.runFor (0.3);
    ok &= expect (std::abs (SineRun::peak (run.left, run.tail()) - wetPeak * 0.8) < 0.002,
                  "an IR loaded while the cab is bypassed is the one that plays when it comes back");

    return ok;
}

//==============================================================================
bool runNamSwapTests (const juce::File& modelFile, double sampleRate)
{
    std::printf ("nam swap:\n");
    bool ok = true;
    const int blockSize = 64;

    const auto other = modelFile.getSiblingFile ("wavenet.nam");

    if (! other.existsAsFile())
        return expect (true, "skipped: no second model next to the test model");

    Proc proc;
    proc.setChainOrder ({ BlockId::amp }, {});
    setBool (proc, params::ampOn, true);
    proc.prepareToPlay (sampleRate, blockSize);

    // Long enough for the DC blocker's start-up transient to be gone: the dry level
    // measured here is what a cleared model has to come back to.
    SineRun run (proc, sampleRate, blockSize, 0.3f, 220.0);
    run.runFor (1.0);
    const double dryPeak = SineRun::peak (run.left, run.tail());

    // --- first model: the unprocessed signal closes, the model opens.
    run.forget();
    const auto loadStarted = juce::Time::getMillisecondCounterHiRes();
    ok &= expect (proc.loadModel (modelFile).isEmpty(), "model A loads while audio runs");
    std::printf ("  loading %s took %.0f ms\n", modelFile.getFileName().toRawUTF8(),
                 juce::Time::getMillisecondCounterHiRes() - loadStarted);
    run.runFor (0.4);
    const double peakA = SineRun::peak (run.left, run.tail());
    ok &= expect (proc.namEngine.hasLiveModel(), "...and goes live");
    std::printf ("  first load: dry %.4f, steady A %.4f, peak during %.4f\n", dryPeak, peakA, SineRun::peak (run.left));

    // The level checks below compare a transition against the model's steady level,
    // which only means something for a model that has one. Some fixtures do not: a
    // recurrent model thumps for a long time after its input starts, an untrained
    // one wanders. That is the model's own output, long after any fade of ours, so
    // for those the checks would be measuring the model and are left out.
    const bool steady = SineRun::peak (run.left, (size_t) (0.1 * sampleRate)) <= 1.15 * peakA;

    if (std::getenv ("TUBAMP_TEST_ENVELOPE") != nullptr)
    {
        std::printf ("  envelope per 10 ms after first load:");
        const auto step = (size_t) (0.01 * sampleRate);

        for (size_t from = 0; from + step <= run.left.size() && from < 30 * step; from += step)
        {
            double p = 0.0;

            for (size_t i = from; i < from + step; ++i)
                p = std::max (p, (double) std::abs (run.left[i]));

            std::printf (" %.2f", p);
        }

        std::printf ("\n");
    }

    if (! steady)
        std::printf ("  note: this model's own output is still settling 100 ms after it starts; "
                     "level checks against its steady level are skipped\n");

    ok &= expect (! steady || SineRun::peak (run.left) <= 1.25 * std::max (dryPeak, peakA),
                  "...without overshooting either signal");

    // --- model to model.
    run.forget();
    ok &= expect (proc.loadModel (other).isEmpty(), "model B loads while model A plays");
    ok &= expect (proc.namEngine.hasLiveModel(), "model A stays live until the swap");
    run.runFor (0.4);
    const double peakB = SineRun::peak (run.left, run.tail());
    const double during = SineRun::peak (run.left);
    std::printf ("  A -> B: steady A %.4f, steady B %.4f, peak during swap %.4f\n", peakA, peakB, during);
    ok &= expect (! steady || during <= 1.25 * std::max (peakA, peakB), "a model swap never exceeds the louder model");
    ok &= expect (SineRun::hasQuietRun (run.left, 0.03 * std::max (peakA, peakB), (int) (0.001 * sampleRate)), "...and happens in silence");
    ok &= expect (proc.namEngine.getModelInfo().name == other.getFileNameWithoutExtension(), "the new model is the one reported");
    ok &= expect (proc.namEngine.getFaultCount() == 0, "no block had to be silenced");

    // --- clearing: the model closes, the dry signal opens.
    run.forget();
    proc.clearModel();
    run.runFor (0.4);
    std::printf ("  cleared: settled peak %.4f (dry %.4f)\n", SineRun::peak (run.left, run.tail()), dryPeak);
    ok &= expect (std::abs (SineRun::peak (run.left, run.tail()) - dryPeak) < 0.01 * dryPeak + 1.0e-4,
                  "clearing the model returns to the dry signal");
    ok &= expect (SineRun::peak (run.left) <= 1.25 * std::max (dryPeak, peakB), "...without overshooting either signal");
    ok &= expect (! proc.namEngine.hasLiveModel() && ! proc.namEngine.hasModel(), "...and the engine reports no model");

    // --- a bypassed amp swaps at once.
    proc.loadModel (modelFile);
    run.runFor (0.15);
    setBool (proc, params::ampOn, false);
    run.run (4);
    proc.loadModel (other);
    run.run (1);
    ok &= expect (proc.namEngine.getModelInfo().name == other.getFileNameWithoutExtension()
                  && proc.namEngine.hasLiveModel(),
                  "a model loaded while the amp is bypassed is adopted within a block");
    setBool (proc, params::ampOn, true);

    // --- garbage in: silence out, and the model is not poisoned by it.
    {
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;
        bool finite = true;

        for (int block = 0; block < 6; ++block)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                    buffer.setSample (ch, i, block == 1 ? std::numeric_limits<float>::quiet_NaN()
                                                        : block == 2 ? std::numeric_limits<float>::infinity() : 0.1f);

            proc.processBlock (buffer, midi);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                    finite = finite && std::isfinite (buffer.getSample (ch, i));
        }

        ok &= expect (finite, "NaN and Inf at the input never reach the output");
    }

    run.forget();
    run.runFor (0.3);
    std::printf ("  after garbage in: peak %.4f (steady B %.4f), faults %d\n",
                 SineRun::peak (run.left, run.tail()), peakB, proc.namEngine.getFaultCount());
    ok &= expect (! steady || std::abs (SineRun::peak (run.left, run.tail()) - peakB) < 0.05 * peakB,
                  "...and the model plays on normally afterwards");

    return ok;
}

/** A very quiet model is normalized up by 20 dB. That gain belongs to the model: the
    unprocessed signal before it loads, and after it is cleared, must never be played
    through it — which is what a gain that merely ramps from one model's value to the
    next would do for the length of the ramp. */
bool runNamGainHandoverTests (const juce::File& modelFile, double sampleRate)
{
    std::printf ("nam gain handover:\n");
    bool ok = true;

    const auto quiet = modelFile.getSiblingFile ("lstm.nam");

    if (! quiet.existsAsFile())
        return expect (true, "skipped: no lstm.nam next to the test model");

    Proc proc;
    proc.setChainOrder ({ BlockId::amp }, {});
    setBool (proc, params::ampOn, true);
    proc.prepareToPlay (sampleRate, 64);

    SineRun run (proc, sampleRate, 64, 0.3f, 220.0);
    run.runFor (1.0);
    const double dryPeak = SineRun::peak (run.left, run.tail());

    run.forget();
    ok &= expect (proc.loadModel (quiet).isEmpty(), "a model 20 dB under the target loads");
    run.runFor (0.5);
    const double wetPeak = SineRun::peak (run.left, run.tail());
    const double compensation = proc.namEngine.getOutputCompensationDb (params::OutputMode::normalized, 12.0f);
    std::printf ("  compensation %+.1f dB; dry %.3f, model %.3f, peak while loading %.3f\n",
                 (double) compensation, dryPeak, wetPeak, SineRun::peak (run.left));
    ok &= expect (compensation > 15.0f, "...and is normalized up by more than 15 dB");
    ok &= expect (SineRun::peak (run.left) <= 1.25 * std::max (dryPeak, wetPeak),
                  "the unprocessed signal is never played at the incoming model's gain");

    run.forget();
    proc.clearModel();
    run.runFor (0.5);
    std::printf ("  peak while clearing %.3f\n", SineRun::peak (run.left));
    ok &= expect (SineRun::peak (run.left) <= 1.25 * std::max (dryPeak, wetPeak),
                  "...nor at the outgoing model's gain when it is cleared");
    ok &= expect (std::abs (SineRun::peak (run.left, run.tail()) - dryPeak) < 0.01 * dryPeak + 1.0e-4,
                  "...and comes back at its own level");

    return ok;
}

bool runNamMetadataTests (const juce::File& modelFile, const juce::File& dir, double sampleRate)
{
    std::printf ("nam metadata:\n");
    bool ok = true;

    const auto source = modelFile.getSiblingFile ("wavenet.nam");

    if (! source.existsAsFile())
        return expect (true, "skipped: no wavenet.nam next to the test model");

    const auto text = source.loadFileAsString();
    const juce::String loudnessKey = "\"loudness\": -20.020729064941406";
    ok &= expect (text.contains (loudnessKey), "the fixture model declares the loudness this test rewrites");

    const auto withLoudness = [&] (const char* name, const char* value)
    {
        const auto file = dir.getChildFile (name);
        file.replaceWithText (text.replace (loudnessKey, juce::String ("\"loudness\": ") + value));
        return file;
    };

    const auto compensation = [&] (const juce::File& file)
    {
        tubamp::NamEngine engine;
        engine.prepare (sampleRate, 256);
        const auto error = engine.loadModel (file);

        if (error.isNotEmpty())
            std::printf ("        %s\n", error.toRawUTF8());

        engine.applyStaging (false);
        return engine.getOutputCompensationDb (params::OutputMode::normalized, 12.0f);
    };

    const float plain = compensation (source);
    std::printf ("  normalization for -20.02 dB: %+.2f dB\n", (double) plain);
    ok &= expect (std::abs (plain - (-18.0f + 20.0207f)) < 0.01f, "normalization is target minus the model's loudness");

    ok &= expect (std::abs (compensation (withLoudness ("quiet.nam", "-70.0")) - 24.0f) < 0.001f,
                  "a very quiet model is boosted by 24 dB at most");
    ok &= expect (std::abs (compensation (withLoudness ("loud.nam", "-0.5")) + 17.5f) < 0.01f,
                  "a loud model is cut by what it takes");
    ok &= expect (compensation (withLoudness ("positive.nam", "40.0")) == 0.0f,
                  "a loudness above 0 dB is not believed: no compensation");
    ok &= expect (compensation (withLoudness ("absurd.nam", "-250.0")) == 0.0f,
                  "a loudness below -100 dB is not believed: no compensation");

    return ok;
}

bool runNamLatencyTests (const juce::File& modelFile, double sampleRate)
{
    std::printf ("nam latency:\n");
    bool ok = true;

    const auto model = modelFile.getSiblingFile ("wavenet.nam");

    if (! model.existsAsFile())
        return expect (true, "skipped: no wavenet.nam next to the test model");

    // Where a small pulse comes out, in samples after it went in. Small, so the
    // model stays close to linear; a quarter-millisecond Gaussian rather than one
    // sample, so it is the same band-limited signal at every rate; and measured
    // against the same engine run on silence, because a model's output for no input
    // is not zero.
    const auto pulseDelay = [&model] (double rate, int& reportedLatency)
    {
        const int block = 256, total = 16 * block;
        const double sigma = 0.00025 * rate;
        const double centre = 2.0 * block + 0.25;

        const auto render = [&] (float amplitude)
        {
            tubamp::NamEngine engine;
            engine.prepare (rate, block);
            engine.loadModel (model);
            engine.applyStaging (false);
            reportedLatency = engine.getLatencySamples();

            std::vector<float> signal ((size_t) total, 0.0f);

            for (int n = 0; n < total; ++n)
            {
                const double x = ((double) n - centre) / sigma;
                signal[(size_t) n] = amplitude * (float) std::exp (-0.5 * x * x);
            }

            for (int offset = 0; offset < total; offset += block)
                engine.process (signal.data() + offset, block);

            return signal;
        };

        const auto idle = render (0.0f);
        const auto hit = render (0.02f);
        int peak = 0;

        for (int i = 1; i < total; ++i)
            if (std::abs (hit[(size_t) i] - idle[(size_t) i]) > std::abs (hit[(size_t) peak] - idle[(size_t) peak]))
                peak = i;

        // Refined between samples: the delay measured at 48 kHz is scaled up to the
        // host rate below, and whole-sample rounding would be scaled up with it.
        double refined = (double) peak;

        if (peak > 0 && peak < total - 1)
        {
            const auto at = [&] (int i) { return (double) std::abs (hit[(size_t) i] - idle[(size_t) i]); };
            const double curvature = at (peak - 1) - 2.0 * at (peak) + at (peak + 1);

            if (curvature < 0.0)
                refined += 0.5 * (at (peak - 1) - at (peak + 1)) / curvature;
        }

        return refined - centre;
    };

    int nativeLatency = 0, hostLatency = 0;
    const double nativeDelay = pulseDelay (48000.0, nativeLatency);
    ok &= expect (nativeLatency == 0, "a model at its own rate reports no latency");

    if (std::abs (sampleRate - 48000.0) < 1.0)
        return ok && expect (true, "resampler latency is only measurable off 48 kHz (run at 44100 or 96000)");

    const double hostDelay = pulseDelay (sampleRate, hostLatency);
    const double modelDelayAtHost = nativeDelay * sampleRate / 48000.0;
    std::printf ("  model delay %.1f smp @48k; at %.0f Hz the pulse arrives %.1f smp late, reported latency %d\n",
                 nativeDelay, sampleRate, hostDelay, hostLatency);
    ok &= expect (hostLatency > 0, "a resampled model reports latency");
    ok &= expect (std::abs ((hostDelay - modelDelayAtHost) - hostLatency) <= 1.0,
                  "the reported latency is where the signal actually arrives");

    return ok;
}

//==============================================================================
/** Strips the embedded files out of a state blob: what a session written before
    they existed looks like. */
juce::MemoryBlock withoutEmbeddedMedia (const juce::MemoryBlock& state)
{
    auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
    auto tree = juce::ValueTree::fromXml (*xml);

    for (int i = tree.getNumChildren(); --i >= 0;)
        if (tree.getChild (i).hasType (tubamp::EmbeddedMedia::treeType))
            tree.removeChild (i, nullptr);

    juce::MemoryBlock out;
    juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), out);
    return out;
}

bool runSessionMediaTests (const juce::File& modelFile, const juce::File& dir, double sampleRate, int blockSize)
{
    std::printf ("session media:\n");
    bool ok = true;

    // The files live outside the library, the way an imported-then-moved or a
    // foreign session's files do.
    const auto elsewhere = dir.getChildFile ("elsewhere");
    elsewhere.createDirectory();
    const auto model = elsewhere.getChildFile ("session-model.nam");
    const auto ir = elsewhere.getChildFile ("session-ir.wav");
    ok &= expect (modelFile.copyFileTo (model), "copied the fixture model");
    writeWav (ir, { impulseKernel (2400, 0.5f) }, sampleRate);

    tubamp::ToneSidecar sidecar;
    sidecar.toneId = 4242;
    sidecar.title = "Test Room";
    sidecar.creator = "someone";
    sidecar.gear = "space";
    sidecar.write (ir);

    juce::MemoryBlock state;

    {
        Proc proc;
        proc.prepareToPlay (sampleRate, blockSize);
        ok &= expect (proc.loadModel (model).isEmpty() && proc.loadIr (ir).isEmpty(), "model and IR load");
        proc.getStateInformation (state);
    }

    std::printf ("  state with files: %d kB (model %d kB)\n", (int) (state.getSize() / 1024), (int) (model.getSize() / 1024));
    ok &= expect ((juce::int64) state.getSize() > model.getSize() / 8, "the session state carries the files");

    juce::MemoryBlock modelBytes, irBytes;
    model.loadFileAsData (modelBytes);
    ir.loadFileAsData (irBytes);

    // --- same machine, files in place: nothing is written anywhere.
    {
        Proc proc;
        proc.prepareToPlay (sampleRate, blockSize);
        proc.setStateInformation (state.getData(), (int) state.getSize());
        ok &= expect (proc.getLoadedModelPath() == model.getFullPathName()
                      && proc.getLoadedIrPath() == ir.getFullPathName(),
                      "files that are where the session says are loaded from there");
        ok &= expect (! tubamp::ModelLibrary::getModelsDir().getChildFile (model.getFileName()).exists(),
                      "...and no copy is made");
    }

    // --- the files are gone: rebuilt into the library from the session.
    model.deleteFile();
    ir.deleteFile();
    tubamp::ToneSidecar::remove (ir);

    const auto rebuiltModel = tubamp::ModelLibrary::getModelsDir().getChildFile (model.getFileName());
    const auto rebuiltIr = tubamp::ModelLibrary::getIrsDir().getChildFile (ir.getFileName());

    {
        Proc proc;
        proc.prepareToPlay (sampleRate, blockSize);
        proc.setStateInformation (state.getData(), (int) state.getSize());

        ok &= expect (proc.namEngine.hasModel() && proc.getLoadedModelPath() == rebuiltModel.getFullPathName(),
                      "a missing model is rebuilt into the library and loaded");
        ok &= expect (proc.cab.hasImpulseResponse() && proc.getLoadedIrPath() == rebuiltIr.getFullPathName(),
                      "a missing IR is rebuilt into the library and loaded");

        juce::MemoryBlock a, b;
        rebuiltModel.loadFileAsData (a);
        rebuiltIr.loadFileAsData (b);
        ok &= expect (a == modelBytes && b == irBytes, "...byte for byte");
        ok &= expect (proc.cab.getInfo().gear == "space" && proc.cab.getInfo().reverbLike,
                      "...with what TONE3000 said about it, so the IR keeps its level");
        ok &= expect (proc.getMissingMediaPath (Proc::Media::model).isEmpty()
                      && proc.getMissingMediaPath (Proc::Media::ir).isEmpty(),
                      "nothing is reported missing");

        bool finite = true;
        SineRun run (proc, sampleRate, blockSize);
        run.run (20);

        for (float v : run.left)
            finite = finite && std::isfinite (v);

        ok &= expect (finite && SineRun::rms (run.left) > 1.0e-5, "...and the restored session makes sound");
    }

    // --- restoring again reuses the rebuilt files instead of piling up copies.
    {
        Proc proc;
        proc.prepareToPlay (sampleRate, blockSize);
        proc.setStateInformation (state.getData(), (int) state.getSize());
        ok &= expect (proc.getLoadedModelPath() == rebuiltModel.getFullPathName()
                      && ! tubamp::ModelLibrary::getModelsDir().getChildFile ("session-model 2.nam").exists(),
                      "a second restore reuses the identical library file");
    }

    // --- a different file of the same name is never overwritten.
    rebuiltModel.replaceWithText ("{ \"not\": \"the same model\" }");

    {
        Proc proc;
        proc.prepareToPlay (sampleRate, blockSize);
        proc.setStateInformation (state.getData(), (int) state.getSize());
        const auto second = tubamp::ModelLibrary::getModelsDir().getChildFile ("session-model 2.nam");
        ok &= expect (proc.getLoadedModelPath() == second.getFullPathName() && second.existsAsFile(),
                      "a name taken by other content gets the model under a free name");
        ok &= expect (rebuiltModel.loadFileAsString().contains ("not"), "...and the other file is left alone");
        second.deleteFile();
    }

    rebuiltModel.deleteFile();
    rebuiltIr.deleteFile();
    tubamp::ToneSidecar::remove (rebuiltIr);

    // --- a session without embedded files (written by an older build).
    const auto legacy = withoutEmbeddedMedia (state);
    juce::MemoryBlock resaved;

    {
        Proc proc;
        proc.prepareToPlay (sampleRate, blockSize);
        proc.setStateInformation (legacy.getData(), (int) legacy.getSize());

        ok &= expect (! proc.namEngine.hasModel() && proc.getLoadedModelPath().isEmpty(),
                      "a file that is nowhere leaves the slot empty");
        ok &= expect (proc.getMissingMediaPath (Proc::Media::model) == model.getFullPathName()
                      && proc.getMissingMediaPath (Proc::Media::ir) == ir.getFullPathName(),
                      "...and is reported as missing, by path");

        proc.getStateInformation (resaved);
    }

    {
        // The file comes back (drive mounted, library restored).
        juce::File (model).replaceWithData (modelBytes.getData(), modelBytes.getSize());

        Proc proc;
        proc.prepareToPlay (sampleRate, blockSize);
        proc.setStateInformation (resaved.getData(), (int) resaved.getSize());
        ok &= expect (proc.getLoadedModelPath() == model.getFullPathName() && proc.namEngine.hasModel(),
                      "saving with a file missing keeps the reference: the session heals when the file returns");
        ok &= expect (proc.getMissingMediaPath (Proc::Media::model).isEmpty(), "...and it is no longer reported missing");

        // Loading something else, or clearing, drops the stale reference.
        proc.clearIr();
        ok &= expect (proc.getMissingMediaPath (Proc::Media::ir).isEmpty(), "clearing a slot forgets its missing file");
    }

    // --- a lost path with no embedded copy is looked up by name in the library.
    model.deleteFile();
    const auto byName = tubamp::ModelLibrary::getModelsDir().getChildFile (model.getFileName());
    byName.replaceWithData (modelBytes.getData(), modelBytes.getSize());

    {
        Proc proc;
        proc.prepareToPlay (sampleRate, blockSize);
        proc.setStateInformation (legacy.getData(), (int) legacy.getSize());
        ok &= expect (proc.getLoadedModelPath() == byName.getFullPathName() && proc.namEngine.hasModel(),
                      "a lost path falls back to the file of that name in the library");

        // Presets and A/B slots hold references only and take the same route.
        const auto preset = proc.captureStateVar();
        proc.clearModel();
        proc.applyStateVar (preset);
        ok &= expect (proc.getLoadedModelPath() == byName.getFullPathName(), "a preset restores its model by reference");
    }

    byName.deleteFile();
    return ok;
}

//==============================================================================
/** Things that looked right and were not, each found by review and pinned here. */
bool runTransitionEdgeTests (const juce::File& modelFile, const juce::File& dir, double sampleRate)
{
    std::printf ("transition edges:\n");
    bool ok = true;
    const int blockSize = 64;

    // --- clearing a slot that is already empty is not a transition. Every restore
    // clears the slots it has no file for; that must not dip the signal.
    {
        Proc proc;
        makeCabOnly (proc, sampleRate, blockSize);
        proc.setChainOrder ({ BlockId::amp, BlockId::cab }, {});
        setBool (proc, params::ampOn, true);
        proc.prepareToPlay (sampleRate, blockSize);

        SineRun run (proc, sampleRate, blockSize, 0.5f, 220.0);
        run.runFor (1.0);
        run.forget();
        proc.clearIr();
        proc.clearModel();
        proc.clearModelB();
        run.runFor (0.3);
        ok &= expect (! SineRun::hasQuietRun (run.left, 0.05, (int) (0.001 * sampleRate)),
                      "clearing an empty amp and an empty cab leaves the signal alone");
    }

    // --- a model staged before the stream starts is the live model once the stream
    // is prepared: latency and gains are right from the first sample.
    {
        Proc proc;
        proc.setChainOrder ({ BlockId::amp }, {});
        ok &= expect (proc.loadModel (modelFile).isEmpty(), "a model loads before prepareToPlay");
        proc.prepareToPlay (sampleRate, blockSize);
        ok &= expect (proc.namEngine.hasLiveModel(), "...and is live as soon as the stream is prepared");
        ok &= expect ((proc.getLatencySamples() > 0) == (std::abs (sampleRate - 48000.0) > 1.0),
                      "...with its latency reported before the first block");

        // The very first block already plays at the model's own gains: the render
        // is the same as one from a stream that ran with the model (so every gain
        // had settled on it) and was then prepared again.
        SineRun early (proc, sampleRate, blockSize, 0.3f, 220.0);
        early.runFor (0.2);

        Proc settled;
        settled.setChainOrder ({ BlockId::amp }, {});
        settled.prepareToPlay (sampleRate, blockSize);
        settled.loadModel (modelFile);
        SineRun warmUp (settled, sampleRate, blockSize, 0.3f, 220.0);
        warmUp.runFor (0.5);
        settled.prepareToPlay (sampleRate, blockSize);
        SineRun late (settled, sampleRate, blockSize, 0.3f, 220.0);
        late.runFor (0.2);

        double worst = 0.0;

        for (size_t i = 0; i < std::min (early.left.size(), late.left.size()); ++i)
            worst = std::max (worst, (double) std::abs (early.left[i] - late.left[i]));

        std::printf ("  model staged before the stream vs settled and re-prepared: max diff %.2e\n", worst);
        ok &= expect (worst < 1.0e-4, "...and its gains in place from the first sample");
    }

    // --- amp2's model-less side of a fade is the lane's untouched signal: no fold
    // to mono, no amp gains, and so no step where the fade meets the pass-through.
    const auto quiet = modelFile.getSiblingFile ("wavenet.nam");

    if (quiet.existsAsFile())
    {
        Proc proc;
        proc.setChainOrder ({ BlockId::split, BlockId::lane2, BlockId::amp2, BlockId::mix }, {});
        setFloat (proc, params::splitMode, 0.0f);
        setBool (proc, params::splitOn, true);
        setBool (proc, params::mixOn, true);
        setBool (proc, params::amp2On, true);
        setFloat (proc, params::amp2Output, -10.0f);
        setFloat (proc, params::mixALevel, -60.0f);     // lane B alone at the output
        proc.prepareToPlay (sampleRate, blockSize);

        SineRun run (proc, sampleRate, blockSize, 0.5f, 220.0);
        const double drySlope = 0.5 * 2.0 * kPi * 220.0 / sampleRate;
        run.runFor (1.0);
        const double dryPeak = SineRun::peak (run.left, run.tail());

        run.forget();
        ok &= expect (proc.loadModelB (quiet).isEmpty(), "engine B loads while its lane plays");
        run.runFor (0.5);
        const double wetPeak = SineRun::peak (run.left, run.tail());
        ok &= expect (SineRun::peak (run.left) <= 1.25 * std::max (dryPeak, wetPeak),
                      "amp2's first model never plays the lane at the amp's gains");
        ok &= expect (SineRun::hasQuietRun (run.left, 0.03 * dryPeak, (int) (0.001 * sampleRate)), "...and comes in through silence");

        run.forget();
        proc.clearModelB();
        run.runFor (0.5);
        std::printf ("  amp2 cleared at -10 dB out: dry %.3f, model %.3f, peak %.3f, largest step %.5f (dry slope %.5f)\n",
                     dryPeak, wetPeak, SineRun::peak (run.left), SineRun::maxStep (run.left), drySlope * dryPeak / 0.5);
        ok &= expect (std::abs (SineRun::peak (run.left, run.tail()) - dryPeak) < 0.01 * dryPeak + 1.0e-4,
                      "clearing amp2 returns its lane to the untouched signal");
        ok &= expect (SineRun::maxStep (run.left) <= 1.3 * drySlope * (dryPeak / 0.5) + dryPeak / (0.025 * sampleRate),
                      "...without a level step where the fade ends");
    }

    // --- the cab's stereo/mono switch goes through the fade too.
    {
        Proc proc;
        makeCabOnly (proc, sampleRate, blockSize);
        proc.setChainOrder ({ BlockId::cab, BlockId::amp }, {});
        setBool (proc, params::ampOn, false);
        proc.prepareToPlay (sampleRate, blockSize);

        const auto stereo = dir.getChildFile ("edge-stereo.wav");
        writeWav (stereo, { impulseKernel (2400, 0.5f), impulseKernel (2400, -0.25f) }, sampleRate);
        proc.loadIr (stereo);

        SineRun run (proc, sampleRate, blockSize, 0.5f, 220.0);
        run.runFor (0.5);
        const double wetPeak = SineRun::peak (run.left, run.tail());
        ok &= expect (proc.cab.getImage() == tubamp::CabSim::Image::stereo, "stereo IR, bypassed amp behind it: true stereo");

        run.forget();
        setBool (proc, params::ampOn, true);    // the path folds to mono from here on
        run.runFor (0.4);
        ok &= expect (proc.cab.getImage() == tubamp::CabSim::Image::monoPath, "enabling the amp switches the cab to its left kernel");
        ok &= expect (SineRun::hasQuietRun (run.left, 0.03 * wetPeak, (int) (0.001 * sampleRate)), "...through the swap fade, not as a cut");
    }

    // --- hostile or broken files named by a session.
    {
        Proc proc;
        proc.prepareToPlay (sampleRate, blockSize);

        const auto started = juce::Time::getMillisecondCounterHiRes();
        ok &= expect (proc.loadIr (juce::File ("/dev/zero")).isNotEmpty(), "a device node named as an IR is refused");
        ok &= expect (juce::Time::getMillisecondCounterHiRes() - started < 5000.0, "...without reading it forever");

        std::vector<float> poisoned (2400, 0.0f);
        poisoned[0] = 0.5f;
        poisoned[10] = std::numeric_limits<float>::quiet_NaN();
        const auto nan = dir.getChildFile ("nan.wav");
        writeWav (nan, { poisoned }, sampleRate, 32);
        ok &= expect (proc.loadIr (nan).isNotEmpty(), "an IR containing NaN is refused");
    }

    return ok;
}

/** Session/preset bookkeeping found wanting by review. */
bool runMediaEdgeTests (const juce::File& modelFile, const juce::File& dir, double sampleRate, int blockSize)
{
    std::printf ("session media edges:\n");
    bool ok = true;

    const auto elsewhere = dir.getChildFile ("edge-elsewhere");
    elsewhere.createDirectory();
    const auto model = elsewhere.getChildFile ("edge-model.nam");
    modelFile.copyFileTo (model);

    juce::MemoryBlock modelBytes;
    model.loadFileAsData (modelBytes);

    juce::MemoryBlock state;
    juce::var presetWithModel, presetWithout;

    {
        Proc proc;
        proc.prepareToPlay (sampleRate, blockSize);
        presetWithout = proc.captureStateVar();
        proc.loadModel (model);
        presetWithModel = proc.captureStateVar();
        proc.getStateInformation (state);
    }

    // --- a preset whose model is missing, then one that has no model at all: the
    // second must not inherit the first one's missing file.
    model.deleteFile();

    {
        Proc proc;
        proc.prepareToPlay (sampleRate, blockSize);
        proc.applyStateVar (presetWithModel);
        ok &= expect (proc.getMissingMediaPath (Proc::Media::model) == model.getFullPathName(),
                      "a preset naming a missing model reports it");
        proc.applyStateVar (presetWithout);
        ok &= expect (proc.getMissingMediaPath (Proc::Media::model).isEmpty(),
                      "the next preset, which has no model, clears that report");
        ok &= expect (proc.captureStateVar()["modelPath"].toString().isEmpty(),
                      "...and does not save the other preset's file as its own");
    }

    // --- a different file of the same name: the rebuilt copy is reused on every
    // later restore instead of piling up "name 2", "name 3", ...
    const auto taken = tubamp::ModelLibrary::getModelsDir().getChildFile (model.getFileName());
    taken.replaceWithText ("{ \"other\": true }");

    for (int round = 0; round < 3; ++round)
    {
        Proc proc;
        proc.prepareToPlay (sampleRate, blockSize);
        proc.setStateInformation (state.getData(), (int) state.getSize());
    }

    ok &= expect (tubamp::ModelLibrary::getModelsDir().getChildFile ("edge-model 2.nam").existsAsFile()
                  && ! tubamp::ModelLibrary::getModelsDir().getChildFile ("edge-model 3.nam").exists(),
                  "three restores next to a same-named file leave exactly one rebuilt copy");
    tubamp::ModelLibrary::getModelsDir().getChildFile ("edge-model 2.nam").deleteFile();
    taken.deleteFile();

    // --- embedded data that does not decode to the file it claims to be: nothing
    // is written, and a file of that name in the library is used instead.
    {
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        auto tree = juce::ValueTree::fromXml (*xml);

        for (auto child : tree)
            if (child.hasType (tubamp::EmbeddedMedia::treeType))
            {
                // Cut in half (on a base64 quantum), whatever the size of the model.
                const auto data = child.getProperty ("data").toString();
                child.setProperty ("data", data.substring (0, (data.length() / 8) * 4), nullptr);
            }

        juce::MemoryBlock damaged;
        juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), damaged);

        {
            Proc proc;
            proc.prepareToPlay (sampleRate, blockSize);
            proc.setStateInformation (damaged.getData(), (int) damaged.getSize());
            ok &= expect (! proc.namEngine.hasModel()
                          && proc.getMissingMediaPath (Proc::Media::model) == model.getFullPathName()
                          && ! taken.exists(),
                          "a truncated embedded file is not written into the library; the slot is reported missing");
        }

        taken.replaceWithData (modelBytes.getData(), modelBytes.getSize());

        {
            Proc proc;
            proc.prepareToPlay (sampleRate, blockSize);
            proc.setStateInformation (damaged.getData(), (int) damaged.getSize());
            ok &= expect (proc.getLoadedModelPath() == taken.getFullPathName(),
                          "...and a file of that name in the library is used when the embedded copy is unusable");
        }

        taken.deleteFile();
    }

    // --- the same path with new content: the session embeds what is loaded now.
    {
        juce::File (model).replaceWithData (modelBytes.getData(), modelBytes.getSize());
        const auto other = modelFile.getSiblingFile ("wavenet.nam");

        if (other.existsAsFile())
        {
            juce::MemoryBlock second;

            {
                Proc proc;
                proc.prepareToPlay (sampleRate, blockSize);
                proc.loadModel (model);
                juce::MemoryBlock first;
                proc.getStateInformation (first);

                other.copyFileTo (model);       // retrained capture, same path
                proc.loadModel (model);
                proc.getStateInformation (second);
            }

            juce::MemoryBlock otherBytes;
            other.loadFileAsData (otherBytes);
            model.deleteFile();

            Proc proc;
            proc.prepareToPlay (sampleRate, blockSize);
            proc.setStateInformation (second.getData(), (int) second.getSize());

            juce::MemoryBlock rebuilt;
            juce::File (proc.getLoadedModelPath()).loadFileAsData (rebuilt);
            ok &= expect (rebuilt == otherBytes, "reloading a path whose file changed embeds the new content");
            juce::File (proc.getLoadedModelPath()).deleteFile();
        }
    }

    model.deleteFile();
    return ok;
}
} // namespace

bool runEngineTests (const juce::File& modelFile, double sampleRate, int blockSize)
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("tubamp-engine-tests-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
    dir.createDirectory();

    bool ok = true;
    ok &= runIrLevelTests (dir, sampleRate, blockSize);
    ok &= runIrStereoTests (dir, sampleRate, blockSize);
    ok &= runIrBlockSizeTests (dir, sampleRate);
    ok &= runIrSwapTests (dir, sampleRate);
    ok &= runNamSwapTests (modelFile, sampleRate);
    ok &= runNamGainHandoverTests (modelFile, sampleRate);
    ok &= runNamMetadataTests (modelFile, dir, sampleRate);
    ok &= runNamLatencyTests (modelFile, sampleRate);
    ok &= runSessionMediaTests (modelFile, dir, sampleRate, blockSize);
    ok &= runTransitionEdgeTests (modelFile, dir, sampleRate);
    ok &= runMediaEdgeTests (modelFile, dir, sampleRate, blockSize);

    dir.deleteRecursively();
    return ok;
}
