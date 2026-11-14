// The shell around detail::step: it owns the Model on the message thread, runs each
// Effect (I/O on one worker thread), and feeds the results back as Events. It decides
// nothing that step() and project() decide.

#include "Updater.h"

#include "detail/EnvelopeCache.h"
#include "detail/Environment.h"
#include "detail/Machine.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace tubamp::update
{
using namespace detail;

namespace
{
    template <class... F> struct Overload : F... { using F::operator()...; };
    template <class... F> Overload (F...) -> Overload<F...>;

    const SupportPaths& supportOf (const InstallSite& site)
    {
        if (const auto* manual = std::get_if<InstallSite::NotSelfUpdatable> (&site.kind))
            return manual->support;
        return std::get<SelfUpdatable> (site.kind).layout().support;
    }

    std::optional<Offer> cachedOffer (const Environment& env)
    {
        const auto cached = loadCachedEnvelope (supportOf (env.site), *env.trust);
        if (! cached)
            return std::nullopt;
        const auto evaluation = evaluate (*cached, OfferContext { env.running, cached->version(), env.os });
        if (const auto* offer = std::get_if<Offer> (&evaluation))
            return *offer;
        return std::nullopt;
    }

    event::Reconciled reconcileJob (const Environment& env)
    {
        const auto disk = std::visit (Overload {
            [] (const SelfUpdatable& self) { return reconcile (self); },
            [] (const InstallSite::NotSelfUpdatable& manual) { return readDisk (manual); },
            [] (const InstallSite::Off&) { return DiskState {}; },
        }, env.site.kind);
        return { disk, loadPrefs (supportOf (env.site)), cachedOffer (env) };
    }

    CheckOutcome checkJob (const Environment& env, bool manual, const CancelToken& cancel)
    {
        const auto& support = supportOf (env.site);
        if (! manual && ! claimDailyCheck (support, env.now()))
            return check::NotDue {};

        Bytes body;
        const auto fetched = env.fetcher->get (envelopeUrl(), envelopeLimits(), [&] (ByteView chunk)
        {
            body.insert (body.end(), chunk.begin(), chunk.end());
            return true;
        }, cancel);
        if (! fetched)
            return fetched.error() == FetchError::tooLarge ? CheckProblem::unverified : CheckProblem::unreachable;

        const auto opened = openEnvelope (body, *env.trust);
        if (! opened)
            return CheckProblem::unverified;

        const auto cached = loadCachedEnvelope (support, *env.trust);
        const auto highestSeen = cached ? std::optional (cached->version()) : std::nullopt;
        const auto evaluation = evaluate (opened.value(), OfferContext { env.running, highestSeen, env.os });
        if (const auto* none = std::get_if<NoOffer> (&evaluation))
        {
            if (*none != NoOffer::upToDate && *none != NoOffer::needsNewerMacOS)
                return CheckProblem::unverified;
            storeEnvelope (support, body, *env.trust);
            return check::Verified {};
        }
        storeEnvelope (support, body, *env.trust);
        return check::Verified { std::get<Offer> (evaluation) };
    }

    /** One attempt, from staging to the swap. Every stage's value is a local here, so the
        Staging is closed when this returns, before the caller reconciles. */
    InstallResult installAttempt (const Environment& env, const SelfUpdatable& site, const Offer& offer,
                                  const CancelToken& cancel, const std::function<void (Event)>& send)
    {
        auto staging = Staging::create (site);
        if (! staging)
            return staging.error();
        auto sink = ArchiveSink::open (staging.value(), offer);
        if (! sink)
            return sink.error();

        using Clock = std::chrono::steady_clock;
        auto lastProgress = Clock::now();
        const auto fetched = env.fetcher->get (archiveUrl (offer), archiveLimits (offer), [&] (ByteView chunk)
        {
            if (! sink.value().write (chunk))
                return false;
            if (Clock::now() - lastProgress >= std::chrono::milliseconds (100))
            {
                lastProgress = Clock::now();
                send (event::Progress { sink.value().received() });
            }
            return true;
        }, cancel);

        if (! fetched)
        {
            switch (fetched.error())
            {
                case FetchError::cancelled:   return Cancelled {};
                case FetchError::tooLarge:    return Failure::integrity;
                case FetchError::sinkRefused: break;
                case FetchError::unreachable:
                case FetchError::httpStatus:
                case FetchError::redirectRefused: return Failure::network;
            }
            auto refused = std::move (sink.value()).finish();
            return refused ? Failure::integrity : refused.error();
        }
        send (event::Progress { sink.value().received() });

        auto archive = std::move (sink.value()).finish();
        if (! archive)
            return archive.error();
        if (cancel.requested())
            return Cancelled {};

        send (event::StageEntered { Stage::unpack });
        auto unpacked = unpack (std::move (archive).value(), staging.value());
        if (! unpacked)
            return unpacked.error();
        if (cancel.requested())
            return Cancelled {};

        send (event::StageEntered { Stage::inspect });
        auto inspected = inspect (std::move (unpacked).value(), staging.value(), offer, site);
        if (! inspected)
            return inspected.error();
        if (cancel.requested())
            return Cancelled {};

        send (event::StageEntered { Stage::commit });
        const auto committed = commit (std::move (inspected).value(), staging.value(), site);
        if (! committed)
            return committed.error();
        return Installed {};   // Committed, or AlreadyInstalled by another process
    }
}

struct Updater::Watch::Token
{
    std::function<void (const View&)> listener;
};

struct Updater::Impl
{
    explicit Impl (Environment e) : env (std::move (e)), model (Model::start (env.running, siteOf (env.site))) {}
    ~Impl() { stop(); }

    // Message thread.
    Environment env;
    Model model;
    std::optional<View> lastPushed;
    std::vector<std::weak_ptr<Watch::Token>> watchers;
    std::shared_ptr<CancelToken> installCancel;
    std::weak_ptr<Impl> self;

    // Shared with the worker.
    CancelToken shutdown;
    std::mutex lock;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    bool stopping = false;
    std::thread worker;

    void stop()
    {
        shutdown.request();
        if (installCancel)
            installCancel->request();
        {
            const std::lock_guard<std::mutex> hold (lock);
            stopping = true;
            jobs.clear();
        }
        wake.notify_all();
        if (worker.joinable())
            worker.join();
    }

    void enqueue (std::function<void()> job)
    {
        {
            const std::lock_guard<std::mutex> hold (lock);
            jobs.push_back (std::move (job));
        }
        if (! worker.joinable())
            worker = std::thread ([this] { work(); });
        wake.notify_one();
    }

    void work()
    {
        for (;;)
        {
            std::unique_lock<std::mutex> hold (lock);
            wake.wait (hold, [this] { return stopping || ! jobs.empty(); });
            if (stopping)
                return;
            auto job = std::move (jobs.front());
            jobs.pop_front();
            hold.unlock();
            job();
        }
    }

    /** Worker to message thread. A result that arrives after the Updater died is dropped. */
    void send (Event result) const
    {
        env.post ([weak = self, event = std::move (result)]
        {
            if (const auto alive = weak.lock())
                alive->dispatch (event, false);
        });
    }

    void dispatch (const Event& e, bool forcePublish)
    {
        auto s = step (model, e);
        model = std::move (s.next);
        for (const auto& effect : s.effects)
            run (effect);
        publish (forcePublish);
    }

    void publish (bool force)
    {
        const auto view = project (model);
        if (! force && lastPushed == view)
            return;
        lastPushed = view;
        const auto current = watchers;
        for (const auto& weak : current)
            if (const auto token = weak.lock())
                token->listener (view);
    }

    void run (const Effect& effect)
    {
        std::visit (Overload {
            [this] (const effect::Reconcile&)
            {
                enqueue ([this] { send (reconcileJob (env)); });
            },
            [this] (const effect::Check& c)
            {
                enqueue ([this, manual = c.manual]
                {
                    const auto outcome = checkJob (env, manual, shutdown);
                    send (event::CheckFinished { outcome, loadPrefs (supportOf (env.site)) });
                });
            },
            [this] (const effect::Install& i)
            {
                installCancel = std::make_shared<CancelToken>();
                enqueue ([this, offer = i.offer, cancel = installCancel]
                {
                    const auto* site = std::get_if<SelfUpdatable> (&env.site.kind);
                    if (site == nullptr)
                    {
                        send (event::InstallFinished { Failure::notWritable, {} });
                        return;
                    }
                    const auto result = installAttempt (env, *site, offer, *cancel, [this] (Event e) { send (std::move (e)); });
                    send (event::InstallFinished { result, reconcile (*site) });
                });
            },
            [this] (const effect::CancelInstall&)
            {
                if (installCancel)
                    installCancel->request();
            },
            [this] (const effect::SavePrefs& s)
            {
                enqueue ([this, patch = s.patch] { send (event::PrefsSaved { mutatePrefs (supportOf (env.site), patch) }); });
            },
            [this] (const effect::OpenInBrowser& o)
            {
                if (env.openInBrowser)
                    env.openInBrowser (o.url);
            },
        }, effect);
    }
};

Updater::Updater (Environment env) : impl (std::make_shared<Impl> (std::move (env)))
{
    impl->self = impl;
}

Updater::~Updater()
{
    impl->stop();
}

View Updater::view() const
{
    return project (impl->model);
}

void Updater::perform (const Command& command)
{
    impl->dispatch (event::Commanded { command }, true);
}

Updater::Watch Updater::watch (std::function<void (const View&)> listener)
{
    Watch watch;
    watch.token = std::make_shared<Watch::Token> (Watch::Token { std::move (listener) });
    std::erase_if (impl->watchers, [] (const auto& weak) { return weak.expired(); });
    impl->watchers.push_back (watch.token);
    impl->dispatch (event::WatchOpened {}, true);
    return watch;
}
} // namespace tubamp::update
