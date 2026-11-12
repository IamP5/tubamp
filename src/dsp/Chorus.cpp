#include "Chorus.h"

#include "FxPrimitives.h"

#include <algorithm>
#include <cmath>

namespace tubamp
{
namespace
{
// Shared machinery of modes 1-4 (docs/FX-ENGINES.md §4.5.4).

constexpr float kDelayMaxSeconds = 0.020f;   // D_max [choice: §4.5.4, D-12]: the 30-cent law holds down to 0.44 Hz
constexpr float kLineSeconds = 0.040f;       // [choice] >= the 34 ms §4.5.4 asks for (10 ms + D_max + Tri shimmer)
constexpr float kMinRateHz = 0.01f;          // ChorusParams::rateHz range [spec: §4.5.2]
constexpr float kMaxRateHz = 20.0f;
constexpr float kSmoothSeconds = 0.02f;      // [choice: §2.5]
constexpr int kControlInterval = 16;         // D and filter coefficients [spec: §4.5.4]
constexpr float kRoundMinHz = 8.0f;          // corner rounding fc = max(8 Hz, 20 rate) [study: shared design]
constexpr float kRoundPerHz = 20.0f;
constexpr float kMaxCutoffRatio = 0.45f;     // [spec: §4.5.4]
constexpr float kSvfDamping = 1.41421356f;   // Butterworth k = sqrt 2 [spec: §4.5.4]
constexpr float kFlushBelow = 1.0e-15f;      // [spec: §2.6]
constexpr float kToneOctaves = 2.0f;         // LP corner x 2^(2 (tone - 0.5)) [spec: §4.5.4]

// Wide [study: R1]
constexpr float kWideCentsMax = 30.0f;       // [fit: chorus/evidence/brigade_chorus.tsv, 29.3 cents at 2.859 Hz]
constexpr float kWideBaseSeconds[2] { 0.006f, 0.010f };
constexpr float kWidePair2Ratio = 1.41421356f;
constexpr float kWidePair2Gain = 0.7f;
constexpr float kWideNorm = 0.819f;          // 1 / sqrt (1 + 0.7^2) [choice: §4.5.5]
constexpr float kWideOffsetR = 0.25f;        // quadrature within a pair [study: R1]
constexpr float kWidePair2Offset = 0.5f;     // [choice: §4.5.5, pairs start half a cycle apart]
constexpr float kWideHpHz = 60.0f;
constexpr float kWideLpHz = 9000.0f;

// CE [study: R2; ver: V:46]
constexpr float kCeBaseSeconds = 0.00295f;   // 1.6 - 4.3 ms sweep [fit: brigade_chorus.tsv]
constexpr float kCeSweepSeconds = 0.0027f;
constexpr float kCeBend = 0.1f;              // [fit: brigade_chorus.tsv; ver: V:48]
constexpr float kCeTopRateHz = 2.86f;        // the pedal's top rate [study: brigade_chorus.tsv; ver: V:46]
constexpr float kCeOffsetR = 0.25f;          // [study: R2]
constexpr float kCeHpHz = 130.0f;            // [fit: study R2 to brigade_fr.tsv]
constexpr float kCeLp2Hz = 4600.0f;
constexpr float kCeLp1Hz = 9000.0f;

// Dimension [study: R3; ver: V:53]
constexpr float kDimBaseSeconds = 0.0085f;   // [study: Anwander]
constexpr float kDimSwingSeconds = 0.0025f;
// Above 0.5 Hz the swing shrinks as 1/f, holding the detune at its 0.5 Hz value (8.6 cents
// at depth 1) [choice: 0.5 Hz is the fastest Dimension D mode (Anwander); a fixed swing
// gave 182 cents at 10 Hz, the defect V:46 found in CE].
constexpr float kDimTopRateHz = 0.5f;
constexpr float kDimHpHz = 80.0f;            // [study: Anwander SDD-320]
constexpr float kDimLpHz = 10000.0f;         // [study: tagged guess]

// Tri [study: R4]
constexpr float kTriBaseSeconds = 0.007f;
constexpr float kTriCentsMax = 20.0f;
constexpr float kTriFastHz = 6.0f;
constexpr float kTriFastCents = 3.0f;
constexpr float kTriPairGain = 0.7f;
constexpr float kTriHpHz = 80.0f;
constexpr float kTriLpHz = 6000.0f;
constexpr float kCos120 = -0.5f;
constexpr float kSin120 = 0.866025404f;

// Transitions (docs/FX-ENGINES.md §4.5.4). A voice's pitch is the slope of its delay, so
// nothing but the LFO at the settings' own depth may move a delay quickly.
// While settings move, every voice delay is slope-limited to the largest peak slope of the
// settings involved plus this much [choice: 4 cents, under the 5-cent acceptance margin].
constexpr float kPitchAllowance = 0.0023131618f;   // 2^(4/1200) - 1
// A change the slope limit cannot finish soon is made by a tap crossfade: every voice
// starts a second read tap on the new law, the outgoing tap moves on with it a fixed
// distance away, and the two are crossfaded (directly, or through the taps half way between
// when those are more alike: readVoices, crossfade). Two taps of one signal a fixed distance
// apart carry no pitch of their own, only a comb while both sound, so the fade keeps the
// wet's short-term power on the straight line between the two taps' own (measured, as for a
// mode switch). Fading the wet out and back in instead leaves a hole at mix 1.
// Glide or crossfade (Chorus.h states the limit). A glide bends every note by at most the
// 4-cent allowance but takes (delay change) x range / allowance; a crossfade slides each
// voice's phase by the delay change, at most half a cycle, within kTapFadeSeconds, 1.5 times
// as fast at its steepest (smoothstep), which bends 82 Hz more than the glide does as soon as
// the glide would take over 20 ms. So the glide is the smaller bend wherever it keeps up, and
// the crossfade is the tool for lag alone.
// A knob that stopped this long ago is a step [choice: a hand's knob move pauses longer]...
constexpr float kQuietSeconds = 0.030f;
// ...and a step whose glide would need longer than this is crossfaded instead [choice: the
// longest that still settles inside 150 ms of the knob stopping].
constexpr float kGlideSeconds = 0.080f;
// A moving knob is followed by the glide; once the glide falls this far behind, the rest
// is crossfaded without waiting for the knob to stop.
constexpr float kCatchUpSeconds = 0.100f;
// At most one knob crossfade starts in this long [choice: an automation ramp the glide
// cannot follow then lags between two, instead of firing a train of blips].
constexpr float kFadeSpacingSeconds = 0.250f;
// The new tap's power is measured this long before it fades in [choice: as the mode switch].
constexpr float kTapMeasureSeconds = 0.010f;
// A retarget restarts the measurement only inside this much pre-roll; later ones wait for
// the fade to end, so a knob in motion cannot postpone the fade indefinitely.
constexpr float kTapPrerollSeconds = 0.020f;
// [spec: 20 - 30 ms, cornerless] A longer fade would slide the phase more slowly (60 ms halves
// the blip) but deepens the comb the taps leave against the dry and the steady wets while the
// LFO moves under it (wet -1.8 dB at 60 ms, -2.1 at 45 ms on a depth ramp, CH25; jumps CH30).
constexpr float kTapFadeSeconds = 0.030f;
constexpr float kTapPowerSeconds = 0.004f;         // two cascaded one-poles, as kSwitchPowerSeconds
// Headroom of the power correction: at most +8.8 dB. The path choice keeps a tone's two faded
// taps no worse than -0.5 correlated, which this lifts to -0.53 dB at the midpoint.
constexpr float kTapSlack = 0.15f;
// After reset() the wet fades in over this once the lines hold enough history.
constexpr float kFadeInSeconds = 0.010f;

// Song-position lock. A host's song position is taken as continuous when it lands within
// this much of where the last one predicts (last + rate x elapsed): 0.01 cycle or 5 ms of
// LFO time, whichever is larger [choice: hosts report positions to the sample, a +-16
// sample jitter is 0.33 ms at 48 kHz and 0.73 ms at 22.05 kHz, a tempo change between two
// blocks of 4096 a few hundredths of a ms; a locate, a loop that is not a whole number of
// cycles or a note-value change moves the phase by more]. Anything larger is a jump.
constexpr double kLockSlackCycles = 0.01;
constexpr double kLockSlackSeconds = 0.005;
// Inside that, the LFO runs free at the tempo's rate and its own error against the host is
// pulled in with this time constant, by a rate nudge that keeps every voice inside its
// steady slope + 0.9 kPitchAllowance and the rate within kLockMaxRatio of its own.
constexpr float kLockSeconds = 0.150f;
constexpr double kLockMaxRatio = 0.25;
// An error this large is not drift: it is taken as a jump.
constexpr double kLockLostCycles = 0.25;
// A discontinuity this soon after the last one is noise, not a locate or a loop [choice: a loop
// shorter than a quarter second is not music]: the LFO runs free until the host settles.
constexpr float kJumpTrainSeconds = 0.250f;

// Mode switches crossfade the outgoing and incoming outputs [choice: §2.4's 20 ms], scaled so
// the short-term power moves straight from one mode's to the other's whatever the two
// outputs' correlation (a chord gives -0.3 to +0.45 between modes' wets, so a plain
// equal-power fade would dip or bump by up to 1.6 dB).
constexpr float kSwitchSeconds = 0.020f;
// The powers are two cascaded one-poles (so a low note's ripple does not modulate the fade),
// run for kSwitchMeasureSeconds before the fade starts.
constexpr float kSwitchPowerSeconds = 0.004f;
constexpr float kSwitchMeasureSeconds = 0.010f;
// Two modes on one held note can sit near antiphase, where no gain undoes the null their fade
// passes, so it starts where every channel's two outputs correlate no worse than kSwitchMinCorrelation
// and are not heading below it; after kSwitchMaxWaitSeconds on any moment good enough now, after
// kSwitchLongestWaitSeconds on any [choice: as FxBlocks' PowerCrossfade, the processor's effect
// switch]. Each channel is corrected on its own, in two bands split at kSwitchSplitHz (two
// modes' lows can cancel while their highs add), by at most +18 dB.
constexpr float kSwitchMinCorrelation = -0.5f;
constexpr float kSwitchMaxWaitSeconds = 0.250f;
constexpr float kSwitchLongestWaitSeconds = 0.400f;
constexpr float kSwitchSplitHz = 1000.0f;
constexpr float kSwitchSlack = 1.0f / (7.943f * 7.943f - 1.0f);
constexpr float kMinMultiplier = 0.02f;
constexpr float kNoCap = 1.0e30f;

// Classic keeps juce::dsp::Chorus, whose line reads at most 7 ms + 10 ms back.
constexpr float kClassicReachSeconds = 0.017f;

/** Delay swing that gives `cents` of detune for a modulator whose peak slope is
    `slopePerHz` * f * D (2 for a triangle, pi for a sine), capped at D_max. */
float centsSwing (float cents, float rateHz, float slopePerHz) noexcept
{
    const float k = std::exp2 (cents * (1.0f / 1200.0f)) - 1.0f;
    return std::min (k / (slopePerHz * rateHz), kDelayMaxSeconds);
}

/** The longest delay a mode can read, i.e. how much history its line needs before it is heard. */
float reachSeconds (int mode) noexcept
{
    switch ((ChorusFx::Mode) mode)
    {
        case ChorusFx::Mode::wide:      return kWideBaseSeconds[1] + kDelayMaxSeconds;
        case ChorusFx::Mode::ce:        return kCeBaseSeconds + 0.5f * kCeSweepSeconds;
        case ChorusFx::Mode::dimension: return kDimBaseSeconds + kDimSwingSeconds;
        case ChorusFx::Mode::tri:       return kTriBaseSeconds + kDelayMaxSeconds + 0.0001f; // + the 92 us shimmer
        case ChorusFx::Mode::classic:
        default:                        return kClassicReachSeconds;
    }
}

inline float wrap01 (float x) noexcept
{
    return x - std::floor (x);
}

inline float triangle (float phase) noexcept
{
    return 1.0f - std::abs (2.0f * wrap01 (phase) - 1.0f);
}

/** The BBD clock's bend: slope x(1 - b) at the short end, x(1 + b) at the long end. */
inline float bentTriangle (float phase) noexcept
{
    const float u = triangle (phase);
    return u * (1.0f + kCeBend * (u - 1.0f));
}

/** sin and cos of 2 pi t. Quadrant reduction, then Taylor terms on [-pi/4, pi/4]
    (error < 4e-7), far cheaper than two libm calls per sample. */
inline void sinCosTurns (float t, float& s, float& c) noexcept
{
    static constexpr float quadCos[4] { 1.0f, 0.0f, -1.0f, 0.0f };
    static constexpr float quadSin[4] { 0.0f, 1.0f, 0.0f, -1.0f };

    const float q = std::floor (t * 4.0f + 0.5f);
    const float x = (t - 0.25f * q) * fx::kTwoPi;
    const float x2 = x * x;
    const float s0 = x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f))));
    const float c0 = 1.0f + x2 * (-0.5f + x2 * (1.0f / 24.0f + x2 * (-1.0f / 720.0f + x2 * (1.0f / 40320.0f))));
    const int k = (int) q & 3;
    s = s0 * quadCos[k] + c0 * quadSin[k];
    c = c0 * quadCos[k] - s0 * quadSin[k];
}

