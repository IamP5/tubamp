// The Updater service end to end: the real shell, state machine, disk chain and envelope
// code, with a scripted Fetcher and a temporary home for every root. The test thread is
// the message thread: Environment::post queues jobs that pump() runs, so every listener
// call happens here, as it does on JUCE's message thread.

#include "DiskFixtures.h"
#include "UpdateTests.h"

#include "Updater.h"
#include "detail/Environment.h"
#include "detail/Files.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <thread>

namespace tubamp::update::test
{
using namespace detail;

namespace
{
int* passedCount = nullptr;
int* failedCount = nullptr;

void check (bool ok, const std::string& what)
{
    if (ok)
        ++*passedCount;
    else
    {
        ++*failedCount;
        std::printf ("  FAIL  %s\n", what.c_str());
    }
}

void group (const char* name) { std::printf ("%s\n", name); }

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

Version v (std::string_view s) { return *Version::parse (s); }

const TestSigner& signer() { static const TestSigner s; return s; }
const TrustRoot& trust() { static const TrustRoot t ({ signer().publicKey() }); return t; }

Bytes envelopeFor (const Bytes& archive, std::string_view version)
{
    const auto payload = std::string ("{\"product\":\"com.t0audio.tubamp\",\"version\":\"") + std::string (version)
                       + "\",\"minimumMacOS\":\"11.0\",\"archive\":{\"sha256\":\""
                       + hex (sha256 (ByteView (archive.data(), archive.size()))) + "\",\"size\":"
                       + std::to_string (archive.size()) + "}}";
    return makeEnvelope (payload, { &signer() });
}

std::string archiveUrlFor (std::string_view version)
{
    const auto s = std::string (version);
    return "https://github.com/IamP5/tubamp/releases/download/v" + s + "/tubamp-" + s + "-macos-universal.zip";
}

class Loop
{
public:
    void post (std::function<void()> job)
    {
        const std::lock_guard<std::mutex> hold (lock);
        jobs.push_back (std::move (job));
        wake.notify_all();
    }

    /** Runs posted jobs until `done()` holds. False on timeout. */
    bool pumpUntil (const std::function<bool()>& done, milliseconds limit = milliseconds (30000))
    {
        const auto deadline = Clock::now() + limit;
        while (! done())
        {
            if (Clock::now() > deadline)
                return false;
            runFor (milliseconds (10));
        }
        return true;
    }

    void runFor (milliseconds span)
    {
        std::deque<std::function<void()>> ready;
        {
            std::unique_lock<std::mutex> hold (lock);
            wake.wait_for (hold, span, [this] { return ! jobs.empty(); });
            ready.swap (jobs);
        }
        for (auto& job : ready)
            job();
    }

private:
    std::mutex lock;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
};

struct Script
{
    Bytes body;
    std::optional<FetchError> fail;
    std::size_t failAfter = 0;          // bytes delivered before `fail`
    std::size_t chunk = 64 * 1024;
    milliseconds perChunk { 0 };
    bool enforceCap = true;             // like the system fetcher: tooLarge before the sink sees the byte

    static Script of (Bytes b) { Script s; s.body = std::move (b); return s; }
    static Script failing (FetchError e) { Script s; s.fail = e; return s; }
};

class FakeFetcher final : public Fetcher
{
public:
    void serve (const std::string& url, Script s)
    {
        const std::lock_guard<std::mutex> hold (lock);
        scripts[url] = std::move (s);
    }

    std::vector<std::string> requests() const
    {
        const std::lock_guard<std::mutex> hold (lock);
        return asked;
    }

    std::size_t count (const std::string& url) const
    {
        const auto all = requests();
        return (std::size_t) std::count (all.begin(), all.end(), url);
    }

