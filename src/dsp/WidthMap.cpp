#include "WidthMap.h"

#include "CabSim.h"
#include "FxBlocks.h"

namespace tubamp::chain
{
Structure structureOf (const std::array<BlockId, maxChainLength>& order, int count) noexcept
{
    Structure structure;

    for (int i = 0; i < juce::jlimit (0, maxChainLength, count); ++i)
    {
        const auto id = order[(size_t) i];

        if (id == BlockId::split && structure.splitAt < 0)
            structure.splitAt = i;
        else if (id == BlockId::lane2 && structure.lane2At < 0)
            structure.lane2At = i;
        else if (id == BlockId::mix && structure.mixAt < 0)
            structure.mixAt = i;
    }

    return structure;
}

bool ampFoldsAfter (const Segments& segments, int at, int ampAt, bool folds) noexcept
{
    return folds && at >= 0 && ampAt > at && ! (segments.of (at) == 1 && segments.of (ampAt) == 2);
}

namespace
{
/** One walk of the order for one host layout. */
class Walk
{
public:
    Walk (const WidthInputs& in, HostLayout host) noexcept
        : inputs (in), wide (host.out >= 2)
    {
        map.host = host;
        map.length = (std::uint8_t) unpackTo (in.order, order);

        const auto structure = structureOf (order, map.length);
        segments = { structure, structure.splitAt >= 0 && structure.valid() };

        for (int i = 0; i < map.length; ++i)
        {
            if (order[(size_t) i] == BlockId::amp)  ampAt = i;
            if (order[(size_t) i] == BlockId::amp2) amp2At = i;
        }

        map.input = wide && host.in >= 2 ? Width::stereo : Width::mono;
    }

    WidthMap run() noexcept
    {
        Width w = map.input;

        if (! segments.lanes)
        {
            w = serial (0, map.length, w);
        }
        else
        {
            const auto& s = segments.structure;
            w = serial (0, s.splitAt, w);

            const auto entry = split (s.splitAt, w);
            const Width a = serial (s.splitAt + 1, s.lane2At, entry.a);
            put (s.lane2At, { BlockId::lane2, entry.b, entry.b, Turn::keeps, Why::none });
            const Width b = serial (s.lane2At + 1, s.mixAt, entry.b);

            w = mixer (s.mixAt, a, b, entry.differ || altered (s.splitAt + 1, s.mixAt));
            w = serial (s.mixAt + 1, map.length, w);
        }

        map.output = w;
        map.wouldBeStereo = w == Width::stereo;
        return map;
    }

private:
    struct LaneEntry { Width a, b; bool differ; };

    Width serial (int from, int to, Width w) noexcept
    {
        for (int i = from; i < to; ++i)
        {
            const auto step = block (i, w);
            put (i, step);
            w = step.out;
        }

        return w;
    }

    /** A block's turn, from its widening cause and its folding cause. */
    Step turn (BlockId id, Width in, Why widens, Why folds) const noexcept
    {
        if (folds != Why::none && in == Width::stereo)
            return { id, Width::mono, Width::mono, Turn::folds, folds };

        if (widens != Why::none && in == Width::mono)
            return wide ? Step { id, Width::stereo, Width::stereo, Turn::widens, widens }
                        : Step { id, Width::mono, Width::mono, Turn::monoHost, widens };

        return { id, in, in, Turn::keeps, Why::none };
    }