/** Trapezoidal one-pole gain G = g / (1 + g). */
float tptGain (float hz, float sampleRate) noexcept
{
    const float g = std::tan (fx::kPi * std::min (hz, kMaxCutoffRatio * sampleRate) / sampleRate);
    return g / (1.0f + g);
}

struct WetCoefficients
{
    float hp = 1.0f;                  // TPT one-pole G
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f; // TPT SVF (Zavalishin / Simper)
    float lp1 = 1.0f;                 // CE's extra one-pole

    void set (float hpHz, float lpHz, float lp1Hz, float sampleRate) noexcept
    {
        hp = tptGain (hpHz, sampleRate);
        const float g = std::tan (fx::kPi * std::min (lpHz, kMaxCutoffRatio * sampleRate) / sampleRate);
        a1 = 1.0f / (1.0f + g * (g + kSvfDamping));
        a2 = g * a1;
        a3 = g * a2;
        lp1 = lp1Hz > 0.0f ? tptGain (lp1Hz, sampleRate) : 1.0f;
    }
};

/** The wet band-limit in front of a line: HP1 -> LP2 Butterworth (-> LP1 for CE). */
struct WetFilter
{
    float hpState = 0.0f, ic1 = 0.0f, ic2 = 0.0f, lp1State = 0.0f;

    void clear() noexcept { hpState = ic1 = ic2 = lp1State = 0.0f; }

    float process (float x, const WetCoefficients& c) noexcept
    {
        const float v = (x - hpState) * c.hp;
        const float low = v + hpState;
        hpState = low + v;
        const float hp = x - low;

        const float v3 = hp - ic2;
        const float v1 = c.a1 * ic1 + c.a2 * v3;
        const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v2;
    }

    float processCe (float x, const WetCoefficients& c) noexcept
    {
        const float y = process (x, c);
        const float v = (y - lp1State) * c.lp1;
        const float out = v + lp1State;
        lp1State = out + v;
        return out;
    }

    void tidy() noexcept
    {
        for (float* s : { &hpState, &ic1, &ic2, &lp1State })
            *s = std::abs (*s) < kFlushBelow ? 0.0f : fx::sanitise (*s);
    }
};

struct Smoothed
{
    float current = 0.0f, target = 0.0f;

    float next (float a) noexcept
    {
        current += a * (target - current);
        return current;
    }

    void snap() noexcept { current = target; }

    /** Lands exactly on the target instead of creeping towards it in denormal steps. */
    void settle() noexcept
    {
        if (std::abs (target - current) < 1.0e-7f)
            current = target;
    }
};

//==============================================================================
/** How a mode's voice delays move: base + swing * multiplier. `slope` is the delay's peak
    slope per hertz of LFO rate and second of swing; `range` the multiplier's largest magnitude. */
struct GroupLaw
{
    float slope, range;
};

GroupLaw groupLaw (int mode) noexcept
{
    switch ((ChorusFx::Mode) mode)
    {
        case ChorusFx::Mode::ce:        return { 2.0f * (1.0f + kCeBend), 0.5f };
        case ChorusFx::Mode::dimension: return { 4.0f, 1.0f };
        case ChorusFx::Mode::tri:       return { fx::kPi, 1.0f };
        case ChorusFx::Mode::wide:
        case ChorusFx::Mode::classic:
        default:                        return { 2.0f, 1.0f };
    }
}

/** Swing (seconds) and rate of each LFO group for a rate and depth. Group 0 is the master
    LFO; group 1 is Wide's second pair or Tri's 6 Hz shimmer. */
void swingLaw (int mode, float f, float depthLin, float swingOut[2], float rateOut[2]) noexcept
{
    const float depth2 = depthLin * depthLin;
    swingOut[0] = swingOut[1] = 0.0f;
    rateOut[0] = rateOut[1] = f;

    switch ((ChorusFx::Mode) mode)
    {
        case ChorusFx::Mode::wide:
            rateOut[1] = f * kWidePair2Ratio;
            swingOut[0] = centsSwing (kWideCentsMax * depth2, f, 2.0f);
            swingOut[1] = centsSwing (kWideCentsMax * depth2, rateOut[1], 2.0f);
            break;
        case ChorusFx::Mode::ce:
            // Above the pedal's top rate the swing shrinks as 1/f, so the detune stays at
            // its 2.86 Hz value instead of growing to 91 cents at 10 Hz (V:46).
            swingOut[0] = kCeSweepSeconds * depthLin * std::min (1.0f, kCeTopRateHz / f);
            break;
        case ChorusFx::Mode::dimension:
            swingOut[0] = kDimSwingSeconds * depthLin * std::min (1.0f, kDimTopRateHz / f);
            break;
        case ChorusFx::Mode::tri:
            rateOut[1] = kTriFastHz;
            swingOut[0] = centsSwing (kTriCentsMax * depth2, f, fx::kPi);
            swingOut[1] = centsSwing (kTriFastCents, kTriFastHz, fx::kPi);
            break;
        case ChorusFx::Mode::classic:
        default:
            break;
    }
}

//==============================================================================
/** One of modes 1-4 on one pair of lines. The lines, filters and LFO state are shared by
    the four modes; a mode switch runs a second instance (ChorusFx::Impl). */
