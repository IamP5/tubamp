#pragma once

// The public value types of the update module (docs/UPDATES.md). No JSON, URL, HTTP,
// zip or juce::var type appears here: wire formats are parsed into these types inside
// src/update/detail.

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace tubamp::update
{
/** A release version, exactly "X.Y.Z".

    The field widths are the invariant: major fits 16 bits and minor and patch fit 8, so
    the ordering of Version equals the ordering of the AudioComponents `version` integer
    (major << 16 | minor << 8 | patch) that macOS and Logic compare. */
struct Version
{
    std::uint16_t major = 0;
    std::uint8_t minor = 0;
    std::uint8_t patch = 0;

    /** Digits and two dots only, no leading zeros, no suffix, each part in range. */
    static std::optional<Version> parse (std::string_view text) noexcept;

    auto operator<=> (const Version&) const = default;

    std::uint32_t audioComponentVersion() const noexcept
    {
        return (std::uint32_t (major) << 16) | (std::uint32_t (minor) << 8) | patch;
    }

    std::string toString() const;
};

/** Why an install attempt ended without changing the installed version. */
enum class Failure
{
    network,       // no connection, timeout, HTTP error, redirect outside the allowlist
    integrity,     // size or SHA-256 differs from the signed payload
    archive,       // an entry breaks the extraction policy
    wrongBundle,   // Info.plist, CPU slices or code seal do not match the offer
    noSpace,       // ENOSPC while staging
    notWritable,   // a folder refused the write or the swap
};

/** Why this copy cannot replace itself. The user gets the release page instead. */
enum class ManualReason
{
    systemDomain,     // running from /Library/Audio/Plug-Ins/Components
    unexpectedPath,   // not <home>/Library/Audio/Plug-Ins/Components/tubamp.component
    readOnly,         // a folder the update writes is not writable by this user
    noAtomicSwap,     // no RENAME_SWAP on the volume, or staging is on another volume
    notAnAudioUnit,   // the Standalone app
};

/** Reported only after an explicit CheckNow. Automatic checks fail silently. */
enum class CheckProblem
{
    unreachable,   // network
    unverified,    // envelope format, signature, payload, product or replay check failed
};

/** What the user can see. Exactly one alternative holds. detail::project derives it
    from the model; it is never stored. */
namespace phase
{
    /** Updates are compiled out or this copy never checks. Nothing is fetched. */
    struct Off { bool operator== (const Off&) const = default; };

    /** Nothing newer is known. `problem` is set only after a failed manual check. */
    struct Idle
    {
        std::optional<CheckProblem> problem;
        bool operator== (const Idle&) const = default;
    };

    struct Checking { bool operator== (const Checking&) const = default; };

    /** A verified offer above the running version, and this copy can install it. */
    struct Available
    {
        Version version;
        bool operator== (const Available&) const = default;
    };

    /** `total` is the signed size, so it is known before the first byte arrives. */
    struct Downloading
    {
        Version version;
        std::uint64_t received = 0, total = 0;
        bool operator== (const Downloading&) const = default;
    };

    /** Unpack, inspect, swap, settle. Sub-second. Not cancellable after the swap. */
    struct Installing
    {
        Version version;
        bool operator== (const Installing&) const = default;
    };

    /** The bundle on disk is `installed`; this process still runs other code. Ends
        only when the host restarts. */
    struct RestartPending
    {
        Version installed;
        bool operator== (const RestartPending&) const = default;
    };

    /** The last attempt failed and the installed version is unchanged. */
    struct Failed
    {
        Version version;
        Failure why;
        bool operator== (const Failed&) const = default;
    };

    /** A verified offer exists but this copy cannot replace itself. */
    struct Manual
    {
        Version version;
        ManualReason why;
        bool operator== (const Manual&) const = default;
    };
}

using Phase = std::variant<phase::Off, phase::Idle, phase::Checking, phase::Available,
                           phase::Downloading, phase::Installing, phase::RestartPending,
                           phase::Failed, phase::Manual>;

/** A complete snapshot. Listeners receive whole Views, never deltas, so a dropped event
    costs nothing: the next one, or the hydration snapshot, is complete. */
struct View
{
    Version running;                   // compiled-in version of the code in this process
    bool autoCheck = true;             // the user's "check automatically" setting
    bool disclose = false;             // show the one-time "checks GitHub daily" toast
    bool systemCopy = false;           // another tubamp is installed in /Library
    std::optional<Version> announce;   // toast this version once, then MarkAnnounced
    Phase phase;

    bool operator== (const View&) const = default;
};

/** Everything a user can ask for. Each is an intent: the machine decides what it means
    in the current state and ignores it when it means nothing. */
namespace command
{
    /** Check now, ignoring the daily throttle. Still one plain GET. */
    struct CheckNow { bool operator== (const CheckNow&) const = default; };

    /** Install the offer the user was shown. Ignored unless `version` is the current
        offer, so the user never installs something they did not see. */
    struct Install
    {
        Version version;
        bool operator== (const Install&) const = default;
    };

    /** Stop a download. No effect once the swap has started. */
    struct Cancel { bool operator== (const Cancel&) const = default; };

    /** Also counts as having seen the disclosure. */
    struct SetAutoCheck
    {
        bool enabled = true;
        bool operator== (const SetAutoCheck&) const = default;
    };

    /** The page showed the toast for `version`; do not announce it again. */
    struct MarkAnnounced
    {
        Version version;
        bool operator== (const MarkAnnounced&) const = default;
    };

    /** The page showed the one-time disclosure toast. */
    struct MarkDisclosed { bool operator== (const MarkDisclosed&) const = default; };

    /** Open the release page of the offered (or running) version in the browser. The
        URL is built in C++ from constants and the version; no URL crosses the bridge. */
    struct OpenReleasePage { bool operator== (const OpenReleasePage&) const = default; };
}

using Command = std::variant<command::CheckNow, command::Install, command::Cancel,
                             command::SetAutoCheck, command::MarkAnnounced,
                             command::MarkDisclosed, command::OpenReleasePage>;
} // namespace tubamp::update
