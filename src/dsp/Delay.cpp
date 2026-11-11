#include "Delay.h"

#include "FxPrimitives.h"

#include <array>
#include <cmath>

namespace tubamp
{
namespace
{
// Tunings for the delay engine (docs/REVERB-DELAY.md §3).

constexpr float kMaxDelaySeconds = 4.0f;       // 2000 ms knob x 200% Dual ratio
constexpr float kModHeadroomSeconds = 0.03f;   // deepest wow excursion, with margin
constexpr float kMinDelaySamples = 4.0f;       // the Hermite read needs history on both sides
constexpr float kParamSmoothSeconds = 0.02f;
constexpr float kCrossfadeSeconds = 0.04f;     // clean types: old tap -> new tap
constexpr float kAnalogGlideSeconds = 0.12f;   // BBD clock slewing to its new rate
constexpr float kTapeGlideSeconds = 0.30f;     // per stage; two stages = capstan inertia
constexpr float kDuckAttackSeconds = 0.003f;
constexpr float kDuckReleaseSeconds = 0.25f;

constexpr float kChorusRateHz = 0.6f;
constexpr float kChorusMaxSeconds = 0.004f;    // +-4 ms at mod = 1 (~26 cents peak)
constexpr float kWowRateHz = 0.55f;
constexpr float kFlutterRateHz = 6.3f;

// The tape head's low-frequency bump, and a loop trim of exactly the same size: the
// bump's peak then sits at the feedback knob's own gain, so even at maximum feedback
// no frequency recirculates above unity and the loop cannot hum itself into oscillation.
constexpr float kHeadBumpDb = 1.0f;
constexpr float kTapeLoopTrim = 0.8913f; // -1 dB

// In-loop diffuser lengths for the Diffuse type, in milliseconds. Mutually prime-ish and
// different per side so the smear decorrelates left from right.
constexpr float kDiffuserMs[2][4] { { 3.1f, 5.3f, 8.9f, 14.3f }, { 3.7f, 6.1f, 9.7f, 15.7f } };

/** Peak delay excursion, in seconds, that makes a sinusoidal modulator at `rateHz`
    bend pitch by `cents` at its steepest point. */
float excursionSecondsFor (float cents, float rateHz) noexcept
{
    return cents * (0.69314718f / 1200.0f) / (fx::kTwoPi * rateHz);
}

class CharacterDelay
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = (float) spec.sampleRate;
        const int capacity = (int) std::ceil ((kMaxDelaySeconds + kModHeadroomSeconds) * sampleRate) + 8;

        for (int c = 0; c < 2; ++c)
        {
            channels[c].line.setSize (capacity);
            diffuserTotal[c] = 0.0f;

            for (int k = 0; k < 4; ++k)
            {
                diffuserLength[c][k] = juce::jmax (2, (int) std::lround (kDiffuserMs[c][k] * 0.001f * sampleRate));
                channels[c].diffusers[(size_t) k].setSize (diffuserLength[c][k] + 2);
                diffuserTotal[c] += (float) diffuserLength[c][k];
            }
        }

        maxDelaySamples = (float) channels[0].line.capacity() - 2.0f;

        feedback.prepare (kParamSmoothSeconds, sampleRate, 0.0f);
        dryGain.prepare (kParamSmoothSeconds, sampleRate, 1.0f);
        wetGain.prepare (kParamSmoothSeconds, sampleRate, 0.0f);
        width.prepare (kParamSmoothSeconds, sampleRate, 1.0f);

        duckAttack = 1.0f - std::exp (-1.0f / (kDuckAttackSeconds * sampleRate));
        duckRelease = 1.0f - std::exp (-1.0f / (kDuckReleaseSeconds * sampleRate));
        fadeStep = 1.0f / (kCrossfadeSeconds * sampleRate);

        chorus.setRate (kChorusRateHz, sampleRate);
        wow.setRate (kWowRateHz, sampleRate);
        flutter.setRate (kFlutterRateHz, sampleRate);
        wowDrift.setRate (0.4f, sampleRate);
        flutterNoise.setRate (9.0f, sampleRate);

        reset();
    }

