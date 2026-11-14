#include "Bridge.h"

#include <json.hpp>

namespace tubamp::update
{
namespace
{
    using Json = nlohmann::json;

    template <class... F> struct Overload : F... { using F::operator()...; };
    template <class... F> Overload (F...) -> Overload<F...>;

    // The page shows one sentence for every way the download failed to match the release.
    const char* nameOf (Failure f)
    {
        switch (f)
        {
            case Failure::network:     return "network";
            case Failure::integrity:
            case Failure::archive:
            case Failure::wrongBundle: return "verify";
            case Failure::noSpace:     return "noSpace";
            case Failure::notWritable: return "notWritable";
        }
        return "network";
    }

    const char* nameOf (ManualReason r)
    {
        switch (r)
        {
            case ManualReason::systemDomain:   return "systemDomain";
            case ManualReason::unexpectedPath: return "unexpectedPath";
            case ManualReason::readOnly:       return "readOnly";
            case ManualReason::noAtomicSwap:   return "noAtomicSwap";
            case ManualReason::notAnAudioUnit: return "notAnAudioUnit";
        }
        return "unexpectedPath";
    }

    const char* nameOf (CheckProblem p)
    {
        return p == CheckProblem::unreachable ? "unreachable" : "unverified";
    }

    Json phaseJson (const Phase& phase)
    {
        return std::visit (Overload {
            [] (const phase::Off&) { return Json { { "kind", "off" } }; },
            [] (const phase::Idle& p)
            {
                Json j { { "kind", "idle" } };
                if (p.problem)
                    j["problem"] = nameOf (*p.problem);
                return j;
            },
            [] (const phase::Checking&) { return Json { { "kind", "checking" } }; },
            [] (const phase::Available& p) { return Json { { "kind", "available" }, { "version", p.version.toString() } }; },
            [] (const phase::Downloading& p)
            {
                return Json { { "kind", "downloading" }, { "version", p.version.toString() },
                              { "received", p.received }, { "total", p.total } };
            },
            [] (const phase::Installing& p) { return Json { { "kind", "installing" }, { "version", p.version.toString() } }; },
            [] (const phase::RestartPending& p) { return Json { { "kind", "restartPending" }, { "version", p.installed.toString() } }; },
            [] (const phase::Failed& p)
            {
                return Json { { "kind", "failed" }, { "version", p.version.toString() }, { "reason", nameOf (p.why) } };
            },
            [] (const phase::Manual& p)
            {
                return Json { { "kind", "manual" }, { "version", p.version.toString() }, { "reason", nameOf (p.why) } };
            },
        }, phase);
    }

    /** The object has exactly these keys. */
    bool keysAre (const Json& j, std::initializer_list<std::string_view> keys)
    {
        if (j.size() != keys.size())
            return false;
        for (auto key : keys)
            if (! j.contains (std::string (key)))
                return false;
        return true;
    }

    std::optional<Version> versionAt (const Json& j)
    {
        const auto it = j.find ("version");
        if (it == j.end() || ! it->is_string())
            return std::nullopt;
        return Version::parse (it->get_ref<const std::string&>());
    }
}

std::string viewJson (const View& view, std::string_view hostName)
{
    Json j { { "running", view.running.toString() },
             { "hostName", std::string (hostName) },
             { "autoCheck", view.autoCheck },
             { "disclose", view.disclose },
             { "systemCopy", view.systemCopy },
             { "phase", phaseJson (view.phase) } };
    if (view.announce)
        j["announce"] = view.announce->toString();
    return j.dump();
}

std::optional<Command> parseCommand (std::string_view text)
{
    const auto j = Json::parse (text.begin(), text.end(), nullptr, false);
    if (! j.is_object() || ! j.contains ("type") || ! j["type"].is_string())
        return std::nullopt;

    const auto& type = j["type"].get_ref<const std::string&>();
    const auto versioned = [&]() -> std::optional<Version>
    {
        return keysAre (j, { "type", "version" }) ? versionAt (j) : std::nullopt;
    };

    if (type == "checkNow" && keysAre (j, { "type" }))
        return command::CheckNow {};
    if (type == "install")
        if (const auto v = versioned())
            return command::Install { *v };
    if (type == "cancel" && keysAre (j, { "type" }))
        return command::Cancel {};
    if (type == "setAutoCheck" && keysAre (j, { "type", "enabled" }) && j["enabled"].is_boolean())
        return command::SetAutoCheck { j["enabled"].get<bool>() };
    if (type == "markAnnounced")
        if (const auto v = versioned())
            return command::MarkAnnounced { *v };
    if (type == "markDisclosed" && keysAre (j, { "type" }))
        return command::MarkDisclosed {};
    if (type == "openReleasePage" && keysAre (j, { "type" }))
        return command::OpenReleasePage {};
    return std::nullopt;
}
} // namespace tubamp::update
