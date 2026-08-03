#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <vector>

// The user-buildable signal chain: which blocks are in the path and in what order.
//
// The chain is a strict series between two fixed endpoints:
//   Input Trim -> [ ordered, user-arranged blocks ] -> DC Blocker -> Output Level
// A block can be absent from the chain entirely (removed) or present-but-bypassed
// (its "*_on" APVTS parameter, which stays automatable). Order itself is NOT a
// parameter: it lives in plugin state (and presets) as a comma-separated token list,
// and reaches the audio thread as a single packed uint64 (lock-free swap).
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
    fx3
};

inline constexpr int numBlockTypes = 12;

/** The three external-AU slots are contiguous at the end of the enum; FxHost indexes
    its slot array with (id - fx1). */
inline constexpr int numFxSlots = 3;

inline constexpr bool isFxSlot (BlockId id) noexcept
{
    return id >= BlockId::fx1 && id <= BlockId::fx3;
}

/** 0..numFxSlots-1 for an fx block, -1 otherwise. */
inline constexpr int fxSlotIndex (BlockId id) noexcept
{
    return isFxSlot (id) ? (int) id - (int) BlockId::fx1 : -1;
}

struct BlockInfo
{
    BlockId id;
    const char* token;         // stable id used in saved state ("gate", "comp", ...)
    const char* displayName;   // "Noise Gate"
    const char* shortName;     // tile label: "GATE"
    const char* enableParamId; // must match Parameters.h ("gate_on", ...)
};

inline constexpr std::array<BlockInfo, numBlockTypes> blockInfos { {
    { BlockId::gate,   "gate",   "Noise Gate", "GATE",   "gate_on" },
    { BlockId::comp,   "comp",   "Compressor", "COMP",   "comp_on" },
    { BlockId::drive,  "drive",  "Drive",      "DRIVE",  "drive_on" },
    { BlockId::amp,    "amp",    "NAM Amp",    "AMP",    "amp_on" },
    { BlockId::cab,    "cab",    "Cab IR",     "CAB",    "cab_on" },
    { BlockId::eq,     "eq",     "Tone Stack", "EQ",     "eq_on" },
    { BlockId::mod,    "mod",    "Modulation", "MOD",    "mod_on" },
    { BlockId::delay,  "delay",  "Delay",      "DELAY",  "delay_on" },
    { BlockId::reverb, "reverb", "Reverb",     "REVERB", "reverb_on" },
    { BlockId::fx1,    "fx1",    "FX Slot 1",  "FX 1",   "fx1_on" },
    { BlockId::fx2,    "fx2",    "FX Slot 2",  "FX 2",   "fx2_on" },
    { BlockId::fx3,    "fx3",    "FX Slot 3",  "FX 3",   "fx3_on" },
} };

inline const BlockInfo& infoFor (BlockId id) noexcept
{
    return blockInfos[(size_t) juce::jlimit (0, numBlockTypes - 1, (int) id)];
}

/** Active chain: ordered subset of block types, no duplicates. */
using Order = std::vector<BlockId>;

inline Order defaultOrder()
{
    return { BlockId::gate, BlockId::comp, BlockId::drive, BlockId::amp, BlockId::cab,
             BlockId::eq, BlockId::mod, BlockId::delay, BlockId::reverb };
}

/** Serialized form of a deliberately-empty chain. Distinguishes "the user removed
    every block" (round-trips as empty) from "state predates the user-arrangeable
    chain" (missing property stringifies to "" and falls back to defaultOrder()). */
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

/** Tolerant parse: unknown tokens are dropped, duplicates keep the first occurrence.
    emptyChainToken parses to an empty order; any other empty/garbage input falls
    back to defaultOrder() so old state without a chain always loads. */
inline Order fromString (const juce::String& text)
{
    if (text.trim() == emptyChainToken)
        return {};

    Order order;
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

    return order.empty() ? defaultOrder() : order;
}

// --- lock-free packing -------------------------------------------------------
// Layout: bits [0..3] = count, then entry i in bits [4+4i .. 7+4i].
// 12 entries * 4 bits + 4 = 52 bits used of 64.
//
// Both fields are 4 bits wide, so the encoding tops out at 15 block types; the
// static_asserts below are the tripwire for anyone adding a 16th.

static_assert (numBlockTypes <= 15, "block ids and the count share a 4-bit field");
static_assert (4 + 4 * numBlockTypes <= 64, "packed order must fit in a uint64");

inline uint64_t pack (const Order& order) noexcept
{
    const auto count = (uint64_t) juce::jmin ((int) order.size(), numBlockTypes);
    uint64_t packed = count;

    for (uint64_t i = 0; i < count; ++i)
        packed |= ((uint64_t) order[(size_t) i] & 0xFULL) << (4 + 4 * i);

    return packed;
}

/** Allocation-free decode for the audio thread. Returns the entry count. */
inline int unpackTo (uint64_t packed, std::array<BlockId, numBlockTypes>& out) noexcept
{
    const int count = juce::jlimit (0, numBlockTypes, (int) (packed & 0xF));

    for (int i = 0; i < count; ++i)
    {
        const auto value = (int) ((packed >> (4 + 4 * i)) & 0xF);
        out[(size_t) i] = (BlockId) juce::jlimit (0, numBlockTypes - 1, value);
    }

    return count;
}

inline Order unpack (uint64_t packed)
{
    std::array<BlockId, numBlockTypes> entries {};
    const int count = unpackTo (packed, entries);
    return Order (entries.begin(), entries.begin() + count);
}
} // namespace tubamp::chain
