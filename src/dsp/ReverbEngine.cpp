#include "ReverbEngine.h"

#include <algorithm>
#include <cmath>

namespace tubamp
{
namespace
{
//==============================================================================
// Constants. Every number here is either quoted from docs/REVERB.md §3–§5 or
// derived by the law that section names; the comment says which.
//==============================================================================

constexpr float kTwoPi = juce::MathConstants<float>::twoPi;
constexpr float kInvSqrt8 = 0.35355339059327376f;   // §3.2, the normalisation lives here

// §3.2 injection sign pattern (zrev2's).
constexpr float kInjectSign[8] = { 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f };

// §3.1 input diffusers, ms at sizeScale 1.0.
constexpr float kDiffuserMs[4] = { 4.8f, 3.6f, 12.7f, 9.3f };
// §3.7 the two short g = 0.5 allpasses after the Early Energy tap sum.
constexpr float kErApMs[2] = { 6.9f, 11.3f };
// §3.12 cross-tuned density allpasses, [channel][stage].
constexpr float kRevApMs[2][2] = { { 6.9f, 11.3f }, { 7.9f, 10.3f } };

// §3.5 soft clip knee and §6.6 staynormal.
constexpr float kClipThreshold = 0.8f;
constexpr float kStayNormal = 1.0e-20f;

// §3.5 ADAA divided-difference guard. 1e-3, not machine-small: a near-machine divisor
// amplifies the float rounding of the antiderivative difference into broadband error
// (~0.4 % worst case at 1e-5). The residual form keeps the crossover to the midpoint
// fallback benign — F_r is O(u^3) near the knee, so both branches are tiny there.
constexpr float kAdaaEps = 1.0e-3f;

// §3.7 / §3.1 fixed gains.
constexpr float kErToOutput = 0.60f;      // wet = tank + 0.60 * erLevel * er
constexpr float kTankTapGain = 0.35f;     // §3.8, each tap scaled 0.35

// §3.1 / §6.5 front-end constants.
constexpr float kInputDcHz = 18.0f;
constexpr float kOutputDcHz = 18.0f;
constexpr float kTiltPivotHz = 800.0f;    // §3.1, +-6 dB shelving pair
constexpr float kTiltDb = 6.0f;
constexpr float kPredelayJumpSeconds = 0.005f;   // §6.5, the 5 ms dual-law threshold
constexpr float kPredelayXfadeSeconds = 0.030f;  // equal-power crossfade length
// Slew for sub-5 ms jumps: 5 % of a sample per sample, so the 5 ms threshold takes
// 100 ms to traverse — an audible, musical glide rather than a zipper.
constexpr float kPredelaySlewPerSample = 0.05f;
constexpr float kSizeSlewPerSecond = 2.5f;       // §6.5, 250 %/s of the mode's range

// §3.1 ducker: feed-forward only, 20 ms attack / 1200 ms release. kDuckDepth turns the
// envelope into a divisor, so a sustained note near -12 dBFS ducks the wet by ~12 dB at
// duck = 1 — comfortably past the >= 6 dB verification gate of §7 Stage 1.
constexpr float kDuckAttackSeconds = 0.020f;
constexpr float kDuckReleaseSeconds = 1.200f;
constexpr float kDuckDepth = 12.0f;

// §3.1 safety limiter, -1 dBFS knee.
constexpr float kLimitThreshold = 0.89125094f;

// §5.1 colour axis.
//
// The 18 kHz ceiling is the knob's TOP corner, and the filter is faded in by the knob
// (see `colorDepth`) rather than run unconditionally. §5.1 calls colour 0 "neutral", and
// an always-on 18 kHz one-pole is not: it sits INSIDE the loop, so its loss compounds
// once per circulation. Room makes ~20 passes/s, and an 18 kHz one-pole is -2.16 dB at
// 16 kHz when the host runs at 96 kHz — -43 dB/s of unaccounted damping stacked on top
// of a Jot solve (§3.4) that exists precisely to own the per-band decay. The probe
// measured it: Room's 16 kHz octave decayed in 0.607 s against a 1.000 s Jot target
// (-39.3 %, outside §8's +-35 % band) at colour 0. Fading the filter in restores
// §3.4's authority over the decay at the default and leaves both documented endpoints
// (18 kHz at 0, 6 kHz at 1) exactly as written.
// AMENDED 2026-08-07: the knob's BOTTOM corner and inSat top are per-mode columns now
// (ModeSpec's colorLpBottomHz / colorInSatTop / colorInterpTop — the era axis, §5.1),
// so the two constants below are the Reverse/Spring rows' values, quoted in kModes so
// those two modes keep rendering exactly as before the amendment. The 18 kHz TOP corner
// stays global: colour 0 is the same "exactly neutral" statement in every mode.
constexpr float kColorLpTopHz = 18000.0f;
constexpr float kColorLpBottomHz = 6000.0f;
constexpr float kColorInSatTop = 0.60f;
constexpr float kColorMaxRho = 0.5f;
constexpr float kColorQuantStep = 32.0f;   // 1/32-sample steps

// §3.6 two-stage reverb_mod law (Strymon Cloud's). The published 0.35 -> 1.6 Hz range is
// applied as a RATIO on the per-mode base rate of §4.1, so Plate's 0.90 Hz does not jump
// down to 0.35 Hz at the knob's midpoint and the law stays continuous at 0.6.
constexpr float kModDepthKnee = 0.6f;
constexpr float kModRateTopMultiplier = 1.6f / 0.35f;
constexpr float kLayerBDepthFraction = 0.4f;
constexpr float kLayerBMinAllpassSeconds = 0.015f;

// §3.6 Layer C (AMENDED 2026-08-07): the Givens layer's four pair rates, as RATIOS on
// the mode's rateHz like everything else in §3.6. Incommensurate with each other and
// with Layer A's (0.6 + 0.8r) and Layer B's 0.5 multipliers, so the mixing never
// settles into a repeating pattern the tail could ring along with. DELIBERATELY an
// octave-and-a-bit under the delay layers' rates (0.02 .. 1.2 Hz across modes and knob):
// a rotation's residual contribution to the wet's instantaneous frequency is the rate
// of arg-response wobble, which scales with the ANGULAR RATE at fixed depth — measured
// on §8's pitch-stability row, Room's worst well-conditioned hop reads 19.7 cents with
// these multipliers at 4x (Layer-B territory) against 6.5 cents for the delay layers
// alone; at the shipped values the GATED statistics (level-weighted rms, worst
// loud-frame hop) return to that baseline while the ungated notch-transit figure
// stays elevated (18.3 cents, recorded in §4.1's amendment). Slow-and-deep is
// the design point the layer exists for: depth buys mode redistribution with zero
// Doppler, and the rate is what the pitch row taxes — the opposite trade from a delay
// modulator, where depth and rate both feed the Doppler term.
constexpr float kRotRateMul[4] = { 0.079f, 0.113f, 0.173f, 0.281f };

// §3.9 Spring. The three spring lengths are given in MILLISECONDS, not in the 48 kHz
// sample counts of §4.1's tank tables: a spring is not an FDN line, so the primality
// argument of §3.3 does not apply to it, and the ms form is rate-invariant as written.
constexpr float kSpringTdMs[3] = { 51.3f, 56.1f, 61.7f };
constexpr float kSpringHpHz = 90.0f;           // the circuit's rumble/feedback high-pass
// §3.9's in-loop LP sits at 2.8 kHz — which is §4.1's hfXoverHz for Spring, so the corner
// IS the mode table's crossover. That fixed corner is the reverb_damping = 0 endpoint;
// the knob darkens it two octaves (2800 -> 700 Hz) so it is never live-but-dead (§5.2).
// §4.1's hfMaxT60 = 0.35 s is then descriptive rather than a second solve, and it is met
// by construction: a one-pole at 2.8 kHz has |H| = w/(2-w) = 0.181 at Nyquist for 48 kHz,
// so with the slowest loop gain the mode can reach (10^(-3*0.054/4.0) = 0.912 at the 4.0 s
// ceiling) the HF band loses 15.6 dB per 54 ms pulse — a 0.21 s HF T60, and darker still
// at every higher host rate.
constexpr float kSpringDampOctaves = 2.0f;
// Dwell (§3.9): pre-emphasis (+6 dB shelf at 2 kHz) -> tanh(drive*x) -> de-emphasis, so
// the harmonics are generated BEFORE the dispersion and get chirped with everything else.
constexpr float kSpringDwellShelfHz = 2000.0f;
constexpr float kSpringDwellShelfGain = 2.0f;  // +6 dB
constexpr float kSpringDwellDepth = 11.0f;     // drive = 1 + depth*inSat*diffusion
// Pickup pans, equal power, springs laid left / centre / right, with the middle spring
// inverted so the three pickups stay decorrelated in the mono sum.
constexpr float kSpringPanL[3] = { 0.97237f, 0.70711f, 0.23345f };
constexpr float kSpringPanR[3] = { 0.23345f, 0.70711f, 0.97237f };
constexpr float kSpringSign[3] = { 1.0f, -1.0f, 1.0f };
// Wet-bus trim, chosen so Spring lands on the tank modes' level rather than by ear. Both
// back ends have line RMS = k_in / sqrt(1 - g^2) for white input, so the ratio is fixed:
// the tank sums 7 taps at kTankTapGain with k_in = 0.35 (0.35*sqrt(7)*0.35 = 0.324) and
// Spring sums 3 pickups with sum(panL^2) = 1.5 at k_in = 0.40 (0.66*sqrt(1.5)*0.40 =
// 0.323).
constexpr float kSpringOutGain = 0.66f;

// §3.12 Reverse.
constexpr float kRevRiseT60Multiple = 1.75f;   // -34.3 dB at t = 0
// §3.12's regulariser, as a FRACTION of W rather than the literal 10 ms. Two reasons,
// both measured. (a) §3.12 also requires the tap positions to be "fixed fractions of W";
// with an absolute 10 ms they are not — the layout changes shape as W moves. (b) 10 ms
// against a 1.5 s window is a 151:1 spacing ratio across only 64 taps, so whichever end
// of the law is the sparse one gets a 180 ms hole. That is not a choice between the two
// mirror directions: the literal formula puts the hole at t = W (the loudest point of
// the swell, which is why the mirror was flipped in the first place) and the flipped one
// puts it at t = 0, where the probe measured the wet envelope collapsing 49.7 dB below
// its running maximum at 0.171 s — §3.12 gate 1a, and audibly a gap in the swell rather
// than the "crescendo of echoes" the section describes. §3.7's ER law is bounded because
// it runs from a real 3 ms floor to 42..78 ms, a ratio of only 14..26:1; a fraction of W
// gives the Reverse mirror the same bounded shape at every window length, and keeps the
// gradient pointing the way §3.12's sentence asks (the last gap stays 3.2x tighter than
// the first).
//
// 0.45 is the measured optimum, not a round number. Gate 1a's worst dip below the running
// maximum reads -49.7 dB at the literal 10 ms, -13.8 dB at 0.15*W, -8.7 dB at 0.45*W and
// -7.4 dB at 2.0*W on the L channel; but the fraction trades directly against gate 1b,
// because spreading the taps also flattens the peak the 40 dB fall is measured from — 1b
// reads 45.6 dB at 0.15, 41.3 dB at 0.45 and 37.1 dB (FAIL) at 0.60, worst case over
// 44.1/48/96 kHz. 0.45 is the largest fraction at which every gate that passes today still
// passes.
//
// The residual dip is the floor of a 64-tap FIR read by a 20 ms window — 64 taps over
// W = 1.5 s is a 23.4 ms mean gap, wider than the window — and it is a floor, not a tuning
// miss: modelled independently of this engine, a perfectly uniform 64-tap layout still
// measures -1.5 dB and reaching the literal 1 dB takes 256-512 taps, 4x to 8x the section's
// own constant and the same multiple on the mode's cost. §3.12's gate 1a is therefore
// AMENDED (2026-08-06) rather than met: it splits into an audible-swell bound and a
// whole-swell bound, measured on the stereo power rather than on L alone, because §3.12
// pans every tap by an independent draw and one channel's envelope carries that draw's
// variance (-8.9 dB against -3.3 dB for the same render). tools/VerbProbe.cpp's
// kRevAudibleDipDb block carries the full table and the rejected alternatives.
//
// Gates 3 and 4 also read outside their thresholds against the ORIGINAL probe, and neither
// was a property of this constant: the two g = 0.5 density allpasses after the tap sum add
// a fixed 18.2 ms of group delay to the wet output that the probe was not counting, exactly
// as it counts the pre-delay. With it counted, gate 3 reads -84 .. -87 dBFS against -80 and
// gate 4 reads +0.1 .. +2.0 % against 5 %, at every rate and window. See
// kRevDensityApSeconds.
constexpr float kRevDensityFraction = 0.45f;
constexpr float kRevModDepthMs = 0.30f;
constexpr float kRevModRateHz = 0.50f;
constexpr float kRevWindowMax = 1.5f;
constexpr float kRevWindowMin = 0.2f;          // reverb_decay's own floor
constexpr float kRevInSatBase = 0.15f;

// §3.11 Shimmer.
//
// The in-loop lowpass after the shifter is not optional: an octave-up shifter doubles the
// spectrum on every pass, so without it a tail accumulates five source octaves in the
// 2-4 kHz band within a few circulations. §3.11 forces it to <= 4 kHz and puts a HARD
// limiter (not §3.5's soft clip) on the shifter output, because a soft clip's gradual
// compression still passes growing energy — the point of the limiter is an absolute
// amplitude bound on what re-enters the loop.
constexpr float kShimmerLpHz = 4000.0f;
constexpr float kShimmerLimit = 1.0f;
// §3.11's stability bound, shimmerGain <= alpha * (1 - g). Where `g` comes from, and why
// the knob scales the bound instead of being clipped by it, is in updateBlockRamps().
constexpr float kShimmerAlpha = 0.5f;

// Fixed LCG seeds. Everything seeded is drawn at prepare()/configure() or re-seeded in
// resetState(), so two renders of the same order are bit-identical (§6.4).
constexpr std::uint32_t kSeedJitter = 0x5F3A17C1u;
constexpr std::uint32_t kSeedEarly = 0x1B9E4D07u;
constexpr std::uint32_t kSeedReverse = 0x7C51A3B9u;
constexpr std::uint32_t kSeedWalk = 0x2D9B31F5u;
constexpr std::uint32_t kSeedPerMode = 7919u;

//==============================================================================
// §4.1 tuning tables. The 48 kHz sample counts are authoritative; ms = samples/48.
// All 24 tank values are prime.
//==============================================================================

constexpr int kTankRoom[8]  = { 1381, 1511, 1723, 1877, 2099, 2269, 2447, 2683 };
constexpr int kTankPlate[8] = { 2309, 2539, 2819, 3037, 3433, 3691, 4027, 4409 };
constexpr int kTankHall[8]  = { 6007, 6619, 7309, 8069, 8719, 9421, 10099, 11027 };

struct ModeSpec
{
    const int* tank48;
    float apRatioBase, apG;
    float inDiffG[4];
    float erWindowMs, erEnvExp, erToTank;
    float modDepthMs, modRateHz;
    float bassXoverHz, hfXoverHz, hfMaxT60;
    bool  plateSpread;
    float sizeMin, sizeMax;
    float decayCeiling;
    float inSat, kIn;
    float tonalCorrect;
    float rotMax;
    float colorLpBottomHz, colorInSatTop, colorInterpTop;
};

// Index 3 is Reverse. It takes none of the tank fields (§4.1): from this table it
// borrows only Hall's inDiffG, a 0.60 .. 1.40 size range and a 1.5 s window ceiling
// standing in for the decay ceiling. The tank columns are filled with Hall's so that
// nothing reads uninitialised, and processReverse() touches none of them.
//
// Index 4 is Spring, appended rather than inserted: specIndexFor() feeds the per-mode
// LCG seed, so any renumbering of 0..3 would move Room/Plate/Hall/Reverse's jitter draws
// and change their renders. Spring's own tank columns are inert for the same reason as
// Reverse's — its geometry is kSpringTdMs and its diffusers are the dispersion (§3.9).
// The trailing column is §3.4's tonal-correction amount (AMENDED 2026-08-07): Plate
// takes the full amplitude ∝ √T60 correction (the clean, level-neutral machine), Room
// deliberately keeps some of the classic boom/dull coupling at 0.6, and Spring/Reverse
// take 0 — their back ends are not the Jot solve, and §3.12 gate 6 asserts that damping
// and bassmult stay bit-inert in Reverse, which a unity-gain shelf would already break.
// Hall is 0.75, MEASURED rather than styled: its first tank pass reaches the output taps
// before the Filt1 stages have acted (lines are 10..19 % of a 2 s decay), and that
// knob-independent chunk carries roughly twice the low-octave energy of the late tail —
// at full strength the shelves overcorrect the steady-state band balance by +3.6 dB at
// bassmult ×0.25 against §8's 3 dB tail-level gate; 0.75 brings both knob extremes
// inside it (worst 2.63/2.76/2.78 dB at 44.1/48/96 kHz, ×0.25 with damping maxed).
// The `rotMax` column is §3.6 Layer C's depth (AMENDED 2026-08-07): the largest tangent
// the Givens layer's angles reach at full reverb_mod. Room takes the deepest rotation —
// it is the mode with NO Layer B (every allpass under 15 ms) and the thinnest Layer A
// (the guards scale depth to the shortest line), so matrix modulation is the only
// liveliness lever it has left; Plate/Hall already run both delay layers and take less.
// Spring and Reverse never reach processTank, so their 0 is structural, not tuning.
// The three trailing colour columns are §5.1's era axis (AMENDED 2026-08-07): the
// knob-top LP corner, inSat drive and axis-4 interpolator blend, so one knob reads as a
// different machine age per mode — Room {6 k, 0.60, 0.90} the 1970s box (dark, gritty,
// coarsest interpolator), Plate {7 k, 0.60, 0.60} the 1980s, Hall {9 k, 0.35, 0.25}
// staying nearly modern. Shimmer shares Hall's row as always. Spring/Reverse quote the
// pre-amendment constants and colorInterpTop 0, all structural: Spring's pickup read is
// its own loop but a spring's era is the dispersion, not a delay interpolator, and
// Reverse's window line is a single-pass FIR where §3.12 already declares linear
// transparent — nothing compounds, so there is no axis to degrade.
constexpr ModeSpec kModes[5] = {
    // Room
    { kTankRoom, 0.22f, 0.58f, { 0.75f, 0.75f, 0.625f, 0.625f },
      42.0f, 1.0f, 0.60f, 0.35f, 0.60f, 450.0f, 4500.0f, 1.00f, false,
      0.40f, 1.40f, 2.5f, 0.15f, 0.35f, 0.60f, 0.30f, 6000.0f, 0.60f, 0.90f },
    // Plate
    { kTankPlate, 0.30f, 0.60f, { 0.78f, 0.78f, 0.70f, 0.70f },
      18.0f, 0.3f, 0.90f, 0.40f, 0.90f, 500.0f, 5500.0f, 1.25f, true,
      0.35f, 1.20f, 4.5f, 0.20f, 0.35f, 1.00f, 0.15f, 7000.0f, 0.60f, 0.60f },
    // Hall
    { kTankHall, 0.16f, 0.55f, { 0.70f, 0.70f, 0.60f, 0.60f },
      78.0f, 0.5f, 0.75f, 0.55f, 0.30f, 400.0f, 3500.0f, 1.25f, true,
      0.50f, 1.50f, 10.0f, 0.10f, 0.30f, 0.75f, 0.10f, 9000.0f, 0.35f, 0.25f },
    // Reverse
    { kTankHall, 0.16f, 0.55f, { 0.70f, 0.70f, 0.60f, 0.60f },
      78.0f, 0.5f, 0.00f, kRevModDepthMs, kRevModRateHz, 400.0f, 3500.0f, 1.25f, false,
      0.60f, 1.40f, kRevWindowMax, kRevInSatBase, 0.30f, 0.00f, 0.00f,
      kColorLpBottomHz, kColorInSatTop, 0.00f },
    // Spring (§4.1's Spring column). apRatio/apG 0 — the dispersion IS the allpass;
    // inDiffG 0 — it replaces the input diffusers (§3.1); erToTank 0 — Early Energy is
    // idled; modDepth 0 — a spring is a mechanical line and does not detune (the rate is
    // only there to keep the shared segment counters well formed).
    { kTankPlate, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f, 0.0f },
      42.0f, 1.0f, 0.00f, 0.0f, 0.50f, 250.0f, 2800.0f, 0.35f, false,
      0.50f, 1.50f, 4.0f, 0.50f, 0.40f, 0.00f, 0.00f,
      kColorLpBottomHz, kColorInSatTop, 0.00f },
};

// Shimmer takes no row of its own. §3.11 is explicit that "Shimmer is a routing switch,
// not a topology": it is Hall's tank with the granular shifter bolted into the loop, so it
// deliberately shares Hall's spec index — and therefore Hall's per-mode LCG seed below, so
// at reverb_shimmer = 0 it renders exactly as Hall does, to the bit. §4.1's tuning table
// has no Shimmer column for the same reason.
int specIndexFor (int resolvedMode) noexcept
{
    if (resolvedMode == ReverbEngine::modeReverse) return 3;
    if (resolvedMode == ReverbEngine::modeSpring)  return 4;
    if (resolvedMode == ReverbEngine::modeShimmer) return 2;
    return juce::jlimit (0, 2, resolvedMode);
}

const ModeSpec& specFor (int resolvedMode) noexcept { return kModes[specIndexFor (resolvedMode)]; }

//==============================================================================
// §3.8 output tap sets. L draws its four "own" taps from the odd lines and R from the
// even ones — the cross-asymmetry that makes the two outputs genuinely uncorrelated.
//==============================================================================

struct TankTap { int line; float frac; float sign; };

constexpr TankTap kTapsL[7] = {
    { 1, 0.081f, +1.0f }, { 3, 0.293f, +1.0f }, { 5, 0.577f, -1.0f }, { 7, 0.771f, +1.0f },
    { 0, 0.412f, -1.0f }, { 2, 0.634f, -1.0f }, { 4, 0.913f, -1.0f },
};
constexpr TankTap kTapsR[7] = {
    { 0, 0.107f, +1.0f }, { 2, 0.331f, +1.0f }, { 4, 0.519f, -1.0f }, { 6, 0.742f, +1.0f },
    { 1, 0.386f, -1.0f }, { 3, 0.691f, -1.0f }, { 5, 0.874f, -1.0f },
};

//==============================================================================
// §3.8 frequency-dependent L/R spread: two DIFFERENTLY tuned cascades of three 2nd-order
// allpasses, one per channel. Stored as (centre, pole bandwidth) in Hz so the group delay
// is the same at every host rate; the radius is exp(-pi*bw/fs). Measured group delay at
// 48 kHz: L 7.24 ms and R 13.00 ms at 100 Hz, both under 0.03 ms above 4 kHz.
// A ONE-sided chain would pull the image left on every LF transient, which is why both
// channels carry a chain.
//==============================================================================

constexpr float kSpreadF0[2][3] = { { 80.0f, 170.0f, 460.0f }, { 80.0f, 110.0f, 240.0f } };
constexpr float kSpreadBw[2][3] = { { 160.0f, 340.0f, 900.0f }, { 90.0f, 130.0f, 280.0f } };

//==============================================================================
// Helpers
//==============================================================================

struct Lcg
{
    std::uint32_t state;
    explicit Lcg (std::uint32_t seed) noexcept : state (seed) {}
    inline std::uint32_t next() noexcept { state = state * 1664525u + 1013904223u; return state; }
    inline float uni() noexcept { return (float) (next() >> 8) * (1.0f / 16777216.0f); }
    inline float bi() noexcept { return uni() * 2.0f - 1.0f; }
};

inline float nextUniform (std::uint32_t& state) noexcept
{
    state = state * 1664525u + 1013904223u;
    return (float) (state >> 8) * (1.0f / 16777216.0f);
}

/** One-pole coefficient for a cutoff in Hz. */
float onePoleW (float hz, double sampleRate) noexcept
{
    const float f = juce::jlimit (0.1f, (float) (sampleRate * 0.49), hz);
    return juce::jlimit (1.0e-6f, 1.0f, 1.0f - std::exp (-kTwoPi * f / (float) sampleRate));
}

/** |H| at `hz` of the one-pole `y += w*(x-y)`, or of its complement `x - y` when
    `highPass`. Used by §3.9's loop-gain solve, which has to count what the in-loop
    filters take out per pulse and not just the delay. */
float onePoleMagnitude (float w, float hz, double sampleRate, bool highPass) noexcept
{
    const float omega = kTwoPi * (float) juce::jlimit (0.1, sampleRate * 0.49, (double) hz)
                        / (float) sampleRate;
    const float p = 1.0f - w;
    const float re = 1.0f - p * std::cos (omega);
    const float im = p * std::sin (omega);
    const float den = juce::jmax (1.0e-12f, std::sqrt (re * re + im * im));

    return highPass ? p * 2.0f * std::abs (std::sin (0.5f * omega)) / den
                    : w / den;
}

/** §3.3 mirrored guard: a write into the first `kGuard` slots is mirrored past the end so
    that any four consecutive samples can be read without a wrap test. */
inline void writeGuarded (float* buf, int len, int pos, float v) noexcept
{
    buf[pos] = v;
    if (pos < 4)
        buf[len + pos] = v;
}

inline int advanceWrite (int pos, int len) noexcept
{
    const int n = pos + 1;
    return n >= len ? 0 : n;
}

/** 4-point cubic Hermite (Catmull-Rom). `delay` is in samples and must be >= 3 when the
    caller reads before writing this sample's value, >= 2 when it writes first. */
inline float readCubic (const float* buf, int len, int writePos, float delay) noexcept
{
    const float readPos = (float) writePos - delay;
    const int base = (int) std::floor (readPos);
    const float t = readPos - (float) base;

    int j = base - 1;
    if (j < 0) j += len;
    if (j >= len) j -= len;

    const float x0 = buf[j], x1 = buf[j + 1], x2 = buf[j + 2], x3 = buf[j + 3];
    const float c0 = x1;
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

/** 2-point linear read. §3.12: a single-pass FIR traverses each read exactly once, so
    linear is transparent there — the cubic mandate of §3.4 is about loop compounding. */
inline float readLinear (const float* buf, int len, int writePos, float delay) noexcept
{
    const float readPos = (float) writePos - delay;
    const int base = (int) std::floor (readPos);
    const float t = readPos - (float) base;

    int j = base;
    if (j < 0) j += len;
    if (j >= len) j -= len;

    return buf[j] * (1.0f - t) + buf[j + 1] * t;
}

/** §3.5 C1-continuous soft clip: value and slope both match at the threshold. The first
    draft's x/(1+0.3|x|) jumped by 0.155 at its own knee — a step generator in a loop. */
inline float softClip (float x, float k) noexcept
{
    const float ax = std::abs (x);
    if (ax <= kClipThreshold)
        return x;

    const float over = ax - kClipThreshold;
    const float y = kClipThreshold + over / (1.0f + k * over);
    return x < 0.0f ? -y : y;
}

/** §3.5 ADAA (AMENDED 2026-08-07): antiderivative of the clip RESIDUAL r(x) =
    softClip(x) - x, which is identically 0 below the knee and odd, so F_r is even and
    this evaluates |x| only. For |x| > t with u = |x| - t:

        r(u)   = u/(1+ku) - u = -k*u^2/(1+ku)
        F_r(u) = u/k - log1p(ku)/k^2 - u^2/2

    F_r is O(u^3) at the knee — the C1 knee softClip went out of its way for (see above)
    is inherited, and the divided difference below stays well conditioned. */
inline float clipResidualAntideriv (float x, float k) noexcept
{
    const float ax = std::abs (x);

    if (ax <= kClipThreshold)
        return 0.0f;

    const float u = ax - kClipThreshold;
    return (u - std::log1p (k * u) / k) / k - 0.5f * u * u;
}

/** §3.5 first-order ADAA on the clip residual: y = x + (F_r(x) - F_r(prev)) / (x - prev).
    Structured so the linear region is EXACTLY identity — the residual is 0 below the
    knee, so sub-knee signal takes the early-out and colour 0 stays exactly neutral,
    Freeze's "clip bypassed" recipe stays honest, and no 2-tap averager is imposed on the
    loop where the clip never engages. The half-sample smoothing ADAA1 implies applies to
    the RESIDUAL only; reported latency stays exactly 0 (§2's first invariant). log1p
    runs only when a sample straddles the knee — the minority. */
inline float softClipAdaa (float x, float prev, float k) noexcept
{
    if (std::abs (x) <= kClipThreshold && std::abs (prev) <= kClipThreshold)
        return x;

    const float dx = x - prev;

    if (std::abs (dx) > kAdaaEps)
        return x + (clipResidualAntideriv (x, k) - clipResidualAntideriv (prev, k)) / dx;

    // Midpoint fallback, C0-continuous with the branch above at |dx| = eps.
    const float mid = 0.5f * (x + prev);
    return x + softClip (mid, k) - mid;
}

/** §3.1 safety limiter. Never expected to engage; C1 so that it does not generate a step
    if it does. */
inline float safetyLimit (float x) noexcept
{
    const float ax = std::abs (x);
    if (ax <= kLimitThreshold)
        return x;

    const float y = kLimitThreshold + (1.0f - kLimitThreshold)
                                          * std::tanh ((ax - kLimitThreshold) / (1.0f - kLimitThreshold));
    return x < 0.0f ? -y : y;
}

/** §3.4 interpolator compensation: |Hi(f)| averaged over a uniform fractional-delay
    distribution, tabulated at configure so r_hi is solved against a compensated target.
    `blend` is §5.1 axis 4's cubic→linear kernel mix: the blended read is the blended
    4-tap FIR h[k] = (1−c)·h_cubic[k] + c·h_lin[k] with h_lin = {0, 1−t, t, 0} on the
    SAME tap frame, so this is the degraded kernel's magnitude EXACTLY, not a model —
    the property that keeps axis 4 out of the −39.3 % failure class kColorLpTopHz
    records. `blend = 0` reproduces the pre-axis-4 value to the bit. */
float meanInterpolatorMagnitude (float freqHz, double sampleRate, double blend = 0.0)
{
    const double w = 2.0 * juce::MathConstants<double>::pi
                     * juce::jlimit (1.0, sampleRate * 0.49, (double) freqHz) / sampleRate;
    constexpr int steps = 64;
    double sum = 0.0;

    for (int i = 0; i < steps; ++i)
    {
        const double t = ((double) i + 0.5) / (double) steps;
        const double t2 = t * t, t3 = t2 * t;
        double h[4] = { -0.5 * t3 + t2 - 0.5 * t,
                        1.5 * t3 - 2.5 * t2 + 1.0,
                        -1.5 * t3 + 2.0 * t2 + 0.5 * t,
                        0.5 * t3 - 0.5 * t2 };

        if (blend > 0.0)
        {
            const double hLin[4] = { 0.0, 1.0 - t, t, 0.0 };

            for (int k = 0; k < 4; ++k)
                h[k] += blend * (hLin[k] - h[k]);
        }

        double re = 0.0, im = 0.0;

        for (int k = 0; k < 4; ++k)
        {
            re += h[k] * std::cos (w * (double) k);
            im -= h[k] * std::sin (w * (double) k);
        }

        sum += std::sqrt (re * re + im * im);
    }

    return (float) juce::jlimit (0.5, 1.0, sum / (double) steps);
}

/** §4.1: apRatio_i = base * (0.90 + 0.20*r_i), rejected while it sits within 1 % of a
    fraction with denominator < 12. A single shared ratio would make every allpass echo
    coincide with a line circulation on all eight lines at once. */
float drawApRatio (float base, Lcg& lcg)
{
    float ratio = base;

    for (int attempt = 0; attempt < 64; ++attempt)
    {
        ratio = base * (0.90f + 0.20f * lcg.uni());
        bool ok = true;

        for (int q = 2; q < 12 && ok; ++q)
            for (int p = 1; p < q && ok; ++p)
            {
                const float f = (float) p / (float) q;
                if (std::abs (ratio - f) < 0.01f * f)
                    ok = false;
            }

        if (ok)
            break;
    }

    return ratio;
}

/** Exact length for `seconds`, plus a few samples so the longest delay a mode can ask for
    is not clipped by the read-time clamp (which reserves the last 5 slots). */
int bufferLength (float seconds, double sampleRate) noexcept
{
    return juce::jmax (16, (int) std::ceil (seconds * (float) sampleRate) + 8);
}
} // namespace

//==============================================================================
// Mode resolution
//==============================================================================
int ReverbEngine::resolveMode (int algo) noexcept
{
    // §4: Spring shipped at Stage 2 and Shimmer at Stage 3, so every entry of the FROZEN
    // choice list is live and NO fallback path remains. What is left is the range clamp —
    // an out-of-range index (a corrupt state, a future entry) still has to land on a real
    // machine rather than index a table off the end.
    return juce::jlimit (0, (int) numModes - 1, algo);
}

float ReverbEngine::decayCeilingFor (int resolvedMode) noexcept
{
    return specFor (resolvedMode).decayCeiling;
}

float ReverbEngine::windowSecondsFor (float decaySeconds) noexcept
{
    return juce::jlimit (kRevWindowMin, kRevWindowMax, decaySeconds);
}

//==============================================================================
// prepare / buffer layout
//==============================================================================
void ReverbEngine::layOutBuffers()
{
    tankLen = bufferLength (kMaxDelaySeconds, sampleRate);
    apLen   = bufferLength (kMaxAllpassSeconds, sampleRate);
    preLen  = bufferLength (kMaxPredelaySeconds, sampleRate);
    erLen   = bufferLength (kMaxErSeconds, sampleRate);
    revLen  = bufferLength (kReverseWindowSeconds, sampleRate);

    // Short allpasses are sized at the widest sizeMax across all modes, so a mode switch
    // never needs a bigger buffer than it already owns (§6.2: it must never allocate).
    const auto shortLen = [this] (float ms)
    {
        return bufferLength (ms * 0.001f * kMaxSizeScale, sampleRate);
    };

    int diffLen[kNumDiffusers] {}, erApLen[2] {}, revApLen[2][2] {};

    for (int k = 0; k < kNumDiffusers; ++k)
        diffLen[k] = shortLen (kDiffuserMs[k]);

    for (int j = 0; j < 2; ++j)
        erApLen[j] = shortLen (kErApMs[j]);

    for (int ch = 0; ch < 2; ++ch)
        for (int j = 0; j < 2; ++j)
            revApLen[ch][j] = shortLen (kRevApMs[ch][j]);

    size_t total = (size_t) kNumLines * (size_t) (tankLen + kGuard)
                 + (size_t) kNumLines * (size_t) (apLen + kGuard)
                 + 2u * (size_t) (preLen + kGuard)
                 + (size_t) (erLen + kGuard)
                 + (size_t) (revLen + kGuard);

    for (int ch = 0; ch < 2; ++ch)
    {
        for (int k = 0; k < kNumDiffusers; ++k) total += (size_t) (diffLen[k] + kGuard);
        for (int j = 0; j < 2; ++j)             total += (size_t) (erApLen[j] + kGuard);
        for (int j = 0; j < 2; ++j)             total += (size_t) (revApLen[ch][j] + kGuard);
    }

    // The single flat allocation of §3.3. Exact-length buffers plus a 4-sample mirrored
    // guard each — NOT power-of-two rings, which would inflate the footprint ~60 %.
    pool.assign (total, 0.0f);

    size_t offset = 0;
    const auto take = [this, &offset] (int len) -> float*
    {
        float* p = pool.data() + offset;
        offset += (size_t) (len + kGuard);
        return p;
    };

    for (int i = 0; i < kNumLines; ++i) tankBuf[(size_t) i] = take (tankLen);
    for (int i = 0; i < kNumLines; ++i) apBuf[(size_t) i]   = take (apLen);
    preBuf[0] = take (preLen);
    preBuf[1] = take (preLen);
    erBuf = take (erLen);
    revBuf = take (revLen);

    for (int ch = 0; ch < 2; ++ch)
    {
        for (int k = 0; k < kNumDiffusers; ++k)
        {
            diffuser[(size_t) ch][(size_t) k].buf = take (diffLen[k]);
            diffuser[(size_t) ch][(size_t) k].len = diffLen[k];
        }

        for (int j = 0; j < 2; ++j)
        {
            erAllpass[(size_t) ch][(size_t) j].buf = take (erApLen[j]);
            erAllpass[(size_t) ch][(size_t) j].len = erApLen[j];
        }

        for (int j = 0; j < 2; ++j)
        {
            revAllpass[(size_t) ch][(size_t) j].buf = take (revApLen[ch][j]);
            revAllpass[(size_t) ch][(size_t) j].len = revApLen[ch][j];
        }
    }

    jassert (offset == pool.size());

    // §3.9's cascades. Sized for EVERY mode, not on entry to Spring, because a mode change
    // must never allocate (§6.2). They sit outside `pool` — 100 short rings per spring is
    // not the same animal as a delay line with a mirrored guard region, and 8.4 KB at
    // 48 kHz (28.8 KB at 192) does not earn a second layout pass in §3.3's flat block.
    for (auto& s : springs)
        s.prepare (sampleRate);

    // §3.11's shifter, sized for EVERY mode for the same reason the cascades are: entering
    // Shimmer must not allocate (§6.2). 70 ms of delay line plus a 1025-point Hann table is
    // 18 KB at 48 kHz (58 KB at 192), so it does not earn a second pass over §3.3's flat
    // block either.
    shifter.prepare (sampleRate);
}

void ReverbEngine::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;

