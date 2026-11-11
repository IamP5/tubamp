// Numeric model of tubamp's ToneStackEQ (src/dsp/FxBlocks.cpp:128-197) for study.
// Coefficient formulas copied in structure from juce_IIRFilter.cpp ArrayCoefficients<float>
// (makeLowShelf / makePeakFilter / makeHighShelf, float arithmetic, normalised by a0 in float),
// filter structure from juce_IIRFilter_Impl.h order-2 case (transposed direct form II, float state).
//
// build: clang++ -O2 -std=c++17 ours.cpp -o ours
// usage: ours cramp | prec | zip <outdir>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

template <typename T> struct Co { T b0, b1, b2, a1, a2; };

template <typename T> Co<T> norm (T b0, T b1, T b2, T a0, T a1, T a2)
{
    const T inv = T (1) / a0;   // juce Coefficients::assignImpl multiplies by 1/a0
    return { b0 * inv, b1 * inv, b2 * inv, a1 * inv, a2 * inv };
}
template <typename T> Co<T> lowShelf (double fs, T f, T Q, T gain)
{
    const T A = std::sqrt (gain), am = A - 1, ap = A + 1;
    const T w = (2 * T (M_PI) * f) / T (fs);
    const T c = std::cos (w), beta = std::sin (w) * std::sqrt (A) / Q, amc = am * c;
    return norm<T> (A * (ap - amc + beta), A * 2 * (am - ap * c), A * (ap - amc - beta), ap + amc + beta, -2 * (am + ap * c), ap + amc - beta);
}
template <typename T> Co<T> highShelf (double fs, T f, T Q, T gain)
{
    const T A = std::sqrt (gain), am = A - 1, ap = A + 1;
    const T w = (2 * T (M_PI) * f) / T (fs);
    const T c = std::cos (w), beta = std::sin (w) * std::sqrt (A) / Q, amc = am * c;
    return norm<T> (A * (ap + amc + beta), A * -2 * (am + ap * c), A * (ap + amc - beta), ap - amc + beta, 2 * (am - ap * c), ap - amc - beta);
}
template <typename T> Co<T> peak (double fs, T f, T Q, T gain)
{
    const T A = std::sqrt (gain);
    const T w = (2 * T (M_PI) * f) / T (fs);
    const T alpha = std::sin (w) / (Q * 2), c2 = -2 * std::cos (w);
    return norm<T> (1 + alpha * A, c2, 1 - alpha * A, 1 + alpha / A, c2, 1 - alpha / A);
}
template <typename T> double magDb (const Co<T>& k, double f, double fs)
{
    const std::complex<double> z = std::exp (std::complex<double> (0, -2 * M_PI * f / fs));
    const auto n = (double) k.b0 + (double) k.b1 * z + (double) k.b2 * z * z;
    const auto d = 1.0 + (double) k.a1 * z + (double) k.a2 * z * z;
    return 20 * std::log10 (std::abs (n / d));
}
static float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }

// analog prototypes (RBJ cookbook), x = f/f0
static double anaHighShelf (double f, double f0, double Q, double gainDb)
{
    const double A = std::pow (10.0, gainDb / 40.0); const std::complex<double> s (0, f / f0);
    return 20 * std::log10 (std::abs (A * (A * s * s + (std::sqrt (A) / Q) * s + 1.0) / (s * s + (std::sqrt (A) / Q) * s + A)));
}
static double anaPeak (double f, double f0, double Q, double gainDb)
{
    const double A = std::pow (10.0, gainDb / 40.0); const std::complex<double> s (0, f / f0);
    return 20 * std::log10 (std::abs ((s * s + (A / Q) * s + 1.0) / (s * s + s / (A * Q) + 1.0)));
}

template <typename T> struct TDF2
{
    T s1 = 0, s2 = 0;
    inline T tick (T x, const Co<T>& k)
    {
        const T y = x * k.b0 + s1;
        s1 = (x * k.b1) - (y * k.a1) + s2;
        s2 = (x * k.b2) - (y * k.a2);
        return y;
    }
};
// Simper/Zavalishin trapezoidal SVF, low shelf form
template <typename T> struct SVFLowShelf
{
    T ic1 = 0, ic2 = 0, a1 = 0, a2 = 0, a3 = 0, m1 = 0, m2 = 0;
    void set (double fs, double f, double Q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0), g = std::tan (M_PI * f / fs) / std::sqrt (A), k = 1.0 / Q;
        const double x1 = 1.0 / (1.0 + g * (g + k));
        a1 = (T) x1; a2 = (T) (g * x1); a3 = (T) (g * g * x1); m1 = (T) (k * (A - 1)); m2 = (T) (A * A - 1);
    }
    inline T tick (T v0)
    {
        const T v3 = v0 - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
        return v0 + m1 * v1 + m2 * v2;
    }
};

