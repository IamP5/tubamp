#include "Gate.h"

#include "FxPrimitives.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <dsp/NoiseGate.h>

namespace tubamp
{
namespace
{
// --- noise gate, mirroring NeuralAmpModeler::ProcessBlock:346-352 ----------
constexpr double kGateTime = 0.01;
constexpr double kGateRatio = 0.1;
constexpr double kGateOpenTime = 0.005;
constexpr double kGateHoldTime = 0.01;
constexpr double kGateCloseTime = 0.05;

// --- Tight and Smooth (docs/FX-ENGINES.md §4.1.5) ----------------------------
constexpr double kAttackSeconds = 0.0005;              // for 60 dB [study: gate/report.md R1]
constexpr float kHysteresisDb = 6.0f;                  // [study: R1, inside the references' 4.8-9.5 dB]
constexpr double kWindowSeconds = 0.012;               // half period of 42 Hz [study: R1 Nwin]
constexpr float kExpanderSlope = 3.0f;                 // Smooth below the close level [study: R1]
constexpr double kButterworthK = 1.4142135623730951;   // sqrt 2 [study: gate/evidence/bb/proto.py:16-22]
constexpr float kScHpfOffHz = 20.5f;                   // at or below: no sidechain filter [§4.1.2]
constexpr float kMuteDb = -89.95f;                     // range at or below: gain exactly 0 [§4.1.2]
constexpr float kPeakFloor = 1.0e-9f;                  // Smooth's log guard [§4.1.5]
constexpr float kSidechainLimit = 1.0e6f;              // [choice: fx::sanitise's bound, but clamped so a huge input still opens the gate]
constexpr double kFlushBelow = 1.0e-15;                // filter state, once per block [§2.6]
constexpr float kDbToGain = 0.11512925464970229f;      // ln(10) / 20

// --- mode switches and meters -------------------------------------------------
constexpr float kSwitchSeconds = 0.02f;                // linear curve crossfade [§4.1.8]
constexpr float kMeterFloorDb = -120.0f;               // [§2.8]
constexpr float kGrMeterMaxDb = 90.0f;                 // Tight/Smooth: mute reads 90 [§4.1.5]
constexpr float kClassicGrMeterMaxDb = 120.0f;         // [§4.1.4]
constexpr float kClassicOpenGain = 0.891f;             // -1 dB [§4.1.4]
constexpr float kClassicClosedGain = 0.01f;            // -40 dB [§4.1.4]

//==============================================================================
// Classic: NoiseGate::Impl and NoiseGate's methods from src/dsp/FxBlocks.cpp, moved verbatim
// (docs/FX-ENGINES.md §2.3). Do not rewrite an expression here, even equivalently.
struct ClassicGate
{
    ::dsp::noise_gate::Trigger trigger;
    ::dsp::noise_gate::Gain gain;

    // The AudioDSPTools objects work in DSP_SAMPLE, which is not necessarily float,
    // so audio crosses in and out of them through these fixed-size scratch buffers.
    std::vector<DSP_SAMPLE> monoIn;  // what the trigger listens to
    std::vector<DSP_SAMPLE> ones;    // probe fed to the gain to read back its curve

    // The gain's own output for a unit input == the linear gain-reduction curve.
    // Owned by `gain`, valid until its next Process() call.
    const DSP_SAMPLE* curve = nullptr;
    int curveFrames = 0;

    int maxBlockSize = 0;
    double sampleRate = 48000.0;

    ClassicGate()
    {
        trigger.AddListener (&gain);
    }

    /** Runs both objects at full size so their vectors reach their final capacity.
        Every later block is smaller or equal, and std::vector never releases capacity
        on shrink, so Process() cannot allocate on the audio thread afterwards. */
    void primeBuffers()
    {
        if (maxBlockSize <= 0)
            return;

        std::fill (monoIn.begin(), monoIn.end(), (DSP_SAMPLE) 0);
        run (maxBlockSize, -80.0f);
        curve = nullptr;
        curveFrames = 0;
    }

