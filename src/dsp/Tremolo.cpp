#include "Tremolo.h"

#include "FxPrimitives.h"

#include <cmath>

namespace tubamp
{
namespace
{
// Tunings for voices 1-4 (docs/FX-ENGINES.md §4.7). Classic carries its own, verbatim.

constexpr float kMinRateHz = 0.01f;            // [§4.7.2: TremoloParams range]
constexpr float kMaxRateHz = 20.0f;            // [§4.7.2: TremoloParams range]
constexpr float kParamSmoothSeconds = 0.02f;   // [choice: §2.5, the chain's 20 ms ramps]
constexpr float kGainSmoothHz = 120.0f;        // [study: R1, choice] tau 1.3 ms, 10-90 % in 2.9 ms
constexpr float kMakeupDb = 3.0f;              // [study: R1] at full depth; TONEX crest 1.4142
constexpr double kSkewMin = 0.1;               // [study: R1] s = 0.1 + 0.8 * shape
constexpr double kSkewRange = 0.8;             // [study: R1]
constexpr double kMaxSpreadCycles = 0.5;       // [study: R3] width 1 = 180 degrees

constexpr double kOpticalLitFrom = 0.75;       // [study: R2] lamp lit for the last quarter of the cycle
constexpr float kOpticalDepth = 0.9f;          // [study: R2] floor 0.1 (-20 dB) at full depth
constexpr float kOpticalFallSeconds = 0.005f;  // [study: R2] while the gain falls (lamp lit)
constexpr float kOpticalRiseSeconds = 0.020f;  // [study: R2] while it recovers (lamp dark)

constexpr float kBiasAttackSeconds = 0.005f;   // [study: R5] envelope follower
constexpr float kBiasReleaseSeconds = 0.150f;  // [study: R5]
constexpr float kBiasKneeMinDb = -24.0f;       // [study: R5] knee = -24 + 24 * shape dBFS
constexpr float kBiasKneeRangeDb = 24.0f;      // [study: R5]

constexpr float kHarmonicCentreHz = 300.0f;    // [study: R6] geometric mean of the circuit's 144 and 636 Hz
constexpr float kHarmonicOctaves = 2.0f;       // [study: R6] tone 0..1 spans 150..600 Hz

// Phases are recounted from a fresh origin this often so the product rate * count stays
// far inside double precision.
constexpr int kPhaseRebaseCount = 1 << 30;

//==============================================================================
/** Classic: the tremolo of the old Modulation block, moved verbatim (src/dsp/FxBlocks.cpp
    at bf81728, :364-405 and :426-448). Not one expression may change: the golden renders
    compare its output bit for bit. */
class ClassicTremolo
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        reset();
    }

    void reset() noexcept
    {
        tremPhase = 0.0f;
    }

    void setParameters (float rateHz, float depth01, float mix01) noexcept
    {
        rate = juce::jlimit (0.01f, 100.0f, rateHz);
        depth = juce::jlimit (0.0f, 1.0f, depth01);
        mix = juce::jlimit (0.0f, 1.0f, mix01);
    }

    void process (juce::dsp::AudioBlock<float> block) noexcept
    {
        if (block.getNumSamples() == 0)
            return;

        // Tremolo: sine LFO amplitude modulation, equal-power dry/wet blend.
        const float dryGain = std::sqrt (1.0f - mix);
        const float wetGain = std::sqrt (mix);
        const float phaseInc = rate / (float) sampleRate;
        const auto numSamples = block.getNumSamples();
        const auto numChannels = block.getNumChannels();

        for (size_t i = 0; i < numSamples; ++i)
        {
            const float lfo = 0.5f + 0.5f * std::sin (juce::MathConstants<float>::twoPi * tremPhase);
            const float modGain = 1.0f - depth * (1.0f - lfo);

            for (size_t ch = 0; ch < numChannels; ++ch)
            {
                auto* data = block.getChannelPointer (ch);
                data[i] = data[i] * dryGain + data[i] * modGain * wetGain;
            }

            tremPhase += phaseInc;

            if (tremPhase >= 1.0f)
                tremPhase -= 1.0f;
        }
    }

private:
    float tremPhase = 0.0f;
    float rate = 1.0f, depth = 0.5f, mix = 0.5f;
    double sampleRate = 44100.0;
};

