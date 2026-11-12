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
constexpr float kHysteresisDb = 6.0f;                  // [study: R1, inside the references' 4.8-9.5 dB]
constexpr double kWindowSeconds = 0.012;               // half period of 42 Hz [study: R1 Nwin]
constexpr float kExpanderSlope = 3.0f;                 // Smooth below the close level [study: R1]
// 4th-order Butterworth high-pass as two sections: at the 75 Hz default a low E reads 1.7 dB
// low (the 2nd-order 90 Hz filter read 3.9) and 50 / 60 Hz hum is cut 14 / 8 dB (was 11 / 8).
// [choice, check_gate X20, G15; section damping 2cos(pi/8), 2cos(3pi/8)]
constexpr double kHighPassDamping[2] { 1.8477590650225735, 0.7653668647301796 };
constexpr float kScHpfOffHz = 20.5f;                   // at or below: no sidechain filter [§4.1.2]
constexpr float kMuteDb = -89.95f;                     // range at or below: gain exactly 0 [§4.1.2]
constexpr float kPeakFloor = 1.0e-9f;                  // Smooth's log guard [§4.1.5]
constexpr float kSidechainLimit = 1.0e6f;              // [choice: fx::sanitise's bound, but clamped so a huge input still opens the gate]
constexpr double kFlushBelow = 1.0e-15;                // filter state, once per block [§2.6]
constexpr float kDbToGain = 0.11512925464970229f;      // ln(10) / 20
// The state machine reads the lower of the filtered and the unfiltered windowed peak: the 4th-order
// filter rings for tens of ms after a note stops (up to 45 ms more after a loud low note over a deep
// floor), the unfiltered signal stops with the note. On a guitar note the unfiltered peak is the
// higher, so the hysteresis stays whole (check_gate X10, B1); hum keeps the filtered one lower.

// Opening follows the onset, timed over the last kOpenNearDb of the climb: a steady climb gets half
// the time it took to cross the hysteresis band, as if timed from the close level, but a floor or a
// scrape sitting lower in the band does not slow a pick. From version 2's 0.2 ms linear rise for a
// pick to a 5 ms smoothstep for a swell; linear at the shortest (a 0.2 ms smoothstep splatters more),
// smoothstep from twice that. [choice, check_gate D1-D4, X16]
constexpr float kOpenNearDb = 1.0f;
constexpr double kOpenRatio = 0.5 * kHysteresisDb / kOpenNearDb;
constexpr double kOpenMinSeconds = 0.0002;
constexpr double kOpenMaxSeconds = 0.005;

// Smooth's floor estimate while closed: the dB peak level, averaged over half the time since the
// gate closed, up to kFloorTauMaxSeconds; the expander gain follows it at most kFloorSlewDbPerSecond,
// so a floor that jumps or wobbles moves no faster. [choice, check_gate X14, E1]
constexpr double kFloorTauGrowth = 0.5;
constexpr double kFloorTauMaxSeconds = 0.2;
constexpr double kFloorSlewDbPerSecond = 100.0;        // 1 dB per 10 ms
constexpr double kRangeSeconds = 0.01;                 // range moves, one-pole in dB, per sample [check_gate G1]

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

            // A NaN would stay in the trigger's level state for good and an Inf would hold it
            // open; read both as silence. Finite input is untouched.
            if (! std::isfinite (monoIn[(size_t) i]))
                monoIn[(size_t) i] = (DSP_SAMPLE) 0;
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
/** One TPT state-variable high-pass section, in double [study: gate/evidence/bb/proto.py:16-22]. */
struct SvfHighPass
{
    double k = 1.0, a1 = 1.0, a2 = 0.0, a3 = 0.0, ic1 = 0.0, ic2 = 0.0;

    void setup (double g, double damping) noexcept
    {
        k = damping;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    double process (double x) noexcept
    {
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        return x - k * v1 - v2;
    }

    void clear() noexcept { ic1 = ic2 = 0.0; }

    /** Tiny states to zero (denormals without FTZ), a state that went bad back to rest. */
    void endBlock() noexcept
    {
        if (! (std::abs (ic1) < 1.0e12) || ! (std::abs (ic2) < 1.0e12))
            clear();

        if (std::abs (ic1) < kFlushBelow)
            ic1 = 0.0;

        if (std::abs (ic2) < kFlushBelow)
            ic2 = 0.0;
    }
};

/** Exact maximum over the last `length` samples, so a detector reads the true level as soon
    as the last crest leaves the window (van Herk / Gil-Werman: the window is the tail of the
    previous segment, whose suffix maxima were computed when it filled, plus the head of the
    current one; no data-dependent branches). */
struct MaxWindow
{
    std::vector<float> segment, suffix;   // suffix[length] stays 0
    int length = 1, pos = 0;
    float prefix = 0.0f;

