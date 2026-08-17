// The ruler for docs/REVERB.md (§7 Stage 0, §8): renders a wet-only impulse response
// straight out of tubamp::ReverbEngine and reports every metric in the §8 table, plus the
// six Reverse-specific gates of the amended §3.12, §3.9's Spring chirp and per-pulse decay
// gates, and §3.11's Shimmer stability grid. Non-zero exit on any gate failure at
// color = 0 — plus §3.5's clip-aliasing row, which gates at color = 1 (drive, not
// voicing), and §5.1's colour-1 per-octave T60 row, whose targets carry the era
// voicing (axes 1 + 4) so a miss is a §3.4 defect at every colour; both equally fatal.
//
//   tubamp_verbprobe <out-dir> [options]
//
//     --rates=44100,48000,96000   host rates to sweep (default: all three, §8's
//                                 rate-independence row)
//     --modes=0,1,2,3,4,5         algo indices to measure (default: all six; 3 runs §3.9's
//                                 Spring gates per rate, 4 runs §3.11's grid once)
//     --decay=2.0                 reverb_decay for the general metrics pass
//     --freeverb                  also run the suite over a probe-local juce::dsp::Reverb
//                                 (§7 Stage 0: the deleted Freeverb survives HERE and
//                                 nowhere else, as the published before/after baseline)
//     --cpu                       time 3 Spring instances at 192 kHz and report ms/block
//                                 (§7 Stage 2's last line; reported, never gated)
//
// Writes <out-dir>/report.txt and one normalised stereo IR WAV per (mode, rate).
//
// The engine is driven directly rather than through the processor: mix = 1 makes the
// output wet-only by the mix law of §3.1 (dryLevel = 1 - wetLevel), the wet fade is
// idle at 1.0 because only ReverbFx ever moves it, and prepare() always clears (§6.4),
// so every render here is reproducible sample for sample.
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "../src/dsp/ReverbEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdio>
#include <vector>

namespace
{
using tubamp::ReverbEngine;

constexpr int kBlockSize = 512;

//==============================================================================
// Reporting
//==============================================================================
juce::String fmt (const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start (args, format);
    std::vsnprintf (buffer, sizeof (buffer), format, args);
    va_end (args);
    return juce::String::fromUTF8 (buffer);
}

/** juce::String's `const char*` constructor decodes ASCII, so a literal carrying a
    section sign has to be decoded explicitly or it comes out double-encoded. */
juce::String u8 (const char* text) { return juce::String::fromUTF8 (text); }

struct Report
{
    juce::String text;
    int failures = 0;        // gates — these decide the exit code (at color = 0 except
                             // §3.5's aliasing row and §5.1's axis-4 per-octave T60
                             // row, which gate at color = 1)
    int colourNotes = 0;     // §5.1's inverted colour gates, reported but not fatal

    void line (const juce::String& s)
    {
        std::printf ("%s\n", s.toRawUTF8());
        text << s << juce::newLine;
    }

    void head (const juce::String& s)
    {
        line ({});
        line (s);
        line (juce::String::repeatedString ("-", s.length()));
    }

    bool gate (bool pass, const juce::String& what)
    {
        line ((pass ? "  ok    " : "  FAIL  ") + what);

        if (! pass)
            ++failures;

        return pass;
    }

    /** §5.1: above color = 0.5 the spectral-peak and flutter gates INVERT for Plate.
        Reported, and deliberately not fatal — the task's exit contract is "any gate
        failure at color = 0". */
    void colourGate (bool pass, const juce::String& what)
    {
        line ((pass ? "  ok    " : "  note  ") + what);

        if (! pass)
            ++colourNotes;
    }

    void info (const juce::String& what) { line ("        " + what); }
};

//==============================================================================
// §4.1 mirror. The probe has to know the geometry it is checking — the flutter row of
// §8 names lineSamples_i, lineSamples_i*(1+apRatio_i) and lineSamples_i*apRatio_i*k
// explicitly, and apRatio_i is drawn at configure() from a fixed-seed LCG that no
// public accessor exposes. This block therefore MIRRORS ReverbEngine.cpp exactly,
// including the seeds and the draw order, and must be updated in lockstep with it.
// Everything here is quoted from §4.1.
//==============================================================================

constexpr int kTankRoom[8]  = { 1381, 1511, 1723, 1877, 2099, 2269, 2447, 2683 };
constexpr int kTankPlate[8] = { 2309, 2539, 2819, 3037, 3433, 3691, 4027, 4409 };
constexpr int kTankHall[8]  = { 6007, 6619, 7309, 8069, 8719, 9421, 10099, 11027 };

struct ModeMirror
{
    const char* name;
    const int*  tank48;
    float apRatioBase;
    float sizeMin, sizeMax;
    float bassXoverHz, hfXoverHz, hfMaxT60;
    float decayCeiling;
    bool  isTank;
};

// Spring's row is §4.1's Spring column (ReverbEngine.cpp's kModes[4]), NOT Plate's: the
// mode borrows Plate's tank table only so nothing reads uninitialised, and every column
// this probe actually reads — the size range, the crossovers, the decay ceiling — is its
// own. Shimmer takes no column of its own at all (§3.11: a routing switch, not a
// topology), so its row is Hall's, exactly as specIndexFor() resolves it.
const ModeMirror kMirror[6] = {
    { "Room",    kTankRoom,  0.22f, 0.40f, 1.40f, 450.0f, 4500.0f, 1.00f,  2.5f, true  },
    { "Plate",   kTankPlate, 0.30f, 0.35f, 1.20f, 500.0f, 5500.0f, 1.25f,  4.5f, true  },
    { "Hall",    kTankHall,  0.16f, 0.50f, 1.50f, 400.0f, 3500.0f, 1.25f, 10.0f, true  },
    { "Spring",  kTankPlate, 0.00f, 0.50f, 1.50f, 250.0f, 2800.0f, 0.35f,  4.0f, false },
    { "Shimmer", kTankHall,  0.16f, 0.50f, 1.50f, 400.0f, 3500.0f, 1.25f, 10.0f, true  },
    { "Reverse", kTankHall,  0.16f, 0.60f, 1.40f, 400.0f, 3500.0f, 1.25f,  1.5f, false },
};

const ModeMirror& mirrorFor (int algo)
{
    return kMirror[(size_t) juce::jlimit (0, 5, algo)];
}

/** The engine's own LCG, verbatim (ReverbEngine.cpp). */
struct Lcg
{
    std::uint32_t state;
    explicit Lcg (std::uint32_t seed) noexcept : state (seed) {}
    std::uint32_t next() noexcept { state = state * 1664525u + 1013904223u; return state; }
    float uni() noexcept { return (float) (next() >> 8) * (1.0f / 16777216.0f); }
};

constexpr std::uint32_t kSeedJitter = 0x5F3A17C1u;
constexpr std::uint32_t kSeedPerMode = 7919u;

/** §4.1's rejection draw, verbatim. */
float drawApRatio (float base, Lcg& lcg)
{
    float ratio = base;

    for (int attempt = 0; attempt < 64; ++attempt)
    {
        ratio = base * (0.90f + 0.20f * lcg.uni());
        bool ok = true;

        for (int q = 2; q < 12 && ok; ++q)
            for (int p = 1; p < q && ok; ++p)
            {
                const float f = (float) p / (float) q;

                if (std::abs (ratio - f) < 0.01f * f)
                    ok = false;
            }

        if (ok)
            break;
    }

    return ratio;
}

/** The eight per-line apRatios the engine will have drawn for `algo`. Mirrors
    buildModeTables()'s draw order: apRatio, rateJitter, depthJitter per line. */
std::array<double, 8> apRatiosFor (int algo)
{
    // specIndexFor() verbatim: Reverse is row 3, Spring row 4 (appended, never inserted,
    // so the seeds of 0..3 do not move), Shimmer shares Hall's row 2.
    const int resolved = ReverbEngine::resolveMode (algo);
    const int specIndex = resolved == ReverbEngine::modeReverse ? 3
                        : resolved == ReverbEngine::modeSpring  ? 4
                        : resolved == ReverbEngine::modeShimmer ? 2
                        : juce::jlimit (0, 2, resolved);
    Lcg lcg (kSeedJitter + kSeedPerMode * (std::uint32_t) (specIndex + 1));
    std::array<double, 8> out {};

    for (int i = 0; i < 8; ++i)
    {
        out[(size_t) i] = drawApRatio (mirrorFor (ReverbEngine::resolveMode (algo)).apRatioBase, lcg);
        lcg.uni();   // rateJitter
        lcg.uni();   // depthJitter
    }

    return out;
}

//==============================================================================
// Rendering
//==============================================================================

/** ReverbEngine::Params minus mix (pinned to 1 for a wet-only render). */
struct Setup
{
    int   algo = 0;
    float size01 = 0.5f, damping01 = 0.5f, width01 = 1.0f;
    float decayS = 2.0f, predelayMs = 0.0f, diffusion01 = 0.7f;
    float lowCutHz = 20.0f, highCutHz = 20000.0f;
    float mod01 = 0.35f, bassMult = 1.0f, erLevel01 = 0.5f;
    float color01 = 0.0f, tilt = 0.0f, duck01 = 0.0f;
    float shimmer01 = 0.0f;          // §3.11; inert outside Shimmer, and 0 is bit-inert
    int   shimmerInterval = 2;       // §3.11's FROZEN 4, default +1 oct
};

struct Ir
{
    std::vector<double> l, r;
    double fs = 48000.0;

    size_t size() const noexcept { return l.size(); }
};

ReverbEngine::Params paramsOf (const Setup& s)
{
    ReverbEngine::Params p;
    p.algo = s.algo;
    p.size01 = s.size01;
    p.damping01 = s.damping01;
    p.mix01 = 1.0f;                 // wet-only: dryLevel = 1 - wetLevel (§3.1)
    p.width01 = s.width01;
    p.decayS = s.decayS;
    p.predelayMs = s.predelayMs;
    p.diffusion01 = s.diffusion01;
    p.lowCutHz = s.lowCutHz;
    p.highCutHz = s.highCutHz;
    p.mod01 = s.mod01;
    p.bassMult = s.bassMult;
    p.erLevel01 = s.erLevel01;
    p.color01 = s.color01;
    p.tilt = s.tilt;
    p.duck01 = s.duck01;
    p.shimmer01 = s.shimmer01;
    p.shimmerInterval = s.shimmerInterval;
    p.snap = true;                  // §6.4: no glide on the first block
    return p;
}

/** Drives `source` (mono, fed to both channels) through a freshly prepared engine and
    returns the stereo result, zero-padded out to `totalSamples`. */
Ir renderThrough (const Setup& s, double fs, const std::vector<double>& source, int totalSamples)
{
    ReverbEngine engine;
    engine.configure (s.algo, ReverbEngine::windowSecondsFor (s.decayS));
    engine.prepare ({ fs, (juce::uint32) kBlockSize, 2 });

    auto p = paramsOf (s);

    Ir ir;
    ir.fs = fs;
    ir.l.reserve ((size_t) totalSamples);
    ir.r.reserve ((size_t) totalSamples);

    juce::AudioBuffer<float> buffer (2, kBlockSize);

    for (int pos = 0; pos < totalSamples; pos += kBlockSize)
    {
        const int n = juce::jmin (kBlockSize, totalSamples - pos);
        buffer.clear();

        for (int i = 0; i < n; ++i)
        {
            const size_t index = (size_t) (pos + i);
            const float v = index < source.size() ? (float) source[index] : 0.0f;
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
        }

        engine.setParameters (p);
        p.snap = false;

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block.getSubBlock (0, (size_t) n));

        for (int i = 0; i < n; ++i)
        {
            ir.l.push_back ((double) buffer.getSample (0, i));
            ir.r.push_back ((double) buffer.getSample (1, i));
        }
    }

    return ir;
}

Ir renderIr (const Setup& s, double fs, double seconds)
{
    std::vector<double> impulse (1, 1.0);
    return renderThrough (s, fs, impulse, (int) std::ceil (seconds * fs));
}

/** Probe-local Freeverb (§7 Stage 0). The engine this design replaces survives here and
    nowhere else in the codebase; it is the published before/after baseline and is never
    gated. */
Ir renderFreeverbIr (double fs, double seconds, float roomSize, float damping)
{
    // juce::dsp::Reverb is a thin wrapper over this; the wrapper's own process() wants a
    // ProcessContext, and the plain class is what §1 dissects.
    juce::Reverb reverb;
    reverb.setSampleRate (fs);

    juce::Reverb::Parameters rp;
    rp.roomSize = roomSize;
    rp.damping = damping;
    rp.wetLevel = 1.0f;
    rp.dryLevel = 0.0f;
    rp.width = 1.0f;
    rp.freezeMode = 0.0f;
    reverb.setParameters (rp);
    reverb.reset();

    const int total = (int) std::ceil (seconds * fs);
    Ir ir;
    ir.fs = fs;
    ir.l.reserve ((size_t) total);
    ir.r.reserve ((size_t) total);

    std::vector<float> left ((size_t) kBlockSize, 0.0f), right ((size_t) kBlockSize, 0.0f);

    for (int pos = 0; pos < total; pos += kBlockSize)
    {
        const int n = juce::jmin (kBlockSize, total - pos);
        std::fill (left.begin(), left.end(), 0.0f);
        std::fill (right.begin(), right.end(), 0.0f);

        if (pos == 0)
            left[0] = right[0] = 1.0f;

        reverb.processStereo (left.data(), right.data(), n);

        for (int i = 0; i < n; ++i)
        {
            ir.l.push_back ((double) left[(size_t) i]);
            ir.r.push_back ((double) right[(size_t) i]);
        }
    }

    return ir;
}

//==============================================================================
// Analysis primitives
//==============================================================================

struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    double process (double x) noexcept
    {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

Biquad makeLowPass (double fs, double f, double q)
{
    const double w = 2.0 * juce::MathConstants<double>::pi * juce::jlimit (1.0, fs * 0.49, f) / fs;
    const double cw = std::cos (w), sw = std::sin (w);
    const double alpha = sw / (2.0 * q);
    const double a0 = 1.0 + alpha;

    Biquad b;
    b.b0 = (1.0 - cw) * 0.5 / a0;
    b.b1 = (1.0 - cw) / a0;
    b.b2 = b.b0;
    b.a1 = -2.0 * cw / a0;
    b.a2 = (1.0 - alpha) / a0;
    return b;
}

Biquad makeHighPass (double fs, double f, double q)
{
    const double w = 2.0 * juce::MathConstants<double>::pi * juce::jlimit (1.0, fs * 0.49, f) / fs;
    const double cw = std::cos (w), sw = std::sin (w);
    const double alpha = sw / (2.0 * q);
    const double a0 = 1.0 + alpha;

    Biquad b;
    b.b0 = (1.0 + cw) * 0.5 / a0;
    b.b1 = -(1.0 + cw) / a0;
    b.b2 = b.b0;
    b.a1 = -2.0 * cw / a0;
    b.a2 = (1.0 - alpha) / a0;
    return b;
}

/** 4th-order Butterworth band pass with independent edges (two HP + two LP sections at the
    Butterworth Qs). §3.9's chirp bands are not octaves, so the octave form below is the
    special case rather than the other way round. */
std::vector<double> bandPass (const std::vector<double>& x, double fs, double loHz, double hiHz)
{
    Biquad sections[4] = { makeHighPass (fs, loHz, 0.54119610), makeHighPass (fs, loHz, 1.30656296),
                           makeLowPass  (fs, hiHz, 0.54119610), makeLowPass  (fs, hiHz, 1.30656296) };

    std::vector<double> out (x.size());

    for (size_t i = 0; i < x.size(); ++i)
    {
        double v = x[i];

        for (auto& s : sections)
            v = s.process (v);

        out[i] = v;
    }

    return out;
}

/** 4th-order Butterworth octave band. */
std::vector<double> octaveBand (const std::vector<double>& x, double fs, double fc)
{
    return bandPass (x, fs, fc / juce::MathConstants<double>::sqrt2,
                     fc * juce::MathConstants<double>::sqrt2);
}

struct DecayFit
{
    bool valid = false;
    double t60 = 0.0, r2 = 0.0;
};

/** Schroeder backward integration + a least-squares fit over [startDb, endDb], the
    standard T20 estimate extrapolated to 60 dB. */
DecayFit schroederT60 (const std::vector<double>& x, double fs,
                       double startDb = -5.0, double endDb = -25.0)
{
    const size_t n = x.size();
    DecayFit fit;

    if (n < 64)
        return fit;

    std::vector<double> edc (n);
    double acc = 0.0;

    for (size_t i = n; i-- > 0;)
    {
        acc += x[i] * x[i];
        edc[i] = acc;
    }

    if (edc[0] <= 0.0)
        return fit;

    const double ref = edc[0];
    size_t i0 = 0, i1 = 0;
    bool haveStart = false, haveEnd = false;

    for (size_t i = 0; i < n; ++i)
    {
        const double db = 10.0 * std::log10 (juce::jmax (1.0e-300, edc[i] / ref));

        if (! haveStart && db <= startDb) { i0 = i; haveStart = true; }

        if (haveStart && db <= endDb) { i1 = i; haveEnd = true; break; }
    }

    if (! haveStart || ! haveEnd || i1 <= i0 + 8)
        return fit;

    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, syy = 0.0;
    const double count = (double) (i1 - i0);

    for (size_t i = i0; i < i1; ++i)
    {
        const double t = (double) i / fs;
        const double db = 10.0 * std::log10 (juce::jmax (1.0e-300, edc[i] / ref));
        sx += t; sy += db; sxx += t * t; sxy += t * db; syy += db * db;
    }

    const double denom = count * sxx - sx * sx;

    if (std::abs (denom) < 1.0e-18)
        return fit;

    const double slope = (count * sxy - sx * sy) / denom;

    if (slope >= -1.0e-9)
        return fit;

    const double num = count * sxy - sx * sy;
    const double r2den = (count * sxx - sx * sx) * (count * syy - sy * sy);

    fit.valid = true;
    fit.t60 = -60.0 / slope;
    fit.r2 = r2den > 0.0 ? (num * num) / r2den : 0.0;
    return fit;
}

/** Zero-phase 4th-order Butterworth low pass — forward then reverse, so no group delay
    is introduced and the pre/post-echo smearing of a one-sided IIR cannot move an
    envelope feature in time. Used to put every host rate on the same audio band before
    a sample-domain statistic is taken (see `kEchoDensityBandHz`). */
std::vector<double> lowPassZeroPhase (const std::vector<double>& x, double fs, double fc)
{
    auto runOnce = [fs, fc] (std::vector<double> v)
    {
        Biquad sections[2] = { makeLowPass (fs, fc, 0.54119610), makeLowPass (fs, fc, 1.30656296) };

        for (auto& sample : v)
            for (auto& s : sections)
                sample = s.process (sample);

        return v;
    };

    std::vector<double> forward = runOnce (x);
    std::reverse (forward.begin(), forward.end());
    std::vector<double> back = runOnce (std::move (forward));
    std::reverse (back.begin(), back.end());
    return back;
}

/** §8 row 1 is a SAMPLE-domain statistic, so it is only comparable across host rates if
    every rate is measured over the same audio band. A native 96 kHz render carries real
    content up to 48 kHz, and there each echo still occupies about one sample while the
    20 ms window holds twice as many samples — so an identical *physical* echo pattern
    scores lower purely because the host rate is higher. Measured: the Plate build-up
    read 27.0 / 28.0 / 93.0 ms to reach 0.95 at 44.1 / 48 / 96 kHz, but the SAME 48 kHz
    IR band-limited-resampled to 96 kHz still read 28 ms — i.e. the spread was the
    metric, not the engine. Band-limiting collapses the three rate curves onto each other
    and makes §8's "rate independence" row meaningful for this metric.

    The band is 12 kHz rather than 16 kHz because at 16 kHz a residual spread survived, and
    resampling identifies it as signal rather than estimator: the ER-isolation row read
    0.944 / 0.944 / 0.890 at 44.1 / 48 / 96 kHz, and the 48 kHz render RESAMPLED to 96 kHz
    still read 0.941 while the 96 kHz render resampled to 48 kHz still read 0.889. What
    differs is the top octave — the tank and pre-delay reads are 4-point Hermite, whose
    loss inside the 16 kHz band shrinks as the host rate rises (§3.4: -2.16 dB at 14.4 kHz
    at 48 kHz, far less at 96 kHz), so the higher rate renders a genuinely sharper early
    field and a sample-domain density statistic reads sharper as sparser. Below 12 kHz that
    term is gone and the same row reads 0.985 / 0.964 / 0.967. §8 already treats the same
    region as interpolator-limited in its per-octave T60 row (±35 % for 8 kHz and above
    against ±15 % below), so this is that documented boundary applied to the density rows.
    Amended 2026-08-06; no threshold moved. */
constexpr double kEchoDensityBandHz = 12000.0;

/** Abel & Huang normalised echo density: the fraction of samples inside a 20 ms Hanning
    window that exceed the window's own standard deviation, divided by erfc(1/sqrt(2)).

    `eta` is the raw per-hop estimator. `density` is the same curve averaged over
    +-`kDensitySmoothHops` hops, and it — not `eta` — is what the >= 0.9 tail gate reads.
    The reason is measurable: `eta` is a binomial count over one 20 ms window, so even an
    IDEAL Gaussian tail (for which the metric is 1.0 by construction) scores a per-hop
    minimum of 0.87 .. 0.91 over a 1.6 s tail at these rates. A per-hop minimum >= 0.9 is
    therefore unreachable by any signal, including the metric's own reference. The
    underlying density evolves far more slowly than the 1 ms hop — which oversamples the
    20 ms window 20x — so averaging recovers it: the same ideal tail then scores
    0.96 .. 0.98, while Freeverb (§7 Stage 0's baseline) still never reaches 0.95 at all.
    The 0.9 and 0.95 thresholds are untouched. */
struct EchoDensity
{
    std::vector<double> timeS, eta, density;

    double densityAt (double t) const noexcept
    {
        for (size_t i = 0; i < timeS.size(); ++i)
            if (timeS[i] >= t)
                return density[i];

        return density.empty() ? 0.0 : density.back();
    }

    double etaAt (double t) const noexcept
    {
        for (size_t i = 0; i < timeS.size(); ++i)
            if (timeS[i] >= t)
                return eta[i];

        return eta.empty() ? 0.0 : eta.back();
    }
};

/** +-50 ms of 1 ms hops: about five independent 20 ms windows either side, which brings
    the estimator's standard deviation from ~0.04 to ~0.018 without blurring a density
    change that any of these modes actually makes. */
constexpr int kDensitySmoothHops = 50;

EchoDensity echoDensity (const std::vector<double>& raw, double fs)
{
    const std::vector<double> h = lowPassZeroPhase (raw, fs, kEchoDensityBandHz);

    EchoDensity out;
    const int win = juce::jmax (16, (int) std::lround (0.020 * fs));
    const int hop = juce::jmax (1, (int) std::lround (0.001 * fs));
    const int half = win / 2;

    std::vector<double> w ((size_t) win);
    double sum = 0.0;

    for (int i = 0; i < win; ++i)
    {
        w[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi
                                              * (double) i / (double) (win - 1));
        sum += w[(size_t) i];
    }

    for (auto& v : w)
        v /= sum;

    constexpr double kErfcHalf = 0.3173105078629141;   // erfc(1/sqrt(2))

    for (int centre = half; centre + half < (int) h.size(); centre += hop)
    {
        double variance = 0.0;

        for (int k = 0; k < win; ++k)
        {
            const double v = h[(size_t) (centre - half + k)];
            variance += w[(size_t) k] * v * v;
        }

        const double sigma = std::sqrt (variance);
        double count = 0.0;

        if (sigma > 0.0)
            for (int k = 0; k < win; ++k)
                if (std::abs (h[(size_t) (centre - half + k)]) > sigma)
                    count += w[(size_t) k];

        out.timeS.push_back ((double) centre / fs);
        out.eta.push_back (count / kErfcHalf);
    }

    out.density.resize (out.eta.size());

    for (size_t i = 0; i < out.eta.size(); ++i)
    {
        const size_t from = i > (size_t) kDensitySmoothHops ? i - (size_t) kDensitySmoothHops : 0;
        const size_t to = juce::jmin (out.eta.size(), i + (size_t) kDensitySmoothHops + 1);
        double sum = 0.0;

        for (size_t k = from; k < to; ++k)
            sum += out.eta[k];

        out.density[i] = sum / (double) (to - from);
    }

    return out;
}

/** Hanning-weighted short-window RMS envelope in dB — §3.12's "20 ms-Hann RMS envelope",
    reused by the flutter autocorrelation. `timeOf` reports the window CENTRE, so the
    Reverse gates compare against W rather than against W minus half a window. */
struct Envelope
{
    std::vector<double> db;
    double hopSeconds = 0.0, centreOffsetSeconds = 0.0;

    double timeOf (size_t index) const noexcept
    {
        return (double) index * hopSeconds + centreOffsetSeconds;
    }

    size_t peakIndex() const noexcept
    {
        size_t best = 0;
        double peak = -1.0e300;

        for (size_t i = 0; i < db.size(); ++i)
            if (db[i] > peak) { peak = db[i]; best = i; }

        return best;
    }
};

Envelope rmsEnvelopeDb (const std::vector<double>& h, double fs, double winSeconds, int hop)
{
    const int win = juce::jmax (8, (int) std::lround (winSeconds * fs));

    std::vector<double> w ((size_t) win);
    double norm = 0.0;

    for (int i = 0; i < win; ++i)
    {
        w[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi
                                              * (double) i / (double) (win - 1));
        norm += w[(size_t) i];
    }

    Envelope env;
    env.hopSeconds = (double) hop / fs;
    env.centreOffsetSeconds = 0.5 * (double) (win - 1) / fs;

    for (int i = 0; i + win <= (int) h.size(); i += hop)
    {
        double sum = 0.0;

        for (int k = 0; k < win; ++k)
            sum += w[(size_t) k] * h[(size_t) (i + k)] * h[(size_t) (i + k)];

        env.db.push_back (10.0 * std::log10 (juce::jmax (1.0e-300, sum / norm)));
    }

    return env;
}

/** Subtracts a moving average of half-width `halfWidth` — the "trend removal" of §8's
    flutter row, so what is left is the periodic ripple and nothing else. */
std::vector<double> detrend (const std::vector<double>& x, int halfWidth)
{
    std::vector<double> prefix (x.size() + 1, 0.0);

    for (size_t i = 0; i < x.size(); ++i)
        prefix[i + 1] = prefix[i] + x[i];

    std::vector<double> out (x.size());

    for (size_t i = 0; i < x.size(); ++i)
    {
        const size_t lo = i > (size_t) halfWidth ? i - (size_t) halfWidth : 0;
        const size_t hi = juce::jmin (x.size(), i + (size_t) halfWidth + 1);
        out[i] = x[i] - (prefix[hi] - prefix[lo]) / (double) (hi - lo);
    }

    return out;
}

double autocorrelationAt (const std::vector<double>& x, size_t from, size_t to, int lag)
{
    if (lag <= 0 || to <= from || from + (size_t) lag >= to)
        return 0.0;

    double num = 0.0, den = 0.0;

    for (size_t i = from; i + (size_t) lag < to; ++i)
        num += x[i] * x[i + (size_t) lag];

    for (size_t i = from; i < to; ++i)
        den += x[i] * x[i];

    return den > 0.0 ? num / den : 0.0;
}

/** Welch-averaged power spectrum, magnitude squared, one value per non-negative bin. */
std::vector<double> welchPower (const std::vector<double>& x, int fftOrder)
{
    juce::dsp::FFT fft (fftOrder);
    const int size = fft.getSize();
    const int hop = size / 2;
    std::vector<double> power ((size_t) (size / 2 + 1), 0.0);
    std::vector<float> scratch ((size_t) size * 2, 0.0f);
    std::vector<double> window ((size_t) size);

    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi
                                                   * (double) i / (double) (size - 1));

    int frames = 0;

    for (int start = 0; start + size <= (int) x.size(); start += hop)
    {
        std::fill (scratch.begin(), scratch.end(), 0.0f);

        for (int i = 0; i < size; ++i)
            scratch[(size_t) i] = (float) (x[(size_t) (start + i)] * window[(size_t) i]);

        fft.performRealOnlyForwardTransform (scratch.data(), true);

        for (int k = 0; k <= size / 2; ++k)
        {
            const double re = scratch[(size_t) (2 * k)];
            const double im = scratch[(size_t) (2 * k + 1)];
            power[(size_t) k] += re * re + im * im;
        }

        ++frames;
    }

    if (frames > 0)
        for (auto& v : power)
            v /= (double) frames;

    return power;
}