    void run (int numSamples, float thresholdDb)
    {
        trigger.SetSampleRate (sampleRate);
        trigger.SetParams (::dsp::noise_gate::TriggerParams (
            kGateTime, (double) thresholdDb, kGateRatio, kGateOpenTime, kGateHoldTime, kGateCloseTime));

        DSP_SAMPLE* triggerIn[1] = { monoIn.data() };
        trigger.Process (triggerIn, 1, (size_t) numSamples);

        // Trigger::Process pushed the new reduction curve into `gain` via the listener
        // link. Running the gain over a buffer of ones turns that dB curve into the
        // linear per-sample multiplier, which we can then apply to any channel count.
        DSP_SAMPLE* probe[1] = { ones.data() };
        curve = gain.Process (probe, 1, (size_t) numSamples)[0];
        curveFrames = numSamples;
    }

    void prepare (double newSampleRate, int newMaxBlockSize)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        maxBlockSize = juce::jmax (1, newMaxBlockSize);

        monoIn.assign ((size_t) maxBlockSize, (DSP_SAMPLE) 0);
        ones.assign ((size_t) maxBlockSize, (DSP_SAMPLE) 1);

        primeBuffers();
    }

    void reset()
    {
        // The trigger keeps no externally resettable state; invalidating the curve is
        // enough to stop apply() from reusing a stale block.
        curve = nullptr;
        curveFrames = 0;
    }

    void measure (const juce::dsp::AudioBlock<float>& block, int numSamples, float thresholdDb)
    {
        curve = nullptr;
        curveFrames = 0;

        const auto numChannels = (int) block.getNumChannels();

        if (numSamples <= 0 || numChannels <= 0 || numSamples > maxBlockSize)
            return; // oversized block: pass through ungated rather than reallocate here

        const float scale = 1.0f / (float) numChannels;

        for (int i = 0; i < numSamples; ++i)
        {
            float sum = 0.0f;

            for (int ch = 0; ch < numChannels; ++ch)
                sum += block.getChannelPointer ((size_t) ch)[i];

            monoIn[(size_t) i] = (DSP_SAMPLE) (sum * scale);
        }

        run (numSamples, thresholdDb);
    }

    void apply (juce::dsp::AudioBlock<float> block, int numSamples) const
    {
        if (curve == nullptr || curveFrames != numSamples)
            return;

        for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
        {
            auto* data = block.getChannelPointer (ch);

            for (int i = 0; i < numSamples; ++i)
                data[i] *= (float) curve[i];
        }
    }

    void apply (float* mono, int numSamples) const
    {
        if (mono == nullptr || curve == nullptr || curveFrames != numSamples)
            return;

        for (int i = 0; i < numSamples; ++i)
            mono[i] *= (float) curve[i];
    }

    JUCE_DECLARE_NON_COPYABLE (ClassicGate)
};

//==============================================================================
/** Tight and Smooth share one detector and one state machine (§4.1.5); only the closed
    target differs, so both gain-reduction ramps run side by side every sample and a switch
    between the two is a crossfade of two live curves. */
class PeakGate
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        windowLength = juce::jmax (1, (int) std::lround (kWindowSeconds * sampleRate));

        segment.assign ((size_t) windowLength, 0.0f);
        suffix.assign ((size_t) windowLength + 1, 0.0f);