class ModernChorus
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = (float) newSampleRate;
        invSampleRate = 1.0 / newSampleRate;
        smoothA = 1.0f - std::exp (-1.0f / std::max (1.0f, kSmoothSeconds * sampleRate));
        fadeInStep = 1.0f / std::max (1.0f, kFadeInSeconds * sampleRate);
        tapStep = 1.0f / std::max (1.0f, kTapFadeSeconds * sampleRate);
        tapPowerA = 1.0f - std::exp (-1.0f / std::max (1.0f, kTapPowerSeconds * sampleRate));
        tapMeasure = (int) std::ceil (kTapMeasureSeconds * sampleRate);
        tapPreroll = (int) std::ceil (kTapPrerollSeconds * sampleRate);
        slopeBlend = 2.0f / (float) std::max (2, tapMeasure);
        lockA = 1.0 - std::exp (-1.0 / std::max (1.0, (double) kLockSeconds * newSampleRate));
        jumpTrain = (int) std::ceil (kJumpTrainSeconds * sampleRate);
        quietSamples = quietNeeded = (int) std::ceil (kQuietSeconds * sampleRate);
        glideSamples = kGlideSeconds * sampleRate;
        catchUpSamples = kCatchUpSeconds * sampleRate;
        fadeSpacing = (int) std::ceil (kFadeSpacingSeconds * sampleRate);

        for (auto& line : lines)
            line.setSize ((int) std::ceil (kLineSeconds * sampleRate) + 8);

        oldMax = (float) lines[0].capacity() - 2.0f;
        clear();
    }

    /** Empty lines, LFO at phase 0, and the wet silent until the lines hold as much history
        as the mode reads; then it fades in (unless autoFadeIn is cleared). */
    void clear() noexcept
    {
        for (auto& line : lines)
            line.clear();

        for (auto& f : filters)
            f.clear();

        phase[0] = phase[1] = 0.0;
        std::fill (std::begin (shape), std::end (shape), 0.0f);
        std::fill (std::begin (swing), std::end (swing), 0.0f);
        std::fill (std::begin (swingStep), std::end (swingStep), 0.0f);
        std::fill (std::begin (hold), std::end (hold), 0.0f);
        std::fill (std::begin (rateCap), std::end (rateCap), kNoCap);
        std::fill (std::begin (delay), std::end (delay), 0.0f);
        std::fill (std::begin (step), std::end (step), 0.0f);
        std::fill (std::begin (oldDelay), std::end (oldDelay), 0.0f);
        std::fill (std::begin (oldSlope), std::end (oldSlope), 0.0f);
        std::fill (std::begin (oldStep), std::end (oldStep), 0.0f);
        std::fill (std::begin (voiceLimit), std::end (voiceLimit), kNoCap);
        tick = 0;
        snapControl = true;
        clamped = false;
        rawDirty = true;
        sinceMove = sinceFade = 1 << 30;
        lastFadeStopped = false;
        xf = XFade::none;
        xfSamples = 0;
        xfPos = 0.0f;
        jumpPending = syncLand = false;
        std::fill (std::begin (viaMid), std::end (viaMid), false);
        hostValid = false;
        sinceHost = 0;
        sinceJump = 1 << 30;
        lockError = lockMax = 0.0;
        wet = Wet::silent;
        wetPos = wetStep = 0.0f;
        refilling = autoFadeIn = true;
        filled = 0;
    }

    void setTargets (const ChorusParams& p) noexcept
    {
        if (! juce::exactlyEqual (p.rateHz, rate.target) || ! juce::exactlyEqual (p.depth, depth.target))
        {
            sinceMove = 0;
            rawDirty = true;
        }

        rate.target = p.rateHz;
        depth.target = p.depth;
        mix.target = p.mix;
        width.target = p.width;
        tone.target = p.tone;
    }

    void snapSmoothers() noexcept
    {
        for (auto* s : { &rate, &depth, &mix, &width, &tone })
            s->snap();
    }

    /** An incoming instance continues the outgoing one's mix (so their dry gains stay
        equal), LFO and song-position lock. */
    void continueFrom (const ModernChorus& other) noexcept
    {
        mix = other.mix;
        phase[0] = other.phase[0];
        phase[1] = other.phase[1];
        hostPhase = other.hostPhase;
        hostRate = other.hostRate;
        sinceHost = other.sinceHost;
        sinceJump = other.sinceJump;
        hostValid = other.hostValid;
        lockError = other.lockError;
    }

    /** Song-position lock (kLockSlackCycles): the host's phase is compared with where its
        previous one predicts. A discontinuity there is one jump, made by a tap crossfade;
        otherwise the LFO runs free and its own error is pulled in by the lock. */
    void syncTo (double cycles) noexcept
    {
        const double wrapped = cycles - std::floor (cycles);
        const double error = wrapHalf (wrapped - phase[0]);
        const double slack = std::max (kLockSlackCycles, kLockSlackSeconds * (double) rate.target);
        const double jump = hostValid ? wrapHalf (wrapped - (hostPhase + hostRate * (double) sinceHost * invSampleRate))
                                      : error;

        hostPhase = wrapped;
        hostRate = (double) rate.target;
        sinceHost = 0;
        hostValid = true;

        const bool discontinuous = std::abs (jump) > slack;

        if (discontinuous && sinceJump < jumpTrain)
        {
            // One of a train: the host's position is noisier than the slack, and following it
            // would hold the outgoing taps for good. Run free at the tempo's rate instead.
            sinceJump = 0;
            lockError = 0.0;
        }
        else if (discontinuous || std::abs (error) > kLockLostCycles)
        {
            sinceJump = 0;
            ++counts.syncJumps;
            requestJump (true);
        }
        else if (! syncLand)
        {
            lockError = error;
        }
    }

    /** No song position this block: the LFO runs free. */
    void unlock() noexcept
    {
        hostValid = syncLand = false;
        lockError = 0.0;
    }

    static int reachSamples (int mode, double fs) noexcept
    {
        return (int) std::ceil (reachSeconds (mode) * fs) + 4;
    }

    bool ready() const noexcept { return ! refilling; }
    void setWetSteady() noexcept { wet = Wet::steady; wetPos = 1.0f; }

    /** Host automation reaches setTargets() once per host block, so a knob is only still
        once a whole block has passed without a change. */
    void setHostBlock (int numSamples) noexcept
    {
        quietNeeded = std::max (quietSamples, numSamples + kControlInterval);
    }

    void process (juce::dsp::AudioBlock<float>& block, int mode, int solo, float* const* trace) noexcept
    {
        const bool stereo = block.getNumChannels() > 1;
        traceOut = solo >= 0 ? trace : nullptr;

        switch ((ChorusFx::Mode) mode)
        {
            case ChorusFx::Mode::wide:
                solo < 0 ? runWide<false> (block, stereo, 0) : runWide<true> (block, stereo, std::min (solo, 3));
                break;
            case ChorusFx::Mode::ce:
                solo < 0 ? runCe<false> (block, stereo, 0) : runCe<true> (block, stereo, std::min (solo, 1));
                break;
            case ChorusFx::Mode::dimension:
                solo < 0 ? runDimension<false> (block, stereo, 0) : runDimension<true> (block, stereo, std::min (solo, 1));
                break;
            case ChorusFx::Mode::tri:
                solo < 0 ? runTri<false> (block, stereo, 0) : runTri<true> (block, stereo, std::min (solo, 2));
                break;
            case ChorusFx::Mode::classic:
            default:
                break;
        }

        // A mono block only runs the left chain; keep the right one in step with it.
        if (! stereo)
            filters[1] = filters[0];
    }

    bool autoFadeIn = true;
    ChorusFx::DebugCounts counts;

