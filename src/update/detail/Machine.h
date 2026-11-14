#pragma once

// PRIVATE to src/update. The update state machine as data and two pure functions:
//
//   step    (Model, Event) -> { Model, effects }     every transition, no I/O
//   project (Model)        -> View                   the user-visible phase, derived
//
// The shell owns one Model per process on the message thread, runs the effects on one
// worker thread and turns their results into Events. The disk is the durable truth;
// the Model dies with the host and is rebuilt from the disk by the next Reconcile.
// Transition names (T1..) are the rows of docs/UPDATES.md "The state machine".

#include "Bundle.h"

#include <vector>

namespace tubamp::update::detail
{
/** Stages of the install job, in order. Commit is the point of no return. */
enum class Stage { download, unpack, inspect, commit, settle };

struct Installed { bool operator== (const Installed&) const = default; };   // or AlreadyInstalled
struct Cancelled { bool operator== (const Cancelled&) const = default; };
using InstallResult = std::variant<Installed, Cancelled, Failure>;

namespace check
{
    /** autoCheck is off, or some process on this account checked in the last 24 hours.
        Nothing was fetched. */
    struct NotDue { bool operator== (const NotDue&) const = default; };

    /** The envelope opened. `offer` is empty when nothing installable is newer. */
    struct Verified
    {
        std::optional<Offer> offer;
        bool operator== (const Verified&) const = default;
    };
}
using CheckOutcome = std::variant<check::NotDue, check::Verified, CheckProblem>;

namespace activity
{
    struct None { bool operator== (const None&) const = default; };
    struct Checking
    {
        bool manual = false;
        bool operator== (const Checking&) const = default;
    };
    struct Installing
    {
        Offer offer;
        Stage stage = Stage::download;
        std::uint64_t received = 0;
        bool operator== (const Installing&) const = default;
    };
}
using Activity = std::variant<activity::None, activity::Checking, activity::Installing>;

struct Model
{
    Version running;                    // constant for the life of the process
    Site site;                          // constant for the life of the process
    DiskState disk;                     // as last reported by the disk
    std::optional<Prefs> prefs;         // empty until the disk has been read once
    std::optional<Offer> offer;         // from the last evaluate (network or cache)
    Activity activity;
    std::optional<phase::Failed> lastFailure;        // cleared by the next attempt
    std::optional<CheckProblem> lastCheckProblem;    // manual checks only

    static Model start (Version running, Site site) { return Model { running, site, {}, {}, {}, {}, {}, {} }; }
    bool operator== (const Model&) const = default;
};

namespace event
{
    struct WatchOpened { bool operator== (const WatchOpened&) const = default; };   // an editor subscribed
    struct Commanded
    {
        Command command;
        bool operator== (const Commanded&) const = default;
    };
    struct Reconciled
    {
        DiskState disk;
        Prefs prefs;
        std::optional<Offer> cached;   // evaluate over the cached envelope
        bool operator== (const Reconciled&) const = default;
    };
    struct CheckFinished
    {
        CheckOutcome outcome;
        Prefs prefs;
        bool operator== (const CheckFinished&) const = default;
    };
    struct StageEntered
    {
        Stage stage;
        bool operator== (const StageEntered&) const = default;
    };
    struct Progress
    {
        std::uint64_t received = 0;
        bool operator== (const Progress&) const = default;
    };
    struct InstallFinished
    {
        InstallResult result;
        DiskState disk;   // as reconcile() left it, so the Model cannot drift from the disk
        bool operator== (const InstallFinished&) const = default;
    };
    struct PrefsSaved
    {
        Prefs prefs;
        bool operator== (const PrefsSaved&) const = default;
    };
}
using Event = std::variant<event::WatchOpened, event::Commanded, event::Reconciled,
                           event::CheckFinished, event::StageEntered, event::Progress,
                           event::InstallFinished, event::PrefsSaved>;

namespace effect
{
    /** reconcile() (or readDisk), loadPrefs, loadCachedEnvelope + evaluate. No network. */
    struct Reconcile { bool operator== (const Reconcile&) const = default; };

    /** An automatic check first claims the daily slot (claimDailyCheck) and reports
        NotDue if it cannot; a manual one skips the claim. The machine has no clock. */
    struct Check
    {
        bool manual = false;
        bool operator== (const Check&) const = default;
    };

    struct Install
    {
        Offer offer;
        bool operator== (const Install&) const = default;
    };

    struct CancelInstall { bool operator== (const CancelInstall&) const = default; };

    struct SavePrefs
    {
        PrefsPatch patch;
        bool operator== (const SavePrefs&) const = default;
    };

    struct OpenInBrowser
    {
        Url url;
        bool operator== (const OpenInBrowser&) const = default;
    };
}
using Effect = std::variant<effect::Reconcile, effect::Check, effect::Install,
                            effect::CancelInstall, effect::SavePrefs, effect::OpenInBrowser>;

struct Step
{
    Model next;
    std::vector<Effect> effects;
};

/** Every transition. Pure and total: an Event that means nothing in a Model yields the
    same Model and no effects. */
Step step (const Model&, const Event&);

/** The View for a Model. Phase precedence, first match wins:

      site is Off                              -> Off
      Installing at Stage::download            -> Downloading
      Installing at a later stage              -> Installing
      Checking, manual                         -> Checking   (automatic checks are invisible)
      disk.installed differs from running      -> RestartPending
      lastFailure                              -> Failed
      offer, site is Manual                    -> Manual
      offer                                    -> Available
      otherwise                                -> Idle { lastCheckProblem }

    announce = the offer's version when the phase is Available or Manual and it is above
               prefs.announced. disclose = prefs read and not yet disclosed, site not Off. */
View project (const Model&);
} // namespace tubamp::update::detail