//==============================================================================
/** LFO phase in cycles, [0, 1).

    It is computed as frac (origin + rate * count / fs) from the number of samples since
    the rate last changed, not by summing an increment. Summing drifts by an ulp per
    sample, which moves a pulse or lamp edge by a whole sample whenever the edge falls
    exactly on one (4 Hz at 48 kHz puts every edge there), and a one-sample edge shift is
    -53 dB of difference against the reference model. Counting reproduces the reference
    phase exactly and costs one double divide. */
class LfoPhase
{
public:
    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate;
        reset();
    }

    void reset() noexcept
    {
        origin = value = 0.0;
        count = 0;
        rate = -1.0f;
    }

    /** Song-position lock: the current sample is at `cycles` (any value; its fraction is used). */
    void lockTo (double cycles) noexcept
    {
        origin = value = cycles - std::floor (cycles);
        count = 0;
    }

    double get() const noexcept { return value; }

    /** Steps one sample at `rateHz`. */
    void advance (float rateHz) noexcept
    {
        if (! juce::exactlyEqual (rateHz, rate) || count >= kPhaseRebaseCount)
        {
            origin = value;
            count = 0;
            rate = rateHz;
        }

        ++count;
        const double p = origin + (double) rate * (double) count / fs;
        value = p - std::floor (p);
    }

private:
    double fs = 48000.0, origin = 0.0, value = 0.0;
    int count = 0;
    float rate = -1.0f;
};

//==============================================================================
/** Voices 1-4: one LFO, one set of smoothers and the per-voice state, so a voice switch is
    a reset of this one object. */
class ShapedTremolo
{
public:
    void prepare (double sampleRate)
    {
        fs = (float) sampleRate;
        phase.prepare (sampleRate);

        for (auto* s : { &rate, &depth, &width, &tone, &shape, &wave })
            s->prepare (kParamSmoothSeconds, fs, 0.0f);

        gainCoeff = 1.0f - std::exp (-fx::kTwoPi * kGainSmoothHz / fs);
        opticalFall = 1.0f - std::exp (-1.0f / (kOpticalFallSeconds * fs));
        opticalRise = 1.0f - std::exp (-1.0f / (kOpticalRiseSeconds * fs));
        biasAttack = 1.0f - std::exp (-1.0f / (kBiasAttackSeconds * fs));
        biasRelease = 1.0f - std::exp (-1.0f / (kBiasReleaseSeconds * fs));

        reset();
    }

    /** Fixed start: phase 0 (Smooth at its dip), gains primed from the first sample's
        target, photocell at 1, envelope and filters empty. The next setTargets() snaps. */
    void reset() noexcept
    {
        phase.reset();
        primeGains = true;

        for (int c = 0; c < 2; ++c)
        {
            gain[c] = highGain[c] = 1.0f;
            photocell[c] = 1.0f;
            lowpass[c] = 0.0f;
        }

        envelope = 0.0f;
        cachedDepth = cachedWidth = cachedSkew = cachedKnee = cachedTone = -1.0f;
        snapPending = true;
    }

    void setTargets (const TremoloParams& p) noexcept
    {
        rate.setTarget (juce::jlimit (kMinRateHz, kMaxRateHz, p.rateHz));
        depth.setTarget (juce::jlimit (0.0f, 1.0f, p.depth));
        width.setTarget (juce::jlimit (0.0f, 1.0f, p.width));
        tone.setTarget (juce::jlimit (0.0f, 1.0f, p.tone));
        shape.setTarget (juce::jlimit (0.0f, 1.0f, p.shape));
        wave.setTarget (juce::jlimit (0.0f, 1.0f, p.wave));

        if (snapPending)
        {
            snapPending = false;

            for (auto* s : { &rate, &depth, &width, &tone, &shape, &wave })
                s->snapToTarget();
        }
    }

