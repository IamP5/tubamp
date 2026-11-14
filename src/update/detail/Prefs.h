#pragma once

// PRIVATE to src/update. The user's update settings: a value with two pure rules
// (defined here) and the disk store (Prefs.cpp). The store is shared by every host
// process on the account, so each write is a read-modify-write under a flock, written
// to a temporary file and renamed.

#include "../UpdateTypes.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace tubamp::update::detail
{
struct Prefs
{
    bool autoCheck = true;
    bool disclosed = false;   // the "checks GitHub once a day" toast was shown
    std::optional<std::chrono::sys_seconds> lastCheckAt;
    std::optional<Version> announced;   // highest version already toasted

    bool operator== (const Prefs&) const = default;
};

/** A change to Prefs, as data, so the machine's requests can be compared in tests and
    applied atomically by the store. `announced` only ever raises the stored value. */
struct PrefsPatch
{
    std::optional<bool> autoCheck;
    std::optional<bool> disclosed;
    std::optional<Version> announced;

    bool operator== (const PrefsPatch&) const = default;
};

inline void apply (Prefs& prefs, const PrefsPatch& patch)
{
    if (patch.autoCheck) prefs.autoCheck = *patch.autoCheck;
    if (patch.disclosed) prefs.disclosed = *patch.disclosed;
    if (patch.announced) prefs.announced = prefs.announced ? std::max (*prefs.announced, *patch.announced)
                                                           : *patch.announced;
}

/** At most daily: due when automatic checks are on and the last claimed check is
    absent, at least 24 hours old, or in the future (a clock that moved backwards). */
inline bool isCheckDue (const Prefs& prefs, std::chrono::sys_seconds now)
{
    using namespace std::chrono_literals;
    return prefs.autoCheck
        && (! prefs.lastCheckAt || *prefs.lastCheckAt > now || now - *prefs.lastCheckAt >= 24h);
}

// ---------------------------------------------------------------------------------
// The store (Prefs.cpp).
// ---------------------------------------------------------------------------------

/** <home>/Library/Application Support/tubamp/update: prefs.json, latest.signed, lock.
    `lock` is the support lock: one flock serializes prefs, the envelope cache, staging
    creation, commit and reconcile across every process on the account. */
struct SupportPaths
{
    std::filesystem::path dir;
};

/** Missing or unreadable file = defaults; a field of the wrong type = that default.
    Never fails. */
Prefs loadPrefs (const SupportPaths&);

/** The single writer: load, apply, write, under the lock. Returns the patched value;
    if the disk refused the write, it holds for this process only. */
Prefs mutatePrefs (const SupportPaths&, const PrefsPatch&);

/** Under the lock: if isCheckDue, set lastCheckAt = now and return true. Makes "at most
    daily" hold across instances and processes, and counts failed checks too. Returns
    false when the claim cannot be saved, so an unwritable folder never means a check
    on every editor open. */
bool claimDailyCheck (const SupportPaths&, std::chrono::sys_seconds now);
} // namespace tubamp::update::detail
