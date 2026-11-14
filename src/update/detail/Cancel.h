#pragma once

// PRIVATE to src/update. The cancel token the worker's jobs poll: a user's Cancel stops
// a download, and the Updater's shutdown stops a download and a wait for the support lock.

#include <atomic>

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

/** For callers with nothing to cancel: tests, tools, a child process. */
inline const CancelToken& neverCancelled()
{
    static const CancelToken token;
    return token;
}
} // namespace tubamp::update::detail