    void lockPhase (double cycles) noexcept { phase.lockTo (cycles); }

    /** `right` is null for a mono block. Trace pointers may be null. */
    void process (TremoloFx::Voice voice, float* left, float* right, int numSamples,
                  float* traceLeft, float* traceRight) noexcept
    {
        switch (voice)
        {
            case TremoloFx::Voice::optical:   processOptical (left, right, numSamples, traceLeft, traceRight); break;
            case TremoloFx::Voice::bias:      processBias (left, right, numSamples, traceLeft, traceRight); break;
            case TremoloFx::Voice::harmonic:  processHarmonic (left, right, numSamples, traceLeft, traceRight); break;
            case TremoloFx::Voice::smooth:
            case TremoloFx::Voice::classic:
            default:                          processSmooth (left, right, numSamples, traceLeft, traceRight); break;
        }

        // Once per block, so one bad sample cannot live in a recursive state.
        envelope = fx::sanitise (envelope);

        for (int c = 0; c < 2; ++c)
            lowpass[c] = fx::sanitise (lowpass[c]);
    }

private:
    //==========================================================================
    void updateDepth (float d) noexcept
    {
        if (juce::exactlyEqual (d, cachedDepth))
            return;

        cachedDepth = d;
        floorGain = (1.0f - d) * (1.0f - d);
        makeup = std::pow (10.0f, kMakeupDb * d / 20.0f);
        opticalDepth = kOpticalDepth * d;
        harmonicNorm = std::sqrt (2.0f / (2.0f - d));
    }

    void updateWidth (float w) noexcept
    {
        if (juce::exactlyEqual (w, cachedWidth))
            return;

        cachedWidth = w;
        spreadCycles = kMaxSpreadCycles * (double) w;
        loudnessExponent = 1.0f - 0.5f * w;
    }

    void updateSkew (float s) noexcept
    {
        if (juce::exactlyEqual (s, cachedSkew))
            return;

        cachedSkew = s;
        skew = kSkewMin + kSkewRange * (double) s;
        skewF = (float) skew;
        riseScale = (float) (0.5 / skew);
        fallScale = (float) (0.5 / (1.0 - skew));
    }

    void updateKnee (float s) noexcept
    {
        if (juce::exactlyEqual (s, cachedKnee))
            return;

        cachedKnee = s;
        inverseKnee = std::pow (10.0f, -(kBiasKneeMinDb + kBiasKneeRangeDb * s) / 20.0f);
    }

    void updateCrossover (float t) noexcept
    {
        if (juce::exactlyEqual (t, cachedTone))
            return;

        cachedTone = t;
        const float hz = juce::jmin (kHarmonicCentreHz * std::exp2 (kHarmonicOctaves * (t - 0.5f)), 0.49f * fs);
        const float g = std::tan (fx::kPi * hz / fs);
        crossoverG = g / (1.0f + g);
    }

    /** The stereo loudness law w^(1 - spread/360) (§4.7.4), for Smooth; exactly w at spread 0. */
    float loudness (float w) const noexcept
    {
        if (juce::exactlyEqual (loudnessExponent, 1.0f))
            return w;

        if (juce::exactlyEqual (loudnessExponent, 0.5f))
            return std::sqrt (w);

        return std::pow (w, loudnessExponent);
    }

    double rightPhase (double p) const noexcept
    {
        const double r = p + spreadCycles;
        return r >= 1.0 ? r - 1.0 : r;
    }

    void smoothGain (int c, float target) noexcept
    {
        gain[c] += gainCoeff * (target - gain[c]) + fx::kAntiDenormal;
    }

