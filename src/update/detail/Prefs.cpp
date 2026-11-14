#include "Files.h"

#include <json.hpp>

namespace tubamp::update::detail
{
namespace
{
    constexpr std::size_t maxPrefsBytes = 64 * 1024;

    fs::path prefsFile (const SupportPaths& support) { return support.dir / "prefs.json"; }

    Prefs parse (const std::optional<Bytes>& bytes)
    {
        Prefs prefs;
        if (! bytes)
            return prefs;
        const auto json = nlohmann::json::parse (bytes->begin(), bytes->end(), nullptr, false);
        if (! json.is_object())
            return prefs;

        if (const auto it = json.find ("autoCheck"); it != json.end() && it->is_boolean())
            prefs.autoCheck = it->get<bool>();
        if (const auto it = json.find ("disclosed"); it != json.end() && it->is_boolean())
            prefs.disclosed = it->get<bool>();
        if (const auto it = json.find ("lastCheckAt"); it != json.end() && it->is_number_integer())
            prefs.lastCheckAt = std::chrono::sys_seconds (std::chrono::seconds (it->get<std::int64_t>()));
        if (const auto it = json.find ("announced"); it != json.end() && it->is_string())
            prefs.announced = Version::parse (it->get<std::string>());
        return prefs;
    }

    bool save (const SupportPaths& support, const Prefs& prefs)
    {
        nlohmann::json json { { "autoCheck", prefs.autoCheck }, { "disclosed", prefs.disclosed } };
        if (prefs.lastCheckAt)
            json["lastCheckAt"] = std::int64_t (prefs.lastCheckAt->time_since_epoch().count());
        if (prefs.announced)
            json["announced"] = prefs.announced->toString();
        const auto text = json.dump (2) + "\n";
        return replaceFile (prefsFile (support), ByteView (reinterpret_cast<const std::uint8_t*> (text.data()), text.size()));
    }
}

Prefs loadPrefs (const SupportPaths& support)
{
    return parse (readFile (prefsFile (support), maxPrefsBytes));
}

Prefs mutatePrefs (const SupportPaths& support, const PrefsPatch& patch)
{
    const auto lock = SupportLock::acquire (support);
    auto prefs = loadPrefs (support);
    apply (prefs, patch);
    if (lock)
        save (support, prefs);
    return prefs;
}

bool claimDailyCheck (const SupportPaths& support, std::chrono::sys_seconds now)
{
    const auto lock = SupportLock::acquire (support);
    if (! lock)
        return false;
    auto prefs = loadPrefs (support);
    if (! isCheckDue (prefs, now))
        return false;
    prefs.lastCheckAt = now;
    return save (support, prefs);
}
} // namespace tubamp::update::detail