static void cramp()
{
    const double grid[] = { 1000, 2000, 5000, 8000, 10000, 12000, 15000, 18000, 20000 };
    std::printf ("# digital (float coefficients, as shipped) minus analog prototype, dB\n");
    std::printf ("case\tfs\t1000\t2000\t5000\t8000\t10000\t12000\t15000\t18000\t20000\tmax_abs_20-20k\n");
    struct Case { const char* name; int type; float f, q, db; };
    const Case cases[] = { { "treble shelf 1800 Hz Q0.707 +10 dB (knob 10)", 0, 1800, 0.707f, 10 }, { "treble shelf 1800 Hz Q0.707 -10 dB (knob 0)", 0, 1800, 0.707f, -10 },
                           { "middle peak 425 Hz Q0.7 +15 dB (knob 10)", 1, 425, 0.7f, 15 },
                           { "hypothetical presence peak 4 kHz Q1 +12 dB", 1, 4000, 1.0f, 12 }, { "hypothetical bell 8 kHz Q1 +12 dB", 1, 8000, 1.0f, 12 },
                           { "hypothetical air shelf 8 kHz Q0.707 +10 dB", 0, 8000, 0.707f, 10 } };
    for (auto& c : cases)
        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            const auto k = c.type == 0 ? highShelf<float> (fs, c.f, c.q, dbToGain (c.db)) : peak<float> (fs, c.f, c.q, dbToGain (c.db));
            std::printf ("%s\t%.0f", c.name, fs);
            for (double f : grid)
                std::printf ("\t%.2f", magDb (k, f, fs) - (c.type == 0 ? anaHighShelf (f, c.f, c.q, c.db) : anaPeak (f, c.f, c.q, c.db)));
            double mx = 0;
            for (double f = 20; f <= 20000; f *= 1.01)
                mx = std::max (mx, std::abs (magDb (k, f, fs) - (c.type == 0 ? anaHighShelf (f, c.f, c.q, c.db) : anaPeak (f, c.f, c.q, c.db))));
            std::printf ("\t%.2f\n", mx);
        }
}

static void prec()
{
    std::printf ("# bass low shelf 150 Hz Q0.707: single precision as shipped against double precision reference\n");
    std::printf ("# coef_err = max |dB(float coefficients) - dB(double coefficients)| over 5 Hz..5 kHz\n");
    std::printf ("# noise = RMS of (float TDF-II output - double TDF-II output, same coefficients), in dB relative to the output RMS; input sine\n");
    std::printf ("gain_dB\tfs\tcoef_err_dB\tin_freq\tin_dBFS\tTDF2_float_noise_dBc\tSVF_float_noise_dBc\n");
    for (float db : { 20.0f, -20.0f, 4.0f })
        for (double fs : { 44100.0, 96000.0, 192000.0 })
        {
            const auto kf = lowShelf<float> (fs, 150.0f, 0.707f, dbToGain (db));
            const auto kd = lowShelf<double> (fs, 150.0, 0.707, std::pow (10.0, db / 20.0));
            double ce = 0;
            for (double f = 5; f <= 5000; f *= 1.01) ce = std::max (ce, std::abs (magDb (kf, f, fs) - magDb (kd, f, fs)));
            const Co<double> kfd { kf.b0, kf.b1, kf.b2, kf.a1, kf.a2 };
            for (double fin : { 82.4, 1000.0 })
                for (double lev : { -20.0, -60.0 })
                {
                    TDF2<float> a; TDF2<double> b; SVFLowShelf<float> sf; SVFLowShelf<double> sd;
                    sf.set (fs, 150, 0.707, db); sd.set (fs, 150, 0.707, db);
                    const int n = (int) (fs * 3); double e2 = 0, y2 = 0, es2 = 0, ys2 = 0;
                    const double amp = std::pow (10.0, lev / 20.0);
                    for (int i = 0; i < n; ++i)
                    {
                        const float x = (float) (amp * std::sin (2 * M_PI * fin * i / fs));
                        const double yf = a.tick (x, kf), yd = b.tick ((double) x, kfd);
                        const double zf = sf.tick (x), zd = sd.tick ((double) x);
                        if (i > n / 3) { e2 += (yf - yd) * (yf - yd); y2 += yd * yd; es2 += (zf - zd) * (zf - zd); ys2 += zd * zd; }
                    }
                    std::printf ("%.0f\t%.0f\t%.4f\t%.1f\t%.0f\t%.1f\t%.1f\n", db, fs, ce, fin, lev, 10 * std::log10 (e2 / y2 + 1e-300), 10 * std::log10 (es2 / ys2 + 1e-300));
                }
        }
}

