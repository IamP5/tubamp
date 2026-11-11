#include "Reverb.h"

#include "FxPrimitives.h"

#include <array>
#include <cmath>
#include <vector>

namespace tubamp
{
namespace
{
//==============================================================================
// Shared tuning (docs/REVERB-DELAY.md §2)

constexpr float kParamSmoothSeconds = 0.02f;
constexpr float kSizeGlideSeconds = 0.2f;     // delay lengths slew; a size move bends pitch briefly
constexpr float kPreDelayFadeSeconds = 0.03f; // pre-delay moves crossfade between taps
constexpr float kMaxPreDelaySeconds = 0.25f;
constexpr int kChunk = 256;                   // wet scratch size; larger host blocks are split

// The damping knob at 0, 0.5 and 1: treble decay as a fraction of the mid-band decay,
// and the crossover above which it applies.
constexpr float kDampingRatio[3] { 0.97f, 0.45f, 0.10f };
constexpr float kDampingHz[3] { 14000.0f, 7400.0f, 2000.0f };

/** What the damping knob means, the same in every type: how much faster the treble
    decays than the mids, and from what frequency up. 0 leaves the treble ringing almost
    as long as the mids; 1 has it gone in a tenth of the time, from 2 kHz up; the centre
    is a bright plate. Each type starts the knob where its kind of space sits
    (kReverbDampingDefault). Two geometric segments, so each half of the knob covers
    its own range evenly. */
struct Damping
{
    float highRatio, highHz;
};

Damping dampingFor (float damping01) noexcept
{
    const float d = 2.0f * juce::jlimit (0.0f, 1.0f, damping01);
    const int segment = d < 1.0f ? 0 : 1;
    const float t = d - (float) segment;

    return { kDampingRatio[segment] * std::pow (kDampingRatio[segment + 1] / kDampingRatio[segment], t),
             kDampingHz[segment] * std::pow (kDampingHz[segment + 1] / kDampingHz[segment], t) };
}

/** Tail modulation depth in samples for a mod knob position, given the depth at full
    scale in samples at 48 kHz. */
float modDepthFor (float mod01, float maxSamplesAt48k, float sampleRate) noexcept
{
    return maxSamplesAt48k * juce::jlimit (0.0f, 1.0f, mod01) * (sampleRate / 48000.0f);
}

//==============================================================================
/** The network behind Room, Hall, Plate and Ambient: a lattice of delays and rotations
    inside one feedback loop.

      a = L + loop,  b = R + loop      (the two delayed channels)
      c = L + loop,  d = R + loop      (two side channels that are never delayed)

      for each of 20 stages, shortest delays first:
          a, b  <- absorb (delay (a)), absorb (delay (b))     modulated on odd stages
          out   += tap * (a, b)                               alternating L/R, signed
          (a,b) <- rotate 45 degrees, sense alternating stage to stage
          (a,c), (b,d) <- rotate 45 degrees
      loop <- a, b, c, d

    What that buys, compared with a conventional feedback delay network:

    - No recirculation inside the chain. Each stage hands half the signal to a side
      channel that skips the delays that follow, so a path through the chain collects
      the delays of some subset of the stages: about a million distinct arrival times,
      none of them periodic. Echo density grows explosively without a single short
      comb — the usual source of a metallic ring.
    - A very long loop. The only feedback is around the whole chain, whose delays add up
      to seconds. A loop has as many resonances per hertz as it has seconds of delay, so
      they sit far closer together than the ear can resolve and the tail has the
      statistics of noise even with the modulation switched off.
    - A continuous build-up. The delays are one geometric series, so the burst that
      reaches each stage's tap is already longer than the gap to the next stage: early
      sound and late tail are the same process, with no seam to hide.
    - An exact decay. Every rotation is lossless, and each delay is followed by an
      absorption filter sized to its own length, so a path loses exactly as much as the
      time it took, per band, whatever route it followed.

    The taps decide what is heard of the early stages: starting the taps later, and
    fading them in over a few stages, moves the listener back from the source. */
struct LatticeVoicing
{
    float chainLo, chainHi;   // seconds of delay around the loop at size 0 / 1
    int firstTap, fadeStages; // earliest stage heard, and how many stages the taps fade in over
    float lowRatio, lowHz;    // bass decay multiplier and its crossover
    float modSamples;         // modulation depth at mod = 1, in samples at 48 kHz
};

// Present and quick to speak: heard from the second stage on.
constexpr LatticeVoicing kRoom    { 0.8f, 2.4f, 2, 2, 1.0f, 250.0f, 24.0f };
// Further back: the first five stages are silent and the next five fade in.
constexpr LatticeVoicing kHall    { 2.4f, 5.5f, 5, 5, 1.25f, 220.0f, 40.0f };
// Every stage heard from the first: dense at once, bass ringing longest.
constexpr LatticeVoicing kPlate   { 1.2f, 3.0f, 0, 1, 1.3f, 300.0f, 30.0f };
// A long swell: nothing for the first eight stages, then a slow fade in.
constexpr LatticeVoicing kAmbient { 4.0f, 8.0f, 8, 6, 1.3f, 200.0f, 70.0f };

class LatticeReverb
{
public:
    static constexpr int numStages = 20;