private:
    enum class Wet { steady, fading, silent };
    // landing: the outgoing taps alone, the new ones land on their law at the next control
    // tick; measuring: both run, the new ones silent; fading: the crossfade.
    enum class XFade { none, landing, measuring, fading };

    static double wrapHalf (double x) noexcept
    {
        return x - std::floor (x + 0.5);
    }

    /** Once per kControlInterval samples: filter coefficients, the delay swings (reached
        linearly over the next interval), the LFO corner rounding and the transition limits. */
    void control (int mode) noexcept
    {
        for (auto* s : { &rate, &depth, &mix, &width, &tone })
            s->settle();

        for (auto& f : filters)
            f.tidy();

        if (xf == XFade::landing)
        {
            land();
            xf = XFade::measuring;
            xfSamples = 0;
            std::fill (&tapPower[0][0][0], &tapPower[0][0][0] + 48, 0.0f);
        }

        const float f = rate.current;
        const float toneScale = std::exp2 (kToneOctaves * (tone.current - 0.5f));

        switch ((ChorusFx::Mode) mode)
        {
            case ChorusFx::Mode::wide:      coefs.set (kWideHpHz, kWideLpHz * toneScale, 0.0f, sampleRate); break;
            case ChorusFx::Mode::ce:        coefs.set (kCeHpHz, kCeLp2Hz * toneScale, kCeLp1Hz * toneScale, sampleRate); break;
            case ChorusFx::Mode::dimension: coefs.set (kDimHpHz, kDimLpHz * toneScale, 0.0f, sampleRate); break;
            case ChorusFx::Mode::tri:       coefs.set (kTriHpHz, kTriLpHz * toneScale, 0.0f, sampleRate); break;
            case ChorusFx::Mode::classic:
            default:                        break;
        }

        const GroupLaw law = groupLaw (mode);
        float target[2], lfoRate[2];
        swingLaw (mode, f, depth.current, target, lfoRate);

        if (rawDirty)
        {
            // The settings the knobs are heading for bound the transition from the start.
            float rawSwing[2], rawRate[2];
            swingLaw (mode, rate.target, depth.target, rawSwing, rawRate);

            for (int k = 0; k < 2; ++k)
                rawSlope[k] = law.slope * rawRate[k] * rawSwing[k];

            rawDirty = false;
        }

        float slope[2];

        for (int k = 0; k < 2; ++k)
        {
            slope[k] = law.slope * lfoRate[k] * target[k];
            const float corner = std::max (kRoundMinHz, kRoundPerHz * lfoRate[k]);
            roundA[k] = 1.0f - std::exp (-fx::kTwoPi * std::min (corner, kMaxCutoffRatio * sampleRate) / sampleRate);
            // The rounding one-pole delays the LFO's fundamental by atan (f / fc); reading the
            // shape that much ahead puts it back on the phase, so a synced LFO lands on the beat.
            lead[k] = std::atan (lfoRate[k] / corner) * (1.0f / fx::kTwoPi);
        }

        if (snapControl)
        {
            for (int k = 0; k < 2; ++k)
            {
                swing[k] = target[k] * sampleRate;
                swingStep[k] = 0.0f;
                hold[k] = std::max (slope[k], rawSlope[k]);
                rateCap[k] = kNoCap;
            }

            std::fill (std::begin (voiceLimit), std::end (voiceLimit), kNoCap);
            primeShapes (mode);
            snapControl = clamped = false;
        }
        else
        {
            glide (mode, law, target, lfoRate, slope);
        }

        // The lock's nudge stays inside what the voices may add to their steady slope.
        const double fe = std::min (lfoRate[0], rateCap[0]);
        const double swingSlope = (double) law.slope * (double) swing[0] * invSampleRate;
        lockMax = std::min (kLockMaxRatio * fe, 0.9 * (double) kPitchAllowance / std::max (swingSlope, 1.0e-9)) * invSampleRate;

        sinceMove = std::min (sinceMove + kControlInterval, 1 << 30);
        sinceFade = std::min (sinceFade + kControlInterval, 1 << 30);
    }

    /** Moves each swing towards its target no faster than keeps every voice inside
        hold + kPitchAllowance: the LFO's own term c f D is held at hold by capping the LFO
        rate while a swing is too large for it, and the swing's term D' u gets the rest.
        Both groups move the same fraction of their gaps, at the slower one's pace, so a
        glide passes through the settings in between (Wide's pairs gliding apart combed a
        plucked note 1.6 dB below every steady setting). A transition that would take too
        long is handed to a tap crossfade. */
    void glide (int mode, GroupLaw law, const float target[2], const float lfoRate[2], const float slope[2]) noexcept
    {
        bool settled = ! clamped;
        clamped = false;
        float remaining = 0.0f;
        float multiplier[2], gaps[2] { 0.0f, 0.0f };
        float fraction = 1.0f;
        multipliers (mode, multiplier);

        for (int k = 0; k < 2; ++k)
        {
            hold[k] = std::max ({ hold[k], slope[k], rawSlope[k] });

            const float want = target[k] * sampleRate;
            const float now = swing[k];
            const float nowSeconds = now / sampleRate;
            rateCap[k] = now > want + 1.0e-3f ? hold[k] / (law.slope * nowSeconds) : kNoCap;

            const float fe = std::min (lfoRate[k], rateCap[k]);
            const float budget = std::max (kPitchAllowance, hold[k] + kPitchAllowance - law.slope * fe * nowSeconds);
            const float reach = juce::jlimit (kMinMultiplier, law.range,
                                              multiplier[k] + law.slope * fe * (float) kControlInterval / sampleRate);
            const float maxStep = budget / reach * (float) kControlInterval;
            const float gap = want - now;

            if (std::abs (gap) < 1.0e-3f)
            {
                swing[k] = want;
                swingStep[k] = 0.0f;
            }
            else
            {
                gaps[k] = gap;
                fraction = std::min (fraction, maxStep / std::abs (gap));

                if (std::abs (gap) > maxStep)
                {
                    settled = false;
                    remaining = std::max (remaining, glideTime (law, hold[k], lfoRate[k], nowSeconds, want / sampleRate) * sampleRate);
                }
            }

            if (rateCap[k] < lfoRate[k])
                settled = false;
        }

        for (int k = 0; k < 2; ++k)
            if (gaps[k] != 0.0f)
                swingStep[k] = fraction * gaps[k] * (1.0f / (float) kControlInterval);

        if (settled)
            for (int k = 0; k < 2; ++k)
                hold[k] = std::max (slope[k], rawSlope[k]);

        setVoiceLimits (mode);

        if (settled)
            return;

        if (wet == Wet::silent)
        {
            snapControl = true;   // nothing is heard: land on the new law at once
            return;
        }

        const bool still = sinceMove >= quietNeeded;

        // A knob that has stopped (twice the quiet time: steps 40 ms apart never do) may finish
        // with one crossfade at once, unless the last was a stopped knob's too.
        const bool stopped = sinceMove >= 2 * quietNeeded;
        const bool spaced = sinceFade >= fadeSpacing || (stopped && ! lastFadeStopped);

        if (xf != XFade::fading && remaining > (still ? glideSamples : catchUpSamples) && (xf != XFade::none || spaced))
        {
            if (xf == XFade::none)
            {
                ++counts.knobFades;
                sinceFade = 0;
                lastFadeStopped = stopped;
            }

            requestJump (false);
        }
    }

    /** Seconds a glide from swing `from` to `to` (seconds) takes at most: the swing's term may
        use what the LFO's own term c s (held at hold while capped) leaves of
        hold + kPitchAllowance, so a growing swing slows down as it arrives. */
    static float glideTime (GroupLaw law, float hold, float lfoRate, float from, float to) noexcept
    {
        constexpr int steps = 8;
        const float ds = (to - from) / (float) steps;
        float t = 0.0f;

        for (int j = 0; j < steps; ++j)
        {
            const float s = from + ((float) j + 0.5f) * ds;
            const float budget = std::max (kPitchAllowance, hold + kPitchAllowance - std::min (law.slope * lfoRate * s, hold));
            t += law.range * std::abs (ds) / budget;
        }

        return t;
    }

    /** Largest magnitude of each group's swing multiplier over its voices. */
    void multipliers (int mode, float out[2]) const noexcept
    {
        out[0] = out[1] = 0.0f;

        switch ((ChorusFx::Mode) mode)
        {
            case ChorusFx::Mode::wide:
                out[0] = std::max (shape[0], shape[1]);
                out[1] = std::max (shape[2], shape[3]);
                break;
            case ChorusFx::Mode::ce:
                out[0] = std::max (std::abs (shape[0] - 0.5f), std::abs (shape[1] - 0.5f));
                break;
            case ChorusFx::Mode::dimension:
                out[0] = std::abs (2.0f * shape[0] - 1.0f);
                break;
            case ChorusFx::Mode::tri:
                out[0] = std::max ({ shape[0], shape[1], shape[2] });
                out[1] = std::max ({ shape[3], shape[4], shape[5] });
                break;
            case ChorusFx::Mode::classic:
            default:
                break;
        }
    }

    void setVoiceLimits (int mode) noexcept
    {
        const float g0 = hold[0] + kPitchAllowance, g1 = hold[1] + kPitchAllowance;

        switch ((ChorusFx::Mode) mode)
        {
            case ChorusFx::Mode::wide:
                voiceLimit[0] = voiceLimit[1] = g0;
                voiceLimit[2] = voiceLimit[3] = g1;
                break;
            case ChorusFx::Mode::tri:
                voiceLimit[0] = voiceLimit[1] = voiceLimit[2] = voiceLimit[3] = g0 + hold[1];
                break;
            case ChorusFx::Mode::ce:
            case ChorusFx::Mode::dimension:
            case ChorusFx::Mode::classic:
            default:
                std::fill (std::begin (voiceLimit), std::end (voiceLimit), g0);
                break;
        }
    }

    /** A change too large to glide (sync: a song-position jump). Unheard, the voices land
        at once; otherwise the outgoing taps carry on along their last slope and the voices
        land on the new law at the next control tick, silent until the crossfade. A request
        while the new taps are still silent moves them again; one during the fade waits. */
    void requestJump (bool sync) noexcept
    {
        syncLand |= sync;

        if (wet == Wet::silent)
        {
            land();
            return;
        }

        switch (xf)
        {
            case XFade::none:
                for (int v = 0; v < 4; ++v)
                {
                    oldDelay[v] = delay[v];
                    oldSlope[v] = step[v];
                }

                xf = XFade::landing;
                xfSamples = 0;
                break;
            case XFade::landing:
                break;
            case XFade::measuring:
                if (xfSamples < tapPreroll)
                {
                    std::copy (std::begin (oldStep), std::end (oldStep), std::begin (oldSlope));
                    xf = XFade::landing;
                }
                else
                {
                    jumpPending = true;
                }
                break;
            case XFade::fading:
                jumpPending = true;
                break;
        }
    }

    /** Every voice jumps to the law the settings and the song position ask for. */
    void land() noexcept
    {
        rate.snap();
        depth.snap();

        if (syncLand)
        {
            // Where the song is now: the host's last phase, moved on at its rate.
            phase[0] = hostPhase + hostRate * (double) sinceHost * invSampleRate;
            phase[0] -= std::floor (phase[0]);
            lockError = 0.0;
            syncLand = false;
        }

        snapControl = true;
    }

    /** A song-position jump that arrived during the fade is made now, if the LFO is still
        further off the song than the lock pulls in. */
    void endCrossfade() noexcept
    {
        xf = XFade::none;

        if (! jumpPending)
            return;

        jumpPending = false;

        if (! (syncLand && hostValid))
            return;

        syncLand = false;
        const double error = wrapHalf (hostPhase + hostRate * (double) sinceHost * invSampleRate - phase[0]);

        if (std::abs (error) > std::max (kLockSlackCycles, kLockSlackSeconds * (double) rate.target))
            requestJump (true);
        else
            lockError = error;
    }

    /** One sample of the tap crossfade: every voice from its outgoing tap to its new one,
        directly or through the tap half way between, whichever path's two ends are the more
        alike for that voice (a tone's taps half a period apart cancel; the midpoint tap is a
        quarter period from each). Each leg is s (cos * from + sin * to), s keeping the
        voice's short-term power on the straight line between its old and new taps'. Decided
        and corrected per voice, not on the sum: one channel's taps can meet in antiphase
        while the other's add, and a shared path and gain then leave a hole in that channel
        (CE on a loop wrap: -32 dB). Where voices share a channel with each other and the dry
        (Tri at mix 0.5), no path choice holds every note: the best dips 4 - 6 dB at 1 kHz.
        `now` returns what each voice plays. */
    template <int N>
    void crossfade (float (&now)[N], const float (&old)[N], const float (&mid)[N]) noexcept
    {
        if (xf == XFade::landing)
        {
            std::copy (std::begin (old), std::end (old), std::begin (now));
            return;
        }

        ++xfSamples;

        for (int v = 0; v < N; ++v)
        {
            const float raw[6] { old[v] * old[v], now[v] * now[v], mid[v] * mid[v],
                                 old[v] * now[v], old[v] * mid[v], mid[v] * now[v] };
            float* p0 = tapPower[0][v];
            float* p1 = tapPower[1][v];

            for (int k = 0; k < 6; ++k)
            {
                p0[k] += tapPowerA * (raw[k] - p0[k]);
                p1[k] += tapPowerA * (p0[k] - p1[k]);
            }
        }

        if (xf == XFade::measuring)
        {
            if (xfSamples >= tapMeasure)
            {
                const auto alike = [] (float cross, float a, float b) { return cross / std::sqrt (a * b + 1.0e-30f); };

                for (int v = 0; v < N; ++v)
                {
                    const float* p = tapPower[1][v];
                    viaMid[v] = std::min (alike (p[4], p[0], p[2]), alike (p[5], p[2], p[1])) > alike (p[3], p[0], p[1]);
                }

                xf = XFade::fading;
                xfPos = 0.0f;
            }

            std::copy (std::begin (old), std::end (old), std::begin (now));
            return;
        }

        xfPos = std::min (1.0f, xfPos + tapStep);

        // Smoothstep first, so no gain starts or stops moving with a corner.
        const float total = xfPos * xfPos * (3.0f - 2.0f * xfPos);
        const bool firstLeg = xfPos < 0.5f;
        const float u = firstLeg ? 2.0f * xfPos : 2.0f * xfPos - 1.0f;
        float gToDirect, gFromDirect, gToLeg, gFromLeg;
        sinCosTurns (0.25f * total, gToDirect, gFromDirect);
        sinCosTurns (0.25f * u * u * (3.0f - 2.0f * u), gToLeg, gFromLeg);

        for (int v = 0; v < N; ++v)
        {
            const float* p = tapPower[1][v];
            float from = old[v], to = now[v], pFrom = p[0], pTo = p[1], cross = p[3];
            float gFrom = gFromDirect, gTo = gToDirect;

            if (viaMid[v])
            {
                gFrom = gFromLeg;
                gTo = gToLeg;

                if (firstLeg)
                {
                    to = mid[v];
                    pTo = p[2];
                    cross = p[4];
                }
                else
                {
                    from = mid[v];
                    pFrom = p[2];
                    cross = p[5];
                }
            }

            const float want = p[0] + total * (p[1] - p[0]);
            const float got = gFrom * gFrom * pFrom + gTo * gTo * pTo + 2.0f * gFrom * gTo * cross;
            const float slack = kTapSlack * want + 1.0e-12f;
            now[v] = std::sqrt ((want + slack) / (std::max (got, 0.0f) + slack)) * (gFrom * from + gTo * to);
        }

        if (xfPos >= 1.0f)
            endCrossfade();
    }

    void startFade (float stepSize) noexcept
    {
        wet = Wet::fading;
        wetStep = stepSize;
    }

    /** The wet's fade-in after reset(), once the lines hold enough history. */
    float wetGain() noexcept
    {
        if (wet == Wet::steady)
            return 1.0f;

        if (wet == Wet::silent)
            return 0.0f;

        wetPos += wetStep;

        if (wetPos >= 1.0f)
        {
            wetPos = 1.0f;
            wet = Wet::steady;
            return 1.0f;
        }

        return wetPos * wetPos * (3.0f - 2.0f * wetPos);
    }

    /** The delay voice v reads: `target`, unless getting there this sample would bend the
        pitch by more than `limit` (samples per sample). */
    float follow (int v, float target, float limit) noexcept
    {
        const float gap = target - delay[v];

        if (std::abs (gap) <= limit)
        {
            step[v] = gap;
            delay[v] = target;
        }
        else
        {
            step[v] = gap > 0.0f ? limit : -limit;
            delay[v] += step[v];
            clamped = true;
        }

        return delay[v];
    }

    /** Reads every voice on its law and, during a crossfade, its outgoing tap and the tap
        half way between the two. The outgoing tap's slope moves from its own last one to the
        new tap's over the first half of the measurement, so the two then run a fixed
        distance apart: their comb and correlation hold still while they are measured and
        faded, and every tap's slope stays a mix of slopes inside the bound. */
    template <int N>
    void readVoices (const float (&target)[N], const int (&line)[N], float (&now)[N], float (&old)[N], float (&mid)[N]) noexcept
    {
        for (int v = 0; v < N; ++v)
            now[v] = lines[line[v]].readHermite (follow (v, target[v], voiceLimit[v]));

        if (xf == XFade::none)
            return;

        // At the first measured sample the new taps jump onto their law: w is still 0 there.
        const float w = xf == XFade::landing ? 0.0f
                                             : (xf == XFade::fading ? 1.0f : std::min (1.0f, (float) xfSamples * slopeBlend));

        for (int v = 0; v < N; ++v)
        {
            oldStep[v] = oldSlope[v] + w * (step[v] - oldSlope[v]);
            oldDelay[v] = juce::jlimit (1.0f, oldMax, oldDelay[v] + oldStep[v]);
            old[v] = lines[line[v]].readHermite (oldDelay[v]);
            mid[v] = lines[line[v]].readHermite (0.5f * (oldDelay[v] + delay[v]));
        }
    }

    /** Every voice's tap, or taps while a crossfade runs: -> what each voice plays, in `now`. */
    template <int N>
    void readHeard (const float (&target)[N], const int (&line)[N], float (&now)[N]) noexcept
    {
        float old[N], mid[N];
        readVoices (target, line, now, old, mid);

        if (xf != XFade::none)
            crossfade (now, old, mid);
    }

    void writeTrace (int i, int voice) const noexcept
    {
        if (traceOut == nullptr)
            return;

        traceOut[0][i] = delay[voice];
        traceOut[1][i] = xf == XFade::none ? delay[voice] : oldDelay[voice];
        traceOut[2][i] = xf == XFade::none ? 0.0f : (xf != XFade::fading ? 1.0f : (viaMid[voice] ? 3.0f : 2.0f));
    }

    /** Starts every rounding one-pole on its input, so nothing glides in after a reset. */
    void primeShapes (int mode) noexcept
    {
        const float p0 = (float) phase[0] + lead[0];
        const float p1 = (float) phase[1] + lead[1];

        switch ((ChorusFx::Mode) mode)
        {
            case ChorusFx::Mode::wide:
                shape[0] = triangle (p0);
                shape[1] = triangle (p0 + kWideOffsetR);
                shape[2] = triangle (p1 + kWidePair2Offset);
                shape[3] = triangle (p1 + kWidePair2Offset + kWideOffsetR);
                break;
            case ChorusFx::Mode::ce:
                shape[0] = bentTriangle (p0);
                shape[1] = bentTriangle (p0 + kCeOffsetR);
                break;
            case ChorusFx::Mode::dimension:
                shape[0] = triangle (p0);
                break;
            case ChorusFx::Mode::tri:
            {
                float s, c;
                sinCosTurns (wrap01 (p0), s, c);
                shape[0] = 0.5f - 0.5f * c;
                shape[1] = 0.5f - 0.5f * (kCos120 * c - kSin120 * s);
                shape[2] = 0.5f - 0.5f * (kCos120 * c + kSin120 * s);
                sinCosTurns (wrap01 (p1), s, c);
                shape[3] = 0.5f - 0.5f * c;
                shape[4] = 0.5f - 0.5f * (kCos120 * c - kSin120 * s);
                shape[5] = 0.5f - 0.5f * (kCos120 * c + kSin120 * s);
                break;
            }
            case ChorusFx::Mode::classic:
            default:
                break;
        }
    }

    /** Splits a block at the control grid, which keeps its place across process() calls,
        so the output does not depend on the block size. `body (start, length)` runs the
        samples of one interval. */
    template <typename Body>
    void forEachInterval (int numSamples, int mode, Body&& body) noexcept
    {
        int i = 0;

        while (i < numSamples)
        {
            if (tick == 0)
                control (mode);

            const int len = std::min (numSamples - i, kControlInterval - tick);
            body (i, len);
            i += len;
            tick = (tick + len) & (kControlInterval - 1);
            sinceHost = std::min (sinceHost + len, 1 << 30);
            sinceJump = std::min (sinceJump + len, 1 << 30);

            if (refilling)
            {
                filled += len;

                // Decided on the control grid, so the fade-in does not depend on the block size.
                if (tick == 0 && filled >= reachSamples (mode, sampleRate))
                {
                    refilling = false;

                    if (autoFadeIn)
                        startFade (fadeInStep);
                }
            }
        }
    }

    /** dry = min (1, 2 - 2m), wet = min (1, 2m) [choice: §4.5.4, dry at unity to the midpoint];
        width scales the wet side signal. */
    struct Output
    {
        float dry, wet, width;

        Output (Smoothed& mixS, Smoothed& widthS, float a) noexcept
        {
            const float m = mixS.next (a);
            dry = std::min (1.0f, 2.0f - 2.0f * m);
            wet = std::min (1.0f, 2.0f * m);
            width = widthS.next (a);
        }

        void write (float* outL, float* outR, int i, float xL, float xR, float wL, float wR, bool stereo) const noexcept
        {
            const float mid = 0.5f * (wL + wR);

            if (stereo)
            {
                const float side = 0.5f * width * (wL - wR);
                outL[i] = dry * xL + wet * (mid + side);
                outR[i] = dry * xR + wet * (mid - side);
            }
            else
            {
                outL[i] = dry * xL + wet * mid;
            }
        }
    };

    static void writeSolo (float* outL, float* outR, int i, float v, bool stereo) noexcept
    {
        outL[i] = v;

        if (stereo)
            outR[i] = v;
    }

    template <bool solo>
    void runWide (juce::dsp::AudioBlock<float>& block, bool stereo, int soloVoice) noexcept
    {
        float* outL = block.getChannelPointer (0);
        float* outR = stereo ? block.getChannelPointer (1) : outL;
        const float base0 = kWideBaseSeconds[0] * sampleRate;
        const float base1 = kWideBaseSeconds[1] * sampleRate;
        const int mode = (int) ChorusFx::Mode::wide;
        static constexpr int lineOf[4] { 0, 1, 0, 1 };

        forEachInterval ((int) block.getNumSamples(), mode, [&] (int start, int len)
        {
            const float ra0 = roundA[0], ra1 = roundA[1];
            const float lead0 = lead[0], lead1 = lead[1];
            const float cap0 = rateCap[0], cap1 = rateCap[1];

            for (int i = start; i < start + len; ++i)
            {
                const float f = rate.next (smoothA);
                depth.next (smoothA);
                tone.next (smoothA);
                const Output out (mix, width, smoothA);

                const float xL = outL[i];
                const float xR = stereo ? outR[i] : xL;
                const float fl = fx::sanitise (filters[0].process (xL, coefs));
                lines[0].write (fl);
                lines[1].write (stereo ? fx::sanitise (filters[1].process (xR, coefs)) : fl);

                const float p0 = (float) phase[0] + lead0;
                const float p1 = (float) phase[1] + lead1;
                shape[0] += ra0 * (triangle (p0) - shape[0]);
                shape[1] += ra0 * (triangle (p0 + kWideOffsetR) - shape[1]);
                shape[2] += ra1 * (triangle (p1 + kWidePair2Offset) - shape[2]);
                shape[3] += ra1 * (triangle (p1 + kWidePair2Offset + kWideOffsetR) - shape[3]);

                const float target[4] { base0 + swing[0] * shape[0], base0 + swing[0] * shape[1],
                                        base1 + swing[1] * shape[2], base1 + swing[1] * shape[3] };
                float v[4];
                readHeard (target, lineOf, v);
                swing[0] += swingStep[0];
                swing[1] += swingStep[1];
                advance (std::min (f, cap0), std::min (f * kWidePair2Ratio, cap1));

                if constexpr (solo)
                {
                    writeTrace (i, soloVoice);
                    writeSolo (outL, outR, i, v[soloVoice] * wetGain(), stereo);
                }
                else
                {
                    const float wl = (v[0] + kWidePair2Gain * v[2]) * kWideNorm;
                    const float wr = (v[1] + kWidePair2Gain * v[3]) * kWideNorm;
                    const float g = wetGain();
                    out.write (outL, outR, i, xL, xR, wl * g, wr * g, stereo);
                }
            }
        });
    }

    template <bool solo>
    void runCe (juce::dsp::AudioBlock<float>& block, bool stereo, int soloVoice) noexcept
    {
        float* outL = block.getChannelPointer (0);
        float* outR = stereo ? block.getChannelPointer (1) : outL;
        const float base = kCeBaseSeconds * sampleRate;
        const int mode = (int) ChorusFx::Mode::ce;
        static constexpr int lineOf[2] { 0, 1 };

        forEachInterval ((int) block.getNumSamples(), mode, [&] (int start, int len)
        {
            const float ra = roundA[0], lead0 = lead[0], cap = rateCap[0];

            for (int i = start; i < start + len; ++i)
            {
                const float f = rate.next (smoothA);
                depth.next (smoothA);
                tone.next (smoothA);
                const Output out (mix, width, smoothA);

                const float xL = outL[i];
                const float xR = stereo ? outR[i] : xL;
                const float fl = fx::sanitise (filters[0].processCe (xL, coefs));
                lines[0].write (fl);
                lines[1].write (stereo ? fx::sanitise (filters[1].processCe (xR, coefs)) : fl);

                const float p = (float) phase[0] + lead0;
                shape[0] += ra * (bentTriangle (p) - shape[0]);
                shape[1] += ra * (bentTriangle (p + kCeOffsetR) - shape[1]);

                const float target[2] { base + swing[0] * (shape[0] - 0.5f), base + swing[0] * (shape[1] - 0.5f) };
                float v[2];
                readHeard (target, lineOf, v);
                swing[0] += swingStep[0];
                const float fe = std::min (f, cap);
                advance (fe, fe);

                if constexpr (solo)
                {
                    writeTrace (i, soloVoice);
                    writeSolo (outL, outR, i, v[soloVoice] * wetGain(), stereo);
                }
                else
                {
                    const float g = wetGain();
                    out.write (outL, outR, i, xL, xR, v[0] * g, v[1] * g, stereo);
                }
            }
        });
    }

    template <bool solo>
    void runDimension (juce::dsp::AudioBlock<float>& block, bool stereo, int soloVoice) noexcept
    {
        float* outL = block.getChannelPointer (0);
        float* outR = stereo ? block.getChannelPointer (1) : outL;
        const float base = kDimBaseSeconds * sampleRate;
        const int mode = (int) ChorusFx::Mode::dimension;
        static constexpr int lineOf[2] { 0, 0 };

        forEachInterval ((int) block.getNumSamples(), mode, [&] (int start, int len)
        {
            const float ra = roundA[0], lead0 = lead[0], cap = rateCap[0];

            for (int i = start; i < start + len; ++i)
            {
                const float f = rate.next (smoothA);
                depth.next (smoothA);
                tone.next (smoothA);
                const Output out (mix, width, smoothA);

                const float xL = outL[i];
                const float xR = stereo ? outR[i] : xL;
                lines[0].write (fx::sanitise (filters[0].process (0.5f * (xL + xR), coefs)));

                shape[0] += ra * (triangle ((float) phase[0] + lead0) - shape[0]);
                // Line B runs the inverted triangle (u_B = 1 - u_A).
                const float sweep = swing[0] * (2.0f * shape[0] - 1.0f);
                const float target[2] { base + sweep, base - sweep };
                float v[2];
                readHeard (target, lineOf, v);
                swing[0] += swingStep[0];
                const float fe = std::min (f, cap);
                advance (fe, fe);

                if constexpr (solo)
                {
                    writeTrace (i, soloVoice);
                    writeSolo (outL, outR, i, v[soloVoice] * wetGain(), stereo);
                }
                else
                {
                    // Mono: line A alone. Summed, the antiphase lines cancel each other's
                    // detune and leave only a comb.
                    const float g = wetGain();
                    out.write (outL, outR, i, xL, xR, v[0] * g, (stereo ? v[1] : v[0]) * g, stereo);
                }
            }
        });
    }

    template <bool solo>
    void runTri (juce::dsp::AudioBlock<float>& block, bool stereo, int soloVoice) noexcept
    {
        float* outL = block.getChannelPointer (0);
        float* outR = stereo ? block.getChannelPointer (1) : outL;
        const float base = kTriBaseSeconds * sampleRate;
        const int mode = (int) ChorusFx::Mode::tri;
        static constexpr int lineOf[3] { 0, 0, 0 };

        forEachInterval ((int) block.getNumSamples(), mode, [&] (int start, int len)
        {
            const float ra0 = roundA[0], ra1 = roundA[1];
            const float lead0 = lead[0], lead1 = lead[1];
            const float cap0 = rateCap[0];

            for (int i = start; i < start + len; ++i)
            {
                const float f = rate.next (smoothA);
                depth.next (smoothA);
                tone.next (smoothA);
                const Output out (mix, width, smoothA);

                const float xL = outL[i];
                const float xR = stereo ? outR[i] : xL;
                lines[0].write (fx::sanitise (filters[0].process (0.5f * (xL + xR), coefs)));

                float s, c;
                sinCosTurns (wrap01 ((float) phase[0] + lead0), s, c);
                shape[0] += ra0 * ((0.5f - 0.5f * c) - shape[0]);
                shape[1] += ra0 * ((0.5f - 0.5f * (kCos120 * c - kSin120 * s)) - shape[1]);
                shape[2] += ra0 * ((0.5f - 0.5f * (kCos120 * c + kSin120 * s)) - shape[2]);
                sinCosTurns (wrap01 ((float) phase[1] + lead1), s, c);
                shape[3] += ra1 * ((0.5f - 0.5f * c) - shape[3]);
                shape[4] += ra1 * ((0.5f - 0.5f * (kCos120 * c - kSin120 * s)) - shape[4]);
                shape[5] += ra1 * ((0.5f - 0.5f * (kCos120 * c + kSin120 * s)) - shape[5]);

                const float target[3] { base + swing[0] * shape[0] + swing[1] * shape[3],
                                        base + swing[0] * shape[1] + swing[1] * shape[4],
                                        base + swing[0] * shape[2] + swing[1] * shape[5] };
                float v[3];
                readHeard (target, lineOf, v);
                swing[0] += swingStep[0];
                swing[1] += swingStep[1];
                advance (std::min (f, cap0), kTriFastHz);

                if constexpr (solo)
                {
                    writeTrace (i, soloVoice);
                    writeSolo (outL, outR, i, v[soloVoice] * wetGain(), stereo);
                }
                else
                {
                    const float g = wetGain();
                    out.write (outL, outR, i, xL, xR, kTriPairGain * (v[0] + v[1]) * g, kTriPairGain * (v[1] + v[2]) * g, stereo);
                }
            }
        });
    }

    /** rate0 drives the master LFO, which the lock nudges towards the song. */
    void advance (float rate0, float rate1) noexcept
    {
        double step0 = (double) rate0 * invSampleRate;

        if (lockError != 0.0)
        {
            const double nudge = juce::jlimit (-lockMax, lockMax, lockError * lockA);
            lockError -= nudge;
            step0 += nudge;
        }

        phase[0] += step0;
        phase[1] += (double) rate1 * invSampleRate;

        if (phase[0] >= 1.0)
            phase[0] -= 1.0;
        else if (phase[0] < 0.0)
            phase[0] += 1.0;

        if (phase[1] >= 1.0)
            phase[1] -= 1.0;
    }

    fx::DelayBuffer lines[2];
    WetFilter filters[2];
    WetCoefficients coefs;
    Smoothed rate, depth, mix, width, tone;

    // phase[0] is the master LFO (the one syncPhase sets); phase[1] is Wide's second pair
    // or Tri's 6 Hz shimmer.
    double phase[2] {};
    float shape[6] {};          // rounded LFO value per voice (Tri: 3 slow, then 3 fast)
    float swing[2] {}, swingStep[2] {};   // samples
    float roundA[2] { 1.0f, 1.0f }, lead[2] {};

    // Transitions. Slopes are delay seconds per second; hold is the largest peak slope of
    // the settings involved since the voices last sat on their law.
    float hold[2] {}, rawSlope[2] {};
    float rateCap[2] { kNoCap, kNoCap };      // Hz, while a swing is too large for the rate
    float delay[4] {}, step[4] {}, voiceLimit[4] {};   // samples; samples per sample
    int sinceMove = 0, sinceFade = 0, quietSamples = 1, quietNeeded = 1, fadeSpacing = 1;
    float glideSamples = 1.0f, catchUpSamples = 1.0f;
    bool clamped = false, rawDirty = true, lastFadeStopped = false;

    // Tap crossfade: the outgoing taps read oldDelay, which moves by oldSlope per sample.
    XFade xf = XFade::none;
    float oldDelay[4] {}, oldSlope[4] {}, oldStep[4] {}, oldMax = 1.0f, slopeBlend = 1.0f;
    float tapPower[2][4][6] {};   // two smoothing stages, per voice: old, new, mid tap powers; old.new, old.mid, mid.new
    float xfPos = 0.0f, tapStep = 1.0f, tapPowerA = 1.0f;
    int xfSamples = 0, tapMeasure = 1, tapPreroll = 1;
    bool jumpPending = false, syncLand = false, viaMid[4] {};

    // Song-position lock: the host's phase at its last block start, sinceHost samples ago,
    // moving at hostRate; lockError is the part of the LFO's error not yet pulled in.
    double hostPhase = 0.0, hostRate = 0.0, lockError = 0.0, lockMax = 0.0, lockA = 0.0;
    int sinceHost = 0, sinceJump = 1 << 30, jumpTrain = 1;
    bool hostValid = false;

    Wet wet = Wet::silent;
    float wetPos = 0.0f, wetStep = 0.0f, fadeInStep = 1.0f;
    bool refilling = true;
    int filled = 0;

    float* const* traceOut = nullptr;
    float sampleRate = 48000.0f, smoothA = 1.0f;
    double invSampleRate = 1.0 / 48000.0;
    int tick = 0;
    bool snapControl = true;
};

