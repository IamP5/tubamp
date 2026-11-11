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
constexpr float kAutoMakeupReferenceDb = -10.0f;   // guitar peak level [study: R3, guess]

// Manual plus auto makeup never exceeds this. Auto makeup alone reaches 47.5 dB (T -60, R 20)
// and manual adds 24: a note the compressor has not attacked yet would pass 71.5 dB of gain
// (2700 x full scale for a 41 Hz onset at 100 ms attack). 48 dB keeps a full-scale onset under
// 251 x, and never limits auto makeup alone. [choice: contract §2.2 bound of 1e3]
constexpr float kMaxTotalMakeupDb = 48.0f;

// A switch to or from Classic crossfades the two outputs (their states are separate); the
// switches between the other types carry the gain state instead and need no fade.
constexpr float kCrossfadeSeconds = 0.04f;         // the old fade-out plus fade-in [choice: §2.4]
// Classic cannot be seeded, so before it fades in it runs unheard from reset until its
// envelope has risen: four of its attack time constants (knob / 2 pi) plus 2 ms. [choice]
constexpr float kClassicWarmupAttacks = 4.0f;
constexpr float kClassicWarmupFloorSeconds = 0.002f;

constexpr float kFetOffsetDbPerOctave = 2.6f;      // threshold rise per doubling of R [fit: R5, 1176 curve_ratio1/4]
constexpr float kFetOffsetFromRatio = 4.0f;
constexpr float kFetSlowRatio = 6.4f;              // tauR2 / tauR1 [fit: R5, 1176 Rev A t_a100_r100]
constexpr float kOptoSlowRatio = 8.3f;             // [fit: R5, LA-2A Gray t_comp_pr50]

// Programme dependence: a memory m in [0, 1] charges toward the depth of the gain reduction
// (full at 6 dB) and discharges slowly; the slow stage's share of the release grows with it,
// as FET slow weight 0.43 m^2 and Opto 0.46 m. Steady compression is unchanged (both stages
// sit on the target); only the release after it gets longer the longer and deeper it was.
// [fit: comp/evidence/bb/ua_1176a/t_a100_r100.tbA/.tbL (t63 95 / 123 ms after 0.5 / 5 s) and
// the 56 ms after 0.1 s of verification.md C14: here 66 / 97 / 122 ms;
// ua_la2a_tc/t_comp_pr50.tbA/.tbL (157 -> 184 ms, x1.17): here 141 -> 162 ms, x1.15]
constexpr float kMemoryPerDb = 1.0f / 6.0f;
constexpr float kMemoryDischargeSeconds = 4.0f;
constexpr float kFetSlowWeightMax = 0.43f;
constexpr float kFetMemoryChargeSeconds = 0.28f;
constexpr float kOptoSlowWeightMax = 0.46f;
constexpr float kOptoMemoryChargeSeconds = 0.25f;

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
    the threshold, s the slope 1/R - 1, wk the knee width. */
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

/** Squash's gain computer, evaluated on the output level o (over the threshold) with the
    feed-forward slope s = 1/R - 1. Its knee is placed so that the loop's steady state is
    exactly the textbook curve gainComputer (input level, s, wk): the knee spans the knob's
    width on the INPUT level, centred on the threshold, as in every other type. Returns the
    gain and, in loopGain, 1 - d gain / d o: the loop gain the ballistics are divided by so
    the closed loop moves at the knobs' time constants (1 under the knee, R above it). */
inline float feedbackGain (float o, float s, float wk, float& loopGain) noexcept
{
    const float h = 0.5f * wk;
    const float inverseRatio = 1.0f + s;

    if (o <= -h)
    {
        loopGain = 1.0f;
        return 0.0f;
    }

    if (o >= h * inverseRatio)
    {
        loopGain = 1.0f / inverseRatio;
        return s * o / inverseRatio;
    }

    // Knee: with u = input level + h, o + h = u + a u^2 (a = s / 2 wk); the gain is a u^2.
    const float a = s / (2.0f * wk);
    const float c = o + h;
    const float u = 2.0f * c / (1.0f + std::sqrt (juce::jmax (0.0f, 1.0f + 4.0f * a * c)));
    loopGain = 1.0f / (1.0f + 2.0f * a * u);
    return c - u;
}