    //==========================================================================
    /** Smooth's shape at phase p, 1 = loud. Edge decisions stay in double so the pulse
        switches on the sample the reference model does. */
    float smoothShape (double p, float waveMix) const noexcept
    {
        const float pf = (float) p;
        const float w0 = p < skew ? riseScale * pf : 0.5f + fallScale * (pf - skewF);
        const float tri = 1.0f - std::abs (2.0f * w0 - 1.0f);

        if (waveMix < 0.5f)
        {
            const float sine = 0.5f - 0.5f * std::cos (fx::kTwoPi * w0);
            return sine + 2.0f * waveMix * (tri - sine);
        }

        double q = p - 0.5 * skew;

        if (q < 0.0)
            q += 1.0;

        const float pulse = q < skew ? 1.0f : 0.0f;
        return tri + (2.0f * waveMix - 1.0f) * (pulse - tri);
    }

    void processSmooth (float* left, float* right, int n, float* traceLeft, float* traceRight) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            updateDepth (depth.next());
            updateWidth (width.next());
            updateSkew (shape.next());
            const float waveMix = wave.next();
            const double p = phase.get();

            const float targetL = (floorGain + (1.0f - floorGain) * loudness (smoothShape (p, waveMix))) * makeup;
            const float targetR = right != nullptr
                                      ? (floorGain + (1.0f - floorGain) * loudness (smoothShape (rightPhase (p), waveMix))) * makeup
                                      : targetL;

            if (primeGains)
            {
                gain[0] = targetL;
                gain[1] = targetR;
                primeGains = false;
            }

            smoothGain (0, targetL);
            left[i] *= gain[0];

            if (right != nullptr)
            {
                smoothGain (1, targetR);
                right[i] *= gain[1];
            }

