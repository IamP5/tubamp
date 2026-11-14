#include "SpectrumAnalyser.h"

#include <algorithm>
#include <cmath>

namespace tubamp
{
namespace
{
    // Hann's worst-case scalloping loss (a sine half-way between two bins).
    constexpr float kMaxScallopDb = 1.4236f;
}

double SpectrumAnalyser::pointFrequency (int i) noexcept
{
    return 20.0 * std::pow (1000.0, (double) i / (double) (kNumPoints - 1));
}

int SpectrumAnalyser::encode (float db) noexcept
{
    if (! std::isfinite (db))
        return db > 0.0f ? 240 : 0;

    return juce::jlimit (0, 240, (int) std::lround (((double) db + 120.0) * 2.0));
}

int SpectrumAnalyser::orderFor (double sampleRate, int resolutionChoice) noexcept
{
    // 2048 / 4096 / 8192 points at the base rates, one octave more per rate doubling.
    const int base = 11 + juce::jlimit (0, kNumResolutions - 1, resolutionChoice);

    if (sampleRate <= 50000.0)
        return base;

    return sampleRate <= 100000.0 ? base + 1 : base + 2;
}

void SpectrumAnalyser::prepare (double sampleRate, int resolutionChoice)
{
    rate = sampleRate > 0.0 ? sampleRate : 44100.0;
    resolution = juce::jlimit (0, kNumResolutions - 1, resolutionChoice);
    const int order = orderFor (rate, resolution);
    fftSize = 1 << order;
    fft = std::make_unique<juce::dsp::FFT> (order);

    // Periodic Hann, scaled by 2 / sum(w): a unit-amplitude sine on a bin centre then has
    // a magnitude of exactly 1 in that bin.
    window.assign ((size_t) fftSize, 0.0f);
    double sum = 0.0;

    for (int n = 0; n < fftSize; ++n)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * n / fftSize);
        window[(size_t) n] = (float) w;
        sum += w;
    }

    for (auto& w : window)
        w = (float) (w * 2.0 / sum);

    work.assign ((size_t) fftSize * 2, 0.0f);
    binDb.assign ((size_t) fftSize / 2 + 1, kFloorDb);

    const int nyquistBin = fftSize / 2;
    const double binHz = rate / fftSize;
    const double halfStep = std::pow (1000.0, 0.5 / (double) (kNumPoints - 1));

    for (int i = 0; i < kNumPoints; ++i)
    {
        auto& p = points[(size_t) i];
        const double f = pointFrequency (i);
        p = {};

        if (f > rate * 0.5)
        {
            p.aboveNyquist = true;
            continue;
        }

        const int lo = (int) std::ceil ((f / halfStep) / binHz);
        const int hi = std::min (nyquistBin, (int) std::floor ((f * halfStep) / binHz));

        if (lo <= hi)
        {
            p.band = true;
            p.lo = std::max (lo, 1);
            p.hi = std::max (hi, p.lo);
        }
        else
        {
            const double k = f / binHz;
            p.lo = std::min ((int) std::floor (k), nyquistBin - 1);
            p.frac = (float) (k - p.lo);
        }
    }
}

void SpectrumAnalyser::analyse (const float* samples, std::array<float, kNumPoints>& dbOut) noexcept
{
    if (fft == nullptr)
    {
        dbOut.fill (kFloorDb);
        return;
    }

    std::fill (work.begin(), work.end(), 0.0f);

    for (int n = 0; n < fftSize; ++n)
        work[(size_t) n] = samples[n] * window[(size_t) n];

    fft->performFrequencyOnlyForwardTransform (work.data(), true);

    const int nyquistBin = fftSize / 2;
    const float minMag = std::pow (10.0f, kFloorDb / 20.0f);

    for (int k = 0; k <= nyquistBin; ++k)
        binDb[(size_t) k] = 20.0f * std::log10 (std::max (work[(size_t) k], minMag));

    for (int i = 0; i < kNumPoints; ++i)
    {
        const auto& p = points[(size_t) i];
        float db = kFloorDb;

        if (p.aboveNyquist)
        {
            db = kFloorDb;
        }
        else if (p.band)
        {
            int peak = p.lo;

            for (int k = p.lo + 1; k <= p.hi; ++k)
                if (binDb[(size_t) k] > binDb[(size_t) peak])
                    peak = k;

            db = binDb[(size_t) peak];

            // A local peak: estimate the level between bins (quadratic in dB), never more
            // than the window can lose, so noise cannot be lifted past that.
            if (peak > 0 && peak < nyquistBin)
            {
                const float a = binDb[(size_t) peak - 1], b = db, c = binDb[(size_t) peak + 1];
                const float curve = a - 2.0f * b + c;

                if (b >= a && b >= c && curve < 0.0f)
                {
                    const float offset = 0.5f * (a - c) / curve;
                    db += juce::jlimit (0.0f, kMaxScallopDb, -0.25f * (a - c) * offset);
                }
            }
        }
        else
        {
            const float a = binDb[(size_t) p.lo], b = binDb[(size_t) p.lo + 1];
            db = a + (b - a) * p.frac;
        }

        dbOut[(size_t) i] = std::max (db, kFloorDb);
    }
}
} // namespace tubamp