    void prepare (int newLength)
    {
        length = newLength;
        segment.assign ((size_t) length, 0.0f);
        suffix.assign ((size_t) length + 1, 0.0f);
        clear();
    }

    void clear() noexcept
    {
        std::fill (segment.begin(), segment.end(), 0.0f);
        std::fill (suffix.begin(), suffix.end(), 0.0f);
        pos = 0;
        prefix = 0.0f;
    }

    float push (float a) noexcept
    {
        segment[(size_t) pos] = a;
        prefix = juce::jmax (prefix, a);
        const float m = juce::jmax (suffix[(size_t) pos + 1], prefix);

        if (++pos == length)
        {
            float s = 0.0f;

            for (int j = length - 1; j >= 0; --j)
            {
                s = juce::jmax (s, segment[(size_t) j]);
                suffix[(size_t) j] = s;
            }

            pos = 0;
            prefix = 0.0f;
        }

        return m;
    }
};

/** One gain ramp (Tight's or Smooth's), in dB. Opening rises in linear gain from where it was;
    closing falls at the release rate to the closed target, which it then follows. */
struct Ramp
{
    double gr = -90.0;   // double: a 500 ms release at 192 kHz is 144 000 steps, too many to sum in float
    float from = 0.0f, phase = 1.0f;
    bool closing = false;
};

/** Tight and Smooth share one detector and one state machine (§4.1.5); only the closed
    target differs, so both gain ramps run side by side every sample and a switch between the
    two is a crossfade of two live curves. */
class PeakGate
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        windowSamples = juce::jmax (1, (int) std::lround (kWindowSeconds * sampleRate));
        main.prepare (windowSamples);
        raw.prepare (windowSamples);
        openMin = kOpenMinSeconds * sampleRate;
        openMax = kOpenMaxSeconds * sampleRate;
        floorTauMax = kFloorTauMaxSeconds * sampleRate;
        floorGrowSamples = (int) std::ceil (floorTauMax / kFloorTauGrowth);
        floorSlew = (float) (kFloorSlewDbPerSecond / sampleRate);
        rangeCoeff = (float) (1.0 - std::exp (-1.0 / (kRangeSeconds * sampleRate)));
        lastThreshold = lastRelease = lastHold = lastRange = lastHpf = -1.0e9f;
        reset();
    }

    /** Closed at the range, windows empty, filters at rest. */
    void reset() noexcept
    {
        for (auto& section : hpf)
            section.clear();

        main.clear();
        raw.clear();
        open = false;
        below = 0;
        sinceNear = kLongAgo;
        floorDb = 0.0;
        floorSince = 0;
        settle = windowSamples;
        range = rangeTarget;
        mute = range <= kMuteDb;
        tight = smooth = Ramp { range, 0.0f, 1.0f, false };
        invalidateCaches();
    }

    void setParameters (float thresholdDb, float releaseMs, float holdMs, float rangeDb, float scHpfHz) noexcept
    {
        if (! juce::exactlyEqual (thresholdDb, lastThreshold))
        {
            lastThreshold = thresholdDb;
            openLevel = std::pow (10.0f, thresholdDb / 20.0f);
            closeLevel = openLevel * std::pow (10.0f, -kHysteresisDb / 20.0f);
            nearLevel = openLevel * std::pow (10.0f, -kOpenNearDb / 20.0f);
            smoothOffsetDb = (kExpanderSlope - 1.0f) * 20.0f * std::log10 (closeLevel);
        }

        if (! juce::exactlyEqual (releaseMs, lastRelease))
        {
            lastRelease = releaseMs;
            releaseStep = 60.0 / ((double) releaseMs * 0.001 * sampleRate);
        }

        if (! juce::exactlyEqual (holdMs, lastHold))
        {
            lastHold = holdMs;
            holdSamples = (double) std::lround ((double) holdMs * 0.001 * sampleRate);
        }

        if (! juce::exactlyEqual (rangeDb, lastRange))
        {
            lastRange = rangeDb;
            rangeTarget = rangeDb;
        }

        if (! juce::exactlyEqual (scHpfHz, lastHpf))
        {
            lastHpf = scHpfHz;
            const bool wasOn = hpfOn;
            hpfOn = scHpfHz > kScHpfOffHz;

            if (hpfOn)
            {
                const double pi = juce::MathConstants<double>::pi;
                const double g = std::tan (pi * juce::jmin ((double) scHpfHz, 0.49 * sampleRate) / sampleRate);

                for (int i = 0; i < 2; ++i)
                    hpf[i].setup (g, kHighPassDamping[i]);
            }

            if (hpfOn != wasOn)
            {
                for (auto& section : hpf)
                    section.clear();
            }
        }
    }

    /** One sample of the mono sidechain: filters, windowed peaks, state, both ramps. */
    void advance (float s) noexcept
    {
        float peak, level;

        if (hpfOn)
        {
            peak = main.push ((float) std::abs (hpf[1].process (hpf[0].process ((double) s))));
            level = juce::jmin (peak, raw.push (std::abs (s)));
        }
        else
        {
            peak = level = main.push (std::abs (s));
            raw.push (std::abs (s));   // kept warm for when the filter comes back on
        }

        mainPeak = peak;
        sinceNear = level < nearLevel ? 0 : juce::jmin (sinceNear + 1, kLongAgo);
        followRange();

        // Until the window has filled once after a reset, Smooth's estimate and expander take the
        // level as it comes: there is no floor yet for the slew to protect.
        const bool settling = settle > 0;

        if (settling)
            --settle;

        if (level >= openLevel)
        {
            below = 0;

            if (! open)
            {
                open = true;
                const double steps = juce::jlimit (openMin, openMax, kOpenRatio * (double) sinceNear + 1.0);
                openStep = 1.0f / (float) steps;
                openCurve = (float) juce::jlimit (0.0, 1.0, steps / openMin - 1.0);
                startOpening (tight, tightCache);
                startOpening (smooth, smoothCache);
            }
        }
        else if (open)
        {
            below = level < closeLevel ? below + 1 : 0;

            if ((double) below > holdSamples)
            {
                open = false;
                tight.closing = smooth.closing = true;
                floorSince = 0;
            }
        }

        if (open)
        {
            advanceOpening (tight);
            advanceOpening (smooth);
            return;
        }

        const double levelDb = 20.0 * std::log10 ((double) juce::jmax (level, kPeakFloor));

        if (floorSince == 0 || settling)
            floorDb = levelDb;
        else
            floorDb += (levelDb - floorDb) * (floorSince < floorGrowSamples ? 1.0 / juce::jmax (1.0, kFloorTauGrowth * floorSince)
                                                                            : 1.0 / floorTauMax);

        floorSince = juce::jmin (floorSince + 1, floorGrowSamples);

        advanceClosed (tight, range, kNoSlewLimit);
        advanceClosed (smooth, juce::jlimit ((double) range, 0.0, (kExpanderSlope - 1.0f) * floorDb - smoothOffsetDb),
                       settling ? kNoSlewLimit : floorSlew);
    }

    float tightGain() noexcept { return gainFor ((float) tight.gr, tightCache); }
    float smoothGain() noexcept { return gainFor ((float) smooth.gr, smoothCache); }
    float gainOf (int mode) noexcept { return mode == (int) GateFx::Mode::smooth ? smoothGain() : tightGain(); }
    float grOf (int mode) const noexcept { return mode == (int) GateFx::Mode::smooth ? (float) smooth.gr : (float) tight.gr; }

    /** Once per block: filter states that went tiny or bad. */
    void endBlock() noexcept
    {
        for (auto& section : hpf)
            section.endBlock();
    }

    float currentPeak() const noexcept { return mainPeak; }
    bool isMuted (float gr) const noexcept { return mute && gr <= kMuteDb; }
    float floorGain() const noexcept { return rangeTarget <= kMuteDb ? 0.0f : std::exp (rangeTarget * kDbToGain); }

    /** 2 open, 1 open in the hysteresis band or the hold, 0 closed (§4.1.5). */
    int state() const noexcept { return open ? (mainPeak >= openLevel ? 2 : 1) : 0; }