    void reset()
    {
        for (auto& ch : channels)
        {
            ch.line.clear();
            ch.lowCut.reset();
            ch.highCut.reset();
            ch.bbd1.reset();
            ch.bbd2.reset();
            ch.headBump.reset();
            ch.lofiPre.reset();
            ch.lofiPost.reset();
            ch.tape1.reset();
            ch.tape2.reset();
            ch.held = 0.0f;
            ch.holdPhase = 1.0f; // sample on the very first frame
            ch.fading = false;

            for (auto& ap : ch.diffusers)
                ap.clear();
        }

        // Modulators restart from a fixed state, so a render after reset() is repeatable
        // sample for sample (offline bounces, and the tests that compare two renders).
        chorus.setPhase (0.0f);
        wow.setPhase (0.0f);
        flutter.setPhase (0.0f);
        wowDrift.seed (0x51ed270bu);
        wowDrift.reset();
        flutterNoise.seed (0x2545f491u);
        flutterNoise.reset();
        duckEnvelope = 0.0f;
        snapOnNextUpdate = true;
    }

    void setParameters (const DelayParams& p, DelayFx::Type newType)
    {
        type = newType;
        mode = (DelayFx::Mode) juce::jlimit (0, 2, p.mode);

        const float color = juce::jlimit (0.0f, 1.0f, p.color);
        const float mod = juce::jlimit (0.0f, 1.0f, p.mod);
        const float mix = juce::jlimit (0.0f, 1.0f, p.mix);
        const float duck = juce::jlimit (0.0f, 1.0f, p.duck);

        // --- times
        const float baseSamples = DelayFx::resolveTimeMs (p) * 0.001f * sampleRate;
        const float ratio = mode == DelayFx::Mode::dual ? p.ratioPct * 0.01f : 1.0f;
        channels[0].target = juce::jlimit (kMinDelaySamples, maxDelaySamples, baseSamples);
        channels[1].target = juce::jlimit (kMinDelaySamples, maxDelaySamples, baseSamples * ratio);

        // Clean types jump between taps under a crossfade; the two "mechanical" types
        // slew the delay itself, so a time change bends the pitch of what is in the line.
        glides = type == DelayFx::Type::analog || type == DelayFx::Type::tape;
        const float glideSeconds = type == DelayFx::Type::tape ? kTapeGlideSeconds : kAnalogGlideSeconds;
        glideA = 1.0f - std::exp (-1.0f / (glideSeconds * sampleRate));
        // The BBD clock is a single lag; tape adds a second one for the transport's inertia.
        glideB = type == DelayFx::Type::tape ? glideA : 1.0f;

        // --- shared loop filters
        const float nyquistSafe = sampleRate * 0.45f;

        for (auto& ch : channels)
        {
            ch.lowCut.setHighpass (p.lowCutHz, 0.7071f, sampleRate);
            ch.highCut.setLowpass (juce::jmin (p.highCutHz, nyquistSafe), 0.7071f, sampleRate);
        }

        // --- per-type character, scaled by `color`
        switch (type)
        {
            case DelayFx::Type::digital:
                // Colour 0 is a strictly linear loop; above it, a soft limiter whose
                // small-signal gain stays 1 so the feedback knob keeps its meaning.
                saturates = color > 1.0e-4f;
                drive = 0.3f + 5.0f * color * color;
                break;

            case DelayFx::Type::analog:
                saturates = true;
                drive = 0.6f + 1.6f * color;
                asymmetry = 0.12f + 0.25f * color; // even harmonics: the compander's signature

                for (auto& ch : channels)
                {
                    // A bucket brigade's clock slows as the delay grows, and its
                    // anti-alias/reconstruction filters have to follow: longer = darker.
                    const float seconds = ch.target / sampleRate;
                    const float cutoff = juce::jlimit (1500.0f, 6500.0f, 3400.0f * std::sqrt (0.3f / seconds))
                                         * (1.35f - 0.7f * color);
                    ch.bbd1.setLowpass (juce::jmin (cutoff, nyquistSafe), 0.54f, sampleRate);
                    ch.bbd2.setLowpass (juce::jmin (cutoff, nyquistSafe), 1.31f, sampleRate); // 4th-order Butterworth pair
                }

                break;

            case DelayFx::Type::tape:
                saturates = true;
                drive = 0.5f + 2.5f * color;

                for (auto& ch : channels)
                {
                    // Gap and spacing loss: two gentle poles that slide down as the tape ages.
                    const float cutoff = juce::jmin (800.0f + 8200.0f * std::exp (-2.7f * color), nyquistSafe);
                    ch.tape1.setCutoff (cutoff, sampleRate);
                    ch.tape2.setCutoff (cutoff, sampleRate);
                    ch.headBump.setPeak (90.0f, 0.8f, kHeadBumpDb, sampleRate);
                }

                break;

            case DelayFx::Type::lofi:
            {
                saturates = false;
                const float reducedRate = 26000.0f + (7000.0f - 26000.0f) * color;
                holdIncrement = juce::jmin (1.0f, reducedRate / sampleRate);
                quantSteps = std::exp2 (11.0f - 4.5f * color); // 12 bits down to ~7.5
                const float cutoff = juce::jmin (0.42f * reducedRate, nyquistSafe);

                for (auto& ch : channels)
                {
                    ch.lofiPre.setLowpass (cutoff, 0.7071f, sampleRate);
                    ch.lofiPost.setLowpass (cutoff, 0.7071f, sampleRate);
                }

                break;
            }

            case DelayFx::Type::diffuse:
                saturates = false;
                diffusion = 0.2f + 0.5f * color;

                // An allpass delays by its own length on average, and the four of them
                // sit inside the loop: take that out of the line so a lap still lasts
                // the time asked for (and left and right stay in step, their diffusers
                // being different lengths). Below the diffusers' own total the lap
                // cannot get any shorter.
                for (int c = 0; c < 2; ++c)
                    channels[c].target = juce::jmax (kMinDelaySamples, channels[c].target - diffuserTotal[c]);

                break;

        }

        invDrive = 1.0f / drive;

        // --- modulation depths, in samples
        if (type == DelayFx::Type::tape)
        {
            // Worn transports wander even with the mod knob down.
            const float wowCents = 3.0f * color + 40.0f * mod * mod;
            const float flutterCents = 1.5f * color + 6.0f * mod;
            wowDepth = excursionSecondsFor (wowCents, kWowRateHz) * sampleRate;
            flutterDepth = excursionSecondsFor (flutterCents, kFlutterRateHz) * sampleRate;
            chorusDepth = 0.0f;
        }
        else
        {
            chorusDepth = kChorusMaxSeconds * mod * mod * sampleRate;
            wowDepth = flutterDepth = 0.0f;
        }

        // --- gains. Constant-dry mix: the dry signal holds unity up to the half-way
        // point, so adding delay never pulls the guitar down.
        feedback.setTarget (juce::jlimit (0.0f, 0.95f, p.feedback));
        dryGain.setTarget (juce::jmin (1.0f, 2.0f - 2.0f * mix));
        wetGain.setTarget (juce::jmin (1.0f, 2.0f * mix));
        width.setTarget (juce::jlimit (0.0f, 1.0f, p.width));
        duckAmount = 40.0f * duck * duck;

        if (snapOnNextUpdate)
        {
            snapOnNextUpdate = false;
            feedback.snapToTarget();
            dryGain.snapToTarget();
            wetGain.snapToTarget();
            width.snapToTarget();

            for (auto& ch : channels)
                ch.tapA = ch.tapB = ch.glide1 = ch.glide2 = ch.target;
        }
    }

