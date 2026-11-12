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

// The closing rule (Gate.h; the gate5 specification). How fast the level fell through the
// hysteresis band tells a stop from a fade: a hand stop crosses it in 1 to 44 ms whatever the
// floor, a decay at 60 dB/s takes 100 ms. [choice, check_gate A-D, R, T rows, I1, I3-I5]
constexpr double kFastFallSeconds = 0.05;              // the band in less: a stop, wait the hold
constexpr double kRideRatio = 2.0;                     // slower: wait twice the band time (a beat trough lasts up to 1.7x it) ...
constexpr float kStillFallingRatio = 0.89125094f;      // ... from the last new low 1 dB under the one before
constexpr float kReboundRatio = 2.0f;                  // and from scratch once the level climbs 6 dB back
constexpr double kRideMaxSeconds = 1.0;
// Dips the level came back from: a fall of kDipDb under its own maximum over kFastFallSeconds and a
// return to within kBackDb of it. With the gate open (a chord's partials beating, which the window
// cannot fill) they lengthen the wait after a stop. E2+F2 near threshold leaves troughs of 105 to
// 146 ms, hence the 150 ms cap. Dips under kDipMinSeconds are a low note's own crest gaps (one
// period of 41.2 Hz minus the window): remembered, they would delay its hand stops.
constexpr float kDipDb = 6.0f;
constexpr float kBackDb = 3.0f;
constexpr double kDipMaxSeconds = 0.2;
constexpr double kDipMinSeconds = 0.0125;
constexpr double kExtendMaxSeconds = 0.15;
constexpr double kExtendRatio = 1.25;
constexpr double kExtendMarginSeconds = 0.001;
constexpr double kDipForget = 0.04;                    // memory lost per sample: 40 ms a second
// The deadline: under the open level for hold + T the gain is at the range, whatever the stop rule
// says; it stays at 0 dB until the last 80 ms, then falls -90 dB x u^2, so it is still over -6 dB
// at hold + 191 ms (longer than every chord trough measured but one) and the floor is heard at
// most hold + 250 ms. A release slower than T / 1.5 keeps its own fall to -90 dB.
constexpr double kDeadlineSeconds = 0.25;
constexpr double kDeadlineSpanFraction = 0.32;
constexpr double kDeadlineDepthDb = -90.0;
// In the band (a stop onto a floor just under the threshold never crosses the close level): a
// level kDipDb under its 50 ms maximum and kBandMarginDb deeper than any trough remembered is a
// stop after the hold plus one beat period. Beats: successive dips kBeatDipDb under the maximum
// (re-armed within kBeatRearmDb of it), two intervals in a row within kBeatSteadyRatio of each
// other and 25 to 150 ms long (faster is a low note's own crests).
constexpr double kBandMarginDb = 6.0;
constexpr double kTroughDepthForgetDbPerSecond = 10.0;
constexpr float kBeatDipDb = 6.0f;
constexpr float kBeatRearmDb = 3.0f;
constexpr double kBeatMinSeconds = 0.025;
constexpr double kBeatMaxSeconds = 0.15;
constexpr double kBeatSteadyRatio = 0.25;
// A beat trough that falls as fast as a stop cannot be told from one: the gate reaches -20 dB on
// the stop's timing, then falls only 20 dB per 1.5 remembered dip lengths (troughs lengthen as the
// note decays), so the trough costs a partial dip, not a cut to mute. Dips the gate closed in count
// here, not in the wait, and only if it reopened as slowly as a beat crest climbs (a palm-muted
// note reopens it at once).
constexpr float kGentleFromDb = -20.0f;
constexpr double kGentleDbPerDip = 20.0 / 1.5;
constexpr double kDipRiseSeconds = 0.015;

