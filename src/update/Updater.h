#pragma once

#include "UpdateTypes.h"

#include <functional>
#include <memory>

namespace tubamp::update
{
namespace detail { struct Environment; }

/**
    In-plugin update: check, offer, install (docs/UPDATES.md).

    One per process. Hold it through `juce::SharedResourcePointer<Updater>` (a member of
    TubampAudioProcessor), so every plugin instance and editor in a host shares one model,
    one worker thread and one attempt at a time. Another host process has its own Updater;
    the two meet only on the disk, where the support lock serializes them.

    Threading: every public method is message-thread only, and listeners are called on
    the message thread. Network and disk work runs on one internal worker thread, started
    by the first watch(). The audio thread never touches this class.

    Lifetime: construction does no I/O beyond InstallSite::detect's stat calls. The
    destructor cancels the running fetch and joins the worker; it returns within tens of
    milliseconds, and whatever the job left on disk is collected by reconcile().

    It never installs without command::Install, never loads the new bundle, and never takes
    a URL from a caller.
*/
class Updater
{
public:
    /** The production environment: the compiled-in version and keys, NSURLSession, the
        real disk. Defined in plugin/Production.cpp, which only the plugin compiles. */
    Updater();

    /** Tests and tubamp_update_tool. */
    explicit Updater (detail::Environment);

    ~Updater();

    /** The current snapshot. No I/O. */
    View view() const;

    /** Apply a user intent. Never blocks. A command that means nothing in the current
        phase is ignored; either way every watcher gets the current View, so a stale page
        corrects itself. */
    void perform (const Command&);

    /** RAII subscription. Destroying it unsubscribes, also after the Updater is gone. */
    class Watch
    {
    public:
        Watch() noexcept = default;
        Watch (Watch&&) noexcept = default;
        Watch& operator= (Watch&&) noexcept = default;
        Watch (const Watch&) = delete;
        Watch& operator= (const Watch&) = delete;

    private:
        friend class Updater;
        struct Token;
        std::shared_ptr<Token> token;
    };

    /** "An editor is looking." The listener gets every new View. Opening a watch
        reconciles the disk and, when automatic checks are on and no process on this
        account checked in the last 24 hours, runs one check. That is the only automatic
        trigger. */
    [[nodiscard]] Watch watch (std::function<void (const View&)> listener);

private:
    struct Impl;
    std::shared_ptr<Impl> impl;

    Updater (const Updater&) = delete;
    Updater& operator= (const Updater&) = delete;
};
} // namespace tubamp::update