    void process (juce::dsp::AudioBlock<float> block)
    {
        const int numChannels = (int) juce::jmin ((size_t) 2, block.getNumChannels());
        const auto numSamples = block.getNumSamples();

        if (numChannels == 0)
            return;

        float* data[2] { block.getChannelPointer (0), numChannels > 1 ? block.getChannelPointer (1) : nullptr };
        const bool pingPong = mode == DelayFx::Mode::pingPong && numChannels == 2;

        for (size_t i = 0; i < numSamples; ++i)
        {
            const float dry[2] { data[0][i], numChannels > 1 ? data[1][i] : 0.0f };

            // What the engine itself is fed. A non-finite input sample still passes
            // through on the dry side, as it would with the block bypassed, but it must
            // not get into the line or the ducker, where it would stay.
            const float fed[2] { fx::sanitise (dry[0]), fx::sanitise (dry[1]) };

            // --- ducking follows the dry input and only ever scales the wet output
            const float peak = juce::jmax (std::abs (fed[0]), std::abs (fed[1]));
            duckEnvelope += (peak > duckEnvelope ? duckAttack : duckRelease) * (peak - duckEnvelope);
            const float duckGain = 1.0f / (1.0f + duckAmount * duckEnvelope);

            // --- time modulation
            float modulation[2] { 0.0f, 0.0f };

            if (type == DelayFx::Type::tape)
            {
                // One transport carries both channels, so they wander together.
                const float wander = wowDepth * (0.7f * wow.next() + 0.6f * wowDrift.next())
                                     + flutterDepth * (0.6f * flutter.next() + 0.6f * flutterNoise.next());
                modulation[0] = modulation[1] = wander;
            }
            else if (chorusDepth > 0.0f)
            {
                modulation[0] = chorusDepth * chorus.next();
                modulation[1] = chorusDepth * chorus.peek (0.25f);
            }

            // --- read
            float wet[2] { 0.0f, 0.0f };

            for (int c = 0; c < numChannels; ++c)
                wet[c] = readLine (channels[c], modulation[c]);

            // --- write: input plus feedback, through the type's character
            const float fb = feedback.next();
            float in[2];

            if (pingPong)
            {
                // Only line L is seeded and each line feeds the other, so repeats
                // alternate L, R, L, R from a mono sum.
                in[0] = 0.5f * (fed[0] + fed[1]) + fb * wet[1];
                in[1] = fb * wet[0];
            }
            else
            {
                in[0] = fed[0] + fb * wet[0];
                in[1] = fed[1] + fb * wet[1];
            }

            for (int c = 0; c < numChannels; ++c)
                channels[c].line.write (colour (c, in[c]));

            // --- output
            const float dryLevel = dryGain.next();
            const float wetLevel = wetGain.next() * duckGain;

            if (numChannels == 2)
            {
                // Width narrows the wet signal only; feedback is fed the raw taps, so
                // width never changes how the repeats decay. At full width the taps go
                // out as they are: the M/S round trip is the identity only up to
                // rounding, and Stereo mode is two independent lines, exactly.
                const float w = width.next();
                float outL = wet[0], outR = wet[1];

                if (w < 1.0f)
                {
                    const float mid = 0.5f * (wet[0] + wet[1]);
                    const float side = 0.5f * (wet[0] - wet[1]) * w;
                    outL = mid + side;
                    outR = mid - side;
                }

                data[0][i] = dry[0] * dryLevel + outL * wetLevel;
                data[1][i] = dry[1] * dryLevel + outR * wetLevel;
            }
            else
            {
                data[0][i] = dry[0] * dryLevel + wet[0] * wetLevel;
            }
        }
    }

private:
    struct Channel
    {
        fx::DelayBuffer line;
        float target = 20000.0f;
        float tapA = 20000.0f, tapB = 20000.0f, fade = 0.0f; // crossfading types
        bool fading = false;
        float glide1 = 20000.0f, glide2 = 20000.0f;          // gliding types
        fx::Biquad lowCut, highCut, bbd1, bbd2, headBump, lofiPre, lofiPost;
        fx::OnePole tape1, tape2;
        std::array<fx::Allpass, 4> diffusers;
        float held = 0.0f, holdPhase = 1.0f;
    };