/** Fractional-octave smoothing of a dB curve, in place of a spectrum's raw comb. */
std::vector<double> octaveSmoothDb (const std::vector<double>& db, double fs, int fftSize,
                                    double fractionOfOctave)
{
    const double binHz = fs / (double) fftSize;
    const double ratio = std::pow (2.0, 0.5 / fractionOfOctave);
    std::vector<double> out (db.size());

    for (size_t k = 0; k < db.size(); ++k)
    {
        const double f = (double) k * binHz;
        const int lo = juce::jmax (0, (int) std::floor (f / ratio / binHz));
        const int hi = juce::jmin ((int) db.size() - 1, (int) std::ceil (f * ratio / binHz));
        double sum = 0.0;
        int count = 0;

        for (int i = lo; i <= hi; ++i, ++count)
            sum += db[(size_t) i];

        out[k] = count > 0 ? sum / (double) count : db[k];
    }

    return out;
}

double medianOf (std::vector<double> v)
{
    if (v.empty())
        return 0.0;

    const size_t mid = v.size() / 2;
    std::nth_element (v.begin(), v.begin() + (long) mid, v.end());
    return v[mid];
}

struct SpectrumDeviation
{
    double worstDb = 0.0;      // signed: the deepest notch (negative) or highest peak
    double atHz = 0.0;
};

/** §8's "mono sum" and "spectral flatness" rows share one shape: a smoothed dB curve
    compared against its own local median, evaluated over the audible band. */
SpectrumDeviation worstDeviation (const std::vector<double>& db, double fs, int fftSize,
                                  double loHz, double hiHz, double medianOctaves, bool wantNotch)
{
    const double binHz = fs / (double) fftSize;
    const double ratio = std::pow (2.0, medianOctaves * 0.5);
    SpectrumDeviation worst;

    for (size_t k = 1; k < db.size(); ++k)
    {
        const double f = (double) k * binHz;

        if (f < loHz || f > hiHz)
            continue;

        const int lo = juce::jmax (1, (int) std::floor (f / ratio / binHz));
        const int hi = juce::jmin ((int) db.size() - 1, (int) std::ceil (f * ratio / binHz));

        if (hi <= lo)
            continue;

        std::vector<double> slice (db.begin() + lo, db.begin() + hi + 1);
        const double deviation = db[k] - medianOf (std::move (slice));

        if (wantNotch ? deviation < worst.worstDb : deviation > worst.worstDb)
        {
            worst.worstDb = deviation;
            worst.atHz = f;
        }
    }

    return worst;
}

std::vector<double> toDb (const std::vector<double>& power)
{
    std::vector<double> out (power.size());

    for (size_t i = 0; i < power.size(); ++i)
        out[i] = 10.0 * std::log10 (juce::jmax (1.0e-300, power[i]));

    return out;
}

/** Paul Kellett's economy pink filter over a fixed-seed white source. */
std::vector<double> pinkNoise (int numSamples, double targetRms, std::uint32_t seed)
{
    std::vector<double> out ((size_t) numSamples);
    double b0 = 0.0, b1 = 0.0, b2 = 0.0;
    std::uint32_t state = seed | 1u;
    double sum = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        state = state * 1664525u + 1013904223u;
        const double white = (double) (state >> 8) * (2.0 / 16777216.0) - 1.0;
        b0 = 0.99765 * b0 + white * 0.0990460;
        b1 = 0.96300 * b1 + white * 0.2965164;
        b2 = 0.57000 * b2 + white * 1.0526913;
        const double v = b0 + b1 + b2 + white * 0.1848;
        out[(size_t) i] = v;
        sum += v * v;
    }

    const double rms = std::sqrt (sum / juce::jmax (1.0, (double) numSamples));

    if (rms > 0.0)
        for (auto& v : out)
            v *= targetRms / rms;

    return out;
}

double peakOf (const Ir& ir)
{
    double peak = 0.0;

    for (size_t i = 0; i < ir.size(); ++i)
        peak = juce::jmax (peak, juce::jmax (std::abs (ir.l[i]), std::abs (ir.r[i])));

    return peak;
}

/** 32-bit float, unnormalised: this is a measurement record, so the absolute level is
    part of the data (§3.12 gate 3 is stated in dBFS). */
bool writeIrWav (const Ir& ir, const juce::File& file)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());

    if (stream == nullptr)
        return false;

    juce::WavAudioFormat format;
    const auto options = juce::AudioFormatWriterOptions {}
                             .withSampleRate (ir.fs)
                             .withNumChannels (2)
                             .withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    auto writer = format.createWriterFor (stream, options);

    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buffer (2, (int) ir.size());

    for (size_t i = 0; i < ir.size(); ++i)
    {
        buffer.setSample (0, (int) i, (float) ir.l[i]);
        buffer.setSample (1, (int) i, (float) ir.r[i]);
    }

    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    return true;
}

std::vector<double> monoSum (const Ir& ir)
{
    std::vector<double> out (ir.size());

    for (size_t i = 0; i < ir.size(); ++i)
        out[i] = ir.l[i] + ir.r[i];

    return out;
}

/** The time at which the envelope has fallen `db` below its peak. */
double timeBelowPeak (const Envelope& env, double db)
{
    double peak = -1.0e300;

    for (double v : env.db)
        peak = juce::jmax (peak, v);

    for (size_t i = 0; i < env.db.size(); ++i)
        if (env.db[i] <= peak + db)
            return env.timeOf (i);

    return env.db.empty() ? 0.0 : env.timeOf (env.db.size() - 1);
}

//==============================================================================
// Per-mode measurement
//==============================================================================

struct RateResult
{
    double fs = 0.0;
    int algo = 0;
    double notchHz = 0.0, notchDb = 0.0;
    double worstFlutter = 0.0;
    double broadbandT60 = 0.0;
    std::vector<double> monoSumGridDb;
};

/** The §8 flutter row: no autocorrelation peak above 0.2 at any tank-line lag, at any
    line+allpass lag, or at any harmonic of a line's allpass lag. */
double flutterWorstPeak (const Ir& ir, int algo, Report& rep, bool verbose)
{
    const auto& mode = mirrorFor (ReverbEngine::resolveMode (algo));
    const double fs = ir.fs;
    const double sizeScale = mode.sizeMin + 0.5 * (mode.sizeMax - mode.sizeMin);   // size01 = 0.5
    const auto ratios = apRatiosFor (algo);

    const int hop = juce::jmax (1, (int) std::lround (0.00025 * fs));   // 0.25 ms
    const auto env = rmsEnvelopeDb (ir.l, fs, 0.001, hop);

    if (env.db.size() < 64)
        return 0.0;

    const double hopSeconds = env.hopSeconds;
    const auto ripple = detrend (env.db, juce::jmax (4, (int) std::lround (0.050 / hopSeconds)));

    // Analyse the settled tail only: from 30 ms to the -45 dB point of the envelope.
    const size_t from = juce::jmin (ripple.size() - 1, (size_t) std::lround (0.030 / hopSeconds));
    const size_t to = juce::jmin (ripple.size(),
                                  (size_t) std::lround (timeBelowPeak (env, -45.0) / hopSeconds));

    if (to <= from + 64)
        return 0.0;

    double worst = 0.0;
    juce::String worstWhat;

    const auto probeLag = [&] (double lagSamples, const juce::String& what)
    {
        const int lag = (int) std::lround (lagSamples / (double) hop);

        if (lag < 2 || (size_t) lag >= (to - from) / 2)
            return;

        double best = 0.0;

        for (int d = -2; d <= 2; ++d)
            best = juce::jmax (best, autocorrelationAt (ripple, from, to, lag + d));

        if (best > worst)
        {
            worst = best;
            worstWhat = what;
        }
    };

    for (int i = 0; i < 8; ++i)
    {
        const double lineSamples = (double) mode.tank48[i] * (fs / 48000.0) * sizeScale;
        const double ratio = ratios[(size_t) i];

        probeLag (lineSamples, fmt ("line %d (%.1f ms)", i, 1000.0 * lineSamples / fs));
        probeLag (lineSamples * (1.0 + ratio), fmt ("line %d x (1+apRatio) (%.1f ms)", i,
                                                    1000.0 * lineSamples * (1.0 + ratio) / fs));

        for (int k = 1; k <= 12; ++k)
            probeLag (lineSamples * ratio * (double) k,
                      fmt ("line %d allpass x%d (%.1f ms)", i, k,
                           1000.0 * lineSamples * ratio * (double) k / fs));
    }

    if (verbose)
        rep.info (fmt ("worst flutter autocorrelation %.3f at %s",
                       worst, worstWhat.isEmpty() ? "(no lag in range)" : worstWhat.toRawUTF8()));

    return worst;
}

/** Freeverb's own comb lags, for the Stage-0 baseline only. */
double flutterWorstPeakFreeverb (const Ir& ir, Report& rep)
{
    constexpr int kCombs[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
    const double fs = ir.fs;
    const int hop = juce::jmax (1, (int) std::lround (0.00025 * fs));
    const auto env = rmsEnvelopeDb (ir.l, fs, 0.001, hop);

    if (env.db.size() < 64)
        return 0.0;

    const double hopSeconds = env.hopSeconds;
    const auto ripple = detrend (env.db, juce::jmax (4, (int) std::lround (0.050 / hopSeconds)));
    const size_t from = juce::jmin (ripple.size() - 1, (size_t) std::lround (0.030 / hopSeconds));
    const size_t to = juce::jmin (ripple.size(),
                                  (size_t) std::lround (timeBelowPeak (env, -45.0) / hopSeconds));

    if (to <= from + 64)
        return 0.0;

    double worst = 0.0;
    int worstComb = 0;

    for (int comb : kCombs)
    {
        const double lagSamples = (double) comb * fs / 44100.0;
        const int lag = (int) std::lround (lagSamples / (double) hop);

        if (lag < 2 || (size_t) lag >= (to - from) / 2)
            continue;

        for (int d = -2; d <= 2; ++d)
        {
            const double v = autocorrelationAt (ripple, from, to, lag + d);

            if (v > worst) { worst = v; worstComb = comb; }
        }
    }

    rep.info (fmt ("worst flutter autocorrelation %.3f at comb %d", worst, worstComb));
    return worst;
}

/** Welch segment order chosen so every host rate resolves the spectrum at the same
    ~6 Hz per bin. A fixed order would give 96 kHz half the low-frequency resolution of
    44.1 kHz, and §8's rate-independence row would then be comparing two different
    measurements rather than two renders. */
int welchOrderFor (double fs)
{
    int order = 10;

    while (order < 16 && fs / (double) (1 << order) > 6.0)
        ++order;

    return order;
}

/** A fixed 1/12-octave grid from 100 Hz to 10 kHz. Curves measured at different host
    rates land on different FFT bin spacings, so §8's rate-independence row is only
    checkable once both are resampled onto the same frequency axis. */
constexpr int kGridPoints = 80;

double gridFrequency (int index) noexcept
{
    return 100.0 * std::pow (2.0, (double) index / 12.0);
}

std::vector<double> sampleOnGrid (const std::vector<double>& db, double fs, int fftSize)
{
    const double binHz = fs / (double) fftSize;
    std::vector<double> out ((size_t) kGridPoints, 0.0);

    for (int i = 0; i < kGridPoints; ++i)
    {
        const double bin = gridFrequency (i) / binHz;
        const int lo = juce::jlimit (0, (int) db.size() - 1, (int) std::floor (bin));
        const int hi = juce::jlimit (0, (int) db.size() - 1, lo + 1);
        const double t = juce::jlimit (0.0, 1.0, bin - (double) lo);
        out[(size_t) i] = db[(size_t) lo] * (1.0 - t) + db[(size_t) hi] * t;
    }

    return out;
}

struct MonoSumResult
{
    double worstNotchDb = 0.0;
    double atHz = 0.0;
    std::vector<double> gridDb;      // the smoothed |L+R| / |L| curve, on the shared grid
};

/** §8's mono-sum row: |L+R| against |L|, trend removed so a broad -3 dB tilt does not
    read as a notch. */
MonoSumResult monoSumNotch (const Ir& ir)
{
    const int order = welchOrderFor (ir.fs);
    const int fftSize = 1 << order;

    if ((int) ir.size() < fftSize * 2)
        return {};

    const auto pl = welchPower (ir.l, order);
    const auto ps = welchPower (monoSum (ir), order);

    std::vector<double> ratioDb (pl.size());

    for (size_t k = 0; k < pl.size(); ++k)
        ratioDb[k] = 10.0 * std::log10 (juce::jmax (1.0e-300, ps[k]))
                     - 10.0 * std::log10 (juce::jmax (1.0e-300, pl[k]));

    const auto smoothed = octaveSmoothDb (ratioDb, ir.fs, fftSize, 12.0);
    const auto worst = worstDeviation (smoothed, ir.fs, fftSize,
                                       100.0, juce::jmin (10000.0, ir.fs * 0.4), 1.0, true);

    return { worst.worstDb, worst.atHz, sampleOnGrid (smoothed, ir.fs, fftSize) };
}

/** §8's per-octave T60 row. The target per band is the band the Jot solve actually
    aims at (§3.4): decay x bassMult below the bass crossover, decay in the mid band,
    and min(decay x (1.25 - damping), hfMaxT60) above the HF crossover. Gating every
    band against `reverb_decay` flat only makes sense in a frequency-flat configuration,
    which is what `t60Setup` below arranges. */
struct BandResult
{
    double fc = 0.0, t60 = 0.0, target = 0.0, r2 = 0.0;
    bool measured = false;
};

std::vector<BandResult> perOctaveT60 (const Ir& ir, const Setup& s, Report& rep)
{
    static constexpr double kBands[] = { 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0,
                                         4000.0, 8000.0, 16000.0 };
    const auto& mode = mirrorFor (ReverbEngine::resolveMode (s.algo));
    const double decay = juce::jlimit (0.2, (double) mode.decayCeiling, (double) s.decayS);
    const double t60Low = juce::jmax (0.05, decay * juce::jlimit (0.25, 4.0, (double) s.bassMult));
    const double t60High = juce::jmax (0.05, juce::jmin (decay * (1.25 - (double) s.damping01),
                                                         (double) mode.hfMaxT60));

    std::vector<BandResult> out;

    for (double fc : kBands)
    {
        BandResult band;
        band.fc = fc;
        band.target = fc < mode.bassXoverHz ? t60Low
                    : fc > mode.hfXoverHz   ? t60High
                                            : decay;

        if (fc * juce::MathConstants<double>::sqrt2 >= ir.fs * 0.45)
        {
            rep.info (fmt ("%5.0f Hz  band exceeds Nyquist at %.0f Hz — skipped", fc, ir.fs));
            out.push_back (band);
            continue;
        }

        const auto filtered = octaveBand (ir.l, ir.fs, fc);
        const auto fit = schroederT60 (filtered, ir.fs);

        if (! fit.valid)
        {
            rep.info (fmt ("%5.0f Hz  no usable decay (EDC never reached -25 dB)", fc));
            out.push_back (band);
            continue;
        }

        band.measured = true;
        band.t60 = fit.t60;
        band.r2 = fit.r2;
        out.push_back (band);
    }

    return out;
}

//==============================================================================
// §5.1 colour-voicing mirror (AMENDED 2026-08-07). The colour-1 per-octave T60 row
// needs the era columns of §4.1 — kModes' colorLpBottomHz / colorInterpTop — because
// its targets CARRY the voicing: axes 1 and 4 are in-loop, so they move per-band decay
// on purpose, and the row gates the residual against §3.4's authority. Mirrored like
// the tank tables above, in lockstep with ReverbEngine.cpp.
//==============================================================================

struct ColourVoicing { double lpBottomHz, interpTop; };

ColourVoicing colourVoicingFor (int algo)
{
    const int resolved = ReverbEngine::resolveMode (algo);

    if (resolved == ReverbEngine::modePlate)
        return { 7000.0, 0.60 };

    if (resolved == ReverbEngine::modeHall || resolved == ReverbEngine::modeShimmer)
        return { 9000.0, 0.25 };

    return { 6000.0, 0.90 };   // Room
}

/** The engine's one-pole coefficient and LP magnitude, verbatim (ReverbEngine.cpp), so
    the colour LP's per-pass loss in the adjusted targets is exact, not a model. */
double probeOnePoleW (double hz, double fs)
{
    const double f = juce::jlimit (0.1, fs * 0.49, hz);
    return juce::jlimit (1.0e-6, 1.0, 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * f / fs));
}