// zipper: bass knob sweep while a sine plays; write float32 raw outputs for analysis
static void zip (const std::string& dir)
{
    const double fs = 48000; const int n = (int) fs * 2;
    auto knobAt = [&] (int i) { const double t = i / fs; return t < 0.5 ? 5.0 : (t < 1.0 ? 5.0 + 10.0 * (t - 0.5) : 10.0); };  // 5 -> 10 in 0.5 s
    for (double fin : { 100.0, 1000.0 })
    {
        std::vector<float> x (n);
        for (int i = 0; i < n; ++i) x[i] = (float) (0.25 * std::sin (2 * M_PI * fin * i / fs));
        auto dump = [&] (const char* name, const std::vector<float>& y)
        {
            const std::string p = dir + "/zip_" + name + "_" + std::to_string ((int) fin) + ".f32";
            FILE* f = std::fopen (p.c_str(), "wb"); std::fwrite (y.data(), 4, y.size(), f); std::fclose (f);
        };
        for (int block : { 512, 64 })
        {   // as shipped: knob read once per block, quantised to the 0.1 parameter step, coefficients swapped at the block edge
            std::vector<float> y (n); TDF2<float> flt; Co<float> k = lowShelf<float> (fs, 150.0f, 0.707f, 1.0f);
            for (int i = 0; i < n; ++i)
            {
                if (i % block == 0) { const float v = std::round ((float) knobAt (i) * 10.0f) / 10.0f; k = lowShelf<float> (fs, 150.0f, 0.707f, dbToGain (4.0f * (v - 5.0f))); }
                y[i] = flt.tick (x[i], k);
            }
            dump (block == 512 ? "shipped512" : "shipped64", y);
        }
        {   // proposal: same quantised per-block target, one-pole smoothed knob (tau 10 ms), SVF retuned every 16 samples
            std::vector<float> y (n); SVFLowShelf<float> s; double sm = 5.0, target = 5.0; const double a = 1.0 - std::exp (-1.0 / (0.010 * fs));
            s.set (fs, 150, 0.707, 0);
            for (int i = 0; i < n; ++i)
            {
                if (i % 512 == 0) target = std::round (knobAt (i) * 10.0) / 10.0;
                sm += a * (target - sm);
                if (i % 16 == 0) s.set (fs, 150, 0.707, 4.0 * (sm - 5.0));
                y[i] = s.tick (x[i]);
            }
            dump ("svf_smoothed", y);
        }
        {   // SVF, smoothed knob, g/k/m targets every 16 samples, linearly interpolated per sample, a1..a3 derived per sample
            std::vector<float> y (n); double sm = 5.0, target = 5.0; const double a = 1.0 - std::exp (-1.0 / (0.010 * fs));
            float ic1 = 0, ic2 = 0; double g = std::tan (M_PI * 150 / fs), k = 1.0 / 0.707, m1 = 0, m2 = 0, dg = 0, dm1 = 0, dm2 = 0;
            for (int i = 0; i < n; ++i)
            {
                if (i % 512 == 0) target = std::round (knobAt (i) * 10.0) / 10.0;
                sm += a * (target - sm);
                if (i % 16 == 0)
                {   // where the knob will be 16 samples from now
                    double s2 = sm; for (int j = 0; j < 16; ++j) s2 += a * (target - s2);
                    const double A = std::pow (10.0, 4.0 * (s2 - 5.0) / 40.0), gT = std::tan (M_PI * 150 / fs) / std::sqrt (A);
                    dg = (gT - g) / 16; dm1 = (k * (A - 1) - m1) / 16; dm2 = (A * A - 1 - m2) / 16;
                }
                g += dg; m1 += dm1; m2 += dm2;
                const float a1 = (float) (1.0 / (1.0 + g * (g + k))), a2 = (float) g * a1, a3 = (float) g * a2;
                const float v0 = x[i], v3 = v0 - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
                y[i] = v0 + (float) m1 * v1 + (float) m2 * v2;
            }
            dump ("svf_interp", y);
        }
        {   // shipped biquad structure, smoothed knob, coefficients recomputed every sample (upper bound for the TDF-II form)
            std::vector<float> y (n); TDF2<float> flt; double sm = 5.0, target = 5.0; const double a = 1.0 - std::exp (-1.0 / (0.010 * fs));
            for (int i = 0; i < n; ++i)
            {
                if (i % 512 == 0) target = std::round (knobAt (i) * 10.0) / 10.0;
                sm += a * (target - sm);
                y[i] = flt.tick (x[i], lowShelf<float> (fs, 150.0f, 0.707f, dbToGain ((float) (4.0 * (sm - 5.0)))));
            }
            dump ("tdf2_persample", y);
        }
        {   // SVF as above but the knob goes through two cascaded one-poles (5 ms each): the gain trajectory has no slope jump
            std::vector<float> y (n); double s1 = 5.0, sm = 5.0, target = 5.0; const double a = 1.0 - std::exp (-1.0 / (0.005 * fs));
            float ic1 = 0, ic2 = 0; double g = std::tan (M_PI * 150 / fs), k = 1.0 / 0.707, m1 = 0, m2 = 0, dg = 0, dm1 = 0, dm2 = 0;
            for (int i = 0; i < n; ++i)
            {
                if (i % 512 == 0) target = std::round (knobAt (i) * 10.0) / 10.0;
                s1 += a * (target - s1); sm += a * (s1 - sm);
                if (i % 16 == 0)
                {
                    double p1 = s1, p2 = sm; for (int j = 0; j < 16; ++j) { p1 += a * (target - p1); p2 += a * (p1 - p2); }
                    const double A = std::pow (10.0, 4.0 * (p2 - 5.0) / 40.0), gT = std::tan (M_PI * 150 / fs) / std::sqrt (A);
                    dg = (gT - g) / 16; dm1 = (k * (A - 1) - m1) / 16; dm2 = (A * A - 1 - m2) / 16;
                }
                g += dg; m1 += dm1; m2 += dm2;
                const float a1 = (float) (1.0 / (1.0 + g * (g + k))), a2 = (float) g * a1, a3 = (float) g * a2;
                const float v0 = x[i], v3 = v0 - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
                y[i] = v0 + (float) m1 * v1 + (float) m2 * v2;
            }
            dump ("svf_interp_2pole", y);
        }
        {   // shipped biquad structure, two-pole smoothed knob, coefficients every 16 samples
            std::vector<float> y (n); TDF2<float> flt; Co<float> k = lowShelf<float> (fs, 150.0f, 0.707f, 1.0f); double s1 = 5.0, sm = 5.0, target = 5.0; const double a = 1.0 - std::exp (-1.0 / (0.005 * fs));
            for (int i = 0; i < n; ++i)
            {
                if (i % 512 == 0) target = std::round (knobAt (i) * 10.0) / 10.0;
                s1 += a * (target - s1); sm += a * (s1 - sm);
                if (i % 16 == 0) k = lowShelf<float> (fs, 150.0f, 0.707f, dbToGain ((float) (4.0 * (sm - 5.0))));
                y[i] = flt.tick (x[i], k);
            }
            dump ("tdf2_smoothed_2pole", y);
        }
        {   // same smoothing but keeping the shipped float TDF-II biquad, coefficients every 16 samples
            std::vector<float> y (n); TDF2<float> flt; Co<float> k = lowShelf<float> (fs, 150.0f, 0.707f, 1.0f); double sm = 5.0, target = 5.0; const double a = 1.0 - std::exp (-1.0 / (0.010 * fs));
            for (int i = 0; i < n; ++i)
            {
                if (i % 512 == 0) target = std::round (knobAt (i) * 10.0) / 10.0;
                sm += a * (target - sm);
                if (i % 16 == 0) k = lowShelf<float> (fs, 150.0f, 0.707f, dbToGain ((float) (4.0 * (sm - 5.0))));
                y[i] = flt.tick (x[i], k);
            }
            dump ("tdf2_smoothed", y);
        }
    }
}

int main (int argc, char** argv)
{
    if (argc > 1 && ! std::strcmp (argv[1], "cramp")) cramp();
    else if (argc > 1 && ! std::strcmp (argv[1], "prec")) prec();
    else if (argc > 2 && ! std::strcmp (argv[1], "zip")) zip (argv[2]);
    else { std::fprintf (stderr, "usage: ours cramp|prec|zip <dir>\n"); return 1; }
    return 0;
}