    void prepare (float newSampleRate)
    {
        sampleRate = newSampleRate;
        const float norm = (kRatio - 1.0f) / (std::pow (kRatio, (float) numStages) - 1.0f);

        for (int k = 0; k < numStages; ++k)
        {
            // Side 1 carries the geometric series itself (it sums to 1, so `chain`
            // seconds is the length of that side); side 0 sits half a step below, so
            // the forty lengths interleave into a single geometric sequence.
            fraction[k][1] = norm * std::pow (kRatio, (float) k);
            fraction[k][0] = fraction[k][1] / std::sqrt (kRatio);

            for (int c = 0; c < 2; ++c)
                stages[k].line[c].setSize ((int) (fraction[k][c] * kAmbient.chainHi * sampleRate)
                                           + (int) (kAmbient.modSamples * 2.0f * sampleRate / 48000.0f) + 16);
        }

        chain.prepare (kSizeGlideSeconds, sampleRate, 1.0f);

        // Everything laid out or cached for the previous sample rate is stale.
        voicing = nullptr;
        lastChain = lastT60 = lastDamping = -1.0f;
        reset();
    }

    void reset()
    {
        int cursor = 0;

        while (! clearSome (cursor)) {}
    }

    /** Clears the delay memory a bounded amount at a time, `cursor` starting at 0 and
        carried between calls; returns true when everything is clean. Clearing it all at
        once is megabytes of memset, too much for one audio callback at high rates. */
    bool clearSome (int& cursor)
    {
        const int total = numStages * 2;
        int cleared = 0;

        while (cursor < total && cleared < kClearBudget)
        {
            auto& buffer = stages[cursor / 2].line[cursor & 1];
            buffer.clear();
            cleared += buffer.storageSize();
            ++cursor;
        }

        if (cursor < total)
            return false;

        for (auto& stage : stages)
            for (auto& filter : stage.absorption)
                filter.reset();

        loop[0] = loop[1] = loop[2] = loop[3] = 0.0f;

        // Fixed start state: a render after reset() is repeatable sample for sample.
        lfoA.setPhase (0.0f);
        lfoB.setPhase (0.37f);
        wanderA.seed (0x1f83d9abu);
        wanderA.reset();
        wanderB.seed (0x5be0cd19u);
        wanderB.reset();
        snap = true;
        return true;
    }

