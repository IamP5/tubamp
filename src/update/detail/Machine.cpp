#include "Machine.h"

namespace tubamp::update::detail
{
namespace
{
    template <class... F> struct Overload : F... { using F::operator()...; };
    template <class... F> Overload (F...) -> Overload<F...>;

    bool idle (const Model& m) { return std::holds_alternative<activity::None> (m.activity); }
    bool writable (const Model& m) { return std::holds_alternative<site::Writable> (m.site); }

    Step same (const Model& m) { return { m, {} }; }

    Step savePrefs (Model m, const PrefsPatch& patch)
    {
        if (m.prefs)
            apply (*m.prefs, patch);
        return { std::move (m), { effect::SavePrefs { patch } } };
    }

    // T19: the latest click during a check waits for it instead of being dropped.
    Step queue (const Model& m, const Queued& click)
    {
        auto next = m;
        auto& checking = std::get<activity::Checking> (next.activity);
        if (checking.then == click)
            return same (m);
        checking.then = click;
        return { std::move (next), {} };
    }

    Step onCommand (const Model& m, const Command& command)
    {
        const auto* checking = std::get_if<activity::Checking> (&m.activity);
        return std::visit (Overload {
            // T3
            [&] (const command::CheckNow& c) -> Step
            {
                if (checking != nullptr && ! checking->manual)
                    return queue (m, c);
                if (! idle (m))
                    return same (m);
                auto next = m;
                next.activity = activity::Checking { true };
                next.lastCheckProblem.reset();
                return { std::move (next), { effect::Check { true } } };
            },
            // T7
            [&] (const command::Install& c) -> Step
            {
                const bool notInstalled = ! m.disk.installed || *m.disk.installed < c.version;
                if (! writable (m) || ! m.offer || m.offer->version() != c.version || ! notInstalled)
                    return same (m);
                if (checking != nullptr)
                    return queue (m, c);
                if (! idle (m))
                    return same (m);
                auto next = m;
                next.activity = activity::Installing { *m.offer, Stage::download, 0 };
                next.lastFailure.reset();
                return { std::move (next), { effect::Install { *m.offer } } };
            },
            // T10
            [&] (const command::Cancel&) -> Step
            {
                const auto* job = std::get_if<activity::Installing> (&m.activity);
                if (job == nullptr || job->stage >= Stage::commit)
                    return same (m);
                return { m, { effect::CancelInstall {} } };
            },
            // T14
            [&] (const command::SetAutoCheck& c) -> Step
            {
                return savePrefs (m, PrefsPatch { c.enabled, true, {} });
            },
            // T15
            [&] (const command::MarkAnnounced& c) -> Step
            {
                return savePrefs (m, PrefsPatch { {}, {}, c.version });
            },
            // T16
            [&] (const command::MarkDisclosed&) -> Step
            {
                return savePrefs (m, PrefsPatch { {}, true, {} });
            },
            // T18
            [&] (const command::OpenReleasePage&) -> Step
            {
                return { m, { effect::OpenInBrowser { releasePageUrl (m.offer ? m.offer->version() : m.running) } } };
            },
        }, command);
    }
}

Step step (const Model& m, const Event& e)
{
    if (std::holds_alternative<site::Off> (m.site))
        return same (m);

    return std::visit (Overload {
        // T1
        [&] (const event::WatchOpened&) -> Step
        {
            if (! idle (m))
                return { m, { effect::Reconcile {} } };
            auto next = m;
            next.activity = activity::Checking { false };
            return { std::move (next), { effect::Reconcile {}, effect::Check { false } } };
        },
        // T2
        [&] (const event::Reconciled& r) -> Step
        {
            auto next = m;
            next.disk = r.disk;
            next.prefs = r.prefs;
            if (r.cached && (! next.offer || r.cached->version() > next.offer->version()))
                next.offer = r.cached;
            if (next.lastFailure && r.disk.installed && next.lastFailure->version <= *r.disk.installed)
                next.lastFailure.reset();
            return { std::move (next), {} };
        },
        // T4, T5, T6
        [&] (const event::CheckFinished& f) -> Step
        {
            const auto* checking = std::get_if<activity::Checking> (&m.activity);
            if (checking == nullptr)
                return same (m);
            auto next = m;
            next.activity = activity::None {};
            next.prefs = f.prefs;
            std::visit (Overload {
                [] (const check::NotDue&) {},
                [&] (const check::Verified& v)
                {
                    next.offer = v.offer;
                    next.lastCheckProblem.reset();
                    if (next.lastFailure && (! v.offer || v.offer->version() != next.lastFailure->version))
                        next.lastFailure.reset();
                },
                [&] (CheckProblem p)
                {
                    if (checking->manual)
                        next.lastCheckProblem = p;
                },
            }, f.outcome);
            // T19
            if (checking->then)
                return std::visit ([&] (const auto& click) { return onCommand (next, Command { click }); }, *checking->then);
            return { std::move (next), {} };
        },
        // T9
        [&] (const event::StageEntered& s) -> Step
        {
            const auto* job = std::get_if<activity::Installing> (&m.activity);
            if (job == nullptr || s.stage <= job->stage)
                return same (m);
            auto next = m;
            std::get<activity::Installing> (next.activity).stage = s.stage;
            return { std::move (next), {} };
        },
        // T8
        [&] (const event::Progress& p) -> Step
        {
            const auto* job = std::get_if<activity::Installing> (&m.activity);
            if (job == nullptr || job->stage != Stage::download)
                return same (m);
            auto next = m;
            std::get<activity::Installing> (next.activity).received = p.received;
            return { std::move (next), {} };
        },
        // T11, T12, T13
        [&] (const event::InstallFinished& f) -> Step
        {
            const auto* job = std::get_if<activity::Installing> (&m.activity);
            if (job == nullptr)
                return same (m);
            auto next = m;
            next.disk = f.disk;
            if (const auto* failure = std::get_if<Failure> (&f.result))
                next.lastFailure = phase::Failed { job->offer.version(), *failure };
            next.activity = activity::None {};
            return { std::move (next), {} };
        },
        // T17
        [&] (const event::PrefsSaved& s) -> Step
        {
            auto next = m;
            next.prefs = s.prefs;
            return { std::move (next), {} };
        },
        [&] (const event::Commanded& c) -> Step { return onCommand (m, c.command); },
    }, e);
}

View project (const Model& m)
{
    View view;
    view.running = m.running;
    view.autoCheck = m.prefs ? m.prefs->autoCheck : true;
    view.systemCopy = m.disk.systemCopy;

    if (std::holds_alternative<site::Off> (m.site))
    {
        view.phase = phase::Off {};
        return view;
    }

    view.disclose = m.prefs && ! m.prefs->disclosed;

    const auto* job = std::get_if<activity::Installing> (&m.activity);
    const auto* checking = std::get_if<activity::Checking> (&m.activity);
    const auto* manual = std::get_if<site::Manual> (&m.site);

    if (job != nullptr && job->stage == Stage::download)
        view.phase = phase::Downloading { job->offer.version(), job->received, job->offer.archive().size };
    else if (job != nullptr)
        view.phase = phase::Installing { job->offer.version() };
    else if (checking != nullptr && (checking->manual || checking->then))
        view.phase = phase::Checking {};
    else if (m.disk.installed && *m.disk.installed != m.running)
        view.phase = phase::RestartPending { *m.disk.installed };
    else if (m.lastFailure)
        view.phase = *m.lastFailure;
    else if (m.offer && manual != nullptr)
        view.phase = phase::Manual { m.offer->version(), manual->why };
    else if (m.offer)
        view.phase = phase::Available { m.offer->version() };
    else
        view.phase = phase::Idle { m.lastCheckProblem };

    const bool offering = std::holds_alternative<phase::Available> (view.phase)
                       || std::holds_alternative<phase::Manual> (view.phase);
    if (offering && m.prefs && (! m.prefs->announced || *m.prefs->announced < m.offer->version()))
        view.announce = m.offer->version();

    return view;
}
} // namespace tubamp::update::detail