    layOutBuffers();
    prepared = true;

    const float fs = (float) sampleRate;

    dcOutW = onePoleW (kOutputDcHz, sampleRate);
    tiltW = onePoleW (kTiltPivotHz, sampleRate);
    shimLpW = onePoleW (kShimmerLpHz, sampleRate);
    springHpW = onePoleW (kSpringHpHz, sampleRate);
    dwellShelfW = onePoleW (kSpringDwellShelfHz, sampleRate);
    duckAttack = 1.0f - std::exp (-1.0f / (kDuckAttackSeconds * fs));
    duckRelease = 1.0f - std::exp (-1.0f / (kDuckReleaseSeconds * fs));
    preSlewPerSample = kPredelaySlewPerSample;
    xfadeInc = 1.0f / juce::jmax (1.0f, kPredelayXfadeSeconds * fs);

    // §6.5: 20 ms on every gain.
    for (auto* s : { &mixWet, &widthAmount, &erLevelAmount, &tiltLowGain, &tiltHighGain, &duckAmount })
        s->reset (sampleRate, 0.02);

    configure (mode, windowSeconds);

    // §6.4: prepare() ALWAYS clears. renderChain's bit-identical contract depends on it.
    reset();
}

//==============================================================================
// configure — mode tables, no allocation (§6.2)
//==============================================================================
void ReverbEngine::configure (int algo, float windowSecondsIn)
{
    mode = resolveMode (algo);
    windowSeconds = windowSecondsFor (windowSecondsIn);

    if (! prepared)
        return;

    buildModeTables();
}