float finiteOr (float value, float fallback) noexcept
{
    return std::isfinite (value) ? value : fallback;
}
} // namespace

//==============================================================================
struct ChorusFx::Impl
{
    juce::dsp::Chorus<float> classic;
    ModernChorus modern[2];
    // During a switch: the block's input and the incoming mode's output.
    juce::AudioBuffer<float> input, scratch;
    ChorusParams params;
    double sampleRate = 48000.0;
    int maxBlock = 512, numChannels = 2, solo = -1;
    int active = (int) Mode::wide, slot = 0;   // the mode heard, and its instance if modern
    int incoming = -1, incomingSlot = 1;
    int classicFilled = 0;
    float blend = 0.0f, switchStep = 0.0f, powerA = 1.0f, splitA = 1.0f;
    // Per channel: two smoothing stages of the outgoing and incoming wets' powers and product,
    // whole and per band (below and above kSwitchSplitHz); the bands' low-pass states; the
    // correlation lagged by one more stage (its trend).
    float power[2][2][3] {}, bandPower[2][2][2][3] {}, lowOut[2] {}, lowIn[2] {}, correlationLag[2] {};
    int measureSamples = 0, measured = 0, waited = 0, maxWait = 0, longestWait = 0;
    bool fadeStarted = false;
    enum class Stage { idle, filling, fading } stage = Stage::idle;
    bool snapEngine = true, snapParams = true;
    float* const* trace = nullptr;
    float* tracePiece[3] {};