// Opening follows the onset: half as long as the level took to climb the hysteresis band, from
// version 2's 0.2 ms linear rise for a pick to a 5 ms smoothstep for a swell; linear at the
// shortest (a 0.2 ms smoothstep splatters more), smoothstep from twice that. [choice, check_gate D1, D2, X16]
constexpr double kOpenRatio = 0.5;
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
    float gr = -90.0f, from = 0.0f, phase = 1.0f;
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
        fastSamples = (int) std::lround (kFastFallSeconds * sampleRate);
        crest.prepare (juce::jmax (1, fastSamples));

        rideMax = kRideMaxSeconds * sampleRate;
        dipMax = (int) std::lround (kDipMaxSeconds * sampleRate);
        riseSlow = (int) std::lround (kDipRiseSeconds * sampleRate);
        extendMax = kExtendMaxSeconds * sampleRate;
        extendMargin = kExtendMarginSeconds * sampleRate;
        dipMin = kDipMinSeconds * sampleRate;
        troughForget = kTroughDepthForgetDbPerSecond / sampleRate;
        beatTrigger = 2.0f * std::pow (10.0f, -kBeatDipDb / 20.0f);    // x top, half the maximum
        beatRearm = 2.0f * std::pow (10.0f, -kBeatRearmDb / 20.0f);
        beatMin = kBeatMinSeconds * sampleRate;
        beatMax = kBeatMaxSeconds * sampleRate;
        dipRatio = std::pow (10.0f, -kDipDb / 20.0f);
        backRatio = std::pow (10.0f, (kDipDb - kBackDb) / 20.0f);
        openMin = kOpenMinSeconds * sampleRate;
        openMax = kOpenMaxSeconds * sampleRate;
        floorTauMax = kFloorTauMaxSeconds * sampleRate;
        floorGrowSamples = (int) std::ceil (floorTauMax / kFloorTauGrowth);
        floorSlew = (float) (kFloorSlewDbPerSecond / sampleRate);
        rangeCoeff = (float) (1.0 - std::exp (-1.0 / (kRangeSeconds * sampleRate)));
        lastThreshold = lastRelease = lastHold = lastRange = lastHpf = -1.0e9f;
        reset();
    }

    /** Closed at the range, windows empty, filters at rest, nothing remembered. */
    void reset() noexcept
    {
        for (auto& section : hpf)
            section.clear();

        main.clear();
        raw.clear();
        crest.clear();
        open = openedSinceReset = false;
        below = 0;
        crossed = -1;
        slowFall = false;
        wait = 0.0;
        sinceOpen = sinceLow = kLongAgo;
        dipActive = dipBlocked = dipTainted = false;
        dipAge = pendingDip = 0;
        reopenedSlowly = false;
        dipRef = lowest = lowMark = 0.0f;
        memory = closedMemory = 0.0;
        dipTop = dipLow = 0.0f;
        troughDepth = pendingDepth = 0.0;
        beat = 0.0;
        beatArmed = false;
        beatAge = beatPrev = kLongAgo;
        bandAt = -1;
        bandRef = 0.0f;
        rideFloorDb = 0.0f;
        rideSince = 0;
        floorDb = 0.0f;
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
            smoothOffsetDb = (kExpanderSlope - 1.0f) * 20.0f * std::log10 (closeLevel);
        }

        if (! juce::exactlyEqual (releaseMs, lastRelease))
        {
            lastRelease = releaseMs;
            releaseStep = (float) (60.0 / ((double) releaseMs * 0.001 * sampleRate));
            deadlineSamples = std::round (juce::jmax (kDeadlineSeconds, -kDeadlineDepthDb / 60.0 * (double) releaseMs * 0.001)
                                          * sampleRate);
            deadlineSpan = deadlineSamples * kDeadlineSpanFraction;
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
        sinceOpen = level >= openLevel ? 0 : juce::jmin (sinceOpen + 1, kLongAgo);
        sinceLow = level < closeLevel ? 0 : juce::jmin (sinceLow + 1, kLongAgo);
        const float top = learnDips (level);
        followRange();

        // Under the open level: the deadline, and Smooth's estimate of what it is riding (the
        // closed estimate's law, restarted at the fall under the open level, not at the close).
        float deadline = 0.0f, smoothCap = 0.0f;

        if (level < openLevel)
        {
            if (sinceOpen == 1)
                rideSince = 0;

            const float levelDb = 20.0f * std::log10 (juce::jmax (level, kPeakFloor));

            if (rideSince == 0)
                rideFloorDb = levelDb;
            else
                rideFloorDb += (levelDb - rideFloorDb) * (rideSince < floorGrowSamples
                                                              ? (float) (1.0 / juce::jmax (1.0, kFloorTauGrowth * rideSince))
                                                              : (float) (1.0 / floorTauMax));

            rideSince = juce::jmin (rideSince + 1, floorGrowSamples);
            // Not before the first opening since a reset: Tight is at the range already, and Smooth,
            // never having opened, has nothing to ride and follows its expander as version 3 did.
            if (openedSinceReset)
                deadline = deadlineGr (sinceOpen);

            if (deadline < 0.0f)
                smoothCap = juce::jmax (deadline, juce::jlimit (range, 0.0f, (kExpanderSlope - 1.0f) * rideFloorDb - smoothOffsetDb));
        }

        if (level >= openLevel)
        {
            below = 0;
            crossed = -1;
            bandAt = -1;

            if (! open)
            {
                open = true;
                openedSinceReset = true;
                reopenedSlowly = sinceLow >= riseSlow;

                if (reopenedSlowly)
                {
                    closedMemory = juce::jmax (closedMemory, (double) pendingDip);
                    troughDepth = juce::jmax (troughDepth, pendingDepth);
                }

                pendingDepth = 0.0;
                pendingDip = 0;
                const double steps = juce::jlimit (openMin, openMax, kOpenRatio * (double) sinceLow + 1.0);
                openStep = 1.0f / (float) steps;
                openCurve = (float) juce::jlimit (0.0, 1.0, steps / openMin - 1.0);
                startOpening (tight, tightCache);
                startOpening (smooth, smoothCache);
            }
        }
        else if (open)
        {
            bool closeNow = false;

            // In the band: deeper than any trough ridden lately, a stop after the hold and one
            // beat period; a return to 3 dB under the maximum it fell from cancels it.
            if (bandAt < 0 && level >= closeLevel && level < top
                && 20.0 * std::log10 ((double) (2.0f * top / level)) > troughDepth + kBandMarginDb)
            {
                bandAt = 0;
                bandRef = top * backRatio;
            }

            if (bandAt >= 0)
            {
                if (level >= bandRef)
                {
                    bandAt = -1;
                }
                else if ((double) ++bandAt > holdSamples + juce::jmin (extendMax, beat))
                {
                    closeNow = true;
                    wait = holdSamples;
                }
            }

            if (level >= closeLevel)
            {
                below = 0;
                crossed = -1;
            }
            else
            {
                if (crossed < 0)
                {
                    crossed = 0;
                    wait = waitSamples();
                    slowFall = sinceOpen > fastSamples;
                    lowest = lowMark = level;
                }

                ++crossed;

                // A slow fall that then drops 6 dB under its own recent maximum is a stop from here.
                if (slowFall && level < top)
                {
                    slowFall = false;
                    wait = holdSamples + extension();
                    below = 0;
                }

                // After a slow fall the wait runs from where the level stops falling, and starts
                // over if it climbs back (a beat trough, not a decay on the floor); 1 s at most.
                if (slowFall)
                {
                    lowest = juce::jmin (lowest, level);

                    if (level >= lowest * kReboundRatio)
                    {
                        below = crossed = 0;
                        lowest = lowMark = level;
                    }
                    else if (level <= lowMark * kStillFallingRatio)
                    {
                        below = 0;
                        lowMark = level;
                    }
                }

                if ((double) ++below > wait || (slowFall && (double) crossed > rideMax))
                    closeNow = true;
            }

            if ((double) sinceOpen >= holdSamples + deadlineSamples)
                closeNow = true;

            if (closeNow)
            {
                open = false;
                crossed = -1;
                bandAt = -1;
                beatPrev = kLongAgo;   // a trough the gate closed in breaks the chain: palm-mute gaps are not beats
                tight.closing = smooth.closing = true;
                const double dip = juce::jmax (memory, closedMemory);
                gentleStep = dip > wait ? juce::jmin (releaseStep, (float) (kGentleDbPerDip / dip)) : releaseStep;
                floorSince = 0;
            }
        }

        if (open)
        {
            advanceOpening (tight);
            advanceOpening (smooth);

            if (deadline < 0.0f)
            {
                tight.gr = juce::jmin (tight.gr, deadline);
                smooth.gr = juce::jmin (smooth.gr, smoothCap);
            }

            return;
        }

        const float levelDb = 20.0f * std::log10 (juce::jmax (level, kPeakFloor));

        // Until the window has filled once after a reset, the estimate and the expander take the
        // level as it comes: there is no floor yet for the slew to protect.
        const bool settling = settle > 0;

        if (settling)
            --settle;

        if (floorSince == 0 || settling)
            floorDb = levelDb;
        else
            floorDb += (levelDb - floorDb) * (floorSince < floorGrowSamples
                                                  ? (float) (1.0 / juce::jmax (1.0, kFloorTauGrowth * floorSince))
                                                  : (float) (1.0 / floorTauMax));

        floorSince = juce::jmin (floorSince + 1, floorGrowSamples);

        const float smoothBefore = smooth.gr;
        advanceClosed (tight, range, kNoSlewLimit);
        advanceClosed (smooth, juce::jlimit (range, 0.0f, (kExpanderSlope - 1.0f) * floorDb - smoothOffsetDb),
                       settling ? kNoSlewLimit : floorSlew);

        // Inside the deadline's window Smooth only falls: a fresh closed estimate landing low on
        // an instant of the level would otherwise slew back up and read as a reopening.
        if (deadline < 0.0f)
        {
            tight.gr = juce::jmin (tight.gr, deadline);
            smooth.gr = juce::jmin (smooth.gr, smoothBefore, smoothCap);
        }
    }

    float tightGain() noexcept { return gainFor (tight.gr, tightCache); }
    float smoothGain() noexcept { return gainFor (smooth.gr, smoothCache); }
    float gainOf (int mode) noexcept { return mode == (int) GateFx::Mode::smooth ? smoothGain() : tightGain(); }
    float grOf (int mode) const noexcept { return mode == (int) GateFx::Mode::smooth ? smooth.gr : tight.gr; }

    /** Once per block: filter states that went tiny or bad. */
    void endBlock() noexcept
    {
        for (auto& section : hpf)
            section.endBlock();
    }

    float currentPeak() const noexcept { return mainPeak; }
    bool isMuted (float gr) const noexcept { return mute && gr <= kMuteDb; }
    float floorGain() const noexcept { return rangeTarget <= kMuteDb ? 0.0f : std::exp (rangeTarget * kDbToGain); }

    /** 2 open, 1 in the hysteresis band or waiting, 0 closed (§4.1.5). */
    int state() const noexcept { return open ? (mainPeak >= openLevel ? 2 : 1) : 0; }