    void configure (const LatticeVoicing& v, float size01, float t60, float damping01, float mod01)
    {
        const bool voicingChanged = voicing != &v;

        if (voicingChanged)
        {
            voicing = &v;

            // Tap polarities are a fixed pseudo-random pattern: summing twenty stages
            // with one sign would pile their shared content up at low frequencies.
            uint32_t random = 0x2545f491u;

            for (int k = 0; k < numStages; ++k)
            {
                float weight = 0.0f;

                if (k >= v.firstTap + v.fadeStages)
                    weight = 1.0f;
                else if (k >= v.firstTap)
                    weight = std::pow ((float) (k - v.firstTap + 1) / (float) (v.fadeStages + 1), 1.5f);

                stages[k].weight = weight;

                for (int c = 0; c < 2; ++c)
                {
                    random = random * 1664525u + 1013904223u;
                    stages[k].tap[c] = (random & 0x800000u) != 0 ? weight : -weight;
                }
            }
        }

        const float chainTarget = v.chainLo + (v.chainHi - v.chainLo) * juce::jlimit (0.0f, 1.0f, size01);
        chain.setTarget (chainTarget);

        if (snap)
        {
            snap = false;
            chain.snapToTarget();
        }

        if (voicingChanged || ! juce::exactlyEqual (chainTarget, lastChain) || ! juce::exactlyEqual (t60, lastT60)
            || ! juce::exactlyEqual (damping01, lastDamping))
        {
            lastChain = chainTarget;
            lastT60 = t60;
            lastDamping = damping01;
            const auto damping = dampingFor (damping01);

            // The level of what the taps deliver depends on which stages they open and
            // on how much the decay has already taken by the time a signal reaches
            // them, so it is worked out rather than tuned: a signal arrives at stage k
            // having spent its own delay plus, on average, half of every earlier one.
            float energy = 0.0f, elapsed = 0.0f;

            for (int k = 0; k < numStages; ++k)
            {
                for (int c = 0; c < 2; ++c)
                {
                    stages[k].absorption[c].setCrossovers (v.lowHz, damping.highHz, sampleRate);
                    stages[k].absorption[c].setDecay (fraction[k][c] * chainTarget, t60, v.lowRatio, damping.highRatio);
                }

                const float own = fraction[k][1] * chainTarget;
                const float gain = std::pow (10.0f, -3.0f * (elapsed + own) / t60);
                energy += stages[k].weight * stages[k].weight * gain * gain;
                elapsed += 0.5f * own;
            }

            tapNorm = 1.0f / std::sqrt (juce::jmax (kMinTapEnergy, energy));
        }

        modDepth = modDepthFor (mod01, v.modSamples, sampleRate);
        const float rate = 0.5f + 1.3f * juce::jlimit (0.0f, 1.0f, mod01);
        lfoA.setRate (rate, sampleRate);
        lfoB.setRate (rate * 1.0457f, sampleRate); // never quite in step with A
        wanderA.setRate (0.35f, sampleRate);
        wanderB.setRate (0.47f, sampleRate);
    }

    void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
    {
        jassert (voicing != nullptr); // configure() runs before every process()
        constexpr float kHalf = 0.70710678f; // cos and sin of 45 degrees

        for (int n = 0; n < numSamples; ++n)
        {
            const float chainSamples = chain.next() * sampleRate;
            lfoA.next();
            lfoB.next();
            const float wander[2] { wanderA.next(), wanderB.next() };

            float a = inL[n] + loop[0];
            float b = inR[n] + loop[1];
            float c = inL[n] + loop[2];
            float d = inR[n] + loop[3];
            float wetL = 0.0f, wetR = 0.0f;

            for (int k = 0; k < numStages; ++k)
            {
                auto& stage = stages[k];

                // Odd stages move, the two sides in opposite directions, so the average
                // pitch of the tail stays put while its fine structure never repeats.
                float m = 0.0f;

                if ((k & 1) != 0)
                {
                    const auto& lfo = (k & 2) != 0 ? lfoB : lfoA;
                    m = modDepth * (0.65f * lfo.peek ((float) k * (1.0f / (float) numStages))
                                    + 0.45f * wander[(k >> 2) & 1]);
                }

                // Read before write: a delay of D samples sits at index D - 1.
                const float delayA = juce::jmax (kMinDelay, fraction[k][0] * chainSamples + m);
                const float delayB = juce::jmax (kMinDelay, fraction[k][1] * chainSamples - m);
                const float da = stage.absorption[0].process (stage.line[0].readHermite (delayA - 1.0f));
                const float db = stage.absorption[1].process (stage.line[1].readHermite (delayB - 1.0f));
                stage.line[0].write (a);
                stage.line[1].write (b);

                // Taps ping-pong: each output takes the two sides on alternate stages.
                if ((k & 1) == 0)
                {
                    wetL += stage.tap[0] * da;
                    wetR += stage.tap[1] * db;
                }
                else
                {
                    wetL += stage.tap[0] * db;
                    wetR += stage.tap[1] * da;
                }

                // Mix the pair, the sense of the rotation alternating stage to stage...
                const float turn = (k & 1) != 0 ? -kHalf : kHalf;
                a = kHalf * da + turn * db;
                b = kHalf * db - turn * da;

                // ...then trade half of each with the side channel that skips the delays.
                const float keptA = kHalf * (a + c), sideA = kHalf * (c - a);
                const float keptB = kHalf * (b + d), sideB = kHalf * (d - b);
                a = keptA;
                c = sideA;
                b = keptB;
                d = sideB;
            }

            loop[0] = a;
            loop[1] = b;
            loop[2] = c;
            loop[3] = d;
            outL[n] = tapNorm * wetL;
            outR[n] = tapNorm * wetR;
        }
    }

private:
    static constexpr float kRatio = 1.3f;          // each stage's delay over the one before
    static constexpr float kMinDelay = 3.0f;       // the Hermite read needs history on both sides
    static constexpr float kMinTapEnergy = 0.001f; // bounds the make-up gain (30 dB) when a decay
                                                   // is too short to reach the open taps
    static constexpr int kClearBudget = 1 << 16;   // samples of delay memory per clearSome()

