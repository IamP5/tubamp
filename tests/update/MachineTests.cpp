// Every transition of detail::step, its totality, the Install-needs-a-click property, and
// the phase precedence of detail::project. Transition names match docs/UPDATES.md.

#include "UpdateTests.h"
#include "detail/Machine.h"

#include <cstdio>
#include <random>
#include <string>

namespace tubamp::update::test
{
namespace
{
using namespace tubamp::update::detail;

int* passCount = nullptr;
int* failCount = nullptr;

void check (bool ok, const std::string& what)
{
    if (ok)
        ++*passCount;
    else
    {
        ++*failCount;
        std::printf ("  FAIL  %s\n", what.c_str());
    }
}

void group (const char* name) { std::printf ("%s\n", name); }

const Version R { 0, 3, 0 };
const Version N { 0, 4, 0 };
const Version M { 0, 5, 0 };

const Offer& offerN() { static const Offer o = *signedOffer ("0.4.0", "0.3.0"); return o; }
const Offer& offerM() { static const Offer o = *signedOffer ("0.5.0", "0.3.0"); return o; }

Model base (Site s = site::Writable {})
{
    auto m = Model::start (R, s);
    m.disk = DiskState { R, false };
    m.prefs = Prefs {};
    return m;
}

Model withOffer (Model m, const Offer& o) { m.offer = o; return m; }
Model withActivity (Model m, Activity a) { m.activity = std::move (a); return m; }
Model installing (Stage s) { return withActivity (withOffer (base(), offerN()), activity::Installing { offerN(), s, 0 }); }

Phase phaseOf (const Model& m) { return project (m).phase; }
Event cmd (Command c) { return event::Commanded { c }; }

bool unchanged (const Model& m, const Event& e)
{
    const auto s = step (m, e);
    return s.next == m && s.effects.empty();
}

struct Named { std::string name; Model model; };
struct NamedEvent { std::string name; Event event; };

std::vector<Named> models()
{
    auto failed = withOffer (base(), offerN());
    failed.lastFailure = phase::Failed { N, Failure::network };
    auto restart = withOffer (base(), offerN());
    restart.disk.installed = N;
    auto off = withOffer (Model::start (R, site::Off {}), offerN());
    return {
        { "fresh", Model::start (R, site::Writable {}) },
        { "idle", base() },
        { "available", withOffer (base(), offerN()) },
        { "checking(auto)", withActivity (withOffer (base(), offerN()), activity::Checking { false }) },
        { "checking(manual)", withActivity (base(), activity::Checking { true }) },
        { "checking(auto, CheckNow queued)",
          withActivity (withOffer (base(), offerN()), activity::Checking { false, Queued { command::CheckNow {} } }) },
        { "downloading", installing (Stage::download) },
        { "installing(unpack)", installing (Stage::unpack) },
        { "installing(inspect)", installing (Stage::inspect) },
        { "installing(commit)", installing (Stage::commit) },
        { "installing(settle)", installing (Stage::settle) },
        { "restartPending", restart },
        { "failed", failed },
        { "manual", withOffer (base (site::Manual { ManualReason::systemDomain }), offerN()) },
        { "off", off },
    };
}

std::vector<NamedEvent> events()
{
    return {
        { "WatchOpened", event::WatchOpened {} },
        { "CheckNow", cmd (command::CheckNow {}) },
        { "Install(N)", cmd (command::Install { N }) },
        { "Install(M)", cmd (command::Install { M }) },
        { "Cancel", cmd (command::Cancel {}) },
        { "SetAutoCheck", cmd (command::SetAutoCheck { false }) },
        { "MarkAnnounced", cmd (command::MarkAnnounced { N }) },
        { "MarkDisclosed", cmd (command::MarkDisclosed {}) },
        { "OpenReleasePage", cmd (command::OpenReleasePage {}) },
        { "Reconciled", event::Reconciled { DiskState { R, false }, Prefs {}, offerN() } },
        { "CheckFinished(NotDue)", event::CheckFinished { check::NotDue {}, Prefs {} } },
        { "CheckFinished(Verified)", event::CheckFinished { check::Verified { offerN() }, Prefs {} } },
        { "CheckFinished(problem)", event::CheckFinished { CheckProblem::unreachable, Prefs {} } },
        { "StageEntered(download)", event::StageEntered { Stage::download } },
        { "StageEntered(unpack)", event::StageEntered { Stage::unpack } },
        { "StageEntered(commit)", event::StageEntered { Stage::commit } },
        { "StageEntered(settle)", event::StageEntered { Stage::settle } },
        { "Progress", event::Progress { 4096 } },
        { "InstallFinished(Installed)", event::InstallFinished { Installed {}, DiskState { N, false } } },
        { "InstallFinished(Cancelled)", event::InstallFinished { Cancelled {}, DiskState { R, false } } },
        { "InstallFinished(Failure)", event::InstallFinished { Failure::integrity, DiskState { R, false } } },
        { "PrefsSaved", event::PrefsSaved { Prefs { false, true, {}, {} } } },
    };
}

/** The In and Guard columns of the transition table: when an event may do anything. */
bool mayAct (const Model& m, const Event& e)
{
    if (std::holds_alternative<site::Off> (m.site))
        return false;
    const bool idle = std::holds_alternative<activity::None> (m.activity);
    const auto* job = std::get_if<activity::Installing> (&m.activity);
    const auto* checking = std::get_if<activity::Checking> (&m.activity);
    const auto queuedAlready = [&] (const Queued& q) { return checking != nullptr && checking->then == q; };

    if (const auto* c = std::get_if<event::Commanded> (&e))
    {
        if (std::holds_alternative<command::CheckNow> (c->command))
            return idle || (checking != nullptr && ! checking->manual && ! queuedAlready (command::CheckNow {}));
        if (const auto* install = std::get_if<command::Install> (&c->command))
            return (idle || (checking != nullptr && ! queuedAlready (*install)))
                && std::holds_alternative<site::Writable> (m.site) && m.offer && m.offer->version() == install->version
                && (! m.disk.installed || *m.disk.installed < install->version);
        if (std::holds_alternative<command::Cancel> (c->command))
            return job != nullptr && job->stage < Stage::commit;
        return true;   // preference commands and OpenReleasePage always answer
    }
    if (std::holds_alternative<event::CheckFinished> (e))
        return std::holds_alternative<activity::Checking> (m.activity);
    if (const auto* s = std::get_if<event::StageEntered> (&e))
        return job != nullptr && s->stage > job->stage;
    if (std::holds_alternative<event::Progress> (e))
        return job != nullptr && job->stage == Stage::download;
    if (std::holds_alternative<event::InstallFinished> (e))
        return job != nullptr;
    return true;
}

void transitions()
{
    group ("machine.transitions");

    {
        const auto s = step (base(), event::WatchOpened {});
        check (s.effects == std::vector<Effect> { effect::Reconcile {}, effect::Check { false } }, "T1 idle: Reconcile and an automatic Check");
        check (phaseOf (s.next) == Phase { phase::Idle {} }, "T1 an automatic check is invisible");
        const auto busy = step (installing (Stage::download), event::WatchOpened {});
        check (busy.effects == std::vector<Effect> { effect::Reconcile {} } && busy.next == installing (Stage::download),
               "T1 during a job: Reconcile only");
    }
    {
        const auto fresh = Model::start (R, site::Writable {});
        const auto s = step (fresh, event::Reconciled { DiskState { R, false }, Prefs {}, offerN() });
        check (s.effects.empty() && phaseOf (s.next) == Phase { phase::Available { N } } && s.next.prefs == Prefs {},
               "T2 a cached offer becomes Available");
        check (phaseOf (step (fresh, event::Reconciled { DiskState { N, false }, Prefs {}, std::nullopt }).next) == Phase { phase::RestartPending { N } },
               "T2 a newer bundle on disk: RestartPending");
        check (phaseOf (step (fresh, event::Reconciled { DiskState { Version { 0, 2, 0 }, false }, Prefs {}, std::nullopt }).next)
                   == Phase { phase::RestartPending { Version { 0, 2, 0 } } },
               "T2 an older bundle on disk (an installer ran): RestartPending");
        check (step (withOffer (base(), offerM()), event::Reconciled { DiskState { R, false }, Prefs {}, offerN() }).next.offer == offerM(),
               "T2 a cached offer lower than the one in memory does not replace it");
        auto failed = withOffer (base(), offerN());
        failed.lastFailure = phase::Failed { N, Failure::network };
        check (! step (failed, event::Reconciled { DiskState { N, false }, Prefs {}, std::nullopt }).next.lastFailure,
               "T2 a failure for a version now installed is cleared");
        check (phaseOf (step (Model::start (R, site::Manual { ManualReason::systemDomain }),
                              event::Reconciled { DiskState { R, true }, Prefs {}, offerN() }).next)
                   == Phase { phase::Manual { N, ManualReason::systemDomain } },
               "T2 a copy that cannot update shows Manual");
    }
    {
        auto withProblem = base();
        withProblem.lastCheckProblem = CheckProblem::unreachable;
        const auto s = step (withProblem, cmd (command::CheckNow {}));
        check (s.effects == std::vector<Effect> { effect::Check { true } } && phaseOf (s.next) == Phase { phase::Checking {} }
                   && ! s.next.lastCheckProblem,
               "T3 CheckNow when idle: a manual Check, phase Checking");
        check (unchanged (installing (Stage::download), cmd (command::CheckNow {})), "T3 CheckNow during a job: ignored");
        check (unchanged (withActivity (base(), activity::Checking { true }), cmd (command::CheckNow {})), "T3 CheckNow during a manual check: ignored");
    }
    {
        const auto autoAvailable = withActivity (withOffer (base(), offerN()), activity::Checking { false });
        const auto manualIdle = withActivity (base(), activity::Checking { true });
        const auto notDue = step (autoAvailable, event::CheckFinished { check::NotDue {}, Prefs {} });
        check (phaseOf (notDue.next) == Phase { phase::Available { N } } && notDue.effects.empty(), "T4 NotDue keeps the cached offer");
        check (phaseOf (step (manualIdle, event::CheckFinished { check::Verified { offerN() }, Prefs {} }).next) == Phase { phase::Available { N } },
               "T5 Verified with an offer: Available");
        check (phaseOf (step (autoAvailable, event::CheckFinished { check::Verified { std::nullopt }, Prefs {} }).next) == Phase { phase::Idle {} },
               "T5 Verified with nothing newer: Idle");
        check (phaseOf (step (withActivity (base (site::Manual { ManualReason::readOnly }), activity::Checking { false }),
                              event::CheckFinished { check::Verified { offerN() }, Prefs {} }).next)
                   == Phase { phase::Manual { N, ManualReason::readOnly } },
               "T5 Verified on a copy that cannot update: Manual");
        check (phaseOf (step (withActivity (base(), activity::Checking { false }), event::CheckFinished { CheckProblem::unreachable, Prefs {} }).next)
                   == Phase { phase::Idle {} },
               "T6 an automatic check fails silently");
        check (phaseOf (step (manualIdle, event::CheckFinished { CheckProblem::unverified, Prefs {} }).next)
                   == Phase { phase::Idle { CheckProblem::unverified } },
               "T6 a manual check reports its problem");
        check (phaseOf (step (withActivity (withOffer (base(), offerN()), activity::Checking { true }),
                              event::CheckFinished { CheckProblem::unreachable, Prefs {} }).next)
                   == Phase { phase::Available { N } },
               "T6 a failed check never withdraws a cached offer");
        Prefs claimed;
        claimed.lastCheckAt = std::chrono::sys_seconds { std::chrono::seconds (1) };
        check (step (autoAvailable, event::CheckFinished { check::NotDue {}, claimed }).next.prefs == claimed, "T4-6 the job's prefs replace the model's");
    }
    {
        const auto s = step (withOffer (base(), offerN()), cmd (command::Install { N }));
        check (s.effects == std::vector<Effect> { effect::Install { offerN() } }, "T7 Install of the offered version: one Install effect");
        check (phaseOf (s.next) == Phase { phase::Downloading { N, 0, offerN().archive().size } }, "T7 phase Downloading against the signed size");
        auto failed = withOffer (base(), offerN());
        failed.lastFailure = phase::Failed { N, Failure::network };
        const auto retry = step (failed, cmd (command::Install { N }));
        check (retry.effects.size() == 1 && ! retry.next.lastFailure, "T7 Install after a failure retries and clears it");
        check (unchanged (installing (Stage::download), cmd (command::Install { N })), "T7 guard: an install is running");
        check (unchanged (withOffer (base (site::Manual { ManualReason::systemDomain }), offerN()), cmd (command::Install { N })), "T7 guard: copy cannot replace itself");
        check (unchanged (base(), cmd (command::Install { N })), "T7 guard: no offer");
        check (unchanged (withOffer (base(), offerN()), cmd (command::Install { M })), "T7 guard: not the version the user was shown");
        auto installed = withOffer (base(), offerN());
        installed.disk.installed = N;
        check (unchanged (installed, cmd (command::Install { N })), "T7 guard: already installed on disk");
    }
    {
        const auto autoCheck = withActivity (withOffer (base(), offerN()), activity::Checking { false });
        const auto clicked = step (autoCheck, cmd (command::Install { N }));
        check (clicked.effects.empty() && std::get<activity::Checking> (clicked.next.activity).then == Queued { command::Install { N } },
               "T19 Update during an automatic check is queued, not dropped");
        check (phaseOf (clicked.next) == Phase { phase::Checking {} }, "T19 a queued click shows Checking, so no button is offered twice");
        const auto ran = step (clicked.next, event::CheckFinished { check::NotDue {}, Prefs {} });
        check (ran.effects == std::vector<Effect> { effect::Install { offerN() } }
                   && phaseOf (ran.next) == Phase { phase::Downloading { N, 0, offerN().archive().size } },
               "T19 the queued Update installs when the check finishes");
        const auto replaced = step (clicked.next, event::CheckFinished { check::Verified { offerM() }, Prefs {} });
        check (replaced.effects.empty() && phaseOf (replaced.next) == Phase { phase::Available { M } },
               "T19 a check that finds a newer offer refuses the stale click and shows the new offer");
        const auto withdrawn = step (clicked.next, event::CheckFinished { check::Verified { std::nullopt }, Prefs {} });
        check (withdrawn.effects.empty() && phaseOf (withdrawn.next) == Phase { phase::Idle {} },
               "T19 a check that withdraws the offer installs nothing");

        const auto checkNow = step (withActivity (base(), activity::Checking { false }), cmd (command::CheckNow {}));
        check (checkNow.effects.empty() && phaseOf (checkNow.next) == Phase { phase::Checking {} },
               "T19 Check now during an automatic check is queued and shows Checking");
        const auto manual = step (checkNow.next, event::CheckFinished { check::NotDue {}, Prefs {} });
        check (manual.effects == std::vector<Effect> { effect::Check { true } } && phaseOf (manual.next) == Phase { phase::Checking {} },
               "T19 the automatic check was not due: the queued click runs a manual check");
        const auto afterProblem = step (checkNow.next, event::CheckFinished { CheckProblem::unreachable, Prefs {} });
        check (afterProblem.effects == std::vector<Effect> { effect::Check { true } },
               "T19 the automatic check failed silently: the manual check runs and will report");

        const auto latest = step (step (autoCheck, cmd (command::CheckNow {})).next, cmd (command::Install { N }));
        check (std::get<activity::Checking> (latest.next.activity).then == Queued { command::Install { N } }, "T19 the latest click wins");
        check (unchanged (withActivity (withOffer (base(), offerN()), activity::Checking { true }), cmd (command::Install { M })),
               "T19 a click the guards refuse is not queued");
        check (unchanged (withActivity (withOffer (base (site::Manual { ManualReason::systemDomain }), offerN()), activity::Checking { false }),
                          cmd (command::Install { N })),
               "T19 Update on a copy that cannot replace itself is not queued");
    }
    {
        const auto progressed = step (installing (Stage::download), event::Progress { 1000 });
        check (phaseOf (progressed.next) == Phase { phase::Downloading { N, 1000, offerN().archive().size } }, "T8 Progress updates received");
        check (unchanged (installing (Stage::unpack), event::Progress { 1000 }), "T8 Progress after the download: ignored");
        check (phaseOf (step (installing (Stage::download), event::StageEntered { Stage::unpack }).next) == Phase { phase::Installing { N } },
               "T9 Unpack moves Downloading to Installing");
        check (unchanged (installing (Stage::commit), event::StageEntered { Stage::unpack }), "T9 an earlier stage arriving late: ignored");
    }
    for (auto stage : { Stage::download, Stage::unpack, Stage::inspect })
    {
        const auto s = step (installing (stage), cmd (command::Cancel {}));
        check (s.effects == std::vector<Effect> { effect::CancelInstall {} } && s.next == installing (stage),
               "T10 Cancel before the swap: CancelInstall, model unchanged until the job answers");
    }
    for (auto stage : { Stage::commit, Stage::settle })
        check (unchanged (installing (stage), cmd (command::Cancel {})), "T10 Cancel after the swap started: ignored");
    {
        check (phaseOf (step (installing (Stage::settle), event::InstallFinished { Installed {}, DiskState { N, false } }).next)
                   == Phase { phase::RestartPending { N } },
               "T11 Installed: RestartPending");
        check (phaseOf (step (installing (Stage::download), event::InstallFinished { Cancelled {}, DiskState { R, false } }).next)
                   == Phase { phase::Available { N } },
               "T12 Cancelled: Available again");
        const auto f = step (installing (Stage::inspect), event::InstallFinished { Failure::wrongBundle, DiskState { R, false } });
        check (phaseOf (f.next) == Phase { phase::Failed { N, Failure::wrongBundle } } && f.effects.empty(), "T13 Failure: Failed, nothing else requested");
    }
    {
        const auto off = step (base(), cmd (command::SetAutoCheck { false }));
        check (off.effects == std::vector<Effect> { effect::SavePrefs { PrefsPatch { false, true, {} } } }, "T14 SetAutoCheck saves autoCheck and disclosed");
        check (! project (off.next).autoCheck && ! project (off.next).disclose, "T14 the view shows checks off and no disclosure");
        const auto available = withOffer (base(), offerN());
        check (project (available).announce == N, "an unannounced offer is announced");
        const auto announced = step (available, cmd (command::MarkAnnounced { N }));
        check (announced.effects == std::vector<Effect> { effect::SavePrefs { PrefsPatch { {}, {}, N } } } && ! project (announced.next).announce,
               "T15 MarkAnnounced saves and clears announce");
        check (project (base()).disclose, "disclosure is pending until marked");
        const auto disclosed = step (base(), cmd (command::MarkDisclosed {}));
        check (disclosed.effects == std::vector<Effect> { effect::SavePrefs { PrefsPatch { {}, true, {} } } } && ! project (disclosed.next).disclose,
               "T16 MarkDisclosed saves and clears disclose");
        const Prefs saved { false, true, {}, M };
        check (step (base(), event::PrefsSaved { saved }).next.prefs == saved, "T17 the disk's answer replaces the model's prefs");
        const auto unloaded = step (Model::start (R, site::Writable {}), cmd (command::MarkDisclosed {}));
        check (! unloaded.next.prefs && unloaded.effects.size() == 1, "T16 before prefs are read: the save is still requested");
    }
    {
        const auto withO = step (withOffer (base(), offerN()), cmd (command::OpenReleasePage {}));
        check (withO.effects == std::vector<Effect> { effect::OpenInBrowser { releasePageUrl (N) } }, "T18 OpenReleasePage opens the offered version's page");
        check (step (base(), cmd (command::OpenReleasePage {})).effects == std::vector<Effect> { effect::OpenInBrowser { releasePageUrl (R) } },
               "T18 with no offer, the running version's page");
    }
}

void totality()
{
    group ("machine.totality");
    int pairs = 0, ignored = 0;
    bool allQuiet = true;
    for (const auto& [mname, model] : models())
        for (const auto& [ename, ev] : events())
        {
            ++pairs;
            if (mayAct (model, ev))
                continue;
            ++ignored;
            if (! unchanged (model, ev))
            {
                allQuiet = false;
                check (false, "ignored pair changed something: " + mname + " x " + ename);
            }
        }
    check (allQuiet, "every pair outside the transition table leaves the model unchanged with no effects ("
                         + std::to_string (ignored) + " of " + std::to_string (pairs) + " pairs)");

    bool offAbsorbs = true;
    for (const auto& [ename, ev] : events())
        offAbsorbs = offAbsorbs && unchanged (models().back().model, ev);
    check (offAbsorbs, "Off absorbs every event and command");
}

void installNeedsAClick()
{
    group ("machine.install-needs-a-click");
    auto pool = events();
    std::erase_if (pool, [] (const NamedEvent& e) { return e.name.starts_with ("Install("); });
    const auto starts = models();

    std::mt19937 rng (20261114);
    int installs = 0;
    for (int walk = 0; walk < 2000; ++walk)
    {
        auto m = starts[rng() % starts.size()].model;
        for (int i = 0; i < 40; ++i)
        {
            auto s = step (m, pool[rng() % pool.size()].event);
            for (const auto& eff : s.effects)
                installs += std::holds_alternative<effect::Install> (eff) ? 1 : 0;
            m = std::move (s.next);
        }
    }
    check (installs == 0, "80000 random steps without Commanded Install request no install");

    int withClick = 0;
    auto m = withOffer (base(), offerN());
    for (const auto& eff : step (m, cmd (command::Install { N })).effects)
        withClick += std::holds_alternative<effect::Install> (eff) ? 1 : 0;
    check (withClick == 1, "and the walk could see one: the click requests exactly one");
}

void projection()
{
    group ("machine.project");
    auto m = withOffer (base(), offerN());
    m.disk.installed = N;
    m.lastFailure = phase::Failed { N, Failure::network };

    auto off = m;
    off.site = site::Off {};
    check (project (off).phase == Phase { phase::Off {} } && ! project (off).disclose, "Off wins over everything, and discloses nothing");

    check (phaseOf (withActivity (m, activity::Installing { offerN(), Stage::download, 7 })) == Phase { phase::Downloading { N, 7, offerN().archive().size } },
           "Downloading wins over RestartPending and Failed");
    check (phaseOf (withActivity (m, activity::Installing { offerN(), Stage::settle, 0 })) == Phase { phase::Installing { N } },
           "a later stage is Installing");
    check (phaseOf (withActivity (m, activity::Checking { true })) == Phase { phase::Checking {} }, "a manual check wins over RestartPending");
    check (phaseOf (withActivity (m, activity::Checking { false })) == Phase { phase::RestartPending { N } }, "an automatic check is invisible");
    check (phaseOf (m) == Phase { phase::RestartPending { N } }, "RestartPending wins over Failed and the offer");

    auto failed = m;
    failed.disk.installed = R;
    check (phaseOf (failed) == Phase { phase::Failed { N, Failure::network } }, "Failed wins over Available");
    check (! project (failed).announce, "no toast while the phase is Failed");

    auto manual = withOffer (base (site::Manual { ManualReason::noAtomicSwap }), offerN());
    check (phaseOf (manual) == Phase { phase::Manual { N, ManualReason::noAtomicSwap } } && project (manual).announce == N,
           "Manual on a copy that cannot update, and it is announced");
    check (phaseOf (withOffer (base(), offerN())) == Phase { phase::Available { N } }, "Available");
    auto problem = base();
    problem.lastCheckProblem = CheckProblem::unreachable;
    check (phaseOf (problem) == Phase { phase::Idle { CheckProblem::unreachable } }, "Idle carries the manual check's problem");

    auto seen = withOffer (base(), offerN());
    seen.prefs->announced = N;
    check (! project (seen).announce, "a version already announced is not announced again");
    seen.offer = offerM();
    check (project (seen).announce == M, "a higher version is announced");
    check (! project (withOffer (Model::start (R, site::Writable {}), offerN())).announce
               && ! project (Model::start (R, site::Writable {})).disclose,
           "nothing is toasted before prefs are read");

    auto system = base();
    system.disk.systemCopy = true;
    check (project (system).systemCopy && project (system).running == R && project (system).autoCheck, "facts pass through to the view");
}
} // namespace

void runMachineTests (int& pass, int& fail)
{
    passCount = &pass;
    failCount = &fail;
    transitions();
    totality();
    installNeedsAClick();
    projection();
}
} // namespace tubamp::update::test