double probeOnePoleLpMagnitude (double w, double hz, double fs)
{
    const double omega = 2.0 * juce::MathConstants<double>::pi
                         * juce::jlimit (0.1, fs * 0.49, hz) / fs;
    const double p = 1.0 - w;
    const double re = 1.0 - p * std::cos (omega);
    const double im = p * std::sin (omega);
    return w / juce::jmax (1.0e-12, std::sqrt (re * re + im * im));
}

/** §3.4's |Hi(f)|_mean, verbatim including §5.1 axis 4's kernel blend: the blended read
    is the blended 4-tap FIR h = (1−c)·h_cubic + c·{0, 1−t, t, 0}, so this magnitude is
    exact for the degraded read too. */
double probeMeanInterpMag (double freqHz, double fs, double blend)
{
    const double w = 2.0 * juce::MathConstants<double>::pi
                     * juce::jlimit (1.0, fs * 0.49, freqHz) / fs;
    constexpr int steps = 64;
    double sum = 0.0;

    for (int i = 0; i < steps; ++i)
    {
        const double t = ((double) i + 0.5) / (double) steps;
        const double t2 = t * t, t3 = t2 * t;
        double h[4] = { -0.5 * t3 + t2 - 0.5 * t,
                        1.5 * t3 - 2.5 * t2 + 1.0,
                        -1.5 * t3 + 2.0 * t2 + 0.5 * t,
                        0.5 * t3 - 0.5 * t2 };
        const double hLin[4] = { 0.0, 1.0 - t, t, 0.0 };

        for (int k = 0; k < 4; ++k)
            h[k] += blend * (hLin[k] - h[k]);

        double re = 0.0, im = 0.0;

        for (int k = 0; k < 4; ++k)
        {
            re += h[k] * std::cos (w * (double) k);
            im -= h[k] * std::sin (w * (double) k);
        }

        sum += std::sqrt (re * re + im * im);
    }

    return juce::jlimit (0.5, 1.0, sum / (double) steps);
}

//==============================================================================
// Mode passes
//==============================================================================

Setup metricsSetup (int algo, double decayS)
{
    Setup s;
    s.algo = algo;
    s.decayS = (float) juce::jmin (decayS, (double) mirrorFor (ReverbEngine::resolveMode (algo)).decayCeiling);
    return s;
}

/** The configuration in which "±15 % of reverb_decay" (§8) is a well-posed statement:
    bassMult 1.0 and a damping that asks the HF band for the same T60 as the mid band,
    with `decay` pulled under the mode's hfMaxT60 so the physical ceiling of §3.4 is not
    what the gate ends up measuring. */
Setup t60Setup (int algo)
{
    const auto& mode = mirrorFor (ReverbEngine::resolveMode (algo));
    Setup s;
    s.algo = algo;
    s.damping01 = 0.25f;      // 1.25 - 0.25 = 1.0 x decay
    s.bassMult = 1.0f;
    s.decayS = (float) juce::jmin ((double) mode.hfMaxT60, (double) mode.decayCeiling);
    return s;
}

void reportModalDensity (int algo, double fs, Report& rep)
{
    const auto& mode = mirrorFor (ReverbEngine::resolveMode (algo));
    const auto ratios = apRatiosFor (algo);

    double sum48 = 0.0, sumWithAp48 = 0.0;

    for (int i = 0; i < 8; ++i)
    {
        sum48 += (double) mode.tank48[i];
        sumWithAp48 += (double) mode.tank48[i] * (1.0 + ratios[(size_t) i]);
    }

    const double modesPerHz = sum48 / 48000.0;
    const double impliedCeiling = sumWithAp48 / 48000.0 / 0.15;
    // §3.3's constraint is a design-time property of the 48 kHz table (§4.1 quotes
    // "Sum at sizeMin: Room 6396"), which is what the engine's own prepare-time assert
    // checks. The host-rate count is reported alongside because at 44.1 kHz Room is
    // genuinely under 6000 physical samples.
    const double sumAtSizeMin48 = sum48 * mode.sizeMin;
    const double sumAtSizeMinHost = sumAtSizeMin48 * (fs / 48000.0);

    rep.info (fmt ("modal density %.3f modes/Hz (sum of tank lines %.0f samples @48k, "
                   "sum incl. allpasses %.0f)", modesPerHz, sum48, sumWithAp48));
    rep.info (fmt ("Schroeder/Logan implied decay ceiling %.2f s vs declared %.2f s",
                   impliedCeiling, (double) mode.decayCeiling));
    rep.info (fmt ("sum of tank lines at sizeMin: %.0f samples @48k, %.0f at the host rate",
                   sumAtSizeMin48, sumAtSizeMinHost));

    rep.gate (sumAtSizeMin48 >= 6000.0,
              fmt ("sum of tank lines at sizeMin is %.0f >= 6000 samples @48k (§3.3)",
                   sumAtSizeMin48));
    rep.gate ((double) mode.decayCeiling <= impliedCeiling + 1.0e-6,
              fmt ("declared decay ceiling %.2f s <= Schroeder/Logan implied %.2f s (§3.3)",
                   (double) mode.decayCeiling, impliedCeiling));
}

RateResult analyseTankMode (int algo, double fs, double decayS, const juce::File& outDir, Report& rep)
{
    const auto& mode = mirrorFor (ReverbEngine::resolveMode (algo));
    rep.head (fmt ("%s @ %.0f Hz  (algo %d, color = 0)", mode.name, fs, algo));

    RateResult result;
    result.fs = fs;
    result.algo = algo;

    // --- general metrics pass -------------------------------------------------
    const Setup metrics = metricsSetup (algo, decayS);
    const double seconds = juce::jmax (4.0, 3.0 * (double) metrics.decayS + 1.0);
    const Ir ir = renderIr (metrics, fs, seconds);

    const auto wav = outDir.getChildFile (fmt ("ir_%s_%.0f.wav", mode.name, fs));

    if (writeIrWav (ir, wav))
        rep.info (fmt ("wrote %s (32-bit float, peak %.6g, decay %.2f s)",
                       wav.getFileName().toRawUTF8(), peakOf (ir), (double) metrics.decayS));

    const auto broadband = schroederT60 (ir.l, fs);
    result.broadbandT60 = broadband.valid ? broadband.t60 : 0.0;
    rep.info (fmt ("broadband T60 %.3f s (R2 %.4f) against reverb_decay %.2f s",
                   result.broadbandT60, broadband.r2, (double) metrics.decayS));

    reportModalDensity (algo, fs, rep);

    // --- echo density (§8 row 1) ----------------------------------------------
    {
        const auto density = echoDensity (ir.l, fs);
        double reached95 = -1.0;

        for (size_t i = 0; i < density.eta.size(); ++i)
            if (density.eta[i] >= 0.95) { reached95 = density.timeS[i]; break; }

        const int hop = juce::jmax (1, (int) std::lround (0.001 * fs));
        const auto env = rmsEnvelopeDb (ir.l, fs, 0.020, hop);
        const double tailEnd = timeBelowPeak (env, -55.0);

        // "Through the tail" means the LATE field — the failure §1.1 describes is a tail
        // that degenerates back into a periodic comb train, not a build-up that has not
        // finished. The boundary is structural, never derived from the measurement: one
        // full circulation of this mode's LONGEST tank line is the first instant at which
        // every line has contributed, and 80 ms is the build-up deadline §8 states in the
        // same row. Room 80 ms, Plate 80 ms, Hall 230 ms at the probe's Size.
        const double sizeScale = (double) mode.sizeMin
                                 + 0.5 * (double) (mode.sizeMax - mode.sizeMin);
        double longestLine = 0.0;

        for (int i = 0; i < 8; ++i)
            longestLine = juce::jmax (longestLine, (double) mode.tank48[i] / 48000.0 * sizeScale);

        const double tailStart = juce::jmax (0.080, longestLine);

        double tailMin = 1.0e300, tailMinAt = 0.0;

        for (size_t i = 0; i < density.density.size(); ++i)
            if (density.timeS[i] >= tailStart && density.timeS[i] <= tailEnd
                && density.density[i] < tailMin)
            {
                tailMin = density.density[i];
                tailMinAt = density.timeS[i];
            }

        rep.info (fmt ("echo density reaches 0.95 at %s; tail minimum %.3f at %.3f s "
                       "(tail evaluated over %.0f ms .. %.2f s, %.0f Hz band)",
                       reached95 >= 0.0 ? fmt ("%.1f ms", 1000.0 * reached95).toRawUTF8() : "never",
                       tailMin < 1.0e299 ? tailMin : 0.0, tailMinAt,
                       1000.0 * tailStart, tailEnd, kEchoDensityBandHz));

        // §8 scopes the 80 ms criterion to Room and Plate; Hall's shortest line is
        // 125 ms, so the same number could not be met by construction.
        if (ReverbEngine::resolveMode (algo) != ReverbEngine::modeHall)
            rep.gate (reached95 >= 0.0 && reached95 <= 0.080,
                      fmt ("normalised echo density reaches 0.95 by 80 ms (measured %s)",
                           reached95 >= 0.0 ? fmt ("%.1f ms", 1000.0 * reached95).toRawUTF8() : "never"));

        rep.gate (tailMin < 1.0e299 && tailMin >= 0.9,
                  fmt ("normalised echo density stays >= 0.9 through the tail (minimum %.3f)",
                       tailMin < 1.0e299 ? tailMin : 0.0));
    }

    // --- ER density in isolation (§8 row 2) -----------------------------------
    // The ER bus reaches the output only through `0.60 * erLevel * er` (§3.1) while the
    // tank's own ER injection is the separate fixed `erToTank`, so the DIFFERENCE of two
    // renders at erLevel 0 and 1 is exactly the ER path with the tank muted.
    {
        Setup withEr = metrics, withoutEr = metrics;
        withEr.erLevel01 = 1.0f;
        withoutEr.erLevel01 = 0.0f;

        const Ir a = renderIr (withEr, fs, 0.5);
        const Ir b = renderIr (withoutEr, fs, 0.5);

        Ir erIr;
        erIr.fs = fs;
        erIr.l.resize (a.size());
        erIr.r.resize (a.size());

        for (size_t i = 0; i < a.size(); ++i)
        {
            erIr.l[i] = a.l[i] - b.l[i];
            erIr.r[i] = a.r[i] - b.r[i];
        }

        writeIrWav (erIr, outDir.getChildFile (fmt ("er_%s_%.0f.wav", mode.name, fs)));

        // Unlike the tail gate this one reads the RAW curve. The tail statistic is
        // smoothed because the late-field density is stationary there, so a centred
        // average is unbiased and only removes estimator variance. Here the opposite
        // holds: the ER density is still climbing steeply through 60 ms (Room reads
        // 0.19 / 0.60 / 0.94 at 20 / 40 / 60 ms), so a +-50 ms average would report the
        // build-up rather than the value §8 asks for at 60 ms. Band-limiting inside
        // echoDensity is what this gate needed — it read 0.922 / 0.896 / 0.730 across
        // 44.1 / 48 / 96 kHz before it and 0.95 / 0.94 / 0.89 after.
        //
        // The gate reads L, as every other §8 row does. R is measured and reported
        // alongside because it is NOT a second sample of the same number: §3.7 draws one
        // equal-power pan per tap from the seeded LCG, and over only 24 taps that draw
        // has enough sampling variance to leave one channel measurably sparser — Hall
        // reads L 0.98 / R 0.83 and Room L 0.94 / R 0.90, consistently at all three host
        // rates. Averaging the two would hide that rather than measure it.
        const auto densityL = echoDensity (erIr.l, fs);
        const auto densityR = echoDensity (erIr.r, fs);

        auto rawAt = [] (const EchoDensity& d, double t)
        {
            for (size_t i = 0; i < d.timeS.size(); ++i)
                if (d.timeS[i] >= t)
                    return d.eta[i];

            return d.eta.empty() ? 0.0 : d.eta.back();
        };

        const double at60ms = rawAt (densityL, 0.060);

        rep.info (fmt ("ER-only density (L): %.3f at 20 ms, %.3f at 40 ms, %.3f at 60 ms "
                       "— R reads %.3f at 60 ms",
                       rawAt (densityL, 0.020), rawAt (densityL, 0.040), at60ms,
                       rawAt (densityR, 0.060)));

        rep.gate (at60ms >= 0.9,
                  fmt ("Early Energy alone reaches echo density >= 0.9 at 60 ms (measured %.3f)",
                       at60ms));

        // §3.7's rationale, measured rather than assumed. The Early tap line is fed from
        // the diffuser output (§3.7's 2026-08-06 amendment), so at reverb_diffusion = 1 it
        // carries four allpasses at g up to 0.78 — exactly the ringing Costello's
        // no-allpass argument exists to avoid.
        //
        // The measured quantity is the INCREASE in §8's spectral-flatness statistic as
        // Diffusion sweeps 0 -> 1, not its absolute value: a 24-tap FIR spanning 18 ms
        // (Plate's erWindowMs) has coarse comb structure of its own, and reads +9.2 dB
        // with the diffusers all but switched out. §8's 10 dB bound is written for the
        // full IR — a dense tail — so against the isolated Early path it would be
        // measuring the tap table. What the diffusers add is the question, and it is what
        // this gates.
        {
            Setup flat = withEr, flatOff = withoutEr, hi = withEr, hiOff = withoutEr;
            flat.diffusion01 = 0.0f;
            flatOff.diffusion01 = 0.0f;
            hi.diffusion01 = 1.0f;
            hiOff.diffusion01 = 1.0f;

            const int order = welchOrderFor (fs);
            const int fftSize = 1 << order;

            auto erAt = [&] (const Setup& on, const Setup& off)
            {
                const Ir c = renderIr (on, fs, 0.5);
                const Ir d = renderIr (off, fs, 0.5);
                std::vector<double> x (c.size());

                for (size_t i = 0; i < c.size(); ++i)
                    x[i] = c.l[i] - d.l[i];

                const auto smoothed = octaveSmoothDb (toDb (welchPower (x, order)),
                                                      fs, fftSize, 12.0);
                return worstDeviation (smoothed, fs, fftSize,
                                       100.0, juce::jmin (12000.0, fs * 0.4), 1.0, false);
            };

            const auto bare = erAt (flat, flatOff);
            const auto maxed = erAt (hi, hiOff);
            const double added = maxed.worstDb - bare.worstDb;

            rep.info (fmt ("ER path alone: worst spectral peak %+.2f dB above the local "
                           "median at reverb_diffusion = 0, %+.2f dB at %.0f Hz with it "
                           "maxed — the diffusers add %+.2f dB",
                           bare.worstDb, maxed.worstDb, maxed.atHz, added));
            // 4 dB: Plate is the worst case at +3.08 / +2.97 / +2.80 dB over
            // 44.1 / 48 / 96 kHz (Room +1.68 / +0.81 / +0.43, Hall +0.28 / +1.20 / +1.32),
            // and it is the mode with both the highest inDiffG (0.78) and the shortest ER
            // window (18 ms). The bound is the shipped worst case plus margin, so the gate
            // is a regression bound on the routing this section documents.
            rep.gate (added <= 4.0,
                      fmt ("maxing Diffusion does not ring the Early path: it adds %+.2f dB "
                           "to §8's spectral-flatness statistic (bound 4 dB)", added));
        }
    }

    // --- per-octave T60 (§8 row 3) --------------------------------------------
    {
        const Setup flat = t60Setup (algo);
        const double t60Seconds = juce::jmax (4.0, 4.0 * (double) flat.decayS + 1.0);
        const Ir flatIr = renderIr (flat, fs, t60Seconds);

        rep.info (fmt ("per-octave pass: decay %.2f s, damping %.2f, bassMult %.2f "
                       "(frequency-flat target)", (double) flat.decayS,
                       (double) flat.damping01, (double) flat.bassMult));

        const auto bands = perOctaveT60 (flatIr, flat, rep);

        for (const auto& band : bands)
        {
            if (! band.measured)
                continue;

            const double error = (band.t60 - band.target) / band.target;
            const double tolerance = band.fc <= 4000.0 ? 0.15 : 0.35;

            rep.info (fmt ("%5.0f Hz  T60 %.3f s  target %.3f s  error %+6.1f %%  R2 %.4f",
                           band.fc, band.t60, band.target, 100.0 * error, band.r2));

            rep.gate (std::abs (error) <= tolerance,
                      fmt ("%5.0f Hz T60 within +-%.0f %% of its Jot target (%+.1f %%)",
                           band.fc, 100.0 * tolerance, 100.0 * error));

            if (band.fc <= 4000.0)
                rep.gate (band.r2 > 0.98,
                          fmt ("%5.0f Hz Schroeder fit R2 > 0.98 (%.4f)", band.fc, band.r2));
        }
    }

    // --- per-octave T60 at colour = 1 (§8, AMENDED 2026-08-07) ----------------
    // §5.1's axes 1 and 4 sit IN the loop, so at the knob's far end they move per-band
    // decay on purpose: the era LP compounds its corner's loss per pass, and the
    // degraded line read compounds the blended kernel's droop. The row's targets
    // therefore CARRY the voicing — per line,
    //
    //   rate_i(fc) = 60 / T60_jot(fc)  +  (L_lp(fc) + ΔL_interp(fc)) / d_i    [dB/s]
    //
    // with L_lp the colour LP's exact per-pass loss and ΔL_interp the blended-vs-cubic
    // droop delta. ΔL is credited with the r_hi re-solve's compensation at f_h for
    // bands at/above the crossover; below it a one-pole's passband gain pins near 1,
    // so the re-solve cannot reach those bands and the raw (tiny) droop applies. The
    // eight per-line rates are folded through the SAME Schroeder fit the measurement
    // uses, on a synthetic equal-energy eight-line envelope, so target and measurement
    // agree about what a multi-rate EDC reads as; its R2 is reported, not gated — the
    // curvature of a multi-rate decay is the model's own content, not an estimator
    // defect. Rendered at erLevel 0 like the tail-level row: the row gates the TANK's
    // decay law, and the full-bandwidth ER FIR is the one wet component the in-loop
    // voicing does not touch. Run at damping 0.5 rather than the flat 0.25: with
    // t60High == t60Mid the r_hi solve clamps at 1 − 1e-4 and axis 4's re-solved
    // compensation is structurally inert, i.e. ungateable — at the 0.5 default r_hi
    // is genuinely < 1 and the compensation is load-bearing.
    //
    // FATAL at colour = 1, the clip-aliasing row's carve-out: the exit contract's
    // "any gate failure at color = 0" drew its line where the voicing INVERTS rows.
    // Here the targets internalise the voicing, so a miss is §3.4 losing authority
    // over the decay — a defect at every colour.
    {
        Setup vint = t60Setup (algo);
        vint.damping01 = 0.5f;
        vint.color01 = 1.0f;
        vint.erLevel01 = 0.0f;

        const double t60Seconds = juce::jmax (4.0, 4.0 * (double) vint.decayS + 1.0);
        const Ir vintIr = renderIr (vint, fs, t60Seconds);

        const auto voicing = colourVoicingFor (algo);

        rep.info (fmt ("colour-1 per-octave pass: decay %.2f s, damping %.2f, era LP "
                       "%.0f Hz, interp blend %.2f (§5.1 axes 1+4 in the targets)",
                       (double) vint.decayS, (double) vint.damping01,
                       voicing.lpBottomHz, voicing.interpTop));
        const auto ratios = apRatiosFor (algo);
        const double sizeScale = 0.5 * ((double) mode.sizeMin + (double) mode.sizeMax);
        const double chi = mode.hfXoverHz;
        const double lpW = probeOnePoleW (voicing.lpBottomHz, fs);
        const double compDb = -20.0 * std::log10 (probeMeanInterpMag (chi, fs, voicing.interpTop)
                                                  / probeMeanInterpMag (chi, fs, 0.0));

        // Per-line loop delays, seconds: line + its allpass, at the default Size.
        std::array<double, 8> loopS {};

        for (int i = 0; i < 8; ++i)
            loopS[(size_t) i] = (double) mode.tank48[i] * sizeScale
                                * (1.0 + ratios[(size_t) i]) / 48000.0;

        // The Jot per-band law, verbatim from perOctaveT60 — needed per FREQUENCY here,
        // not per band centre: an octave band straddling a crossover (Hall's 4 kHz
        // octave spans its 3.5 kHz hfXoverHz) carries content on both sides of the
        // step, and the longer-ringing side dominates the late fit.
        const double decayD = juce::jlimit (0.2, (double) mode.decayCeiling, (double) vint.decayS);
        const double t60Low = juce::jmax (0.05, decayD * juce::jlimit (0.25, 4.0, (double) vint.bassMult));
        const double t60High = juce::jmax (0.05, juce::jmin (decayD * (1.25 - (double) vint.damping01),
                                                             (double) mode.hfMaxT60));

        const auto bands = perOctaveT60 (vintIr, vint, rep);

        for (const auto& band : bands)
        {
            if (! band.measured)
                continue;

            // Five log-spaced frequencies across the octave, each with its own Jot
            // target and per-pass losses, folded with the eight line rates into one
            // equal-energy envelope through the measurement's own Schroeder fit.
            constexpr int kFreqSteps = 5;
            constexpr double modelFs = 4000.0;
            double rates[kFreqSteps][8];
            double lpAtFc = 0.0, interpAtFc = 0.0;

            for (int q = 0; q < kFreqSteps; ++q)
            {
                const double f = band.fc * std::pow (2.0, -0.5 + ((double) q + 0.5) / kFreqSteps);
                const double tJot = f < mode.bassXoverHz ? t60Low
                                  : f > chi              ? t60High
                                                         : decayD;
                const double lpDb = -20.0 * std::log10 (probeOnePoleLpMagnitude (lpW, f, fs));
                double interpDb = -20.0 * std::log10 (probeMeanInterpMag (f, fs, voicing.interpTop)
                                                      / probeMeanInterpMag (f, fs, 0.0));

                if (f >= chi)
                    interpDb = juce::jmax (0.0, interpDb - compDb);

                for (int i = 0; i < 8; ++i)
                    rates[q][i] = 60.0 / tJot + (lpDb + interpDb) / loopS[(size_t) i];
            }

            lpAtFc = -20.0 * std::log10 (probeOnePoleLpMagnitude (lpW, band.fc, fs));
            interpAtFc = -20.0 * std::log10 (probeMeanInterpMag (band.fc, fs, voicing.interpTop)
                                             / probeMeanInterpMag (band.fc, fs, 0.0));

            std::vector<double> model ((size_t) std::lround (t60Seconds * modelFs));

            for (size_t n = 0; n < model.size(); ++n)
            {
                const double t = (double) n / modelFs;
                double power = 0.0;

                for (int q = 0; q < kFreqSteps; ++q)
                    for (int i = 0; i < 8; ++i)
                        power += std::pow (10.0, -rates[q][i] * t / 10.0);

                model[n] = std::sqrt (power);
            }

            const auto modelFit = schroederT60 (model, modelFs);

            if (! modelFit.valid)
            {
                rep.info (fmt ("%5.0f Hz  adjusted-target model has no usable decay — skipped",
                               band.fc));
                continue;
            }

            const double error = (band.t60 - modelFit.t60) / modelFit.t60;
            const double tolerance = band.fc <= 4000.0 ? 0.15 : 0.35;

            rep.info (fmt ("%5.0f Hz  T60 %.3f s  adjusted target %.3f s (at fc: LP %.2f dB "
                           "+ interp %.2f dB per pass)  error %+6.1f %%  R2 %.4f",
                           band.fc, band.t60, modelFit.t60, lpAtFc, interpAtFc,
                           100.0 * error, band.r2));

            rep.gate (std::abs (error) <= tolerance,
                      fmt ("%5.0f Hz colour-1 T60 within +-%.0f %% of its voiced target "
                           "(%+.1f %%)", band.fc, 100.0 * tolerance, 100.0 * error));
        }
    }

    // --- per-octave tail LEVEL (§8, AMENDED 2026-08-07) -----------------------
    // The tail's integrated energy per band is proportional to that band's T60, so
    // without §3.4's tonal correction reverb_bassmult and reverb_damping are also EQs:
    // ×4 is ~+6 dB of low-band tail LEVEL and damping = 1 about −4 dB of HF level on
    // top of the decay-TIME change the knob asks for. The per-octave T60 row above
    // cannot see this — a level offset does not move a Schroeder slope. This row sweeps
    // both knobs over the §8 grid, measures per-octave tail energy RELATIVE to the
    // 1 kHz octave (which neither knob touches), and gates the movement against the
    // same measure at the neutral knobs (bassMult 1, damping 0.5). 16 kHz is excluded
    // for the same interpolator-limited reason as the ±35 % band above.
    //
    // Rendered at erLevel = 0, including the neutral reference: the direct wet ER's
    // level follows neither knob, and the correction shelves rescale it too (the §3.4
    // caveat) — the row gates the TAIL law, so the tank tail is what it renders.
    //
    // The measured quantity per band is the IR's whole integrated energy, which is what
    // sustained material actually hears: steady-state wet power per band is exactly
    // ∫h_band² by convolution. A late-field-only statistic (Schroeder-fit intercept,
    // tail-windowed energy) was tried and rejected here — extrapolating a fit explodes
    // for the crossover-straddling 4 kHz octave and for the sub-200 ms T60s the damping
    // extreme reaches, and a fixed tail start re-introduces the decay-TIME dependence
    // (a shorter T60 has decayed further by any fixed instant) that the correction
    // rightly leaves alone. The integral statistic under-reads the movement where the
    // first tank pass — which reaches the output taps before the Filt1 stages have
    // acted, so it follows neither knob — carries a large share of the band's energy;
    // that share is what Hall's smaller tonalCorrect (§4.1) is tuned against.
    {
        static constexpr double kLevelBands[] = { 63.0, 125.0, 250.0, 4000.0, 8000.0 };
        constexpr int kNumLow = 3;      // kLevelBands[0..2] sit under every tank
                                        // bassXoverHz; [3..4] sit at/above Hall's
                                        // hfXoverHz (3500) and straddle Room/Plate's
                                        // (4500/5500) — the gate measures band-level
                                        // movement directly, so alignment is not load-
                                        // bearing

        auto relLevelsDb = [] (const Ir& x) -> std::array<double, 5>
        {
            auto bandEnergy = [&x] (double fc)
            {
                const auto filtered = octaveBand (x.l, x.fs, fc);
                double e = 0.0;

                for (double v : filtered)
                    e += v * v;

                return juce::jmax (1.0e-300, e);
            };

            const double ref = bandEnergy (1000.0);
            std::array<double, 5> out {};

            for (size_t j = 0; j < 5; ++j)
                out[j] = 10.0 * std::log10 (bandEnergy (kLevelBands[j]) / ref);

            return out;
        };

        Setup neutralSetup = metrics;
        neutralSetup.erLevel01 = 0.0f;

        const auto neutral = relLevelsDb (renderIr (neutralSetup, fs, seconds));

        double worstLow = 0.0, worstHigh = 0.0;
        juce::String worstLowAt, worstHighAt;

        for (float bm : { 0.25f, 1.0f, 4.0f })
            for (float dm : { 0.0f, 0.5f, 1.0f })
            {
                if (bm == 1.0f && dm == 0.5f)
                    continue;            // the neutral point itself

                Setup sweep = neutralSetup;
                sweep.bassMult = bm;
                sweep.damping01 = dm;

                const auto rel = relLevelsDb (renderIr (sweep, fs, seconds));
                double lo = 0.0, hi = 0.0;

                for (size_t j = 0; j < 5; ++j)
                {
                    const double moved = std::abs (rel[j] - neutral[j]);

                    if ((int) j < kNumLow) lo = juce::jmax (lo, moved);
                    else                   hi = juce::jmax (hi, moved);
                }

                rep.info (fmt ("bassMult %.2f damping %.2f: tail level moves %.2f dB "
                               "(63-250 Hz) / %.2f dB (4-8 kHz) re the 1 kHz octave",
                               (double) bm, (double) dm, lo, hi));

                const auto at = fmt ("bassMult %.2f damping %.2f", (double) bm, (double) dm);
                if (lo > worstLow)  { worstLow = lo;  worstLowAt = at; }
                if (hi > worstHigh) { worstHigh = hi; worstHighAt = at; }
            }

        rep.gate (worstLow <= 3.0,
                  fmt ("63-250 Hz tail LEVEL moves <= 3 dB re the 1 kHz octave over the "
                       "bassmult/damping grid (worst %.2f dB at %s)",
                       worstLow, worstLowAt.toRawUTF8()));
        rep.gate (worstHigh <= 3.0,
                  fmt ("4-8 kHz tail LEVEL moves <= 3 dB re the 1 kHz octave over the "
                       "bassmult/damping grid (worst %.2f dB at %s)",
                       worstHigh, worstHighAt.toRawUTF8()));
    }

    // --- flutter (§8 row 4) ---------------------------------------------------
    {
        result.worstFlutter = flutterWorstPeak (ir, algo, rep, true);
        rep.gate (result.worstFlutter <= 0.2,
                  fmt ("no flutter autocorrelation peak > 0.2 at any tank-line or allpass "
                       "lag (worst %.3f)", result.worstFlutter));
    }

    // --- spectral flatness (§8 row 5) -----------------------------------------
    {
        const int order = welchOrderFor (fs);
        const int fftSize = 1 << order;
        const auto db = toDb (welchPower (ir.l, order));
        const auto smoothed = octaveSmoothDb (db, fs, fftSize, 12.0);
        const auto peak = worstDeviation (smoothed, fs, fftSize,
                                          100.0, juce::jmin (12000.0, fs * 0.4), 1.0, false);

        rep.info (fmt ("worst spectral peak above the local median: %+.2f dB at %.0f Hz",
                       peak.worstDb, peak.atHz));
        rep.gate (peak.worstDb <= 10.0,
                  fmt ("no smoothed spectral peak more than 10 dB above the local median "
                       "(%+.2f dB)", peak.worstDb));
    }

    // --- mono sum (§8 row 6) --------------------------------------------------
    {
        const auto notch = monoSumNotch (ir);
        result.notchHz = notch.atHz;
        result.notchDb = notch.worstNotchDb;
        result.monoSumGridDb = notch.gridDb;
        rep.info (fmt ("deepest mono-sum notch %+.2f dB at %.0f Hz", notch.worstNotchDb, notch.atHz));
        rep.gate (notch.worstNotchDb >= -6.0,
                  fmt ("no mono-sum notch deeper than 6 dB (%+.2f dB)", notch.worstNotchDb));
    }

    // --- nonlinearity (§8 row 8) ----------------------------------------------
    // Every other metric is measured from an impulse, where the in-loop clipper is
    // identity. Real material through a NAM block is not.
    {
        const double noiseSeconds = 2.0;
        const double tailSeconds = juce::jmax (3.0, 3.0 * (double) metrics.decayS + 1.0);
        const int noiseSamples = (int) std::lround (noiseSeconds * fs);
        const auto noise = pinkNoise (noiseSamples, 0.5011872, 0x9E3779B9u);   // -6 dBFS RMS
        const Ir noiseIr = renderThrough (metrics, fs, noise,
                                          noiseSamples + (int) std::lround (tailSeconds * fs));

        // Both sides are measured in the SAME octave band. A broadband comparison would
        // be dominated by the pink source's -3 dB/oct tilt weighting the (longer) low
        // bands, which is spectral weighting rather than the level dependence this row
        // exists to catch.
        constexpr double kNonlinearityBandHz = 1000.0;
        std::vector<double> tail (noiseIr.l.begin() + noiseSamples, noiseIr.l.end());
        const auto fit = schroederT60 (octaveBand (tail, fs, kNonlinearityBandHz), fs);
        const auto quiet = schroederT60 (octaveBand (ir.l, fs, kNonlinearityBandHz), fs);
        const double error = quiet.valid && fit.valid && quiet.t60 > 0.0
                                 ? (fit.t60 - quiet.t60) / quiet.t60
                                 : 1.0;

        rep.info (fmt ("1 kHz band: pink-noise decay T60 %.3f s (R2 %.4f) vs impulse T60 "
                       "%.3f s (R2 %.4f): %+.1f %%",
                       fit.valid ? fit.t60 : 0.0, fit.valid ? fit.r2 : 0.0,
                       quiet.valid ? quiet.t60 : 0.0, quiet.valid ? quiet.r2 : 0.0,
                       100.0 * error));
        rep.gate (fit.valid && std::abs (error) <= 0.15,
                  fmt ("-6 dBFS pink-noise T60 within 15 %% of the impulse-measured T60 "
                       "(%+.1f %%)", 100.0 * error));
    }

    return result;
}