    static bool isModern (int mode) noexcept { return mode != (int) Mode::classic; }

    /** A mode switch keeps the outgoing mode sounding while the incoming one fills its line
        from the live input, then crossfades the two whole outputs by measured power: what is
        heard is what holds, the dry's interference with each wet included (fading the wets
        alone held their power and still left Tri -> Wide at mix 0.5 -9.4 dB on a held note). */
    void begin (int mode)
    {
        incoming = mode;
        stage = Stage::filling;

        if (! isModern (mode))
        {
            classic.reset();
            classicFilled = 0;
            return;
        }

        incomingSlot = isModern (active) ? 1 - slot : 0;
        auto& next = modern[incomingSlot];
        next.clear();
        next.setTargets (params);
        next.snapSmoothers();
        next.autoFadeIn = false;

        if (isModern (active))
            next.continueFrom (modern[slot]);
    }

    bool incomingReady() const noexcept
    {
        return isModern (incoming) ? modern[incomingSlot].ready()
                                   : classicFilled >= ModernChorus::reachSamples (incoming, sampleRate);
    }

    void startBlend() noexcept
    {
        stage = Stage::fading;
        blend = 0.0f;
        measured = waited = 0;
        fadeStarted = false;
        std::fill (&power[0][0][0], &power[0][0][0] + 12, 0.0f);
        std::fill (&bandPower[0][0][0][0], &bandPower[0][0][0][0] + 24, 0.0f);
        std::fill (std::begin (lowOut), std::end (lowOut), 0.0f);
        std::fill (std::begin (lowIn), std::end (lowIn), 0.0f);
        std::fill (std::begin (correlationLag), std::end (correlationLag), 1.0f);

        if (isModern (incoming))
            modern[incomingSlot].setWetSteady();
    }

