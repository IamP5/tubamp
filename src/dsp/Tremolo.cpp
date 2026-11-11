#include "Tremolo.h"

#include "FxPrimitives.h"

#include <cmath>
#include <utility>

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

// The pulse's edges are raised-cosine ramps this long, centred on the hard edge, ahead of the
// 120 Hz smoother: a one-pole alone starts every edge with a corner, which is what splattered
// above 2 kHz (N9). 10-90 % is 2.96 ms against the one-pole's 2.92, and the energy above 2 kHz
// is 22 to 29 dB below a one-pole edge of that time, rates 1-20 Hz, duty 0.1-0.9.
constexpr double kPulseEdgeSeconds = 0.001;    // [choice, measured: check_tremolo.py Y5]
static_assert (kMaxRateHz * kPulseEdgeSeconds < kSkewMin, "an edge must fit inside the narrowest pulse");

constexpr double kOpticalLitFrom = 0.75;       // [study: R2] lamp lit for the last quarter of the cycle
constexpr float kOpticalDepth = 0.9f;          // [study: R2] floor 0.1 (-20 dB) at full depth
constexpr float kOpticalFallSeconds = 0.005f;  // [study: R2] while the gain falls (lamp lit)
constexpr float kOpticalRiseSeconds = 0.020f;  // [study: R2] while it recovers (lamp dark)

constexpr float kBiasAttackSeconds = 0.005f;   // [study: R5] envelope follower
constexpr float kBiasReleaseSeconds = 0.150f;  // [study: R5]
constexpr float kBiasKneeMinDb = -24.0f;       // [study: R5] knee = -24 + 24 * shape dBFS
constexpr float kBiasKneeRangeDb = 24.0f;      // [study: R5]
constexpr float kBiasDetectorHz = 10.0f;       // [choice] AC coupling ahead of the detector (Tremolo.h)

constexpr float kHarmonicCentreHz = 300.0f;    // [study: R6] geometric mean of the circuit's 144 and 636 Hz
constexpr float kHarmonicOctaves = 2.0f;       // [study: R6] tone 0..1 spans 150..600 Hz

// Song-position lock (Tremolo.h). A block whose host phase differs from the host's own previous
// phase advanced by its rate by more than this is a jump; anything smaller is host jitter or the
// residue of a tempo change, which the phase-locked loop absorbs. 0.01 cycle is 2.5 ms at 4 Hz;
// 2 ms of LFO time takes over above 5 Hz. Two blocks jittered by +-8 samples differ by at most
// 0.73 ms at 22.05 kHz, under a third of either, and a locate, loop or note-value change that
// moves the LFO by less is inaudible as a jump.
constexpr double kJumpCycles = 0.01;           // [choice, measured: check_tremolo.py Y2]
constexpr double kJumpSeconds = 0.002;         // [choice, measured: Y2]
constexpr double kLockSeconds = 0.15;          // [choice] loop time constant
constexpr double kLockMaxNudge = 0.03;         // [choice] largest rate correction, 3 %
constexpr double kRoundingCycles = 1.0e-6;     // [choice] below this the host's phase is taken as is
constexpr float kJumpFadeSeconds = 0.010f;     // [choice, measured: S1, Y1]

// A voice switch crossfades the two voices' outputs, both running on one LFO.
constexpr float kVoiceFadeSeconds = 0.040f;    // [choice: §2.4's 20 + 20 ms switch, measured: T16, Y3]

// Classic's LFO, 0.5 + 0.5 sin (2 pi p), dips at p = 0.75; voices 1-4 dip at 0.
constexpr double kClassicDipPhase = 0.75;

// Phases are recounted from a fresh origin this often so the product rate * count stays
// far inside double precision.
constexpr int kPhaseRebaseCount = 1 << 30;

double wrapHalf (double cycles) noexcept
{
    return cycles - std::floor (cycles + 0.5);
}

double frac (double cycles) noexcept
{
    return cycles - std::floor (cycles);
}

/** cos (2 pi x): reduced to a quarter cycle and a Taylor series to x^12, within 2e-7. The LFO
    shapes evaluate it per sample and channel, and this is cheaper than std::cos (N15). */
