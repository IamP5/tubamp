// tubamp_update_keycheck: the release-key guard that src/update/update.cmake compiles and
// runs at configure time, with the parser the plugin uses (parsePublicKeyPem), so CMake
// and the plugin cannot disagree about what a key file holds.
//
//   tubamp_update_keycheck <minimum count> <test key>... -- <release key>...
//
// Prints one line per problem and exits 1 if there is any.

#include "detail/Trust.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace tubamp::update::detail;

namespace
{
std::optional<std::string> readText (const std::string& path)
{
    std::ifstream in (path, std::ios::binary);
    if (! in)
        return std::nullopt;
    return std::string (std::istreambuf_iterator<char> (in), {});
}

const char* reason (KeyFileError e)
{
    return e == KeyFileError::notOnePublicKeyBlock
               ? "must hold exactly one PEM block, \"-----BEGIN PUBLIC KEY-----\""
               : "is not an uncompressed P-256 (prime256v1) public key";
}
}

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf (stderr, "usage: tubamp_update_keycheck <minimum count> <test key>... -- <release key>...\n");
        return 2;
    }
    const auto minimum = std::stoul (argv[1]);
    std::vector<std::string> testKeys, releaseKeys;
    bool afterSeparator = false;
    for (int i = 2; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--" && ! afterSeparator)
            afterSeparator = true;
        else
            (afterSeparator ? releaseKeys : testKeys).push_back (arg);
    }

    std::vector<std::string> problems;
    std::vector<std::pair<std::string, TrustedKey>> known;
    for (const auto& path : testKeys)
    {
        const auto text = readText (path);
        const auto key = text ? std::optional (parsePublicKeyPem (*text)) : std::nullopt;
        if (! key || ! *key)
            problems.push_back (path + ": this test key does not parse, so release keys cannot be compared with it.");
        else
            known.emplace_back (path, key->value());
    }

    if (releaseKeys.size() < minimum)
        problems.push_back (std::to_string (releaseKeys.size()) + " release key(s) in src/update/keys, at least "
                            + std::to_string (minimum) + " needed: the release key and the offline recovery key "
                            + "(see src/update/keys/README.md).");

    for (const auto& path : releaseKeys)
    {
        const auto text = readText (path);
        if (! text)
        {
            problems.push_back (path + " cannot be read.");
            continue;
        }
        if (text->find ("PRIVATE KEY") != std::string::npos)
            problems.push_back (path + " holds a private key. Delete it from the repository now.");
        if (text->find ("TEST KEY") != std::string::npos)
            problems.push_back (path + " is marked as a test key.");
        const auto key = parsePublicKeyPem (*text);
        if (! key)
        {
            problems.push_back (path + " " + reason (key.error()) + ".");
            continue;
        }
        for (const auto& [testPath, testKey] : known)
            if (testKey == key.value())
                problems.push_back (path + " is the test key " + testPath + ".");
    }

    for (const auto& p : problems)
        std::printf ("  %s\n", p.c_str());
    return problems.empty() ? 0 : 1;
}