void ReverbEngine::buildModeTables()
{
    const ModeSpec& spec = specFor (mode);
    const float fs = (float) sampleRate;

    sizeMin = spec.sizeMin;
    sizeMax = spec.sizeMax;
    sizeSlewPerSample = kSizeSlewPerSecond * (sizeMax - sizeMin) / fs;

    // A mode change can narrow the Size range under a value the previous mode allowed.
    sizeScaleCur = juce::jlimit (sizeMin, sizeMax, sizeScaleCur);
    sizeScaleTarget = juce::jlimit (sizeMin, sizeMax, sizeScaleTarget);
    erToTank = spec.erToTank;
    kIn = spec.kIn;
    bassXoverHz = spec.bassXoverHz;
    hfXoverHz = spec.hfXoverHz;
    hfMaxT60 = spec.hfMaxT60;
    tonalCorrect = spec.tonalCorrect;
    rotMax = spec.rotMax;                          // §3.6 Layer C depth
    tcLoW = onePoleW (bassXoverHz, sampleRate);    // §3.4 tonal-correction shelf corners
    tcHiW = onePoleW (hfXoverHz, sampleRate);
    inSatBase = spec.inSat;
    inSat = spec.inSat;
    colorLpBottomHz = spec.colorLpBottomHz;        // §5.1 era-axis columns
    colorInSatTop = spec.colorInSatTop;
    colorInterpTop = spec.colorInterpTop;
    modDepthMs = spec.modDepthMs;
    modRateHz = spec.modRateHz;
    spreadOn = spec.plateSpread;

    for (int k = 0; k < kNumDiffusers; ++k)
        inDiffG[(size_t) k] = spec.inDiffG[k];

    Lcg lcg (kSeedJitter + kSeedPerMode * (std::uint32_t) (specIndexFor (mode) + 1));

    for (int i = 0; i < kNumLines; ++i)
    {
        auto& line = lines[(size_t) i];
        line.apRatio = drawApRatio (spec.apRatioBase, lcg);
        line.apG = juce::jmin (0.60f, spec.apG);        // §3.5: hard cap for every mode
        line.rateJitter = 0.6f + 0.8f * lcg.uni();      // §3.6
        line.depthJitter = 0.85f + 0.3f * lcg.uni();

        // Layer B quadrature phase offsets 2*pi*k/8, held as cos/sin so the runtime
        // needs one complex rotation per sample rather than eight sines.
        const float phase = kTwoPi * (float) i / (float) kNumLines;
        line.phaseCos = std::cos (phase);
        line.phaseSin = std::sin (phase);
    }

    // §3.4: tabulate |Hi(f_h)|_mean at configure, per mode (f_h is a per-mode constant).
    // §5.1 axis 4 (AMENDED 2026-08-07): tabulated over the kernel blend c ∈ [0, 1] in
    // 1/16 steps rather than as one scalar, so updateBlockRamps can lerp the r_hi
    // compensation to the kernel the loop actually reads through at the live colour.
    // ~8.7 k trig calls, at configure — a mode switch is compute-allowed, alloc-free.
    for (int k = 0; k < (int) interpMagTable.size(); ++k)
        interpMagTable[(size_t) k] = meanInterpolatorMagnitude (hfXoverHz, sampleRate,
                                                                (double) k / 16.0);

    interpMagAtHf = interpMagTable[0];

    // §3.8 spread cascades. r = exp(-pi*bw/fs) keeps the group delay rate-independent.
    for (int ch = 0; ch < 2; ++ch)
        for (int s = 0; s < kNumSpreadSections; ++s)
        {
            const float r = std::exp (-juce::MathConstants<float>::pi * kSpreadBw[ch][s] / fs);
            const float w0 = kTwoPi * kSpreadF0[ch][s] / fs;
            spread[(size_t) ch][(size_t) s].a1 = -2.0f * r * std::cos (w0);
            spread[(size_t) ch][(size_t) s].a2 = r * r;
        }

    revColorW = colorLpW;

    // §3.9: the dispersion's DC group delay is part of every spring's loop period, so the
    // loop-gain solve has to count it. All three cascades share K, a1 and M.
    springLfDelay = springs[0].lfDelaySamples();

    // §3.2 decay-normalised injection (AMENDED 2026-08-07): the reference per-pass loop
    // gain, solved once per mode at the 1.5 s pivot (the same pivot §3.6's t60clamp uses)
    // and at sizeScale 1.0. The MINIMUM loop delay carries the LARGEST g, matching
    // gMidMax — the max-over-lines quantity the per-block scale in updateBlockRamps()
    // divides against — so the scale is exactly 1.0 at the pivot. Spring's loop period
    // counts the dispersion's DC delay exactly as §3.9's live solve does; the /loopLoss
    // compensation is deliberately on NEITHER side — the k_in/sqrt(1-g^2) law (see
    // kSpringOutGain) is stated on the per-pass gain the signal actually keeps, which is
    // 10^(-3*D/T60) by the solve's own construction, BEFORE the division that hands back
    // what the in-loop filters then take out again.
    {
        float minLoop;   // fractional samples at the host rate

        if (mode == modeSpring)
        {
            minLoop = kSpringTdMs[0] * 0.001f * fs + springLfDelay;

            for (int s = 1; s < kNumSprings; ++s)
                minLoop = juce::jmin (minLoop, kSpringTdMs[s] * 0.001f * fs + springLfDelay);
        }
        else
        {
            minLoop = (float) spec.tank48[0] * (fs / 48000.0f) * (1.0f + lines[0].apRatio);

            for (int i = 1; i < kNumLines; ++i)
                minLoop = juce::jmin (minLoop, (float) spec.tank48[i] * (fs / 48000.0f)
                                                   * (1.0f + lines[(size_t) i].apRatio));
        }

        gRef = juce::jmin (0.9999f, std::pow (0.001f, minLoop / fs / 1.5f));
    }

    rebuildErTaps();
    rebuildReverseTaps();

    // §3.3: the only prepare-time hard constraint. Satisfied by construction at each
    // mode's sizeMin — Room 6396, Plate 9192, Hall 33635 at 48 kHz. §4.1 marks the row
    // "n/a" for Spring, and rightly: modal density is an FDN argument, and three springs
    // at 84.5 ms are a physical model of three springs, not a sparse tank.
    float sum = 0.0f;
    for (int i = 0; i < kNumLines; ++i)
        sum += (float) spec.tank48[i] * sizeMin;
    jassert (mode == modeReverse || mode == modeSpring || sum >= 6000.0f);
    juce::ignoreUnused (sum);

    // §8 matrix unitarity: the normalized Hadamard is exactly orthogonal in exact
    // arithmetic, so cond(A) = 1. Assert the implemented butterfly preserves the norm.
   #if JUCE_DEBUG
    {
        float v[kNumLines] = { 0.3f, -1.1f, 0.7f, 2.0f, -0.4f, 0.9f, -1.7f, 0.2f };
        float before = 0.0f;
        for (float x : v) before += x * x;

        for (int i = 0; i < 8; i += 2) { const float u = v[i], t = v[i + 1]; v[i] = u + t; v[i + 1] = u - t; }
        for (int b = 0; b < 8; b += 4)
            for (int i = 0; i < 2; ++i) { const float u = v[b + i], t = v[b + i + 2]; v[b + i] = u + t; v[b + i + 2] = u - t; }
        for (int i = 0; i < 4; ++i) { const float u = v[i], t = v[i + 4]; v[i] = u + t; v[i + 4] = u - t; }

        float after = 0.0f;
        for (float x : v) after += x * x * kInvSqrt8 * kInvSqrt8;
        jassert (std::abs (after / before - 1.0f) < 1.0e-3f);
    }
   #endif
}

