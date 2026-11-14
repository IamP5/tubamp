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
//                      [--archive <zip> --installed <tubamp.component>...] [--expect-keys <n>]
//       Opens the envelope exactly as the plugin does and prints the offer or the refusal.
//       Without --trust it uses the release keys compiled into this build. --expect-keys
//       refuses unless exactly n release keys are compiled in, so a key file the plugin
//       could not parse stops the release. With --archive
//       it then runs the plugin's own download, unpack and inspect stages on the zip, once
//       for each --installed bundle, in a temporary home where that bundle is the live
//       copy. Nothing is swapped. Exit 0 only when there is an offer and every archive
//       check accepts it.
//
//   tubamp_update_tool fetch-check [--path <path under the repository>] [--cancel-after <ms>]
//       One real GET through the system fetcher, of https://github.com/IamP5/tubamp/<path>
//       (default releases/latest). Prints every hop with the policy's verdict and the
//       outcome; --cancel-after cancels it and prints how long get() took to return after
//       the cancel. Uses the network; never part of `test`.

#include "UpdateTests.h"
#include "detail/Bundle.h"
#include "detail/Fetch.h"
#include "detail/Trust.h"

#include <atomic>
#include <filesystem>
#include <unistd.h>
#include <vector>
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

const char* name (Failure f)
{
    switch (f)
    {
        case Failure::network: return "network";
        case Failure::integrity: return "size or SHA-256 differs from the signed payload";
        case Failure::archive: return "an entry breaks the extraction rules";
        case Failure::wrongBundle: return "Info.plist, CPU slices or code seal do not match the offer";
        case Failure::noSpace: return "no space";
        case Failure::notWritable: return "not writable";
    }
    return "?";
}

const char* name (ManualReason r)
{
    switch (r)
    {
        case ManualReason::systemDomain: return "system domain";
        case ManualReason::unexpectedPath: return "unexpected path";
        case ManualReason::readOnly: return "read-only";
        case ManualReason::noAtomicSwap: return "no atomic swap";
        case ManualReason::notAnAudioUnit: return "not an Audio Unit";
    }
    return "?";
}

/** The stages the plugin runs between the download and the swap, on a copy of
    `installed` in a temporary home. The temporary home is deleted before returning. */
bool archiveAccepted (const Offer& offer, const std::string& archivePath, const std::string& installedPath)
{
    namespace fs = std::filesystem;
    const char* tmp = std::getenv ("TMPDIR");
    std::string pattern = (fs::path (tmp != nullptr && *tmp != 0 ? tmp : "/tmp") / "tubamp-archive-gate.XXXXXX").string();
    if (::mkdtemp (pattern.data()) == nullptr)
    {
        std::printf ("refused: cannot create a temporary home\n");
        return false;
    }
    const fs::path root (pattern);
    const auto home = root / "home";
    const auto live = home / "Library" / "Audio" / "Plug-Ins" / "Components" / bundleRoot;

    const auto run = [&]() -> std::optional<Failure>
    {
        std::error_code ec;
        fs::create_directories (live.parent_path(), ec);
        fs::copy (installedPath, live, fs::copy_options::recursive, ec);
        if (ec)
        {
            std::printf ("refused: cannot copy %s: %s\n", installedPath.c_str(), ec.message().c_str());
            return Failure::notWritable;
        }
        const auto site = InstallSite::detect (live / "Contents" / "MacOS" / "tubamp", home, true, false, root / "System");
        const auto* writable = std::get_if<SelfUpdatable> (&site.kind);
        if (writable == nullptr)
        {
            const auto* manual = std::get_if<InstallSite::NotSelfUpdatable> (&site.kind);
            std::printf ("refused: the temporary copy cannot replace itself (%s)\n", manual != nullptr ? name (manual->why) : "off");
            return Failure::notWritable;
        }

        auto staging = Staging::create (*writable);
        if (! staging)
            return staging.error();
        auto sink = ArchiveSink::open (staging.value(), offer);
        if (! sink)
            return sink.error();
        std::ifstream in (archivePath, std::ios::binary);
        if (! in)
        {
            std::printf ("refused: cannot read %s\n", archivePath.c_str());
            return Failure::integrity;
        }
        std::vector<char> chunk (1 << 16);
        while (in.read (chunk.data(), std::streamsize (chunk.size())) || in.gcount() > 0)
            if (! sink.value().write (ByteView (reinterpret_cast<const std::uint8_t*> (chunk.data()), std::size_t (in.gcount()))))
                break;
        auto archive = std::move (sink.value()).finish();
        if (! archive)
            return archive.error();
        auto unpacked = unpack (std::move (archive).value(), staging.value());
        if (! unpacked)
            return unpacked.error();
        auto inspected = inspect (std::move (unpacked).value(), staging.value(), offer, *writable);
        if (! inspected)
            return inspected.error();
        return std::nullopt;
    };

    const auto failure = run();
    std::error_code ignored;
    fs::remove_all (root, ignored);
    if (failure)
        std::printf ("archive refused for installed %s: %s\n", installedPath.c_str(), name (*failure));
    else
        std::printf ("archive accepted for installed %s\n", installedPath.c_str());
    return ! failure;
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
                  "                          [--archive <zip> --installed <tubamp.component>...] [--expect-keys <n>]\n"
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
    std::string archivePath;
    std::vector<std::string> installed;
    std::optional<std::size_t> expectedKeys;

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
        else if (flag == "--archive")
            archivePath = value;
        else if (flag == "--installed")
            installed.push_back (value);
        else if (flag == "--expect-keys")
        {
            if (value.empty() || value.find_first_not_of ("0123456789") != std::string::npos)
                return usage();
            expectedKeys = std::stoul (value);
        }
        else if (flag == "--trust")
        {
            const auto pem = readFile (value);
            const auto key = pem ? std::optional (parsePublicKeyPem (*pem)) : std::nullopt;
            if (! key || ! *key)
            {
                std::fprintf (stderr, "refused: %s is not a P-256 public key PEM\n", value.c_str());
                return 2;
            }
            trusted.push_back (key->value());
            customTrust = true;
        }
        else
            return usage();
    }
    if (envelopePath.empty() || ! running || (argc % 2) != 0 || archivePath.empty() != installed.empty())
        return usage();

    const auto file = readFile (envelopePath);
    if (! file)
    {
        std::fprintf (stderr, "refused: cannot read %s\n", envelopePath.c_str());
        return 1;
    }

    const auto compiledIn = TrustRoot::release().keys().size();
    std::printf ("release keys compiled in: %zu\n", compiledIn);
    if (expectedKeys && compiledIn != *expectedKeys)
    {
        std::fprintf (stderr, "refused: %zu release keys are compiled in, %zu expected\n", compiledIn, *expectedKeys);
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

    bool accepted = true;
    for (const auto& bundle : installed)
        accepted = archiveAccepted (offer, archivePath, bundle) && accepted;
    return accepted ? 0 : 1;
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
