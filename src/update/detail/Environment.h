#pragma once

// PRIVATE to src/update. Everything the Updater shell takes from the outside world, so
// tests run it with a fake fetcher, temporary roots and a hand-pumped message queue.
// The production value is built in src/update/plugin/Production.cpp.

#include "Bundle.h"
#include "Fetch.h"

#include <chrono>
#include <functional>
#include <memory>

namespace tubamp::update::detail
{
struct Environment
{
    Version running;
    InstallSite site { InstallSite::Off {} };
    const TrustRoot* trust = nullptr;
    std::shared_ptr<Fetcher> fetcher;
    OsVersion os;
    std::function<std::chrono::sys_seconds()> now;

    /** Runs `job` later on the message thread. Called from the worker. */
    std::function<void (std::function<void()> job)> post;

    /** Message thread. The Url comes from releasePageUrl, never from the page. */
    std::function<void (const Url&)> openInBrowser;
};

inline FetchLimits envelopeLimits()
{
    return { maxEnvelopeBytes, std::chrono::seconds (15), std::chrono::seconds (30), 4 };
}

/** The byte cap is the signed size: one byte more is not the release. */
inline FetchLimits archiveLimits (const Offer& offer)
{
    return { offer.archive().size, std::chrono::seconds (30), std::chrono::minutes (15), 4 };
}
} // namespace tubamp::update::detail
