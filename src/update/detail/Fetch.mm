// The production Fetcher (Fetch.h). Compiled with -fobjc-arc (update.cmake).
//
// NSURLSession calls the delegate on its own serial queue. The delegate only records:
// it copies body bytes into Transfer and wakes the worker, which calls the sink, counts
// bytes, and enforces the deadlines and the cancel token itself. So the sink runs on the
// caller's thread, and get() can return the moment it decides, without waiting for the
// session: a late delegate callback writes into a Transfer nobody reads any more.

#import <Foundation/Foundation.h>

#include "Fetch.h"

#include <condition_variable>
#include <deque>
#include <mutex>

#ifndef TUBAMP_FETCH_DELEGATE
 #define TUBAMP_FETCH_DELEGATE TubampUpdateFetchDelegate
#endif

namespace tubamp::update::detail
{
namespace
{
    struct Transfer
    {
        Transfer (const FetchLimits& l, HopObserver o) : limits (l), policy (l.maxRedirects), observer (std::move (o)) {}

        bool admit (std::string_view url)
        {
            const bool ok = policy.admit (url);
            if (observer)
                observer (url, ok);
            return ok;
        }

        const FetchLimits limits;
        HopPolicy policy;   // delegate queue only, after the first admit
        const HopObserver observer;

        std::mutex lock;
        std::condition_variable wake;
        std::deque<Bytes> chunks;
        std::optional<FetchError> refused;   // the first policy decision against the transfer
        bool done = false;                   // didCompleteWithError arrived
        bool failed = false;                 // ...with an error
        bool responded = false;

        void refuse (FetchError why)
        {
            const std::lock_guard<std::mutex> hold (lock);
            if (! refused)
                refused = why;
            wake.notify_all();
        }
    };

    NSMutableURLRequest* plainGet (NSURL* url, const std::string& userAgent, const FetchLimits& limits)
    {
        auto* request = [NSMutableURLRequest requestWithURL: url
                                                cachePolicy: NSURLRequestReloadIgnoringLocalAndRemoteCacheData
                                            timeoutInterval: (NSTimeInterval) limits.idleTimeout.count()];
        request.HTTPMethod = @"GET";
        request.HTTPShouldHandleCookies = NO;
        [request setValue: [NSString stringWithUTF8String: userAgent.c_str()] forHTTPHeaderField: @"User-Agent"];
        // Fixed values, so the request carries nothing from the user's locale, and no
        // transport compression the byte cap would have to see through.
        [request setValue: @"en" forHTTPHeaderField: @"Accept-Language"];
        [request setValue: @"identity" forHTTPHeaderField: @"Accept-Encoding"];
        [request setValue: @"*/*" forHTTPHeaderField: @"Accept"];
        return request;
    }

    NSURLSessionConfiguration* storelessConfiguration (const FetchLimits& limits)
    {
        auto* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        config.HTTPCookieStorage = nil;
        config.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
        config.HTTPShouldSetCookies = NO;
        config.URLCredentialStorage = nil;
        config.URLCache = nil;
        config.requestCachePolicy = NSURLRequestReloadIgnoringLocalAndRemoteCacheData;
        config.timeoutIntervalForRequest = (NSTimeInterval) limits.idleTimeout.count();
        config.timeoutIntervalForResource = (NSTimeInterval) limits.totalTimeout.count();
        config.waitsForConnectivity = NO;
        config.TLSMinimumSupportedProtocolVersion = tls_protocol_version_TLSv12;
        return config;
    }
}
} // namespace tubamp::update::detail

using tubamp::update::detail::FetchError;
using tubamp::update::detail::Transfer;

@interface TUBAMP_FETCH_DELEGATE : NSObject <NSURLSessionDataDelegate>
{
@public
    std::shared_ptr<Transfer> transfer;
    std::string userAgent;
}
@end

@implementation TUBAMP_FETCH_DELEGATE

- (void) URLSession: (NSURLSession*) session didReceiveChallenge: (NSURLAuthenticationChallenge*) challenge
  completionHandler: (void (^) (NSURLSessionAuthChallengeDisposition, NSURLCredential*)) completion
{
    (void) session;
    // The system's trust evaluation for TLS; any other challenge (HTTP auth, client
    // certificates) is refused, so no credential is ever offered.
    if ([challenge.protectionSpace.authenticationMethod isEqualToString: NSURLAuthenticationMethodServerTrust])
        completion (NSURLSessionAuthChallengePerformDefaultHandling, nil);
    else
        completion (NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);
}

- (void) URLSession: (NSURLSession*) session task: (NSURLSessionTask*) task
  didReceiveChallenge: (NSURLAuthenticationChallenge*) challenge
  completionHandler: (void (^) (NSURLSessionAuthChallengeDisposition, NSURLCredential*)) completion
{
    (void) task;
    [self URLSession: session didReceiveChallenge: challenge completionHandler: completion];
}

- (void) URLSession: (NSURLSession*) session task: (NSURLSessionTask*) task
  willPerformHTTPRedirection: (NSHTTPURLResponse*) response newRequest: (NSURLRequest*) request
  completionHandler: (void (^) (NSURLRequest*)) completion
{
    (void) session; (void) response;
    NSString* target = request.URL.absoluteString;
    if (target != nil && transfer->admit (target.UTF8String))
    {
        // A fresh request rather than the one offered, so nothing from the previous hop
        // is carried to the next host.
        completion (tubamp::update::detail::plainGet (request.URL, userAgent, transfer->limits));
        return;
    }
    transfer->refuse (FetchError::redirectRefused);
    completion (nil);
    [task cancel];
}

