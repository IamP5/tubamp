#pragma once

// PRIVATE to src/update. The network boundary: one interface, one production
// implementation (NSURLSession, Fetch.mm), fakes in tests. Tone3000Client's download code
// is not reused: it sends a Bearer token, checks https on the first URL only, and never
// hashes.

#include "Cancel.h"
#include "Rules.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

namespace tubamp::update::detail
{
struct FetchLimits
{
    std::uint64_t maxBytes = 0;                  // the transfer is aborted past this
    std::chrono::seconds idleTimeout { 15 };     // no bytes for this long, connecting included
    std::chrono::seconds totalTimeout { 30 };    // whole transfer, redirects included
    int maxRedirects = 4;
};

enum class FetchError
{
    cancelled,
    unreachable,       // DNS, connect, TLS, idle or total timeout
    httpStatus,        // final status is not 200
    redirectRefused,   // a hop is not https, its host fails hostAllowed(), or one hop too many
    tooLarge,          // more than maxBytes arrived, or Content-Length says so
    sinkRefused,       // the sink returned false; the sink knows why
};

/** The redirect rule as a value with no I/O. The fetcher asks it before every request it
    makes: the first URL, then each redirect target. Once it refuses, it refuses
    everything, and the transfer ends with redirectRefused. */
class HopPolicy
{
public:
    explicit HopPolicy (int maxRedirects) noexcept : limit (maxRedirects) {}

    bool admit (std::string_view url)
    {
        seen.emplace_back (url);
        open = open && hostAllowed (url) && int (seen.size()) - 1 <= limit;
        return open;
    }

    /** Every URL offered to admit(), in order, the refused one included. */
    const std::vector<std::string>& hops() const noexcept { return seen; }

private:
    int limit;
    bool open = true;
    std::vector<std::string> seen;
};

/** Receives the body in order, on the thread that called get(). Returning false aborts
    the transfer with sinkRefused. */
using ByteSink = std::function<bool (ByteView)>;

/** A plain GET. Every implementation keeps this contract:
    - GET only, no body, no cookies, no cache, no credentials, no Authorization header;
    - HopPolicy on the first URL and on every redirect target before it is requested;
    - bytes go to the sink as they arrive; the fetcher writes nothing to disk;
    - returns within about 100 ms of CancelToken::request();
    - blocks the calling worker thread; never called on the message or audio thread. */
class Fetcher
{
public:
    virtual ~Fetcher() = default;
    virtual Result<std::monostate, FetchError> get (const Url&, const FetchLimits&,
                                                    const ByteSink&, const CancelToken&) = 0;
};

/** Called with each URL the policy was asked about, and its verdict. Diagnostics only. */
using HopObserver = std::function<void (std::string_view url, bool admitted)>;

/** NSURLSession with an ephemeral configuration that stores nothing, a redirect delegate
    that asks HopPolicy, and the User-Agent "tubamp/<running>". Fetch.mm. */
std::unique_ptr<Fetcher> makeSystemFetcher (Version running, HopObserver = {});
} // namespace tubamp::update::detail
