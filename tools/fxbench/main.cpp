// tubamp_fxbench: offline bench for the effect engines (docs/FX-ENGINES.md §5.3).
// Runs one engine (or its frozen legacy reference) over a WAV or a generated signal,
// with a per-block parameter timeline, and checks Classic identity, reset determinism
// and real-time safety. Each tools/fxbench/fx_<effect>.cpp registers one effect.
//
//   tubamp_fxbench <effect> <in.wav | gen:SPEC> <out.wav> [key=value ...] [options]
//   tubamp_fxbench --help
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "FxBench.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <pthread.h>
#include <string>
#include <vector>

//==============================================================================
// --rt-check: every global operator new/delete, plus (on macOS) libmalloc's logger hook,
// which also sees malloc/free calls that never go through operator new (juce::HeapBlock,
// so AudioBuffer::setSize). Counting is armed only around setParams() and process(), and
// only on the thread that runs them: the hooks see every thread, and a WAV input leaves
// reader threads freeing their buffers while the render runs.
namespace
{
std::atomic<bool> rtArmed { false };
std::atomic<pthread_t> rtThread { nullptr };
std::atomic<long> rtNew { 0 }, rtDelete { 0 }, rtMalloc { 0 }, rtFree { 0 };

// A per-thread flag by thread identity: a thread_local would itself be allocated by
// libmalloc on a thread's first access, from inside the malloc hook.
inline bool rtCounting() noexcept
{
    return rtArmed.load (std::memory_order_relaxed)
        && pthread_equal (pthread_self(), rtThread.load (std::memory_order_relaxed)) != 0;
}

inline void noteNew() noexcept
{
    if (rtCounting())
        rtNew.fetch_add (1, std::memory_order_relaxed);
}

inline void noteDelete() noexcept
{
    if (rtCounting())
        rtDelete.fetch_add (1, std::memory_order_relaxed);
}

void* allocateAligned (std::size_t size, std::size_t alignment) noexcept
{
    void* p = nullptr;
    alignment = std::max (alignment, sizeof (void*));
    return posix_memalign (&p, alignment, size != 0 ? size : 1) == 0 ? p : nullptr;
}
} // namespace

void* operator new (std::size_t size)
{
    noteNew();

    if (void* p = std::malloc (size != 0 ? size : 1))
        return p;

    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    noteNew();

    if (void* p = std::malloc (size != 0 ? size : 1))
        return p;

    throw std::bad_alloc();
}

void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    noteNew();
    return std::malloc (size != 0 ? size : 1);
}

void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept
{
    noteNew();
    return std::malloc (size != 0 ? size : 1);
}

void* operator new (std::size_t size, std::align_val_t alignment)
{
    noteNew();

    if (void* p = allocateAligned (size, (std::size_t) alignment))
        return p;

    throw std::bad_alloc();
}

void* operator new[] (std::size_t size, std::align_val_t alignment)
{
    noteNew();

    if (void* p = allocateAligned (size, (std::size_t) alignment))
        return p;

    throw std::bad_alloc();
}

void* operator new (std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    noteNew();
    return allocateAligned (size, (std::size_t) alignment);
}

void* operator new[] (std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    noteNew();
    return allocateAligned (size, (std::size_t) alignment);
}

