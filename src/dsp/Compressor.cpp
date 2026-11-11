#include "Compressor.h"

#include "FxPrimitives.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace tubamp
{
namespace
{
//==============================================================================
// Classic (type 0) is the processor's compressor code moved here verbatim
// (docs/FX-ENGINES.md §2.3, §4.2.3). Its expressions, float types and call order are
// frozen: the golden renders compare its output bit for bit.

constexpr double kGainSmoothingSeconds = 0.02;

/** Applies a smoothed gain to the whole buffer as a single ramp — one pass, no zipper. */
void applySmoothedGain (juce::AudioBuffer<float>& buffer, int numSamples,
                        juce::SmoothedValue<float>& smoothed)
{
    const float start = smoothed.getCurrentValue();
    const float end = smoothed.skip (numSamples);

    if (juce::approximatelyEqual (start, end))
        buffer.applyGain (0, numSamples, start);
    else
        buffer.applyGainRamp (0, numSamples, start, end);
}

//==============================================================================
// Clean, Squash, FET and Opto (docs/FX-ENGINES.md §4.2.4, §4.2.5).

// Detector window W = tauR / 4 within [1, 12.5] ms: longer than half a period of any
// tone above 40 Hz, short enough that a fast release stays fast. [choice; sim: §4.2.9]
constexpr float kWindowPerRelease = 0.25f;
constexpr float kMinWindowSeconds = 0.001f;
constexpr float kMaxWindowSeconds = 0.0125f;

constexpr float kLevelFloor = 1.0e-6f;             // -120 dB [contract §4.2.4]
constexpr float kScHpfOffHz = 20.5f;               // at or below: detector filter off [contract §4.2.2]
constexpr float kInputLimit = 1.0e6f;              // +120 dBFS: bounds every product on broken input [choice]
constexpr float kRampSeconds = 0.02f;              // threshold and slope, linear [study: R6]
constexpr float kSmoothSeconds = 0.02f;            // knee, mix, sc hpf, auto makeup amount [choice: §2.5]
constexpr int kControlInterval = 16;               // auto makeup and sc hpf updates [contract §4.2.4]
constexpr int kSwitchSectionSamples = 32;          // granularity of a type switch's reset point [choice]
constexpr float kAutoMakeupReferenceDb = -10.0f;   // guitar peak level [study: R3, guess]

constexpr float kFetOffsetDbPerOctave = 2.6f;      // threshold rise per doubling of R [fit: R5, 1176 curve_ratio1/4]
constexpr float kFetOffsetFromRatio = 4.0f;
constexpr float kFetSlowRatio = 6.4f;              // tauR2 / tauR1 [fit: R5, 1176 Rev A t_a100_r100]
constexpr float kFetFastWeight = 0.7f;
constexpr float kOptoSlowRatio = 8.3f;             // [fit: R5, LA-2A Gray t_comp_pr50]
constexpr float kOptoFastWeight = 0.6f;

// The share of the detector window taken off each release time constant, so that a
// release measured from the end of a note keeps the knob's meaning: all of it for the
// single-stage types (t63 = knob) [contract §4.2.4]. For the two-stage types only half
// of it: the R5 fits were made on a detector that already held for 8 ms, and with the
// whole window removed the curves land at 58/84 ms (FET t50/t63) and 78/126 ms (Opto),
// below both reference bands. [fit: R5 bands, see check_comp.py C14/C15]
constexpr float kHoldCompensationSingle = 1.0f;
constexpr float kHoldCompensationTwoStage = 0.5f;

constexpr float kDbToLog2 = 0.166096404744368f;    // log2(10) / 20

/** Soft-knee gain computer (Giannoulis, Massberg, Reiss, JAES 2012): o is the level over
    the threshold, s the slope (1/R - 1, or 1 - R in feedback), wk the knee width. */
inline float gainComputer (float o, float s, float wk) noexcept
{
    if (2.0f * o <= -wk)
        return 0.0f;

    if (2.0f * o < wk)
    {
        const float u = o + 0.5f * wk;
        return s * u * u / (2.0f * wk);
    }

    return s * o;
}

/** Steady-state gain of the feedback loop g = gc(o + g) with slope s = 1 - R: the static
    curve Squash really has. The knee acts on the output level, so it spans R + 1 times
    half the knee on the input side and the feed-forward formula is up to ~1 dB off. */
inline float feedbackStaticGain (float o, float s, float wk) noexcept
{
    const float ratio = 1.0f - s;
    const float h = 0.5f * wk;

    if (o <= -h)
        return 0.0f;

    if (wk <= 0.0f || o >= ratio * h)
        return s * o / ratio;

    // Knee: with u = o + g + h, u - (o + h) = s u^2 / (2 wk); the positive root.
    const float a = -s / (2.0f * wk);
    const float c = o + h;
    const float u = 2.0f * c / (1.0f + std::sqrt (1.0f + 4.0f * a * c));
    return u - c;
}

/** Linear ramp over a fixed number of samples; a new target restarts it from wherever it is. */
class LinearRamp
{
public:
    void setLength (int samples) noexcept { length = juce::jmax (1, samples); }

    void setTarget (float value) noexcept
    {
        if (juce::exactlyEqual (value, target))
            return;

        target = value;
        remaining = length;
        step = (target - current) / (float) length;
    }

    void snap() noexcept
    {
        current = target;
        remaining = 0;
    }

    float getCurrent() const noexcept { return current; }

    float next() noexcept
    {
        if (remaining > 0)
            current = --remaining == 0 ? target : current + step;

        return current;
    }

private:
    float current = 0.0f, target = 0.0f, step = 0.0f;
    int length = 1, remaining = 0;
};

/** One-pole smoother that lands exactly on its target (so mix = 100 % really is wet only). */
class OnePoleSmoother
{
public:
    void setTimeConstant (float seconds, float updateRate) noexcept
    {
        a = 1.0f - std::exp (-1.0f / juce::jmax (1.0f, seconds * updateRate));
    }

    void setTarget (float value) noexcept { target = value; }
    void snap() noexcept { current = target; }
    float getCurrent() const noexcept { return current; }

    float next() noexcept
    {
        current += a * (target - current);

        if (std::abs (target - current) < 1.0e-6f)
            current = target;

        return current;
    }

private:
    float a = 1.0f, current = 0.0f, target = 0.0f;
};

/** Exact maximum of the last `length` inputs: a monotonic deque in a ring buffer,
    amortised O(1) per sample. */
class WindowMax
{
public:
    void setCapacity (int maxLength)
    {
        const int size = fx::nextPow2 (maxLength + 2);
        values.assign ((size_t) size, 0.0f);
        stamps.assign ((size_t) size, 0u);
        mask = (uint32_t) size - 1u;
        clear();
    }

    void clear() noexcept { head = tail = now = 0u; }

    void setLength (int samples) noexcept { length = (uint32_t) juce::jlimit (1, (int) mask - 1, samples); }

    float push (float x) noexcept
    {
        while (tail != head && values[(tail - 1u) & mask] <= x)
            --tail;

        values[tail & mask] = x;
        stamps[tail & mask] = now;
        ++tail;

        while (now - stamps[head & mask] >= length)
            ++head;

        ++now;
        return values[head & mask];
    }

private:
    std::vector<float> values { 0.0f, 0.0f };
    std::vector<uint32_t> stamps { 0u, 0u };
    uint32_t mask = 1u, length = 1u, head = 0u, tail = 0u, now = 0u;
};

CompParams clampParams (const CompParams& in) noexcept
{
    const auto clampFinite = [] (float v, float lo, float hi, float fallback)
    {
        return std::isfinite (v) ? juce::jlimit (lo, hi, v) : fallback;
    };

    CompParams p;
    p.type = juce::jlimit (0, CompFx::numTypes - 1, in.type);
    p.thresholdDb = clampFinite (in.thresholdDb, -60.0f, 0.0f, -20.0f);
    p.ratio = clampFinite (in.ratio, 1.0f, 20.0f, 4.0f);
    p.attackMs = clampFinite (in.attackMs, 0.1f, 100.0f, 5.0f);
    p.releaseMs = clampFinite (in.releaseMs, 10.0f, 1000.0f, 120.0f);
    p.makeupDb = clampFinite (in.makeupDb, 0.0f, 24.0f, 0.0f);
    p.kneeDb = clampFinite (in.kneeDb, 0.0f, 24.0f, 6.0f);
    p.mixPct = clampFinite (in.mixPct, 0.0f, 100.0f, 100.0f);
    p.scHpfHz = clampFinite (in.scHpfHz, 20.0f, 300.0f, 20.0f);
    p.autoMakeup = in.autoMakeup;
    return p;
}
} // namespace

//==============================================================================
struct CompFx::Impl
{
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = (float) spec.sampleRate;
        numChannels = (int) spec.numChannels;

        compressor.prepare (spec);
        compressor.reset();
        shadow.prepare (spec);

        makeup.reset (spec.sampleRate, kGainSmoothingSeconds);
        makeupPrimed = false;

        window.setCapacity ((int) std::ceil (kMaxWindowSeconds * sampleRate) + 2);
        const int rampSamples = juce::jmax (1, (int) std::lround (kRampSeconds * sampleRate));
        threshold.setLength (rampSamples);
        slope.setLength (rampSamples);
        knee.setTimeConstant (kSmoothSeconds, sampleRate);
        mix.setTimeConstant (kSmoothSeconds, sampleRate);
        autoMakeupDb.setTimeConstant (kSmoothSeconds, sampleRate);
        scCutoff.setTimeConstant (kSmoothSeconds, sampleRate / (float) kControlInterval);

        switcher.prepare (spec.sampleRate, (int) spec.maximumBlockSize, numChannels, params.type);
        reset();
    }

    void reset()
    {
        compressor.reset();
        shadow.reset();
        resetDynamics();
        controlPhase = 0;
        snapEngine = true;
        gainReductionDb = 0.0f;
    }

    void resetDynamics() noexcept
    {
        window.clear();

        for (auto& f : scFilter)
            f.reset();

        g = g1 = g2 = 0.0f;
        snapSmoothers = true;
    }

    /** Called by the switcher while nothing of the effect is audible. */
    void resetType (int type) noexcept
    {
        if (type == (int) Type::classic)
        {
            compressor.reset();
            shadow.reset();
            return;
        }

        resetDynamics();
        updateDynamics ((Type) type);
        snapDynamics ((Type) type);
    }

    //==============================================================================
    void setParameters (const CompParams& newParams) noexcept
    {
        params = clampParams (newParams);

        if (! makeupPrimed)
        {
            makeup.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (params.makeupDb));
            makeupPrimed = true;
        }
    }

    void process (juce::dsp::AudioBlock<float> block)
    {
        if (snapEngine)
        {
            // Nothing has been heard since prepare() or reset(): start on the requested type.
            snapEngine = false;
            activeType = params.type;
            switcher.snapTo (params.type);
        }

        gainReductionDb = 0.0f;
        const auto numSamples = (int) block.getNumSamples();

        if (params.type == activeType)
        {
            processSection (block, 0);
        }
        else
        {
            // The switcher can only reset the incoming type at the start of a call, so
            // while a switch is due the block goes through it in short sections: the
            // fade then takes its 40 ms whatever the host's block size.
            for (int start = 0; start < numSamples; start += kSwitchSectionSamples)
                processSection (block.getSubBlock ((size_t) start, (size_t) juce::jmin (kSwitchSectionSamples, numSamples - start)),
                                start);
        }

        trace = nullptr;
    }

    void processSection (juce::dsp::AudioBlock<float> block, int offset)
    {
        const int type = switcher.begin (block, params.type, [this] (int next, bool)
        {
            resetType (next);
            return true;
        });

        activeType = type;
        float* const sectionTrace = trace != nullptr ? trace + offset : nullptr;

        // While a switch fades out, `type` is still the outgoing one: it keeps running
        // on its own settings until it is silent.
        if (type == (int) Type::classic)
        {
            measureClassic (block, sectionTrace);
            processClassic (block);
        }
        else
        {
            updateDynamics ((Type) type);

            if (snapSmoothers)
                snapDynamics ((Type) type);

            switch ((Type) type)
            {
                case Type::squash: runDynamics<Type::squash> (block, sectionTrace); break;
                case Type::fet:    runDynamics<Type::fet> (block, sectionTrace); break;
                case Type::opto:   runDynamics<Type::opto> (block, sectionTrace); break;
                case Type::classic:
                case Type::clean:
                default:           runDynamics<Type::clean> (block, sectionTrace); break;
            }
        }

        switcher.end (block);
    }

    void skip (int numSamples) noexcept
    {
        makeup.setTargetValue (juce::Decibels::decibelsToGain (params.makeupDb));
        makeup.skip (numSamples);
        gainReductionDb = 0.0f;
        trace = nullptr;

        if (snapEngine)
            return;

        // Nothing is audible while bypassed, so a type change completes without a fade.
        if (params.type != activeType)
        {
            resetType (params.type);
            switcher.snapTo (params.type);
            activeType = params.type;
        }
        else if (params.type != (int) Type::classic)
        {
            updateDynamics ((Type) params.type);
            snapDynamics ((Type) params.type);
        }
    }

    //==============================================================================
    // Classic

    void processClassic (juce::dsp::AudioBlock<float>& block)
    {
        const auto numSamples = (int) block.getNumSamples();
        juce::dsp::ProcessContextReplacing<float> context (block);

        compressor.setThreshold (params.thresholdDb);
        compressor.setRatio (juce::jmax (1.0f, params.ratio));
        compressor.setAttack (juce::jmax (0.0f, params.attackMs));
        compressor.setRelease (juce::jmax (0.0f, params.releaseMs));
        compressor.process (context);

        // The processor applied the makeup through an AudioBuffer over the same channels;
        // a non-owning view keeps applyGain's approximatelyEqual shortcut identical.
        float* channels[2] { block.getChannelPointer (0),
                             block.getNumChannels() > 1 ? block.getChannelPointer (1) : nullptr };
        juce::AudioBuffer<float> buf (channels, juce::jmin (2, (int) block.getNumChannels()), numSamples);

        makeup.setTargetValue (juce::Decibels::decibelsToGain (params.makeupDb));
        applySmoothedGain (buf, numSamples, makeup);
    }

    /** The Classic meter: a copy of juce::dsp::Compressor's own envelope filter, fed the
        same input, read through the same gain law. The audio path never sees it. */
    void measureClassic (const juce::dsp::AudioBlock<float>& block, float* gainTrace) noexcept
    {
        const auto numSamples = (int) block.getNumSamples();
        const int channels = juce::jmin (2, (int) block.getNumChannels());
        const float thresholdLin = juce::Decibels::decibelsToGain (params.thresholdDb, -200.0f);
        const float ratioInverse = 1.0f / juce::jmax (1.0f, params.ratio);

        shadow.setAttackTime (juce::jmax (0.0f, params.attackMs));
        shadow.setReleaseTime (juce::jmax (0.0f, params.releaseMs));

        const auto grFor = [thresholdLin, ratioInverse] (float env) noexcept
        {
            if (env < thresholdLin)
                return 0.0f;

            return -20.0f * std::log10 (std::pow (env / thresholdLin, ratioInverse - 1.0f));
        };

        float envMax = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            float env = 0.0f;

            for (int ch = 0; ch < channels; ++ch)
                env = juce::jmax (env, shadow.processSample (ch, block.getSample (ch, i)));

            envMax = juce::jmax (envMax, env);

            if (gainTrace != nullptr)
                gainTrace[i] = grFor (env);
        }

        gainReductionDb = juce::jmax (gainReductionDb, grFor (envMax));
    }

    //==============================================================================
    // Clean, Squash, FET, Opto

    /** Per-block targets and coefficients for `type` from the stored parameters. */
    void updateDynamics (Type type) noexcept
    {
        const float ratio = params.ratio;
        float thresholdTarget = params.thresholdDb;

        if (type == Type::fet && ratio > kFetOffsetFromRatio)
            thresholdTarget += kFetOffsetDbPerOctave * std::log2 (ratio / kFetOffsetFromRatio);

        threshold.setTarget (thresholdTarget);
        slope.setTarget (type == Type::squash ? 1.0f - ratio : 1.0f / ratio - 1.0f);
        knee.setTarget (params.kneeDb);
        mix.setTarget (params.mixPct * 0.01f);
        scCutoff.setTarget (params.scHpfHz);
        autoMakeup = params.autoMakeup;

        const bool filterOn = params.scHpfHz > kScHpfOffHz;

        if (filterOn && ! scFilterOn)
        {
            for (auto& f : scFilter)
                f.reset();

            scCutoff.snap();
            appliedCutoff = -1.0f;
        }

        scFilterOn = filterOn;

        const float tauRelease = params.releaseMs * 0.001f;
        const int windowSamples = juce::jmax (1, (int) std::lround (juce::jlimit (kMinWindowSeconds, kMaxWindowSeconds,
                                                                                    kWindowPerRelease * tauRelease) * sampleRate));
        window.setLength (windowSamples);

        const auto coefficient = [this] (float tau) noexcept
        {
            return 1.0f - std::exp (-1.0f / juce::jmax (1.0f, tau * sampleRate));
        };

        const float hold = (float) windowSamples / sampleRate;
        const bool twoStage = type == Type::fet || type == Type::opto;
        const float compensation = (twoStage ? kHoldCompensationTwoStage : kHoldCompensationSingle) * hold;

        attackK = coefficient (params.attackMs * 0.001f);
        releaseK1 = coefficient (tauRelease - compensation);
        releaseK2 = coefficient (tauRelease * (type == Type::opto ? kOptoSlowRatio : kFetSlowRatio) - compensation);
        fastWeight = type == Type::opto ? kOptoFastWeight : kFetFastWeight;
    }

    /** Steady-state auto makeup M = -gc(-10 dBFS - T) on the type's real static curve. */
    float autoMakeupFor (Type type) const noexcept
    {
        const float o = kAutoMakeupReferenceDb - threshold.getCurrent();

        if (type == Type::squash)
            return -feedbackStaticGain (o, slope.getCurrent(), knee.getCurrent());

        return -gainComputer (o, slope.getCurrent(), knee.getCurrent());
    }

    void updateDetectorFilter() noexcept
    {
        if (! scFilterOn)
            return;

        const float cutoff = scCutoff.next();

        if (! juce::exactlyEqual (cutoff, appliedCutoff))
        {
            for (auto& f : scFilter)
                f.setCutoff (cutoff, sampleRate);

            appliedCutoff = cutoff;
        }
    }

    void snapDynamics (Type type) noexcept
    {
        threshold.snap();
        slope.snap();
        knee.snap();
        mix.snap();
        scCutoff.snap();
        appliedCutoff = -1.0f;
        updateDetectorFilter();
        autoMakeupDb.setTarget (autoMakeup ? autoMakeupFor (type) : 0.0f);
        autoMakeupDb.snap();
        snapSmoothers = false;
    }

    template <Type type>
    void runDynamics (juce::dsp::AudioBlock<float>& block, float* gainTrace) noexcept
    {
        const auto numSamples = (int) block.getNumSamples();
        float* left = block.getChannelPointer (0);
        float* right = block.getNumChannels() > 1 && numChannels > 1 ? block.getChannelPointer (1) : nullptr;
        const bool filterOn = scFilterOn;
        float maxReduction = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            if (controlPhase == 0)
            {
                updateDetectorFilter();
                autoMakeupDb.setTarget (autoMakeup ? autoMakeupFor (type) : 0.0f);
            }

            controlPhase = (controlPhase + 1) % kControlInterval;

            const float thr = threshold.next();
            const float s = slope.next();
            const float wk = knee.next();
            const float wet = mix.next();
            const float autoDb = autoMakeupDb.next();
            const float makeupGain = makeup.getNextValue();

            const float inL = juce::jlimit (-kInputLimit, kInputLimit, left[i]);
            const float inR = right != nullptr ? juce::jlimit (-kInputLimit, kInputLimit, right[i]) : 0.0f;
            float detector;

            if (filterOn)
            {
                detector = std::abs (scFilter[0].highpass (inL));

                if (right != nullptr)
                    detector = juce::jmax (detector, std::abs (scFilter[1].highpass (inR)));
            }
            else
            {
                detector = juce::jmax (std::abs (inL), std::abs (inR));
            }

            const float level = 20.0f * std::log10 (juce::jmax (window.push (detector), kLevelFloor));
            float gain;

            if constexpr (type == Type::squash)
            {
                // Log-domain feedback: the gain computer sees the output level L + g. The
                // attack step is divided by R, the loop gain, so the closed loop moves at
                // the knob's time constant whatever the ratio, and never overshoots.
                const float gc = gainComputer (level + g - thr, s, wk);
                g += (gc < g ? attackK / (1.0f - s) : releaseK1) * (gc - g);
                gain = g;
            }
            else if constexpr (type == Type::fet || type == Type::opto)
            {
                const float gc = gainComputer (level - thr, s, wk);
                g1 += (gc < g1 ? attackK : releaseK1) * (gc - g1);
                g2 += (gc < g2 ? attackK : releaseK2) * (gc - g2);
                gain = fastWeight * g1 + (1.0f - fastWeight) * g2;
            }
            else
            {
                const float gc = gainComputer (level - thr, s, wk);
                g += (gc < g ? attackK : releaseK1) * (gc - g);
                gain = g;
            }

            const float linear = std::exp2 ((gain + autoDb) * kDbToLog2) * makeupGain;

            if (juce::exactlyEqual (wet, 1.0f))
            {
                left[i] = inL * linear;

                if (right != nullptr)
                    right[i] = inR * linear;
            }
            else
            {
                left[i] = wet * (inL * linear) + (1.0f - wet) * inL;

                if (right != nullptr)
                    right[i] = wet * (inR * linear) + (1.0f - wet) * inR;
            }

            maxReduction = juce::jmax (maxReduction, -gain);

            if (gainTrace != nullptr)
                gainTrace[i] = -gain;
        }

        // A gain state decays toward 0 dB forever after the input stops; flushing keeps it
        // out of denormal range without FTZ, and sanitising keeps a bad value out of it.
        const auto settle = [] (float& state) noexcept
        {
            state = fx::sanitise (state);

            if (std::abs (state) < 1.0e-15f)
                state = 0.0f;
        };

        settle (g);
        settle (g1);
        settle (g2);
        gainReductionDb = juce::jmax (gainReductionDb, maxReduction);
    }

    //==============================================================================
    CompParams params;
    float sampleRate = 48000.0f;
    int numChannels = 2;

    fx::EngineSwitcher switcher;
    int activeType = (int) Type::clean;
    bool snapEngine = true;

    juce::SmoothedValue<float> makeup { 1.0f };
    bool makeupPrimed = false;

    juce::dsp::Compressor<float> compressor;
    juce::dsp::BallisticsFilter<float> shadow;

    WindowMax window;
    fx::TptOnePole scFilter[2];
    LinearRamp threshold, slope;
    OnePoleSmoother knee, mix, autoMakeupDb, scCutoff;
    float appliedCutoff = -1.0f;
    bool scFilterOn = false, autoMakeup = false, snapSmoothers = true;
    int controlPhase = 0;

    float attackK = 1.0f, releaseK1 = 1.0f, releaseK2 = 1.0f, fastWeight = 1.0f;
    float g = 0.0f, g1 = 0.0f, g2 = 0.0f;

    float gainReductionDb = 0.0f;
    float* trace = nullptr;
};

//==============================================================================
CompFx::CompFx() : impl (std::make_unique<Impl>()) {}
CompFx::~CompFx() = default;

void CompFx::prepare (const juce::dsp::ProcessSpec& spec) { impl->prepare (spec); }
void CompFx::reset() { impl->reset(); }
void CompFx::setParameters (const CompParams& params) { impl->setParameters (params); }
void CompFx::process (juce::dsp::AudioBlock<float> block) { impl->process (block); }
void CompFx::skip (int numSamples) noexcept { impl->skip (numSamples); }

float CompFx::getGainReductionDb() const noexcept { return impl->gainReductionDb; }
void CompFx::setGainTrace (float* destination) noexcept { impl->trace = destination; }
} // namespace tubamp