    struct Stage
    {
        fx::DelayBuffer line[2];
        fx::DecayFilter absorption[2];
        float tap[2] { 0.0f, 0.0f };
        float weight = 0.0f;
    };

    const LatticeVoicing* voicing = nullptr; // set by the first configure()
    float sampleRate = 48000.0f;
    Stage stages[numStages];
    float fraction[numStages][2] {};
    float loop[4] {};
    float tapNorm = 1.0f;

    fx::Smoother chain;
    fx::SineLfo lfoA, lfoB;
    fx::RandomLfo wanderA, wanderB;
    float modDepth = 0.0f;
    float lastChain = -1.0f, lastT60 = -1.0f, lastDamping = -1.0f;
    bool snap = true;
};

//==============================================================================
/** Spring: three springs, each a feedback loop around a long chain of "stretched"
    allpasses (after Välimäki, Parker & Abel, "Parametric Spring Reverberation Effect",
    JAES 2010).

      in -> (+) -> [allpass (a + z^-K)/(1 + a z^-K)] x M -> delay -> out
             ^                                                |
             +------------ gain < 0 <- low-pass <-------------+

    A spring is dispersive: high frequencies travel slower than low ones, so every echo
    arrives as a downward-then-smeared chirp — the "drip". The allpass chain reproduces
    exactly that: its group delay grows from a couple of milliseconds at DC to nearly
    ninety at the top of the band, and because it sits inside the loop each lap smears
    further, as it does in a real tank. Above the band edge (fs / 2K) the chain's
    response repeats, so the output is low-passed there, like the pickup of a real
    tank that passes little above 4-5 kHz. */
class SpringReverb
{
public:
    static constexpr int numSprings = 3;

    void prepare (float newSampleRate)
    {
        sampleRate = newSampleRate;
        stretch = juce::jmax (1, (int) std::lround (sampleRate / (2.0f * kBandEdgeHz)));
        const float bandEdge = sampleRate / (2.0f * (float) stretch);

        for (int i = 0; i < numSprings; ++i)
        {
            auto& spring = springs[i];
            spring.state.assign ((size_t) (kSections[i] * stretch), 0.0f);
            spring.delay.setSize ((int) (kLoopSeconds[i] * kMaxScale * sampleRate) + 64);
            spring.dcDelay = (float) (kSections[i] * stretch) * (1.0f - kCoefficient[i]) / (1.0f + kCoefficient[i]);
            spring.wobble.setRate (2.5f + 0.7f * (float) i, sampleRate);
        }

        // Fourth-order Butterworth just under the band edge.
        for (auto& side : outputFilter)
        {
            side[0].setLowpass (0.92f * bandEdge, 0.541f, sampleRate);
            side[1].setLowpass (0.92f * bandEdge, 1.307f, sampleRate);
        }

        scale.prepare (kSizeGlideSeconds, sampleRate, 1.0f);
        reset();
    }

    void reset()
    {
        for (int i = 0; i < numSprings; ++i)
        {
            auto& spring = springs[i];
            std::fill (spring.state.begin(), spring.state.end(), 0.0f);
            spring.delay.clear();
            spring.damping.reset();
            spring.index = 0;
            spring.wobble.seed (0x3c6ef372u + 0x9e3779b9u * (uint32_t) i);
            spring.wobble.reset();
        }

        for (auto& side : outputFilter)
            for (auto& filter : side)
                filter.reset();

        snap = true;
    }