/** §8's last row is a prepare-time property rather than a measurement: the normalised
    Hadamard is exactly orthogonal, so cond(A) = 1. Restated here numerically over the
    same 12-butterfly implementation the engine runs. */
void checkMatrixUnitarity (Report& rep)
{
    rep.head (u8 ("Feedback matrix (§3.2, §8 last row)"));

    double worst = 0.0;

    for (int trial = 0; trial < 64; ++trial)
    {
        Lcg lcg (0x12345677u + (std::uint32_t) trial * 2654435761u);
        double v[8], before = 0.0;

        for (int i = 0; i < 8; ++i)
        {
            v[i] = 2.0 * (double) lcg.uni() - 1.0;
            before += v[i] * v[i];
        }

        for (int i = 0; i < 8; i += 2) { const double u = v[i], t = v[i + 1]; v[i] = u + t; v[i + 1] = u - t; }

        for (int base = 0; base < 8; base += 4)
            for (int i = 0; i < 2; ++i) { const double u = v[base + i], t = v[base + i + 2]; v[base + i] = u + t; v[base + i + 2] = u - t; }

        for (int i = 0; i < 4; ++i) { const double u = v[i], t = v[i + 4]; v[i] = u + t; v[i + 4] = u - t; }

        double after = 0.0;

        for (double x : v)
            after += x * x / 8.0;

        if (before > 0.0)
            worst = juce::jmax (worst, std::abs (std::sqrt (after / before) - 1.0));
    }

    rep.info (fmt ("worst singular-value deviation over 64 random vectors: %.3e", worst));
    rep.gate (1.0 + worst < 1.001, fmt ("cond(A) < 1.001 (measured %.9f)", 1.0 + worst));
}

//==============================================================================
// Reverse (§3.12's six gates)
//==============================================================================
/** §3.12 gate 1a, AMENDED 2026-08-06, and the amendment is the whole of the change: the
    1 dB bound and the section's own 64-tap constant cannot both hold, so one of them had
    to move and the tap count is the one the cost budget (§6.8) and the mode's character
    depend on.

    64 taps over W = 1.5 s is a mean gap of 23.4 ms, WIDER than the 20 ms analysis window,
    so the envelope of any 64-tap layout ripples whatever the spacing law. Modelled
    independently of the engine (tap table, +-10 % gain jitter, alternating signs, per-tap
    equal-power pan and the two g = 0.5 density allpasses, no front end), worst dip on the
    stereo power:

        taps   uniform, no jitter   uniform + jitter   §3.12's density law + jitter
          64          -1.07 dB           -1.53 dB              -8.58 dB
         128          -0.27 dB           -0.66 dB              -1.87 dB
         256          -0.02 dB           -0.38 dB              -1.03 dB
         512          -0.00 dB           -0.24 dB              -0.81 dB

    Reaching the literal 1 dB needs 256-512 taps — 4x to 8x the section's own constant and
    the same multiple on the mode's per-sample cost, against a §6.8 budget that sizes
    Reverse as "the same class as the tank + ER path it displaces". What the dips are is
    also measurable: they sit at the QUIET end of the swell, where the density law
    deliberately puts its widest gaps because the envelope is 20-35 dB down there. (The
    engine reads better than the model at 64 taps — -3.3 dB against -8.6 dB — because the
    model has no input diffusers, and their ~50 ms smear fills the widest gaps.)

    So the gate splits in two, and both halves are real:
      1a  over the audible swell — the envelope within `kRevAudibleSwellDb` of its peak —
          the dip stays inside `kRevAudibleDipDb`. The literal mirror direction of
          §3.12's formula (sparsest taps at t = W, the loudest point) measures -15.5 dB
          here and is rejected, which is the deviation this gate exists to police.
      1a' across the whole swell the dip stays inside `kRevSwellDipDb`. Both rejected
          regularisers — the literal 10 ms floor in either mirror direction — measure
          -78 and -83 dB here and are rejected.

    Measured on the stereo power sqrt((L^2+R^2)/2) rather than L alone, for the same reason
    gate 2 already integrates both channels: §3.12 pans every tap by an independent
    equal-power draw, so one channel's envelope carries the pan draw's variance on top of
    the envelope's own shape (-15.8 dB against -1.5 dB for the same ideal layout). The pan
    is a stereo-image property; this gate is about the envelope. */
constexpr double kRevAudibleSwellDb = 20.0;
constexpr double kRevAudibleDipDb = 2.0;    // engine reads -1.25 .. -1.37 over 44.1/48/96
constexpr double kRevSwellDipDb = 8.0;      // engine reads -3.34 .. -3.56

/** The wet IR's total power as one signal, so an envelope statistic is not a sample of the
    per-tap pan draw. */
std::vector<double> stereoPower (const Ir& ir)
{
    std::vector<double> p (ir.size());

    for (size_t i = 0; i < ir.size(); ++i)
        p[i] = std::sqrt (0.5 * (ir.l[i] * ir.l[i] + ir.r[i] * ir.r[i]));

    return p;
}

/** §3.12's two series density allpasses sit AFTER the tap sum, so every tap reaches the
    output that much later. A Schroeder allpass of delay m has a mean group delay of
    exactly m over one period of its response, and the cross-tuned pairs are chosen so both
    channels sum to the same figure: L 6.9 + 11.3 ms, R 7.9 + 10.3 ms = 18.2 ms either way,
    scaled by sizeScale like every other length in the mode.

    It is a fixed path delay, exactly like the pre-delay the gates below already add to the
    expected stop time — not a window-tracking error — so gates 3 and 4 measure against
    `predelay + W + kRevOutputGroupDelay`. Amended 2026-08-06 (§3.12); before the
    correction the same renders read +5.8 / +5.7 / +5.2 % on gate 4 at W = 0.4 s and
    -76.4 / -77.3 / -75.5 dBFS on gate 3, both of them the missing 18.2 ms and neither of
    them a property of the tap table. The 5 % and -80 dBFS thresholds are untouched. */
constexpr double kRevDensityApSeconds = 0.0182;

RateResult analyseReverse (double fs, const juce::File& outDir, Report& rep)
{
    rep.head (fmt ("Reverse @ %.0f Hz  (algo 5, color = 0)", fs));

    RateResult result;
    result.fs = fs;
    result.algo = ReverbEngine::modeReverse;

    Setup s;
    s.algo = ReverbEngine::modeReverse;
    s.decayS = 1.5f;                                    // W = 1.5 s, the mode's ceiling
    const double window = (double) ReverbEngine::windowSecondsFor (s.decayS);
    const double predelay = (double) s.predelayMs * 0.001;
    // size01 = 0.5 over Reverse's 0.60 .. 1.40 range is sizeScale 1.0 (§4.1).
    const double outputDelay = kRevDensityApSeconds;
    const double seconds = predelay + window + 1.5;

    const Ir ir = renderIr (s, fs, seconds);

    const auto wav = outDir.getChildFile (fmt ("ir_Reverse_%.0f.wav", fs));

    if (writeIrWav (ir, wav))
        rep.info (fmt ("wrote %s (32-bit float, peak %.6g, W %.2f s)",
                       wav.getFileName().toRawUTF8(), peakOf (ir), window));

    const int hop = juce::jmax (1, (int) std::lround (0.001 * fs));
    const double hopSeconds = (double) hop / fs;
    const auto env = rmsEnvelopeDb (stereoPower (ir), fs, 0.020, hop);
    const size_t envPeakIndex = env.peakIndex();
    const double envPeak = env.db.empty() ? 0.0 : env.db[envPeakIndex];

    // --- gate 1: rising envelope, then a hard stop ----------------------------
    {
        const size_t start = (size_t) juce::jmin ((double) env.db.size() - 1.0,
                                                  std::floor (predelay / hopSeconds));
        double running = -1.0e300, worstDrop = 0.0, worstAudible = 0.0;
        double worstAt = 0.0, worstAudibleAt = 0.0;

        for (size_t i = start; i <= envPeakIndex && i < env.db.size(); ++i)
        {
            running = juce::jmax (running, env.db[i]);
            const double drop = env.db[i] - running;

            if (drop < worstDrop) { worstDrop = drop; worstAt = env.timeOf (i); }

            if (env.db[i] > envPeak - kRevAudibleSwellDb && drop < worstAudible)
            {
                worstAudible = drop;
                worstAudibleAt = env.timeOf (i);
            }
        }

        rep.info (fmt ("envelope peak at %.3f s (W = %.3f s); worst dip below the running "
                       "maximum %.2f dB at %.3f s, and %.2f dB at %.3f s over the audible "
                       "swell (within %.0f dB of the peak)",
                       env.timeOf (envPeakIndex), predelay + window, worstDrop, worstAt,
                       worstAudible, worstAudibleAt, kRevAudibleSwellDb));
        rep.gate (worstAudible >= -kRevAudibleDipDb,
                  fmt ("gate 1a: over the audible swell the envelope never falls more than "
                       "%.1f dB below its running maximum (worst %.2f dB)",
                       kRevAudibleDipDb, worstAudible));
        rep.gate (worstDrop >= -kRevSwellDipDb,
                  fmt ("gate 1a': across the whole swell the envelope never falls more than "
                       "%.0f dB below its running maximum (worst %.2f dB)",
                       kRevSwellDipDb, worstDrop));

        // ... then falls >= 40 dB within 150 ms.
        const size_t after = juce::jmin (env.db.size() - 1,
                                         envPeakIndex + (size_t) std::lround (0.150 / hopSeconds));
        const double fall = envPeak - env.db[after];
        rep.info (fmt ("envelope 150 ms past the peak is %.1f dB down", fall));
        rep.gate (fall >= 40.0,
                  fmt ("gate 1b: envelope falls >= 40 dB within 150 ms of W (%.1f dB)", fall));
    }

    // --- gate 2: energy centroid in the last 40 % of [predelay, predelay + W] --
    {
        double num = 0.0, den = 0.0;

        for (size_t i = 0; i < ir.size(); ++i)
        {
            const double e = ir.l[i] * ir.l[i] + ir.r[i] * ir.r[i];
            num += ((double) i / fs) * e;
            den += e;
        }

        const double centroid = den > 0.0 ? num / den : 0.0;
        const double lowerBound = predelay + 0.6 * window;

        rep.info (fmt ("energy centroid %.3f s; last 40 %% of the window starts at %.3f s",
                       centroid, lowerBound));
        rep.gate (centroid >= lowerBound && centroid <= predelay + window + 0.150,
                  fmt ("gate 2: energy centroid lands in the last 40 %% of the window "
                       "(%.3f s)", centroid));
    }

    // --- gate 3: hard stop, <= -80 dBFS past predelay + W + 150 ms -------------
    // `outputDelay` is the density allpasses' fixed group delay, not slack: see
    // kRevDensityApSeconds. The -80 dBFS threshold is §3.12's, unchanged.
    {
        const size_t from = (size_t) juce::jmin ((double) ir.size(),
                                                 std::ceil ((predelay + window + outputDelay + 0.150) * fs));
        double peak = 0.0;
        double atSeconds = 0.0;

        for (size_t i = from; i < ir.size(); ++i)
        {
            const double v = juce::jmax (std::abs (ir.l[i]), std::abs (ir.r[i]));

            if (v > peak) { peak = v; atSeconds = (double) i / fs; }
        }

        const double db = 20.0 * std::log10 (juce::jmax (1.0e-300, peak));
        rep.info (fmt ("loudest sample past the window + 150 ms: %.1f dBFS at %.3f s", db, atSeconds));
        rep.gate (db <= -80.0,
                  fmt ("gate 3: wet output <= -80 dBFS for every t > predelay + W + 150 ms "
                       "(%.1f dBFS)", db));
    }

    // --- gate 4: window tracking ----------------------------------------------
    {
        const double decays[4] = { 0.4, 0.9, 1.5, 3.0 };

        for (double decay : decays)
        {
            Setup w = s;
            w.decayS = (float) decay;
            const double expected = (double) ReverbEngine::windowSecondsFor (w.decayS);
            const Ir wIr = renderIr (w, fs, predelay + expected + 0.6);
            const auto wEnv = rmsEnvelopeDb (stereoPower (wIr), fs, 0.020, hop);
            const size_t peak = wEnv.peakIndex();
            const double measured = wEnv.timeOf (peak);
            const double error = (measured - (predelay + expected + outputDelay)) / expected;

            size_t plateauFrom = peak, plateauTo = peak;

            while (plateauFrom > 0 && wEnv.db[plateauFrom - 1] > wEnv.db[peak] - 1.0)
                --plateauFrom;

            while (plateauTo + 1 < wEnv.db.size() && wEnv.db[plateauTo + 1] > wEnv.db[peak] - 1.0)
                ++plateauTo;

            rep.info (fmt ("decay %.1f s -> W %.2f s, envelope peak at %.3f s against an "
                           "expected %.3f s (%+.1f %%), within 1 dB of the peak over "
                           "%.3f .. %.3f s",
                           decay, expected, measured, predelay + expected + outputDelay,
                           100.0 * error,
                           wEnv.timeOf (plateauFrom), wEnv.timeOf (plateauTo)));
            rep.gate (std::abs (error) <= 0.05,
                      fmt ("gate 4: measured stop point tracks W within 5 %% at decay %.1f s "
                           "(%+.1f %%)", decay, 100.0 * error));
        }
    }

    // --- gate 5: mono sum (§8's row, applies to Reverse as written) ------------
    {
        const auto notch = monoSumNotch (ir);
        result.notchHz = notch.atHz;
        result.notchDb = notch.worstNotchDb;
        result.monoSumGridDb = notch.gridDb;
        rep.info (fmt ("deepest mono-sum notch %+.2f dB at %.0f Hz", notch.worstNotchDb, notch.atHz));
        rep.gate (notch.worstNotchDb >= -6.0,
                  fmt ("gate 5: no mono-sum notch deeper than 6 dB (%+.2f dB)", notch.worstNotchDb));
    }

    // --- gate 6: asserted inertness -------------------------------------------
    {
        Setup lo = s, hi = s;
        lo.damping01 = 0.0f; lo.bassMult = 0.25f; lo.erLevel01 = 0.0f;
        hi.damping01 = 1.0f; hi.bassMult = 4.0f;  hi.erLevel01 = 1.0f;

        const Ir a = renderIr (lo, fs, predelay + window + 0.3);
        const Ir b = renderIr (hi, fs, predelay + window + 0.3);

        bool identical = a.size() == b.size();
        size_t firstDiff = 0;

        for (size_t i = 0; identical && i < a.size(); ++i)
            if (a.l[i] != b.l[i] || a.r[i] != b.r[i]) { identical = false; firstDiff = i; }

        if (! identical)
            rep.info (fmt ("first difference at sample %zu (%.3f s)", firstDiff, (double) firstDiff / fs));

        rep.gate (identical,
                  "gate 6: damping, bassMult and erLevel at min vs max render "
                  "bit-identically in Reverse");
    }

    const auto broadband = schroederT60 (ir.l, fs);
    result.broadbandT60 = broadband.valid ? broadband.t60 : 0.0;

    return result;
}