void operator delete (void* p) noexcept                                       { if (p != nullptr) noteDelete(); std::free (p); }
void operator delete[] (void* p) noexcept                                     { if (p != nullptr) noteDelete(); std::free (p); }
void operator delete (void* p, std::size_t) noexcept                          { if (p != nullptr) noteDelete(); std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept                        { if (p != nullptr) noteDelete(); std::free (p); }
void operator delete (void* p, const std::nothrow_t&) noexcept                { if (p != nullptr) noteDelete(); std::free (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept              { if (p != nullptr) noteDelete(); std::free (p); }
void operator delete (void* p, std::align_val_t) noexcept                     { if (p != nullptr) noteDelete(); std::free (p); }
void operator delete[] (void* p, std::align_val_t) noexcept                   { if (p != nullptr) noteDelete(); std::free (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept        { if (p != nullptr) noteDelete(); std::free (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept      { if (p != nullptr) noteDelete(); std::free (p); }
void operator delete (void* p, std::align_val_t, const std::nothrow_t&) noexcept   { if (p != nullptr) noteDelete(); std::free (p); }
void operator delete[] (void* p, std::align_val_t, const std::nothrow_t&) noexcept { if (p != nullptr) noteDelete(); std::free (p); }

#if JUCE_MAC
// libmalloc calls this (when set) for every allocation and free in every zone; it is
// what malloc stack logging uses. Bit 2 = allocate, bit 4 = deallocate (realloc: both).
typedef void (malloc_logger_t) (uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                uintptr_t result, uint32_t numHotFramesToSkip);
extern "C" malloc_logger_t* malloc_logger;

namespace
{
void rtMallocLogger (uint32_t type, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t)
{
    if (! rtCounting())
        return;

    if ((type & 2u) != 0)
        rtMalloc.fetch_add (1, std::memory_order_relaxed);

    if ((type & 4u) != 0)
        rtFree.fetch_add (1, std::memory_order_relaxed);
}
} // namespace
#endif

//==============================================================================
namespace fxbench
{
double Args::get (const char* key, double fallback) const
{
    const auto it = values.find (key);
    return it != values.end() ? it->second : fallback;
}

bool Args::has (const char* key) const
{
    return values.count (key) > 0;
}

namespace
{
std::vector<Registration>& registry()
{
    static std::vector<Registration> list;
    return list;
}
} // namespace

bool registerEffect (const Registration& r)
{
    registry().push_back (r);
    return true;
}
} // namespace fxbench

namespace
{
//==============================================================================
bool parseNumber (const std::string& s, double& out)
{
    if (s.empty())
        return false;

    char* end = nullptr;
    out = std::strtod (s.c_str(), &end);
    return end != nullptr && *end == '\0';
}

/** key=value, key@T=value, key@T0..T1=V0..V1; evaluated once per block at its first
    sample's time. A key with no constant and no event started yet is absent, so the
    adapter's own default applies. */
class Timeline
{
public:
    bool add (const std::string& token, std::string& error)
    {
        const auto eq = token.find ('=');

        if (eq == std::string::npos || eq == 0)
        {
            error = "expected key=value: " + token;
            return false;
        }

        const auto lhs = token.substr (0, eq), rhs = token.substr (eq + 1);
        const auto at = lhs.find ('@');
        const auto key = lhs.substr (0, at);

        if (at == std::string::npos)
        {
            double v = 0.0;

            if (! parseNumber (rhs, v))
            {
                error = "bad value: " + token;
                return false;
            }

            constants[key] = v;
            return true;
        }

        const auto when = lhs.substr (at + 1);
        Event e;
        const auto dotsT = when.find ("..");

        if (dotsT == std::string::npos)
        {
            if (! parseNumber (when, e.t0) || ! parseNumber (rhs, e.v0))
            {
                error = "bad step: " + token;
                return false;
            }

            e.t1 = e.t0;
            e.v1 = e.v0;
        }
        else
        {
            const auto dotsV = rhs.find ("..");
            e.ramp = true;

            if (dotsV == std::string::npos
                || ! parseNumber (when.substr (0, dotsT), e.t0) || ! parseNumber (when.substr (dotsT + 2), e.t1)
                || ! parseNumber (rhs.substr (0, dotsV), e.v0) || ! parseNumber (rhs.substr (dotsV + 2), e.v1)
                || e.t1 <= e.t0)
            {
                error = "bad ramp (key@T0..T1=V0..V1, T1 > T0): " + token;
                return false;
            }
        }

        auto& list = events[key];
        list.push_back (e);
        std::stable_sort (list.begin(), list.end(), [] (const Event& a, const Event& b) { return a.t0 < b.t0; });
        return true;
    }

    void at (double t, fxbench::Args& args) const
    {
        args.values = constants;

        for (const auto& [key, list] : events)
        {
            for (const auto& e : list)
            {
                if (t < e.t0)
                    break;

                args.values[key] = ! e.ramp || t >= e.t1 ? e.v1 : e.v0 + (e.v1 - e.v0) * (t - e.t0) / (e.t1 - e.t0);
            }
        }
    }

private:
    struct Event
    {
        double t0 = 0.0, t1 = 0.0, v0 = 0.0, v1 = 0.0;
        bool ramp = false;
    };

    std::map<std::string, double> constants;
    std::map<std::string, std::vector<Event>> events;
};

//==============================================================================
struct Signal
{
    juce::AudioBuffer<float> audio;
    double sampleRate = 48000.0;
};

bool readWav (const juce::File& file, Signal& out, std::string& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
    {
        error = "cannot read " + file.getFullPathName().toStdString();
        return false;
    }

    if (reader->numChannels < 1 || reader->numChannels > 2)
    {
        error = "only mono and stereo inputs are supported";
        return false;
    }

    out.sampleRate = reader->sampleRate;
    out.audio.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&out.audio, 0, (int) reader->lengthInSamples, 0, true, true);
    return true;
}

bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    file.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());

    if (stream == nullptr)
        return false;

    const auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                            .withSampleRate (sampleRate)
                                                            .withNumChannels (buffer.getNumChannels())
                                                            .withBitsPerSample (32)
                                                            .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));

    return writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

/** White noise from the gen:music LCG, independent per channel, uniform, at an RMS level. */
void fillWhite (juce::AudioBuffer<float>& b, double rmsDb, uint32_t seed)
{
    const double amp = std::pow (10.0, rmsDb / 20.0) * std::sqrt (3.0);
    uint32_t x = seed;

    for (int i = 0; i < b.getNumSamples(); ++i)
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            x = x * 1664525u + 1013904223u;
            b.setSample (ch, i, (float) (amp * ((double) x / 4294967296.0 * 2.0 - 1.0)));
        }
}

/** Pink noise: the LCG's white noise through Paul Kellet's refined filter, each channel
    scaled to the RMS level over the whole signal. */
void fillPink (juce::AudioBuffer<float>& b, double rmsDb, uint32_t seed)
{
    const int n = b.getNumSamples();
    uint32_t x = seed;
    std::vector<double> pink ((size_t) n * (size_t) b.getNumChannels());
    std::vector<double> state ((size_t) b.getNumChannels() * 7, 0.0);

    for (int i = 0; i < n; ++i)
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            x = x * 1664525u + 1013904223u;
            const double w = (double) x / 4294967296.0 * 2.0 - 1.0;
            double* s = state.data() + ch * 7;
            s[0] = 0.99886 * s[0] + w * 0.0555179;
            s[1] = 0.99332 * s[1] + w * 0.0750759;
            s[2] = 0.96900 * s[2] + w * 0.1538520;
            s[3] = 0.86650 * s[3] + w * 0.3104856;
            s[4] = 0.55000 * s[4] + w * 0.5329522;
            s[5] = -0.7616 * s[5] - w * 0.0168980;
            pink[(size_t) (ch * n + i)] = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + w * 0.5362;
            s[6] = w * 0.115926;
        }

    const double target = std::pow (10.0, rmsDb / 20.0);

    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        double sum = 0.0;

        for (int i = 0; i < n; ++i)
            sum += pink[(size_t) (ch * n + i)] * pink[(size_t) (ch * n + i)];

        const double scale = n > 0 && sum > 0.0 ? target / std::sqrt (sum / n) : 0.0;

        for (int i = 0; i < n; ++i)
            b.setSample (ch, i, (float) (pink[(size_t) (ch * n + i)] * scale));
    }
}

bool generate (const std::string& spec, Signal& out, double sampleRate, double seconds, int channels, std::string& error)
{
    juce::StringArray parts;
    parts.addTokens (juce::String (spec), ":", "");
    const auto kind = parts[0];
    out.sampleRate = sampleRate;
    out.audio.setSize (channels, juce::jmax (1, (int) std::llround (seconds * sampleRate)));
    out.audio.clear();

    if (kind == "music")
    {
        fxbench::fillMusic (out.audio, sampleRate);
        return true;
    }

    if (kind == "sine" && parts.size() == 3)
    {
        const double hz = parts[1].getDoubleValue(), amp = std::pow (10.0, parts[2].getDoubleValue() / 20.0);

        for (int i = 0; i < out.audio.getNumSamples(); ++i)
            for (int ch = 0; ch < channels; ++ch)
                out.audio.setSample (ch, i, (float) (amp * std::sin (2.0 * juce::MathConstants<double>::pi * hz * i / sampleRate)));

        return true;
    }

    if (kind == "noise" && parts.size() == 3 && (parts[1] == "white" || parts[1] == "pink"))
    {
        if (parts[1] == "white")
            fillWhite (out.audio, parts[2].getDoubleValue(), 12345u);
        else
            fillPink (out.audio, parts[2].getDoubleValue(), 12345u);

        return true;
    }

    if (kind == "impulse")
    {
        const double amp = parts.size() > 1 ? std::pow (10.0, parts[1].getDoubleValue() / 20.0) : 1.0;

        for (int ch = 0; ch < channels; ++ch)
            out.audio.setSample (ch, 0, (float) amp);

        return true;
    }

    if (kind == "silence")
        return true;

    error = "unknown generator: gen:" + spec;
    return false;
}

//==============================================================================
struct RenderOptions
{
    int block = 512;
    bool ftz = true;
    bool rtCheck = false;
    bool timing = false;
    bool forceClassic = false;
    juce::AudioBuffer<float>* trace = nullptr;
    std::vector<std::string>* meters = nullptr;
};

struct RenderStats
{
    double seconds = 0.0;
};

RenderStats render (fxbench::Effect& fx, juce::AudioBuffer<float>& io, double sampleRate,
                    const Timeline& timeline, const RenderOptions& o)
{
    RenderStats stats;
    std::optional<juce::ScopedNoDenormals> noDenormals;

    if (o.ftz)
        noDenormals.emplace();

    const int numChannels = io.getNumChannels();
    const int total = io.getNumSamples();
    const int traceChannels = o.trace != nullptr ? o.trace->getNumChannels() : 0;
    fxbench::Args args;

    for (int start = 0; start < total; start += o.block)
    {
        const int n = juce::jmin (o.block, total - start);
        timeline.at ((double) start / sampleRate, args);

        if (o.forceClassic)
            args.values[fx.typeKey()] = 0.0;

        float* ptrs[2] { io.getWritePointer (0, start), numChannels > 1 ? io.getWritePointer (1, start) : nullptr };
        juce::AudioBuffer<float> view (ptrs, numChannels, n);

        const auto t0 = std::chrono::steady_clock::now();

        if (o.rtCheck)
        {
            rtThread.store (pthread_self(), std::memory_order_relaxed);
            rtArmed.store (true, std::memory_order_relaxed);
        }

        fx.setParams (args);
        fx.process (view, n);

        if (o.rtCheck)
            rtArmed.store (false, std::memory_order_relaxed);

        if (o.timing)
            stats.seconds += std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();

        if (traceChannels > 0)
        {
            float* tp[8] {};

            for (int ch = 0; ch < juce::jmin (8, traceChannels); ++ch)
                tp[ch] = o.trace->getWritePointer (ch, start);

            fx.trace (tp, n);
        }

        if (o.meters != nullptr)
            o.meters->push_back (std::to_string (start) + "\t" + fx.meterRow());
    }

    return stats;
}

struct Difference
{
    long count = 0;
    long firstFrame = -1;
    int firstChannel = 0;
    float a = 0.0f, b = 0.0f;
    double maxAbs = 0.0;
};

/** Bitwise: the golden hashes compare bytes, so -0 against +0 counts as a difference too. */
Difference compare (const juce::AudioBuffer<float>& x, const juce::AudioBuffer<float>& y)
{
    Difference d;

    for (int i = 0; i < x.getNumSamples(); ++i)
        for (int ch = 0; ch < x.getNumChannels(); ++ch)
        {
            const float a = x.getSample (ch, i), b = y.getSample (ch, i);

            if (std::memcmp (&a, &b, sizeof (float)) != 0)
            {
                if (d.count++ == 0)
                {
                    d.firstFrame = i;
                    d.firstChannel = ch;
                    d.a = a;
                    d.b = b;
                }

                d.maxAbs = std::max (d.maxAbs, (double) std::abs (a - b));
            }
        }

    return d;
}

void printHelp()
{
    std::printf (
        "usage: tubamp_fxbench <effect> <in.wav | gen:SPEC> <out.wav> [key=value ...] [options]\n"
        "  key=value              engine parameter, units as Parameters.h\n"
        "  key@T=value            step at T seconds\n"
        "  key@T0..T1=V0..V1      linear ramp, evaluated once per block\n"
        "  gen:SPEC               sine:<hz>:<dBFS peak> | noise:white|pink:<dBFS rms> | impulse[:<dBFS>]\n"
        "                         | music | silence   (with --sr=<Hz> --seconds=<s> --channels=<1|2>;\n"
        "                         defaults 48000, 6, 2)\n"
        "  --block=N              default 512\n"
        "  --trace=<path.wav>     per-sample trace (gain etc.), Effect::trace\n"
        "  --meters=<path.tsv>    per-block Effect::meterRow (first column: block start sample)\n"
        "  --latency              print Effect::latency after the render\n"
        "  --classic-null         render with typeKey=0 and the legacy reference; print the number\n"
        "                         of differing samples and the first index; exit 1 if any differ\n"
        "  --legacy               render the legacy reference instead of the engine\n"
        "  --reset-repeat         render, reset, render the same input; exit 1 if not identical\n"
        "  --rt-check             count allocations inside setParams/process; exit 1 on any\n"
        "  --no-ftz               do not wrap processing in juce::ScopedNoDenormals\n"
        "  --bench                time setParams+process; print ns/sample and %% of real time (min of\n"
        "                         5 runs); generator defaults become 48000 Hz, block 64, 10 s, stereo\n"
        "exit status: 0 ok, 1 a check failed, 2 usage or I/O error\n\neffects:\n");

    for (const auto& r : fxbench::registry())
        std::printf ("  %-8s %s\n", r.name, r.help);
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2 || std::string (argv[1]) == "--help" || std::string (argv[1]) == "-h")
    {
        printHelp();
        return argc < 2 ? 2 : 0;
    }

    const std::string effectName = argv[1];
    std::vector<std::string> positional;
    std::map<std::string, std::string> opts;
    Timeline timeline;

    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];

        if (a.rfind ("--", 0) == 0)
        {
            const auto eq = a.find ('=');
            opts[a.substr (0, eq)] = eq == std::string::npos ? std::string ("1") : a.substr (eq + 1);
        }
        else if (positional.size() < 2)
        {
            positional.push_back (a);
        }
        else
        {
            std::string error;

            if (! timeline.add (a, error))
            {
                std::fprintf (stderr, "%s\n", error.c_str());
                return 2;
            }
        }
    }

    static const char* const known[] { "--block", "--trace", "--meters", "--latency", "--classic-null", "--legacy",
                                       "--reset-repeat", "--rt-check", "--no-ftz", "--bench", "--sr", "--seconds", "--channels" };

    for (const auto& [key, value] : opts)
    {
        if (std::find_if (std::begin (known), std::end (known), [&key] (const char* k) { return key == k; }) == std::end (known))
        {
            std::fprintf (stderr, "unknown option %s\n", key.c_str());
            return 2;
        }
    }

    const fxbench::Registration* reg = nullptr;

    for (const auto& r : fxbench::registry())
        if (effectName == r.name)
            reg = &r;

    if (reg == nullptr)
    {
        std::fprintf (stderr, "unknown effect '%s' (built with:", effectName.c_str());

        for (const auto& r : fxbench::registry())
            std::fprintf (stderr, " %s", r.name);

        std::fprintf (stderr, ")\n");
        return 2;
    }

    if (positional.size() < 2)
    {
        printHelp();
        return 2;
    }

    const auto has = [&opts] (const char* k) { return opts.count (k) > 0; };
    const auto num = [&opts] (const char* k, double fallback) { const auto it = opts.find (k); return it != opts.end() ? std::atof (it->second.c_str()) : fallback; };
    const auto cwdFile = [] (const std::string& path) { return juce::File::getCurrentWorkingDirectory().getChildFile (path); };
    const bool bench = has ("--bench");

    Signal input;
    std::string error;

    if (positional[0].rfind ("gen:", 0) == 0)
    {
        const int channels = (int) num ("--channels", 2.0);

        if (channels < 1 || channels > 2)
        {
            std::fprintf (stderr, "--channels must be 1 or 2\n");
            return 2;
        }

        if (! generate (positional[0].substr (4), input, num ("--sr", 48000.0), num ("--seconds", bench ? 10.0 : 6.0),
                        channels, error))
        {
            std::fprintf (stderr, "%s\n", error.c_str());
            return 2;
        }
    }
    else if (! readWav (cwdFile (positional[0]), input, error))
    {
        std::fprintf (stderr, "%s\n", error.c_str());
        return 2;
    }

    const juce::File outFile = cwdFile (positional[1]);
    const double sampleRate = input.sampleRate;
    const int numChannels = input.audio.getNumChannels();

    RenderOptions o;
    o.block = juce::jmax (1, (int) num ("--block", bench ? 64.0 : 512.0));
    o.ftz = ! has ("--no-ftz");
    o.rtCheck = has ("--rt-check");
    o.timing = bench;

    auto engine = reg->make();
    std::unique_ptr<fxbench::Effect> legacy;

    if (has ("--classic-null") || has ("--legacy"))
    {
        legacy = engine->makeLegacy();

        if (legacy == nullptr)
        {
            std::fprintf (stderr, "%s has no legacy reference\n", reg->name);
            return 2;
        }
    }

    fxbench::Effect& fx = has ("--legacy") ? *legacy : *engine;