    void configure (float size01, float t60, float damping01, float mod01)
    {
        scale.setTarget (kMinScale + (kMaxScale - kMinScale) * juce::jlimit (0.0f, 1.0f, size01));

        if (snap)
        {
            snap = false;
            scale.snapToTarget();
        }

        // A tank rings for a few seconds at most; past that the loop gain gets close
        // enough to 1 that the chirps stop decaying audibly.
        const float seconds = juce::jlimit (0.3f, 6.0f, t60);
        const float dampingHz = 7500.0f * std::pow (0.22f, juce::jlimit (0.0f, 1.0f, damping01));

        for (int i = 0; i < numSprings; ++i)
        {
            auto& spring = springs[i];
            // Negative: the wave inverts when it reflects off the end of the spring.
            spring.gain = -std::pow (10.0f, -3.0f * kLoopSeconds[i] * scale.getTarget() / seconds);
            spring.damping.setCutoff (dampingHz, sampleRate);
        }

        // Real springs are never perfectly still; a little random length wobble also
        // keeps the loop's comb from sounding static.
        wobbleDepth = 12.0f * juce::jlimit (0.0f, 1.0f, mod01) * (sampleRate / 48000.0f);
    }

    void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
    {
        for (int n = 0; n < numSamples; ++n)
        {
            const float s = scale.next();
            const float x = 0.5f * (inL[n] + inR[n]); // one driver, like a real tank
            float out[numSprings];

            for (int i = 0; i < numSprings; ++i)
            {
                auto& spring = springs[i];
                const float a = kCoefficient[i];

                // The loop delay makes up what the chain does not already delay at DC,
                // so the echo period of the low end is the spring's own round-trip time.
                const float loop = juce::jmax (8.0f, kLoopSeconds[i] * s * sampleRate - spring.dcDelay
                                                         + wobbleDepth * spring.wobble.next());
                float v = x + spring.gain * spring.damping.lowpass (spring.delay.readLinear (loop - 1.0f));

                float* state = spring.state.data() + spring.index;

                for (int m = 0; m < kSections[i]; ++m, state += stretch)
                {
                    const float w = v - a * *state;
                    v = a * w + *state;
                    *state = w;
                }

                spring.index = spring.index + 1 < stretch ? spring.index + 1 : 0;
                spring.delay.write (v);

                // The pickup sits at the far end: the first arrival is half a round trip in.
                out[i] = spring.delay.readLinear (0.5f * loop);
            }

            // Two outer springs to the sides, the third shared: wide, but mono-safe.
            const float left = 0.8f * out[0] + 0.5f * out[2];
            const float right = 0.8f * out[1] + 0.5f * out[2];
            outL[n] = outputFilter[0][1].process (outputFilter[0][0].process (left));
            outR[n] = outputFilter[1][1].process (outputFilter[1][0].process (right));
        }
    }

private:
    static constexpr float kBandEdgeHz = 4300.0f;
    static constexpr int kSections[numSprings] { 100, 88, 112 };
    static constexpr float kCoefficient[numSprings] { 0.75f, 0.73f, 0.77f };
    static constexpr float kLoopSeconds[numSprings] { 0.056f, 0.0463f, 0.0661f };
    static constexpr float kMinScale = 0.7f, kMaxScale = 1.3f;

    struct Spring
    {
        std::vector<float> state; // kSections x stretch, laid out section by section
        fx::DelayBuffer delay;
        fx::OnePole damping;
        fx::RandomLfo wobble;
        float gain = -0.8f, dcDelay = 0.0f;
        int index = 0;
    };

    float sampleRate = 48000.0f;
    int stretch = 6;
    Spring springs[numSprings];
    fx::Biquad outputFilter[2][2];
    fx::Smoother scale;
    float wobbleDepth = 0.0f;
    bool snap = true;
};

//==============================================================================
/** What surrounds both engines: pre-delay, wet-only low and high cut ahead of the
    engine, then width and the constant-dry mix behind it. */
class ReverbShell
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = (float) spec.sampleRate;

        for (auto& line : preDelay)
            line.setSize ((int) (kMaxPreDelaySeconds * sampleRate) + 8);

        lattice.prepare (sampleRate);
        spring.prepare (sampleRate);

        dryGain.prepare (kParamSmoothSeconds, sampleRate, 1.0f);
        wetGain.prepare (kParamSmoothSeconds, sampleRate, 0.0f);
        width.prepare (kParamSmoothSeconds, sampleRate, 1.0f);
        fadeStep = 1.0f / (kPreDelayFadeSeconds * sampleRate);
        reset();
    }

    void reset()
    {
        bool restart = true;

        while (! resetSome (restart))
            restart = false;
    }