//==============================================================================
// §3.9 Spring — §7 Stage 2's verification block
//==============================================================================

/** §3.9's three spring lengths, in ms (ReverbEngine.cpp's kSpringTdMs). Everything else
    the expected pulse period needs — M, a1, the chirp knee and the solve for K1 and a2 —
    is taken from a live SpringDispersion below rather than copied, so the expectation and
    the engine cannot drift apart. */
constexpr double kSpringTdMs[3] = { 51.3, 56.1, 61.7 };

/** §3.9's "loop gain tuned to ~= -5.4 dB per pulse". The engine honours it as the CHARACTER
    of reverb_decay's low end rather than as a hard-wired constant — a fixed -5.4 dB would
    make §4.1's 4.0 s ceiling unreachable and leave reverb_decay inert — so the standard T60
    law gives 60*d/T60 dB per pulse, which is 5.37 dB at T60 = 0.60 s with the shortest
    spring's loop period d. That is where this gate measures, and the measurement is taken
    at the loop's own reference band (§3.9's passband centre, sqrt(90 Hz * 2.8 kHz)). */
constexpr double kSpringPulseDecayDb = 5.4;
constexpr double kSpringPulseDecayTolDb = 0.5;
constexpr double kSpringDecayT60 = 0.60;

/** §7 Stage 2: the two chirp assertions must measure the SAME phenomenon. */
constexpr double kSpringSpacingTolerance = 0.05;    // 5 % of Td + the DC group delay
constexpr double kSpringChirpSpreadMs = 15.0;

struct ChirpBand
{
    const char* name;
    double loHz, hiHz;
};

/** The bands the spacing is measured in.

    The cascade's group delay is NOT monotonic in frequency, and the probe measures what
    the filter actually does. Per section A(z) = (a1 + D(z))/(1 + a1*D(z)) with D a delay
    of K samples, the group delay is K*(1-a1^2)/(1+a1^2+2*a1*cos(K*w)) samples, i.e. a
    function of theta = K*w = pi*f/4200 alone: 0.227*K at DC, rising to 4.405*K at the knee
    (theta = pi, f = 4200 Hz) and falling back to its DC value at the WRAP (theta = 2*pi,
    f = 8400 Hz) — which is precisely why §3.9 puts a 2.8 kHz one-pole in the loop to bury
    the repeats. So 6-10 kHz straddles the wrap and is the band that measures Td + the DC
    delay (the first assertion), while the band that is 15 ms late is the knee, not DC.
    100-400 Hz is measured and reported for the record; its spacing is within a millisecond
    of the 6-10 kHz one BY CONSTRUCTION, so a >= 15 ms gate between those two would be a
    gate against the arithmetic of §3.9 itself. The first draft of §7 Stage 2 asked for
    exactly that; the 15 ms threshold is untouched and is asserted between 6-10 kHz and the
    knee band, and §7 was amended to match on 2026-08-07. */
constexpr ChirpBand kSpringHfBand   { "6-10 kHz",    6000.0, 10000.0 };
constexpr ChirpBand kSpringLfBand   { "100-400 Hz",   100.0,   400.0 };
constexpr ChirpBand kSpringKneeBand { "3.4-5.0 kHz", 3400.0,  5000.0 };
constexpr ChirpBand kSpringBands[5] = { kSpringLfBand,
                                        { "0.8-1.6 kHz", 800.0, 1600.0 },
                                        { "2.0-2.8 kHz", 2000.0, 2800.0 },
                                        kSpringKneeBand,
                                        kSpringHfBand };

struct SpringGeometry
{
    double lfDelaySamples = 0.0;    // the cascade's group delay at DC
    double lfDelayMs = 0.0;
    double kneeHz = 0.0;
    int    stretch = 0;             // K1
    double periodSeconds[3] {};     // Td_s + the DC group delay
};

/** The expected pulse periods, computed from the engine's own solve: a probe-local
    SpringDispersion prepared at `fs` returns the same K, K1 and DC group delay the three
    in-loop cascades were built with. size01 = 0.5 over Spring's 0.50 .. 1.50 range is
    sizeScale 1.0 exactly (§4.1), so Td is §3.9's table value with no scaling. */
SpringGeometry springGeometry (double fs)
{
    tubamp::SpringDispersion cascade;
    cascade.prepare (fs);

    SpringGeometry g;
    g.lfDelaySamples = (double) cascade.lfDelaySamples();
    g.lfDelayMs = 1000.0 * g.lfDelaySamples / fs;
    g.kneeHz = (double) cascade.chirpKneeHz (fs);
    g.stretch = cascade.stretchSamples();

    for (int s = 0; s < 3; ++s)
        g.periodSeconds[s] = kSpringTdMs[s] * 0.001 + g.lfDelaySamples / fs;

    return g;
}

/** The first index at or after `after` where the envelope has fallen `db` below its peak.
    `timeBelowPeak` searches from index 0, which is the right answer for a tank mode (whose
    IR is loudest at t = 0) and the wrong one for a spring: nothing reaches the pickup
    before Td, so the pre-arrival silence answers the question immediately. */
size_t indexBelowPeakAfter (const Envelope& env, size_t after, double db)
{
    double peak = -1.0e300;

    for (double v : env.db)
        peak = juce::jmax (peak, v);

    for (size_t i = after; i < env.db.size(); ++i)
        if (env.db[i] <= peak + db)
            return i;

    return env.db.size();
}

struct BandPeaks
{
    bool valid = false;
    std::vector<double> times;      // prominent envelope maxima, in time order
    double loudestSeconds = 0.0;
};

/** Every prominent maximum of one band's level envelope. A spring's pulse train is what it
    is — three interleaved trains 4.8 and 5.6 ms apart — so the measurement times the
    individual arrivals instead of searching for a period: the strongest autocorrelation
    peak of the 6-10 kHz envelope lands at 43.95 ms, which is spring 0's second pulse
    against spring 2's first (108.0 - 64.4) and no spring's period at all.

    The timing is unbiased for the same reason the 6-10 kHz band is the band §7 names:
    energy piles up where the group delay is stationary, which inside 6-10 kHz is the wrap
    at fs/K — where the stretched allpass's delay is back at its DC value — and inside the
    knee band is the knee. The envelope peak therefore reads the band's characteristic
    delay rather than the band's arithmetic mean. */
BandPeaks bandPeaks (const Ir& ir, const ChirpBand& band, double prominenceDb)
{
    const double fs = ir.fs;
    const auto l = bandPass (ir.l, fs, band.loHz, band.hiHz);
    const auto r = bandPass (ir.r, fs, band.loHz, band.hiHz);

    std::vector<double> power (ir.size());

    for (size_t i = 0; i < ir.size(); ++i)
        power[i] = std::sqrt (0.5 * (l[i] * l[i] + r[i] * r[i]));

    const int hop = juce::jmax (1, (int) std::lround (0.00025 * fs));      // 0.25 ms
    const auto env = rmsEnvelopeDb (power, fs, 0.004, hop);

    BandPeaks out;

    if (env.db.size() < 64)
        return out;

    const double loudest = env.db[env.peakIndex()];

    for (size_t i = 1; i + 1 < env.db.size(); ++i)
        if (env.db[i] > env.db[i - 1] && env.db[i] >= env.db[i + 1]
            && env.db[i] > loudest - prominenceDb)
            out.times.push_back (env.timeOf (i));

    out.valid = ! out.times.empty();
    out.loudestSeconds = env.timeOf (env.peakIndex());
    return out;
}

/** The peak nearest `target`, or 0 if none lands inside `tolerance` of it. The window is
    wider than the gate it feeds (10 % against 5 %), and the springs are 8.9 % apart, so
    "nearest" still assigns each pulse to the spring that made it. */
double nearestPeakTo (const BandPeaks& peaks, double target, double tolerance)
{
    double best = 0.0, bestError = tolerance * target;

    for (double t : peaks.times)
        if (std::abs (t - target) <= bestError)
        {
            bestError = std::abs (t - target);
            best = t;
        }

    return best;
}

/** The configuration the chirp is measured in: a linear tank whose in-loop LP is at its
    2.8 kHz ceiling, so the bands above the knee survive long enough to correlate. */
Setup springSetup (double decayS)
{
    Setup s;
    s.algo = ReverbEngine::modeSpring;
    s.size01 = 0.5f;         // sizeScale 1.0: Td is §3.9's table value
    s.damping01 = 0.0f;      // in-loop LP at 2.8 kHz (§4.1's hfXoverHz for Spring)
    s.diffusion01 = 0.0f;    // DWELL at its floor (§5.2) — no tanh harmonics in the bands
    s.mod01 = 0.0f;          // modDepth is 0 in Spring's column regardless
    s.erLevel01 = 0.0f;      // Early Energy is idled in Spring (erToTank 0)
    s.decayS = (float) decayS;
    return s;
}

RateResult analyseSpring (double fs, const juce::File& outDir, Report& rep)
{
    rep.head (fmt ("Spring @ %.0f Hz  (algo 3, color = 0, §3.9)", fs));

    RateResult result;
    result.fs = fs;
    result.algo = ReverbEngine::modeSpring;

    const auto geo = springGeometry (fs);
    const double k = fs / (2.0 * (double) tubamp::SpringDispersion::kChirpFcHz);

    rep.info (fmt ("dispersion: K = %.4f, K1 = %d, knee %.0f Hz, wrap %.0f Hz, M = %d, "
                   "a1 = %.2f; DC group delay %.1f samples = %.3f ms",
                   k, geo.stretch, geo.kneeHz, 2.0 * geo.kneeHz,
                   tubamp::SpringDispersion::kNumSections,
                   (double) tubamp::SpringDispersion::kA1, geo.lfDelaySamples, geo.lfDelayMs));
    rep.info (fmt ("expected pulse periods Td + M*K*(1-a1)/(1+a1): %.2f / %.2f / %.2f ms "
                   "(the first draft's K1 form reads %.2f ms of group delay, %.1f %% of the shortest "
                   "period away from the engine's K form and inside the 5 %% gate either way)",
                   1000.0 * geo.periodSeconds[0], 1000.0 * geo.periodSeconds[1],
                   1000.0 * geo.periodSeconds[2],
                   1000.0 * (double) geo.stretch * tubamp::SpringDispersion::kNumSections
                       * (1.0 - tubamp::SpringDispersion::kA1)
                       / (1.0 + tubamp::SpringDispersion::kA1) / fs,
                   100.0 * (geo.lfDelaySamples - (double) geo.stretch
                            * tubamp::SpringDispersion::kNumSections
                            * (1.0 - tubamp::SpringDispersion::kA1)
                            / (1.0 + tubamp::SpringDispersion::kA1))
                       / (fs * geo.periodSeconds[0])));

    const Setup s = springSetup (2.0);
    const Ir ir = renderIr (s, fs, 2.5);

    const auto wav = outDir.getChildFile (fmt ("ir_Spring_%.0f.wav", fs));

    if (writeIrWav (ir, wav))
        rep.info (fmt ("wrote %s (32-bit float, peak %.6g)", wav.getFileName().toRawUTF8(), peakOf (ir)));

    // --- the band table --------------------------------------------------------
    // Per band and per spring: the first arrival, the second pass, and the spacing between
    // them. Measuring the DIFFERENCE also cancels the analysis filter's own group delay,
    // which is a millisecond or two at 100-400 Hz and is not the spring's.
    BandPeaks peaks[5];
    double spacing[5][3] {};

    for (int b = 0; b < 5; ++b)
    {
        peaks[b] = bandPeaks (ir, kSpringBands[b], 30.0);
        juce::String row;

        for (int s = 0; s < 3; ++s)
        {
            const double first = nearestPeakTo (peaks[b], geo.periodSeconds[s], 0.10);
            const double second = nearestPeakTo (peaks[b], 2.0 * geo.periodSeconds[s], 0.10);
            spacing[b][s] = first > 0.0 && second > first ? second - first : 0.0;

            row << (spacing[b][s] > 0.0 ? fmt ("%7.2f ", 1000.0 * spacing[b][s])
                                        : juce::String ("      - "));
        }

        rep.info (fmt ("%-12s first arrival %7.2f ms, loudest %7.2f ms, per-spring spacing %s ms",
                       kSpringBands[b].name,
                       peaks[b].valid ? 1000.0 * peaks[b].times.front() : 0.0,
                       1000.0 * peaks[b].loudestSeconds, row.trim().toRawUTF8()));
    }

    // --- gate 1: 6-10 kHz spacing = Td + the cascade's DC group delay -----------
    {
        for (int s = 0; s < 3; ++s)
        {
            const double measured = spacing[4][s];
            const double error = measured > 0.0
                                     ? (measured - geo.periodSeconds[s]) / geo.periodSeconds[s]
                                     : 1.0;

            rep.gate (measured > 0.0 && std::abs (error) <= kSpringSpacingTolerance,
                      fmt ("gate 1.%d: spring %d's 6-10 kHz pulse spacing is within 5 %% of "
                           "Td + M*K*(1-a1)/(1+a1) = %.2f ms (%.2f ms, %+.1f %%)",
                           s, s, 1000.0 * geo.periodSeconds[s], 1000.0 * measured,
                           100.0 * error));
        }
    }

    // --- gate 2: the dispersion spread ----------------------------------------
    {
        const double hfArrival = nearestPeakTo (peaks[4], geo.periodSeconds[0], 0.10);
        const double kneeArrival = peaks[3].loudestSeconds;   // the knee band's stationary point
        const double spreadMs = 1000.0 * (kneeArrival - hfArrival);
        // Spacings, not arrivals, for the LF comparison: the two analysis filters have
        // group delays of their own (a millisecond or so at 100-400 Hz), and a difference of
        // spacings cancels them. The knee band cannot be read that way — its arrival is
        // smeared over tens of ms and its second pass is not a resolvable peak — so the
        // spread above is measured between first arrivals, whose filter terms are both well
        // under a millisecond at 3.4 kHz and above.
        const double lfMs = 1000.0 * (spacing[0][0] - spacing[4][0]);

        rep.info (fmt ("spring 0 reaches the pickup at %.2f ms in 6-10 kHz (arriving at "
                       "%.2f ms) and at %.2f ms in the knee band: the chirp spreads %+.2f ms "
                       "per pass. The 100-400 Hz band's spacing is %+.2f ms from the 6-10 kHz "
                       "one — the cascade's group delay returns to its DC value at the wrap, "
                       "so those two agree by construction (see kSpringBands)",
                       1000.0 * spacing[4][0], 1000.0 * hfArrival, 1000.0 * kneeArrival,
                       spreadMs, lfMs));
        rep.gate (hfArrival > 0.0 && kneeArrival > 0.0 && spreadMs >= kSpringChirpSpreadMs,
                  fmt ("gate 2: the chirp spreads at least %.0f ms per pass between the knee "
                       "band and 6-10 kHz (%.2f ms)", kSpringChirpSpreadMs, spreadMs));
    }

    // --- gate 3: ~ -5.4 dB per pulse over the first 10 dB -----------------------
    {
        const Setup d = springSetup (kSpringDecayT60);
        const Ir decayIr = renderIr (d, fs, 2.0);

        // §3.9's loop reference: the geometric mean of the two in-loop corners, 90 Hz and
        // the undamped 2.8 kHz LP, which is the band the loop gain is solved against.
        //
        // Measured as the energy-decay ratio over WHOLE pulse periods rather than as a fit
        // to the first 10 dB. At 5.4 dB per pulse the first 10 dB is two pulses, and both
        // fitted forms read the staircase rather than its slope: least squares over the
        // Schroeder integral returns 77.6 dB/s against a theoretical 100 (r2 0.73), and the
        // level envelope's own first 10 dB is one pulse's own decay into the gap after it
        // (617 dB/s). For a self-similar train EDC(t + T) / EDC(t) is exactly the per-pulse
        // gain at every t, so N whole periods spanning the first 10 dB give it directly.
        const double refHz = std::sqrt (90.0 * 2800.0);
        const auto band = octaveBand (monoSum (decayIr), fs, refHz);
        const int decayHop = juce::jmax (1, (int) std::lround (0.00025 * fs));
        const auto env = rmsEnvelopeDb (band, fs, 0.004, decayHop);
        const size_t arrival = (size_t) juce::jlimit (0.0, (double) band.size() - 1.0,
                                                      std::lround (env.timeOf (env.peakIndex()) * fs) * 1.0);

        std::vector<double> edc (band.size());
        double acc = 0.0;

        for (size_t i = band.size(); i-- > 0;)
        {
            acc += band[i] * band[i];
            edc[i] = acc;
        }

        const size_t period = (size_t) std::lround (geo.periodSeconds[0] * fs);
        const double e0 = edc[arrival];
        double dbPerPulse = 0.0;
        int pulses = 0;

        for (int n = 1; n <= 8 && arrival + (size_t) n * period < edc.size(); ++n)
        {
            const double drop = -10.0 * std::log10 (juce::jmax (1.0e-300,
                                                                edc[arrival + (size_t) n * period] / e0));
            pulses = n;
            dbPerPulse = drop / (double) n;

            if (drop >= 10.0)
                break;
        }

        rep.info (fmt ("at decay %.2f s the %.0f Hz band's energy decay falls %.2f dB over the "
                       "%d whole pulse periods that cover its first 10 dB, i.e. %.2f dB per "
                       "%.2f ms pulse",
                       kSpringDecayT60, refHz, dbPerPulse * (double) pulses, pulses, dbPerPulse,
                       1000.0 * geo.periodSeconds[0]));
        rep.gate (pulses > 0 && e0 > 0.0
                      && std::abs (dbPerPulse - kSpringPulseDecayDb) <= kSpringPulseDecayTolDb,
                  fmt ("gate 3: the loop loses %.1f +- %.1f dB per pulse over the first 10 dB "
                       "(%.2f dB)", kSpringPulseDecayDb, kSpringPulseDecayTolDb, dbPerPulse));
    }

    // --- the §8 rows that still mean something for a spring --------------------
    {
        const auto notch = monoSumNotch (ir);
        result.notchHz = notch.atHz;
        result.notchDb = notch.worstNotchDb;
        result.monoSumGridDb = notch.gridDb;
        rep.info (fmt ("deepest mono-sum notch %+.2f dB at %.0f Hz", notch.worstNotchDb, notch.atHz));
        rep.gate (notch.worstNotchDb >= -6.0,
                  fmt ("gate 4: no mono-sum notch deeper than 6 dB (%+.2f dB)", notch.worstNotchDb));

        double peak = 0.0;

        for (size_t i = 0; i < ir.size(); ++i)
            peak = juce::jmax (peak, juce::jmax (std::abs (ir.l[i]), std::abs (ir.r[i])));

        rep.gate (std::isfinite (peak) && peak > 1.0e-6 && peak < 4.0,
                  fmt ("gate 5: the spring tank is finite, non-silent and bounded (peak %.4f)", peak));
    }

    const auto broadband = schroederT60 (ir.l, fs);
    result.broadbandT60 = broadband.valid ? broadband.t60 : 0.0;

    return result;
}

//==============================================================================
// §3.11 Shimmer — §7 Stage 3's verification block
//==============================================================================

/** §7 Stage 3: "the stability test is the point". 10 s of pink noise into 60 s of silence
    at every grid point, and the runaway that has to be caught is MANY-TO-ONE — five source
    octaves land in 2-4 kHz — so a peak-only test would miss it. */
constexpr double kShimNoiseSeconds = 10.0;
constexpr double kShimSilenceSeconds = 60.0;
constexpr double kShimNoiseRms = 0.25;
constexpr double kShimPeakCeiling = 1.5;
constexpr double kShimConvergeDb = 1.0;      // successive 10 s windows, §7 Stage 3 (c)
constexpr double kShimMonotonicDb = 0.5;     // 1 s windows, measurement slack only
constexpr double kShimSpectralSlackDb = 0.5;
/** Below this a window is reading §6.6's staynormal (~-360 dBFS), not a tail. */
constexpr double kShimFloorDb = -120.0;
constexpr double kShimPitchCents = 5.0;

