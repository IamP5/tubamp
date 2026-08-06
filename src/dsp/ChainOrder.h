#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

// The user-buildable signal chain: which blocks are in the path and in what order.
//
// The chain is a strict series between two fixed endpoints:
//   Input Trim -> [ ordered, user-arranged blocks ] -> DC Blocker -> Output Level
// A block can be absent from the chain entirely (removed) or present-but-bypassed
// (its "*_on" APVTS parameter, which stays automatable). Order itself is NOT a
// parameter: it lives in plugin state (and presets) as a comma-separated token list,
// and reaches the audio thread as a single packed 128-bit word (lock-free swap).
//
// A BlockId identifies a block INSTANCE, not a block type: six of the nine built-in
// kinds may sit in the chain up to three times, and every instance is its own id with
// its own token, its own tile and its own APVTS parameters. Instance 1 of each kind
// keeps the original id, token and parameter ids, so state written before the pool
// existed loads unchanged.
namespace tubamp::chain
{
enum class BlockId : int
{
    gate = 0,
    comp,
    drive,
    amp,
    cab,
    eq,
    mod,
    delay,
    reverb,

    // External AudioUnit slots. Unlike the nine built-in blocks these carry no DSP
    // of their own: each hosts a third-party AU effect chosen by the user (see
    // dsp/FxHost.h). They are absent from defaultOrder() — a slot only enters the
    // chain when the user adds it — and an empty slot is a bit-exact pass-through.
    fx1,
    fx2,
    fx3,

    // Instances 2 and 3 of the duplicable kinds. Appended so the twelve ids above keep
    // their values: those are what saved state, presets and the packed word contain.
    // Each kind's pair is contiguous and the pairs follow the kind order above —
    // instanceOf() and instanceId() both depend on that layout.
    comp2 = 12,
    comp3,
    drive2,
    drive3,
    eq2,
    eq3,
    mod2,
    mod3,
    delay2,
    delay3,
    reverb2,
    reverb3,