    /** reset() in instalments, for a type switch on the audio thread: call once per
        block, `restart` true the first time, until it returns true. */
    bool resetSome (bool restart)
    {
        if (restart)
        {
            for (auto& line : preDelay)
                line.clear();

            for (auto& side : lowCut)
                for (auto& filter : side)
                    filter.reset();

            for (auto& filter : highCut)
                filter.reset();

            spring.reset();
            fading = false;
            clearCursor = 0;
        }

        if (! lattice.clearSome (clearCursor))
            return false;

        snap = true;
        return true;
    }

    void setParameters (const ReverbParams& p, ReverbFx::Type newType)
    {
        type = newType;

        const float t60 = juce::jlimit (0.2f, 20.0f, p.decaySeconds);
        const float mix = juce::jlimit (0.0f, 1.0f, p.mix);
        float trim = 1.0f;

        switch (type)
        {
            case ReverbFx::Type::room:    lattice.configure (kRoom, p.size, t60, p.damping, p.mod);    trim = kRoomTrim; break;
            case ReverbFx::Type::hall:    lattice.configure (kHall, p.size, t60, p.damping, p.mod);    trim = kHallTrim; break;
            case ReverbFx::Type::plate:   lattice.configure (kPlate, p.size, t60, p.damping, p.mod);   trim = kPlateTrim; break;
            case ReverbFx::Type::ambient: lattice.configure (kAmbient, p.size, t60, p.damping, p.mod); trim = kAmbientTrim; break;
            case ReverbFx::Type::spring:
                spring.configure (p.size, t60, p.damping, p.mod);
                // A spring that rings longer holds more energy for the same input (about
                // 3 dB per doubling); half of that is taken back out so the decay knob
                // lengthens the tail more than it raises it. The lattice needs no such
                // correction: it normalises its own taps.
                trim = kSpringTrim * std::pow (juce::jmin (t60, 6.0f) / kReferenceDecaySeconds, -0.25f);
                break;
        }

        // Constant-dry mix: the dry signal holds unity up to the half-way point, and
        // the wet side rises on a curve that gives the bottom of the knob fine control.
        const float wetLinear = juce::jmin (1.0f, 2.0f * mix);
        dryGain.setTarget (juce::jmin (1.0f, 2.0f - 2.0f * mix));
        wetGain.setTarget (trim * wetLinear * std::sqrt (wetLinear));
        width.setTarget (juce::jlimit (0.0f, 1.0f, p.width));

        preDelayTarget = juce::jlimit (0, preDelay[0].capacity(),
                                       (int) std::lround (juce::jlimit (0.0f, 250.0f, p.preDelayMs) * 0.001f * sampleRate));

        for (int c = 0; c < 2; ++c)
        {
            // Two cascaded sections each way: 12 dB/octave, and wet-only by construction.
            lowCut[c][0].setHighpass (p.lowCutHz, 0.7071f, sampleRate);
            highCut[c].setLowpass (juce::jmin (p.highCutHz, 0.45f * sampleRate), 0.7071f, sampleRate);
        }

        if (snap)
        {
            snap = false;
            dryGain.snapToTarget();
            wetGain.snapToTarget();
            width.snapToTarget();
            preDelayA = preDelayB = preDelayTarget;
            fading = false;
        }
    }