#if JUCE_MAC
    if (o.rtCheck)
        malloc_logger = rtMallocLogger;
#endif

    fx.prepare (sampleRate, o.block, numChannels);

    juce::AudioBuffer<float> traceBuffer;
    std::vector<std::string> meterRows;

    if (has ("--trace"))
    {
        if (fx.traceChannels() <= 0)
        {
            std::fprintf (stderr, "%s does not support --trace\n", reg->name);
            return 2;
        }

        traceBuffer.setSize (fx.traceChannels(), input.audio.getNumSamples());
        traceBuffer.clear();
        o.trace = &traceBuffer;
    }

    if (has ("--meters"))
    {
        meterRows.reserve ((size_t) (input.audio.getNumSamples() / o.block + 1));
        o.meters = &meterRows;
    }

    int status = 0;
    juce::AudioBuffer<float> output;

    if (has ("--classic-null"))
    {
        legacy->prepare (sampleRate, o.block, numChannels);
        o.forceClassic = true;

        juce::AudioBuffer<float> legacyOut;
        output.makeCopyOf (input.audio);
        legacyOut.makeCopyOf (input.audio);
        render (*engine, output, sampleRate, timeline, o);

        auto legacyOptions = o;
        legacyOptions.trace = nullptr;
        legacyOptions.meters = nullptr;
        render (*legacy, legacyOut, sampleRate, timeline, legacyOptions);

        const auto d = compare (output, legacyOut);
        const long total = (long) output.getNumSamples() * numChannels;

        if (d.count == 0)
        {
            std::printf ("classic-null: PASS, 0 of %ld samples differ\n", total);
        }
        else
        {
            std::printf ("classic-null: FAIL, %ld of %ld samples differ; first at frame %ld channel %d "
                         "(engine %.9g, legacy %.9g); max |diff| %.3g\n",
                         d.count, total, d.firstFrame, d.firstChannel, (double) d.a, (double) d.b, d.maxAbs);
            status = 1;
        }
    }
    else if (bench)
    {
        double best = 1.0e30;

        for (int run = 0; run < 5; ++run)
        {
            fx.reset();
            output.makeCopyOf (input.audio);
            meterRows.clear();
            best = std::min (best, render (fx, output, sampleRate, timeline, o).seconds);
        }

        const double frames = (double) input.audio.getNumSamples();
        std::printf ("bench: %.2f ns/sample, %.4f %% of real time (min of 5 runs; %d Hz, block %d, %d ch, %s)\n",
                     best / frames * 1.0e9, 100.0 * best / (frames / sampleRate), (int) sampleRate, o.block,
                     numChannels, o.ftz ? "FTZ" : "no FTZ");
    }
    else
    {
        output.makeCopyOf (input.audio);
        render (fx, output, sampleRate, timeline, o);

        if (has ("--reset-repeat"))
        {
            fx.reset();
            juce::AudioBuffer<float> again;
            again.makeCopyOf (input.audio);
            auto repeatOptions = o;
            repeatOptions.trace = nullptr;
            repeatOptions.meters = nullptr;
            render (fx, again, sampleRate, timeline, repeatOptions);

            const auto d = compare (output, again);

            if (d.count == 0)
            {
                std::printf ("reset-repeat: PASS, identical\n");
            }
            else
            {
                std::printf ("reset-repeat: FAIL, %ld samples differ; first at frame %ld channel %d; max |diff| %.3g\n",
                             d.count, d.firstFrame, d.firstChannel, d.maxAbs);
                status = 1;
            }
        }
    }