void ReverbEngine::rebuildErTaps()
{
    // §3.7: one mono tapped delay line, 24 taps, no allpasses in the tap set, per-tap
    // equal-power pan. Positions do NOT scale with Size — the 0.09 s buffer of §6.7 is
    // sized for Hall's 78 ms window as written.
    const ModeSpec& spec = specFor (mode);
    const float fs = (float) sampleRate;
    Lcg lcg (kSeedEarly + kSeedPerMode * (std::uint32_t) (specIndexFor (mode) + 1));

    const float t0 = 0.003f;                                                   // 3 ms
    const float t1 = juce::jmax (t0 * 2.0f,
                                 juce::jmin (spec.erWindowMs * 0.001f, kMaxErSeconds - 0.002f));
    const float span = t1 * t1 - t0 * t0;
    float energy = 0.0f;
    float nominal[kNumErTaps];

    // Density grows quadratically: the cumulative tap count is proportional to t^2, i.e.
    // spacing is proportional to 1/t. Positions are laid out first, then jittered by a
    // fraction of the LOCAL gap so the set stays ordered and non-arithmetic.
    for (int k = 0; k < kNumErTaps; ++k)
        nominal[k] = std::sqrt (t0 * t0 + (float) k / (float) (kNumErTaps - 1) * span);

    for (int k = 0; k < kNumErTaps; ++k)
    {
        const float prevGap = k > 0 ? nominal[k] - nominal[k - 1] : nominal[1] - nominal[0];
        const float nextGap = k + 1 < kNumErTaps ? nominal[k + 1] - nominal[k]
                                                 : nominal[k] - nominal[k - 1];
        const float t = juce::jlimit (t0 * 0.5f, t1,
                                      nominal[k] + 0.15f * juce::jmin (prevGap, nextGap) * lcg.bi());

        float a = std::pow (t / t0, -spec.erEnvExp) * (1.0f + 0.10f * lcg.bi());
        if ((k & 1) != 0)
            a = -a;

        const float pan = lcg.uni();
        auto& tap = erTaps[(size_t) k];
        tap.delaySamples = juce::jlimit (1, erLen - 2, (int) std::lround (t * fs));
        tap.gainL = a * std::cos (pan * juce::MathConstants<float>::halfPi);
        tap.gainR = a * std::sin (pan * juce::MathConstants<float>::halfPi);
        tap.gainMono = a;
        energy += a * a;
    }

    const float norm = 1.0f / std::sqrt (juce::jmax (1.0e-12f, energy));

    for (auto& tap : erTaps)
    {
        tap.gainL *= norm;
        tap.gainR *= norm;
        tap.gainMono *= norm;
    }
}

void ReverbEngine::rebuildReverseTaps()
{
    // §3.12: 64 taps whose gains RISE with tap time, positions spaced by §3.7's density
    // law time-mirrored (spacing ~ 1/(W - t + 10 ms)), gains the time-mirror of an
    // exponential decay with T60 = 1.75*W, normalised so the wet loudness is
    // window-invariant.
    const float fs = (float) sampleRate;
    const float w = windowSeconds;
    Lcg lcg (kSeedReverse);

    const float a0 = kRevDensityFraction * w;
    const float span = (w + a0) * (w + a0) - a0 * a0;
    float energy = 0.0f;

    // The 8 shared Layer-A walkers inherit §3.6's per-stream jitter laws.
    for (int k = 0; k < kNumWalkers; ++k)
    {
        walkers[(size_t) k].rateJitter = 0.6f + 0.8f * lcg.uni();
        walkers[(size_t) k].depthJitter = 0.85f + 0.3f * lcg.uni();
    }

    // §3.7's quadratic density law with the regulariser above: the cumulative tap count
    // is proportional to (t + a0)^2, so the taps crowd together as t approaches W —
    // "echo density grows exactly where the envelope grows".
    //
    // DEVIATION, deliberate: §3.12 also writes the law as "spacing ~ 1/(W - t_k + 10 ms)",
    // which is the same law mirrored the other way and puts the SPARSEST taps at t = W.
    // At W = 1.5 s that is a 180 ms gap at the loudest point of the swell — the discrete
    // slapback the same paragraph says diffusion exists to prevent — and it makes gate 1
    // (no envelope point more than 1 dB below its running maximum) unreachable. The
    // sentence the formula sits in, and the gate, are followed instead of the formula.
    float nominal[kNumRevTaps];

    for (int k = 0; k < kNumRevTaps; ++k)
        nominal[k] = std::sqrt (a0 * a0 + (float) k / (float) (kNumRevTaps - 1) * span) - a0;

    for (int k = 0; k < kNumRevTaps; ++k)
    {
        const float prevGap = k > 0 ? nominal[k] - nominal[k - 1] : nominal[1] - nominal[0];
        const float nextGap = k + 1 < kNumRevTaps ? nominal[k + 1] - nominal[k]
                                                  : nominal[k] - nominal[k - 1];
        const float t = juce::jlimit (0.0f, w,
                                      nominal[k] + 0.15f * juce::jmin (prevGap, nextGap) * lcg.bi());
        const float back = w - t;

        float a = std::pow (10.0f, -3.0f * back / (kRevRiseT60Multiple * w))
                  * (1.0f + 0.10f * lcg.bi());
        if ((k & 1) != 0)
            a = -a;

        const float pan = lcg.uni();
        auto& tap = revTaps[(size_t) k];
        tap.delaySamples = juce::jlimit (0.0f, (float) (revLen - 5), t * fs);
        tap.gainL = a * std::cos (pan * juce::MathConstants<float>::halfPi);
        tap.gainR = a * std::sin (pan * juce::MathConstants<float>::halfPi);
        tap.gainMono = a;
        tap.walker = k % kNumWalkers;     // decorrelates ADJACENT taps
        energy += a * a;
    }

    const float norm = 1.0f / std::sqrt (juce::jmax (1.0e-12f, energy));

    for (auto& tap : revTaps)
    {
        tap.gainL *= norm;
        tap.gainR *= norm;
        tap.gainMono *= norm;
    }
}

//==============================================================================
// reset / resetStep (§6.3)
//==============================================================================
void ReverbEngine::clearAll()
{
    std::fill (pool.begin(), pool.end(), 0.0f);
}

void ReverbEngine::resetState()
{
    tankWrite = apWrite = preWrite = erWrite = revWrite = 0;

    Lcg seeds (kSeedWalk);

    for (int i = 0; i < kNumLines; ++i)
    {
        auto& line = lines[(size_t) i];
        line.sLo = line.sHi = 0.0f;
        line.colorLp = 0.0f;
        line.clipPrev = 0.0f;
        line.walkPos = line.walkInc = line.walkTarget = 0.0f;
        line.walkCount = 0;
        line.rng = seeds.next() | 1u;
    }

    for (int k = 0; k < kNumWalkers; ++k)
    {
        auto& w = walkers[(size_t) k];
        w.pos = w.inc = 0.0f;
        w.count = 0;
        w.rng = seeds.next() | 1u;
    }

    commonRng = seeds.next() | 1u;
    commonTarget = 0.0f;
    commonCount = 0;

    layerBCos = 1.0f;
    layerBSin = 0.0f;

    // §3.6 Layer C rotators restart from fixed quarter-turn phases — exact constants,
    // not trig, so two renders stay bit-identical (§6.4). The four pairs decorrelate
    // through their incommensurate rates; the phases just keep them from starting in
    // step.
    rotCos[0] = 1.0f;  rotSin[0] = 0.0f;
    rotCos[1] = 0.0f;  rotSin[1] = 1.0f;
    rotCos[2] = -1.0f; rotSin[2] = 0.0f;
    rotCos[3] = 0.0f;  rotSin[3] = -1.0f;

    for (int ch = 0; ch < 2; ++ch)
    {
        dcInState[ch] = dcOutState[ch] = 0.0f;
        sendHpState[ch] = sendLpState[ch] = 0.0f;
        tiltLpState[ch] = 0.0f;
        tcLoState[ch] = tcHiState[ch] = 0.0f;

        for (auto& ap : diffuser[(size_t) ch])  ap.write = 0;
        for (auto& ap : erAllpass[(size_t) ch]) ap.write = 0;
        for (auto& ap : revAllpass[(size_t) ch]) ap.write = 0;
        for (auto& s : spread[(size_t) ch])     s.clear();
    }

    revColorLp = 0.0f;
    duckEnv = 0.0f;

    // §3.9 Spring. The delay lines are tankBuf[0..2] and were cleared above with the rest
    // of the pool; what is left is the dispersion state (2.8 KB per spring at 48 kHz, well
    // inside §6.3's per-step bound) and the loop's own one-poles.
    for (auto& s : springs)
        s.clear();

    for (int s = 0; s < kNumSprings; ++s)
        springHpState[s] = 0.0f;

    for (int ch = 0; ch < 2; ++ch)
        dwellPreState[ch] = dwellDeState[ch] = 0.0f;

    dwellDrive.inc = 0.0f;

    // §3.11 Shimmer. The shifter's line is not part of `pool` (see layOutBuffers), so it is
    // cleared here — 13 KB at 48 kHz, an order under the largest existing resetStep() chunk
    // — and its LCG is re-seeded so two renders stay bit-identical (§6.4).
    shifter.clear();
    shimLpState = 0.0f;
    shimmerGain.snap (0.0f);
    shimmerActive = false;

    preDelayOld = preDelayCur = preDelayTarget;
    xfadePos = 1.0f;
    sizeScaleCur = sizeScaleTarget;

    wetFade.snap (wetFadeEnd);

    for (auto* s : { &mixWet, &widthAmount, &erLevelAmount, &tiltLowGain, &tiltHighGain, &duckAmount })
        s->setCurrentAndTargetValue (s->getTargetValue());

    // Ramped coefficients restart from their targets; the next block re-derives them.
    for (auto& line : lines)
    {
        line.lineDelay.inc = line.apDelay.inc = 0.0f;
        line.gMid.inc = line.gLo.inc = line.wLo.inc = line.wHi.inc = 0.0f;
    }

    tcLoGain.inc = tcHiGain.inc = 0.0f;
    injectGain.inc = 0.0f;
}

void ReverbEngine::reset()
{
    if (! prepared)
        return;

    clearAll();
    resetState();
    clearStep = 0;
}

bool ReverbEngine::resetStep()
{
    if (! prepared)
        return true;

    // Steps 0..7 clear one tank line (and its in-loop allpass) each — 276 KB at 192 kHz,
    // the bound §6.3 sets. Step 8 sweeps the small front-end buffers. Steps 9..12 clear
    // the Reverse window line in tank-line-sized quarters.
    constexpr int kReverseChunks = 4;
    constexpr int kNumSteps = kNumLines + 1 + kReverseChunks;

    const int step = clearStep;

    if (step < kNumLines)
    {
        std::fill (tankBuf[(size_t) step], tankBuf[(size_t) step] + tankLen + kGuard, 0.0f);
        std::fill (apBuf[(size_t) step], apBuf[(size_t) step] + apLen + kGuard, 0.0f);
    }
    else if (step == kNumLines)
    {
        std::fill (preBuf[0], preBuf[0] + preLen + kGuard, 0.0f);
        std::fill (preBuf[1], preBuf[1] + preLen + kGuard, 0.0f);
        std::fill (erBuf, erBuf + erLen + kGuard, 0.0f);

        for (int ch = 0; ch < 2; ++ch)
        {
            for (auto& ap : diffuser[(size_t) ch])   std::fill (ap.buf, ap.buf + ap.len + kGuard, 0.0f);
            for (auto& ap : erAllpass[(size_t) ch])  std::fill (ap.buf, ap.buf + ap.len + kGuard, 0.0f);
            for (auto& ap : revAllpass[(size_t) ch]) std::fill (ap.buf, ap.buf + ap.len + kGuard, 0.0f);
        }
    }
    else
    {
        const int chunk = step - kNumLines - 1;
        const int total = revLen + kGuard;
        const int from = (int) ((std::int64_t) total * chunk / kReverseChunks);
        const int to   = (int) ((std::int64_t) total * (chunk + 1) / kReverseChunks);
        std::fill (revBuf + from, revBuf + to, 0.0f);
    }

    ++clearStep;

    if (clearStep >= kNumSteps)
    {
        clearStep = 0;
        resetState();
        return true;
    }

    return false;
}