    Result<std::monostate, FetchError> get (const Url& url, const FetchLimits& limits, const ByteSink& sink,
                                            const CancelToken& cancel) override
    {
        Script s;
        {
            const std::lock_guard<std::mutex> hold (lock);
            asked.push_back (url.text());
            const auto it = scripts.find (url.text());
            if (it == scripts.end())
                return FetchError::httpStatus;
            s = it->second;
        }
        if (! HopPolicy (limits.maxRedirects).admit (url.text()))
            return FetchError::redirectRefused;

        std::uint64_t sent = 0;
        for (std::size_t offset = 0; offset < s.body.size() || (offset == 0 && s.body.empty()); offset += s.chunk)
        {
            for (auto waited = milliseconds (0); waited < s.perChunk; waited += milliseconds (2))
            {
                if (cancel.requested())
                    return FetchError::cancelled;
                std::this_thread::sleep_for (milliseconds (2));
            }
            if (cancel.requested())
                return FetchError::cancelled;
            if (s.fail && offset >= s.failAfter)
                return *s.fail;
            if (s.body.empty())
                break;
            const auto n = std::min (s.chunk, s.body.size() - offset);
            if (s.enforceCap && sent + n > limits.maxBytes)
                return FetchError::tooLarge;
            if (! sink (ByteView (s.body.data() + offset, n)))
                return FetchError::sinkRefused;
            sent += n;
        }
        if (s.fail)
            return *s.fail;
        return std::monostate {};
    }

private:
    mutable std::mutex lock;
    std::map<std::string, Script> scripts;
    std::vector<std::string> asked;
};

/** One service under test: its environment, its listener's history, its browser. */
struct Rig
{
    Rig (const Home& home, Loop& l, std::shared_ptr<FakeFetcher> f, InstallSite site, Version running = v ("0.3.0"))
        : loop (l), fetcher (std::move (f))
    {
        Environment env;
        env.running = running;
        env.site = std::move (site);
        env.trust = &trust();
        env.fetcher = fetcher;
        env.os = { 15, 0 };
        env.now = [] { return std::chrono::sys_seconds (std::chrono::seconds (1'800'000'000)); };
        env.post = [&l] (std::function<void()> job) { l.post (std::move (job)); };
        env.openInBrowser = [this] (const Url& url) { opened.push_back (url.text()); };
        updater = std::make_unique<Updater> (std::move (env));
        (void) home;
    }

    void open()
    {
        watch = updater->watch ([this] (const View& view) { seen.push_back (view); });
    }

    template <class P>
    bool waitFor (milliseconds limit = milliseconds (30000))
    {
        return loop.pumpUntil ([this] { return std::holds_alternative<P> (updater->view().phase); }, limit);
    }

    template <class P>
    bool sawPhase() const
    {
        return std::any_of (seen.begin(), seen.end(), [] (const View& view) { return std::holds_alternative<P> (view.phase); });
    }

    template <class P>
    std::optional<P> phase() const
    {
        const auto view = updater->view();
        if (const auto* p = std::get_if<P> (&view.phase))
            return *p;
        return std::nullopt;
    }

    Loop& loop;
    std::shared_ptr<FakeFetcher> fetcher;
    std::unique_ptr<Updater> updater;
    Updater::Watch watch;
    std::vector<View> seen;
    std::vector<std::string> opened;
};

/** A temporary home whose live bundle is 0.3.0, and a release 0.4.0 the fetcher serves.
    The disclosure toast was already shown on this account unless `firstOpen`. */
struct World
{
    explicit World (bool firstOpen = false) : home (dir.path())
    {
        home.installLive (fixtures().bundle ("0.3.0"));
        if (! firstOpen)
            mutatePrefs (home.support, PrefsPatch { {}, true, {} });
        archive = readAll (fixtures().zip ("0.4.0"));
        fetcher->serve (std::string (envelopeUrl().text()), Script::of (envelopeFor (archive, "0.4.0")));
        fetcher->serve (archiveUrlFor ("0.4.0"), Script::of (archive));
    }

    Script slowArchive (std::size_t pieces = 20, milliseconds each = milliseconds (40)) const
    {
        auto s = Script::of (archive);
        s.chunk = std::max<std::size_t> (1, archive.size() / pieces);
        s.perChunk = each;
        return s;
    }

    std::optional<Version> liveVersion() const { return versionOf (home.live()); }
    bool clean() const { return home.stagingDirs().empty(); }

    TempDir dir;
    Home home;
    Loop loop;
    std::shared_ptr<FakeFetcher> fetcher = std::make_shared<FakeFetcher>();
    Bytes archive;
};

void offAndInert()
{
    group ("service: a build with updates off is inert");
    World w (true);
    Rig rig (w.home, w.loop, w.fetcher, InstallSite { InstallSite::Off {} });
    rig.open();
    rig.updater->perform (command::CheckNow {});
    rig.updater->perform (command::Install { v ("0.4.0") });
    w.loop.runFor (milliseconds (100));
    check (std::holds_alternative<phase::Off> (rig.updater->view().phase), "the phase is off");
    check (w.fetcher->requests().empty(), "nothing is fetched, not even on Check now");
    check (! fs::exists (w.home.support.dir), "nothing is written to the support folder");
    check (! rig.seen.empty() && std::holds_alternative<phase::Off> (rig.seen.back().phase), "watchers still get the off view");
}

void happyPath()
{
    group ("service: check, offer, download with progress, verify, unpack, swap, restart pending");
    World w;
    w.fetcher->serve (archiveUrlFor ("0.4.0"), w.slowArchive (8, milliseconds (60)));
    Rig rig (w.home, w.loop, w.fetcher, w.home.detect());
    rig.open();

    check (rig.waitFor<phase::Available>(), "the automatic check on first watch offers 0.4.0");
    const auto offered = rig.updater->view();
    check (offered.announce == v ("0.4.0") && ! offered.disclose, "the offer is announced once, after the disclosure was shown");
    check (w.fetcher->count (std::string (envelopeUrl().text())) == 1, "one envelope GET");
    check (fs::exists (w.home.support.dir / "latest.signed"), "the verified envelope is cached");

    rig.updater->perform (command::MarkAnnounced { v ("0.4.0") });
    w.loop.pumpUntil ([&] { return ! rig.updater->view().announce; });
    check (! rig.updater->view().announce, "MarkAnnounced clears the announcement");

    rig.updater->perform (command::Install { v ("0.4.0") });
    check (rig.waitFor<phase::RestartPending>(), "the install ends in restart pending");

    std::vector<std::uint64_t> progress;
    std::uint64_t total = 0;
    for (const auto& view : rig.seen)
        if (const auto* d = std::get_if<phase::Downloading> (&view.phase))
        {
            progress.push_back (d->received);
            total = d->total;
        }
    check (total == w.archive.size(), "downloading reports the signed size as the total");
    check (std::is_sorted (progress.begin(), progress.end()) && ! progress.empty() && progress.back() == total,
           "progress only grows and ends at the total");
    check (std::any_of (progress.begin(), progress.end(), [&] (auto r) { return r > 0 && r < total; }),
           "a mid-download progress value reaches the watcher");
    check (rig.sawPhase<phase::Installing>(), "installing is shown after the download");

    const auto pending = rig.phase<phase::RestartPending>();
    check (pending  && pending->installed == v ("0.4.0"), "restart pending names 0.4.0");
    check (w.liveVersion() == v ("0.4.0"), "the live bundle on disk is 0.4.0");
    check (versionOf (w.home.previous()) == v ("0.3.0"), "0.3.0 is kept as the previous bundle");
    check (w.clean(), "no staging directory is left");
    check (w.fetcher->count (archiveUrlFor ("0.4.0")) == 1, "one archive GET");

    group ("service: re-opening an editor after the swap");
    rig.updater->perform (command::Install { v ("0.4.0") });
    auto again = rig.updater->watch ([] (const View&) {});
    w.loop.runFor (milliseconds (200));
    check (rig.phase<phase::RestartPending>().has_value(), "still restart pending");
    check (w.fetcher->count (archiveUrlFor ("0.4.0")) == 1, "Install of an installed version starts nothing");
    check (w.fetcher->count (std::string (envelopeUrl().text())) == 1, "the daily slot is taken: no second envelope GET");

    rig.updater->perform (command::OpenReleasePage {});
    check (rig.opened == std::vector<std::string> { "https://github.com/IamP5/tubamp/releases/tag/v0.4.0" },
           "OpenReleasePage opens the compiled-in page of the offered version");
}

void ignoredCommandsStillPublish()
{
    group ("service: an ignored command republishes, and a dropped watch hears nothing");
    World w;
    Rig rig (w.home, w.loop, w.fetcher, w.home.detect());
    rig.open();
    rig.waitFor<phase::Available>();
    const auto before = rig.seen.size();
    rig.updater->perform (command::Install { v ("0.9.9") });
    check (rig.seen.size() == before + 1 && rig.seen.back() == rig.updater->view(),
           "an Install for a version never offered is ignored and the current view is pushed");
    check (w.fetcher->count (archiveUrlFor ("0.9.9")) == 0, "and nothing is fetched");

    rig.watch = {};
    rig.updater->perform (command::CheckNow {});
    w.loop.runFor (milliseconds (200));
    check (rig.seen.size() == before + 1, "a destroyed Watch is never called again");
}

void checkProblems()
{
    group ("service: check problems are reported only after Check now");
    {
        World w;
        w.fetcher->serve (std::string (envelopeUrl().text()), Script::failing (FetchError::unreachable));
        Rig rig (w.home, w.loop, w.fetcher, w.home.detect());
        rig.open();
        w.loop.pumpUntil ([&] { return w.fetcher->requests().size() == 1; });
        w.loop.runFor (milliseconds (100));
        auto idle = rig.phase<phase::Idle>();
        check (idle  && ! idle->problem, "an automatic check that cannot connect stays silent");

        rig.updater->perform (command::CheckNow {});
        check (rig.sawPhase<phase::Checking>(), "Check now shows checking");
        w.loop.pumpUntil ([&] { const auto i = rig.phase<phase::Idle>(); return i && i->problem; });
        idle = rig.phase<phase::Idle>();
        check (idle  && idle->problem == CheckProblem::unreachable, "Check now reports unreachable");
    }
    {
        World w;
        auto forged = envelopeFor (w.archive, "0.4.0");
        forged.back() ^= 0x01;   // the payload's closing brace changes, so the signature no longer holds
        w.fetcher->serve (std::string (envelopeUrl().text()), Script::of (forged));
        Rig rig (w.home, w.loop, w.fetcher, w.home.detect());
        rig.open();
        w.loop.pumpUntil ([&] { return w.fetcher->requests().size() == 1; });
        w.loop.runFor (milliseconds (100));
        rig.updater->perform (command::CheckNow {});
        w.loop.pumpUntil ([&] { const auto i = rig.phase<phase::Idle>(); return i && i->problem; }, milliseconds (10000));
        const auto idle = rig.phase<phase::Idle>();
        check (idle  && idle->problem == CheckProblem::unverified, "a tampered envelope is unverified, and offers nothing");
        check (! fs::exists (w.home.support.dir / "latest.signed"), "unverified bytes are never cached");
    }
    {
        World w;
        Bytes big (maxEnvelopeBytes + 1, 'x');
        w.fetcher->serve (std::string (envelopeUrl().text()), Script::of (big));
        Rig rig (w.home, w.loop, w.fetcher, w.home.detect());
        rig.open();
        w.loop.pumpUntil ([&] { return w.fetcher->requests().size() == 1; });
        w.loop.runFor (milliseconds (100));
        rig.updater->perform (command::CheckNow {});
        w.loop.pumpUntil ([&] { const auto i = rig.phase<phase::Idle>(); return i && i->problem; }, milliseconds (10000));
        const auto idle = rig.phase<phase::Idle>();
        check (idle  && idle->problem == CheckProblem::unverified, "an envelope above 16 KiB is cut off and unverified");
    }
}

void installFailures()
{
    group ("service: every install failure leaves the installed version and a clean disk");

    struct Case
    {
        const char* what;
        Failure expected;
        std::function<void (World&)> arrange;
    };

    const std::vector<Case> cases {
        { "the connection drops mid-download", Failure::network, [] (World& w)
          {
              auto s = w.slowArchive (8, milliseconds (0));
              s.fail = FetchError::unreachable;
              s.failAfter = w.archive.size() / 2;
              w.fetcher->serve (archiveUrlFor ("0.4.0"), s);
          } },
        { "a redirect is refused", Failure::network, [] (World& w)
          {
              w.fetcher->serve (archiveUrlFor ("0.4.0"), Script::failing (FetchError::redirectRefused));
          } },
        { "one byte differs from the signed hash", Failure::integrity, [] (World& w)
          {
              auto body = w.archive;
              body[body.size() / 2] ^= 0xff;
              w.fetcher->serve (archiveUrlFor ("0.4.0"), Script::of (body));
          } },
        { "the server sends one byte past the signed size", Failure::integrity, [] (World& w)
          {
              auto body = w.archive;
              body.push_back (0);
              w.fetcher->serve (archiveUrlFor ("0.4.0"), Script::of (body));
          } },
        { "a fetcher without a cap: the sink refuses the extra byte", Failure::integrity, [] (World& w)
          {
              auto body = w.archive;
              body.push_back (0);
              auto s = Script::of (body);
              s.enforceCap = false;
              w.fetcher->serve (archiveUrlFor ("0.4.0"), s);
          } },
        { "an archive entry escapes the bundle", Failure::archive, [] (World& w)
          {
              auto entries = forgedBundle();
              entries.emplace_back ("tubamp.component/../escape", 0100644, "x");
              const auto zip = forgeZip (entries);
              w.fetcher->serve (std::string (envelopeUrl().text()), Script::of (envelopeFor (zip, "0.4.0")));
              w.fetcher->serve (archiveUrlFor ("0.4.0"), Script::of (zip));
          } },
        { "the bundle has another bundle id", Failure::wrongBundle, [] (World& w)
          {
              const auto zip = readAll (fixtures().zip ("0.4.0", Variant::otherBundleId));
              w.fetcher->serve (std::string (envelopeUrl().text()), Script::of (envelopeFor (zip, "0.4.0")));
              w.fetcher->serve (archiveUrlFor ("0.4.0"), Script::of (zip));
          } },
        { "the staging folder is read-only", Failure::notWritable, [] (World& w)
          {
              fs::create_directories (w.home.privateDir);
              ::chmod (w.home.privateDir.c_str(), 0500);
          } },
    };

    for (const auto& c : cases)
    {
        World w;
        Rig rig (w.home, w.loop, w.fetcher, w.home.detect());
        c.arrange (w);
        rig.open();
        if (! rig.waitFor<phase::Available>())
        {
            check (false, std::string (c.what) + ": no offer to install");
            continue;
        }
        rig.updater->perform (command::Install { v ("0.4.0") });
        const bool ended = rig.waitFor<phase::Failed>();
        const auto failed = rig.phase<phase::Failed>();
        check (ended && failed  && failed->why == c.expected && failed->version == v ("0.4.0"),
               std::string (c.what) + ": failed with the expected reason");
        check (w.liveVersion() == v ("0.3.0"), std::string (c.what) + ": 0.3.0 is still installed");
        ::chmod (w.home.privateDir.c_str(), 0700);
        check (w.clean(), std::string (c.what) + ": no staging directory is left");
    }
}

void cancelMidDownload()
{
    group ("service: cancel mid-download");
    World w;
    w.fetcher->serve (archiveUrlFor ("0.4.0"), w.slowArchive (50, milliseconds (40)));
    Rig rig (w.home, w.loop, w.fetcher, w.home.detect());
    rig.open();
    rig.waitFor<phase::Available>();
    rig.updater->perform (command::Install { v ("0.4.0") });
    w.loop.pumpUntil ([&] { const auto d = rig.phase<phase::Downloading>(); return d && d->received > 0; });

    const auto start = Clock::now();
    rig.updater->perform (command::Cancel {});
    const bool back = rig.waitFor<phase::Available> (milliseconds (5000));
    const auto took = std::chrono::duration_cast<milliseconds> (Clock::now() - start).count();
    check (back, "Cancel returns to the offer");
    const auto cancelBound = (long long) (500 * wallClockSlack());
    check (took < cancelBound, "the offer is back " + std::to_string (took) + " ms after Cancel (bound " + std::to_string (cancelBound) + " ms)");
    check (w.liveVersion() == v ("0.3.0") && w.clean(), "nothing was installed and no staging directory is left");
    std::printf ("  note  cancel to offer: %lld ms\n", (long long) took);
}

void destroyMidDownload()
{
    group ("service: destroying the service mid-download");
    World w;
    w.fetcher->serve (archiveUrlFor ("0.4.0"), w.slowArchive (50, milliseconds (40)));
    auto rig = std::make_unique<Rig> (w.home, w.loop, w.fetcher, w.home.detect());
    rig->open();
    rig->waitFor<phase::Available>();
    rig->updater->perform (command::Install { v ("0.4.0") });
    w.loop.pumpUntil ([&] { const auto d = rig->phase<phase::Downloading>(); return d && d->received > 0; });

    std::size_t heard = 0;
    auto late = rig->updater->watch ([&] (const View&) { ++heard; });
    heard = 0;
    const auto start = Clock::now();
    rig->updater.reset();
    const auto took = std::chrono::duration_cast<std::chrono::microseconds> (Clock::now() - start).count();
    w.loop.runFor (milliseconds (300));
    late = {};
    const auto teardownBound = (long long) (250 * wallClockSlack());
    check (took < teardownBound * 1000, "~Updater returns in " + std::to_string (took / 1000) + " ms mid-download (bound " + std::to_string (teardownBound) + " ms)");
    check (heard == 0, "no listener runs after the Updater is gone, though results were still queued");
    check (w.liveVersion() == v ("0.3.0") && w.clean(), "the disk is unchanged and the attempt was collected");
    std::printf ("  note  ~Updater mid-download: %.1f ms\n", double (took) / 1000.0);
}

void firstOpenWaitsForTheDisclosure()
{
    group ("service: the first automatic check waits until the disclosure was shown");
    const auto envelopeGets = [] (const World& w) { return w.fetcher->count (std::string (envelopeUrl().text())); };
    {
        World w (true);
        Rig rig (w.home, w.loop, w.fetcher, w.home.detect());
        rig.open();
        w.loop.pumpUntil ([&] { return rig.updater->view().disclose; });
        w.loop.runFor (milliseconds (300));
        check (rig.updater->view().disclose && envelopeGets (w) == 0,
               "the first editor open shows the disclosure and makes no request");
        rig.updater->perform (command::MarkDisclosed {});
        w.loop.pumpUntil ([&] { return ! rig.updater->view().disclose; });
        w.loop.runFor (milliseconds (200));
        check (envelopeGets (w) == 0, "showing the disclosure does not itself fire a request");

        Rig later (w.home, w.loop, w.fetcher, w.home.detect());
        later.open();
        check (later.waitFor<phase::Available>() && envelopeGets (w) == 1, "the next open, with checks left on, checks once");
    }
    {
        World w (true);
        Rig rig (w.home, w.loop, w.fetcher, w.home.detect());
        rig.open();
        rig.updater->perform (command::SetAutoCheck { false });
        w.loop.runFor (milliseconds (200));
        Rig later (w.home, w.loop, w.fetcher, w.home.detect());
        later.open();
        w.loop.runFor (milliseconds (300));
        check (envelopeGets (w) == 0, "Turn off on the disclosure means no request, now or on the next open");
    }
    {
        World w (true);
        Rig rig (w.home, w.loop, w.fetcher, w.home.detect());
        rig.open();
        w.loop.runFor (milliseconds (200));
        rig.updater->perform (command::CheckNow {});
        check (rig.waitFor<phase::Available>() && envelopeGets (w) == 1, "Check now checks at once, disclosure or not");
    }
}

void destroyWhileAnotherProcessHoldsTheLock()
{
    group ("service: destroying the service while another process holds the support lock");
    World w;
    Child holder ({ "hold-support", w.home.support.dir.string() });
    check (holder.waitReady(), "another process holds the support lock, as a host stopped in a debugger would");
    std::thread letGo ([&] { std::this_thread::sleep_for (milliseconds (3000)); holder.release(); });

    auto rig = std::make_unique<Rig> (w.home, w.loop, w.fetcher, w.home.detect());
    rig->open();
    std::this_thread::sleep_for (milliseconds (200));   // the worker is now waiting in reconcile()
    const auto start = Clock::now();
    rig->updater.reset();
    const auto took = std::chrono::duration_cast<milliseconds> (Clock::now() - start).count();
    letGo.join();
    holder.wait();
    const auto heldBound = (long long) (100 * wallClockSlack());
    check (took < heldBound, "~Updater returns in " + std::to_string (took) + " ms while the lock is held for 3 s (bound " + std::to_string (heldBound) + " ms)");
    std::printf ("  note  ~Updater with the support lock held elsewhere: %lld ms\n", (long long) took);

    Rig next (w.home, w.loop, w.fetcher, w.home.detect());
    next.open();
    check (next.waitFor<phase::Available>(), "once the lock is free, the next open reconciles and checks as usual");
}

void twoServices()
{
    group ("service: two services on one account (two hosts)");
    World w;
    w.fetcher->serve (archiveUrlFor ("0.4.0"), w.slowArchive (10, milliseconds (30)));
    Rig a (w.home, w.loop, w.fetcher, w.home.detect());
    Rig b (w.home, w.loop, w.fetcher, w.home.detect());
    a.open();
    check (a.waitFor<phase::Available>(), "the first service checks and offers");
    b.open();
    check (b.waitFor<phase::Available>(), "the second offers from the cached envelope");
    w.loop.runFor (milliseconds (200));   // its own automatic check (not due) must end before Install is accepted
    check (w.fetcher->count (std::string (envelopeUrl().text())) == 1, "the daily slot holds across services: one envelope GET");

    a.updater->perform (command::Install { v ("0.4.0") });
    b.updater->perform (command::Install { v ("0.4.0") });
    const bool both = w.loop.pumpUntil ([&]
    {
        return std::holds_alternative<phase::RestartPending> (a.updater->view().phase)
            && std::holds_alternative<phase::RestartPending> (b.updater->view().phase);
    });
    check (both, "both end in restart pending: one swapped, the other found it installed");
    check (w.liveVersion() == v ("0.4.0") && versionOf (w.home.previous()) == v ("0.3.0"), "one swap: 0.4.0 live, 0.3.0 previous");
    check (w.clean(), "no staging directory is left");
}

void notSelfUpdatable()
{
    group ("service: a copy that cannot replace itself (the Standalone)");
    World w;
    const auto app = w.dir.path() / "Applications" / "tubamp.app";
    fs::create_directories (app / "Contents" / "MacOS");
    Rig rig (w.home, w.loop, w.fetcher,
             InstallSite::detect (app / "Contents" / "MacOS" / "tubamp", w.home.home, true, true, w.home.system));
    rig.open();
    check (rig.waitFor<phase::Manual>(), "the offer shows as manual");
    const auto manual = rig.phase<phase::Manual>();
    check (manual  && manual->why == ManualReason::notAnAudioUnit && manual->version == v ("0.4.0"),
           "manual, because it is not an Audio Unit");
    rig.updater->perform (command::Install { v ("0.4.0") });
    w.loop.runFor (milliseconds (200));
    check (w.fetcher->count (archiveUrlFor ("0.4.0")) == 0 && w.liveVersion() == v ("0.3.0"),
           "Install downloads nothing and swaps nothing");
}

void restartPendingFromDisk()
{
    group ("service: restart pending is read from the disk");
    World w;
    w.home.installLive (fixtures().bundle ("0.4.0"));
    Rig rig (w.home, w.loop, w.fetcher, w.home.detect());
    rig.open();
    check (rig.waitFor<phase::RestartPending>(), "a live bundle newer than the running code means restart pending");
}
} // namespace

void runServiceTests (int& passed, int& failed)
{
    passedCount = &passed;
    failedCount = &failed;
    offAndInert();
    happyPath();
    firstOpenWaitsForTheDisclosure();
    ignoredCommandsStillPublish();
    checkProblems();
    installFailures();
    cancelMidDownload();
    destroyMidDownload();
    destroyWhileAnotherProcessHoldsTheLock();
    twoServices();
    notSelfUpdatable();
    restartPendingFromDisk();
}
} // namespace tubamp::update::test