            writeTrace (traceLeft, traceRight, i, right != nullptr);
            phase.advance (rate.next());
        }
    }

    void processOptical (float* left, float* right, int n, float* traceLeft, float* traceRight) noexcept
    {
        const auto follow = [this] (int c, double p)
        {
            const float target = p >= kOpticalLitFrom ? 1.0f - opticalDepth : 1.0f;
            photocell[c] += (target > photocell[c] ? opticalRise : opticalFall) * (target - photocell[c]) + fx::kAntiDenormal;
            return photocell[c];
        };

        for (int i = 0; i < n; ++i)
        {
            updateDepth (depth.next());
            updateWidth (width.next());
            const double p = phase.get();

            // The lamp is either lit or dark, so the loudness law (u^e) leaves it unchanged.
            gain[0] = follow (0, p);
            left[i] *= gain[0];

            if (right != nullptr)
            {
                gain[1] = follow (1, rightPhase (p));
                right[i] *= gain[1];
            }

            writeTrace (traceLeft, traceRight, i, right != nullptr);
            phase.advance (rate.next());
        }

        primeGains = false;
    }

    void processBias (float* left, float* right, int n, float* traceLeft, float* traceRight) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            updateDepth (depth.next());
            updateWidth (width.next());
            updateKnee (shape.next());
            const double p = phase.get();

            // [choice] the mean of the channels, so the knee means the same dBFS in mono and stereo.
            const float level = std::abs (right != nullptr ? 0.5f * (left[i] + right[i]) : left[i]);
            envelope += (level > envelope ? biasAttack : biasRelease) * (level - envelope) + fx::kAntiDenormal;
            const float q = envelope * inverseKnee;

            // No loudness law here [coordinator decision, deviates from §4.7.4]: the gain is
            // already the square root of a law linear in u, so at spread 180 L^2 + R^2 is
            // constant at any depth and level; u^e would add up to 2 dB of ripple. A division,
            // not a reciprocal: at depth 0 it is exactly 1, so the voice is transparent.
            const auto target = [&] (double ph)
            {
                const float u = 0.5f + 0.5f * std::cos (fx::kTwoPi * (float) ph);
                return std::sqrt (juce::jmax (0.0f, (1.0f - cachedDepth * u + q) / (1.0f + q)));
            };

            const float targetL = target (p);
            const float targetR = right != nullptr ? target (rightPhase (p)) : targetL;

            if (primeGains)
            {
                gain[0] = targetL;
                gain[1] = targetR;
                primeGains = false;
            }

            smoothGain (0, targetL);
            left[i] *= gain[0];

            if (right != nullptr)
            {
                smoothGain (1, targetR);
                right[i] *= gain[1];
            }

            writeTrace (traceLeft, traceRight, i, right != nullptr);
            phase.advance (rate.next());
        }
    }

    void processHarmonic (float* left, float* right, int n, float* traceLeft, float* traceRight) noexcept
    {
        const auto band = [this] (int c, float x)
        {
            // TPT one-pole: L = low-pass, H = x - L, so depth 0 sums back to x.
            const float v = (x - lowpass[c]) * crossoverG;
            const float low = v + lowpass[c];
            lowpass[c] = low + v + fx::kAntiDenormal;
            return gain[c] * low + highGain[c] * (x - low);
        };

        for (int i = 0; i < n; ++i)
        {
            updateDepth (depth.next());
            updateWidth (width.next());
            updateCrossover (tone.next());
            const double p = phase.get();

            float lowTarget[2] {}, highTarget[2] {};

            // No loudness law here either [coordinator decision, deviates from §4.7.4]: the
            // band powers are linear in u, so at spread 180 L^2 + R^2 is constant for any
            // spectrum; u^e would add up to 1.3 dB of ripple.
            const auto targets = [&] (int c, double ph)
            {
                const float u = 0.5f - 0.5f * std::cos (fx::kTwoPi * (float) ph);
                lowTarget[c] = harmonicNorm * std::sqrt (juce::jmax (0.0f, 1.0f - cachedDepth * u));
                highTarget[c] = harmonicNorm * std::sqrt (juce::jmax (0.0f, 1.0f - cachedDepth * (1.0f - u)));
            };

            targets (0, p);

            if (right != nullptr)
                targets (1, rightPhase (p));

            const int channels = right != nullptr ? 2 : 1;

            if (primeGains)
            {
                for (int c = 0; c < channels; ++c)
                {
                    gain[c] = lowTarget[c];
                    highGain[c] = highTarget[c];
                }

                primeGains = false;
            }

            for (int c = 0; c < channels; ++c)
            {
                smoothGain (c, lowTarget[c]);
                highGain[c] += gainCoeff * (highTarget[c] - highGain[c]) + fx::kAntiDenormal;
            }

            left[i] = band (0, left[i]);

            if (right != nullptr)
                right[i] = band (1, right[i]);

            writeTrace (traceLeft, traceRight, i, right != nullptr);
            phase.advance (rate.next());
        }
    }

    void writeTrace (float* traceLeft, float* traceRight, int i, bool stereo) const noexcept
    {
        if (traceLeft == nullptr)
            return;

        traceLeft[i] = gain[0];

        if (traceRight != nullptr)
            traceRight[i] = stereo ? gain[1] : gain[0];
    }

    //==========================================================================
    float fs = 48000.0f;
    LfoPhase phase;
    fx::Smoother rate, depth, width, tone, shape, wave;
    bool snapPending = true, primeGains = true;

    float gainCoeff = 1.0f, opticalFall = 1.0f, opticalRise = 1.0f, biasAttack = 1.0f, biasRelease = 1.0f;

    // Values derived from the smoothed knobs, recomputed only when a knob moves.
    float cachedDepth = -1.0f, cachedWidth = -1.0f, cachedSkew = -1.0f, cachedKnee = -1.0f, cachedTone = -1.0f;
    float floorGain = 1.0f, makeup = 1.0f, opticalDepth = 0.0f, harmonicNorm = 1.0f;
    double spreadCycles = 0.0, skew = 0.5;
    float loudnessExponent = 1.0f, skewF = 0.5f, riseScale = 1.0f, fallScale = 1.0f;
    float inverseKnee = 1.0f, crossoverG = 0.5f;

    // Audio state. gain[] is each channel's smoothed gain (Harmonic: its low band's).
    float gain[2] { 1.0f, 1.0f }, highGain[2] { 1.0f, 1.0f }, photocell[2] { 1.0f, 1.0f };
    float lowpass[2] {};
    float envelope = 0.0f;
};