    void finish() noexcept
    {
        if (isModern (incoming))
            slot = incomingSlot;

        active = incoming;
        abandon();
    }

    void abandon() noexcept
    {
        incoming = -1;
        stage = Stage::idle;
    }

    /** firstPiece: this sub-block starts the host's block, which syncPhase belongs to. */
    void runEngine (int mode, int engineSlot, juce::dsp::AudioBlock<float> block, bool firstPiece, float* const* traceTo)
    {
        if (! isModern (mode))
        {
            // FxBlocks.cpp Modulation::process, chorus branch (§4.5.3).
            juce::dsp::ProcessContextReplacing<float> context (block);
            classic.process (context);
            return;
        }

        auto& engine = modern[engineSlot];

        if (firstPiece)
        {
            if (params.syncPhase >= 0.0)
                engine.syncTo (params.syncPhase);
            else
                engine.unlock();
        }

        engine.process (block, mode, solo, traceTo);
    }

    /** block = s (cos * block + sin * scratch), the outgoing and incoming outputs, per channel
        and band, s keeping the short-term power of each on the straight line between the two's,
        once a moment comes where they will not cancel through the fade (kSwitchMinCorrelation). */
    void crossfadeOutputs (juce::dsp::AudioBlock<float>& block, size_t channels, size_t n) noexcept
    {
        const int numCh = (int) std::min<size_t> (channels, 2);
        const float fadeLength = 1.0f / switchStep;

        for (size_t i = 0; i < n; ++i)
        {
            float x[2] {}, y[2] {}, xl[2] {}, yl[2] {};
            bool good = true, heading = true;

            for (int ch = 0; ch < numCh; ++ch)
            {
                x[ch] = block.getChannelPointer ((size_t) ch)[i];
                y[ch] = scratch.getReadPointer (ch)[i];
                lowOut[ch] += splitA * (x[ch] - lowOut[ch]);
                lowIn[ch] += splitA * (y[ch] - lowIn[ch]);
                xl[ch] = lowOut[ch];
                yl[ch] = lowIn[ch];
                const float xh = x[ch] - xl[ch], yh = y[ch] - yl[ch];
                const float raw[3][3] { { x[ch] * x[ch], y[ch] * y[ch], x[ch] * y[ch] },
                                        { xl[ch] * xl[ch], yl[ch] * yl[ch], xl[ch] * yl[ch] },
                                        { xh * xh, yh * yh, xh * yh } };

                for (int k = 0; k < 3; ++k)
                {
                    power[0][ch][k] += powerA * (raw[0][k] - power[0][ch][k]);
                    power[1][ch][k] += powerA * (power[0][ch][k] - power[1][ch][k]);

                    for (int band = 0; band < 2; ++band)
                    {
                        float& p0 = bandPower[0][ch][band][k];
                        float& p1 = bandPower[1][ch][band][k];
                        p0 += powerA * (raw[band + 1][k] - p0);
                        p1 += powerA * (p0 - p1);
                    }
                }

                const float* p = power[1][ch];
                const float both = std::sqrt (p[0] * p[1]);
                const float correlation = both < 1.0e-12f ? 1.0f : p[2] / both;
                const float trend = correlation - correlationLag[ch];
                correlationLag[ch] += powerA * trend;
                good &= correlation >= kSwitchMinCorrelation;
                heading &= trend >= 0.0f || correlation + trend * powerA * fadeLength >= 0.0f;
            }

            if (measured < measureSamples)
                ++measured;
            else if (! fadeStarted)
                fadeStarted = (good && (heading || waited >= maxWait)) || waited >= longestWait;

            if (! fadeStarted)
            {
                waited += measured >= measureSamples ? 1 : 0;
                continue;
            }

            blend = std::min (1.0f, blend + switchStep);
            // Smoothstep first, so neither gain starts or stops moving with a corner.
            const float r = blend * blend * (3.0f - 2.0f * blend);
            float gIn, gOut;
            sinCosTurns (0.25f * r, gIn, gOut);

            for (int ch = 0; ch < numCh; ++ch)
            {
                float scale[2];

                for (int band = 0; band < 2; ++band)
                {
                    const float* p = bandPower[1][ch][band];
                    const float want = p[0] + r * (p[1] - p[0]);
                    const float got = gOut * gOut * p[0] + gIn * gIn * p[1] + 2.0f * gOut * gIn * p[2];
                    const float slack = kSwitchSlack * want + 1.0e-12f;
                    scale[band] = std::sqrt ((want + slack) / (std::max (got, 0.0f) + slack));
                }

                const float low = gOut * xl[ch] + gIn * yl[ch];
                const float all = gOut * x[ch] + gIn * y[ch];
                block.getChannelPointer ((size_t) ch)[i] = scale[0] * low + scale[1] * (all - low);
            }
        }
    }

