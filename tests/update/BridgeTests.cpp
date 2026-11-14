// The editor bridge against ui/src/bridge/update-contract.json, the file tsc also checks
// against ui/src/bridge/types.ts. A shape that drifts on either side fails one of the two.
// Also the redirect policy (HopPolicy) as a function of hop sequences.

#include "UpdateTests.h"

#include "Bridge.h"
#include "detail/Fetch.h"
#include "tubamp_update_fixtures.inc"

#include <json.hpp>

#include <cstdio>
#include <functional>
#include <set>

namespace tubamp::update::test
{
namespace
{
using namespace tubamp::update::detail;
using Json = nlohmann::json;

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

Json golden()
{
    for (const auto& f : fixtureFiles)
        if (f.name == "update-contract.json")
            return Json::parse (f.text);
    throw std::runtime_error ("update-contract.json is not in the fixture table");
}

View base (Phase phase)
{
    View v;
    v.running = R;
    v.autoCheck = true;
    v.disclose = false;
    v.systemCopy = false;
    v.phase = std::move (phase);
    return v;
}

Json emitted (const View& v) { return Json::parse (viewJson (v, "Logic Pro")); }

std::set<std::string> keysOf (const Json& object)
{
    std::set<std::string> keys;
    for (const auto& item : object.items())
        keys.insert (item.key());
    return keys;
}

void views (const Json& g)
{
    group ("bridge: every phase against the golden view");

    auto off = base (phase::Off {});
    off.autoCheck = false;
    auto available = base (phase::Available { N });
    available.announce = N;
    available.disclose = true;
    auto restart = base (phase::RestartPending { N });
    restart.systemCopy = true;
    auto manual = base (phase::Manual { N, ManualReason::systemDomain });
    manual.announce = N;

    const std::vector<std::pair<std::string, View>> cases {
        { "off", off },
        { "idle", base (phase::Idle {}) },
        { "checking", base (phase::Checking {}) },
        { "available", available },
        { "downloading", base (phase::Downloading { N, 5000000, 17345678 }) },
        { "installing", base (phase::Installing { N }) },
        { "restartPending", restart },
        { "failed", base (phase::Failed { N, Failure::network }) },
        { "manual", manual },
    };

    std::set<std::string> produced;
    for (const auto& [kind, view] : cases)
    {
        produced.insert (kind);
        check (g["views"].contains (kind) && emitted (view) == g["views"][kind], "viewJson matches the golden view '" + kind + "'");
        check (g["views"].contains (kind) && g["views"][kind]["phase"]["kind"] == kind, "the golden view '" + kind + "' holds that kind");
    }
    check (keysOf (g["views"]) == produced, "C++ produces every phase kind the golden file names, and no other");
    check (std::variant_size_v<Phase> == produced.size(), "every alternative of Phase has a golden view");
}

template <class Value>
void reasons (const Json& section, const char* sectionName, const char* field,
              const std::vector<std::pair<Value, std::string>>& mapping, const std::function<Phase (Value)>& phaseOf)
{
    std::set<std::string> produced;
    for (const auto& [value, name] : mapping)
    {
        produced.insert (name);
        const auto out = emitted (base (phaseOf (value)))["phase"];
        check (section.contains (name) && out == section[name],
               std::string (sectionName) + ": viewJson matches the golden '" + name + "'");
        check (section.contains (name) && section[name][field] == name,
               std::string (sectionName) + ": the golden '" + name + "' holds that " + field);
    }
    check (keysOf (section) == produced, std::string (sectionName) + ": C++ produces exactly the golden set");
}

void failuresAndReasons (const Json& g)
{
    group ("bridge: failures, manual reasons and check problems");

    reasons<Failure> (g["failures"], "failures", "reason",
                      { { Failure::network, "network" }, { Failure::integrity, "verify" }, { Failure::archive, "verify" },
                        { Failure::wrongBundle, "verify" }, { Failure::noSpace, "noSpace" }, { Failure::notWritable, "notWritable" } },
                      [] (Failure f) -> Phase { return phase::Failed { N, f }; });

    reasons<ManualReason> (g["manualReasons"], "manual reasons", "reason",
                           { { ManualReason::systemDomain, "systemDomain" }, { ManualReason::unexpectedPath, "unexpectedPath" },
                             { ManualReason::readOnly, "readOnly" }, { ManualReason::noAtomicSwap, "noAtomicSwap" },
                             { ManualReason::notAnAudioUnit, "notAnAudioUnit" } },
                           [] (ManualReason r) -> Phase { return phase::Manual { N, r }; });

    reasons<CheckProblem> (g["checkProblems"], "check problems", "problem",
                           { { CheckProblem::unreachable, "unreachable" }, { CheckProblem::unverified, "unverified" } },
                           [] (CheckProblem p) -> Phase { return phase::Idle { p }; });
}

void commands (const Json& g)
{
    group ("bridge: every command parses from the golden JSON, and malformed ones are refused");

    const std::vector<std::pair<std::string, Command>> cases {
        { "checkNow", command::CheckNow {} },
        { "install", command::Install { N } },
        { "cancel", command::Cancel {} },
        { "setAutoCheck", command::SetAutoCheck { false } },
        { "markAnnounced", command::MarkAnnounced { N } },
        { "markDisclosed", command::MarkDisclosed {} },
        { "openReleasePage", command::OpenReleasePage {} },
    };

    std::set<std::string> produced;
    for (const auto& [type, expected] : cases)
    {
        produced.insert (type);
        const auto parsed = g["commands"].contains (type) ? parseCommand (g["commands"][type].dump()) : std::nullopt;
        check (parsed && *parsed == expected, "parseCommand reads the golden '" + type + "'");
        check (g["commands"].contains (type) && g["commands"][type]["type"] == type, "the golden '" + type + "' holds that type");
    }
    check (keysOf (g["commands"]) == produced, "C++ parses exactly the golden command set");
    check (std::variant_size_v<Command> == produced.size(), "every alternative of Command has a golden entry");

    for (const auto& refused : g["refusedCommands"])
        check (! parseCommand (refused.dump()), "refused: " + refused.dump());
    check (! parseCommand ("") && ! parseCommand ("undefined") && ! parseCommand ("{\"type\":\"checkNow\""),
           "text that is not JSON is refused");
}

void hops()
{
    group ("fetch: the redirect policy over hop sequences");

    const auto run = [] (std::vector<std::string_view> sequence, int maxRedirects = 4)
    {
        HopPolicy policy (maxRedirects);
        std::vector<bool> verdicts;
        for (auto hop : sequence)
            verdicts.push_back (policy.admit (hop));
        return verdicts;
    };
    using V = std::vector<bool>;

    const auto envelope = envelopeUrl().text();
    check (run ({ envelope }) == V { true }, "the envelope URL itself is admitted");
    check (run ({ envelope, "https://objects.githubusercontent.com/github-production-release-asset/1?x=1",
                  "https://release-assets.githubusercontent.com/a/b" }) == V { true, true, true },
           "GitHub's asset hosts are admitted as redirect targets");
    check (run ({ envelope, "http://objects.githubusercontent.com/a" }) == V { true, false }, "a plain-http hop is refused");
    check (run ({ envelope, "https://evil.example/update-v1.signed" }) == V { true, false }, "a foreign host is refused");
    check (run ({ envelope, "https://github.com.evil.example/x" }) == V { true, false }, "a host that only starts like GitHub is refused");
    check (run ({ envelope, "https://evil.example.github.com/x" }) == V { true, false }, "a subdomain of an allowed host is refused");
    check (run ({ envelope, "https://user@github.com/x" }) == V { true, false }, "user info in a hop is refused");
    check (run ({ envelope, "https://github.com:8443/x" }) == V { true, false }, "a port in a hop is refused");
    check (run ({ envelope, "https://GITHUB.COM/x" }) == V { true, false }, "the match is exact, case included");
    check (run ({ "ftp://github.com/x" }) == V { false }, "a first URL that is not https is refused");
    check (run ({ envelope, "https://evil.example/", "https://github.com/x" }) == V { true, false, false },
           "after one refusal every later hop is refused");
    check (run ({ envelope, "https://github.com/1", "https://github.com/2" }, 2) == V { true, true, true },
           "exactly maxRedirects redirects are followed");
    check (run ({ envelope, "https://github.com/1", "https://github.com/2", "https://github.com/3" }, 2)
               == V { true, true, true, false },
           "one redirect past maxRedirects is refused");
    check (run ({ envelope, "https://github.com/1" }, 0) == V { true, false }, "maxRedirects 0 follows none");

    HopPolicy recorded (4);
    recorded.admit (envelope);
    recorded.admit ("https://evil.example/");
    check (recorded.hops() == std::vector<std::string> { std::string (envelope), "https://evil.example/" },
           "the policy records every hop it was asked about, the refused one included");

    group ("fetch: repository URLs for fetch-check");
    check (repositoryUrl ("releases/latest") && repositoryUrl ("releases/latest")->text() == "https://github.com/IamP5/tubamp/releases/latest",
           "a plain path stays under the compiled-in repository");
    for (auto bad : { "", "/releases", "releases/", "a//b", "../other", "a/../../x", "a/./b", "x?y=1", "x#y", "@evil.example",
                      "a b", "%2e%2e/x", "a:b" })
        check (! repositoryUrl (bad), std::string ("refused repository path '") + bad + "'");
}
} // namespace

void runBridgeTests (int& passed, int& failed)
{
    passCount = &passed;
    failCount = &failed;
    const auto g = golden();
    views (g);
    failuresAndReasons (g);
    commands (g);
    hops();
}
} // namespace tubamp::update::test