/** Replaces NaN and infinity with 0 so that nothing non-finite reaches a state. Finite
    input is left untouched (Classic stays bit for bit). */
inline void sanitiseInput (juce::dsp::AudioBlock<float>& block) noexcept
{
    const auto n = (int) block.getNumSamples();

    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
    {
        float* x = block.getChannelPointer (ch);
        float probe = 0.0f;

        for (int i = 0; i < n; ++i)
            probe += x[i] * 0.0f;

        if (std::isfinite (probe))
            continue;

        for (int i = 0; i < n; ++i)
            if (! std::isfinite (x[i]))
                x[i] = 0.0f;
    }
}

constexpr bool isTwoStage (int type) noexcept
{
    return type == (int) CompFx::Type::fet || type == (int) CompFx::Type::opto;
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

        const int maxBlock = juce::jmax (1, (int) spec.maximumBlockSize);
        incoming.setSize (juce::jmax (1, numChannels), maxBlock, false, true, false);
        incomingTrace.assign ((size_t) maxBlock, 0.0f);
        outgoingTrace.assign ((size_t) maxBlock, 0.0f);
        fadeStep = 1.0f / juce::jmax (1.0f, kCrossfadeSeconds * sampleRate);

        const auto coefficient = [this] (float tau) noexcept { return 1.0f - std::exp (-1.0f / (tau * sampleRate)); };
        memoryDischargeK = coefficient (kMemoryDischargeSeconds);
        fetMemoryChargeK = coefficient (kFetMemoryChargeSeconds);
        optoMemoryChargeK = coefficient (kOptoMemoryChargeSeconds);

        reset();
    }

    void reset()
    {
        compressor.reset();
        shadow.reset();
        resetDynamics();
        controlPhase = 0;
        snapEngine = true;
        scFilterOn = false;
        fading = false;
        gainReductionDb = 0.0f;
        classicGainReductionDb = 0.0f;
    }

    void resetDynamics() noexcept
    {
        window.clear();

        for (auto& f : scFilter)
            f.reset();

        g = g1 = g2 = memory = 0.0f;
        refill = 0;
        snapSmoothers = true;
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
        sanitiseInput (block);

        if (snapEngine)
        {
            // Nothing has been heard since prepare() or reset(): start on the requested type.
            snapEngine = false;
            activeType = params.type;
        }

        gainReductionDb = 0.0f;

        if (fading)
            retarget (params.type);
        else if (params.type != activeType)
            beginSwitch (params.type);

        if (! fading)
        {
            processType (activeType, block, trace, makeup);
        }
        else
        {
            const auto numSamples = (int) block.getNumSamples();
            const int capacity = incoming.getNumSamples();

            for (int start = 0; start < numSamples; start += capacity)
            {
                const int length = juce::jmin (capacity, numSamples - start);
                auto section = block.getSubBlock ((size_t) start, (size_t) length);
                float* const sectionTrace = trace != nullptr ? trace + start : nullptr;

                if (fading)
                    processFade (section, sectionTrace);
                else
                    processType (activeType, section, sectionTrace, makeup);
            }
        }

        trace = nullptr;
    }

    void skip (int numSamples) noexcept
    {
        makeup.setTargetValue (juce::Decibels::decibelsToGain (params.makeupDb));
        makeup.skip (numSamples);
        gainReductionDb = 0.0f;
        trace = nullptr;

        if (snapEngine)
            return;

        // Nothing is audible while bypassed, so a switch completes without a fade.
        if (fading)
        {
            fading = false;
            activeType = fadeTo;
        }

        if (params.type != activeType)
        {
            if (params.type == (int) Type::classic)
            {
                compressor.reset();
                shadow.reset();
            }
            else if (activeType == (int) Type::classic)
            {
                seedFromClassic ((Type) params.type);
            }
            else
            {
                carryGain (activeType, params.type);
            }

            activeType = params.type;
        }

        if (activeType != (int) Type::classic)
        {
            updateDynamics ((Type) activeType);
            snapDynamics();
        }
    }

    //==============================================================================
    // Type switches

    /** Clean, Squash, FET and Opto share the detector and the gain state, so a switch
        between them hands the gain over and the new type moves from it with its own
        ballistics. Anything with Classic crossfades the two outputs. */
    void beginSwitch (int requested) noexcept
    {
        if (activeType != (int) Type::classic && requested != (int) Type::classic)
        {
            carryGain (activeType, requested);
            activeType = requested;
            return;
        }

        fading = true;
        fadeFrom = activeType;
        fadeTo = requested;
        fadeWeight = 0.0f;
        prepareIncoming();
    }

    void prepareIncoming() noexcept
    {
        warmup = 0;

        if (fadeTo == (int) Type::classic)
        {
            compressor.reset();
            shadow.reset();
            const float attackSeconds = kClassicWarmupAttacks * params.attackMs * 0.001f / juce::MathConstants<float>::twoPi;
            warmup = (int) std::lround ((attackSeconds + kClassicWarmupFloorSeconds) * sampleRate);
        }
        else
        {
            seedFromClassic ((Type) fadeTo);
        }
    }

    /** A new request while a crossfade runs: within the incoming side it is a hand-over,
        back to the outgoing side the fade reverses from where it is. */
    void retarget (int requested) noexcept
    {
        if (requested == fadeTo)
            return;

        const bool towardClassic = requested == (int) Type::classic;

        if (towardClassic == (fadeTo == (int) Type::classic))
        {
            carryGain (fadeTo, requested);
            fadeTo = requested;
            return;
        }

        std::swap (fadeFrom, fadeTo);
        fadeWeight = 1.0f - fadeWeight;
        warmup = 0;

        if (requested != fadeTo)
        {
            carryGain (fadeTo, requested);
            fadeTo = requested;
        }
    }

    void processFade (juce::dsp::AudioBlock<float>& block, float* gainTrace)
    {
        const auto numSamples = (int) block.getNumSamples();
        const auto channels = block.getNumChannels();
        juce::dsp::AudioBlock<float> in (incoming.getArrayOfWritePointers(), channels, (size_t) numSamples);
        in.copyFrom (block);
        float* const outTrace = gainTrace != nullptr ? gainTrace : outgoingTrace.data();
        float* const inTrace = incomingTrace.data();
        const float reductionBefore = gainReductionDb;

        // Classic advances the shared makeup smoother itself; the other side steps a copy
        // over the same samples, so the smoother moves once per sample.
        auto makeupCopy = makeup;
        const bool classicIncoming = fadeTo == (int) Type::classic;
        processType (fadeFrom, block, outTrace, classicIncoming ? makeupCopy : makeup);
        processType (fadeTo, in, inTrace, classicIncoming ? makeup : makeupCopy);

        float w = fadeWeight, reduction = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            if (warmup > 0)
                --warmup;
            else
                w = juce::jmin (1.0f, w + fadeStep);

            for (size_t ch = 0; ch < channels; ++ch)
            {
                float* out = block.getChannelPointer (ch);
                out[i] += w * (in.getChannelPointer (ch)[i] - out[i]);
            }

            // The output is the linear blend of the two gains, so the reduction is blended the
            // same way; the larger of the two would overstate it by up to the difference.
            const float blended = (1.0f - w) * std::exp2 (-outTrace[i] * kDbToLog2) + w * std::exp2 (-inTrace[i] * kDbToLog2);
            outTrace[i] = -std::log2 (juce::jmax (blended, 1.0e-30f)) / kDbToLog2;
            reduction = juce::jmax (reduction, outTrace[i]);
        }

        fadeWeight = w;
        gainReductionDb = juce::jmax (reductionBefore, reduction);

        if (w >= 1.0f)
        {
            fading = false;
            activeType = fadeTo;
        }
    }

    /** The gain a new type is applying now, in dB. */
    float currentGain (int type) const noexcept
    {
        if (! isTwoStage (type))
            return g;

        return g1 + slowWeight ((Type) type, memory) * (g2 - g1);
    }

    void carryGain (int from, int to) noexcept
    {
        g = g1 = g2 = currentGain (from);

        if (! (isTwoStage (from) && isTwoStage (to)))
            memory = 0.0f;
    }

    /** Starts a new type at the reduction Classic was applying. The detector and the gain
        computer run from the first sample; until the window has filled, only attacks act
        (see runDynamics). */
    void seedFromClassic (Type type) noexcept
    {
        resetDynamics();
        updateDynamics (type);
        snapDynamics();
        g = g1 = g2 = -classicGainReductionDb;
        refill = windowSamples;
    }

    void processType (int type, juce::dsp::AudioBlock<float>& block, float* gainTrace,
                      juce::SmoothedValue<float>& makeupGain)
    {
        // While a switch fades out, `type` is still the outgoing one: it keeps running
        // on its own settings until it is silent.
        if (type == (int) Type::classic)
        {
            measureClassic (block, gainTrace);
            processClassic (block, makeupGain);
            return;
        }

        updateDynamics ((Type) type);

        if (snapSmoothers)
            snapDynamics();

        makeupGain.setTargetValue (juce::Decibels::decibelsToGain (params.makeupDb));

        switch ((Type) type)
        {
            case Type::squash: runDynamics<Type::squash> (block, gainTrace, makeupGain); break;
            case Type::fet:    runDynamics<Type::fet> (block, gainTrace, makeupGain); break;
            case Type::opto:   runDynamics<Type::opto> (block, gainTrace, makeupGain); break;
            case Type::classic:
            case Type::clean:
            default:           runDynamics<Type::clean> (block, gainTrace, makeupGain); break;
        }
    }

    //==============================================================================
    // Classic

    void processClassic (juce::dsp::AudioBlock<float>& block, juce::SmoothedValue<float>& makeupGain)
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

        makeupGain.setTargetValue (juce::Decibels::decibelsToGain (params.makeupDb));
        applySmoothedGain (buf, numSamples, makeupGain);
    }

    /** The Classic meter: a copy of juce::dsp::Compressor's own envelope filter, fed the
        same input, read through the same gain law. The audio path never sees it. Its input
        is limited like the other types' (a 3e38 sample overflowed env / threshold and gave
        an infinite reduction); that changes nothing below +120 dBFS. */
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

        float envMax = 0.0f, env = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            env = 0.0f;

            for (int ch = 0; ch < channels; ++ch)
                env = juce::jmax (env, shadow.processSample (ch, juce::jlimit (-kInputLimit, kInputLimit, block.getSample (ch, i))));

            envMax = juce::jmax (envMax, env);

            if (gainTrace != nullptr)
                gainTrace[i] = grFor (env);
        }

        gainReductionDb = juce::jmax (gainReductionDb, grFor (envMax));
        classicGainReductionDb = fx::sanitise (grFor (env));
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
        slope.setTarget (1.0f / ratio - 1.0f);
        knee.setTarget (params.kneeDb);
        mix.setTarget (params.mixPct * 0.01f);
        scCutoff.setTarget (params.scHpfHz);
        autoMakeup = params.autoMakeup;

        const bool filterOn = params.scHpfHz > kScHpfOffHz;
        const bool switchedOn = filterOn && ! scFilterOn;
        scFilterOn = filterOn;

        if (switchedOn)
        {
            // TptOnePole::reset() clears the state but keeps the old coefficient, which would
            // otherwise run until the next control tick (up to 15 samples, block dependent).
            for (auto& f : scFilter)
                f.reset();

            scCutoff.snap();
            appliedCutoff = -1.0f;
            updateDetectorFilter();
        }

        const float tauRelease = params.releaseMs * 0.001f;
        windowSamples = juce::jmax (1, (int) std::lround (juce::jlimit (kMinWindowSeconds, kMaxWindowSeconds,
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
        memoryChargeK = type == Type::opto ? optoMemoryChargeK : fetMemoryChargeK;
    }

    static float slowWeight (Type type, float m) noexcept
    {
        return type == Type::opto ? kOptoSlowWeightMax * m : kFetSlowWeightMax * m * m;
    }

    /** Steady-state auto makeup M = -gc(-10 dBFS - T), within the total makeup limit. */
    float autoMakeupFor() const noexcept
    {
        const float o = kAutoMakeupReferenceDb - threshold.getCurrent();
        return juce::jmin (-gainComputer (o, slope.getCurrent(), knee.getCurrent()), kMaxTotalMakeupDb - params.makeupDb);
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

    void snapDynamics() noexcept
    {
        threshold.snap();
        slope.snap();
        knee.snap();
        mix.snap();
        scCutoff.snap();
        appliedCutoff = -1.0f;
        updateDetectorFilter();
        autoMakeupDb.setTarget (autoMakeup ? autoMakeupFor() : 0.0f);
        autoMakeupDb.snap();
        snapSmoothers = false;
    }

    template <Type type>
    void runDynamics (juce::dsp::AudioBlock<float>& block, float* gainTrace, juce::SmoothedValue<float>& makeupGain) noexcept
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
                autoMakeupDb.setTarget (autoMakeup ? autoMakeupFor() : 0.0f);
            }

            controlPhase = (controlPhase + 1) % kControlInterval;

            const float thr = threshold.next();
            const float s = slope.next();
            const float wk = knee.next();
            const float wet = mix.next();
            const float autoDb = autoMakeupDb.next();
            const float makeupLinear = makeupGain.getNextValue();

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

            // While the window refills after a seed from Classic, its maximum can only be lower
            // than the full window's: an attack it calls for is real, a release is not yet known.
            float release1 = releaseK1, release2 = releaseK2;

            if (refill > 0)
            {
                --refill;
                release1 = release2 = 0.0f;
            }

            if constexpr (type == Type::squash)
            {
                // Log-domain feedback: the gain computer sees the output level L + g. Both
                // steps are divided by the loop gain, so the closed loop moves at the knobs'
                // time constants at any ratio and never overshoots; left open, the loop sped
                // the release up R times and distorted low notes more at fast settings. What
                // stays feedback: after a drop the output falls under the threshold, where the
                // loop is open and the gain heads for 0 dB, so a partial drop recovers sooner
                // (down to a third of the knob at R 8, a quarter at R 20; Compressor.h has the table).
                float loopGain;
                const float gc = feedbackGain (level + g - thr, s, wk, loopGain);
                g += (gc < g ? attackK : release1) / loopGain * (gc - g);
                gain = g;
            }
            else if constexpr (type == Type::fet || type == Type::opto)
            {
                const float gc = gainComputer (level - thr, s, wk);
                g1 += (gc < g1 ? attackK : release1) * (gc - g1);
                g2 += (gc < g2 ? attackK : release2) * (gc - g2);
                gain = g1 + slowWeight (type, memory) * (g2 - g1);

                const float depth = juce::jmin (1.0f, -gain * kMemoryPerDb);
                memory += (depth > memory ? memoryChargeK : memoryDischargeK) * (depth - memory);
            }
            else
            {
                const float gc = gainComputer (level - thr, s, wk);
                g += (gc < g ? attackK : release1) * (gc - g);
                gain = g;
            }

            const float linear = std::exp2 ((gain + autoDb) * kDbToLog2) * makeupLinear;

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
        settle (memory);

        // The detector filter's state is private: probe it on a copy, reset it if broken.
        for (auto& f : scFilter)
        {
            auto probe = f;

            if (! std::isfinite (probe.lowpass (0.0f)))
                f.reset();
        }

        gainReductionDb = juce::jmax (gainReductionDb, maxReduction);
    }

    //==============================================================================
    CompParams params;
    float sampleRate = 48000.0f;
    int numChannels = 2;

    int activeType = (int) Type::clean;
    bool snapEngine = true;

    // Crossfade to or from Classic: the incoming side renders into `incoming`.
    juce::AudioBuffer<float> incoming;
    std::vector<float> incomingTrace, outgoingTrace;
    bool fading = false;
    int fadeFrom = 0, fadeTo = 0, warmup = 0;
    float fadeWeight = 0.0f, fadeStep = 1.0f;
    float classicGainReductionDb = 0.0f;

    juce::SmoothedValue<float> makeup { 1.0f };
    bool makeupPrimed = false;

    juce::dsp::Compressor<float> compressor;
    juce::dsp::BallisticsFilter<float> shadow;

    WindowMax window;
    int windowSamples = 1, refill = 0;
    fx::TptOnePole scFilter[2];
    LinearRamp threshold, slope;
    OnePoleSmoother knee, mix, autoMakeupDb, scCutoff;
    float appliedCutoff = -1.0f;
    bool scFilterOn = false, autoMakeup = false, snapSmoothers = true;
    int controlPhase = 0;

    float attackK = 1.0f, releaseK1 = 1.0f, releaseK2 = 1.0f;
    float memoryChargeK = 1.0f, memoryDischargeK = 1.0f, fetMemoryChargeK = 1.0f, optoMemoryChargeK = 1.0f;
    float g = 0.0f, g1 = 0.0f, g2 = 0.0f, memory = 0.0f;

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
