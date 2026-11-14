#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <memory>
#include <vector>

namespace tubamp
{
/**
    Magnitude spectrum on 256 log-spaced points, for the EQ block's live analyser
    (docs/FX-ENGINES.md §2.8.1). Message thread only: prepare() allocates.

    Point i sits at 20 * 1000^(i / 255) Hz, 20 Hz to 20 kHz. The FFT is sized so a bin is
    about 11.7 Hz wide at any rate at the default Resolution (4096 up to 50 kHz, 8192 up to
    100 kHz, 16384 above); Low halves the FFT and High doubles it.
    Where a point's band (half a point spacing either side, in log frequency) holds one or
    more bin centres, the point reads the loudest of them; below that crossover (about
    430 Hz) it interpolates, in dB, between the two bins around its frequency. Points
    above Nyquist read the floor.

    Levels are dBFS: the Hann window is scaled so a full-scale sine on a bin centre reads
    0 dB. A sine between bins loses up to 1.42 dB to the window's scalloping, so a bin
    that is a local peak is corrected by quadratic interpolation over its neighbours (in
    dB), capped at that 1.42 dB; a full-scale sine then reads 0 dB at its point wherever
    it falls in the band-per-bin range.
*/
class SpectrumAnalyser
{
public:
    static constexpr int kNumPoints = 256;
    static constexpr float kFloorDb = -120.0f;

    /** Frequency of point i. */
    static double pointFrequency (int i) noexcept;

    /** The wire encoding of the eqSpectrum event: round((dB + 120) * 2), clamped to 0..240. */
    static int encode (float db) noexcept;

    /** Resolution choices, as the analyser's Resolution control offers them: the FFT is
        2048, 4096 or 8192 points at 44.1 / 48 kHz (Logic's Channel EQ offers the same
        three; docs/research/eq-analyser.md), doubled at 88.2 / 96 kHz and again above so
        a choice keeps its bin width. More points resolve the bass better and take longer
        to respond. */
    enum Resolution { low = 0, medium = 1, high = 2 };
    static constexpr int kNumResolutions = 3;
    static constexpr int kDefaultResolution = medium;

    /** FFT order for a rate and a resolution (11 to 15); out-of-range resolutions clamp. */
    static int orderFor (double sampleRate, int resolution = kDefaultResolution) noexcept;

    void prepare (double sampleRate, int resolution = kDefaultResolution);

    double getSampleRate() const noexcept { return rate; }
    int getResolution() const noexcept { return resolution; }

    /** Samples analyse() reads; 0 before prepare(). */
    int getFftSize() const noexcept { return fftSize; }

    /** `samples` holds getFftSize() samples, oldest first. dbOut is in dBFS, floor -120,
        not clamped above (a signal over full scale reads over 0). */
    void analyse (const float* samples, std::array<float, kNumPoints>& dbOut) noexcept;

private:
    struct Point
    {
        bool band = false;    // true: max over [lo, hi]; false: interpolate lo..lo+1 at frac
        bool aboveNyquist = false;
        int lo = 0, hi = 0;
        float frac = 0.0f;
    };

    double rate = 0.0;
    int resolution = kDefaultResolution;
    int fftSize = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, work, binDb;
    std::array<Point, kNumPoints> points {};
};
} // namespace tubamp