    float readAt (const Channel& ch, float delaySamples) const noexcept
    {
        // The line is read before this frame's write, so the newest sample is already
        // one frame old: a delay of D samples sits at index D - 1.
        return ch.line.readHermite (juce::jlimit (kMinDelaySamples, maxDelaySamples, delaySamples) - 1.0f);
    }

    float readLine (Channel& ch, float modulation) noexcept
    {
        if (glides)
        {
            ch.glide1 += glideA * (ch.target - ch.glide1);
            ch.glide2 += glideB * (ch.glide1 - ch.glide2);
            return readAt (ch, ch.glide2 + modulation);
        }

        if (! ch.fading && std::abs (ch.target - ch.tapA) > 0.5f)
        {
            ch.tapB = ch.target;
            ch.fade = 0.0f;
            ch.fading = true;
        }

        const float current = readAt (ch, ch.tapA + modulation);

        if (! ch.fading)
            return current;

        // Equal-power: the two taps hold unrelated audio, so a linear fade would dip.
        ch.fade = juce::jmin (1.0f, ch.fade + fadeStep);
        const float next = readAt (ch, ch.tapB + modulation);
        const float out = current * std::sqrt (1.0f - ch.fade) + next * std::sqrt (ch.fade);

        if (ch.fade >= 1.0f)
        {
            // A target that moved during the fade starts the next one on the next frame.
            ch.tapA = ch.tapB;
            ch.fading = false;
        }

        return out;
    }

    float saturate (float x) const noexcept
    {
        return fx::softClip (x * drive) * invDrive;
    }