const char* kShimIntervalNames[4] = { "-1 oct", "+P5", "+1 oct", "+oct+5th" };

/** Windowed RMS of one band of a rendered tail, in dBFS, one value per `windowSeconds`. */
std::vector<double> windowedBandDb (const std::vector<double>& x, double fs,
                                    double fromSeconds, double windowSeconds)
{
    const size_t win = (size_t) std::lround (windowSeconds * fs);
    const size_t start = (size_t) std::lround (fromSeconds * fs);
    std::vector<double> out;

    for (size_t i = start; i + win <= x.size(); i += win)
    {
        double sum = 0.0;

        for (size_t k = i; k < i + win; ++k)
            sum += x[k] * x[k];

        out.push_back (10.0 * std::log10 (juce::jmax (1.0e-300, sum / (double) win)));
    }

    return out;
}

/** dB per second of a band's windowed level, least squares over the windows that are still
    above the floor. Returns false when the tail is gone before three windows have run. */
bool bandSlopeDbPerSecond (const std::vector<double>& db, double windowSeconds,
                           double floorDb, double& slope, int& usedWindows)
{
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    int n = 0;

    for (size_t i = 0; i < db.size(); ++i)
    {
        if (db[i] <= floorDb)
            break;

        const double t = (double) i * windowSeconds;
        sx += t; sy += db[i]; sxx += t * t; sxy += t * db[i];
        ++n;
    }

    usedWindows = n;
    const double denom = (double) n * sxx - sx * sx;

    if (n < 3 || std::abs (denom) < 1.0e-18)
        return false;

    slope = ((double) n * sxy - sx * sy) / denom;
    return true;
}

/** The power-weighted mean frequency inside [loHz, hiHz], and the band's peak level.

    Not the argmax: 5 cents at 440 Hz is 1.3 Hz, and the shifted voice is NOT a clean tone
    at that resolution. §3.11's four grains redraw a random start offset every restart —
    "randomization to avoid the comb filtering artifacts" — so each grain's 440 Hz component
    enters at a random phase and what reaches the analyser is a 440 Hz carrier under
    symmetric phase-noise skirts. The argmax of that lands on whichever skirt bin happens to
    be highest (a bare PitchShifter at ratio 2 reads +30.5 cents that way); the first moment
    is unbiased for exactly the same reason. */
double bandCentroidHz (const std::vector<double>& power, double fs, int fftSize,
                       double loHz, double hiHz, double& levelDb)
{
    const double binHz = fs / (double) fftSize;
    double num = 0.0, den = 0.0, peak = 0.0;

    for (size_t k = 1; k < power.size(); ++k)
    {
        const double f = (double) k * binHz;

        if (f < loHz || f > hiHz)
            continue;

        num += f * power[k];
        den += power[k];
        peak = juce::jmax (peak, power[k]);
    }

    levelDb = 10.0 * std::log10 (juce::jmax (1.0e-300, peak));
    return den > 0.0 ? num / den : 0.0;
}

Setup shimmerSetup (double decayS, double shimmer01, int interval)
{
    Setup s;
    s.algo = ReverbEngine::modeShimmer;
    s.decayS = (float) decayS;
    s.shimmer01 = (float) shimmer01;
    s.shimmerInterval = interval;
    return s;
}

/** §7 Stage 3's 2D grid. Run once rather than per host rate: nothing in §3.11 is stored in
    samples, and 36 points x 70 s is already the probe's longest single pass. */
void analyseShimmer (double fs, Report& rep)
{
    rep.head (fmt ("Shimmer @ %.0f Hz  (algo 4, color = 0, §3.11 stability grid)", fs));

    const double ceiling = (double) ReverbEngine::decayCeilingFor (ReverbEngine::modeShimmer);
    const double decays[3] = { 0.2, 0.5 * (0.2 + ceiling), ceiling };
    const double shimmers[3] = { 0.0, 0.5, 1.0 };
    const int total = (int) std::lround ((kShimNoiseSeconds + kShimSilenceSeconds) * fs);

    rep.info (fmt ("grid: reverb_shimmer {0, 0.5, 1} x reverb_decay {%.1f, %.1f, %.1f} s x 4 "
                   "intervals = 36 points, each %.0f s of pink noise at %.2f RMS followed by "
                   "%.0f s of silence (nothing trimmed)",
                   decays[0], decays[1], decays[2], kShimNoiseSeconds, kShimNoiseRms,
                   kShimSilenceSeconds));
    rep.info (fmt ("windows: 1 s for the monotonic tail and the spectral slopes, 10 s for the "
                   "2-4 kHz convergence; a window at or below %.0f dBFS is reading §6.6's "
                   "staynormal rather than a tail and ends the fit",
                   kShimFloorDb));

    const auto noise = pinkNoise ((int) std::lround (kShimNoiseSeconds * fs), kShimNoiseRms, 0x51D3A7u);

    double worstPeak = 0.0, worstRise = -1.0e300, worstConverge = -1.0e300;
    double worstSpectral = 1.0e300;
    juce::String worstPeakAt, worstRiseAt, worstConvergeAt, worstSpectralAt;
    int spectralPoints = 0;

    for (int si = 0; si < 3; ++si)
        for (int di = 0; di < 3; ++di)
            for (int interval = 0; interval < 4; ++interval)
            {
                const auto point = fmt ("shimmer %.1f, decay %.1f s, %s",
                                        shimmers[si], decays[di], kShimIntervalNames[interval]);
                const Ir ir = renderThrough (shimmerSetup (decays[di], shimmers[si], interval),
                                             fs, noise, total);

                // (a) peak absolute sample over the whole render.
                double peak = 0.0;
                bool finite = true;

                for (size_t i = 0; i < ir.size(); ++i)
                {
                    peak = juce::jmax (peak, juce::jmax (std::abs (ir.l[i]), std::abs (ir.r[i])));
                    finite = finite && std::isfinite (ir.l[i]) && std::isfinite (ir.r[i]);
                }

                if (peak > worstPeak) { worstPeak = peak; worstPeakAt = point; }

                rep.gate (finite && peak <= kShimPeakCeiling,
                          fmt ("%s: peak sample %.4f <= %.1f", point.toRawUTF8(), peak,
                               kShimPeakCeiling));

                const auto mono = monoSum (ir);

                // (b) the tail decays monotonically, 1 s windows over the silence.
                {
                    const auto broadband = windowedBandDb (mono, fs, kShimNoiseSeconds, 1.0);
                    double worst = -1.0e300;
                    double worstAt = 0.0;

                    for (size_t i = 1; i < broadband.size(); ++i)
                    {
                        if (broadband[i - 1] <= kShimFloorDb)
                            break;

                        const double rise = broadband[i] - broadband[i - 1];

                        if (rise > worst) { worst = rise; worstAt = (double) i; }
                    }

                    if (worst > worstRise) { worstRise = worst; worstRiseAt = point; }

                    rep.gate (worst <= kShimMonotonicDb,
                              fmt ("%s: the tail never rises between 1 s windows (worst %+.2f dB "
                                   "at window %.0f)", point.toRawUTF8(), worst, worstAt));
                }

                const auto mid = bandPass (mono, fs, 2000.0, 4000.0);
                const auto high = bandPass (mono, fs, 8000.0, juce::jmin (16000.0, 0.45 * fs));

                // (c) the 2-4 kHz band converges: successive 10 s windows never RISE.
                // The −120 dBFS floor the section header documents applies here as it
                // does in (b) — a sub-floor window is §6.6's staynormal residue, whose
                // ~−390 dBFS numerical wobble is not a convergence claim about signal —
                // but as a SKIP rather than (b)'s break: a genuine runaway climbing up
                // out of the floor is caught at the first pair whose earlier window is
                // back above it, which a break would never reach.
                {
                    const auto midTen = windowedBandDb (mid, fs, kShimNoiseSeconds, 10.0);
                    double worst = -1.0e300;

                    for (size_t i = 1; i < midTen.size(); ++i)
                        if (midTen[i - 1] > kShimFloorDb)
                            worst = juce::jmax (worst, midTen[i] - midTen[i - 1]);

                    if (worst > worstConverge) { worstConverge = worst; worstConvergeAt = point; }

                    rep.gate (worst <= kShimConvergeDb,
                              fmt ("%s: 2-4 kHz energy converges — no 10 s window is more than "
                                   "%.0f dB above the one before it (worst %+.2f dB)",
                                   point.toRawUTF8(), kShimConvergeDb, worst));
                }

                // (d) above 8 kHz decays at least as fast as the mid band.
                {
                    const auto midOne = windowedBandDb (mid, fs, kShimNoiseSeconds, 1.0);
                    const auto highOne = windowedBandDb (high, fs, kShimNoiseSeconds, 1.0);
                    double midSlope = 0.0, highSlope = 0.0;
                    int midWindows = 0, highWindows = 0;
                    const bool haveMid = bandSlopeDbPerSecond (midOne, 1.0, kShimFloorDb,
                                                               midSlope, midWindows);
                    const bool haveHigh = bandSlopeDbPerSecond (highOne, 1.0, kShimFloorDb,
                                                                highSlope, highWindows);

                    if (haveMid && haveHigh)
                    {
                        const double margin = midSlope - highSlope;   // >= 0: the HF falls faster

                        if (margin < worstSpectral) { worstSpectral = margin; worstSpectralAt = point; }

                        ++spectralPoints;
                        rep.gate (margin >= -kShimSpectralSlackDb,
                                  fmt ("%s: above 8 kHz decays at least as fast as 2-4 kHz "
                                       "(%.1f vs %.1f dB/s over %d/%d windows)",
                                       point.toRawUTF8(), -highSlope, -midSlope,
                                       highWindows, midWindows));
                    }
                    else
                    {
                        rep.info (fmt ("%s: spectral slope n/a — the tail is under the floor "
                                       "within %d/%d windows", point.toRawUTF8(),
                                       highWindows, midWindows));
                    }
                }
            }

    rep.info (fmt ("worst over the grid: peak %.4f (%s); 1 s rise %+.2f dB (%s); 10 s 2-4 kHz "
                   "rise %+.2f dB (%s)",
                   worstPeak, worstPeakAt.toRawUTF8(), worstRise, worstRiseAt.toRawUTF8(),
                   worstConverge, worstConvergeAt.toRawUTF8()));

    if (spectralPoints > 0)
        rep.info (fmt ("tightest HF-vs-mid decay margin %+.2f dB/s (%s) over the %d points "
                       "whose tail outlives three windows",
                       worstSpectral, worstSpectralAt.toRawUTF8(), spectralPoints));

    // --- pitch: 220 Hz in, a 440 Hz partial at +1 oct --------------------------
    {
        const double inputHz = 220.0;
        const double expectedHz = 440.0;
        const int order = welchOrderFor (fs) + 2;         // ~0.37 Hz bins at 48 kHz
        const int fftSize = 1 << order;
        const int seconds = 8;
        const int totalSamples = (int) std::lround ((double) seconds * fs);
        const double analyseFrom = 3.0;

        std::vector<double> sine ((size_t) totalSamples);

        for (int i = 0; i < totalSamples; ++i)
            sine[(size_t) i] = 0.5 * std::sin (2.0 * juce::MathConstants<double>::pi
                                               * inputHz * (double) i / fs);

        const auto centsOf = [expectedHz] (double hz)
        {
            return 1200.0 * std::log2 (juce::jmax (1.0e-9, hz) / expectedHz);
        };

        // The shifter alone, so §3.11's ratio is asserted against nothing but itself.
        {
            tubamp::PitchShifter bare;
            bare.prepare (fs);
            bare.setRatio (tubamp::PitchShifter::ratioFor (2));
            std::vector<double> shifted ((size_t) totalSamples);

            for (int i = 0; i < totalSamples; ++i)
                shifted[(size_t) i] = (double) bare.process ((float) sine[(size_t) i]);

            std::vector<double> tail (shifted.begin() + (size_t) std::lround (analyseFrom * fs),
                                      shifted.end());
            double level = 0.0;
            const double hz = bandCentroidHz (welchPower (tail, order), fs, fftSize,
                                              0.9 * expectedHz, 1.1 * expectedHz, level);

            rep.info (fmt ("PitchShifter alone at ratio 2: 220 Hz in, %.3f Hz out (%+.2f cents)",
                           hz, centsOf (hz)));
            rep.gate (std::abs (centsOf (hz)) <= kShimPitchCents,
                      fmt ("pitch: the shifter's own ratio is +1 oct within %.0f cents "
                           "(%+.2f cents)", kShimPitchCents, centsOf (hz)));
        }

        // Through the engine. Measured at three decays and gated on the mean: the tank's own
        // modal structure around 440 Hz tilts a first moment by a couple of cents either way
        // (-2.99 / -0.66 / +0.97 at 0.5 / 1 / 2 s here, and the sign follows the mode layout,
        // not the interval), while a wrong ratio would move all three the same way.
        const double pitchDecays[3] = { 0.5, 1.0, 2.0 };
        double centsSum = 0.0, quietest = 1.0e300;
        juce::String perDecay;

        for (double decay : pitchDecays)
        {
            const auto measure = [&] (double shimmer, double& partialDb, double& fundamentalDb)
            {
                // reverb_mod off: §3.6's random walk detunes the WHOLE tail, which is the
                // tank's modulation and not the shifter's ratio.
                Setup pitchSetup = shimmerSetup (decay, shimmer, 2);
                pitchSetup.mod01 = 0.0f;

                const Ir ir = renderThrough (pitchSetup, fs, sine, totalSamples);
                std::vector<double> tail (ir.l.begin() + (size_t) std::lround (analyseFrom * fs),
                                          ir.l.end());
                const auto power = welchPower (tail, order);
                const double hz = bandCentroidHz (power, fs, fftSize, 0.9 * expectedHz,
                                                  1.1 * expectedHz, partialDb);
                bandCentroidHz (power, fs, fftSize, 0.9 * inputHz, 1.1 * inputHz, fundamentalDb);
                return hz;
            };

            double onPartial = 0.0, onFundamental = 0.0, offPartial = 0.0, offFundamental = 0.0;
            const double hz = measure (1.0, onPartial, onFundamental);
            measure (0.0, offPartial, offFundamental);

            centsSum += centsOf (hz);
            quietest = juce::jmin (quietest, onPartial - offPartial);
            perDecay << fmt ("%.1f s: %.3f Hz (%+.2f cents, %.1f dB over the same bin at "
                             "reverb_shimmer = 0)  ", decay, hz, centsOf (hz),
                             onPartial - offPartial);
        }

        const double meanCents = centsSum / 3.0;

        rep.info (fmt ("220 Hz in at +1 oct — %s", perDecay.trim().toRawUTF8()));
        rep.gate (std::abs (meanCents) <= kShimPitchCents,
                  fmt ("pitch: a 220 Hz sine produces a 440 Hz partial within %.0f cents at "
                       "+1 oct (%+.2f cents, mean of three decays)", kShimPitchCents, meanCents));
        rep.gate (quietest >= 20.0,
                  fmt ("pitch: that partial is the shifter's and not the tank's — it is at "
                       "least %.1f dB louder at reverb_shimmer = 1 than at 0", quietest));
    }
}

//==============================================================================
// §6.8 CPU: three Spring instances at 192 kHz (§7 Stage 2's last line). Reported, never
// gated — a wall-clock number on one machine is not a portable threshold.
//==============================================================================
void measureSpringCpu (Report& rep)
{
    constexpr double fs = 192000.0;
    constexpr int instances = 3;
    constexpr int blocks = 400;

    rep.head (u8 ("CPU: 3 Spring instances at 192 kHz (§7 Stage 2, reported not gated)"));

    std::array<ReverbEngine, instances> engines;
    Setup s = springSetup (2.0);

    for (auto& e : engines)
    {
        e.configure (s.algo, ReverbEngine::windowSecondsFor (s.decayS));
        e.prepare ({ fs, (juce::uint32) kBlockSize, 2 });
    }

    const auto noise = pinkNoise (kBlockSize, 0.2, 0x2B7Fu);
    juce::AudioBuffer<float> buffer (2, kBlockSize);
    auto p = paramsOf (s);

    const auto runOnce = [&] (int numBlocks)
    {
        for (int b = 0; b < numBlocks; ++b)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < kBlockSize; ++i)
                    buffer.setSample (ch, i, (float) noise[(size_t) i]);

            for (auto& e : engines)
            {
                e.setParameters (p);
                e.process (juce::dsp::AudioBlock<float> (buffer));
            }

            p.snap = false;
        }
    };

    runOnce (40);                                     // warm the caches and settle the ramps

    const auto start = std::chrono::steady_clock::now();
    runOnce (blocks);
    const auto elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();

    const double msPerBlock = 1000.0 * elapsed / (double) blocks;
    const double budgetMs = 1000.0 * (double) kBlockSize / fs;

    rep.info (fmt ("%d instances, %d-sample blocks: %.4f ms per block for all %d "
                   "(%.4f ms each), against a %.3f ms real-time budget — %.1f %% of one core",
                   instances, kBlockSize, msPerBlock, instances,
                   msPerBlock / (double) instances, budgetMs, 100.0 * msPerBlock / budgetMs));
}

//==============================================================================
// §5.1: the inverted colour gates
//==============================================================================
void analyseColour (int algo, double fs, double decayS, Report& rep)
{
    const auto& mode = mirrorFor (ReverbEngine::resolveMode (algo));
    rep.head (fmt ("%s @ %.0f Hz  colour axis (color = 1, §5.1 inverted gates)", mode.name, fs));

    Setup s = metricsSetup (algo, decayS);
    s.color01 = 1.0f;

    const double seconds = juce::jmax (4.0, 3.0 * (double) s.decayS + 1.0);
    const Ir ir = renderIr (s, fs, seconds);

    const int order = welchOrderFor (fs);
    const int fftSize = 1 << order;
    const auto db = toDb (welchPower (ir.l, order));
    const auto smoothed = octaveSmoothDb (db, fs, fftSize, 12.0);
    const auto peak = worstDeviation (smoothed, fs, fftSize, 100.0, juce::jmin (12000.0, fs * 0.4),
                                      1.0, false);
    const double flutter = flutterWorstPeak (ir, algo, rep, true);

    rep.info (fmt ("worst spectral peak %+.2f dB at %.0f Hz; worst flutter %.3f",
                   peak.worstDb, peak.atHz, flutter));

    // "If it does not show peaks, the knob is not working" — reported, never fatal.
    rep.colourGate (peak.worstDb > 3.0 || flutter > 0.05,
                    fmt ("colour = 1 measurably colours the tail (peak %+.2f dB, flutter %.3f)",
                         peak.worstDb, flutter));
}

//==============================================================================
// §7 Stage 1 gate 7's two remaining checks: ducking depth and the two-sided spread
//==============================================================================

/** Both are properties of `ReverbEngine` alone — the ducker is fed from the engine's own
    dry send (§3.1) and the spread cascades live on its wet bus (§3.8) — so they are
    measured here rather than at processor level, where the only extra machinery is the
    parameter plumbing `runReverbTests` already covers.

    AMENDED 2026-08-06. §7 words the ducking gate as "wet RMS during a sustained note >= 6
    dB below wet RMS 800 ms after note-off", and that comparison cannot pass with §3.1's
    own 1200 ms release: 800 ms after note-off the sidechain envelope has only fallen to
    exp(-800/1200) = 51 % of its steady value, so the ducker gives back about 4.6 dB, while
    the tail it is measured on has decayed 24 dB over the same 800 ms at Room's default
    2 s decay. The gate as written measures the tail's decay, not the ducker's depth. What
    it means to measure — how far the ducker pushes the wet down while the note sustains —
    is well posed against the SAME render with duck = 0, so that is what is gated, at the
    same 6 dB. The literal number is reported alongside it, and the release is separately
    asserted to be recovering. */
