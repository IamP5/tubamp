// tubamp_update_tool: the update core's test runner and the release gate.
//
//   tubamp_update_tool test
//       Runs every table. Exit 1 on any failure.
//
//   tubamp_update_tool verify-envelope --envelope <file> --running <X.Y.Z>
//                      [--highest-seen <X.Y.Z>] [--macos <X.Y>] [--trust <pub.pem>]...
//       Opens the envelope exactly as the plugin does and prints the offer or the refusal.
//       Without --trust it uses the release keys compiled into this build. Exit 0 only
//       when there is an offer.

#include "UpdateTests.h"
#include "detail/Trust.h"

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
                  "                          [--highest-seen <X.Y.Z>] [--macos <X.Y>] [--trust <pub.pem>]...\n");
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
} // namespace

int main (int argc, char** argv)
{
    if (argc >= 2 && std::string (argv[1]) == "test")
        return tubamp::update::test::runUpdateTests() == 0 ? 0 : 1;
    if (argc >= 2 && std::string (argv[1]) == "verify-envelope")
        return verifyEnvelope (argc, argv);
    return usage();
}
