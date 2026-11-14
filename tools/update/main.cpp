// tubamp_update_tool: the update core's test runner and the release gate.
//
//   tubamp_update_tool test
//       Runs every table. Exit 1 on any failure. TUBAMP_UPDATE_COMPONENT=<built
//       tubamp.component> runs the disk tables on copies of that bundle.
//
//   tubamp_update_tool child <command> ...
//       A process the disk tests spawn, race and kill. Not for people.
//
//   tubamp_update_tool verify-envelope --envelope <file> --running <X.Y.Z>
//                      [--highest-seen <X.Y.Z>] [--macos <X.Y>] [--trust <pub.pem>]...
//       Opens the envelope exactly as the plugin does and prints the offer or the refusal.
//       Without --trust it uses the release keys compiled into this build. Exit 0 only
//       when there is an offer.
//
//   tubamp_update_tool fetch-check [--path <path under the repository>] [--cancel-after <ms>]
//       One real GET through the system fetcher, of https://github.com/IamP5/tubamp/<path>
//       (default releases/latest). Prints every hop with the policy's verdict and the
//       outcome; --cancel-after cancels it and prints how long get() took to return after
//       the cancel. Uses the network; never part of `test`.

#include "UpdateTests.h"
#include "detail/Fetch.h"
#include "detail/Trust.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

using namespace tubamp::update;
using namespace tubamp::update::detail;

