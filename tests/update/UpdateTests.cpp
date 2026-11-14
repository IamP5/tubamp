// The update core's tests, run by `tubamp_update_tool test`. Every assertion goes through
// the functions the shell and the release gate call; no test reaches past a private
// constructor. Signed inputs come from in-memory keys (TestSigner) and from envelopes
// that openssl signed (tests/update/fixtures, written by make-fixtures.sh).

#include "UpdateTests.h"

#include "TestSigner.h"
#include "detail/Machine.h"
#include "tubamp_update_fixtures.inc"
#include "tubamp_update_release_keys.inc"

#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <set>

namespace tubamp::update::test
{
namespace
{
int passed = 0, failed = 0;

void check (bool ok, const std::string& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::printf ("  FAIL  %s\n", what.c_str());
    }
}

void group (const char* name) { std::printf ("%s\n", name); }

Version v (std::string_view s) { return *Version::parse (s); }

std::string_view fixture (std::string_view name)
{
    for (const auto& f : fixtureFiles)
        if (f.name == name)
            return f.text;
    throw std::runtime_error ("missing fixture " + std::string (name));
}

TrustedKey fixtureKey (std::string_view name) { return *parsePublicKeyPem (fixture (name)); }

constexpr std::string_view hashHex = "0c47c115a3f4cf9bd3e0b2e0f1e1c7b0d1f2a3b4c5d6e7f8091a2b3c4d5e6f70";

std::string payload (std::string_view version, std::string_view extra = "",
                     std::string_view size = "17345678", std::string_view minOs = "11.0",
                     std::string_view product = "com.t0audio.tubamp", std::string_view hash = hashHex)
{
    return std::string ("{\"product\":\"") + std::string (product) + "\",\"version\":\"" + std::string (version)
         + "\",\"minimumMacOS\":\"" + std::string (minOs) + "\",\"archive\":{\"sha256\":\"" + std::string (hash)
         + "\",\"size\":" + std::string (size) + "}" + std::string (extra) + "}";
}

const TestSigner& signerA() { static const TestSigner s; return s; }
const TestSigner& signerB() { static const TestSigner s; return s; }
const TrustRoot& trustA() { static const TrustRoot t ({ signerA().publicKey() }); return t; }

Result<Manifest, Refusal> open (std::string_view json)
{
    return openEnvelope (makeEnvelope (json, { &signerA() }), trustA());
}

Manifest manifest (std::string_view json) { return open (json).value(); }

OfferContext context (std::string_view running, std::optional<Version> highest = std::nullopt, OsVersion os = { 15, 0 })
{
    return { v (running), highest, os };
}

std::optional<Offer> offerFor (std::string_view version, std::string_view running = "0.3.0")
{
    const auto evaluation = evaluate (manifest (payload (version)), context (running));
    if (const auto* offer = std::get_if<Offer> (&evaluation))
        return *offer;
    return std::nullopt;
}

template <class E, class T>
bool refusedWith (const Result<T, Refusal>& r, E expected)
{
    if (r)
        return false;
    const auto* e = std::get_if<E> (&r.error());
    return e != nullptr && *e == expected;
}

// ---------------------------------------------------------------------------------

void versions()
{
    group ("version");
    for (auto good : { "0.0.0", "0.1.0", "1.2.3", "65535.255.255", "10.0.0" })
        check (Version::parse (good) && Version::parse (good)->toString() == good, std::string ("parses ") + good);
    for (auto bad : { "", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.256.0", "1.2.256", "65536.0.0",
                      "1.2.3-rc1", " 1.2.3", "1.2.3 ", "a.b.c", "1..3", "+1.2.3", "1.2.3\n" })
        check (! Version::parse (bad), std::string ("refuses \"") + bad + "\"");

    const std::uint16_t majors[] = { 0, 1, 2, 255, 256, 65535 };
    const std::uint8_t smalls[] = { 0, 1, 9, 10, 254, 255 };
    std::vector<Version> all;
    for (auto ma : majors) for (auto mi : smalls) for (auto pa : smalls) all.push_back ({ ma, mi, pa });
    bool sameOrder = true;
    for (const auto& a : all)
        for (const auto& b : all)
            sameOrder = sameOrder && ((a < b) == (a.audioComponentVersion() < b.audioComponentVersion()));
    check (sameOrder, "Version order equals AudioComponents integer order over " + std::to_string (all.size() * all.size()) + " pairs");
    check (v ("0.1.0").audioComponentVersion() == 256, "0.1.0 is AudioComponents version 256");
}

void envelopes()
{
    group ("envelope.split");
    const auto good = toBytes (fixture ("good-0.4.0.signed"));
    const auto split = SignedEnvelope::split (good);
    check (split && split.value().signatures().size() == 1, "openssl fixture splits into one signature");
    check (split && toText (split.value().payload()).starts_with ("{\"product\""), "payload is the bytes after the '.' line");

    const std::string magic = std::string (envelopeMagic) + "\n";
    const std::string sigLine = signatureLine (signerA().id(), signerA().sign (toBytes ("x")));
    auto splitText = [] (const std::string& s) { return SignedEnvelope::split (toBytes (s)); };
    auto refused = [&] (const std::string& s, EnvelopeError e)
    {
        const auto r = splitText (s);
        return ! r && r.error() == e;
    };

    check (refused ("", EnvelopeError::badMagic), "empty file: badMagic");
    check (refused ("tubamp-update-v2\n" + sigLine + "\n.\n{}", EnvelopeError::badMagic), "v2 magic: badMagic");
    check (refused (std::string (envelopeMagic), EnvelopeError::badMagic), "magic without newline: badMagic");
    check (refused (magic + ".\n{}", EnvelopeError::noSignature), "no sig line: noSignature");
    std::string five = magic;
    for (int i = 0; i < 5; ++i) five += sigLine + "\n";
    check (refused (five + ".\n{}", EnvelopeError::tooManySignatures), "five sig lines: tooManySignatures");
    std::string four = magic;
    for (int i = 0; i < 4; ++i) four += sigLine + "\n";
    check (splitText (four + ".\n{}") && true, "four sig lines split");

    std::string upper = sigLine;
    for (std::size_t i = 4; i < 20; ++i) upper[i] = char (std::toupper (upper[i]));
    const bool kidHasLetters = upper != sigLine;
    check (! kidHasLetters || refused (magic + upper + "\n.\n{}", EnvelopeError::malformedLine), "uppercase key id: malformedLine");
    check (refused (magic + sigLine + "\r\n.\n{}", EnvelopeError::malformedLine), "CR in header: malformedLine");
    check (refused (magic + "sig 0011223344556677 not*base64\n.\n{}", EnvelopeError::malformedLine), "bad base64: malformedLine");
    check (refused (magic + "sig 0011223344556677\n.\n{}", EnvelopeError::malformedLine), "no signature field: malformedLine");
    check (refused (magic + "sig  " + sigLine.substr (4) + "\n.\n{}", EnvelopeError::malformedLine), "double space: malformedLine");
    check (refused (magic + "\n" + sigLine + "\n.\n{}", EnvelopeError::malformedLine), "blank header line: malformedLine");
    check (refused (magic + sigLine + "\n{}", EnvelopeError::noTerminator), "no '.' line: noTerminator");
    check (refused (magic + sigLine + "\n.\n" + std::string (maxPayloadBytes + 1, ' '), EnvelopeError::tooLarge), "payload over 8 KiB: tooLarge");
    check (splitText (magic + sigLine + "\n.\n" + std::string (maxPayloadBytes, ' ')) && true, "payload of exactly 8 KiB splits");
    check (refused (std::string (maxEnvelopeBytes + 1, 'x'), EnvelopeError::tooLarge), "file over 16 KiB: tooLarge, before looking inside");
}

void trust()
{
    group ("trust.verify");
    const auto& a = signerA();
    const auto& b = signerB();
    const auto json = payload ("0.4.0");
    const TrustRoot trustB ({ b.publicKey() }), trustAB ({ a.publicKey(), b.publicKey() });

    auto verify = [] (const Bytes& file, const TrustRoot& root) -> std::optional<SignatureError>
    {
        const auto env = SignedEnvelope::split (file);
        if (! env) throw std::runtime_error ("test envelope does not split");
        const auto r = root.verify (env.value());
        return r ? std::nullopt : std::optional (r.error());
    };
    const auto messageFor = [] (std::string_view p) { return toBytes (std::string (envelopeMagic) + "\n" + std::string (p)); };
    const auto assemble = [] (std::initializer_list<std::string> lines, std::string_view p)
    {
        std::string text = std::string (envelopeMagic) + "\n";
        for (const auto& l : lines) text += l + "\n";
        return toBytes (text + ".\n" + std::string (p));
    };

    const auto good = makeEnvelope (json, { &a });
    check (! verify (good, trustA()), "signed by a trusted key: verifies");
    {
        const auto env = SignedEnvelope::split (good).value();
        const auto r = trustA().verify (env);
        check (r && toText (r.value().bytes()) == json, "the verified payload is exactly the signed bytes");
    }
    check (verify (good, trustB) == SignatureError::unknownKey, "signed by an untrusted key: unknownKey");
    check (verify (good, TrustRoot ({})) == SignatureError::unknownKey, "no trusted keys: nothing verifies");

    auto flipped = good;
    flipped[flipped.size() - 3] ^= 0x01;
    check (verify (flipped, trustA()) == SignatureError::invalid, "one payload byte flipped: invalid");
    auto appended = good;
    appended.push_back (' ');
    check (verify (appended, trustA()) == SignatureError::invalid, "a byte appended to the payload: invalid");

    auto der = a.sign (messageFor (json));
    auto truncated = der;
    truncated.resize (truncated.size() - 2);
    check (verify (assemble ({ signatureLine (a.id(), truncated) }, json), trustA()) == SignatureError::invalid, "signature truncated: invalid");
    check (verify (assemble ({ signatureLine (b.id(), der) }, json), trustAB) == SignatureError::invalid, "A's signature under B's key id: invalid");
    check (! verify (assemble ({ signatureLine (b.id(), b.sign (messageFor (json))), signatureLine (a.id(), der) }, json), trustA()),
           "rotation: an unknown key's line is skipped, the trusted one verifies");
    check (! verify (assemble ({ signatureLine (b.id(), b.sign (messageFor (json))), signatureLine (a.id(), der) }, json), trustAB),
           "rotation: both lines trusted and valid: verifies");
    auto badB = b.sign (messageFor (json));
    badB.back() ^= 0x01;
    check (verify (assemble ({ signatureLine (b.id(), badB), signatureLine (a.id(), der) }, json), trustAB) == SignatureError::invalid,
           "a trusted key's bad line rejects the envelope although another line is valid");
    check (verify (assemble ({ signatureLine (a.id(), a.sign (toBytes (json))) }, json), trustA()) == SignatureError::invalid,
           "signature over the payload without the domain prefix: invalid");

    group ("trust.interop (envelopes signed by openssl)");
    const TrustRoot fixA ({ fixtureKey ("test-key-a.pub.pem") }), fixB ({ fixtureKey ("test-key-b.pub.pem") });
    auto openFixture = [] (std::string_view name, const TrustRoot& root) { return openEnvelope (toBytes (fixture (name)), root); };
    const auto opened = openFixture ("good-0.4.0.signed", fixA);
    check (opened && opened.value().version() == v ("0.4.0"), "openssl-signed envelope opens under its public key");
    check (refusedWith (openFixture ("good-0.4.0.signed", fixB), SignatureError::unknownKey), "and not under another key");
    check (openFixture ("rotated-0.4.0.signed", fixA) && openFixture ("rotated-0.4.0.signed", fixB), "a two-key envelope opens under either key");
    check (refusedWith (openFixture ("no-prefix-0.4.0.signed", fixA), SignatureError::invalid), "openssl signature without the domain prefix: invalid");
    check (refusedWith (openFixture ("garbage-payload.signed", fixA), ManifestError::notJson), "a validly signed non-JSON payload fails only at parse");
    const auto header = SignedEnvelope::split (toBytes (fixture ("good-0.4.0.signed"))).value();
    check (header.signatures()[0].key == keyIdOf (fixtureKey ("test-key-a.pub.pem")),
           "key id derived here equals the pipeline's `tail -c 65 | shasum -a 256`");

    group ("trust.order (signature before parse)");
    auto garbageBadSig = makeEnvelope ("not json at all", { &b });
    check (refusedWith (openEnvelope (garbageBadSig, trustA()), SignatureError::unknownKey), "unsigned garbage is refused by the signature check, never parsed");
    auto badJsonGoodSig = makeEnvelope ("{\"product\":", { &a });
    check (refusedWith (openEnvelope (badJsonGoodSig, trustA()), ManifestError::notJson), "the parser sees only verified bytes");

    group ("trust.keys");
    const auto pemA = fixture ("test-key-a.pub.pem");
    check (parsePublicKeyPem (pemA).has_value(), "a PEM public key with a comment line before it parses");
    std::string offCurve (pemA);
    const auto at = offCurve.find ("-----END") - 6;
    offCurve[at] = offCurve[at] == 'A' ? 'B' : 'A';
    check (! parsePublicKeyPem (offCurve), "a point edited off the curve is refused");
    check (! parsePublicKeyPem ("-----BEGIN EC PRIVATE KEY-----\nMHcCAQEE\n-----END EC PRIVATE KEY-----\n"), "a private key PEM is refused");
    check (! parsePublicKeyPem ("nothing here"), "text without a PEM block is refused");

    group ("trust.release-set");
    const auto& release = TrustRoot::release();
    std::printf ("  release keys compiled in: %zu\n", release.keys().size());
    bool noTestKey = true;
    for (const auto& k : release.keys())
        for (auto name : { "test-key-a.pub.pem", "test-key-b.pub.pem" })
            noTestKey = noTestKey && keyIdOf (k) != keyIdOf (fixtureKey (name));
    check (noTestKey, "no test fixture key is in the release set");
    check (release.keys().size() == releaseKeyFiles.size(), "every committed release key file parses");

    group ("trust.sha256");
    check (hex (sha256 (toBytes ("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of \"abc\"");
    Sha256Stream stream;
    stream.update (toBytes ("a"));
    stream.update (toBytes ("bc"));
    check (stream.finish() == sha256 (toBytes ("abc")), "streamed in pieces equals one shot");
}

void manifests()
{
    group ("payload.parse");
    const auto m = open (payload ("0.4.0", ",\"publishedAt\":\"x\",\"notes\":\"y\",\"url\":\"https://evil.example/x.zip\""));
    check (m && m.value().version() == v ("0.4.0") && m.value().product() == "com.t0audio.tubamp"
               && m.value().minimumMacOS() == OsVersion { 11, 0 } && m.value().archive().size == 17345678
               && hex (m.value().archive().sha256) == hashHex,
           "every field read, unknown fields ignored");

    auto refused = [] (const std::string& json, ManifestError e) { return refusedWith (open (json), e); };
    check (refused ("[1,2]", ManifestError::notJson), "a JSON array: notJson");
    check (refused (payload ("0.4.0") + " x", ManifestError::notJson), "trailing bytes: notJson");
    check (refused ("{\"version\":\"0.4.0\",\"minimumMacOS\":\"11.0\",\"archive\":{\"sha256\":\"" + std::string (hashHex) + "\",\"size\":1}}",
                    ManifestError::missingField), "no product: missingField");
    check (refused ("{\"product\":\"com.t0audio.tubamp\",\"version\":\"0.4.0\",\"minimumMacOS\":\"11.0\"}", ManifestError::missingField), "no archive: missingField");
    check (refused ("{\"product\":\"com.t0audio.tubamp\",\"version\":\"0.4.0\",\"minimumMacOS\":\"11.0\",\"archive\":{\"size\":1}}",
                    ManifestError::missingField), "no sha256: missingField");
    check (refused ("{\"product\":\"com.t0audio.tubamp\",\"version\":4,\"minimumMacOS\":\"11.0\",\"archive\":{}}", ManifestError::missingField), "version not a string: missingField");
    check (refused (payload ("0.4"), ManifestError::badVersion), "version 0.4: badVersion");
    check (refused (payload ("0.4.0", "", "1", "eleven"), ManifestError::badMacOS), "minimumMacOS eleven: badMacOS");
    std::string upperHash (hashHex);
    upperHash[0] = 'C';
    upperHash[1] = 'F';
    check (refused (payload ("0.4.0", "", "1", "11.0", "com.t0audio.tubamp", upperHash), ManifestError::badHash), "uppercase hash: badHash");
    check (refused (payload ("0.4.0", "", "1", "11.0", "com.t0audio.tubamp", hashHex.substr (1)), ManifestError::badHash), "63-digit hash: badHash");
    for (auto size : { "0", "-1", "1.5", "\"12\"", "null" })
        check (refused (payload ("0.4.0", "", size), ManifestError::badSize), std::string ("size ") + size + ": badSize");
}

void offers()
{
    group ("offer.evaluate");
    auto eval = [] (std::string_view json, const OfferContext& ctx) { return evaluate (manifest (json), ctx); };
    auto noOffer = [] (const Evaluation& e, NoOffer expected)
    {
        const auto* n = std::get_if<NoOffer> (&e);
        return n != nullptr && *n == expected;
    };

    const auto newer = eval (payload ("0.4.0"), context ("0.3.0"));
    const auto* offer = std::get_if<Offer> (&newer);
    check (offer != nullptr && offer->version() == v ("0.4.0"), "0.4.0 over running 0.3.0: offered");
    check (offer != nullptr && offer->archive().size == 17345678 && hex (offer->archive().sha256) == hashHex,
           "the offer carries the signed size and hash, and nothing else supplies them");
    check (noOffer (eval (payload ("0.3.0"), context ("0.3.0")), NoOffer::upToDate), "equal to running: upToDate");
    check (noOffer (eval (payload ("0.2.9"), context ("0.3.0")), NoOffer::upToDate), "below running: upToDate");
    check (noOffer (eval (payload ("0.4.0"), context ("0.3.0", v ("0.5.0"))), NoOffer::replayed), "below the highest seen: replayed");
    check (std::holds_alternative<Offer> (eval (payload ("0.5.0"), context ("0.3.0", v ("0.5.0")))), "equal to the highest seen: offered");
    check (noOffer (eval (payload ("0.4.0", "", "1", "11.0", "com.example.other"), context ("0.3.0")), NoOffer::wrongProduct), "another product: wrongProduct");
    check (noOffer (eval (payload ("0.4.0", "", std::to_string (ArchiveSpec::maxBytes + 1)), context ("0.3.0")), NoOffer::oversized), "above the size ceiling: oversized");
    check (std::holds_alternative<Offer> (eval (payload ("0.4.0", "", std::to_string (ArchiveSpec::maxBytes)), context ("0.3.0"))), "exactly the ceiling: offered");
    check (noOffer (eval (payload ("0.4.0", "", "1", "15.0"), context ("0.3.0", std::nullopt, { 14, 6 })), NoOffer::needsNewerMacOS), "needs macOS 15 on 14.6: needsNewerMacOS");
    check (std::holds_alternative<Offer> (eval (payload ("0.4.0", "", "1", "15.0"), context ("0.3.0", std::nullopt, { 15, 0 }))), "needs macOS 15 on 15.0: offered");

    group ("offer.supersedes");
    check (supersedes (manifest (payload ("0.4.0")), std::nullopt), "anything supersedes an empty cache");
    check (supersedes (manifest (payload ("0.4.0")), v ("0.4.0")), "the same version supersedes");
    check (! supersedes (manifest (payload ("0.3.0")), v ("0.4.0")), "a lower version never replaces the cache");
}

void urls()
{
    group ("urls");
    const auto offer = offerFor ("0.4.0");
    check (envelopeUrl().text() == "https://github.com/IamP5/tubamp/releases/latest/download/update-v1.signed", "envelope URL is the constant");
    check (offer && archiveUrl (*offer).text() == "https://github.com/IamP5/tubamp/releases/download/v0.4.0/tubamp-0.4.0-macos-universal.zip",
           "archive URL is built from the version alone (the payload's url field is never read)");
    check (releasePageUrl (v ("0.4.0")).text() == "https://github.com/IamP5/tubamp/releases/tag/v0.4.0", "release page URL");
    check (hostAllowed (envelopeUrl().text()) && offer && hostAllowed (archiveUrl (*offer).text()), "the built URLs pass the host check");

    group ("urls.hostAllowed");
    for (auto ok : { "https://github.com/x", "https://objects.githubusercontent.com/a?b=c",
                     "https://release-assets.githubusercontent.com/x#y", "https://github.com" })
        check (hostAllowed (ok), std::string ("allows ") + ok);
    for (auto bad : { "http://github.com/x", "https://github.com.evil.example/x", "https://evil.example/github.com",
                      "https://raw.githubusercontent.com/x", "https://github.com@evil.example/x", "https://GitHub.com/x",
                      "https://github.com./x", "https://140.82.112.3/x", "https://github.com:443/x", "ftp://github.com/x",
                      "HTTPS://github.com/x", "https:///x", "github.com/x", "" })
        check (! hostAllowed (bad), std::string ("refuses \"") + bad + "\"");
}

void archives()
{
    group ("archive.planExtraction");
    using K = EntryKind;
    const std::vector<ArchiveEntry> good = {
        { "tubamp.component/", 0, K::directory },
        { "tubamp.component/Contents/", 0, K::directory },
        { "tubamp.component/Contents/Info.plist", 1600, K::file },
        { "tubamp.component/Contents/MacOS/tubamp", 17000000, K::file },
        { "tubamp.component/Contents/_CodeSignature/CodeResources", 2200, K::file },
    };
    const auto plan = planExtraction (good);
    check (plan && plan.value().size() == good.size(), "a well-formed bundle plans every entry");
    if (plan)
    {
        std::map<std::string, unsigned> modes;
        for (const auto& e : plan.value())
        {
            std::string path;
            for (const auto& c : e.components) path += (path.empty() ? "" : "/") + c;
            modes[path] = e.mode;
        }
        check (modes["tubamp.component/Contents/MacOS/tubamp"] == 0755, "the executable gets 0755");
        check (modes["tubamp.component/Contents/Info.plist"] == 0644, "other files get 0644");
        check (modes["tubamp.component/Contents"] == 0755, "directories get 0755");
    }

    auto with = [&] (std::vector<ArchiveEntry> extra)
    {
        auto entries = good;
        entries.insert (entries.end(), extra.begin(), extra.end());
        return entries;
    };
    auto refused = [] (const std::vector<ArchiveEntry>& entries, ArchiveViolation expected)
    {
        const auto r = planExtraction (entries);
        return ! r && r.error() == expected;
    };
    const std::pair<ArchiveEntry, ArchiveViolation> rows[] = {
        { { "/etc/x", 1, K::file }, ArchiveViolation::absolutePath },
        { { "tubamp.component/../x", 1, K::file }, ArchiveViolation::dotSegment },
        { { "tubamp.component/./x", 1, K::file }, ArchiveViolation::dotSegment },
        { { "../tubamp.component/x", 1, K::file }, ArchiveViolation::dotSegment },
        { { "tubamp.component//x", 1, K::file }, ArchiveViolation::emptyComponent },
        { { "tubamp.component/x/", 1, K::file }, ArchiveViolation::emptyComponent },
        { { "tubamp.component\\x", 1, K::file }, ArchiveViolation::badCharacter },
        { { std::string ("tubamp.component/a\0b", 20), 1, K::file }, ArchiveViolation::badCharacter },
        { { "tubamp.component/caf\xc3\xa9", 1, K::file }, ArchiveViolation::badCharacter },
        { { "other.component/x", 1, K::file }, ArchiveViolation::outsideBundle },
        { { "x", 1, K::file }, ArchiveViolation::outsideBundle },
        { { "tubamp.component", 1, K::file }, ArchiveViolation::outsideBundle },
        { { "tubamp.component/Contents/link", 0, K::symlink }, ArchiveViolation::symlink },
        { { "tubamp.component/Contents/fifo", 0, K::other }, ArchiveViolation::specialFile },
        { { "tubamp.component/Contents/info.PLIST", 1, K::file }, ArchiveViolation::duplicate },
        { { "tubamp.component/Contents/Info.plist/x", 1, K::file }, ArchiveViolation::conflict },
    };
    for (const auto& [entry, violation] : rows)
        check (refused (with ({ entry }), violation), "refuses \"" + entry.path + "\"");

    check (refused ({ { "tubamp.component/Contents/Info.plist", 1, K::file } }, ArchiveViolation::missingExecutable), "no executable: missingExecutable");
    check (refused (with ({ { "tubamp.component/Contents/MacOS/tubamp/", 0, K::directory } }), ArchiveViolation::duplicate), "executable named twice as file and directory: duplicate");
    check (refused (with ({ { "tubamp.component/big", maxUnpackedBytes, K::file } }), ArchiveViolation::tooLarge), "declared sizes over 512 MiB: tooLarge");
    check (refused (with ({ { "tubamp.component/huge", ~std::uint64_t (0), K::file } }), ArchiveViolation::tooLarge), "a size that would overflow the sum: tooLarge");
    std::vector<ArchiveEntry> many = good;
    for (std::size_t i = many.size(); i <= maxArchiveEntries; ++i)
        many.push_back ({ "tubamp.component/f" + std::to_string (i), 1, K::file });
    check (refused (many, ArchiveViolation::tooManyEntries), "513 entries: tooManyEntries");
    many.pop_back();
    check (planExtraction (many) && true, "512 entries plan");
}

void prefs()
{
    group ("prefs");
    using namespace std::chrono;
    const sys_seconds now { seconds (1'800'000'000) };
    Prefs p;
    check (isCheckDue (p, now), "never checked: due");
    p.lastCheckAt = now - hours (23);
    check (! isCheckDue (p, now), "checked 23 h ago: not due");
    p.lastCheckAt = now - hours (24);
    check (isCheckDue (p, now), "checked 24 h ago: due");
    p.lastCheckAt = now + hours (1);
    check (isCheckDue (p, now), "last check in the future: due");
    p.lastCheckAt.reset();
    p.autoCheck = false;
    check (! isCheckDue (p, now), "automatic checks off: never due");

    Prefs q;
    apply (q, PrefsPatch { {}, {}, v ("0.5.0") });
    apply (q, PrefsPatch { {}, {}, v ("0.4.0") });
    check (q.announced == v ("0.5.0"), "announced only ever rises");
    apply (q, PrefsPatch { false, true, {} });
    check (! q.autoCheck && q.disclosed && q.announced == v ("0.5.0"), "a patch changes only the fields it names");
}
} // namespace

int runUpdateTests()
{
    versions();
    envelopes();
    trust();
    manifests();
    offers();
    urls();
    archives();
    prefs();
    runMachineTests (passed, failed);

    std::printf ("\n%d passed, %d failed\n", passed, failed);
    return failed;
}

// Used by MachineTests.cpp: an offer only exists through the signed path.
std::optional<Offer> signedOffer (std::string_view version, std::string_view running)
{
    return offerFor (version, running);
}
} // namespace tubamp::update::test