    Step block (int index, Width in) const noexcept
    {
        const auto id = order[(size_t) index];
        const auto k = (size_t) instanceOf (id);
        const bool on = inputs.active[(size_t) id];

        switch (kindOf (id))
        {
            case BlockId::amp:  return turn (id, in, Why::none, on ? Why::amp : Why::none);
            case BlockId::amp2: return turn (id, in, Why::none, on ? Why::amp2 : Why::none);

            case BlockId::cab:
            case BlockId::cab2:
            {
                const auto c = (size_t) (id == BlockId::cab2 ? 1 : 0);
                const bool allowStereo = ! (ampFoldsAfter (segments, index, ampAt, inputs.active[(size_t) BlockId::amp])
                                            || ampFoldsAfter (segments, index, amp2At, inputs.active[(size_t) BlockId::amp2]));
                const auto why = on ? CabSim::stereoWhy (inputs.cab[c].irStereo, allowStereo, inputs.cab[c].mix) : Why::none;
                return turn (id, in, why, Why::none);
            }

            case BlockId::mod:
            {
                const auto& m = inputs.mod[k];
                return turn (id, in, on ? Modulation::stereoWhy (m.type, m.chorusMode, m.tremVoice, m.depth, m.width, m.mix)
                                        : Why::none, Why::none);
            }

            case BlockId::delay:
            {
                const auto& d = inputs.delay[k];
                return turn (id, in, on ? DelayFx::stereoWhy (d.mode, d.type, d.ratioPct, d.width, d.mix, d.mod) : Why::none,
                             Why::none);
            }

            case BlockId::reverb:
            {
                const auto& r = inputs.reverb[k];
                return turn (id, in, on ? ReverbFx::stereoWhy (r.mix, r.width) : Why::none, Why::none);
            }

            case BlockId::fx1:
            case BlockId::fx2:
            case BlockId::fx3:
            {
                const auto slot = on ? inputs.fx[(size_t) fxSlotIndex (id)] : WidthInputs::Fx::empty;
                return turn (id, in,
                             slot == WidthInputs::Fx::hostWidth ? Why::plugin : Why::none,
                             slot == WidthInputs::Fx::monoOnly ? Why::pluginMono : Why::none);
            }

            case BlockId::gate:
            case BlockId::comp:
            case BlockId::drive:
            case BlockId::eq:
            case BlockId::split:
            case BlockId::lane2:
            case BlockId::mix:
            case BlockId::comp2:
            case BlockId::comp3:
            case BlockId::drive2:
            case BlockId::drive3:
            case BlockId::eq2:
            case BlockId::eq3:
            case BlockId::mod2:
            case BlockId::mod3:
            case BlockId::delay2:
            case BlockId::delay3:
            case BlockId::reverb2:
            case BlockId::reverb3:
                break;
        }

        return turn (id, in, Why::none, Why::none);
    }

    /** splitLanes: L/R hands lane A the left channel and lane B the right, each on both of
        its channels; Copy and X-Over keep the input's width in both lanes. */
    LaneEntry split (int index, Width in) noexcept
    {
        const auto mode = (SplitMode) juce::jlimit (0, 2, inputs.split.mode);

        if (mode == SplitMode::leftRight && in == Width::stereo)
        {
            put (index, { BlockId::split, Width::mono, Width::mono, Turn::folds, Why::lanes });
            return { Width::mono, Width::mono, true };
        }

        put (index, { BlockId::split, in, in, Turn::keeps, Why::none });
        return { in, in, mode == SplitMode::crossover };
    }

    /** mixLanes, as a question of whether L is a multiple of R. `differ`: the two lanes do
        not carry the same signal. */
    Width mixer (int index, Width a, Width b, bool differ) noexcept
    {
        const bool laneB = inputs.split.on;
        const bool in = a == Width::stereo || (laneB && b == Width::stereo);
        const auto& mix = inputs.mix;

        const auto step = [this, index] (Width out, Turn t, Why why)
        {
            put (index, { BlockId::mix, out, out, t, why });
            return out;
        };

        // Two different mono lanes placed apart: the only width the mixer makes itself.
        const bool panned = mix.on && laneB && differ && ! juce::approximatelyEqual (mix.aPan, mix.bPan);

        if (! wide)
            return step (Width::mono, panned ? Turn::monoHost : Turn::keeps, panned ? Why::lanes : Why::none);

        if (! mix.on)
            return step (in ? Width::stereo : Width::mono, Turn::keeps, Why::none);

        // Every lane heard hard to one side: the other channel is silent.
        const auto side = [] (float pan) { return pan <= -1.0f ? -1 : (pan >= 1.0f ? 1 : 0); };
        const bool oneSide = side (mix.aPan) != 0 && (! laneB || side (mix.bPan) == side (mix.aPan));

        if (oneSide)
            return step (Width::mono, in ? Turn::folds : Turn::keeps, in ? Why::lanes : Why::none);

        if (in)
            return step (Width::stereo, Turn::keeps, Why::none);

        if (panned)
            return step (Width::stereo, Turn::widens, Why::lanes);

        return step (Width::mono, Turn::keeps, Why::none);
    }

    bool altered (int from, int to) const noexcept
    {
        for (int i = from; i < to; ++i)
            if (inputs.active[(size_t) order[(size_t) i]])
                return true;

        return false;
    }

    void put (int index, Step step) noexcept
    {
        step.id = order[(size_t) index];
        map.steps[(size_t) index] = step;
    }

    const WidthInputs& inputs;
    const bool wide;
    std::array<BlockId, maxChainLength> order {};
    Segments segments;
    int ampAt = -1, amp2At = -1;
    WidthMap map;
};
} // namespace

WidthMap planWidth (const WidthInputs& inputs) noexcept
{
    auto map = Walk (inputs, inputs.host).run();

    if (inputs.host.out < 2)
        map.wouldBeStereo = Walk (inputs, { inputs.host.in, 2 }).run().output == Width::stereo;

    return map;
}
} // namespace tubamp::chain