/** Replaces a non-finite knob with its default, so a bad host value cannot poison a smoother. */
float finiteOr (float value, float fallback) noexcept
{
    return std::isfinite (value) ? value : fallback;
}
} // namespace

//==============================================================================
struct TremoloFx::Impl
{
    ClassicTremolo classic;
    ShapedTremolo shaped;
    fx::EngineSwitcher switcher;
    TremoloParams params;
    float* traceLeft = nullptr;
    float* traceRight = nullptr;
    int maxBlock = 0;
    bool snapVoice = true;

    void resetVoice (int voice) noexcept
    {
        if (voice == (int) Voice::classic)
        {
            classic.reset();
            return;
        }

        shaped.reset();
        shaped.setTargets (params);
    }
};

TremoloFx::TremoloFx() : impl (std::make_unique<Impl>()) {}
TremoloFx::~TremoloFx() = default;

void TremoloFx::prepare (const juce::dsp::ProcessSpec& spec)
{
    impl->classic.prepare (spec);
    impl->shaped.prepare (spec.sampleRate);
    impl->switcher.prepare (spec.sampleRate, (int) spec.maximumBlockSize, (int) spec.numChannels, (int) Voice::smooth);
    impl->maxBlock = (int) spec.maximumBlockSize;
    impl->snapVoice = true;
}

void TremoloFx::reset()
{
    impl->classic.reset();
    impl->shaped.reset();
    // Both voices are clean, so the next block starts on the requested voice outright
    // rather than finishing a switch that was in flight.
    impl->snapVoice = true;
}

void TremoloFx::setParameters (const TremoloParams& params)
{
    const TremoloParams defaults;
    auto& p = impl->params;

    p.voice = juce::jlimit (0, numVoices - 1, params.voice);
    // Classic clamps rate, depth and mix itself, exactly as it always did.
    p.rateHz = finiteOr (params.rateHz, defaults.rateHz);
    p.depth = finiteOr (params.depth, defaults.depth);
    p.mix = finiteOr (params.mix, defaults.mix);
    p.width = finiteOr (params.width, defaults.width);
    p.tone = finiteOr (params.tone, defaults.tone);
    p.shape = finiteOr (params.shape, defaults.shape);
    p.wave = finiteOr (params.wave, defaults.wave);
    p.syncPhase = std::isfinite (params.syncPhase) ? params.syncPhase : -1.0;

    impl->shaped.setTargets (p);
}

void TremoloFx::setTraceBuffers (float* left, float* right) noexcept
{
    impl->traceLeft = left;
    impl->traceRight = right;
}

void TremoloFx::process (juce::dsp::AudioBlock<float> block)
{
    auto& s = *impl;
    const int numSamples = (int) block.getNumSamples();
    const int numChannels = juce::jmin (2, (int) block.getNumChannels());

    if (numSamples == 0 || numChannels == 0)
        return;

    if (s.snapVoice)
    {
        // Nothing has been heard since prepare() or reset(): start on the requested voice.
        s.snapVoice = false;
        s.switcher.snapTo (s.params.voice);
    }

    const int voice = s.switcher.begin (block, s.params.voice, [&s] (int v, bool)
    {
        s.resetVoice (v);
        return true;
    });

    if (voice == (int) Voice::classic)
    {
        s.classic.setParameters (s.params.rateHz, s.params.depth, s.params.mix);
        s.classic.process (block);
    }
    else
    {
        if (s.params.syncPhase >= 0.0)
            s.shaped.lockPhase (s.params.syncPhase);

        const bool traced = s.traceLeft != nullptr && numSamples <= s.maxBlock;
        s.shaped.process ((Voice) voice, block.getChannelPointer (0),
                          numChannels > 1 ? block.getChannelPointer (1) : nullptr, numSamples,
                          traced ? s.traceLeft : nullptr, traced ? s.traceRight : nullptr);
    }

    s.switcher.end (block);
}
} // namespace tubamp