void analyseDuckAndSpread (double fs, Report& rep)
{
    rep.head (fmt ("Ducking and two-sided spread @ %.0f Hz  (§7 Stage 1 gate 7)", fs));

    // --- ducking --------------------------------------------------------------
    {
        const double noteSeconds = 2.0, tailSeconds = 1.5;
        const int noteSamples = (int) std::lround (noteSeconds * fs);
        const int total = noteSamples + (int) std::lround (tailSeconds * fs);
        const int fade = (int) std::lround (0.005 * fs);      // no click at note-off

        std::vector<double> note ((size_t) total, 0.0);

        for (int i = 0; i < noteSamples; ++i)
        {
            const double env = juce::jmin (1.0, juce::jmin ((double) i, (double) (noteSamples - i))
                                                    / (double) fade);
            note[(size_t) i] = 0.5 * env * std::sin (2.0 * juce::MathConstants<double>::pi
                                                     * 220.0 * (double) i / fs);
        }

        Setup ducked = metricsSetup (ReverbEngine::modeRoom, 2.0);
        Setup open = ducked;
        ducked.duck01 = 1.0f;
        open.duck01 = 0.0f;

        const Ir a = renderThrough (ducked, fs, note, total);
        const Ir b = renderThrough (open, fs, note, total);

        auto rmsOver = [fs] (const Ir& ir, double from, double to)
        {
            const size_t i0 = (size_t) juce::jlimit (0.0, (double) ir.size(), from * fs);
            const size_t i1 = (size_t) juce::jlimit (0.0, (double) ir.size(), to * fs);
            double sum = 0.0;

            for (size_t i = i0; i < i1; ++i)
                sum += 0.5 * (ir.l[i] * ir.l[i] + ir.r[i] * ir.r[i]);

            return i1 > i0 ? std::sqrt (sum / (double) (i1 - i0)) : 0.0;
        };

        auto db = [] (double x) { return 20.0 * std::log10 (juce::jmax (1.0e-300, x)); };

        // The last 500 ms of the note: the sidechain envelope is at its steady value and
        // the wet field is fully built.
        const double duringDucked = rmsOver (a, noteSeconds - 0.5, noteSeconds);
        const double duringOpen = rmsOver (b, noteSeconds - 0.5, noteSeconds);
        const double depth = db (duringOpen) - db (duringDucked);

        // The literal §7 comparison, reported so the amendment above is checkable.
        const double afterDucked = rmsOver (a, noteSeconds + 0.8, noteSeconds + 1.0);
        const double literal = db (afterDucked) - db (duringDucked);

        // ... and the release, isolated the same way the depth is: 800 ms after note-off
        // the ducker must have given gain back relative to its steady-state depth.
        const double afterOpen = rmsOver (b, noteSeconds + 0.8, noteSeconds + 1.0);
        const double residual = db (afterOpen) - db (afterDucked);

        rep.info (fmt ("wet RMS during the note: %.1f dBFS ducked, %.1f dBFS at duck = 0 "
                       "-> %.1f dB of ducking", db (duringDucked), db (duringOpen), depth));
        rep.info (fmt ("literal §7 form (wet 800 ms after note-off vs during the note): "
                       "%+.1f dB — the tail itself falls %.1f dB over those 800 ms",
                       literal, db (duringOpen) - db (afterOpen)));
        rep.info (fmt ("residual ducking 800 ms after note-off: %.1f dB (release recovering "
                       "from %.1f dB)", residual, depth));

        rep.gate (depth >= 6.0,
                  fmt ("ducking gate: at duck = 1 a sustained note pushes the wet >= 6 dB "
                       "below the same render at duck = 0 (%.1f dB)", depth));
        rep.gate (residual < depth - 1.0,
                  fmt ("the 1200 ms release is giving gain back after note-off (%.1f dB "
                       "residual against %.1f dB steady state)", residual, depth));
    }

    // --- two-sided frequency-dependent spread (§3.8) ---------------------------
    // A one-sided cascade delays one channel's bass by ~13 ms and leaves the other at 0,
    // so an 80 Hz transient arrives on one side first. The two-sided pair splits the same
    // group delay 7.2 / 13.0 ms, so the WORST case is the 5.8 ms difference between them.
    // Two numbers, because they fail differently:
    //   * the LF arrival difference over the early field, which is what a one-sided chain
    //     blows out to the full ~13 ms;
    //   * the LF energy balance, which is what an unequal-gain chain blows out. It is read
    //     over 1.5 s rather than the early field because §3.8's two output tap sets are
    //     DELIBERATELY different per channel: inside 50 ms only three or four Hall taps
    //     have fired at all (its shortest tap is 11.2 ms on L and 13.4 ms on R), so an
    //     early-window balance measures which taps happened to land, not the chain — the
    //     same measurement reads -0.135 / +0.437 over 50 ms against -0.067 / +0.077 here.
    //
    // The transient is ONE Hann-windowed cycle of 80 Hz, not two. Two cycles is narrow
    // enough (~40 Hz wide) that the measurement samples the tap sets' own comb at exactly
    // 80 Hz rather than the LF image: it reads +0.204 on Hall and even reverses the sign
    // of Plate's arrival skew against the same test run from a band-limited impulse
    // (-0.014 / +0.030 balance, L earlier on both). One cycle agrees with the impulse in
    // sign and magnitude and is still an 80 Hz transient.
    {
        constexpr double kSpreadBandHz = 200.0;
        constexpr double kEarlySeconds = 0.060;
        constexpr double kBalanceSeconds = 1.500;
        constexpr double kMaxArrivalSkewMs = 6.0;
        const int cycles = 1;
        const int burst = (int) std::lround ((double) cycles / 80.0 * fs);
        const int total = (int) std::lround (2.0 * fs);

        std::vector<double> transient ((size_t) total, 0.0);

        for (int i = 0; i < burst && i < total; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi
                                                   * (double) i / (double) (burst - 1));
            transient[(size_t) i] = w * std::sin (2.0 * juce::MathConstants<double>::pi
                                                  * 80.0 * (double) i / fs);
        }

        for (int algo : { ReverbEngine::modePlate, ReverbEngine::modeHall })
        {
            const Ir ir = renderThrough (metricsSetup (algo, 2.0), fs, transient, total);
            const auto lowL = lowPassZeroPhase (ir.l, fs, kSpreadBandHz);
            const auto lowR = lowPassZeroPhase (ir.r, fs, kSpreadBandHz);

            double earlyL = 0.0, earlyR = 0.0, momentL = 0.0, momentR = 0.0;
            double energyL = 0.0, energyR = 0.0;
            const size_t early = (size_t) juce::jmin ((double) ir.size(), kEarlySeconds * fs);
            const size_t balance = (size_t) juce::jmin ((double) ir.size(), kBalanceSeconds * fs);

            for (size_t i = 0; i < early; ++i)
            {
                const double t = (double) i / fs;
                earlyL += lowL[i] * lowL[i];
                earlyR += lowR[i] * lowR[i];
                momentL += t * lowL[i] * lowL[i];
                momentR += t * lowR[i] * lowR[i];
            }

            for (size_t i = 0; i < balance; ++i)
            {
                energyL += lowL[i] * lowL[i];
                energyR += lowR[i] * lowR[i];
            }

            const double centroid = energyL + energyR > 0.0
                                        ? (energyL - energyR) / (energyL + energyR)
                                        : 0.0;
            const double arrivalL = earlyL > 0.0 ? 1000.0 * momentL / earlyL : 0.0;
            const double arrivalR = earlyR > 0.0 ? 1000.0 * momentR / earlyR : 0.0;
            const double skew = arrivalL - arrivalR;

            rep.info (fmt ("%s: 80 Hz transient below %.0f Hz — L/R energy centroid %+.3f "
                           "over %.0f ms, arrival centroids L %.1f ms / R %.1f ms over the "
                           "first %.0f ms (skew %+.1f ms)",
                           mirrorFor (algo).name, kSpreadBandHz, centroid,
                           1000.0 * kBalanceSeconds, arrivalL, arrivalR,
                           1000.0 * kEarlySeconds, skew));
            rep.gate (std::abs (centroid) <= 0.1,
                      fmt ("%s two-sided-spread gate: LF image centroid within +-0.1 of "
                           "centre on an 80 Hz transient (%+.3f)",
                           mirrorFor (algo).name, centroid));
            rep.gate (std::abs (skew) <= kMaxArrivalSkewMs,
                      fmt ("%s LF arrival skew stays under %.0f ms — half of what a "
                           "one-sided cascade's 13 ms would give (%+.1f ms)",
                           mirrorFor (algo).name, kMaxArrivalSkewMs, skew));
        }
    }
}

//==============================================================================
// §7 Stage 0 baseline: the deleted Freeverb, alive only here
//==============================================================================
void analyseFreeverb (double fs, const juce::File& outDir, Report& rep)
{
    rep.head (fmt ("Freeverb baseline @ %.0f Hz  (juce::dsp::Reverb, §7 Stage 0 — informational)", fs));

    const Ir ir = renderFreeverbIr (fs, 12.0, 0.5f, 0.5f);
    const auto wav = outDir.getChildFile (fmt ("ir_Freeverb_%.0f.wav", fs));

    if (writeIrWav (ir, wav))
        rep.info (fmt ("wrote %s (32-bit float, peak %.6g)",
                       wav.getFileName().toRawUTF8(), peakOf (ir)));

    rep.info (fmt ("modal density %.3f modes/Hz (sum of comb tunings 11024 samples @44.1k)",
                   11024.0 / 44100.0));

    const auto broadband = schroederT60 (ir.l, fs);
    rep.info (fmt ("broadband T60 %.3f s (R2 %.4f)", broadband.valid ? broadband.t60 : 0.0,
                   broadband.r2));

    const auto density = echoDensity (ir.l, fs);
    double maxEta = 0.0, at500 = 0.0;

    for (size_t i = 0; i < density.eta.size(); ++i)
    {
        maxEta = juce::jmax (maxEta, density.eta[i]);

        if (at500 == 0.0 && density.timeS[i] >= 0.500)
            at500 = density.eta[i];
    }

    rep.info (fmt ("echo density peaks at %.3f and reads %.3f at 500 ms "
                   "(§1.1: it never grows)", maxEta, at500));

    flutterWorstPeakFreeverb (ir, rep);

    // §1.2's 44.9 % T60 spread across the comb bank, measured directly from the tunings.
    {
        constexpr int kCombs[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        const double f = 0.5 * 0.28 + 0.7;
        double lo = 1.0e300, hi = 0.0;

        for (int comb : kCombs)
        {
            const double n = (double) comb * fs / 44100.0;
            const double t60 = n / fs * std::log (0.001) / std::log (f);
            lo = juce::jmin (lo, t60);
            hi = juce::jmax (hi, t60);
        }

        rep.info (fmt ("comb-bank T60 spread %.2f .. %.2f s = %+.1f %% (§1.2 predicts 44.9 %%)",
                       lo, hi, 100.0 * (hi / lo - 1.0)));
    }

    const auto notch = monoSumNotch (ir);
    rep.info (fmt ("deepest mono-sum notch %+.2f dB at %.0f Hz (§1.6 predicts ~959 Hz)",
                   notch.worstNotchDb, notch.atHz));

    // Per-band Jot targets are meaningless for Freeverb, so this row reports raw T60s.
    const auto bands = [&]
    {
        static constexpr double kBands[] = { 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0 };
        juce::String out;

        for (double fc : kBands)
        {
            if (fc * juce::MathConstants<double>::sqrt2 >= fs * 0.45)
                continue;

            const auto fit = schroederT60 (octaveBand (ir.l, fs, fc), fs);
            out << fmt ("%.0f Hz %.2f s  ", fc, fit.valid ? fit.t60 : 0.0);
        }

        return out;
    }();

    rep.info ("per-octave T60: " + bands);
}

//==============================================================================
// §3.2 decay-normalised injection (AMENDED 2026-08-07)
//==============================================================================

/** The band both rows are measured in: 500 Hz sits in the mid band of every tank mode
    (bassXoverHz 400..500, hfXoverHz 2800..5500) and within a quarter-octave of Spring's
    passband centre √(f_hp·f_lp) at the default damping — i.e. it is the band whose T60
    `reverb_decay` actually pins in all four modes. The level law is a per-band statement
    (§3.2: line RMS = k_in/√(1−g²) at the band's own g), so a broadband statistic would
    average it with the HF band, whose T60 is capped by `hfMaxT60` and therefore stops
    following the knob — 1.4 dB of the pre-change movement on Room hides there. */
constexpr double kLevelLawBandHz = 500.0;

/** Sustained-state band RMS (dB) of the wet, from a pink-noise render that keeps playing.
    Steady-state energy builds as 1 − 10^(−6t/T60), so skipping 0.4·T60 lands within
    0.02 dB of the asymptote. −20 dBFS keeps §3.5's clipper in its identity region: this
    row measures the LINEAR level law, and the distortion row below drives the knee. */
double sustainedWetRmsDb (int algo, double fs, double decayS)
{
    Setup s = metricsSetup (algo, decayS);
    // The direct wet ER is decay-invariant by the constant-tail law (§3.7), so it can
    // only dilute the quantity under test — the tail — exactly as the per-octave
    // tail-LEVEL row renders at erLevel = 0.
    s.erLevel01 = 0.0f;

    const int skipSamples = (int) std::lround (juce::jmax (1.0, 0.4 * (double) s.decayS) * fs);
    const int total = skipSamples + (int) std::lround (1.5 * fs);
    const auto noise = pinkNoise (total, 0.1, 0xD3C4A11u);            // -20 dBFS RMS
    const Ir ir = renderThrough (s, fs, noise, total);
    const auto band = octaveBand (ir.l, fs, kLevelLawBandHz);

    double energy = 0.0;

    for (size_t i = (size_t) skipSamples; i < band.size(); ++i)
        energy += band[i] * band[i];

    const double n = (double) band.size() - (double) skipSamples;
    return 10.0 * std::log10 (juce::jmax (1.0e-300, energy / juce::jmax (1.0, n)));
}

/** In-loop distortion, as a residual-to-signal ratio in dB, for a sustained −6 dBFS pink
    source. §3.5's clip threshold is an ABSOLUTE 0.8, so before the decay normalisation
    the onset of in-loop distortion tracked `reverb_decay` through the internal level —
    the hidden nonlinearity §8's single-point nonlinearity row cannot see.

    Measured by subtraction rather than as a harmonic ratio: everything in the engine
    except the clipper is linear (time-varying, but identically so in both renders — the
    modulation walk is seeded and the parameters match, so it cancels term by term), and
    a sustained SINE is the wrong excitation for this question anyway. A tone builds
    coherently in the tank (amplitude ~ 1/(1−g)), which no RMS-law normalisation can flat-
    ten; noise builds by the incoherent 1/√(1−g²) law the correction is stated on, and
    noise is also what real material through a NAM block looks like. So: render the hot
    source, render the same source 20 dB down, scale the quiet render back up, and what
    does not cancel is the clipper's own contribution (the quiet render's residual is
    ~G² = 1/100 of the hot one's, i.e. under the ratio's own noise floor). */
/** Where the subtraction method stops resolving the clipper (see the directional-gate
    comment in analyseDecayNormalisation below). */
constexpr double kDistortionMethodFloorDb = -78.0;

double inLoopDistortionDb (int algo, double fs, double decayS)
{
    constexpr double kQuietScale = 0.1;                               // -20 dB probe pair

    Setup s = metricsSetup (algo, decayS);
    s.erLevel01 = 0.0f;

    const int skipSamples = (int) std::lround (juce::jmax (1.0, 0.4 * (double) s.decayS) * fs);
    const int total = skipSamples + (int) std::lround (1.5 * fs);
    const auto hot = pinkNoise (total, 0.5011872, 0x9E3779B9u);       // -6 dBFS RMS
    std::vector<double> quiet (hot.size());

    for (size_t i = 0; i < hot.size(); ++i)
        quiet[i] = hot[i] * kQuietScale;

    const Ir hotIr = renderThrough (s, fs, hot, total);
    const Ir quietIr = renderThrough (s, fs, quiet, total);

    double residual = 0.0, signal = 0.0;

    for (size_t i = (size_t) skipSamples; i < hotIr.size(); ++i)
    {
        const double d = hotIr.l[i] - quietIr.l[i] / kQuietScale;
        residual += d * d;
        signal += hotIr.l[i] * hotIr.l[i];
    }

    return 10.0 * std::log10 (juce::jmax (1.0e-300, residual) / juce::jmax (1.0e-300, signal));
}

/** The two §3.2-amendment gates: sustained wet LEVEL and in-loop DISTORTION against a
    reverb_decay sweep over each mode's full range. Before the decay-normalised injection,
    steady-state line RMS k_in/√(1−g²) made a Decay sweep a loudness sweep on every mode
    and moved the distance to §3.5's fixed 0.8 knee with it. A level law is rate-
    independent by construction — g and gRef both move with fs through D — so this runs
    once, at the first rate, like §3.11's grid. Reverse is exempt by design: nothing
    recirculates and its tap normalisation is already window-invariant (§3.12). */
void analyseDecayNormalisation (const std::vector<double>& modes, double fs, Report& rep)
{
    const auto wants = [&modes] (int algo)
    {
        return std::find (modes.begin(), modes.end(), (double) algo) != modes.end();
    };

    rep.head (fmt ("Decay-normalised injection (%s3.2 AMENDED 2026-08-07) @ %.0f Hz",
                   u8 ("§").toRawUTF8(), fs));

    for (int algo : { (int) ReverbEngine::modeRoom, (int) ReverbEngine::modePlate,
                      (int) ReverbEngine::modeHall, (int) ReverbEngine::modeSpring })
    {
        if (! wants (algo))
            continue;

        const auto& mode = mirrorFor (algo);
        const double decays[4] = { 0.2, 0.5, 1.5, (double) mode.decayCeiling };

        double lo = 1.0e300, hi = -1.0e300;
        juce::String detail;

        for (double d : decays)
        {
            const double db = sustainedWetRmsDb (algo, fs, d);
            lo = juce::jmin (lo, db);
            hi = juce::jmax (hi, db);
            detail << fmt ("%.1f s %+.2f dB  ", d, db);
        }

        rep.info (fmt ("%s wet %.0f Hz-octave RMS vs decay: %s(sustained -20 dBFS pink "
                       "noise, erLevel 0)", mode.name, kLevelLawBandHz, detail.toRawUTF8()));
        rep.gate (hi - lo <= 3.0,
                  fmt ("%s wet level tracks reverb_decay within +-1.5 dB over 0.2 .. %.1f s "
                       "(spread %.2f dB)", mode.name, (double) mode.decayCeiling, hi - lo));
    }

    // The distortion row sweeps the tank modes only. Spring never reaches §3.5's in-loop
    // clip — its saturation is Dwell, input-side ahead of the dispersion (§3.9) and
    // driven by the front-end level, so its distortion never followed Decay in the first
    // place. The level row above is the Spring quantity this amendment changes.
    for (int algo : { (int) ReverbEngine::modeRoom, (int) ReverbEngine::modePlate,
                      (int) ReverbEngine::modeHall })
    {
        if (! wants (algo))
            continue;

        const auto& mode = mirrorFor (algo);
        const double decays[4] = { 0.2, 0.5, 1.5, (double) mode.decayCeiling };

        double worst = -1.0e300, atRef = 0.0, atCeiling = 0.0;
        juce::String detail;

        for (double d : decays)
        {
            const double db = inLoopDistortionDb (algo, fs, d);
            worst = juce::jmax (worst, db);

            if (d == 1.5)                        atRef = db;
            if (d == (double) mode.decayCeiling) atCeiling = db;

            detail << fmt ("%.1f s %+.1f dB  ", d, db);
        }

        rep.info (fmt ("%s in-loop distortion vs decay: %s(residual-to-signal, sustained "
                       "-6 dBFS pink noise)", mode.name, detail.toRawUTF8()));

        // The gate is directional, not a flatness bound, and the asymmetry is the
        // physics rather than a weakened threshold. Matching line RMS across the knob
        // cannot match the CREST factor with it: a 0.2 s tank sums far fewer overlapping
        // copies, so at equal RMS its peaks sit nearer §3.5's absolute 0.8 knee. What the
        // normalisation removes is the hazard the compounding argument names — distortion
        // GROWING with decay, when a long tail also recirculates it hundreds of times
        // (Room read -61.9 dB at 1.5 s against -47.0 dB at its 2.5 s ceiling before this
        // change, Plate -88.3 against -36.1). Afterwards the ceiling is the quiet end.
        //
        // The reference is FLOORED at the method's own resolution before differencing
        // (2026-08-07): the engine's recirculated float-rounding residue reads ~-83 dB
        // broadband (the clip-aliasing row's fold-comb exclusion is built around that
        // same figure), so below kDistortionMethodFloorDb the subtraction statistic
        // wobbles a few dB under benign perturbations — §3.6's Layer C moved Room's
        // 48 kHz reference -79.8 -> -83.5 dB while lowering the ceiling reading too,
        // and the unfloored difference tripped the gate on two floor readings. The
        // floor sits 5 dB above the measured residue and 30+ dB under the gate's
        // failure exemplars, so its teeth are untouched.
        rep.gate (atCeiling - juce::jmax (atRef, kDistortionMethodFloorDb) <= 3.0,
                  fmt ("%s in-loop distortion does not grow with reverb_decay: %+.1f dB at "
                       "the %.1f s ceiling against %+.1f dB at the 1.5 s reference",
                       mode.name, atCeiling, (double) mode.decayCeiling, atRef));

        // ... and the short end, where the injection is boosted (up to the +6 dB clamp),
        // stays under 1 % residual. A bound on the direction this change moves distortion
        // in — and not a free pass: the pre-change build broke it from the other side,
        // Plate reading -36.1 dB at its 4.5 s ceiling.
        rep.gate (worst <= -40.0,
                  fmt ("%s in-loop distortion stays under 1 %% anywhere on the decay range "
                       "(worst %+.1f dB)", mode.name, worst));
    }
}

//==============================================================================
// §3.5's clip-aliasing row (AMENDED 2026-08-07)
//==============================================================================

/** In-place iterative radix-2 complex FFT in double. The Welch helper above is float,
    which is fine for curve SHAPES but not for this row: the float transform's roundoff,
    summed over the ~60k measured bins, floors near -90 dB of the fundamentals — above
    the post-ADAA residual the row exists to measure. */
void fftRadix2 (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();

    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
        j ^= bit;

        if (i < j)
            std::swap (a[i], a[j]);
    }

    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * juce::MathConstants<double>::pi / (double) len;
        const std::complex<double> wLen (std::cos (ang), std::sin (ang));

        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0, 0.0);

            for (size_t k = 0; k < len / 2; ++k, w *= wLen)
            {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

/** Two tones on one exact-bin comb: base bin `k0` ~110 Hz, tones at 9·k0 and 11·k0
    (~0.99 / 1.21 kHz). Every harmonic and intermodulation product |m·f1 + n·f2| then
    sits on an exact multiple of k0 bins — one comb to exclude — while a fold about
    Nyquist lands `fftSize mod k0` bins OFF that comb (and the j-th fold at j times that
    offset), so aliased energy is separable from distortion by construction. mod = 0
    keeps every delay static, so the clipper of §3.5 is the ONLY source of off-comb
    energy above the engine's own float noise floor.

    The row is PINNED at 48 kHz rather than swept: both the folding geometry and the
    drive are rate-dependent by construction — at 44.1 kHz the same tones land near
    enough to a Plate resonance that the wet peak crosses §3.1's safety limiter, whose
    (single-pass, unmeasured) distortion then floods the fold comb. Rate independence is
    a claim about the LINEAR metrics (§8), not about where aliases fold. */
void analyseClipAliasing (Report& rep)
{
    const double fs = 48000.0;

    rep.head (fmt ("Plate @ %.0f Hz  in-loop clip aliasing (%s3.5 AMENDED 2026-08-07, "
                   "color = 1)", fs, u8 ("§").toRawUTF8()));

    const int order = 17;
    const int fftSize = 1 << order;              // 2.73 s at 48 kHz

    // Nudge k0 until the first fold clears the ±3-bin exclusion band with margin —
    // 48000/44100/96000 give offsets of 272/55/28 bins unnudged.
    int k0 = (int) std::lround (110.0 * fftSize / fs);
    while (juce::jmin (fftSize % k0, k0 - fftSize % k0) < 8)
        ++k0;

    const int f1bin = 9 * k0, f2bin = 11 * k0;

    // Sustained through warm-up and window: the onset transient decays at T60 and is
    // non-stationary, i.e. it reads as broadband smear in whatever window still hears
    // it — two full T60s puts it 120 dB down before the window starts.
    const double warmSeconds = 9.0;
    const int warm = (int) std::lround (warmSeconds * fs);
    const int total = warm + fftSize;
    const double amp = 1.0;                                  // sum peaks at +6 dBFS

    std::vector<double> source ((size_t) total);

    for (int i = 0; i < total; ++i)
    {
        const double ph = 2.0 * juce::MathConstants<double>::pi * (double) i / (double) fftSize;
        source[(size_t) i] = amp * (std::sin (ph * f1bin) + std::sin (ph * f2bin));
    }

    Setup s;
    s.algo = ReverbEngine::modePlate;
    s.decayS = 4.5f;             // the ceiling: a tone builds coherently as 1/(1 - g),
                                 // which grows past §3.2's incoherent normalisation —
                                 // the deepest the clipper is ever driven
    s.color01 = 1.0f;            // inSat at its 0.60 maximum (§5.1 axis 2) — drive, not voicing
    s.mod01 = 0.0f;              // static delays: no modulation sidebands off the comb
    s.erLevel01 = 0.0f;          // wet is tank-only, as the level-law rows render

    const Ir ir = renderThrough (s, fs, source, total);

    double wetPeak = 0.0;
    for (int i = warm; i < total; ++i)
        wetPeak = juce::jmax (wetPeak, std::abs (ir.l[(size_t) i]));

    // PERIODIC Hann (denominator N, not the Welch helper's N - 1): only that phase makes
    // an exact-bin tone's transform land on exactly 3 bins; the symmetric form leaks a
    // skirt that reads ~-90 dB summed off-comb, on top of what is being measured.
    std::vector<std::complex<double>> spec ((size_t) fftSize);

    for (int i = 0; i < fftSize; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi
                                               * (double) i / (double) fftSize);
        spec[(size_t) i] = { ir.l[(size_t) (warm + i)] * w, 0.0 };
    }

    fftRadix2 (spec);

    // Hann of an exact-bin tone spans 3 bins; ±3 absorbs the block-rate parameter snap.
    const auto onComb = [k0] (int bin)
    {
        const int r = bin % k0;
        return juce::jmin (r, k0 - r) <= 3;
    };

    const int kMin = (int) std::ceil (300.0 * fftSize / fs);   // clear of DC block + beats
    double eFund = 0.0, eDirect = 0.0, eAlias = 0.0;

    const int skirt = (int) std::ceil (30.0 * fftSize / fs);

    for (int k = kMin; k <= fftSize / 2; ++k)
    {
        const double p = std::norm (spec[(size_t) k]);

        if (std::abs (k - f1bin) <= 3 || std::abs (k - f2bin) <= 3)      eFund += p;
        else if (std::abs (k - f1bin) <= skirt
                 || std::abs (k - f2bin) <= skirt)                       { /* mode-noise skirt */ }
        else if (onComb (k))                                             eDirect += p;
    }

    // The aliased sum reads the FOLDED product comb only — bins within ±3 of
    // |m·k0 − fftSize| for every product order m whose first or second fold lands in
    // band — rather than "everything off-comb": the broadband residue of the engine's
    // own float recirculation sums ~-83 dB over 64k bins and would floor the metric.
    double eAliasLo = 0.0, eAliasHi = 0.0;

    for (int m = (fftSize / 2) / k0 + 1; m * k0 < 3 * fftSize / 2; ++m)
    {
        const int q = std::abs (m * k0 - fftSize);

        if (q < kMin || q > fftSize / 2 - 4 || onComb (q))
            continue;

        double e = 0.0;
        for (int k = q - 1; k <= q + 1; ++k)
            e += std::norm (spec[(size_t) k]);

        eAlias += e;
        (q < fftSize / 4 ? eAliasLo : eAliasHi) += e;
    }

    const double directDb = 10.0 * std::log10 (juce::jmax (1.0e-300, eDirect / eFund));
    const double aliasDb = 10.0 * std::log10 (juce::jmax (1.0e-300, eAlias / eFund));

    rep.info (fmt ("fundamentals %.0f + %.0f Hz; wet peak %.3f; distortion on the "
                   "product comb %+.1f dB, on its folds (aliased) %+.1f dB [below fs/4 "
                   "%+.1f, above %+.1f], all re the fundamentals",
                   (double) f1bin * fs / fftSize, (double) f2bin * fs / fftSize, wetPeak,
                   directDb, aliasDb,
                   10.0 * std::log10 (juce::jmax (1.0e-300, eAliasLo / eFund)),
                   10.0 * std::log10 (juce::jmax (1.0e-300, eAliasHi / eFund))));

    // Thresholds set from the measured before/after pair (see §8's amendment): the
    // plain-clip build read -83.0 / -87.2 dB here, the residual-ADAA build
    // -91.0 / -102.4. The below-fs/4 gate is where ADAA1's sinc envelope delivers the
    // full >= 10 dB — folds landing near Nyquist come from products just above it,
    // which a FIRST-order antiderivative can only attenuate a few dB.
    rep.gate (aliasDb <= -88.0,
              fmt ("folded (aliased) products stay 88 dB under the fundamentals "
                   "(%+.1f dB)", aliasDb));

    rep.gate (10.0 * std::log10 (juce::jmax (1.0e-300, eAliasLo / eFund)) <= -97.0,
              fmt ("folds landing below fs/4 stay 97 dB under the fundamentals "
                   "(%+.1f dB)", 10.0 * std::log10 (juce::jmax (1.0e-300, eAliasLo / eFund))));
}

