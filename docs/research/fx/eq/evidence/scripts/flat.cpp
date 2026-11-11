// Is ToneStackEQ at knob 5 (gain factor 1.0f) a bit-exact pass-through? Same formulas as ours.cpp.
// build twice: clang++ -O2 -std=c++17 flat.cpp -o flat_fma ; clang++ -O2 -ffp-contract=off -std=c++17 flat.cpp -o flat_nofma
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>
struct Co { float b0, b1, b2, a1, a2; };
static Co norm (float b0, float b1, float b2, float a0, float a1, float a2) { const float inv = 1.0f / a0; return { b0 * inv, b1 * inv, b2 * inv, a1 * inv, a2 * inv }; }
static Co lowShelf (double fs, float f, float Q, float gain) { const float A = std::sqrt (gain), am = A - 1, ap = A + 1; const float w = (2 * 3.14159265358979323846f * f) / (float) fs; const float c = std::cos (w), beta = std::sin (w) * std::sqrt (A) / Q, amc = am * c;
    return norm (A * (ap - amc + beta), A * 2 * (am - ap * c), A * (ap - amc - beta), ap + amc + beta, -2 * (am + ap * c), ap + amc - beta); }
static Co highShelf (double fs, float f, float Q, float gain) { const float A = std::sqrt (gain), am = A - 1, ap = A + 1; const float w = (2 * 3.14159265358979323846f * f) / (float) fs; const float c = std::cos (w), beta = std::sin (w) * std::sqrt (A) / Q, amc = am * c;
    return norm (A * (ap + amc + beta), A * -2 * (am + ap * c), A * (ap + amc - beta), ap - amc + beta, 2 * (am - ap * c), ap - amc - beta); }
static Co peak (double fs, float f, float Q, float gain) { const float A = std::sqrt (gain); const float w = (2 * 3.14159265358979323846f * f) / (float) fs; const float alpha = std::sin (w) / (Q * 2), c2 = -2 * std::cos (w);
    return norm (1 + alpha * A, c2, 1 - alpha * A, 1 + alpha / A, c2, 1 - alpha / A); }
struct F { float s1 = 0, s2 = 0; __attribute__((noinline)) float tick (float x, const Co& k) { const float y = x * k.b0 + s1; s1 = (x * k.b1) - (y * k.a1) + s2; s2 = (x * k.b2) - (y * k.a2); return y; } };
int main() {
    std::printf ("fs\tband\tb0\tb1==a1\tb2==a2\tsamples_differing_of_480000\tmax_abs_diff\n");
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 }) {
        const Co ks[3] = { lowShelf (fs, 150.0f, 0.707f, 1.0f), peak (fs, 425.0f, 0.7f, 1.0f), highShelf (fs, 1800.0f, 0.707f, 1.0f) };
        const char* nm[3] = { "bass", "middle", "treble" };
        for (int b = 0; b < 3; ++b) {
            std::mt19937 rng (1); std::uniform_real_distribution<float> d (-0.5f, 0.5f); F f; long diff = 0; float mx = 0;
            for (int i = 0; i < 480000; ++i) { const float x = d (rng), y = f.tick (x, ks[b]); if (std::memcmp (&x, &y, 4) != 0) { ++diff; mx = std::fmax (mx, std::fabs (y - x)); } }
            std::printf ("%.0f\t%s\t%.9g\t%d\t%d\t%ld\t%.3g\n", fs, nm[b], ks[b].b0, ks[b].b1 == ks[b].a1, ks[b].b2 == ks[b].a2, diff, mx);
        }
    }
}