        attackStep = (float) (60.0 / (kAttackSeconds * sampleRate));
        lastThreshold = lastRelease = lastHold = lastRange = lastHpf = -1.0e9f;
        reset();
    }

    /** Closed at the range, window empty, filter at rest. */
    void reset() noexcept
    {
        ic1 = ic2 = 0.0;
        std::fill (segment.begin(), segment.end(), 0.0f);
        std::fill (suffix.begin(), suffix.end(), 0.0f);
        segmentPos = 0;
        prefix = 0.0f;
        peak = 0.0f;
        open = false;
        holdLeft = 0;
        grTight = grSmooth = range;
        invalidateCaches();
    }

    void setParameters (float thresholdDb, float releaseMs, float holdMs, float rangeDb, float scHpfHz) noexcept
    {
        if (! juce::exactlyEqual (thresholdDb, lastThreshold))
        {
            lastThreshold = thresholdDb;
            openLevel = std::pow (10.0f, thresholdDb / 20.0f);
            closeLevel = openLevel * std::pow (10.0f, -kHysteresisDb / 20.0f);
            smoothOffsetDb = (kExpanderSlope - 1.0f) * 20.0f * std::log10 (closeLevel);
            invalidateCaches();
        }

        if (! juce::exactlyEqual (releaseMs, lastRelease))
        {
            lastRelease = releaseMs;
            releaseStep = (float) (60.0 / ((double) releaseMs * 0.001 * sampleRate));
        }

        if (! juce::exactlyEqual (holdMs, lastHold))
        {
            lastHold = holdMs;
            holdSamples = (int) std::lround ((double) holdMs * 0.001 * sampleRate);
        }

        if (! juce::exactlyEqual (rangeDb, lastRange))
        {
            lastRange = rangeDb;
            range = rangeDb;
            mute = rangeDb <= kMuteDb;
            invalidateCaches();
        }

        if (! juce::exactlyEqual (scHpfHz, lastHpf))
        {
            lastHpf = scHpfHz;
            const bool wasOn = hpfOn;
            hpfOn = scHpfHz > kScHpfOffHz;

            if (hpfOn)
            {
                const double fc = juce::jmin ((double) scHpfHz, 0.49 * sampleRate);
                const double g = std::tan (juce::MathConstants<double>::pi * fc / sampleRate);
                a1 = 1.0 / (1.0 + g * (g + kButterworthK));
                a2 = g * a1;
                a3 = g * a2;
            }

            if (hpfOn != wasOn)
                ic1 = ic2 = 0.0;
        }
    }

    /** One sample of the mono sidechain: filter, sliding-window peak, state, both ramps. */
    void advance (float s) noexcept
    {
        double x = (double) s;

        if (hpfOn)
        {
            const double v3 = x - ic2;
            const double v1 = a1 * ic1 + a2 * v3;
            const double v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0 * v1 - ic1;
            ic2 = 2.0 * v2 - ic2;
            x = x - kButterworthK * v1 - v2;
        }

        const float a = (float) std::abs (x);

        // Exact maximum over the last windowLength samples, so the detector reads the true
        // level as soon as the last crest leaves the window (van Herk / Gil-Werman: the
        // window is the tail of the previous segment, whose suffix maxima were computed
        // when it filled, plus the head of the current one; no data-dependent branches).
        segment[(size_t) segmentPos] = a;
        prefix = juce::jmax (prefix, a);
        peak = juce::jmax (suffix[(size_t) segmentPos + 1], prefix);

        if (++segmentPos == windowLength)
        {
            float m = 0.0f;

            for (int j = windowLength - 1; j >= 0; --j)
            {
                m = juce::jmax (m, segment[(size_t) j]);
                suffix[(size_t) j] = m;
            }

            segmentPos = 0;
            prefix = 0.0f;
        }

        if (peak >= openLevel)
        {
            open = true;
            holdLeft = holdSamples;
        }
        else if (open)
        {
            if (peak >= closeLevel)
                holdLeft = holdSamples;
            else if (holdLeft > 0)
                --holdLeft;
            else
                open = false;
        }

        float smoothTarget = 0.0f;

        if (! open)
        {
            if (! juce::exactlyEqual (peak, smoothPeak))
            {
                smoothPeak = peak;
                const float law = (kExpanderSlope - 1.0f) * 20.0f * std::log10 (juce::jmax (peak, kPeakFloor)) - smoothOffsetDb;
                smoothLaw = juce::jlimit (range, 0.0f, law);
            }

            smoothTarget = smoothLaw;
        }

        grTight = slew (grTight, open ? 0.0f : range);
        grSmooth = slew (grSmooth, smoothTarget);
    }

    float tightGain() noexcept { return gainFor (grTight, tightCache); }
    float smoothGain() noexcept { return gainFor (grSmooth, smoothCache); }
    float gainOf (int mode) noexcept { return mode == (int) GateFx::Mode::smooth ? smoothGain() : tightGain(); }
    float grOf (int mode) const noexcept { return mode == (int) GateFx::Mode::smooth ? grSmooth : grTight; }

    /** Once per block: tiny filter states to zero (denormals without FTZ), and a state
        that went bad back to rest. */
    void endBlock() noexcept
    {
        if (! (std::abs (ic1) < 1.0e12) || ! (std::abs (ic2) < 1.0e12))
            ic1 = ic2 = 0.0;

        if (std::abs (ic1) < kFlushBelow)
            ic1 = 0.0;

        if (std::abs (ic2) < kFlushBelow)
            ic2 = 0.0;
    }

    float currentPeak() const noexcept { return peak; }
    bool isMuted (float gr) const noexcept { return mute && gr <= kMuteDb; }
    float floorGain() const noexcept { return mute ? 0.0f : std::exp (range * kDbToGain); }

    /** 2 open, 1 in the hysteresis band or holding, 0 closed (§4.1.5). */
    int state() const noexcept { return open ? (peak >= openLevel ? 2 : 1) : 0; }