namespace
{
const char* name (EnvelopeError e)
{
    switch (e)
    {
        case EnvelopeError::tooLarge: return "envelope too large";
        case EnvelopeError::badMagic: return "not an update-v1 envelope";
        case EnvelopeError::noSignature: return "no signature line";
        case EnvelopeError::tooManySignatures: return "too many signature lines";
        case EnvelopeError::malformedLine: return "malformed header line";
        case EnvelopeError::noTerminator: return "no '.' line";
    }
    return "?";
}

const char* name (SignatureError e)
{
    return e == SignatureError::unknownKey ? "no signature by a trusted key" : "signature does not verify";
}

const char* name (ManifestError e)
{
    switch (e)
    {
        case ManifestError::notJson: return "payload is not a JSON object";
        case ManifestError::missingField: return "payload field missing";
        case ManifestError::badVersion: return "bad version";
        case ManifestError::badMacOS: return "bad minimumMacOS";
        case ManifestError::badHash: return "bad archive sha256";
        case ManifestError::badSize: return "bad archive size";
    }
    return "?";
}

const char* name (NoOffer n)
{
    switch (n)
    {
        case NoOffer::wrongProduct: return "wrong product";
        case NoOffer::oversized: return "archive above the size ceiling";
        case NoOffer::replayed: return "below the highest version seen";
        case NoOffer::upToDate: return "not newer than the running version";
        case NoOffer::needsNewerMacOS: return "needs a newer macOS";
    }
    return "?";
}

std::optional<std::string> readFile (const std::string& path)
{
    std::ifstream in (path, std::ios::binary);
    if (! in)
        return std::nullopt;
    return std::string (std::istreambuf_iterator<char> (in), {});
}

int usage()
{
    std::fprintf (stderr,
                  "usage: tubamp_update_tool test\n"
                  "       tubamp_update_tool verify-envelope --envelope <file> --running <X.Y.Z>\n"
                  "                          [--highest-seen <X.Y.Z>] [--macos <X.Y>] [--trust <pub.pem>]...\n"
                  "       tubamp_update_tool fetch-check [--path <path under the repository>]\n");
    return 2;
}

int verifyEnvelope (int argc, char** argv)
{
    std::string envelopePath;
    std::optional<Version> running, highest;
    OsVersion os { 999, 0 };   // unconstrained unless asked: the gate checks everything else
    std::vector<TrustedKey> trusted;
    bool customTrust = false;

    for (int i = 2; i + 1 < argc; i += 2)
    {
        const std::string flag = argv[i], value = argv[i + 1];
        if (flag == "--envelope")
            envelopePath = value;
        else if (flag == "--running")
            running = Version::parse (value);
        else if (flag == "--highest-seen")
        {
            if (! (highest = Version::parse (value)))
                return usage();
        }
        else if (flag == "--macos")
        {
            const auto parsed = OsVersion::parse (value);
            if (! parsed)
                return usage();
            os = *parsed;
        }
        else if (flag == "--trust")
        {
            const auto pem = readFile (value);
            const auto key = pem ? parsePublicKeyPem (*pem) : std::nullopt;
            if (! key)
            {
                std::fprintf (stderr, "refused: %s is not a P-256 public key PEM\n", value.c_str());
                return 2;
            }
            trusted.push_back (*key);
            customTrust = true;
        }
        else
            return usage();
    }
    if (envelopePath.empty() || ! running || (argc % 2) != 0)
        return usage();

    const auto file = readFile (envelopePath);
    if (! file)
    {
        std::fprintf (stderr, "refused: cannot read %s\n", envelopePath.c_str());
        return 1;
    }

    const TrustRoot custom (trusted);
    const auto& root = customTrust ? custom : TrustRoot::release();
    if (root.keys().empty())
        std::fprintf (stderr, "note: no trusted keys (this build has no release keys and no --trust was given)\n");

    const auto bytes = ByteView (reinterpret_cast<const std::uint8_t*> (file->data()), file->size());
    const auto opened = openEnvelope (bytes, root);
    if (! opened)
    {
        std::visit ([] (auto e) { std::printf ("refused: %s\n", name (e)); }, opened.error());
        return 1;
    }

    const auto& m = opened.value();
    const auto evaluation = evaluate (m, OfferContext { *running, highest, os });
    if (const auto* none = std::get_if<NoOffer> (&evaluation))
    {
        std::printf ("no offer: %s (signed version %s)\n", name (*none), m.version().toString().c_str());
        return 1;
    }

    const auto& offer = std::get<Offer> (evaluation);
    std::string hash;
    for (auto b : offer.archive().sha256)
    {
        char two[3];
        std::snprintf (two, sizeof two, "%02x", b);
        hash += two;
    }
    std::printf ("offer %s\n  minimumMacOS %d.%d\n  size %llu\n  sha256 %s\n  archive %s\n",
                 offer.version().toString().c_str(), m.minimumMacOS().major, m.minimumMacOS().minor,
                 (unsigned long long) offer.archive().size, hash.c_str(), archiveUrl (offer).text().c_str());
    return 0;
}
const char* name (FetchError e)
{
    switch (e)
    {
        case FetchError::cancelled: return "cancelled";
        case FetchError::unreachable: return "unreachable (DNS, connect, TLS or timeout)";
        case FetchError::httpStatus: return "final status is not 200";
        case FetchError::redirectRefused: return "redirect refused by the host policy";
        case FetchError::tooLarge: return "more bytes than the cap";
        case FetchError::sinkRefused: return "the sink refused the bytes";
    }
    return "?";
}

int fetchCheck (int argc, char** argv)
{
    std::string path = "releases/latest";
    std::optional<int> cancelAfter;
    for (int i = 2; i < argc; i += 2)
    {
        const std::string flag = argv[i];
        if (i + 1 >= argc)
            return usage();
        if (flag == "--path")
            path = argv[i + 1];
        else if (flag == "--cancel-after")
            cancelAfter = std::atoi (argv[i + 1]);
        else
            return usage();
    }

    const auto url = repositoryUrl (path);
    if (! url)
    {
        std::fprintf (stderr, "refused: '%s' is not a plain path under the repository\n", path.c_str());
        return 2;
    }

    int hop = 0;
    auto fetcher = makeSystemFetcher (Version { 0, 0, 0 }, [&hop] (std::string_view u, bool admitted)
    {
        std::printf ("hop %d  %s  %.*s\n", ++hop, admitted ? "admitted" : "REFUSED ", (int) u.size(), u.data());
    });
    FetchLimits limits;
    limits.maxBytes = 4u << 20;
    std::uint64_t bytes = 0;
    CancelToken cancel;
    std::atomic<std::int64_t> cancelledAt { 0 };
    std::thread canceller;
    if (cancelAfter)
        canceller = std::thread ([&]
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (*cancelAfter));
            cancelledAt = std::chrono::steady_clock::now().time_since_epoch().count();
            cancel.request();
        });
    const auto start = std::chrono::steady_clock::now();
    const auto result = fetcher->get (*url, limits, [&] (ByteView chunk) { bytes += chunk.size(); return true; }, cancel);
    const auto end = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (end - start).count();
    if (canceller.joinable())
        canceller.join();
    if (cancelAfter && ! result && result.error() == FetchError::cancelled)
        std::printf ("cancel   get() returned %.1f ms after the cancel\n",
                     double (end.time_since_epoch().count() - cancelledAt.load()) / 1e6);
    if (result)
        std::printf ("outcome  200, %llu bytes in %lld ms\n", (unsigned long long) bytes, (long long) ms);
    else
        std::printf ("outcome  %s, %llu bytes in %lld ms\n", name (result.error()), (unsigned long long) bytes, (long long) ms);
    return result ? 0 : 1;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc >= 2 && std::string (argv[1]) == "test")
        return tubamp::update::test::runUpdateTests() == 0 ? 0 : 1;
    if (argc >= 2 && std::string (argv[1]) == "verify-envelope")
        return verifyEnvelope (argc, argv);
    if (argc >= 2 && std::string (argv[1]) == "fetch-check")
        return fetchCheck (argc, argv);
    if (argc >= 3 && std::string (argv[1]) == "child")
        return tubamp::update::test::runDiskChild (argc, argv);
    return usage();
}