//==============================================================================
// Parameters
//==============================================================================
void ReverbEngine::setParameters (const Params& p)
{
    // The configured mode is owned by configure(); `algo` is deliberately not read here
    // so a mode change can be deferred through the fade / pendingReset path of §6.2.
    current = p;
}

void ReverbEngine::setWetFade (float startGain, float endGain) noexcept
{
    wetFadeStart = juce::jlimit (0.0f, 1.0f, startGain);
    wetFadeEnd = juce::jlimit (0.0f, 1.0f, endGain);
}

void ReverbEngine::updateBlockRamps (int numSamples)
{
    const Params& p = current;
    const bool snap = p.snap;
    const float fs = (float) sampleRate;
    const float invN = 1.0f / (float) numSamples;
    const ModeSpec& spec = specFor (mode);

    // --- Size: slew-limited to 250 %/s of the mode's own range (§6.5) ------------
    sizeScaleTarget = sizeMin + juce::jlimit (0.0f, 1.0f, p.size01) * (sizeMax - sizeMin);

    if (snap)
    {
        sizeScaleCur = sizeScaleTarget;
    }
    else
    {
        const float maxStep = sizeSlewPerSample * (float) numSamples;
        sizeScaleCur += juce::jlimit (-maxStep, maxStep, sizeScaleTarget - sizeScaleCur);
    }

    // --- geometry ---------------------------------------------------------------
    const float rateScale = fs / 48000.0f;
    const float maxLine = (float) (tankLen - 5);
    const float maxAp = (float) (apLen - 5);
    float minLine = maxLine;
    float lineTarget[kNumLines], apTarget[kNumLines];

    for (int i = 0; i < kNumLines; ++i)
    {
        auto& line = lines[(size_t) i];

        // §3.9: Spring's three lines are specified in ms rather than in 48 kHz samples,
        // so they scale with fs directly. Lines 3..7 are idle in Spring; they are still
        // ramped to a legal value so nothing reads a stale target on the way back out.
        const float nominal = mode == modeSpring
                                  ? kSpringTdMs[(size_t) juce::jmin (i, kNumSprings - 1)] * 0.001f * fs
                                  : (float) spec.tank48[i] * rateScale;

        lineTarget[i] = juce::jlimit (4.0f, maxLine, nominal * sizeScaleCur);
        apTarget[i] = juce::jlimit (4.0f, maxAp, lineTarget[i] * line.apRatio);
        minLine = juce::jmin (minLine, lineTarget[i]);

        if (snap) { line.lineDelay.snap (lineTarget[i]); line.apDelay.snap (apTarget[i]); }
        else      { line.lineDelay.setTarget (lineTarget[i], invN); line.apDelay.setTarget (apTarget[i], invN); }
    }

    // --- §5.1 axis 4: interpolator degradation (AMENDED 2026-08-07) --------------
    // Solved BEFORE the Filt1 block, because the r_hi solve below divides by the
    // |Hi(f_h)|_mean of the kernel the loop reads through — at colour 0 the cubic
    // (table entry 0, the pre-axis-4 value to the bit), toward colour 1 the per-mode
    // blend. Leaving the compensation at the cubic while the read degrades would be
    // exactly the compounding the kColorLpTopHz comment documents at −39.3 %.
    const float color = juce::jlimit (0.0f, 1.0f, p.color01);
    colorInterp = colorInterpTop * color;

    {
        const float pos = colorInterp * 16.0f;
        const int i0 = juce::jlimit (0, 15, (int) pos);
        const float f = pos - (float) i0;
        interpMagAtHf = interpMagTable[(size_t) i0]
                        + f * (interpMagTable[(size_t) i0 + 1] - interpMagTable[(size_t) i0]);
    }

    // --- §3.4 Jot damping, zita Filt1 in ratio form -----------------------------
    const float decay = juce::jlimit (0.2f, spec.decayCeiling, p.decayS);
    const float t60mid = decay;
    const float t60low = juce::jmax (0.05f, decay * juce::jlimit (0.25f, 4.0f, p.bassMult));
    const float t60high = juce::jmax (0.05f,
                                      juce::jmin (decay * (1.25f - juce::jlimit (0.0f, 1.0f, p.damping01)),
                                                  hfMaxT60));
    const float wLoTarget = kTwoPi * bassXoverHz / fs;
    const float chi = kTwoPi * hfXoverHz / fs;

    // The loop gain §3.11's shimmer bound is solved against. Tracked as the MAX over the
    // lines (the shortest line always carries the largest g), which is the conservative
    // end of the set.
    float gMidMax = 0.0f;

    // §3.2 decay-normalised injection: the PER-PASS loop gain 10^(-3*D/T60) the
    // k_in/sqrt(1-g^2) RMS law is stated on, also max-over-lines. In the tank it IS
    // gMid; in Spring it is gMid before the /loopLoss compensation — that division
    // carries what the in-loop filters take out again per pass, so the level law never
    // sees it (which is also why gMidMax itself is the wrong g here: at long decays it
    // saturates toward the 0.9999 cap and would over-quiet the springs by several dB).
    float gLoopMax = 0.0f;

    // --- §3.9 Spring: fixed circuit filters, NOT the Jot solve ------------------
    // The zita solve assumes the loop delay is the same at every frequency. In a spring
    // it emphatically is not: the dispersion's group delay runs 2.70 ms at DC to ~52 ms at
    // the chirp knee at 48 kHz, a factor of 19, so a per-band T60 solve driven by a single
    // D would be wrong by that factor in the band the mode exists for. §3.9 asks for the
    // real circuit instead — an in-loop LP, an in-loop HP and one loop gain.
    if (mode == modeSpring)
    {
        const float lpHz = hfXoverHz * std::pow (0.5f, kSpringDampOctaves
                                                           * juce::jlimit (0.0f, 1.0f, p.damping01));
        const float wHiTarget = onePoleW (lpHz, sampleRate);

        // reverb_decay pins the T60 of the loop's PASSBAND CENTRE — the geometric mean of
        // the two in-loop corners — rather than of DC, so the loop gain has to carry what
        // those filters take out per pulse. Solving at DC instead (the naive form) was
        // measured 14.7 % short at 500 Hz, 26.2 % at 1 kHz and 40 % broadband against the
        // knob, because a 2.8 kHz one-pole is still -0.5 dB at 1 kHz and that loss
        // compounds ~18 times a second. The centre is also within 0.2 % of where the loop
        // gain peaks, so the compensation cannot make ANY band ring longer than the knob
        // asks — and it tracks the damped corner, which keeps reverb_damping a tilt
        // control (as it is in every other mode) rather than a second decay control.
        const float refHz = std::sqrt (kSpringHpHz * lpHz);
        const float loopLoss = juce::jmax (0.05f,
                                           onePoleMagnitude (wHiTarget, refHz, sampleRate, false)
                                               * onePoleMagnitude (springHpW, refHz, sampleRate, true));

        for (int s = 0; s < kNumSprings; ++s)
        {
            auto& line = lines[(size_t) s];

            // The loop period at low frequency: the spring plus the cascade's DC delay.
            // §3.9's "loop gain ~ -5.4 dB/pulse" is exactly what the standard T60 law
            // gives here at T60 = 0.60 s (60 * 0.0537 / 0.60 = 5.37 dB with the shortest
            // spring at 48 kHz), so it is honoured as the character of the knob's low end
            // rather than hard-wired — a fixed -5.4 dB would make §4.1's 4.0 s decay
            // ceiling unreachable and leave reverb_decay inert.
            const float d = (lineTarget[s] + springLfDelay) / fs;
            const float gLoop = juce::jmin (0.9999f, std::pow (0.001f, d / t60mid));
            const float gMid = juce::jmin (0.9999f, gLoop / loopLoss);
            gMidMax = juce::jmax (gMidMax, gMid);
            gLoopMax = juce::jmax (gLoopMax, gLoop);

            // reverb_bassmult rides the same low shelf as the tank, at §4.1's 250 Hz.
            const float gLo = std::pow (0.001f, d / t60low) / gMid - 1.0f;

            if (snap)
            {
                line.gMid.snap (gMid); line.gLo.snap (gLo);
                line.wLo.snap (wLoTarget); line.wHi.snap (wHiTarget);
            }
            else
            {
                line.gMid.setTarget (gMid, invN); line.gLo.setTarget (gLo, invN);
                line.wLo.setTarget (wLoTarget, invN); line.wHi.setTarget (wHiTarget, invN);
            }
        }
    }

    else
    {
        for (int i = 0; i < kNumLines; ++i)
        {
            auto& line = lines[(size_t) i];

            // D_i is the total loop delay: line plus its allpass (zita's scalar
            // approximation — which is why apG is capped at 0.60 in §3.5).
            const float d = (lineTarget[i] + apTarget[i]) / fs;

            // §6.6 keeps an explicit loop-gain ceiling against float drift.
            const float gMid = juce::jmin (0.9999f, std::pow (0.001f, d / t60mid));
            gMidMax = juce::jmax (gMidMax, gMid);
            gLoopMax = gMidMax;      // no in-loop filter compensation in the tank solve
            const float gLo = std::pow (0.001f, d / t60low) / gMid - 1.0f;
            const float rHi = juce::jlimit (1.0e-4f, 1.0f - 1.0e-4f,
                                            std::pow (0.001f, d / t60high) / (gMid * interpMagAtHf));
            const float t = (1.0f - rHi * rHi) / (2.0f * rHi * rHi * chi);
            const float wHi = t > 1.0e-9f ? (std::sqrt (1.0f + 4.0f * t) - 1.0f) / (2.0f * t) : 1.0f;

            if (snap)
            {
                line.gMid.snap (gMid); line.gLo.snap (gLo);
                line.wLo.snap (wLoTarget); line.wHi.snap (juce::jlimit (1.0e-5f, 1.0f, wHi));
            }
            else
            {
                line.gMid.setTarget (gMid, invN); line.gLo.setTarget (gLo, invN);
                line.wLo.setTarget (wLoTarget, invN);
                line.wHi.setTarget (juce::jlimit (1.0e-5f, 1.0f, wHi), invN);
            }
        }
    }

    // --- §3.2 decay-normalised injection (AMENDED 2026-08-07) -------------------
    // Steady-state line RMS is k_in/sqrt(1-g^2) (the law kSpringOutGain is derived from),
    // and g runs ~0.86 at Room's 0.2 s floor to the 0.9999 cap at Hall's 10 s ceiling —
    // over 20 dB of internal level swing driven by a knob that is supposed to set a TIME.
    // Two consequences the engine cared about: the wet output level rides Decay (against
    // the constant-tail law kErToOutput already holds for Early Energy), and the distance
    // to §3.5's ABSOLUTE 0.8 clip knee rides it too, so in-loop distortion onset is a
    // hidden function of Decay that §8's single-point nonlinearity row never sweeps.
    // Dividing the injection by sqrt(1-g^2) relative to the reference gRef (§3.2's
    // buildModeTables, the 1.5 s pivot) makes line RMS — hence both — decay-invariant.
    // The [0.25, 2.0] clamp keeps a 0.2 s Room from being driven hot into the same knee
    // from the other side. Reverse never reads the ramp (nothing recirculates, and its
    // tap normalisation is already window-invariant, §3.12), so computing it from Hall's
    // borrowed columns there is harmless.
    const float injectScale = juce::jlimit (0.25f, 2.0f,
        std::sqrt (juce::jmax (1.0e-6f, 1.0f - gLoopMax * gLoopMax))
            / std::sqrt (juce::jmax (1.0e-6f, 1.0f - gRef * gRef)));

    if (snap) injectGain.snap (injectScale);
    else      injectGain.setTarget (injectScale, invN);

    // --- §3.4 tonal correction (AMENDED 2026-08-07) -----------------------------
    // The tail's steady-state energy per band is proportional to that band's T60, so the
    // solve above makes bassmult ×4 sit ~+6 dB hotter in the low band and damping = 1
    // ~-5 dB duller in the high band ON TOP of the decay-TIME change the knob asks for.
    // Two wet-bus shelves (outputBus) hand the LEVEL back: amplitude ∝ √T60, hence the
    // 0.5 in the exponent. The HF gain is solved against the NEUTRAL-KNOB reference
    // t60highRef — t60high at damping's 0.5 default — rather than against t60mid, so the
    // hfMaxT60 air-absorption ceiling cancels between numerator and denominator: the
    // default preset stays neutral, the correction can never fight the ceiling with a
    // boost, and the [0.35, 2.8] clamp is a safety net rather than the operating point.
    // Spring/Reverse (tonalCorrect 0) never reach this: their back ends are not this
    // solve, and outputBus skips the shelves outright to keep their renders bit-identical.
    if (tonalCorrect > 0.0f)
    {
        const float t60highRef = juce::jmax (0.05f, juce::jmin (decay * 0.75f, hfMaxT60));
        const float gCorrLow = juce::jlimit (0.35f, 2.8f,
                                             std::pow (t60mid / t60low, 0.5f * tonalCorrect));
        const float gCorrHigh = juce::jlimit (0.35f, 2.8f,
                                              std::pow (t60highRef / t60high, 0.5f * tonalCorrect));

        if (snap) { tcLoGain.snap (gCorrLow); tcHiGain.snap (gCorrHigh); }
        else      { tcLoGain.setTarget (gCorrLow, invN); tcHiGain.setTarget (gCorrHigh, invN); }
    }

    // --- §3.6 modulation --------------------------------------------------------
    const float m = juce::jlimit (0.0f, 1.0f, p.mod01);
    const float depthScale = juce::jmin (1.0f, m / kModDepthKnee);
    const float rateMul = m > kModDepthKnee
                              ? 1.0f + (m - kModDepthKnee) / (1.0f - kModDepthKnee) * (kModRateTopMultiplier - 1.0f)
                              : 1.0f;
    const float rateHz = juce::jmax (0.01f, modRateHz * rateMul);

    if (mode == modeReverse)
    {
        // §3.12: 8 shared walkers, depth scaled by W/1.5 — the 1.5/T60 compounding clamp
        // is inapplicable because nothing recirculates.
        float depth = kRevModDepthMs * 0.001f * fs * depthScale * (windowSeconds / kRevWindowMax);
        depth = juce::jmin (depth, juce::jmax (0.0f, ((float) revLen - 8.0f) * 0.5f));

        for (int k = 0; k < kNumWalkers; ++k)
        {
            auto& w = walkers[(size_t) k];
            w.depth = depth * w.depthJitter;
            w.segment = juce::jmax (1, (int) (fs / (rateHz * w.rateJitter)));
        }
    }
    else
    {
        // Both depth laws of §3.6: a fixed sample depth is a larger fractional detune on
        // a short line, and detuning compounds once per loop pass.
        const float t60clamp = juce::jlimit (0.25f, 1.0f, 1.5f / t60mid);
        float depth = modDepthMs * 0.001f * fs * depthScale * sizeScaleCur * t60clamp;

        // Negative-delay guard #1, at parameter-set time (the ValhallaRoom 1.0.8 bug).
        depth = juce::jmin (depth, juce::jmax (0.0f, (minLine - 4.0f) * 0.5f));

        for (int i = 0; i < kNumLines; ++i)
        {
            auto& line = lines[(size_t) i];
            line.walkDepth = juce::jmin (depth * line.depthJitter,
                                         juce::jmax (0.0f, (lineTarget[i] - 4.0f) * 0.5f));
            line.walkSegment = juce::jmax (1, (int) (fs / (rateHz * line.rateJitter)));

            // Layer B is disabled per line whenever the allpass is shorter than 15 ms.
            line.layerB = apTarget[i] >= kLayerBMinAllpassSeconds * fs;
            line.depthB = juce::jmin (kLayerBDepthFraction * line.walkDepth,
                                      juce::jmax (0.0f, (apTarget[i] - 4.0f) * 0.5f));
        }
    }

    commonSegment = juce::jmax (1, (int) (fs / rateHz));

    // Shared Layer-B rotator: half Layer A's rate, phases 2*pi*k/8.
    const float wB = kTwoPi * (0.5f * rateHz) / fs;
    layerBIncCos = std::cos (wB);
    layerBIncSin = std::sin (wB);

    // §3.6 Layer C: the Givens layer's depth and pair rates. The depth rides the same
    // depthScale as Layers A and B, so one knob drives all three and reverb_mod = 0
    // zeroes the layer exactly (processTank skips it outright at rotTheta 0). Unlike
    // the delay layers there is nothing else to solve: no negative-delay guard, no
    // t60clamp — a rotation is norm-preserving at any angle at any rate, which is the
    // whole reason the layer exists (§3.6).
    rotTheta = rotMax * depthScale;

    for (int p = 0; p < 4; ++p)
    {
        const float wR = kTwoPi * (kRotRateMul[p] * rateHz) / fs;
        rotIncCos[p] = std::cos (wR);
        rotIncSin[p] = std::sin (wR);
    }

    // --- §5.1 colour axes 1-3 (axis 4 is solved above the Filt1 block) ----------
    // Bottom corner and inSat top are the per-mode era columns (AMENDED 2026-08-07);
    // the 18 kHz top corner stays global, so colour 0 is unchanged in every mode.
    inSat = inSatBase + color * (colorInSatTop - inSatBase);
    colorLpW = onePoleW (kColorLpTopHz * std::pow (colorLpBottomHz / kColorLpTopHz, color), sampleRate);
    colorDepth = color;
    revColorW = colorLpW;
    colorRho = kColorMaxRho * color;
    colorRhoNorm = 1.0f / std::sqrt ((1.0f - colorRho) * (1.0f - colorRho) + colorRho * colorRho);
    colorQuant = color;

    // --- §3.11 Shimmer ----------------------------------------------------------
    // The gain constraint is `shimmerGain <= alpha*(1 - g)` with alpha = 0.5, and it is
    // applied as a SCALE on the knob rather than as a clip of it:
    //
    //     shimmerGain = reverb_shimmer * alpha * (1 - g)
    //
    // which satisfies the inequality for every knob position by construction. Two
    // deliberate readings of §3.11, both worth stating because neither is the literal
    // sentence.
    //
    // (a) `g` is the LIVE mid-band loop gain, re-solved every block, not a constant
    //     frozen at prepare(). §3.11 says "solved at prepare from the mode's loop gain",
    //     but `g` is a function of reverb_decay, which is a live parameter: at Hall's
    //     0.2 s floor g = 0.003 and at its 10 s ceiling g = 0.891, a bound of 0.498
    //     against 0.054. Freezing it would have to freeze it at the mode's ceiling to
    //     stay safe, which is the 0.054 figure at EVERY decay setting — a knob that does
    //     almost nothing for the whole lower half of the decay range. Re-solving per
    //     block is strictly as safe (the bound holds at the g actually in force) and
    //     costs one max() in a loop that already computes every g.
    // (b) Scaling rather than clipping, for §5.2's rule: a clip would leave the knob's
    //     top 90 % dead at long decays, and "a silently inert knob is worse than a greyed
    //     one". Scaling keeps it monotonic and live everywhere, and zero stays exactly
    //     zero, which is what makes the mode bit-inert at reverb_shimmer = 0.
    //
    // What the bound buys: the Regen tap is read at line level and injected back at line
    // level (see processTank), and the shifter cannot exceed unity — the four Hann windows
    // sum to exactly 2 and the output is halved, with §3.11's hard limiter as the absolute
    // backstop — so a line's own g-weighted recirculation gains at most shimmerGain per
    // sample and the total is g + alpha*(1-g) = 0.5*(1+g) < 1 for every g the engine can
    // reach. Measured over §7 Stage 3's grid (4 intervals x decay 0.2/1/4/10 s x shimmer
    // 0.25/0.5/1.0, 10 s of noise then 30 s of silence at 48 kHz): worst peak 0.217
    // against the 1.5 bound, every tail monotonically decaying, and the 2-4 kHz band —
    // the many-to-one runaway a peak test would miss — converging to within +0.03 dB
    // window to window against a +1 dB bound.
    const float shimmerKnob = mode == modeShimmer ? juce::jlimit (0.0f, 1.0f, p.shimmer01) : 0.0f;
    const float shimmerTarget = shimmerKnob * kShimmerAlpha * juce::jmax (0.0f, 1.0f - gMidMax);

    // Only processTank() advances the ramp, so the two back ends that do not run it must
    // not be able to leave `shimmerActive` latched: they snap the gain instead of ramping
    // it, which costs nothing (they were never mixing the shifter in) and keeps the
    // invariant local rather than resting on "a mode change always resets".
    if (mode == modeReverse || mode == modeSpring)
        shimmerGain.snap (0.0f);

    // A completed ramp-down has to land on EXACTLY zero: `shimmerActive` below is a
    // bit-inertness gate, not a level test, so a 1e-9 float residual would keep the
    // shifter running (and the write-back off its untouched form) forever.
    else if (shimmerTarget <= 0.0f && std::abs (shimmerGain.cur) < 1.0e-6f)
        shimmerGain.snap (0.0f);

    // The shifter is touched only while the level is live, so the mode costs nothing at
    // zero and every other mode costs nothing at all. Crossing back into life re-arms the
    // warm-up that keeps the stale line from replaying (PitchShifter::activate).
    const bool wasActive = shimmerActive;
    shimmerActive = shimmerTarget > 0.0f || shimmerGain.cur > 0.0f;

    if (shimmerActive)
    {
        shifter.setRatio (PitchShifter::ratioFor (p.shimmerInterval));

        if (! wasActive)
            shifter.activate();
    }

    if (snap) shimmerGain.snap (shimmerTarget);
    else      shimmerGain.setTarget (shimmerTarget, invN);

    // --- front end --------------------------------------------------------------
    const float dcW = onePoleW (kInputDcHz, sampleRate);
    const float hpW = onePoleW (juce::jlimit (20.0f, 800.0f, p.lowCutHz), sampleRate);
    const float lpW = onePoleW (juce::jlimit (1200.0f, 20000.0f, p.highCutHz), sampleRate);
    const float diffAmt = 0.15f + 0.85f * juce::jlimit (0.0f, 1.0f, p.diffusion01);

    // §5.2: in Spring, reverb_diffusion is DWELL — the drive into §3.9's tanh — and §4.1's
    // inSat base of 0.50 is its depth, so the colour axis widens it with every other
    // saturation in the engine (0.50 -> 0.60, i.e. drive 6.5 -> 7.6 at DWELL = 1). The
    // ramp is kept warm in every mode; only processSpring() reads it.
    const float driveTarget = 1.0f + kSpringDwellDepth * inSat * juce::jlimit (0.0f, 1.0f, p.diffusion01);

    if (snap)
    {
        dcInW.snap (dcW); sendHpW.snap (hpW); sendLpW.snap (lpW); diffAmount.snap (diffAmt);
        dwellDrive.snap (driveTarget);
    }
    else
    {
        dcInW.setTarget (dcW, invN); sendHpW.setTarget (hpW, invN);
        sendLpW.setTarget (lpW, invN); diffAmount.setTarget (diffAmt, invN);
        dwellDrive.setTarget (driveTarget, invN);
    }

    for (int k = 0; k < kNumDiffusers; ++k)
    {
        const float d = juce::jlimit (4.0f, (float) (diffuser[0][(size_t) k].len - 5),
                                      kDiffuserMs[k] * 0.001f * fs * sizeScaleCur);

        for (int ch = 0; ch < 2; ++ch)
        {
            auto& ap = diffuser[(size_t) ch][(size_t) k];
            if (snap) ap.delay.snap (d); else ap.delay.setTarget (d, invN);
        }
    }

    for (int j = 0; j < 2; ++j)
    {
        const float d = juce::jlimit (4.0f, (float) (erAllpass[0][(size_t) j].len - 5),
                                      kErApMs[j] * 0.001f * fs * sizeScaleCur);

        for (int ch = 0; ch < 2; ++ch)
        {
            auto& ap = erAllpass[(size_t) ch][(size_t) j];
            if (snap) ap.delay.snap (d); else ap.delay.setTarget (d, invN);
        }
    }

    for (int ch = 0; ch < 2; ++ch)
        for (int j = 0; j < 2; ++j)
        {
            auto& ap = revAllpass[(size_t) ch][(size_t) j];
            const float d = juce::jlimit (4.0f, (float) (ap.len - 5),
                                          kRevApMs[ch][j] * 0.001f * fs * sizeScaleCur);
            if (snap) ap.delay.snap (d); else ap.delay.setTarget (d, invN);
        }

    // --- pre-delay, dual law (§6.5) ---------------------------------------------
    preDelayTarget = juce::jlimit (3.0f, (float) (preLen - 5),
                                   juce::jlimit (0.0f, 250.0f, p.predelayMs) * 0.001f * fs);

    if (snap)
    {
        preDelayCur = preDelayOld = preDelayTarget;
        xfadePos = 1.0f;
    }
    else if (xfadePos >= 1.0f && std::abs (preDelayTarget - preDelayCur) >= kPredelayJumpSeconds * fs)
    {
        // Equal-power crossfade for jumps >= 5 ms. Below that the two reads are highly
        // correlated, so a crossfade would give +3 dB and a swept comb — hence the slew.
        preDelayOld = preDelayCur;
        preDelayCur = preDelayTarget;
        xfadePos = 0.0f;
    }

    // --- gains (§6.5, 20 ms) ----------------------------------------------------
    const float tilt = juce::jlimit (-1.0f, 1.0f, p.tilt);
    mixWet.setTargetValue (juce::jlimit (0.0f, 1.0f, p.mix01));
    widthAmount.setTargetValue (juce::jlimit (0.0f, 1.0f, p.width01));
    erLevelAmount.setTargetValue (juce::jlimit (0.0f, 1.0f, p.erLevel01));
    tiltLowGain.setTargetValue (juce::Decibels::decibelsToGain (-kTiltDb * tilt));
    tiltHighGain.setTargetValue (juce::Decibels::decibelsToGain (kTiltDb * tilt));
    duckAmount.setTargetValue (juce::jlimit (0.0f, 1.0f, p.duck01));

    if (snap)
        for (auto* s : { &mixWet, &widthAmount, &erLevelAmount, &tiltLowGain, &tiltHighGain, &duckAmount })
            s->setCurrentAndTargetValue (s->getTargetValue());

    wetFade.cur = wetFadeStart;
    wetFade.setTarget (wetFadeEnd, invN);
}