- (void) URLSession: (NSURLSession*) session dataTask: (NSURLSessionDataTask*) task
  didReceiveResponse: (NSURLResponse*) response
  completionHandler: (void (^) (NSURLSessionResponseDisposition)) completion
{
    (void) session; (void) task;
    std::optional<FetchError> refusal;
    auto* http = [response isKindOfClass: [NSHTTPURLResponse class]] ? (NSHTTPURLResponse*) response : nil;
    const long long declared = response.expectedContentLength;

    if (http == nil || response.URL.absoluteString == nil
        || ! tubamp::update::detail::hostAllowed (response.URL.absoluteString.UTF8String))
        refusal = FetchError::redirectRefused;
    else if (http.statusCode != 200)
        refusal = FetchError::httpStatus;
    else if (declared != NSURLResponseUnknownLength && (unsigned long long) declared > transfer->limits.maxBytes)
        refusal = FetchError::tooLarge;

    {
        const std::lock_guard<std::mutex> hold (transfer->lock);
        transfer->responded = true;
        transfer->wake.notify_all();
    }

    if (refusal)
    {
        transfer->refuse (*refusal);
        completion (NSURLSessionResponseCancel);
        return;
    }
    completion (NSURLSessionResponseAllow);
}

- (void) URLSession: (NSURLSession*) session dataTask: (NSURLSessionDataTask*) task didReceiveData: (NSData*) data
{
    (void) session; (void) task;
    const std::lock_guard<std::mutex> hold (transfer->lock);
    [data enumerateByteRangesUsingBlock: ^(const void* bytes, NSRange range, BOOL*) {
        const auto* first = static_cast<const std::uint8_t*> (bytes);
        transfer->chunks.emplace_back (first, first + range.length);
    }];
    transfer->wake.notify_all();
}

- (void) URLSession: (NSURLSession*) session task: (NSURLSessionTask*) task didCompleteWithError: (NSError*) error
{
    (void) session; (void) task;
    const std::lock_guard<std::mutex> hold (transfer->lock);
    transfer->done = true;
    transfer->failed = error != nil;
    transfer->wake.notify_all();
}

@end

namespace tubamp::update::detail
{
namespace
{
    class SystemFetcher final : public Fetcher
    {
    public:
        SystemFetcher (Version running, HopObserver o) : userAgent ("tubamp/" + running.toString()), observer (std::move (o)) {}

        Result<std::monostate, FetchError> get (const Url& url, const FetchLimits& limits, const ByteSink& sink,
                                                const CancelToken& cancel) override
        {
            @autoreleasepool
            {
                auto transfer = std::make_shared<Transfer> (limits, observer);
                NSString* text = [NSString stringWithUTF8String: url.text().c_str()];
                NSURL* first = text != nil ? [NSURL URLWithString: (NSString* _Nonnull) text] : nil;
                if (first == nil || ! transfer->admit (url.text()))
                    return FetchError::redirectRefused;

                auto* delegate = [[TUBAMP_FETCH_DELEGATE alloc] init];
                delegate->transfer = transfer;
                delegate->userAgent = userAgent;
                auto* queue = [[NSOperationQueue alloc] init];
                queue.maxConcurrentOperationCount = 1;
                auto* session = [NSURLSession sessionWithConfiguration: storelessConfiguration (limits)
                                                              delegate: delegate
                                                         delegateQueue: queue];
                auto* task = [session dataTaskWithRequest: plainGet (first, userAgent, limits)];
                [task resume];

                const auto outcome = pump (*transfer, limits, sink, cancel);

                [task cancel];
                [session invalidateAndCancel];
                if (outcome)
                    return *outcome;
                return std::monostate {};
            }
        }

    private:
        using Clock = std::chrono::steady_clock;

        /** The worker's half: drain bytes into the sink until the transfer ends. Returns
            the error, or nullopt for a complete 200 body. */
        static std::optional<FetchError> pump (Transfer& t, const FetchLimits& limits, const ByteSink& sink,
                                               const CancelToken& cancel)
        {
            const auto deadline = Clock::now() + limits.totalTimeout;
            auto lastActivity = Clock::now();
            std::uint64_t received = 0;
            bool sawResponse = false;

            std::unique_lock<std::mutex> hold (t.lock);
            for (;;)
            {
                while (! t.chunks.empty())
                {
                    auto chunk = std::move (t.chunks.front());
                    t.chunks.pop_front();
                    hold.unlock();
                    lastActivity = Clock::now();
                    received += chunk.size();
                    if (received > limits.maxBytes)
                        return FetchError::tooLarge;
                    if (! sink (ByteView (chunk.data(), chunk.size())))
                        return FetchError::sinkRefused;
                    hold.lock();
                }
                if (t.responded && ! sawResponse)
                {
                    sawResponse = true;
                    lastActivity = Clock::now();
                }
                if (t.refused)
                    return t.refused;
                if (t.done)
                    return t.failed ? std::optional (FetchError::unreachable) : std::nullopt;
                if (cancel.requested())
                    return FetchError::cancelled;
                const auto now = Clock::now();
                if (now >= deadline || now - lastActivity >= limits.idleTimeout)
                    return FetchError::unreachable;
                t.wake.wait_for (hold, std::chrono::milliseconds (20));
            }
        }

        std::string userAgent;
        HopObserver observer;
    };
}

std::unique_ptr<Fetcher> makeSystemFetcher (Version running, HopObserver observer)
{
    return std::make_unique<SystemFetcher> (running, std::move (observer));
}
} // namespace tubamp::update::detail