private:
    struct GainCache
    {
        float gr = 1.0f, gain = 1.0f;
    };

    float slew (float gr, float target) const noexcept
    {
        const float d = target - gr;

        if (d > attackStep)
            return gr + attackStep;

        if (d < -releaseStep)
            return gr - releaseStep;

        return target;
    }

    float gainFor (float gr, GainCache& cache) const noexcept
    {
        if (! juce::exactlyEqual (gr, cache.gr))
        {
            cache.gr = gr;
            cache.gain = isMuted (gr) ? 0.0f : std::exp (gr * kDbToGain);
        }

        return cache.gain;
    }

    void invalidateCaches() noexcept
    {
        tightCache = smoothCache = GainCache { 1.0f, 1.0f };
        smoothPeak = -1.0f;
    }

    double sampleRate = 48000.0;

    // Sidechain high-pass (TPT SVF, Butterworth), in double.
    double a1 = 1.0, a2 = 0.0, a3 = 0.0, ic1 = 0.0, ic2 = 0.0;
    bool hpfOn = false;

    std::vector<float> segment, suffix;   // suffix[windowLength] stays 0
    int windowLength = 1, segmentPos = 0;
    float prefix = 0.0f;

    float openLevel = 1.0e-4f, closeLevel = 5.0e-5f, smoothOffsetDb = 0.0f;
    float attackStep = 2.5f, releaseStep = 0.04f, range = -90.0f;
    int holdSamples = 0;
    bool mute = true;

    float peak = 0.0f, grTight = -90.0f, grSmooth = -90.0f;
    float smoothPeak = -1.0f, smoothLaw = 0.0f;
    bool open = false;
    int holdLeft = 0;

    GainCache tightCache, smoothCache;
    float lastThreshold = -1.0e9f, lastRelease = -1.0e9f, lastHold = -1.0e9f, lastRange = -1.0e9f, lastHpf = -1.0e9f;
};

float clampParam (float value, float lo, float hi, float fallback) noexcept
{
    return std::isnan (value) ? fallback : juce::jlimit (lo, hi, value);
}

float clampSidechain (float s) noexcept
{
    if (std::abs (s) < kSidechainLimit)
        return s;

    return s > 0.0f ? kSidechainLimit : (s < 0.0f ? -kSidechainLimit : 0.0f);
}
} // namespace

//==============================================================================
struct GateFx::Impl
{
    enum class Curve { classic, peak, mixed };

    ClassicGate classic;
    PeakGate peak;
    GateParams params;

    // Tight/Smooth gain per measured sample, long enough for the deferred delay.
    std::vector<float> ring;
    int ringMask = 1, ringWrite = 0;
    // Weight of the Classic curve per sample, while Classic fades in or out.
    std::vector<float> classicWeight;
    std::vector<float> mono;

    double sampleRate = 48000.0;
    int maxBlockSize = 1;
    int curveDelay = 0;
    int curveFrames = 0;
    Curve curve = Curve::peak;

    int active = 1, target = 1;
    bool switching = false, snapMode = true, restartPeak = true;
    float fade = 0.0f, fadeStep = 1.0f;

    int meterState = 0;
    float meterGrDb = 0.0f, meterLevelDb = kMeterFloorDb;

    void applyCurve (float* data, int numSamples, int delay) const noexcept
    {
        const int start = ringWrite - numSamples - delay;

        if (curve == Curve::mixed)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float w = classicWeight[(size_t) i];
                const float g = w * (float) classic.curve[i] + (1.0f - w) * ring[(size_t) ((start + i) & ringMask)];
                data[i] *= g;
            }