//==============================================================================
// Per-sample helpers
//==============================================================================
float ReverbEngine::processAllpass (Allpass& ap, float x, float g) noexcept
{
    const float d = juce::jlimit (3.0f, (float) (ap.len - 5), ap.delay.next());
    const float delayed = readCubic (ap.buf, ap.len, ap.write, d);
    const float v = x + g * delayed;
    writeGuarded (ap.buf, ap.len, ap.write, v);
    ap.write = advanceWrite (ap.write, ap.len);
    return delayed - g * v;
}

void ReverbEngine::advanceWalk (LineState& line) noexcept
{
    if (--line.walkCount <= 0)
    {
        const float own = 2.0f * nextUniform (line.rng) - 1.0f;

        // §5.1 axis 3: correlate the per-line streams (rho 0 -> 0.5) and quantise the
        // target to 1/32-sample steps. At rho = 0 this is exactly the raw stream.
        float target = colorRhoNorm * ((1.0f - colorRho) * own + colorRho * commonTarget)
                       * line.walkDepth;
        const float quantised = std::round (target * kColorQuantStep) * (1.0f / kColorQuantStep);
        target += colorQuant * (quantised - target);

        line.walkTarget = target;
        line.walkCount = line.walkSegment;
        line.walkInc = (target - line.walkPos) / (float) line.walkSegment;
    }

    line.walkPos += line.walkInc;
}

void ReverbEngine::advanceWalker (Walker& w) noexcept
{
    if (--w.count <= 0)
    {
        const float own = 2.0f * nextUniform (w.rng) - 1.0f;
        float target = colorRhoNorm * ((1.0f - colorRho) * own + colorRho * commonTarget) * w.depth;
        const float quantised = std::round (target * kColorQuantStep) * (1.0f / kColorQuantStep);
        target += colorQuant * (quantised - target);

        w.count = w.segment;
        w.inc = (target - w.pos) / (float) w.segment;
    }

    w.pos += w.inc;
}