    // Split-path structure (docs/SPLIT.md): a chain may contain at most one region
    // shaped `split, <lane A ids...>, lane2, <lane B ids...>, mix`. split/lane2/mix are
    // structural furniture, each its own singleton kind; lane2 is never rendered as a
    // tile but still needs a token so parse/pack round-trip it. amp2 is an ordinary
    // singleton block (engine B) that happens to be useful inside a lane but is not
    // itself structural — it survives sanitizeStructure's flatten like any block.
    split,
    lane2,
    mix,
    amp2
};

inline constexpr int numBlockTypes = 28;

/** Ids below this are the twelve that a build without the instance pool knows how to
    read; the chainOrderV2 scheme in PluginProcessor.cpp keys off the same number. */
inline constexpr int numV1BlockIds = 12;

/** Instances of a duplicable kind. Singletons (gate, amp, cab, fx1-3) have one. */
inline constexpr int maxInstancesPerKind = 3;

/** Chosen cap on chain length — a UI board this deep is already unwieldy to page
    through, so this stopped being an id-space consequence the moment the id space grew
    past it. Not derived from numBlockTypes: growing the enum does not grow this. Keep
    in sync with the packed-word assert below (5 + 5*24 = 125 <= 128). */
inline constexpr int maxChainLength = 24;

/** The three external-AU slots are contiguous at the end of the enum; FxHost indexes
    its slot array with (id - fx1). */
inline constexpr int numFxSlots = 3;

static_assert ((int) BlockId::fx3 == 11,
               "isFxSlot's range test needs the fx slots to stay the last v1 ids");
static_assert ((int) BlockId::amp2 == numBlockTypes - 1,
               "the instance pairs and the split/lane2/mix/amp2 ids must fill the enum "
               "up to numBlockTypes");
static_assert ((int) BlockId::split == 24,
               "split's token is persisted (state, presets); this pins its id so a "
               "reorder above can't silently change what old saves decode to");

inline constexpr bool isFxSlot (BlockId id) noexcept
{
    return id >= BlockId::fx1 && id <= BlockId::fx3;
}

/** 0..numFxSlots-1 for an fx block, -1 otherwise. */
inline constexpr int fxSlotIndex (BlockId id) noexcept
{
    return isFxSlot (id) ? (int) id - (int) BlockId::fx1 : -1;
}

/** The kind an instance belongs to: comp2 and comp3 are both comp; every singleton is
    its own kind. What the DSP dispatch and the UI's panel/glyph tables key off. */
constexpr BlockId kindOf (BlockId id) noexcept
{
    switch (id)
    {
        case BlockId::comp2:
        case BlockId::comp3:   return BlockId::comp;
        case BlockId::drive2:
        case BlockId::drive3:  return BlockId::drive;
        case BlockId::eq2:
        case BlockId::eq3:     return BlockId::eq;
        case BlockId::mod2:
        case BlockId::mod3:    return BlockId::mod;
        case BlockId::delay2:
        case BlockId::delay3:  return BlockId::delay;
        case BlockId::reverb2:
        case BlockId::reverb3: return BlockId::reverb;

        case BlockId::gate:
        case BlockId::comp:
        case BlockId::drive:
        case BlockId::amp:
        case BlockId::cab:
        case BlockId::eq:
        case BlockId::mod:
        case BlockId::delay:
        case BlockId::reverb:
        case BlockId::fx1:
        case BlockId::fx2:
        case BlockId::fx3:
        case BlockId::split:
        case BlockId::lane2:
        case BlockId::mix:
        case BlockId::amp2:    break;
    }

    return id;
}

/** 0-based instance number: 0 for every singleton and for instance 1 of a duplicable
    kind (which keeps the kind's own id), 1 for comp2, 2 for comp3. split/lane2/mix/amp2
    sit above reverb3, outside the instance-pair block, and are singletons like gate or
    amp — the pair arithmetic below must not run on them. */
constexpr int instanceOf (BlockId id) noexcept
{
    const int index = (int) id;

    if (index < numV1BlockIds || index > (int) BlockId::reverb3)
        return 0;

    return (index - numV1BlockIds) % 2 + 1;
}

constexpr int numInstancesOf (BlockId kind) noexcept
{
    switch (kind)
    {
        case BlockId::comp:
        case BlockId::drive:
        case BlockId::eq:
        case BlockId::mod:
        case BlockId::delay:
        case BlockId::reverb:  return maxInstancesPerKind;

        case BlockId::gate:
        case BlockId::amp:
        case BlockId::cab:
        case BlockId::fx1:
        case BlockId::fx2:
        case BlockId::fx3:
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
        case BlockId::split:
        case BlockId::lane2:
        case BlockId::mix:
        case BlockId::amp2:    break;
    }

    return 1;
}

/** Inverse of kindOf/instanceOf: the id of a kind's 0-based instance. An instance
    number this kind does not have answers with the kind itself. */
constexpr BlockId instanceId (BlockId kind, int instance) noexcept
{
    if (instance <= 0 || instance >= numInstancesOf (kind))
        return kind;

    switch (kind)
    {
        case BlockId::comp:   return (BlockId) ((int) BlockId::comp2   + instance - 1);
        case BlockId::drive:  return (BlockId) ((int) BlockId::drive2  + instance - 1);
        case BlockId::eq:     return (BlockId) ((int) BlockId::eq2     + instance - 1);
        case BlockId::mod:    return (BlockId) ((int) BlockId::mod2    + instance - 1);
        case BlockId::delay:  return (BlockId) ((int) BlockId::delay2  + instance - 1);
        case BlockId::reverb: return (BlockId) ((int) BlockId::reverb2 + instance - 1);

        case BlockId::gate:
        case BlockId::amp:
        case BlockId::cab:
        case BlockId::fx1:
        case BlockId::fx2:
        case BlockId::fx3:
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
        case BlockId::split:
        case BlockId::lane2:
        case BlockId::mix:
        case BlockId::amp2:    break;
    }

    return kind;
}

struct BlockInfo
{
    BlockId id;
    const char* token;         // stable id used in saved state ("gate", "comp2", ...)
    const char* displayName;   // "Noise Gate"
    const char* shortName;     // tile label: "GATE"
    const char* enableParamId; // must match Parameters.h ("gate_on", ...)
};

inline constexpr std::array<BlockInfo, numBlockTypes> blockInfos { {
    { BlockId::gate,    "gate",    "Noise Gate",    "GATE",     "gate_on" },
    { BlockId::comp,    "comp",    "Compressor",    "COMP",     "comp_on" },
    { BlockId::drive,   "drive",   "Drive",         "DRIVE",    "drive_on" },
    { BlockId::amp,     "amp",     "NAM Amp",       "AMP",      "amp_on" },
    { BlockId::cab,     "cab",     "Cab IR",        "CAB",      "cab_on" },
    { BlockId::eq,      "eq",      "Tone Stack",    "EQ",       "eq_on" },
    { BlockId::mod,     "mod",     "Modulation",    "MOD",      "mod_on" },
    { BlockId::delay,   "delay",   "Delay",         "DELAY",    "delay_on" },
    { BlockId::reverb,  "reverb",  "Reverb",        "REVERB",   "reverb_on" },
    { BlockId::fx1,     "fx1",     "FX Slot 1",     "FX 1",     "fx1_on" },
    { BlockId::fx2,     "fx2",     "FX Slot 2",     "FX 2",     "fx2_on" },
    { BlockId::fx3,     "fx3",     "FX Slot 3",     "FX 3",     "fx3_on" },
    { BlockId::comp2,   "comp2",   "Compressor 2",  "COMP 2",   "comp2_on" },
    { BlockId::comp3,   "comp3",   "Compressor 3",  "COMP 3",   "comp3_on" },
    { BlockId::drive2,  "drive2",  "Drive 2",       "DRIVE 2",  "drive2_on" },
    { BlockId::drive3,  "drive3",  "Drive 3",       "DRIVE 3",  "drive3_on" },
    { BlockId::eq2,     "eq2",     "Tone Stack 2",  "EQ 2",     "eq2_on" },
    { BlockId::eq3,     "eq3",     "Tone Stack 3",  "EQ 3",     "eq3_on" },
    { BlockId::mod2,    "mod2",    "Modulation 2",  "MOD 2",    "mod2_on" },
    { BlockId::mod3,    "mod3",    "Modulation 3",  "MOD 3",    "mod3_on" },
    { BlockId::delay2,  "delay2",  "Delay 2",       "DELAY 2",  "delay2_on" },
    { BlockId::delay3,  "delay3",  "Delay 3",       "DELAY 3",  "delay3_on" },
    { BlockId::reverb2, "reverb2", "Reverb 2",      "REVERB 2", "reverb2_on" },
    { BlockId::reverb3, "reverb3", "Reverb 3",      "REVERB 3", "reverb3_on" },
    { BlockId::split,   "split",   "Splitter",      "SPLIT",    "split_on" },
    { BlockId::lane2,   "lane2",   "Lane B",        "LANE B",   "split_on" },
    { BlockId::mix,     "mix",     "Mixer",         "MIX",      "mix_on" },
    { BlockId::amp2,    "amp2",    "NAM Amp 2",     "AMP 2",    "amp2_on" },
} };

inline const BlockInfo& infoFor (BlockId id) noexcept
{
    return blockInfos[(size_t) juce::jlimit (0, numBlockTypes - 1, (int) id)];
}

/** Active chain: ordered subset of block instances, no duplicates. */
using Order = std::vector<BlockId>;

/** What a fresh instance runs: the amp alone. Everything else is added by the user. */
inline Order defaultOrder()
{
    return { BlockId::amp };
}

/** The full arrangement the plugin used to start with. Not a default any more, but
    still the shape of a factory preset and the fallback for state that predates the
    user-arrangeable chain. */
inline Order classicOrder()
{
    return { BlockId::gate, BlockId::comp, BlockId::drive, BlockId::amp, BlockId::cab,
             BlockId::eq, BlockId::mod, BlockId::delay, BlockId::reverb };
}

/** Serialized form of a deliberately-empty chain. Distinguishes "the user removed
    every block" (round-trips as empty) from "state predates the user-arrangeable
    chain" (missing property stringifies to "" and falls back to classicOrder()). */
inline constexpr const char* emptyChainToken = "-";

/** "gate,comp,drive,amp,cab,eq,mod,delay,reverb"; an empty order serializes to
    emptyChainToken so it survives a save/reload. */
inline juce::String toString (const Order& order)
{
    if (order.empty())
        return emptyChainToken;

    juce::StringArray tokens;

    for (auto id : order)
        tokens.add (infoFor (id).token);

    return tokens.joinIntoString (",");
}

/** Tolerant parse: unknown tokens are dropped, duplicates keep the first occurrence,
    and emptyChainToken, "" and garbage all parse to an empty order. No fallback —
    this is the parse live UI input goes through, where "the user removed every block"
    must never turn into a chain nobody asked for. */
inline Order parseOrder (const juce::String& text)
{
    Order order;

    if (text.trim() == emptyChainToken)
        return order;

    std::array<bool, numBlockTypes> seen {};

    for (const auto& raw : juce::StringArray::fromTokens (text, ",", {}))
    {
        const auto token = raw.trim();

        for (const auto& info : blockInfos)
        {
            if (token == info.token && ! seen[(size_t) info.id])
            {
                seen[(size_t) info.id] = true;
                order.push_back (info.id);
                break;
            }
        }
    }

    return order;
}

/** parseOrder for persisted bytes only (state, presets, A/B). State written before the
    chain became user-arrangeable carries no token list at all, and an empty parse that
    was not the deliberate emptyChainToken means exactly that: it loads as the
    arrangement the plugin had then. Live UI input must use parseOrder. */
inline Order parseOrderOrLegacy (const juce::String& text)
{
    auto order = parseOrder (text);

    if (order.empty() && text.trim() != emptyChainToken)
        return classicOrder();

    return order;
}

// --- split-path structure (docs/SPLIT.md) -------------------------------------
// Exactly one split region per chain: split, <lane A...>, lane2, <lane B...>, mix.
// The flat Order is the only source of truth for this shape — findStructure locates it,
// sanitizeStructure enforces it (or flattens to plain serial), and every entry point
// that can put an Order in front of the audio thread runs the sanitizer first.

/** Positions of the three structural ids within an Order, or -1 where absent. */
struct Structure
{
    int splitAt = -1, lane2At = -1, mixAt = -1;