            return;
        }

        for (int i = 0; i < numSamples; ++i)
            data[i] *= ring[(size_t) ((start + i) & ringMask)];
    }

    void classicMeters (int numSamples)
    {
        if (classic.curve == nullptr || classic.curveFrames != numSamples)
            return;

        double minGain = 1.0, maxIn = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            minGain = juce::jmin (minGain, (double) classic.curve[i]);
            maxIn = juce::jmax (maxIn, std::abs ((double) classic.monoIn[(size_t) i]));
        }

        const auto last = (float) classic.curve[numSamples - 1];
        meterState = last >= kClassicOpenGain ? 2 : (last <= kClassicClosedGain ? 0 : 1);
        meterGrDb = minGain > 0.0 ? juce::jlimit (0.0f, kClassicGrMeterMaxDb, (float) (-20.0 * std::log10 (minGain)))
                                  : kClassicGrMeterMaxDb;
        meterLevelDb = maxIn > 0.0 ? juce::jmax (kMeterFloorDb, (float) (20.0 * std::log10 (maxIn))) : kMeterFloorDb;
    }
};

GateFx::GateFx() : impl (std::make_unique<Impl>()) {}
GateFx::~GateFx() = default;

void GateFx::prepare (const juce::dsp::ProcessSpec& spec)
{
    auto& s = *impl;
    s.sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;
    s.maxBlockSize = juce::jmax (1, (int) spec.maximumBlockSize);

    s.classic.prepare (s.sampleRate, s.maxBlockSize);
    s.peak.prepare (s.sampleRate);

    const int ringSize = fx::nextPow2 (s.maxBlockSize + kMaxCurveDelaySamples);
    s.ring.assign ((size_t) ringSize, 0.0f);
    s.ringMask = ringSize - 1;
    s.classicWeight.assign ((size_t) s.maxBlockSize, 0.0f);
    s.mono.assign ((size_t) s.maxBlockSize, 0.0f);

    s.fadeStep = 1.0f / (kSwitchSeconds * (float) s.sampleRate);

    setParameters (s.params);
    reset();
}

void GateFx::reset()
{
    auto& s = *impl;
    s.classic.reset();
    s.peak.reset();
    s.curveFrames = 0;
    s.restartPeak = true;
    s.snapMode = true;
    s.switching = false;
    s.fade = 0.0f;
    s.meterState = 0;
    s.meterGrDb = 0.0f;
    s.meterLevelDb = kMeterFloorDb;
}

void GateFx::setParameters (const GateParams& params)
{
    auto& s = *impl;
    const GateParams defaults;
    s.params.mode = juce::jlimit (0, numModes - 1, params.mode);
    s.params.thresholdDb = clampParam (params.thresholdDb, -100.0f, 0.0f, defaults.thresholdDb);
    s.params.releaseMs = clampParam (params.releaseMs, 5.0f, 500.0f, defaults.releaseMs);
    s.params.holdMs = clampParam (params.holdMs, 0.0f, 250.0f, defaults.holdMs);
    s.params.rangeDb = clampParam (params.rangeDb, -90.0f, 0.0f, defaults.rangeDb);
    s.params.scHpfHz = clampParam (params.scHpfHz, 20.0f, 300.0f, defaults.scHpfHz);

    s.peak.setParameters (s.params.thresholdDb, s.params.releaseMs, s.params.holdMs, s.params.rangeDb,
                          s.params.scHpfHz);
}

void GateFx::setCurveDelay (int samples) noexcept
{
    impl->curveDelay = juce::jlimit (0, kMaxCurveDelaySamples, samples);
}