void ReverbEngine::frontEnd (float inL, float inR, float& a, float& b) noexcept
{
    const float wDc = dcInW.next();
    const float wHp = sendHpW.next();
    const float wLp = sendLpW.next();

    // §3.1 ducker sidechain: feed-forward from the DRY send, no lookahead.
    const float sidechain = std::abs (0.5f * (inL + inR));
    duckEnv += (sidechain > duckEnv ? duckAttack : duckRelease) * (sidechain - duckEnv);

    // §3.2 injection crossfeed. L == R collapses to a == b.
    a = 0.70f * inL + 0.30f * inR;
    b = 0.70f * inR + 0.30f * inL;

    // Injection DC block, 18 Hz one-pole HP.
    dcInState[0] += wDc * (a - dcInState[0]); a -= dcInState[0];
    dcInState[1] += wDc * (b - dcInState[1]); b -= dcInState[1];

    // Send HP (reverb_lowcut) then send LP (reverb_highcut).
    sendHpState[0] += wHp * (a - sendHpState[0]); a -= sendHpState[0];
    sendHpState[1] += wHp * (b - sendHpState[1]); b -= sendHpState[1];
    sendLpState[0] += wLp * (a - sendLpState[0]); a = sendLpState[0];
    sendLpState[1] += wLp * (b - sendLpState[1]); b = sendLpState[1];

    // Pre-delay, Dattorro's order: after the send filters, before the diffusers.
    writeGuarded (preBuf[0], preLen, preWrite, a);
    writeGuarded (preBuf[1], preLen, preWrite, b);

    const float maxPre = (float) (preLen - 5);

    if (xfadePos >= 1.0f)
    {
        const float diff = preDelayTarget - preDelayCur;
        preDelayCur += juce::jlimit (-preSlewPerSample, preSlewPerSample, diff);
        const float d = juce::jlimit (3.0f, maxPre, preDelayCur);
        a = readCubic (preBuf[0], preLen, preWrite, d);
        b = readCubic (preBuf[1], preLen, preWrite, d);
    }
    else
    {
        xfadePos = juce::jmin (1.0f, xfadePos + xfadeInc);
        const float gNew = std::sqrt (xfadePos);
        const float gOld = std::sqrt (1.0f - xfadePos);
        const float dOld = juce::jlimit (3.0f, maxPre, preDelayOld);
        const float dNew = juce::jlimit (3.0f, maxPre, preDelayCur);
        a = gOld * readCubic (preBuf[0], preLen, preWrite, dOld)
            + gNew * readCubic (preBuf[0], preLen, preWrite, dNew);
        b = gOld * readCubic (preBuf[1], preLen, preWrite, dOld)
            + gNew * readCubic (preBuf[1], preLen, preWrite, dNew);
    }

    preWrite = advanceWrite (preWrite, preLen);
}

float ReverbEngine::dwell (float x, float& preState, float& deState, float drive) noexcept
{
    // §3.9 Dwell: pre-emphasis (+6 dB shelf at 2 kHz) -> tanh(drive*x) -> de-emphasis.
    // Driving the treble harder is the point: the harmonics are made BEFORE the dispersion
    // and get chirped along with everything else, which is why a real tank sounds the way
    // it does. The de-emphasis shelf is the inverse GAIN, not the exact inverse filter —
    // a first-order shelf's true inverse is a different first-order shelf, and the residual
    // is under 1 dB across the band, well below the harmonics the stage exists to make.
    preState += dwellShelfW * (x - preState);
    x = preState + (x - preState) * kSpringDwellShelfGain;

    // Normalised so the small-signal gain is exactly 1 at every drive: DWELL then changes
    // the character rather than the send level. The floor is not decoration — `drive` is
    // a divisor inside a feedback loop, and a NaN injected into a spring never leaves it.
    const float g = juce::jmax (1.0e-3f, drive);
    x = std::tanh (g * x) / g;

    deState += dwellShelfW * (x - deState);
    return deState + (x - deState) * (1.0f / kSpringDwellShelfGain);
}

void ReverbEngine::applyDiffusers (float& a, float& b) noexcept
{
    // 4 input diffusers, NEVER modulated (short allpasses slosh). Split out of frontEnd()
    // so that the Early Energy tap point (§3.1) is an explicit ordering decision at the
    // call site rather than an accident of where the loop happens to sit.
    const float diffAmt = diffAmount.next();

    for (int k = 0; k < kNumDiffusers; ++k)
    {
        const float g = inDiffG[(size_t) k] * diffAmt;
        a = processAllpass (diffuser[0][(size_t) k], a, g);
        b = processAllpass (diffuser[1][(size_t) k], b, g);
    }
}

void ReverbEngine::outputBus (float wetL, float wetR, float dryL, float dryR,
                              bool stereo, float& outL, float& outR) noexcept
{
    // Output DC block.
    dcOutState[0] += dcOutW * (wetL - dcOutState[0]); wetL -= dcOutState[0];
    dcOutState[1] += dcOutW * (wetR - dcOutState[1]); wetR -= dcOutState[1];

    // §3.4 tonal correction (AMENDED 2026-08-07): the per-band tail-LEVEL shelves, in the
    // tilt's shelf idiom, at the mode's own crossovers. SKIPPED — not run at unity — when
    // the mode's amount is 0: a one-pole shelf at gain 1 still computes state + (x - state),
    // which is not bit-transparent in float, and Spring/Reverse must render bit-identically
    // to a build without the filter (§3.12 gate 6 asserts damping/bassmult bit-inert there).
    // Caveat, recorded in §3.4: the ER sum is already on this bus, so the shelves rescale
    // Early Energy too, whose level does not follow the T60 energy law — acceptable because
    // the ref-solve keeps the correction small near the defaults.
    if (tonalCorrect > 0.0f)
    {
        const float gCorrLow = tcLoGain.next();
        const float gCorrHigh = tcHiGain.next();

        tcLoState[0] += tcLoW * (wetL - tcLoState[0]);
        wetL = tcLoState[0] * gCorrLow + (wetL - tcLoState[0]);
        tcLoState[1] += tcLoW * (wetR - tcLoState[1]);
        wetR = tcLoState[1] * gCorrLow + (wetR - tcLoState[1]);

        tcHiState[0] += tcHiW * (wetL - tcHiState[0]);
        wetL = tcHiState[0] + (wetL - tcHiState[0]) * gCorrHigh;
        tcHiState[1] += tcHiW * (wetR - tcHiState[1]);
        wetR = tcHiState[1] + (wetR - tcHiState[1]) * gCorrHigh;
    }

    // §3.8 frequency-dependent L/R spread (Plate and Hall only).
    if (spreadOn)
    {
        for (auto& s : spread[0]) wetL = s.process (wetL);
        for (auto& s : spread[1]) wetR = s.process (wetR);
    }

    // reverb_tilt: first-order shelving pair about the 800 Hz pivot.
    const float gLow = tiltLowGain.getNextValue();
    const float gHigh = tiltHighGain.getNextValue();
    tiltLpState[0] += tiltW * (wetL - tiltLpState[0]);
    wetL = tiltLpState[0] * gLow + (wetL - tiltLpState[0]) * gHigh;
    tiltLpState[1] += tiltW * (wetR - tiltLpState[1]);
    wetR = tiltLpState[1] * gLow + (wetR - tiltLpState[1]) * gHigh;

    // M/S width on the wet only, matching DelayFx's law. Ignored on a mono host.
    const float width = widthAmount.getNextValue();

    if (stereo)
    {
        const float mid = 0.5f * (wetL + wetR);
        const float side = 0.5f * (wetL - wetR) * width;
        wetL = mid + side;
        wetR = mid - side;
    }

    wetL = safetyLimit (wetL);
    wetR = safetyLimit (wetR);

    const float fade = wetFade.next();
    const float duck = duckAmount.getNextValue();
    const float duckGain = 1.0f / (1.0f + duck * kDuckDepth * duckEnv);
    const float mix = mixWet.getNextValue();
    const float wetGain = mix * fade * duckGain;

    // The mix law is preserved verbatim: dryLevel = 1 - wetLevel, not equal power.
    outL = dryL * (1.0f - mix) + wetL * wetGain;
    outR = dryR * (1.0f - mix) + wetR * wetGain;
}

//==============================================================================
// process
//==============================================================================
void ReverbEngine::process (juce::dsp::AudioBlock<float> block)
{
    const int numSamples = (int) block.getNumSamples();
    const int numChannels = (int) juce::jmin ((size_t) 2, block.getNumChannels());

    if (! prepared || numSamples <= 0 || numChannels <= 0)
        return;

    float* data[2] { block.getChannelPointer (0),
                     numChannels > 1 ? block.getChannelPointer (1) : nullptr };

    updateBlockRamps (numSamples);

    if (mode == modeReverse)
        processReverse (data, numChannels, numSamples);
    else if (mode == modeSpring)
        processSpring (data, numChannels, numSamples);
    else
        processTank (data, numChannels, numSamples);

    // The Layer-B rotator drifts off the unit circle over millions of samples.
    const float norm = 1.0f / std::sqrt (layerBCos * layerBCos + layerBSin * layerBSin + 1.0e-30f);
    layerBCos *= norm;
    layerBSin *= norm;

    // The Layer-C rotators drift the same way and get the same per-block fix. Their
    // drift is a depth/phase error only — the rotation each one drives is re-projected
    // onto the unit circle every sample by the (1,θ)·rsqrt form, so losslessness never
    // depended on this; it just keeps the modulation depth honest over hours.
    for (int p = 0; p < 4; ++p)
    {
        const float rNorm = 1.0f / std::sqrt (rotCos[p] * rotCos[p]
                                              + rotSin[p] * rotSin[p] + 1.0e-30f);
        rotCos[p] *= rNorm;
        rotSin[p] *= rNorm;
    }

    current.snap = false;
}