    /** What the loop does to a signal each time it goes round. It runs on the way INTO
        the line, so the first repeat is coloured once and the n-th n times. */
    float colour (int c, float x) noexcept
    {
        auto& ch = channels[c];

        switch (type)
        {
            case DelayFx::Type::digital:
                return ch.highCut.process (ch.lowCut.process (saturates ? saturate (x) : x));

            case DelayFx::Type::analog:
            {
                // The squared term's DC is taken out by the low cut that follows.
                const float y = saturate (x + asymmetry * x * x);
                return ch.bbd2.process (ch.bbd1.process (ch.highCut.process (ch.lowCut.process (y))));
            }

            case DelayFx::Type::tape:
            {
                const float y = ch.headBump.process (saturate (x));
                return kTapeLoopTrim * ch.highCut.process (ch.lowCut.process (ch.tape2.lowpass (ch.tape1.lowpass (y))));
            }

            case DelayFx::Type::lofi:
            {
                const float y = ch.lofiPre.process (ch.highCut.process (ch.lowCut.process (x)));
                ch.holdPhase += holdIncrement;

                if (ch.holdPhase >= 1.0f)
                {
                    // A slow converter with a short word: hold, then quantise. Truncating
                    // toward zero (rather than rounding) means a quantised value can only
                    // ever be smaller than what went in, so the loop cannot settle into a
                    // limit cycle: the tail falls into true digital silence.
                    ch.holdPhase -= 1.0f;
                    ch.held = std::trunc (y * quantSteps) / quantSteps;
                }

                return ch.lofiPost.process (ch.held);
            }

            case DelayFx::Type::diffuse:
            {
                float y = ch.highCut.process (ch.lowCut.process (x));

                for (int k = 0; k < 4; ++k)
                    y = ch.diffusers[(size_t) k].process (y, diffuserLength[c][k], diffusion);

                return y;
            }

        }

        return x;
    }

    Channel channels[2];
    int diffuserLength[2][4] {};
    float diffuserTotal[2] {};
    float sampleRate = 48000.0f, maxDelaySamples = 1000.0f;

    DelayFx::Type type = DelayFx::Type::digital;
    DelayFx::Mode mode = DelayFx::Mode::stereo;
    bool glides = false, saturates = false, snapOnNextUpdate = true;
    float glideA = 1.0f, glideB = 1.0f, fadeStep = 0.001f;
    float drive = 1.0f, invDrive = 1.0f, asymmetry = 0.0f;
    float holdIncrement = 1.0f, quantSteps = 2048.0f, diffusion = 0.5f;
    float chorusDepth = 0.0f, wowDepth = 0.0f, flutterDepth = 0.0f;
    float duckAmount = 0.0f, duckAttack = 1.0f, duckRelease = 1.0f, duckEnvelope = 0.0f;

    fx::Smoother feedback, dryGain, wetGain, width;
    fx::SineLfo chorus, wow, flutter;
    fx::RandomLfo wowDrift, flutterNoise;
};
} // namespace

//==============================================================================
struct DelayFx::Impl
{
    CharacterDelay engine;
    fx::EngineSwitcher switcher;
    DelayParams params;
    bool snapEngine = true;
};

DelayFx::DelayFx() : impl (std::make_unique<Impl>()) {}
DelayFx::~DelayFx() = default;

float DelayFx::resolveTimeMs (const DelayParams& params) noexcept
{
    const int sync = juce::jlimit (0, kNumDelaySyncChoices - 1, params.sync);

    if (sync == 0)
        return params.timeMs;

    const double bpm = params.bpm > 1.0 ? params.bpm : 120.0;
    return juce::jlimit (20.0f, 2000.0f, (float) (kDelaySyncBeats[sync] * 60000.0 / bpm));
}

void DelayFx::prepare (const juce::dsp::ProcessSpec& spec)
{
    impl->engine.prepare (spec);
    impl->switcher.prepare (spec.sampleRate, (int) spec.maximumBlockSize, (int) spec.numChannels,
                            (int) Type::digital);
    impl->snapEngine = true;
}

void DelayFx::reset()
{
    impl->engine.reset();
    // The engine is clean, so the next block starts on the requested type outright
    // rather than finishing a switch that was in flight.
    impl->snapEngine = true;
}

void DelayFx::setParameters (const DelayParams& params)
{
    impl->params = params;
    impl->params.type = juce::jlimit (0, numTypes - 1, params.type);
}

void DelayFx::process (juce::dsp::AudioBlock<float> block)
{
    auto& s = *impl;

    if (s.snapEngine)
    {
        // Nothing has been heard since prepare() or reset(): start on the requested type.
        s.snapEngine = false;
        s.switcher.snapTo (s.params.type);
    }

    // Every type is the same engine with a different character, so a type change is a
    // fade out, a reset while silent, and a fade back in on the new character.
    const int type = s.switcher.begin (block, s.params.type, [&s] (int, bool)
    {
        s.engine.reset();
        return true;
    });

    // While a switch is fading out, `type` is still the outgoing one: it keeps its own
    // character until it is silent.
    s.engine.setParameters (s.params, (Type) type);
    s.engine.process (block);
    s.switcher.end (block);
}
} // namespace tubamp