#if JUCE_MAC
    malloc_logger = nullptr;
#endif

    if (o.rtCheck)
    {
        const long allocs = rtNew.load() + rtDelete.load() + rtMalloc.load() + rtFree.load();
        std::printf ("rt-check: %s, inside setParams/process: operator new %ld, delete %ld; malloc %ld, free %ld\n",
                     allocs == 0 ? "PASS" : "FAIL", rtNew.load(), rtDelete.load(), rtMalloc.load(), rtFree.load());

        if (allocs != 0)
            status = 1;
    }

    if (has ("--latency"))
        std::printf ("latency\t%d\n", fx.latency());

    if (! writeWav (outFile, output, sampleRate))
    {
        std::fprintf (stderr, "cannot write %s\n", outFile.getFullPathName().toRawUTF8());
        return 2;
    }

    if (o.trace != nullptr && ! writeWav (cwdFile (opts["--trace"]), traceBuffer, sampleRate))
    {
        std::fprintf (stderr, "cannot write %s\n", opts["--trace"].c_str());
        return 2;
    }

    if (o.meters != nullptr)
    {
        juce::String text;

        for (const auto& row : meterRows)
            text << row << "\n";

        if (! cwdFile (opts["--meters"]).replaceWithText (text, false, false, "\n"))
        {
            std::fprintf (stderr, "cannot write %s\n", opts["--meters"].c_str());
            return 2;
        }
    }

    return status;
}