void ReverbEngine::processTank (float* const* data, int numChannels, int numSamples)
{
    const bool stereo = numChannels > 1;
    float* left = data[0];
    float* right = stereo ? data[1] : nullptr;

    for (int n = 0; n < numSamples; ++n)
    {
        const float dryL = left[n];
        const float dryR = stereo ? right[n] : dryL;

        float a = 0.0f, b = 0.0f;
        frontEnd (dryL, dryR, a, b);
        applyDiffusers (a, b);

        // --- §3.7 Early Energy ------------------------------------------------
        // DEVIATION, deliberate and measured: §3.1 draws EARLY ENERGY tapping the rail
        // between PREDELAY and the INPUT DIFFUSERS, so the tap line would be fed the
        // undiffused signal. It is fed the diffused signal instead. Both routings were
        // built and measured (Room, 44.1 / 48 / 96 kHz, the §8 rows):
        //
        //   feed             ER-only density @60 ms    full-IR 0.95 by    tail minimum
        //   post-diffuser    0.963 / 0.944 / 0.890     66.9 / 64 / 74 ms  0.933 / 0.948 / 0.931
        //   pre-diffuser     0.635 / 0.599 / 0.624     74.9 / 100 / 77 ms 0.907 / 0.880 / 0.887
        //
        // The pre-diffuser feed fails §8's ER-isolation row outright at every rate and
        // drags Room's headline echo-density rows under their thresholds with it — the
        // one defect §1.1 names as the reason this engine exists. What the tank sees is
        // identical either way: the diffuser cascade and the ER FIR are both LTI, so
        // diffuse(rail) + erToTank*ER(diffuse(rail)) = diffuse(rail + erToTank*ER(rail)).
        // The whole difference is whether the DIRECT wet ER carries the diffusers.
        //
        // §3.7's rationale (Costello: the Early section has no allpasses "which allows it
        // to avoid the coloration caused by short allpasses") is a claim about audible
        // ringing on pulse-like sources, and it is now measured rather than assumed: the
        // probe's ER-isolation section runs §8's spectral-flatness statistic on the ER
        // path alone and gates what the diffusers ADD to it as reverb_diffusion sweeps
        // 0 -> 1 (worst case Plate, +3.08 dB, against a 4 dB bound). The absolute figure
        // is the 24-tap FIR's own comb, which is why the increase is the gated quantity.
        const float erIn = 0.5f * (a + b);
        float erL = 0.0f, erR = 0.0f;

        for (int k = 0; k < kNumErTaps; ++k)
        {
            const auto& tap = erTaps[(size_t) k];
            int idx = erWrite - tap.delaySamples;
            if (idx < 0) idx += erLen;
            const float s = erBuf[idx];

            if (stereo) { erL += s * tap.gainL; erR += s * tap.gainR; }
            else        { erL += s * tap.gainMono; }
        }

        writeGuarded (erBuf, erLen, erWrite, erIn);
        erWrite = advanceWrite (erWrite, erLen);

        erL = processAllpass (erAllpass[0][0], erL, 0.5f);
        erL = processAllpass (erAllpass[0][1], erL, 0.5f);
        erR = processAllpass (erAllpass[1][0], erR, 0.5f);
        erR = processAllpass (erAllpass[1][1], erR, 0.5f);

        // Gardner: driving the tank from the FIR output raises tank-input density for
        // free. The tank gain is FIXED, so turning Early up never starves the tail.
        const float injA = a + erToTank * erL;
        const float injB = b + erToTank * erR;

        // --- shared modulation sources ----------------------------------------
        if (--commonCount <= 0)
        {
            commonTarget = 2.0f * nextUniform (commonRng) - 1.0f;
            commonCount = commonSegment;
        }

        const float bc = layerBCos, bs = layerBSin;
        layerBCos = bc * layerBIncCos - bs * layerBIncSin;
        layerBSin = bs * layerBIncCos + bc * layerBIncSin;

        // §3.6 Layer C rotators, advanced unconditionally like Layer B's so the phase
        // stays a deterministic function of the sample count whatever the knob does.
        for (int p = 0; p < 4; ++p)
        {
            const float rc = rotCos[p], rs = rotSin[p];
            rotCos[p] = rc * rotIncCos[p] - rs * rotIncSin[p];
            rotSin[p] = rs * rotIncCos[p] + rc * rotIncSin[p];
        }

        // --- §3.2 the tank ----------------------------------------------------
        float w[kNumLines];

        for (int i = 0; i < kNumLines; ++i)
        {
            auto& line = lines[(size_t) i];

            const float lineDelay = line.lineDelay.next();
            const float apDelay = line.apDelay.next();
            const float gMid = line.gMid.next();
            const float gLo = line.gLo.next();
            const float wLo = line.wLo.next();
            const float wHi = line.wHi.next();

            advanceWalk (line);

            // Negative-delay guard #2, at read time.
            const float d = juce::jlimit (3.0f, (float) (tankLen - 5), lineDelay + line.walkPos);
            float x = readCubic (tankBuf[(size_t) i], tankLen, tankWrite, d);

            // §5.1 axis 4 (AMENDED 2026-08-07): the era knob coarsens THIS read — the
            // Layer-A-modulated line read, where a moving fractional-delay LP is the
            // 224's own compounding halo — by fading it toward the 2-point kernel. The
            // blended read IS the blended 4-tap FIR, so the r_hi solve compensates it
            // exactly (see interpMagTable). The branch, not `+ 0.0f`, for the shimmer
            // ternary's reason: colour 0 must stay bit-identical to the cubic engine.
            // The in-loop allpass read below stays cubic DELIBERATELY: r_hi divides by
            // |Hi| once per pass, so one degraded crossing per pass is the exactly-
            // compensated configuration — degrading both would leave a second
            // linear-vs-cubic delta compounding per pass behind a squared-ratio model
            // stacked on the allpass's already-approximate scalar-D treatment (§3.5).
            if (colorInterp > 0.0f)
                x += colorInterp * (readLinear (tankBuf[(size_t) i], tankLen, tankWrite, d) - x);

            // Modulated Schroeder allpass INSIDE the loop — the thing Freeverb
            // structurally cannot do, so echo density grows with time.
            float am = apDelay;

            if (line.layerB)
                am += line.depthB * (bs * line.phaseCos + bc * line.phaseSin);

            am = juce::jlimit (3.0f, (float) (apLen - 5), am);
            const float delayed = readCubic (apBuf[(size_t) i], apLen, apWrite, am);
            const float v = x + line.apG * delayed;
            writeGuarded (apBuf[(size_t) i], apLen, apWrite, v);
            x = delayed - line.apG * v;

            // §3.4 zita Filt1: low shelf, HF one-pole, mid gain.
            line.sLo += wLo * (x - line.sLo);
            x += gLo * line.sLo;
            line.sHi += wHi * (x - line.sHi);
            x = gMid * line.sHi;

            // §5.1 axis 1: in-loop bandwidth ceiling, on top of the Jot HF band. The
            // filter is mixed in by `colorDepth` so that colour 0 is EXACTLY neutral —
            // see kColorLpTopHz for why an unconditional 18 kHz one-pole is not.
            line.colorLp += colorLpW * (x - line.colorLp);
            x += colorDepth * (line.colorLp - x);

            // §3.5 AMENDED 2026-08-07: ADAA'd — this clip recirculates, so every folded
            // harmonic re-enters the loop and re-aliases, ~300x at long decays. The two
            // single-pass clips (Spring's dwell, Reverse's window-line, §3.9 / §3.12)
            // stay plain.
            w[i] = softClipAdaa (x, line.clipPrev, inSat) + kStayNormal;
            line.clipPrev = x;
        }

        // --- output taps, read before the write-back --------------------------
        float tankL = 0.0f, tankR = 0.0f;

        for (int t = 0; t < kNumTankTaps; ++t)
        {
            const auto& tapL = kTapsL[t];
            const float dL = juce::jlimit (3.0f, (float) (tankLen - 5),
                                           tapL.frac * lines[(size_t) tapL.line].lineDelay.cur);
            tankL += tapL.sign * readLinear (tankBuf[(size_t) tapL.line], tankLen, tankWrite, dL);

            if (stereo)
            {
                const auto& tapR = kTapsR[t];
                const float dR = juce::jlimit (3.0f, (float) (tankLen - 5),
                                               tapR.frac * lines[(size_t) tapR.line].lineDelay.cur);
                tankR += tapR.sign * readLinear (tankBuf[(size_t) tapR.line], tankLen, tankWrite, dR);
            }
        }

        tankL *= kTankTapGain;
        tankR *= kTankTapGain;

        // --- normalized Hadamard: 12 butterflies, then the 1/sqrt(8) fold ------
        for (int i = 0; i < 8; i += 2) { const float u = w[i], t = w[i + 1]; w[i] = u + t; w[i + 1] = u - t; }
        for (int base = 0; base < 8; base += 4)
            for (int i = 0; i < 2; ++i) { const float u = w[base + i], t = w[base + i + 2]; w[base + i] = u + t; w[base + i + 2] = u - t; }
        for (int i = 0; i < 4; ++i) { const float u = w[i], t = w[i + 4]; w[i] = u + t; w[i + 4] = u - t; }

        // --- §3.11 Shimmer, Regen routing --------------------------------------
        // The tap is row 0 of the NORMALISED Hadamard, sum(w)/sqrt(8), which for a tank of
        // decorrelated lines sits at line level — so the shifter's gain and the bound of
        // §3.11 are stated against the same reference and need no fudge constant. The
        // return is injected with kInjectSign, whose eight entries sum to zero: the shifted
        // voice therefore cannot re-tap itself coherently on the very next sample, and the
        // energy it adds per circulation is the incoherent shimmerGain the bound assumes,
        // not sqrt(8) times it. The shifter's own minimum delay (4 samples) plus the line
        // delays keep the loop strictly causal.
        //
        // Input and Both — §3.11's other two routings — are feed-forward sends and are NOT
        // built: §5 declares no routing id, and a field with no parameter behind it is a
        // field nothing can set (§6.1's rule). Regen is the routing that carries the
        // hazard and the one the mode exists for.
        float shimInject = 0.0f;

        if (shimmerActive)
        {
            const float shifted = shifter.process (w[0] * kInvSqrt8);

            // LP first, THEN the hard limiter, so the bound applies to what actually
            // re-enters the loop rather than to a signal the filter still has to touch.
            shimLpState += shimLpW * (shifted - shimLpState);
            shimInject = shimmerGain.next()
                         * juce::jlimit (-kShimmerLimit, kShimmerLimit, shimLpState);
        }

        // --- §3.6 Layer C (AMENDED 2026-08-07): the Givens layer -----------------
        // Four 2x2 rotations on pairs (0,1)(2,3)(4,5)(6,7) between the butterflies and
        // the write-back, so the mixing matrix is R(t)·Â — time-varying but EXACTLY
        // orthogonal at every sample: (c,s) = (1,θ)·rsqrt(1+θ²) satisfies c²+s²=1 by
        // construction for any θ, so cond stays 1 and §3.4's solve keeps owning the
        // decay. This is modulation with no Doppler — the delays never move — which is
        // what lets Room run it at depth with none of Layer A's detune cost (§8's
        // pitch-stability row is the gate). Two placement invariants, both load-bearing:
        //   (a) the Shimmer tap above reads w[0] BEFORE this layer, so row 0 of the
        //       matrix the tap sees stays sum(w)/√8 — §3.11's gain bound and the
        //       Shimmer-renders-as-Hall-at-zero identity are stated against it;
        //   (b) the layer is skipped outright at rotTheta 0 rather than run at angle 0
        //       (the shimmer ternary's reasoning: -0.0f in, +0.0f out), so reverb_mod=0
        //       renders stay bit-identical to the static-Hadamard engine.
        if (rotTheta > 0.0f)
        {
            for (int p = 0; p < 4; ++p)
            {
                const float th = rotTheta * rotSin[p];
                const float rn = 1.0f / std::sqrt (1.0f + th * th);
                const float c = rn, s = th * rn;
                const float u = w[2 * p], t = w[2 * p + 1];
                w[2 * p]     = c * u - s * t;
                w[2 * p + 1] = s * u + c * t;
            }
        }

        // §3.2's decay normalisation, hoisted: the ramp advances once per SAMPLE, not
        // once per line, or eight blocks' worth of increment would land in every block.
        const float ig = injectGain.next();

        for (int i = 0; i < kNumLines; ++i)
        {
            const float inject = ig * kIn * kInjectSign[i] * ((i & 1) != 0 ? injB : injA);
            const float back = w[i] * kInvSqrt8 + inject;

            // The ternary rather than a `+ 0.0f`: the whole point of the gate is that a
            // non-Shimmer render is bit-identical, and -0.0f + 0.0f is +0.0f.
            writeGuarded (tankBuf[(size_t) i], tankLen, tankWrite,
                          shimmerActive ? back + kInjectSign[i] * shimInject : back);
        }

        tankWrite = advanceWrite (tankWrite, tankLen);
        apWrite = advanceWrite (apWrite, apLen);

        // --- wet bus: the constant-tail law, NOT a crossfade -------------------
        const float er = erLevelAmount.getNextValue();
        const float wetL = tankL + kErToOutput * er * erL;
        const float wetR = tankR + kErToOutput * er * erR;

        float outL = 0.0f, outR = 0.0f;
        outputBus (wetL, wetR, dryL, dryR, stereo, outL, outR);

        left[n] = outL;
        if (stereo) right[n] = outR;
    }
}

void ReverbEngine::processSpring (float* const* data, int numChannels, int numSamples)
{
    const bool stereo = numChannels > 1;
    float* left = data[0];
    float* right = stereo ? data[1] : nullptr;

    for (int n = 0; n < numSamples; ++n)
    {
        const float dryL = left[n];
        const float dryR = stereo ? right[n] : dryL;

        float a = 0.0f, b = 0.0f;
        frontEnd (dryL, dryR, a, b);

        // §3.1: in Spring the DISPERSION replaces the input diffusers, so applyDiffusers()
        // is not called, and the Early Energy section is idled (erToTank 0, §4.1) — the
        // ER bus is not built at all, exactly as in Reverse.
        const float drive = dwellDrive.next();
        a = dwell (a, dwellPreState[0], dwellDeState[0], drive);
        b = dwell (b, dwellPreState[1], dwellDeState[1], drive);

        // §3.2's decay normalisation rides Spring's injection too, and must: kSpringOutGain
        // was derived from the SAME k_in/sqrt(1-g^2) law assuming both back ends sit at
        // matched g, so scaling only the tank would break the level parity that trim
        // exists to hold. Hoisted above the three springs for the same reason as in the
        // tank — one .next() per sample.
        const float ig = injectGain.next();

        float wetL = 0.0f, wetR = 0.0f;

        for (int s = 0; s < kNumSprings; ++s)
        {
            auto& line = lines[(size_t) s];

            const float lineDelay = line.lineDelay.next();
            const float gMid = line.gMid.next();
            const float gLo = line.gLo.next();
            const float wLo = line.wLo.next();
            const float wHi = line.wHi.next();

            // The pickup, at the far end of the spring. Read before the write-back.
            const float d = juce::jlimit (3.0f, (float) (tankLen - 5), lineDelay);
            const float pickup = readCubic (tankBuf[(size_t) s], tankLen, tankWrite, d);

            // In-loop, per §3.9: the 250 Hz low shelf carrying reverb_bassmult, the
            // circuit's LP (2.8 kHz, darkened by reverb_damping) and the 90 Hz rumble HP.
            float x = pickup;
            line.sLo += wLo * (x - line.sLo);
            x += gLo * line.sLo;
            line.sHi += wHi * (x - line.sHi);
            x = line.sHi;
            springHpState[(size_t) s] += springHpW * (x - springHpState[(size_t) s]);
            x -= springHpState[(size_t) s];

            // §5.1 axis 1 rides the loop exactly as it does in the tank, and colour 0 is
            // EXACTLY neutral because the filter is mixed in by colorDepth.
            line.colorLp += colorLpW * (x - line.colorLp);
            x += colorDepth * (line.colorLp - x);

            x *= gMid;

            // §3.9 puts A_M(z) between the summing node and the line, so the input is
            // chirped once on the way in and once more per recirculation — the boing is
            // a sequence of progressively dispersed copies, not one dispersed copy
            // repeating. No in-loop soft clip: Spring's saturation is Dwell, ahead of the
            // dispersion (§4.1's inSat column reads "0.50 (Dwell)").
            const float inject = ig * kIn * kInjectSign[s] * ((s & 1) != 0 ? b : a);
            x = springs[(size_t) s].process (x + inject) + kStayNormal;

            writeGuarded (tankBuf[(size_t) s], tankLen, tankWrite, x);

            const float out = kSpringSign[s] * pickup;
            wetL += out * kSpringPanL[s];

            if (stereo)
                wetR += out * kSpringPanR[s];
        }

        tankWrite = advanceWrite (tankWrite, tankLen);

        wetL *= kSpringOutGain;
        wetR *= kSpringOutGain;

        // erLevel is advanced but never read: the ER section is idle in Spring, so the
        // smoothed value must still walk in step with every other block-rate gain.
        erLevelAmount.getNextValue();

        float outL = 0.0f, outR = 0.0f;
        outputBus (wetL, wetR, dryL, dryR, stereo, outL, outR);

        left[n] = outL;
        if (stereo) right[n] = outR;
    }
}

void ReverbEngine::processReverse (float* const* data, int numChannels, int numSamples)
{
    const bool stereo = numChannels > 1;
    float* left = data[0];
    float* right = stereo ? data[1] : nullptr;

    for (int n = 0; n < numSamples; ++n)
    {
        const float dryL = left[n];
        const float dryR = stereo ? right[n] : dryL;

        float a = 0.0f, b = 0.0f;
        frontEnd (dryL, dryR, a, b);
        applyDiffusers (a, b);

        // §3.12: one mono window line, written with 0.5*(a+b) from the diffuser output —
        // the even/odd a/b split is a tank concept.
        float x = 0.5f * (a + b);

        // §3.12 colour map: axis 1 becomes a one-pole LP on the window-line input,
        // axis 2 is pre-window saturation. Plain softClip, not the tank's ADAA form
        // (§3.5): this clip feeds a pure FIR tap table — its aliasing is rendered once,
        // never recirculated, which is the whole case for ADAA.
        revColorLp += revColorW * (x - revColorLp);
        x = softClip (x + colorDepth * (revColorLp - x), inSat);

        // Write first: this is a pure FIR and tap 0 sits at delay 0.
        writeGuarded (revBuf, revLen, revWrite, x);

        if (--commonCount <= 0)
        {
            commonTarget = 2.0f * nextUniform (commonRng) - 1.0f;
            commonCount = commonSegment;
        }

        for (int k = 0; k < kNumWalkers; ++k)
            advanceWalker (walkers[(size_t) k]);

        float wetL = 0.0f, wetR = 0.0f;

        for (int k = 0; k < kNumRevTaps; ++k)
        {
            const auto& tap = revTaps[(size_t) k];
            const float d = juce::jlimit (0.0f, (float) (revLen - 5),
                                          tap.delaySamples + walkers[(size_t) tap.walker].pos);
            const float s = readLinear (revBuf, revLen, revWrite, d);

            if (stereo) { wetL += s * tap.gainL; wetR += s * tap.gainR; }
            else        { wetL += s * tap.gainMono; }
        }

        revWrite = advanceWrite (revWrite, revLen);

        // Cross-tuned density allpasses: true allpasses, so they add ring, not comb.
        wetL = processAllpass (revAllpass[0][0], wetL, 0.5f);
        wetL = processAllpass (revAllpass[0][1], wetL, 0.5f);
        wetR = processAllpass (revAllpass[1][0], wetR, 0.5f);
        wetR = processAllpass (revAllpass[1][1], wetR, 0.5f);

        // erLevel is advanced but never read: damping, bassmult and erlevel are asserted
        // bit-inert in Reverse (§3.12 gate 6), so the ER bus is not built at all.
        erLevelAmount.getNextValue();

        float outL = 0.0f, outR = 0.0f;
        outputBus (wetL, wetR, dryL, dryR, stereo, outL, outR);

        left[n] = outL;
        if (stereo) right[n] = outR;
    }
}
} // namespace tubamp