    void process (juce::dsp::AudioBlock<float> block)
    {
        const int numChannels = (int) juce::jmin ((size_t) 2, block.getNumChannels());

        if (numChannels == 0)
            return;

        float* left = block.getChannelPointer (0);
        float* right = numChannels > 1 ? block.getChannelPointer (1) : nullptr;
        const int total = (int) block.getNumSamples();

        for (int start = 0; start < total; start += kChunk)
        {
            const int n = juce::jmin (kChunk, total - start);
            float* l = left + start;
            float* r = right != nullptr ? right + start : nullptr;

            // --- pre-delay and the wet-only filters
            for (int i = 0; i < n; ++i)
            {
                const float dryL = fx::sanitise (l[i]);
                const float dryR = r != nullptr ? fx::sanitise (r[i]) : dryL;
                preDelay[0].write (dryL);
                preDelay[1].write (dryR);

                if (! fading && preDelayTarget != preDelayA)
                {
                    preDelayB = preDelayTarget;
                    fade = 0.0f;
                    fading = true;
                }

                float tapL = preDelay[0].read (preDelayA);
                float tapR = preDelay[1].read (preDelayA);

                if (fading)
                {
                    fade = juce::jmin (1.0f, fade + fadeStep);
                    const float gainA = std::sqrt (1.0f - fade), gainB = std::sqrt (fade);
                    tapL = gainA * tapL + gainB * preDelay[0].read (preDelayB);
                    tapR = gainA * tapR + gainB * preDelay[1].read (preDelayB);

                    if (fade >= 1.0f)
                    {
                        preDelayA = preDelayB;
                        fading = false;
                    }
                }

                inL[i] = highCut[0].process (lowCut[0][0].process (tapL));
                inR[i] = highCut[1].process (lowCut[1][0].process (tapR));
            }

            // --- the engine
            switch (type)
            {
                case ReverbFx::Type::room:
                case ReverbFx::Type::hall:
                case ReverbFx::Type::plate:
                case ReverbFx::Type::ambient: lattice.process (inL, inR, wetL, wetR, n); break;
                case ReverbFx::Type::spring:  spring.process (inL, inR, wetL, wetR, n); break;
            }

            // --- width and mix
            for (int i = 0; i < n; ++i)
            {
                const float dry = dryGain.next();
                const float wet = wetGain.next();
                const float w = width.next();
                const float mid = 0.5f * (wetL[i] + wetR[i]);
                const float side = 0.5f * (wetL[i] - wetR[i]) * w;

                if (r != nullptr)
                {
                    l[i] = l[i] * dry + (mid + side) * wet;
                    r[i] = r[i] * dry + (mid - side) * wet;
                }
                else
                {
                    l[i] = l[i] * dry + mid * wet;
                }
            }
        }
    }

private:
    // Output trims that put every engine's tail at a comparable loudness for the same
    // input and decay (measured with tubamp_fxrender; see docs/REVERB-DELAY.md §5).
    static constexpr float kRoomTrim = 1.4f;
    static constexpr float kHallTrim = 1.7f;
    static constexpr float kPlateTrim = 1.29f;
    static constexpr float kSpringTrim = 1.4f;
    static constexpr float kAmbientTrim = 1.26f;
    static constexpr float kReferenceDecaySeconds = 1.6f;


    float sampleRate = 48000.0f;
    ReverbFx::Type type = ReverbFx::Type::plate;

    fx::DelayBuffer preDelay[2];
    int preDelayTarget = 0, preDelayA = 0, preDelayB = 0, clearCursor = 0;
    float fade = 0.0f, fadeStep = 0.001f;
    bool fading = false, snap = true;

    fx::Biquad lowCut[2][1], highCut[2];
    fx::Smoother dryGain, wetGain, width;

    LatticeReverb lattice;
    SpringReverb spring;

    float inL[kChunk] {}, inR[kChunk] {}, wetL[kChunk] {}, wetR[kChunk] {};
};
} // namespace

//==============================================================================
struct ReverbFx::Impl
{
    ReverbShell shell;
    fx::EngineSwitcher switcher;
    ReverbParams params;
    bool snapEngine = true;
};

ReverbFx::ReverbFx() : impl (std::make_unique<Impl>()) {}
ReverbFx::~ReverbFx() = default;

void ReverbFx::prepare (const juce::dsp::ProcessSpec& spec)
{
    impl->shell.prepare (spec);
    impl->switcher.prepare (spec.sampleRate, (int) spec.maximumBlockSize, (int) spec.numChannels,
                            (int) Type::plate);
    impl->snapEngine = true;
}

void ReverbFx::reset()
{
    impl->shell.reset();
    // The shell is clean, so the next block starts on the requested type outright
    // rather than finishing a switch that was in flight.
    impl->snapEngine = true;
}

void ReverbFx::setParameters (const ReverbParams& params)
{
    impl->params = params;
    impl->params.type = juce::jlimit (0, numTypes - 1, params.type);
}

void ReverbFx::process (juce::dsp::AudioBlock<float> block)
{
    auto& s = *impl;

    if (s.snapEngine)
    {
        // Nothing has been heard since prepare() or reset(): start on the requested type.
        s.snapEngine = false;
        s.switcher.snapTo (s.params.type);
    }

    const int type = s.switcher.begin (block, s.params.type,
                                       [&s] (int, bool restart) { return s.shell.resetSome (restart); });

    // While the incoming type's memory is still being cleared the switcher outputs the
    // dry input and the shell must not run; while a switch is fading out, `type` is
    // still the outgoing one.
    if (! s.switcher.isHolding())
    {
        s.shell.setParameters (s.params, (Type) type);
        s.shell.process (block);
    }

    s.switcher.end (block);
}
} // namespace tubamp