void GateFx::measure (const juce::dsp::AudioBlock<float>& block, int numSamples)
{
    auto& s = *impl;
    s.curveFrames = 0;

    if (s.snapMode)
    {
        // Nothing has been heard since prepare() or reset(): start on the requested mode.
        s.snapMode = false;
        s.active = s.params.mode;
    }
    else if (! s.switching && s.params.mode != s.active)
    {
        // A request that arrives mid-switch waits for the fade to finish.
        s.switching = true;
        s.target = s.params.mode;
        s.fade = 0.0f;
    }

    const int from = s.active;
    const int to = s.switching ? s.target : s.active;

    // Every detector runs every block (§4.1.8), so whichever mode is picked next is already
    // listening and a switch is only a crossfade of two live curves.
    s.classic.measure (block, numSamples, s.params.thresholdDb);

    const auto numChannels = (int) block.getNumChannels();

    if (numSamples <= 0 || numChannels <= 0 || numSamples > s.maxBlockSize)
        return; // oversized block: pass through ungated rather than reallocate here

    if (s.restartPeak)
    {
        s.restartPeak = false;
        s.peak.reset();
        std::fill (s.ring.begin(), s.ring.end(), s.peak.floorGain());
    }

    const float scale = 1.0f / (float) numChannels;

    for (int i = 0; i < numSamples; ++i)
    {
        float sum = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
            sum += block.getChannelPointer ((size_t) ch)[i];

        s.mono[(size_t) i] = sum * scale;
    }

    // The ring always carries a Tight/Smooth curve, even under steady Classic, so a switch
    // to either finds a warm, correctly delayed history.
    const bool newToNew = s.switching && from != (int) Mode::classic && to != (int) Mode::classic;
    const bool classicFade = s.switching && ! newToNew;
    const int ringMode = from != (int) Mode::classic ? from : (to != (int) Mode::classic ? to : (int) Mode::tight);
    const int meterMode = to;

    s.curve = classicFade ? Impl::Curve::mixed : (from == (int) Mode::classic ? Impl::Curve::classic : Impl::Curve::peak);

    float maxPeak = 0.0f, minGr = 0.0f;
    bool muted = false;

    for (int i = 0; i < numSamples; ++i)
    {
        s.peak.advance (clampSidechain (s.mono[(size_t) i]));

        float w = 0.0f;

        if (s.switching)
        {
            s.fade = juce::jmin (1.0f, s.fade + s.fadeStep);
            w = s.fade;
        }

        float g;

        if (newToNew)
            g = (1.0f - w) * s.peak.gainOf (from) + w * s.peak.gainOf (to);
        else
            g = s.peak.gainOf (ringMode);

        s.ring[(size_t) ((s.ringWrite + i) & s.ringMask)] = g;

        if (classicFade)
            s.classicWeight[(size_t) i] = to == (int) Mode::classic ? w : 1.0f - w;

        maxPeak = juce::jmax (maxPeak, s.peak.currentPeak());

        if (meterMode != (int) Mode::classic)
        {
            const float gr = s.peak.grOf (meterMode);
            minGr = juce::jmin (minGr, gr);
            muted = muted || s.peak.isMuted (gr);
        }
    }

    s.ringWrite = (s.ringWrite + numSamples) & s.ringMask;
    s.peak.endBlock();

    if (s.switching && s.fade >= 1.0f)
    {
        s.active = s.target;
        s.switching = false;
    }

    if (meterMode == (int) Mode::classic)
    {
        s.classicMeters (numSamples);
    }
    else
    {
        s.meterState = s.peak.state();
        s.meterGrDb = muted ? kGrMeterMaxDb : juce::jlimit (0.0f, kGrMeterMaxDb, -minGr);
        s.meterLevelDb = maxPeak > 0.0f ? juce::jmax (kMeterFloorDb, 20.0f * std::log10 (maxPeak)) : kMeterFloorDb;
    }

    s.curveFrames = numSamples;
}

void GateFx::apply (juce::dsp::AudioBlock<float> block, int numSamples)
{
    const auto& s = *impl;

    if (numSamples <= 0 || s.curveFrames != numSamples)
        return;

    if (s.curve == Impl::Curve::classic)
    {
        s.classic.apply (block, numSamples);
        return;
    }

    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
        s.applyCurve (block.getChannelPointer (ch), numSamples, 0);
}

void GateFx::apply (float* mono, int numSamples)
{
    const auto& s = *impl;

    if (mono == nullptr || numSamples <= 0 || s.curveFrames != numSamples)
        return;

    if (s.curve == Impl::Curve::classic)
    {
        s.classic.apply (mono, numSamples);
        return;
    }

    s.applyCurve (mono, numSamples, s.curveDelay);
}

int GateFx::getState() const noexcept { return impl->meterState; }
float GateFx::getGainReductionDb() const noexcept { return impl->meterGrDb; }
float GateFx::getDetectorLevelDb() const noexcept { return impl->meterLevelDb; }
} // namespace tubamp
