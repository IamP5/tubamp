#pragma once

#include "ChainOrder.h"

#include <array>
#include <cstdint>

// The chain's width, position by position: where the signal is mono, where it becomes
// stereo and why, where it folds back. Derived from the same packed order and parameter
// values the audio thread runs, on the message thread, and never measured: a map that
// says "this delay makes it stereo" holds before a note is played.
//
// "Stereo" means left and right carry different signals. One source placed in the field
// (a lane panned alone, L = k * R) is mono. A one-channel instance is mono everywhere.
namespace tubamp::chain
{
enum class Width : std::uint8_t { mono, stereo };

/** What the block at one position does to the width it receives. */
enum class Turn : std::uint8_t
{
    keeps,     // out == in
    widens,    // mono in, stereo out, because of `why`
    folds,     // stereo in, mono out, because of `why`
    monoHost   // would widen with two output channels; this instance has one
};

/** One value per user-visible cause. */
enum class Why : std::uint8_t
{
    none,
    pingPong, dualRatio, delayMod, diffuse,
    reverb,
    chorus, tremolo,
    stereoIr,
    lanes,
    plugin,
    amp, amp2, pluginMono
};

/** split_mode's choice order, frozen with the parameter (docs/SPLIT.md §2). */
enum class SplitMode { copy = 0, leftRight = 1, crossover = 2 };

struct HostLayout
{
    std::uint8_t in = 2, out = 2;
    bool operator== (const HostLayout&) const = default;
};

struct Step
{
    BlockId id = BlockId::gate;
    Width out = Width::mono;     // the wire leaving this block (split: lane A's entry)
    Width outB = Width::mono;    // split: lane B's entry; otherwise == out
    Turn turn = Turn::keeps;
    Why why = Why::none;
    bool operator== (const Step&) const = default;
};

struct WidthMap
{
    HostLayout host;
    Width input = Width::mono;
    std::uint8_t length = 0;
    std::array<Step, maxChainLength> steps {};   // parallel to the decoded order
    Width output = Width::mono;
    bool wouldBeStereo = false;                  // output of the same walk with two output channels
    bool operator== (const WidthMap&) const = default;
};

inline constexpr int kWidthInstances = maxInstancesPerKind;
inline constexpr int kWidthCabs = 2;

/** Everything the walk reads, as plain values, read from the atomics the audio thread
    reads (TubampAudioProcessor::widthInputs). */
struct WidthInputs
{
    Packed order = 0;
    HostLayout host;

    /** The block changes the signal at all: on, and for a cab an IR in the path, for amp2
        a model, for an fx slot a live plugin. Decides whether two lanes can differ. */
    std::array<bool, numBlockTypes> active {};

    struct Cab   { bool irStereo = false; float mix = 1.0f; };
    struct Mod   { int type = 0, chorusMode = 1, tremVoice = 1; float depth = 0.4f, width = 1.0f, mix = 0.35f; };
    struct Delay { int mode = 0, type = 0; float ratioPct = 100.0f, width = 1.0f, mix = 0.25f, mod = 0.2f; };
    struct Verb  { float mix = 0.25f, width = 1.0f; };

    /** A live slot's main bus: our own width, or mono because the plugin would take
        nothing else (FxHost's collapse-to-mono). */
    enum class Fx : std::uint8_t { empty, hostWidth, monoOnly };

    struct Split { bool on = true; int mode = 0; };
    struct Mix   { bool on = true; float aPan = 0.0f, bPan = 0.0f; };

    std::array<Cab, kWidthCabs> cab {};
    std::array<Mod, kWidthInstances> mod {};
    std::array<Delay, kWidthInstances> delay {};
    std::array<Verb, kWidthInstances> reverb {};
    std::array<Fx, numFxSlots> fx {};
    Split split;
    Mix mix;
};

WidthMap planWidth (const WidthInputs&) noexcept;

/** Which of the four segments an entry runs in: 0 serial before the split (and every
    entry of a chain without lanes), 1 lane A, 2 lane B, 3 serial after the mixer. */
struct Segments
{
    Structure structure;
    bool lanes = false;

    int of (int index) const noexcept
    {
        if (! lanes || index < structure.splitAt) return 0;
        if (index < structure.lane2At)            return 1;
        if (index < structure.mixAt)              return 2;

        return 3;
    }
};

/** First occurrence of split/lane2/mix in a decoded order, as the audio thread finds it. */
Structure structureOf (const std::array<BlockId, maxChainLength>& order, int count) noexcept;

/** The one fold rule, shared by the cab's stereo verdict (audio thread) and planWidth: an
    amp at `ampAt` that folds (`folds`) is downstream of the entry at `at` when it is later
    in the order, except that lane B is not downstream of lane A. */
bool ampFoldsAfter (const Segments&, int at, int ampAt, bool folds) noexcept;
} // namespace tubamp::chain