    void run (juce::dsp::AudioBlock<float> block, bool firstPiece, size_t offset)
    {
        const int requested = params.mode;
        const auto n = block.getNumSamples();
        const auto channels = block.getNumChannels();
        float* const* traceTo = nullptr;

        if (trace != nullptr)
        {
            for (int k = 0; k < 3; ++k)
                tracePiece[k] = trace[k] + offset;

            traceTo = tracePiece;
        }

        if ((int) channels > numChannels)
        {
            // More channels than prepare() promised: no room to run two modes, so a switch
            // completes at once.
            abandon();

            if (requested != active)
            {
                active = requested;
                slot = 0;
                classic.reset();
                modern[0].clear();
                modern[0].setTargets (params);
                modern[0].snapSmoothers();
            }

            runEngine (active, slot, block, firstPiece, traceTo);
            return;
        }

        if (stage == Stage::idle && requested != active)
            begin (requested);
        else if ((stage == Stage::filling || (stage == Stage::fading && ! fadeStarted)) && requested != incoming)
            requested == active ? abandon() : begin (requested);

        if (stage == Stage::filling && incomingReady())
            startBlend();

        if (stage == Stage::idle)
        {
            runEngine (active, slot, block, firstPiece, traceTo);
            return;
        }

        for (size_t ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::copy (input.getWritePointer ((int) ch), block.getChannelPointer (ch), (int) n);

        runEngine (active, slot, block, firstPiece, traceTo);

        for (size_t ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::copy (scratch.getWritePointer ((int) ch), input.getReadPointer ((int) ch), (int) n);

        juce::dsp::AudioBlock<float> other (scratch.getArrayOfWritePointers(), channels, n);
        runEngine (incoming, incomingSlot, other, firstPiece, nullptr);

        if (stage == Stage::filling)
        {
            if (! isModern (incoming))
                classicFilled += (int) n;

            return;
        }

        crossfadeOutputs (block, channels, n);

        if (blend >= 1.0f)
            finish();
    }
};

ChorusFx::ChorusFx() : impl (std::make_unique<Impl>()) {}
ChorusFx::~ChorusFx() = default;

void ChorusFx::prepare (const juce::dsp::ProcessSpec& spec)
{
    auto& s = *impl;
    s.sampleRate = spec.sampleRate;
    s.maxBlock = juce::jmax (1, (int) spec.maximumBlockSize);
    s.numChannels = juce::jlimit (1, 2, (int) spec.numChannels);

    // FxBlocks.cpp Modulation::prepare, chorus parts (§4.5.3); reset() follows below.
    s.classic.prepare (spec);
    s.classic.setCentreDelay (7.0f);
    s.classic.setFeedback (0.0f);

    for (auto& m : s.modern)
        m.prepare (spec.sampleRate);

    for (auto* b : { &s.input, &s.scratch })
        b->setSize (s.numChannels, s.maxBlock);

    s.switchStep = 1.0f / juce::jmax (1.0f, kSwitchSeconds * (float) spec.sampleRate);
    s.powerA = 1.0f - std::exp (-1.0f / juce::jmax (1.0f, kSwitchPowerSeconds * (float) spec.sampleRate));
    s.measureSamples = (int) std::ceil (kSwitchMeasureSeconds * spec.sampleRate);
    s.maxWait = (int) std::ceil (kSwitchMaxWaitSeconds * spec.sampleRate);
    s.longestWait = (int) std::ceil (kSwitchLongestWaitSeconds * spec.sampleRate);
    s.splitA = 1.0f - std::exp (-fx::kTwoPi * kSwitchSplitHz / (float) spec.sampleRate);
    reset();
}

void ChorusFx::reset()
{
    auto& s = *impl;
    s.classic.reset();

    for (auto& m : s.modern)
        m.clear();

    s.abandon();
    s.snapEngine = true;
    s.snapParams = true;
}

void ChorusFx::setParameters (const ChorusParams& params)
{
    auto& s = *impl;
    auto& p = s.params;
    p.mode = juce::jlimit (0, numModes - 1, params.mode);

    if (p.mode == (int) Mode::classic)
    {
        // FxBlocks.cpp Modulation::setParameters, chorus branch, verbatim (§4.5.3).
        const float rate = juce::jlimit (0.01f, 100.0f, finiteOr (params.rateHz, 1.0f));
        const float depth = juce::jlimit (0.0f, 1.0f, finiteOr (params.depth, 0.0f));
        const float mix = juce::jlimit (0.0f, 1.0f, finiteOr (params.mix, 0.0f));
        s.classic.setRate (rate);
        s.classic.setDepth (depth);
        s.classic.setMix (mix);
    }

    p.rateHz = juce::jlimit (kMinRateHz, kMaxRateHz, finiteOr (params.rateHz, 1.0f));
    p.depth = juce::jlimit (0.0f, 1.0f, finiteOr (params.depth, 0.0f));
    p.mix = juce::jlimit (0.0f, 1.0f, finiteOr (params.mix, 0.0f));
    p.width = juce::jlimit (0.0f, 1.0f, finiteOr (params.width, 1.0f));
    p.tone = juce::jlimit (0.0f, 1.0f, finiteOr (params.tone, 0.5f));
    p.syncPhase = std::isfinite (params.syncPhase) ? params.syncPhase : -1.0;

    for (auto& m : s.modern)
        m.setTargets (p);

    if (s.snapParams)
    {
        s.snapParams = false;

        for (auto& m : s.modern)
            m.snapSmoothers();
    }
}

void ChorusFx::process (juce::dsp::AudioBlock<float> block)
{
    auto& s = *impl;
    const auto total = block.getNumSamples();

    if (total == 0)
        return;

    // No instrument delivers a sample beyond +-1e6 (+120 dBFS); a non-finite one, or a pair
    // like +-3e38, would stay in Classic's feedback path for good and in a line for its
    // length. Such samples are treated as silence; everything else is untouched.
    constexpr float maxInput = 1.0e6f;

    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
    {
        float* data = block.getChannelPointer (ch);
        bool bad = false;

        for (size_t i = 0; i < total; ++i)
            bad |= ! (std::abs (data[i]) <= maxInput);

        if (bad)
            for (size_t i = 0; i < total; ++i)
                if (! (std::abs (data[i]) <= maxInput))
                    data[i] = 0.0f;
    }

    for (auto& m : s.modern)
        m.setHostBlock ((int) total);

    if (s.snapEngine)
    {
        // Nothing has been heard since prepare() or reset(): start on the requested mode.
        s.snapEngine = false;
        s.active = s.params.mode;
        s.slot = 0;
    }

    // juce::dsp::Chorus and the switch buffers are sized for the prepared block, so a
    // larger block runs in pieces; syncPhase belongs to the first of them.
    const auto step = (size_t) s.maxBlock;

    for (size_t start = 0; start < total; start += step)
        s.run (block.getSubBlock (start, juce::jmin (step, total - start)), start == 0, start);
}

void ChorusFx::setDebugSolo (int voice) noexcept
{
    impl->solo = voice;
}

void ChorusFx::setDebugTrace (float* const* channels) noexcept
{
    impl->trace = channels;
}

ChorusFx::DebugCounts ChorusFx::getDebugCounts() const noexcept
{
    DebugCounts sum;

    for (const auto& m : impl->modern)
    {
        sum.syncJumps += m.counts.syncJumps;
        sum.knobFades += m.counts.knobFades;
    }

    return sum;
}
} // namespace tubamp