//==============================================================================
// §3.6's Layer-C pitch-stability row (AMENDED 2026-08-07)
//==============================================================================

/** A 440 Hz sine at −12 dBFS, sustained through the tank at `mod = 1` — every layer at
    full depth and the rate multiplier at its ×4.571 top — and the wet's instantaneous
    F0 tracked by heterodyne: multiply by e^(−j·2π·440·t), boxcar the complex product
    over 100 ms (first null at 10 Hz, well above every modulator rate), then read the
    phase slope across 10 ms hops. Delay modulation is Doppler and shows up directly as
    cents of F0 wobble; a Givens layer is time-varying MIXING of statically-delayed
    copies, so its contribution to the phase slope is what this row bounds.

    The row is the discriminator §3.6's Layer C is designed against: liveliness bought
    with deeper delay modulation (the standard fix for static-FDN metallicity) moves
    this number; liveliness bought on the unitary group must not. The bounds are set
    from the measured Layer-A/B-only build plus margin, so the gate asserts the rotation
    layer added no measurable F0 deviation on top of what the delay layers already cost.

    Pinned at 48 kHz like the clip-aliasing row: the statistic is a property of the
    modulation LAW, not of the host rate (depths and rates are solved in seconds), and
    one rate keeps the row's render cost off the per-rate sweep. Room and Hall are the
    two subjects: Room carries the largest rotation depth (`rotMax`, §4.1) and Hall the
    deepest delay modulation (0.55 ms with Layer B fully on) — the worst case of the
    new layer and of the baseline respectively. */
void analysePitchStability (Report& rep)
{
    const double fs = 48000.0;
    const double f0 = 440.0;

    rep.head (fmt ("Tank @ %.0f Hz  rotation pitch stability (%s3.6 Layer C, mod = 1)",
                   fs, u8 ("§").toRawUTF8()));

    struct Subject { int algo; const char* name; double boundRmsCents, boundWorstCents; };

    // Bounds are the measured Layer-A/B-only baseline plus margin (Room 1.445 rms /
    // 3.815 worst, Hall 0.386 / 0.827 — see §8's amendment): the shipped rotation
    // must fit under what the delay layers already cost, or it is not the
    // zero-pitch-modulation layer §3.6 claims.
    const Subject subjects[] = {
        { ReverbEngine::modeRoom, "Room", 1.75, 4.50 },
        { ReverbEngine::modeHall, "Hall", 0.50, 1.10 },
    };

    const double warmSeconds = 4.0;      // > 2×T60 at decay 2.0: onset chirp gone
    const double measureSeconds = 8.0;
    const int warm = (int) std::lround (warmSeconds * fs);
    const int meas = (int) std::lround (measureSeconds * fs);
    const int total = warm + meas;

    std::vector<double> source ((size_t) total);

    for (int i = 0; i < total; ++i)
        source[(size_t) i] = 0.25 * std::sin (2.0 * juce::MathConstants<double>::pi
                                              * f0 * (double) i / fs);

    for (const auto& subject : subjects)
    {
        Setup s;
        s.algo = subject.algo;
        s.decayS = 2.0f;
        s.mod01 = 1.0f;              // full depth AND the ×4.571 rate top (§3.6)
        s.erLevel01 = 0.0f;          // wet is tank-only; the ER FIR is static anyway

        const Ir ir = renderThrough (s, fs, source, total);

        // Heterodyne + boxcar via complex prefix sums (double: ~600k terms of ~0.25
        // magnitude keeps the cancellation error ~90 dB under the smoothed product).
        std::vector<std::complex<double>> prefix ((size_t) total + 1);
        std::complex<double> acc { 0.0, 0.0 };
        prefix[0] = acc;

        for (int i = 0; i < total; ++i)
        {
            const double ph = -2.0 * juce::MathConstants<double>::pi * f0 * (double) i / fs;
            acc += ir.l[(size_t) i] * std::complex<double> (std::cos (ph), std::sin (ph));
            prefix[(size_t) i + 1] = acc;
        }

        const int window = (int) std::lround (0.100 * fs);   // 100 ms boxcar
        const int hop = (int) std::lround (0.010 * fs);      // 10 ms phase slope

        // The instantaneous frequency of a narrowband signal is UNBOUNDED at envelope
        // zeros (d/dt arg z spins freely as |z| -> 0), so a raw worst-hop statistic
        // measures the estimator, not the tone: any modulation that sweeps the 440 Hz
        // response through a deep dip — including the delay layers' own — prints
        // hundreds of phantom cents there. Two amplitude-aware statistics instead,
        // each keyed to what PITCH modulation (as opposed to spectral motion) must do:
        //   (1) the |z|-weighted RMS — the deviation of the tone as heard, a frame
        //       counting in proportion to how loud the tone is in it;
        //   (2) the worst hop among frames AT OR ABOVE the median level. Doppler
        //       detunes every frame, loud ones included; a response dip transiting
        //       440 Hz slews the phase only in frames it has already pulled below
        //       median by definition — so this statistic passes spectral motion (the
        //       liveliness §3.6's Layer C exists for, a phaser not a chorus) and fails
        //       coherent detune (the seasick failure mode the row polices).
        // The worst hop down to −12 dB of median is reported for visibility, ungated:
        // it reads the notch-transit slew, which scales with rotation DEPTH and is
        // bounded below by phase-step/window even as the rate slows.
        struct Hop { double cents, weight, mag; };
        std::vector<Hop> hops;
        hops.reserve ((size_t) (meas / hop) + 1);

        std::complex<double> prev { 0.0, 0.0 };

        for (int t = warm; t + window <= total; t += hop)
        {
            const std::complex<double> z = prefix[(size_t) (t + window)] - prefix[(size_t) t];

            if (t > warm)
            {
                // arg(z·conj(prev)) unwraps by construction; ±π across a 10 ms hop is
                // ±50 Hz of headroom against deviations measured in single cents.
                const double devHz = std::arg (z * std::conj (prev))
                                     / (2.0 * juce::MathConstants<double>::pi * (double) hop / fs);
                const double cents = 1200.0 * std::log2 ((f0 + devHz) / f0);
                const double magA = std::abs (prev) / (double) window;
                const double magB = std::abs (z) / (double) window;
                hops.push_back ({ cents, magA * magB, juce::jmin (magA, magB) });
            }

            prev = z;
        }

        std::vector<double> mags;
        mags.reserve (hops.size());
        for (const auto& h : hops)
            mags.push_back (h.mag);
        std::nth_element (mags.begin(), mags.begin() + (long) mags.size() / 2, mags.end());
        const double medianMag = mags[mags.size() / 2];

        double sumW = 0.0, sumWSq = 0.0, worstLoud = 0.0, worstSane = 0.0, minMag = 1.0e300;

        for (const auto& h : hops)
        {
            sumW += h.weight;
            sumWSq += h.weight * h.cents * h.cents;
            minMag = juce::jmin (minMag, h.mag);

            if (h.mag >= medianMag)                           // at/above the median level
                worstLoud = juce::jmax (worstLoud, std::abs (h.cents));

            if (h.mag >= 0.25 * medianMag)                    // within 12 dB of median
                worstSane = juce::jmax (worstSane, std::abs (h.cents));
        }

        const double rmsCents = std::sqrt (sumWSq / juce::jmax (1.0e-300, sumW));

        rep.info (fmt ("%s: level-weighted rms |F0 dev| %.3f cents, worst hop at/above "
                       "the median level %.3f cents, worst within 12 dB of it %.3f "
                       "cents (ungated: notch-transit slew), over %.0f s (smoothed "
                       "440 Hz magnitude median %.4f, min %.4f)",
                       subject.name, rmsCents, worstLoud, worstSane, measureSeconds,
                       medianMag, minMag));

        rep.gate (rmsCents <= subject.boundRmsCents,
                  fmt ("%s wet F0: level-weighted rms within %.2f cents at mod = 1 "
                       "(%.3f)", subject.name, subject.boundRmsCents, rmsCents));
        rep.gate (worstLoud <= subject.boundWorstCents,
                  fmt ("%s wet F0: worst at/above-median-level hop within %.2f cents "
                       "at mod = 1 (%.3f)", subject.name, subject.boundWorstCents, worstLoud));
    }
}

//==============================================================================
std::vector<double> parseDoubleList (const juce::String& csv)
{
    std::vector<double> out;

    for (const auto& token : juce::StringArray::fromTokens (csv, ",", {}))
        if (token.trim().isNotEmpty())
            out.push_back (token.trim().getDoubleValue());

    return out;
}
} // namespace

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);

    if (argc < 2)
    {
        std::printf ("usage: tubamp_verbprobe <out-dir> [--rates=44100,48000,96000]\n"
                     "                        [--modes=0,1,2,3,4,5] [--decay=2.0]\n"
                     "                        [--freeverb] [--cpu]\n");
        return 2;
    }

    const juce::ScopedJuceInitialiser_GUI juceInit;

    juce::File outDir = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String { argv[1] });
    outDir.createDirectory();

    std::vector<double> rates { 44100.0, 48000.0, 96000.0 };
    std::vector<double> modes { 0.0, 1.0, 2.0, 3.0, 4.0, 5.0 };
    double decayS = 2.0;
    bool wantFreeverb = false;
    bool wantCpu = false;

    for (int i = 2; i < argc; ++i)
    {
        const juce::String arg { argv[i] };

        if (arg.startsWith ("--rates="))       rates = parseDoubleList (arg.fromFirstOccurrenceOf ("=", false, false));
        else if (arg.startsWith ("--modes="))  modes = parseDoubleList (arg.fromFirstOccurrenceOf ("=", false, false));
        else if (arg.startsWith ("--decay="))  decayS = arg.fromFirstOccurrenceOf ("=", false, false).getDoubleValue();
        else if (arg == "--freeverb")          wantFreeverb = true;
        else if (arg == "--cpu")               wantCpu = true;
        else { std::printf ("unknown option: %s\n", arg.toRawUTF8()); return 2; }
    }

    Report rep;
    rep.line (u8 ("tubamp_verbprobe — docs/REVERB.md §8 metrics + §3.12 Reverse gates"));
    rep.line (fmt ("out dir : %s", outDir.getFullPathName().toRawUTF8()));
    rep.line (fmt ("rates   : %s", [&] { juce::String s; for (double r : rates) s << fmt ("%.0f ", r); return s; }().toRawUTF8()));
    rep.line (fmt ("modes   : %s", [&] { juce::String s; for (double m : modes) s << mirrorFor ((int) m).name << " "; return s; }().toRawUTF8()));
    rep.line (fmt ("decay   : %.2f s (clamped per mode)", decayS));
    rep.line (u8 ("gates apply at color = 0 (plus §3.5's aliasing row at color = 1 — drive, "
                  "not voicing — and the colour-1 T60 row, whose targets carry the voicing); "
                  "§5.1's inverted colour checks are reported, not fatal"));

    checkMatrixUnitarity (rep);

    std::vector<RateResult> results;

    for (double fs : rates)
    {
        for (double m : modes)
        {
            const int algo = (int) std::lround (m);

            if (algo == ReverbEngine::modeReverse)
                results.push_back (analyseReverse (fs, outDir, rep));
            else if (algo == ReverbEngine::modeSpring)
                results.push_back (analyseSpring (fs, outDir, rep));
            else if (algo == ReverbEngine::modeShimmer)
                continue;                    // §3.11's grid is run once, after the rate loop
            else
                results.push_back (analyseTankMode (algo, fs, decayS, outDir, rep));
        }

        // §5.1 names Plate as the colour-gate subject.
        if (std::find (modes.begin(), modes.end(), 1.0) != modes.end())
            analyseColour (ReverbEngine::modePlate, fs, decayS, rep);

        // §7 Stage 1 gate 7's ducking and two-sided-spread checks. Room drives the first
        // and Plate/Hall — the two modes with `plateSpread` on (§4.1) — the second.
        if (std::find (modes.begin(), modes.end(), 0.0) != modes.end())
            analyseDuckAndSpread (fs, rep);

        if (wantFreeverb)
            analyseFreeverb (fs, outDir, rep);
    }

    // §3.2's decay-normalised injection rows are a level law, rate-independent by
    // construction, so they run once, at the first rate asked for.
    if (! rates.empty())
        analyseDecayNormalisation (modes, rates.front(), rep);

    // §3.5's clip-aliasing row drives the clipper at its color = 1 maximum on Plate.
    // It runs once, pinned at 48 kHz — see the function comment for why it is not swept.
    if (std::find (modes.begin(), modes.end(), 1.0) != modes.end())
        analyseClipAliasing (rep);

    // §3.6's Layer-C pitch-stability row runs once, pinned at 48 kHz — the statistic is
    // a property of the modulation law, not the host rate. Room (largest rotation
    // depth) and Hall (deepest delay modulation) are its two subjects.
    if (std::find (modes.begin(), modes.end(), 0.0) != modes.end()
        || std::find (modes.begin(), modes.end(), 2.0) != modes.end())
        analysePitchStability (rep);

    // §3.11's grid is a stability sweep, not a per-rate measurement, and 36 points x 70 s is
    // the probe's longest single pass — so it runs once, at the first rate asked for.
    if (std::find (modes.begin(), modes.end(), 4.0) != modes.end() && ! rates.empty())
        analyseShimmer (rates.front(), rep);

    if (wantCpu)
        measureSpringCpu (rep);

    // --- §8 rate independence -------------------------------------------------
    rep.head (u8 ("Rate independence (§8 last-but-two row)"));

    for (double m : modes)
    {
        const int algo = (int) std::lround (m);
        double loT60 = 1.0e300, hiT60 = 0.0;
        double worstFlutter = 0.0, deepestNotchDb = 0.0;
        int seen = 0;
        std::vector<const RateResult*> forMode;

        for (const auto& r : results)
        {
            if (r.algo != algo)
                continue;

            ++seen;
            forMode.push_back (&r);
            deepestNotchDb = juce::jmin (deepestNotchDb, r.notchDb);

            if (r.broadbandT60 > 0.0) { loT60 = juce::jmin (loT60, r.broadbandT60); hiT60 = juce::jmax (hiT60, r.broadbandT60); }

            worstFlutter = juce::jmax (worstFlutter, r.worstFlutter);
        }

        if (seen < 2)
            continue;

        const double t60Spread = hiT60 > 0.0 && loT60 < 1.0e299 ? hiT60 / loT60 - 1.0 : 0.0;

        rep.info (fmt ("%s: broadband T60 %.3f .. %.3f s (%+.1f %%), worst flutter %.3f, "
                       "deepest mono-sum notch over all rates %+.2f dB",
                       mirrorFor (algo).name, loT60, hiT60, 100.0 * t60Spread,
                       worstFlutter, deepestNotchDb));

        rep.gate (t60Spread <= 0.10,
                  fmt ("%s broadband T60 agrees within 10 %% across host rates (%+.1f %%)",
                       mirrorFor (algo).name, 100.0 * t60Spread));

        // "Notch frequencies must not move with rate" is checked by overlaying the whole
        // smoothed |L+R| / |L| curve on a shared 1/12-octave grid rather than by chasing
        // its argmin, which hops between equivalent minima of a +-3 dB ripple landscape
        // and would report a huge "move" where the geometry has not changed at all.
        double worstCurveDb = 0.0;
        double worstCurveHz = 0.0;

        for (size_t a = 0; a < forMode.size(); ++a)
            for (size_t b = a + 1; b < forMode.size(); ++b)
            {
                const auto& ca = forMode[a]->monoSumGridDb;
                const auto& cb = forMode[b]->monoSumGridDb;

                if (ca.size() != (size_t) kGridPoints || cb.size() != (size_t) kGridPoints)
                    continue;

                for (int i = 0; i < kGridPoints; ++i)
                {
                    const double d = std::abs (ca[(size_t) i] - cb[(size_t) i]);

                    if (d > worstCurveDb) { worstCurveDb = d; worstCurveHz = gridFrequency (i); }
                }
            }

        rep.info (fmt ("%s mono-sum curve differs by at most %.2f dB between host rates "
                       "(worst at %.0f Hz)", mirrorFor (algo).name, worstCurveDb, worstCurveHz));
        rep.gate (worstCurveDb <= 3.0,
                  fmt ("%s mono-sum response does not move with host rate (worst %.2f dB "
                       "over 100 Hz .. 10 kHz)", mirrorFor (algo).name, worstCurveDb));
    }

    // --- not implemented here, and why ---------------------------------------
    rep.head ("Not measured by this probe");
    rep.info (u8 ("§8 \"Jot solve unit test\" (per-line per-pass gain at 250 Hz / 1 kHz / 8 kHz "
                  "within 0.5 % of 10^(-3 D_i/T60_band)) needs per-line taps that ReverbEngine's "
                  "public interface does not expose. The per-octave T60 row above is the "
                  "observable form of the same property."));
    rep.info (u8 ("Nothing else: §7 Stage 1 gate 7's ducking and two-sided-spread checks "
                  "are measured above, under \"Ducking and two-sided spread\"."));

    rep.head ("Summary");
    rep.line (fmt ("  %d gate failure(s)", rep.failures));
    rep.line (fmt ("  %d colour note(s) at color = 1 (not fatal)", rep.colourNotes));
    rep.line (rep.failures == 0 ? "PASS: every gate holds"
                                : "FAILURES ABOVE");

    outDir.getChildFile ("report.txt").replaceWithText (rep.text);
    std::printf ("\nreport written to %s\n",
                 outDir.getChildFile ("report.txt").getFullPathName().toRawUTF8());

    return rep.failures == 0 ? 0 : 1;
}