private:
    struct GainCache
    {
        float gr = 1.0f, gain = 1.0f;
    };

    static constexpr int kLongAgo = 1 << 30;
    static constexpr float kNoSlewLimit = 1.0e9f;

    /** The wait under the close level, decided as the level crosses it (Gate.h). */
    double waitSamples() const noexcept
    {
        const double afterStop = holdSamples + extension();

        if (sinceOpen <= fastSamples)
            return afterStop;

        return juce::jmax (afterStop, juce::jmin (rideMax, kRideRatio * (double) sinceOpen));
    }

    /** The deadline's gain at `since` samples under the open level (Gate.h). */
    float deadlineGr (int since) const noexcept
    {
        const double start = holdSamples + deadlineSamples - deadlineSpan;

        if ((double) since <= start)
            return 0.0f;

        const double u = ((double) since - start) / deadlineSpan;
        return juce::jmax (range, u < 1.0 ? (float) (kDeadlineDepthDb * u * u) : -100.0f);
    }

    /** The remembered trough's share of the wait after a stop. */
    double extension() const noexcept
    {
        return memory > 0.0 ? juce::jmin (extendMax, kExtendRatio * memory + extendMargin) : 0.0;
    }

    /** Remembers how long and how deep the level's recent dips were, and its beat period (see
        kDipDb, kBandMarginDb, kGentleFromDb). Returns `top`: half the level's 50 ms maximum. */
    float learnDips (float level) noexcept
    {
        const float top = crest.push (level) * dipRatio;
        memory = juce::jmax (0.0, memory - kDipForget);
        closedMemory = juce::jmax (0.0, closedMemory - kDipForget);
        troughDepth = juce::jmax (0.0, troughDepth - troughForget);
        beat = juce::jmax (0.0, beat - kDipForget);
        beatAge = juce::jmin (beatAge + 1, kLongAgo);

        if (open)
        {
            if (beatArmed && level < top * beatTrigger)
            {
                beatArmed = false;
                const int interval = beatAge;
                const bool steady = std::abs ((double) (interval - beatPrev)) <= kBeatSteadyRatio * (double) beatPrev;
                beatAge = 0;
                beatPrev = interval;

                if (steady && (double) interval >= beatMin && (double) interval <= beatMax && (double) interval > beat)
                    beat = (double) interval;
            }
            else if (! beatArmed && level >= top * beatRearm)
            {
                beatArmed = true;
            }
        }

        if (dipActive)
        {
            ++dipAge;
            dipTainted = dipTainted || ! open;
            dipLow = juce::jmin (dipLow, level);

            if (level >= dipRef)
            {
                const bool longEnough = (double) dipAge >= dipMin;

                if (longEnough && (! dipTainted || reopenedSlowly))
                    troughDepth = juce::jmax (troughDepth, dipDepth());

                if (! dipTainted)
                {
                    if (longEnough)
                        memory = juce::jmax (memory, (double) dipAge);
                }
                else if (reopenedSlowly)
                {
                    closedMemory = juce::jmax (closedMemory, (double) dipAge);
                }
                else if (! open)
                {
                    pendingDip = dipAge;   // came back before the gate reopened: judged when it does

                    if (longEnough)
                        pendingDepth = dipDepth();
                }

                dipActive = false;
            }
            else if (dipAge > dipMax)
            {
                dipActive = false;
                dipBlocked = true;
            }
        }
        else if (dipBlocked)
        {
            dipBlocked = level < top;
        }
        else if (level < top)
        {
            dipActive = true;
            dipTainted = ! open;
            dipAge = 0;
            reopenedSlowly = false;
            dipRef = top * backRatio;
            dipTop = 2.0f * top;
            dipLow = level;
        }

        return top;
    }

    double dipDepth() const noexcept
    {
        return 20.0 * std::log10 ((double) dipTop / juce::jmax ((double) dipLow, 1.0e-12));
    }

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
        r.from = gainFor (r.gr, cache);
        r.phase = r.gr >= 0.0f ? 1.0f : 0.0f;
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
    void advanceClosed (Ramp& r, float target, float slew) const noexcept
    {
        if (r.closing)
        {
            if (r.gr > target)
                r.gr = juce::jmax (target, r.gr - (r.gr <= kGentleFromDb ? gentleStep : releaseStep));

            r.closing = r.gr > target;
            return;
        }

        r.gr += juce::jlimit (-slew, slew, target - r.gr);
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

    MaxWindow main, raw, crest;   // filtered, unfiltered, recent maximum of the level

    float openLevel = 1.0e-4f, closeLevel = 5.0e-5f, smoothOffsetDb = 0.0f, mainPeak = 0.0f;
    float releaseStep = 0.04f, gentleStep = 0.04f, openStep = 1.0f, openCurve = 0.0f, floorSlew = 0.002f;
    float dipRatio = 0.5f, backRatio = 1.4f, rangeCoeff = 0.002f;
    float range = -90.0f, rangeTarget = -90.0f;
    double holdSamples = 0.0, rideMax = 48000.0, extendMax = 960.0, extendMargin = 48.0;
    double openMin = 9.6, openMax = 240.0, floorTauMax = 9600.0;
    int fastSamples = 2400, dipMax = 4800, riseSlow = 240, floorGrowSamples = 19200;
    bool mute = true;

    Ramp tight, smooth;
    float floorDb = 0.0f, dipRef = 0.0f, lowest = 0.0f, lowMark = 0.0f;
    int floorSince = 0, below = 0, crossed = -1, sinceOpen = 0, sinceLow = 0, settle = 0, windowSamples = 576;
    int dipAge = 0, pendingDip = 0;
    bool open = false, slowFall = false;
    bool dipActive = false, dipBlocked = false, dipTainted = false, reopenedSlowly = false;
    double wait = 0.0, memory = 0.0, closedMemory = 0.0;

    // Deadline, in-band stop, trough depth and beat period (Gate.h).
    double deadlineSamples = 12000.0, deadlineSpan = 3840.0, dipMin = 600.0, troughForget = 2.0e-4;
    double beatMin = 1200.0, beatMax = 7200.0;
    float beatTrigger = 1.0f, beatRearm = 1.4f;
    float dipTop = 0.0f, dipLow = 0.0f, bandRef = 0.0f, rideFloorDb = 0.0f;
    double troughDepth = 0.0, pendingDepth = 0.0, beat = 0.0;
    int beatAge = 0, beatPrev = 0, bandAt = -1, rideSince = 0;
    bool beatArmed = false, openedSinceReset = false;

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