private:
    struct GainCache
    {
        float gr = 1.0f, gain = 1.0f;
    };

    static constexpr int kLongAgo = 1 << 30;
    static constexpr float kNoSlewLimit = 1.0e9f;

    void followRange() noexcept
    {
        if (juce::exactlyEqual (range, rangeTarget))
            return;

        range += (rangeTarget - range) * rangeCoeff;

        if (std::abs (rangeTarget - range) < 1.0e-3f)
            range = rangeTarget;

        if (const bool nowMute = range <= kMuteDb; nowMute != mute)
        {
            mute = nowMute;
            invalidateCaches();
        }
    }

    void startOpening (Ramp& r, GainCache& cache) noexcept
    {
        r.from = gainFor ((float) r.gr, cache);
        r.phase = r.gr >= 0.0 ? 1.0f : 0.0f;
        r.closing = false;
    }

    void advanceOpening (Ramp& r) const noexcept
    {
        if (r.phase >= 1.0f)
            return;

        r.phase = juce::jmin (1.0f, r.phase + openStep);
        const float p = r.phase;
        const float shape = p + openCurve * (p * p * (3.0f - 2.0f * p) - p);
        const float g = r.from + (1.0f - r.from) * shape;
        r.gr = p >= 1.0f || g >= 1.0f ? 0.0f : std::log (juce::jmax (g, 1.0e-9f)) / kDbToGain;
    }

    /** Release to `target`, then follow it at most `slew` dB per sample. */
    void advanceClosed (Ramp& r, double target, float slew) const noexcept
    {
        if (r.closing)
        {
            if (r.gr > target)
                r.gr = juce::jmax (target, r.gr - releaseStep);

            r.closing = r.gr > target;
            return;
        }

        r.gr += juce::jlimit ((double) -slew, (double) slew, target - r.gr);
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
    }

    double sampleRate = 48000.0;

    SvfHighPass hpf[2];
    bool hpfOn = false;

    MaxWindow main, raw;   // filtered, unfiltered

    float openLevel = 1.0e-4f, closeLevel = 5.0e-5f, nearLevel = 8.9e-5f, smoothOffsetDb = 0.0f, mainPeak = 0.0f;
    float openStep = 1.0f, openCurve = 0.0f, floorSlew = 0.002f, rangeCoeff = 0.002f;
    float range = -90.0f, rangeTarget = -90.0f;
    double releaseStep = 0.04, holdSamples = 0.0, openMin = 9.6, openMax = 240.0, floorTauMax = 9600.0;
    int floorGrowSamples = 19200;
    bool mute = true;

    Ramp tight, smooth;
    double floorDb = 0.0;   // double: at 192 kHz a float estimate stalls 0.07 dB short of a steady floor
    int floorSince = 0, below = 0, sinceNear = 0, settle = 0, windowSamples = 576;
    bool open = false;

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

    // NaN and Inf carry no level: read as silence, so nothing non-finite reaches a state and
    // the gate is back on the real signal the next sample.
    if (! std::isfinite (s))
        return 0.0f;

    return s > 0.0f ? kSidechainLimit : -kSidechainLimit;
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

    if (numSamples <= 0 || numChannels <= 0)
        return;

    if (s.restartPeak)
    {
        s.restartPeak = false;
        s.peak.reset();
        std::fill (s.ring.begin(), s.ring.end(), s.peak.floorGain());
    }

    // A block longer than maxBlockSize gets no curve and passes ungated, but the detector still
    // hears it and the ring still records its curve, so the state, the meters and a deferred
    // apply further down the chain all stay continuous.
    const bool oversized = numSamples > s.maxBlockSize;

    // The ring always carries a Tight/Smooth curve, even under steady Classic, so a switch
    // to either finds a warm, correctly delayed history.
    const bool newToNew = s.switching && from != (int) Mode::classic && to != (int) Mode::classic;
    const bool classicFade = s.switching && ! newToNew && ! oversized;
    const int ringMode = from != (int) Mode::classic ? from : (to != (int) Mode::classic ? to : (int) Mode::tight);
    const int meterMode = to;

    s.curve = classicFade ? Impl::Curve::mixed : (from == (int) Mode::classic ? Impl::Curve::classic : Impl::Curve::peak);

    const float scale = 1.0f / (float) numChannels;
    float maxPeak = 0.0f, minGr = 0.0f, maxIn = 0.0f;
    bool muted = false;

    for (int i = 0; i < numSamples; ++i)
    {
        float sum = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
            sum += block.getChannelPointer ((size_t) ch)[i];

        const float monoSample = sum * scale;
        s.peak.advance (clampSidechain (monoSample));

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
        else if (oversized && std::isfinite (monoSample))
        {
            maxIn = juce::jmax (maxIn, std::abs (monoSample));
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
        if (oversized)
        {
            // The Classic trigger cannot take the block (its buffers are fixed): it passed at unity.
            s.meterState = 2;
            s.meterGrDb = 0.0f;
            s.meterLevelDb = maxIn > 0.0f ? juce::jmax (kMeterFloorDb, 20.0f * std::log10 (maxIn)) : kMeterFloorDb;
        }
        else
        {
            s.classicMeters (numSamples);
        }
    }
    else
    {
        s.meterState = s.peak.state();
        s.meterGrDb = muted ? kGrMeterMaxDb : juce::jlimit (0.0f, kGrMeterMaxDb, -minGr);
        s.meterLevelDb = maxPeak > 0.0f ? juce::jmax (kMeterFloorDb, 20.0f * std::log10 (maxPeak)) : kMeterFloorDb;
    }

    if (! oversized)
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
