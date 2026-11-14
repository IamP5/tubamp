#pragma once

// PRIVATE to src/update. The network boundary: one interface, one production
// implementation (NSURLSession, network phase), fakes in tests. Declared here; only
// CancelToken is implemented. Tone3000Client's download code is not reused: it sends a
// Bearer token, checks https on the first URL only, and never hashes.

#include "Rules.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

namespace tubamp::update::detail
{
/** Set from the message thread (Cancel, shutdown), read by the worker. */
class CancelToken
{
public:
    void request() noexcept { flag.store (true, std::memory_order_relaxed); }
    bool requested() const noexcept { return flag.load (std::memory_order_relaxed); }

private:
    std::atomic<bool> flag { false };
};

struct FetchLimits
{
    std::uint64_t maxBytes = 0;                  // the transfer is aborted past this
    std::chrono::seconds idleTimeout { 15 };     // no bytes for this long
    std::chrono::seconds totalTimeout { 30 };    // whole transfer, redirects included
    int maxRedirects = 4;
};

enum class FetchError
{
    cancelled,
    unreachable,       // DNS, connect, TLS, idle or total timeout
    httpStatus,        // final status is not 200
    redirectRefused,   // a hop is not https or its host fails hostAllowed()
    tooLarge,          // more than maxBytes arrived, or Content-Length says so
};

/** Receives the body in order. Returning false aborts the transfer (write error). */
using ByteSink = std::function<bool (ByteView)>;

/** A plain GET. Every implementation keeps this contract:
    - GET only, no body, no cookies, no cache, no credentials, no Authorization header,
      User-Agent exactly "tubamp-update/1";
    - hostAllowed() on the first URL and on every redirect target before it is requested;
    - bytes go to the sink as they arrive; the fetcher writes nothing to disk;
    - stops within about 100 ms of CancelToken::request();
    - blocks the calling worker thread; never called on the message or audio thread. */
class Fetcher
{
public:
    virtual ~Fetcher() = default;
    virtual Result<std::monostate, FetchError> get (const Url&, const FetchLimits&,
                                                    const ByteSink&, const CancelToken&) = 0;
};

/** NSURLSession, ephemeral configuration, redirect delegate answering nil unless
    hostAllowed(). Implemented by the network phase. */
std::unique_ptr<Fetcher> makeSystemFetcher();
} // namespace tubamp::update::detail