float cosCycles (float x) noexcept
{
    float y = std::abs (x - std::floor (x + 0.5f));   // [0, 0.5]
    const bool flip = y > 0.25f;
    y = flip ? 0.5f - y : y;                           // [0, 0.25]
    const float a = fx::kTwoPi * y;
    const float z = a * a;
    const float c = 1.0f + z * (-0.5f + z * (4.16666667e-2f + z * (-1.38888889e-3f + z * (2.48015873e-5f
                    + z * (-2.75573192e-7f + z * 2.08767570e-9f)))));
    return flip ? -c : c;
}

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

    /** A voice switch carries the LFO over; the steady path never calls these. */
    float getPhase() const noexcept { return tremPhase; }
    void setPhase (float cycles) noexcept { tremPhase = cycles; }

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

    /** The current sample is at `cycles` (any value; its fraction is used). */
    void lockTo (double cycles) noexcept
    {
        origin = value = frac (cycles);
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
/** fx::Smoother that is no longer stepped once it has stalled: a one-pole whose step rounds
    to nothing returns that same value until its target moves, so holding it is exact and
    spares five smoothers per sample in steady playing. */
class Knob
{
public:
    void prepare (float seconds, float sampleRate) noexcept
    {
        smoother.prepare (seconds, sampleRate, 0.0f);
        value = 0.0f;
        settled = true;
    }

    void setTarget (float target) noexcept
    {
        if (juce::exactlyEqual (target, smoother.getTarget()))
            return;

        smoother.setTarget (target);
        settled = false;
    }

    void snap() noexcept
    {
        smoother.snapToTarget();
        value = smoother.getTarget();
        settled = true;
    }

    float next() noexcept
    {
        if (! settled)
        {
            const float v = smoother.next();
            settled = juce::exactlyEqual (v, value);
            value = v;
        }

        return value;
    }

    float getTarget() const noexcept { return smoother.getTarget(); }
    float get() const noexcept { return value; }
    bool isSettled() const noexcept { return settled; }

private:
    fx::Smoother smoother;
    float value = 0.0f;
    bool settled = true;
};

//==============================================================================
/** Voices 1-4: one LFO and one set of knob smoothers, shared, and each voice's own audio
    state, so two voices can run side by side on the same cycle during a switch. */
class ShapedTremolo
{
public:
    static constexpr int numShaped = TremoloFx::numVoices;   // indexed by Voice; Classic's row unused

    void prepare (double sampleRate)
    {
        fs = (float) sampleRate;
        phase.prepare (sampleRate);
        oldPhase.prepare (sampleRate);

        for (auto* s : { &rate, &depth, &width, &tone, &shape, &wave })
            s->prepare (kParamSmoothSeconds, fs);

        gainCoeff = 1.0f - std::exp (-fx::kTwoPi * kGainSmoothHz / fs);
        jumpLength = juce::jmax (1, (int) std::lround (kJumpFadeSeconds * fs));
        opticalFall = 1.0f - std::exp (-1.0f / (kOpticalFallSeconds * fs));
        opticalRise = 1.0f - std::exp (-1.0f / (kOpticalRiseSeconds * fs));
        biasAttack = 1.0f - std::exp (-1.0f / (kBiasAttackSeconds * fs));
        biasRelease = 1.0f - std::exp (-1.0f / (kBiasReleaseSeconds * fs));
        detectorCoeff = 1.0f - std::exp (-fx::kTwoPi * kBiasDetectorHz / fs);

        reset();
    }

    /** Every gain at unity so the first block glides from the input into the tremolo through
        the gain smoother, envelope and filters empty. The LFO starts where the voice is at its
        crest (crestPhase). Every setTargets() until the next process() snaps the knobs and
        that start, so they hold whether the host updates them before or after the reset. */
    void reset() noexcept
    {
        phase.reset();
        oldPhase.reset();
        freshPhase = true;
        jumpLeft = 0;
        jumpRestart = 0;
        hostValid = false;
        syncRate = 0.0f;
        jumps = 0;

        anyPrime = false;

        for (int v = 0; v < numShaped; ++v)
        {
            prime[v] = false;

            for (int c = 0; c < 2; ++c)
                lastU[v][c] = jumpOffset[v][c] = 0.0f;
        }

        for (int c = 0; c < 2; ++c)
        {
            smoothDev[c] = biasDev[c] = lowDev[c] = highDev[c] = photocellDev[c] = 0.0f;
            lowpass[c] = 0.0f;
            gainOut[c] = 1.0f;
        }

        envelope = detectorLow = 0.0f;
        cachedDepth = cachedWidth = cachedSkew = cachedTone = -1.0f;
        lawExponent = -1.0f;   // rebuilt as from prepare(), so a reset engine renders as a fresh one
        snapPending = knobsMoving = true;
    }

    void setTargets (const TremoloParams& p) noexcept
    {
        rate.setTarget (juce::jlimit (kMinRateHz, kMaxRateHz, p.rateHz));
        depth.setTarget (juce::jlimit (0.0f, 1.0f, p.depth));
        width.setTarget (juce::jlimit (0.0f, 1.0f, p.width));
        tone.setTarget (juce::jlimit (0.0f, 1.0f, p.tone));
        shape.setTarget (juce::jlimit (0.0f, 1.0f, p.shape));
        wave.setTarget (juce::jlimit (0.0f, 1.0f, p.wave));
        knobsMoving = knobsMoving || ! (depth.isSettled() && width.isSettled() && tone.isSettled() && shape.isSettled() && wave.isSettled());

        if (snapPending)
        {
            snapKnobs();
            phase.lockTo (crestPhase (p.voice));
            currentRate = rate.getTarget();
        }
    }

    /** The shaped engine takes over from Classic at `cycles` (in its own convention); the
        voice fading in is primed on its first sample. */
    void enterAt (double cycles) noexcept
    {
        snapKnobs();
        phase.lockTo (cycles);
        currentRate = rate.getTarget();
        jumpLeft = 0;
        jumpRestart = 0;
        hostValid = false;
        freshPhase = true;
        envelope = detectorLow = 0.0f;
    }

    void startVoice (int voice) noexcept
    {
        prime[voice] = anyPrime = true;

        for (int c = 0; c < 2; ++c)
            jumpOffset[voice][c] = 0.0f;
    }

    /** Bias joining mid-note: its detector starts on this block's DC and peak level, close to
        where the follower sits on a held note, instead of at silence. */
    void primeBias (const float* left, const float* right, int n) noexcept
    {
        float sum = 0.0f;

        for (int i = 0; i < n; ++i)
            sum += right != nullptr ? 0.5f * (left[i] + right[i]) : left[i];

        detectorLow = sum / (float) juce::jmax (1, n);
        envelope = 0.0f;

        for (int i = 0; i < n; ++i)
            envelope = juce::jmax (envelope, std::abs ((right != nullptr ? 0.5f * (left[i] + right[i]) : left[i]) - detectorLow));
    }

    double getPhase() const noexcept { return phase.get(); }
    int getJumpCount() const noexcept { return jumps; }

    /** Song-position lock for one block (Tremolo.h). `hostPhase` < 0: the LFO runs free. */
    void syncTo (double hostPhase, float hostRateHz, int numSamples) noexcept
    {
        if (hostPhase < 0.0)
        {
            hostValid = false;
            syncRate = 0.0f;
            return;
        }

        const float r = juce::jlimit (kMinRateHz, kMaxRateHz, hostRateHz);
        // Released, the rate glides on from the tempo's.
        rate.setTarget (r);
        rate.snap();

        const double h = frac (hostPhase);
        double error = wrapHalf (h - phase.get());

        if (freshPhase || std::abs (error) < kRoundingCycles)
        {
            // Nothing heard yet, or only rounding between the two counts: take the host's.
            phase.lockTo (h);
            error = 0.0;
        }
        else
        {
            const double limit = juce::jmax (kJumpCycles, kJumpSeconds * (double) r);
            const double discontinuity = hostValid ? wrapHalf (h - (hostPrev + hostPrevRate * (double) lastBlock / (double) fs))
                                                   : error;

            // The second test only catches a loop that lost the host altogether.
            if (std::abs (discontinuity) > limit || std::abs (error) > 2.0 * limit)
            {
                startJump();
                phase.lockTo (h);
                error = 0.0;
            }
        }

        const double seconds = (double) numSamples / (double) fs;
        const double nudge = juce::jlimit (-kLockMaxNudge, kLockMaxNudge, error / (juce::jmax (kLockSeconds, seconds) * (double) r));
        syncRate = (float) ((double) r * (1.0 + nudge));
        hostPrev = h;
        hostPrevRate = (double) r;
        hostValid = true;
    }

    /** One voice on its own. `right` is null for a mono block. Trace pointers may be null. */
    void process (int voice, float* left, float* right, int numSamples, float* traceLeft, float* traceRight) noexcept
    {
        switch ((TremoloFx::Voice) voice)
        {
            case TremoloFx::Voice::optical:  run (left, right, numSamples, traceLeft, traceRight, [this] (float& l, float* r) { stepOptical (l, r); }); break;
            case TremoloFx::Voice::bias:     run (left, right, numSamples, traceLeft, traceRight, [this] (float& l, float* r) { stepBias (l, r); }); break;
            case TremoloFx::Voice::harmonic: run (left, right, numSamples, traceLeft, traceRight, [this] (float& l, float* r) { stepHarmonic (l, r); }); break;
            case TremoloFx::Voice::smooth:
            case TremoloFx::Voice::classic:
            default:                         run (left, right, numSamples, traceLeft, traceRight, [this] (float& l, float* r) { stepSmooth (l, r); }); break;
        }
    }

    /** Two voices on the one LFO, `from` crossfading into `to` by `weight (i)`. */
    template <typename Weight>
    void processPair (int from, int to, float* left, float* right, int numSamples, float* traceLeft, float* traceRight,
                      Weight&& weight) noexcept
    {
        int i = 0;

        run (left, right, numSamples, traceLeft, traceRight, [&] (float& l, float* r)
        {
            float outL = l, outR = r != nullptr ? *r : 0.0f;
            step (from, outL, r != nullptr ? &outR : nullptr);
            step (to, l, r);
            const float w = weight (i++);
            l = outL + w * (l - outL);

            if (r != nullptr)
                *r = outR + w * (*r - outR);
        });
    }

private:
    //==========================================================================
    void snapKnobs() noexcept
    {
        for (auto* s : { &rate, &depth, &width, &tone, &shape, &wave })
            s->snap();

        knobsMoving = true;   // the derived values still have to be computed once
    }

    /** Where each voice's steady gain is highest, so a reset starts the first note at full
        level and the cycle begins from there (N10). Optical starts dark: the photocell is at
        unity and the lamp lights three quarters of a cycle later. */
    double crestPhase (int voice) const noexcept
    {
        switch ((TremoloFx::Voice) voice)
        {
            case TremoloFx::Voice::smooth:  return kSkewMin + kSkewRange * (double) shape.getTarget();
            case TremoloFx::Voice::bias:    return 0.5;
            case TremoloFx::Voice::classic:
            case TremoloFx::Voice::optical:
            case TremoloFx::Voice::harmonic:
            default:                        return 0.0;
        }
    }

    void startJump() noexcept
    {
        // During a fade the old side becomes the blend heard now (lastU), so it retargets
        // from there without a step.
        jumpRestart = jumpLeft > 0 ? 2 : 1;
        oldPhase = phase;
        jumpLeft = jumpLength;
        ++jumps;
    }

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

    /** A mono block has nothing to spread, so it runs as width 0 whatever the knob says. */
    void updateWidth (float w, bool stereo) noexcept
    {
        if (! stereo)
            w = 0.0f;

        if (juce::exactlyEqual (w, cachedWidth))
            return;

        cachedWidth = w;
        spreadCycles = kMaxSpreadCycles * (double) w;
        loudnessExponent = 1.0f - 0.5f * w;
    }

    /** Shape is Smooth's skew and Bias's knee. */
    void updateShape (float s) noexcept
    {
        if (juce::exactlyEqual (s, cachedSkew))
            return;

        cachedSkew = s;
        skew = kSkewMin + kSkewRange * (double) s;
        skewF = (float) skew;
        riseScale = (float) (0.5 / skew);
        fallScale = (float) (0.5 / (1.0 - skew));
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

    /** The stereo loudness law w^(1 - spread/360) (§4.7.4), for Smooth; exactly w at spread 0.
        Between spread 0 and 180, with Width at rest, from a table of s^(2e) over s = sqrt (w),
        which is smooth down to w = 0 (within 3e-6); std::pow per sample and channel was what
        put Smooth at width 0.5 over its budget (N15). */
    forcedinline float loudness (float w) const noexcept
    {
        if (juce::exactlyEqual (loudnessExponent, 1.0f))
            return w;

        if (juce::exactlyEqual (loudnessExponent, 0.5f))
            return std::sqrt (w);

        if (juce::exactlyEqual (lawExponent, loudnessExponent))
        {
            const float x = std::sqrt (juce::jlimit (0.0f, 1.0f, w)) * (float) kLawSize;
            const int i = juce::jmin ((int) x, kLawSize - 1);
            return law[i] + (x - (float) i) * (law[i + 1] - law[i]);
        }

        return std::pow (w, loudnessExponent);
    }

    /** Rebuilds the loudness table once Width has come to rest at a new value. */
    void refreshLaw() noexcept
    {
        if (knobsMoving || juce::exactlyEqual (lawExponent, loudnessExponent)
            || juce::exactlyEqual (loudnessExponent, 1.0f) || juce::exactlyEqual (loudnessExponent, 0.5f))
            return;

        lawExponent = loudnessExponent;

        for (int i = 0; i <= kLawSize; ++i)
            law[i] = std::pow ((float) i / (float) kLawSize, 2.0f * lawExponent);
    }

    double rightPhase (double p) const noexcept
    {
        const double r = p + spreadCycles;
        return r >= 1.0 ? r - 1.0 : r;
    }

    // Gains are held as their distance from unity. Near 1 a float one-pole stalls a few
    // ulps short of its target (Optical's lamp at -86 dB), so depth 0 would never come
    // back to exact transparency; the distance decays to nothing and 1 + it rounds to 1.
    static float smoothToward (float& dev, float coeff, float target) noexcept
    {
        dev += coeff * ((target - 1.0f) - dev) + fx::kAntiDenormal;
        return 1.0f + dev;
    }

    /** Weight of the new phase in a jump crossfade for this sample, 1 outside one. */
    float jumpWeight() const noexcept
    {
        if (jumpLeft == 0)
            return 1.0f;

        return 0.5f - 0.5f * std::cos (fx::kPi * (float) (jumpLength - jumpLeft) / (float) jumpLength);
    }

    /** Voice v's LFO shape `shapeAt (phase)` on channel c for this sample; during a jump fade
        the shape at the old phase (plus, after a jump inside a fade, the offset that makes it
        the blend heard at that jump) crossfaded into the shape at the new one. */
    template <typename ShapeAt>
    forcedinline float lfo (int v, int c, ShapeAt&& shapeAt) noexcept
    {
        if (c == 1 && juce::exactlyEqual (spreadCycles, 0.0))
        {
            // Without spread the right channel's LFO is the left one's, value for value.
            lastU[v][1] = lastU[v][0];
            jumpOffset[v][1] = jumpOffset[v][0];
            return leftU;
        }

        float u = shapeAt (ph[c]);

        if (k < 1.0f)
        {
            const float before = shapeAt (oldPh[c]);

            if (jumpRestart != 0)
                jumpOffset[v][c] = jumpRestart == 2 ? lastU[v][c] - before : 0.0f;

            const float from = before + jumpOffset[v][c];
            u = from + k * (u - from);
            // A restart (jumpRestart 2) only happens inside a fade, so only a fade needs this.
            lastU[v][c] = u;
        }

        leftU = u;
        return u;
    }

    /** Everything voices share for one sample: the knobs (only while one moves) and the LFO's
        phases. */
    forcedinline void beginSample (bool stereo) noexcept
    {
        if (knobsMoving)
        {
            updateDepth (depth.next());
            updateWidth (width.next(), stereo);
            updateShape (shape.next());
            waveMix = wave.next();
            updateCrossover (tone.next());
            knobsMoving = ! (depth.isSettled() && width.isSettled() && tone.isSettled() && shape.isSettled() && wave.isSettled());
        }

        k = jumpWeight();
        ph[0] = phase.get();
        ph[1] = rightPhase (ph[0]);

        if (jumpLeft > 0)
        {
            oldPh[0] = oldPhase.get();
            oldPh[1] = rightPhase (oldPh[0]);
        }
    }

    forcedinline void advancePhase() noexcept
    {
        const float r = syncRate > 0.0f ? syncRate : rate.next();
        currentRate = r;
        phase.advance (r);
        jumpRestart = 0;

        if (jumpLeft > 0)
        {
            oldPhase.advance (r);
            --jumpLeft;
        }
    }

    template <typename Step>
    void run (float* left, float* right, int n, float* traceLeft, float* traceRight, Step&& stepSample) noexcept
    {
        const bool stereo = right != nullptr;
        updateWidth (width.get(), stereo);   // a block may change from mono to stereo with the knobs at rest
        refreshLaw();

        for (int i = 0; i < n; ++i)
        {
            beginSample (stereo);
            stepSample (left[i], stereo ? right + i : nullptr);

            if (traceLeft != nullptr)
            {
                traceLeft[i] = gainOut[0];

                if (traceRight != nullptr)
                    traceRight[i] = stereo ? gainOut[1] : gainOut[0];
            }

            advancePhase();

            if (anyPrime)
            {
                anyPrime = false;

                for (auto& p : prime)
                    p = false;
            }
        }

        snapPending = freshPhase = false;
        lastBlock = n;
    }

    void step (int voice, float& l, float* r) noexcept
    {
        switch ((TremoloFx::Voice) voice)
        {
            case TremoloFx::Voice::optical:  stepOptical (l, r); break;
            case TremoloFx::Voice::bias:     stepBias (l, r); break;
            case TremoloFx::Voice::harmonic: stepHarmonic (l, r); break;
            case TremoloFx::Voice::smooth:
            case TremoloFx::Voice::classic:
            default:                         stepSmooth (l, r); break;
        }
    }

    /** The smoother coefficient for this sample: 1 on a primed voice's first sample, which
        snaps its gain onto its own trajectory. */
    float glideCoeff (int voice) const noexcept
    {
        return prime[voice] ? 1.0f : gainCoeff;
    }

    //==========================================================================
    static float raisedCosine (double x) noexcept
    {
        return 0.5f - 0.5f * std::cos (fx::kPi * (float) x);
    }

    /** Smooth's pulse, centred on the crest: high for frac (p - s/2) < s, its edges raised-
        cosine ramps of kPulseEdgeSeconds centred on the hard edges. Edge decisions stay in
        double so the pulse switches on the sample the reference model does. */
    forcedinline float pulseAt (double p) const noexcept
    {
        double q = p - 0.5 * skew;

        if (q < 0.0)
            q += 1.0;

        const double edge = kPulseEdgeSeconds * (double) currentRate;
        const double half = 0.5 * edge;

        if (q < half)
            return raisedCosine (0.5 + q / edge);

        if (q < skew - half)
            return 1.0f;

        if (q < skew + half)
            return 1.0f - raisedCosine (0.5 + (q - skew) / edge);

        if (q < 1.0 - half)
            return 0.0f;

        return raisedCosine (0.5 + (q - 1.0) / edge);
    }

    /** Smooth's shape at phase p, 1 = loud. */
    forcedinline float smoothShape (double p) const noexcept
    {
        const float pf = (float) p;
        const float w0 = p < skew ? riseScale * pf : 0.5f + fallScale * (pf - skewF);
        const float tri = 1.0f - std::abs (2.0f * w0 - 1.0f);

        if (waveMix < 0.5f)
        {
            const float sine = 0.5f - 0.5f * cosCycles (w0);
            return sine + 2.0f * waveMix * (tri - sine);
        }

        return tri + (2.0f * waveMix - 1.0f) * (pulseAt (p) - tri);
    }

    forcedinline void stepSmooth (float& l, float* r) noexcept
    {
        constexpr int v = (int) TremoloFx::Voice::smooth;
        const float coeff = glideCoeff (v);
        const auto shapeAt = [this] (double p) { return loudness (smoothShape (p)); };
        const auto target = [&] (int c) { return (floorGain + (1.0f - floorGain) * lfo (v, c, shapeAt)) * makeup; };

        gainOut[0] = smoothToward (smoothDev[0], coeff, target (0));
        l *= gainOut[0];

        if (r != nullptr)
        {
            gainOut[1] = smoothToward (smoothDev[1], coeff, target (1));
            *r *= gainOut[1];
        }
    }

    /** The photocell's steady periodic state at this sample's phase, each channel: where it
        would be had the voice been running, so a switch into Optical joins its trajectory. */
    void primeOptical() noexcept
    {
        const double cycle = (double) fs / (double) juce::jmax (kMinRateHz, currentRate);
        const double darkLog = std::log1p (-(double) opticalRise) * cycle;   // per cycle
        const double litLog = std::log1p (-(double) opticalFall) * cycle;
        const double a = std::exp (darkLog * kOpticalLitFrom), b = std::exp (litLog * (1.0 - kOpticalLitFrom));
        const double d = (double) opticalDepth;
        const double atDark = -d * (1.0 - b) / (1.0 - a * b);                  // as the lamp goes dark

        for (int c = 0; c < 2; ++c)
        {
            const double p = ph[c];
            photocellDev[c] = (float) (p < kOpticalLitFrom ? atDark * std::exp (darkLog * p)
                                                           : -d + (atDark * a + d) * std::exp (litLog * (p - kOpticalLitFrom)));
        }
    }

    forcedinline void stepOptical (float& l, float* r) noexcept
    {
        constexpr int v = (int) TremoloFx::Voice::optical;

        if (prime[v])
            primeOptical();

        const auto lit = [] (double p) { return p >= kOpticalLitFrom ? 1.0f : 0.0f; };

        const auto follow = [this, &lit] (int c)
        {
            // Held as the distance from unity, like the gain smoothers (smoothToward). The lamp
            // is either lit or dark, so the loudness law (u^e) leaves it unchanged.
            const float target = -opticalDepth * lfo (v, c, lit);
            auto& d = photocellDev[c];
            d += (target > d ? opticalRise : opticalFall) * (target - d) + fx::kAntiDenormal;
            return 1.0f + d;
        };

        gainOut[0] = follow (0);
        l *= gainOut[0];

        if (r != nullptr)
        {
            gainOut[1] = follow (1);
            *r *= gainOut[1];
        }
    }

    forcedinline void stepBias (float& l, float* r) noexcept
    {
        constexpr int v = (int) TremoloFx::Voice::bias;
        const float coeff = glideCoeff (v);

        // [choice] the mean of the channels, so the knee means the same dBFS in mono and stereo.
        const float mono = r != nullptr ? 0.5f * (l + *r) : l;
        detectorLow += detectorCoeff * (mono - detectorLow) + fx::kAntiDenormal;
        const float level = std::abs (mono - detectorLow);
        envelope += (level > envelope ? biasAttack : biasRelease) * (level - envelope) + fx::kAntiDenormal;
        const float q = envelope * inverseKnee;

        // No loudness law here [coordinator decision, deviates from §4.7.4]: the gain is
        // already the square root of a law linear in u, so at spread 180 L^2 + R^2 is
        // constant at any depth and level; u^e would add up to 2 dB of ripple. A division,
        // not a reciprocal: at depth 0 it is exactly 1, so the voice is transparent.
        const auto dip = [] (double p) { return 0.5f + 0.5f * cosCycles ((float) p); };
        const auto target = [&] (int c) { return std::sqrt (juce::jmax (0.0f, (1.0f - cachedDepth * lfo (v, c, dip) + q) / (1.0f + q))); };

        gainOut[0] = smoothToward (biasDev[0], coeff, target (0));
        l *= gainOut[0];

        if (r != nullptr)
        {
            gainOut[1] = smoothToward (biasDev[1], coeff, target (1));
            *r *= gainOut[1];
        }
    }

    forcedinline void stepHarmonic (float& l, float* r) noexcept
    {
        constexpr int v = (int) TremoloFx::Voice::harmonic;

        if (prime[v])
            lowpass[0] = lowpass[1] = 0.0f;

        const float coeff = glideCoeff (v);
        const auto rise = [] (double p) { return 0.5f - 0.5f * cosCycles ((float) p); };

        // No loudness law here either [coordinator decision, deviates from §4.7.4]: the
        // band powers are linear in u, so at spread 180 L^2 + R^2 is constant for any
        // spectrum; u^e would add up to 1.3 dB of ripple.
        const auto band = [&] (int c, float x)
        {
            const float u = lfo (v, c, rise);
            gainOut[c] = smoothToward (lowDev[c], coeff, harmonicNorm * std::sqrt (juce::jmax (0.0f, 1.0f - cachedDepth * u)));
            const float highGain = smoothToward (highDev[c], coeff, harmonicNorm * std::sqrt (juce::jmax (0.0f, 1.0f - cachedDepth * (1.0f - u))));

            // TPT one-pole: L = low-pass, H = x - L. Written as gH * x + (gL - gH) * L, so
            // equal band gains of exactly 1 return x itself.
            const float t = (x - lowpass[c]) * crossoverG;
            const float low = t + lowpass[c];
            lowpass[c] = low + t + fx::kAntiDenormal;
            return highGain * x + (gainOut[c] - highGain) * low;
        };

        l = band (0, l);

        if (r != nullptr)
            *r = band (1, *r);
    }

    //==========================================================================
    float fs = 48000.0f;
    LfoPhase phase, oldPhase;
    Knob rate, depth, width, tone, shape, wave;
    bool snapPending = true, freshPhase = true, knobsMoving = true;
    int jumpLength = 1, jumpLeft = 0, jumpRestart = 0, lastBlock = 0, jumps = 0;

    // Song-position lock.
    bool hostValid = false;
    double hostPrev = 0.0, hostPrevRate = 0.0;
    float syncRate = 0.0f, currentRate = 1.0f;

    float gainCoeff = 1.0f, opticalFall = 1.0f, opticalRise = 1.0f, biasAttack = 1.0f, biasRelease = 1.0f, detectorCoeff = 1.0f;

    // Values derived from the smoothed knobs, recomputed only when a knob moves.
    float cachedDepth = -1.0f, cachedWidth = -1.0f, cachedSkew = -1.0f, cachedTone = -1.0f;
    float floorGain = 1.0f, makeup = 1.0f, opticalDepth = 0.0f, harmonicNorm = 1.0f;
    double spreadCycles = 0.0, skew = 0.5;
    float loudnessExponent = 1.0f, skewF = 0.5f, riseScale = 1.0f, fallScale = 1.0f, waveMix = 0.0f;
    static constexpr int kLawSize = 1024;
    float law[kLawSize + 1] {}, lawExponent = -1.0f;
    float inverseKnee = 1.0f, crossoverG = 0.5f;

    // This sample's shared context (beginSample).
    double ph[2] {}, oldPh[2] {};
    float k = 1.0f, leftU = 0.0f;

    // Per voice: primed for its first sample, its last LFO value and its jump offset per channel.
    bool prime[numShaped] {}, anyPrime = false;
    float lastU[numShaped][2] {}, jumpOffset[numShaped][2] {};

    // Audio state, gains held as distances from unity. gainOut[] is what the last voice
    // stepped applied to each channel (Harmonic: its low band), for the trace.
    float gainOut[2] { 1.0f, 1.0f };
    float smoothDev[2] {}, biasDev[2] {}, lowDev[2] {}, highDev[2] {}, photocellDev[2] {};
    float lowpass[2] {};
    float envelope = 0.0f, detectorLow = 0.0f;
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
    TremoloParams params;
    juce::AudioBuffer<float> outgoing;   // the outgoing side's output while Classic is in a switch
    float* traceLeft = nullptr;
    float* traceRight = nullptr;
    int maxBlock = 0;
    int voice = 1, fromVoice = 1, fadeLeft = 0, fadeLength = 1;
    bool snapVoice = true;

    void startSwitch (int to) noexcept
    {
        fromVoice = voice;
        voice = to;
        fadeLeft = fadeLength;

        // The LFO carries over, so the new voice continues the same cycle.
        if (fromVoice == (int) Voice::classic)
            shaped.enterAt ((double) classic.getPhase() - kClassicDipPhase);
        else if (to == (int) Voice::classic)
            classic.setPhase ((float) frac (shaped.getPhase() + kClassicDipPhase));

        if (to != (int) Voice::classic)
            shaped.startVoice (to);
    }

    /** Weight of the incoming voice at this sample of the block: a raised cosine. */
    float fadeWeight (int sampleInBlock) const noexcept
    {
        const int done = fadeLength - fadeLeft + sampleInBlock + 1;

        if (done >= fadeLength)
            return 1.0f;

        return 0.5f - 0.5f * std::cos (fx::kPi * (float) done / (float) fadeLength);
    }

    void runShaped (int v, const juce::dsp::AudioBlock<float>& block, int numChannels, bool traced) noexcept
    {
        shaped.process (v, block.getChannelPointer (0), numChannels > 1 ? block.getChannelPointer (1) : nullptr,
                        (int) block.getNumSamples(), traced ? traceLeft : nullptr, traced ? traceRight : nullptr);
    }
};

TremoloFx::TremoloFx() : impl (std::make_unique<Impl>()) {}
TremoloFx::~TremoloFx() = default;

void TremoloFx::prepare (const juce::dsp::ProcessSpec& spec)
{
    impl->classic.prepare (spec);
    impl->shaped.prepare (spec.sampleRate);
    impl->shaped.setTargets (impl->params);
    impl->outgoing.setSize (juce::jmax (1, (int) spec.numChannels), juce::jmax (1, (int) spec.maximumBlockSize));
    impl->fadeLength = juce::jmax (1, (int) std::lround (kVoiceFadeSeconds * spec.sampleRate));
    impl->maxBlock = (int) spec.maximumBlockSize;
    impl->fadeLeft = 0;
    impl->snapVoice = true;
}

void TremoloFx::reset()
{
    impl->classic.reset();
    impl->shaped.reset();
    // The knobs restart at the last values the host sent, in case process() comes next.
    impl->shaped.setTargets (impl->params);
    // Both voices are clean, so the next block starts on the requested voice outright
    // rather than finishing a switch that was in flight.
    impl->fadeLeft = 0;
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

int TremoloFx::getSyncJumpCount() const noexcept { return impl->shaped.getJumpCount(); }
double TremoloFx::getLfoPhase() const noexcept { return impl->shaped.getPhase(); }

void TremoloFx::process (juce::dsp::AudioBlock<float> block)
{
    auto& s = *impl;
    const int numSamples = (int) block.getNumSamples();
    const int numChannels = juce::jmin (2, (int) block.getNumChannels());

    if (numSamples == 0 || numChannels == 0)
        return;

    // No instrument produces a sample beyond 1e6; such a sample, like a non-finite one, is
    // silenced before any state or gain sees it (N14). Everything below passes untouched.
    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
    {
        auto* data = block.getChannelPointer (ch);

        for (int i = 0; i < numSamples; ++i)
            data[i] = fx::sanitise (data[i]);
    }

    const int requested = s.params.voice;

    if (s.snapVoice)
    {
        // Nothing has been heard since prepare() or reset(): start on the requested voice.
        s.snapVoice = false;
        s.voice = s.fromVoice = requested;
        s.fadeLeft = 0;
    }
    else if (requested != s.voice)
    {
        if (s.fadeLeft > 0 && requested == s.fromVoice)
        {
            // Asked back mid-switch: the same crossfade runs the other way from where it is.
            std::swap (s.voice, s.fromVoice);
            s.fadeLeft = s.fadeLength - s.fadeLeft;
        }
        else if (s.fadeLeft == 0)
        {
            s.startSwitch (requested);

            if (requested == (int) Voice::bias)
                s.shaped.primeBias (block.getChannelPointer (0), numChannels > 1 ? block.getChannelPointer (1) : nullptr, numSamples);
        }
        // Otherwise the switch in flight finishes first.
    }

    // A block larger than prepare() promised cannot be crossfaded without the heap.
    if (numSamples > s.maxBlock || (int) block.getNumChannels() > s.outgoing.getNumChannels())
        s.fadeLeft = 0;

    const bool traced = s.traceLeft != nullptr && numSamples <= s.maxBlock;
    const bool shapedNow = s.voice != (int) Voice::classic;
    const bool shapedBefore = s.fadeLeft > 0 && s.fromVoice != (int) Voice::classic;

    if (shapedNow || shapedBefore)
        s.shaped.syncTo (s.params.syncPhase, s.params.rateHz, numSamples);

    s.classic.setParameters (s.params.rateHz, s.params.depth, s.params.mix);

    if (s.fadeLeft == 0)
    {
        if (shapedNow)
            s.runShaped (s.voice, block, numChannels, traced);
        else
            s.classic.process (block);

        return;
    }

    if (shapedNow && shapedBefore)
    {
        s.shaped.processPair (s.fromVoice, s.voice, block.getChannelPointer (0),
                              numChannels > 1 ? block.getChannelPointer (1) : nullptr, numSamples,
                              traced ? s.traceLeft : nullptr, traced ? s.traceRight : nullptr,
                              [&s] (int i) { return s.fadeWeight (i); });
    }
    else
    {
        // Classic on one side: the outgoing side renders a copy of the input.
        const auto channels = block.getNumChannels();
        auto before = juce::dsp::AudioBlock<float> (s.outgoing).getSubsetChannelBlock (0, channels).getSubBlock (0, (size_t) numSamples);
        before.copyFrom (block);

        if (shapedNow)
        {
            s.classic.process (before);
            s.runShaped (s.voice, block, numChannels, traced);
        }
        else
        {
            s.runShaped (s.fromVoice, before, numChannels, false);
            s.classic.process (block);
        }

        for (size_t ch = 0; ch < channels; ++ch)
        {
            auto* data = block.getChannelPointer (ch);
            const auto* old = before.getChannelPointer (ch);

            for (int i = 0; i < numSamples; ++i)
                data[i] = old[i] + s.fadeWeight (i) * (data[i] - old[i]);
        }
    }

    s.fadeLeft = juce::jmax (0, s.fadeLeft - numSamples);
}
} // namespace tubamp