    /** Either no structure at all, or a well-formed one: split before lane2 before
        mix. Anything else (a lone split, mix ahead of split, a missing lane2) is not
        a structure sanitizeStructure would ever leave standing. */
    bool valid() const noexcept
    {
        if (splitAt < 0 && lane2At < 0 && mixAt < 0)
            return true;

        return splitAt >= 0 && lane2At >= 0 && mixAt >= 0
            && splitAt < lane2At && lane2At < mixAt;
    }
};

/** First occurrence of split/lane2/mix in order, whatever shape the order is in —
    callers judge validity via Structure::valid() or by re-deriving through
    sanitizeStructure first. */
inline Structure findStructure (const Order& order) noexcept
{
    Structure structure;

    for (int i = 0; i < (int) order.size(); ++i)
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

/** Enforces the split-region grammar, tolerantly, in priority order:
      1. A repeated structural id keeps only its first occurrence (belt: parseOrder's
         own dedup already guarantees this for anything that went through it, but
         hand-built/persisted orders may not have).
      2. If split < lane2 < mix all occur, the structure is kept as-is.
      3. Otherwise every structural id is stripped and the order flattens to plain
         serial. amp2 is not structural — it is an ordinary block and survives the
         flatten untouched, wherever it sat.
    Never errors: a garbled structure degrades to no structure, not a rejected load. */
inline Order sanitizeStructure (Order order)
{
    Order deduped;
    deduped.reserve (order.size());

    bool sawSplit = false, sawLane2 = false, sawMix = false;

    for (auto id : order)
    {
        if (id == BlockId::split)
        {
            if (sawSplit)
                continue;
            sawSplit = true;
        }
        else if (id == BlockId::lane2)
        {
            if (sawLane2)
                continue;
            sawLane2 = true;
        }
        else if (id == BlockId::mix)
        {
            if (sawMix)
                continue;
            sawMix = true;
        }

        deduped.push_back (id);
    }

    if (findStructure (deduped).valid())
        return deduped;

    Order flattened;
    flattened.reserve (deduped.size());

    for (auto id : deduped)
        if (id != BlockId::split && id != BlockId::lane2 && id != BlockId::mix)
            flattened.push_back (id);

    return flattened;
}

// --- lock-free packing -------------------------------------------------------
// Layout: bits [0..4] = count, then entry i in bits [5+5i .. 9+5i].
// 24 entries * 5 bits + 5 = 125 bits used of 128.
//
// One whole-value store / one whole-value load, exactly as with the uint64 this grew
// out of: concurrent writers stay benign and the audio thread can never see a torn
// order. Both fields are 5 bits wide, so the encoding tops out at 31 block ids; the
// static_asserts below are the tripwire for anyone adding a 32nd.

using Packed = unsigned __int128;

static_assert (std::atomic<Packed>::is_always_lock_free,
               "the audio thread reads the order without a lock");
static_assert (numBlockTypes <= 31, "block ids and the count share a 5-bit field");
static_assert (5 + 5 * maxChainLength <= 128, "packed order must fit in one word");

inline Packed pack (const Order& order) noexcept
{
    const auto count = (Packed) juce::jmin ((int) order.size(), maxChainLength);
    Packed packed = count;

    for (Packed i = 0; i < count; ++i)
        packed |= ((Packed) order[(size_t) i] & (Packed) 0x1F) << (5 + 5 * i);

    return packed;
}

/** Allocation-free decode for the audio thread. Returns the entry count. Both clamps
    are load-bearing: they are what makes the decoded ids safe to index present[] with,
    whatever bit pattern the word happens to hold. */
inline int unpackTo (Packed packed, std::array<BlockId, maxChainLength>& out) noexcept
{
    const int count = juce::jlimit (0, maxChainLength, (int) (packed & (Packed) 0x1F));

    for (int i = 0; i < count; ++i)
    {
        const auto value = (int) ((packed >> (5 + 5 * i)) & (Packed) 0x1F);
        out[(size_t) i] = (BlockId) juce::jlimit (0, numBlockTypes - 1, value);
    }

    return count;
}

inline Order unpack (Packed packed)
{
    std::array<BlockId, maxChainLength> entries {};
    const int count = unpackTo (packed, entries);
    return Order (entries.begin(), entries.begin() + count);
}
} // namespace tubamp::chain
