#pragma once

// PRIVATE to src/update. Pure rules: no network, no filesystem, no clock. Everything is a
// function of its arguments, so tubamp_update_tool tests it with tables.

#include "../UpdateTypes.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace tubamp::update::detail
{
/** std::expected stand-in for C++20. Holds exactly one of T or E. */
template <class T, class E>
class Result
{
public:
    Result (T value) : state (std::move (value)) {}
    Result (E error) : state (std::move (error)) {}
    explicit operator bool() const noexcept { return state.index() == 0; }
    T& value() & { return std::get<0> (state); }
    const T& value() const& { return std::get<0> (state); }
    T&& value() && { return std::get<0> (std::move (state)); }
    const E& error() const { return std::get<1> (state); }

private:
    std::variant<T, E> state;
};

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;
using Sha256 = std::array<std::uint8_t, 32>;
using KeyId = std::array<std::uint8_t, 8>;   // first 8 bytes of SHA-256(public point)

/** Strict standard base64: length a multiple of 4, padding only at the end, no
    whitespace. Shared by the envelope header and PEM public keys. */
std::optional<Bytes> decodeBase64 (std::string_view) noexcept;

struct OsVersion
{
    int major = 0, minor = 0;

    /** "11", "11.0" or "11.0.1" (the patch is ignored). */
    static std::optional<OsVersion> parse (std::string_view) noexcept;
    auto operator<=> (const OsVersion&) const = default;
};

// update-v1.signed wire format:
//
//   tubamp-update-v1\n
//   sig <16 lowercase hex key id> <base64 DER ECDSA-P256-SHA256 signature>\n   (1..4)
//   .\n
//   <payload: JSON, at most 8 KiB, to end of file>
//
// The signed message is "tubamp-update-v1\n" followed by the payload, so a signature this
// key made for anything else is not an update. The header is ASCII lines split without a
// JSON parser: no structured parsing of attacker bytes happens before a signature holds.
// The format version lives in the file name; a v2 ships beside v1, never inside it.

inline constexpr std::string_view envelopeMagic = "tubamp-update-v1";
inline constexpr std::size_t maxEnvelopeBytes = 16 * 1024;
inline constexpr std::size_t maxPayloadBytes = 8 * 1024;
inline constexpr std::size_t maxSignatureLines = 4;

struct Signature
{
    KeyId key;
    Bytes der;
    bool operator== (const Signature&) const = default;
};

enum class EnvelopeError
{
    tooLarge,           // file above maxEnvelopeBytes or payload above maxPayloadBytes
    badMagic,
    noSignature,
    tooManySignatures,
    malformedLine,      // a header line that is not exactly "sig <kid> <base64>", or a CR
    noTerminator,
};

/** The file split into signature lines and payload. Nothing in it is trusted yet. */
class SignedEnvelope
{
public:
    static Result<SignedEnvelope, EnvelopeError> split (ByteView file);

    const std::vector<Signature>& signatures() const noexcept { return sigs; }
    ByteView payload() const noexcept { return body; }

    /** The exact bytes each signature covers: magic, LF, payload. */
    Bytes signedMessage() const;

private:
    SignedEnvelope (std::vector<Signature> s, Bytes p) : sigs (std::move (s)), body (std::move (p)) {}
    std::vector<Signature> sigs;
    Bytes body;
};

/** Payload bytes whose signature verified under a trusted key. Only TrustRoot::verify
    (detail/Trust.h) constructs one, and parseManifest takes nothing else, so parsing
    before verifying does not compile. */
class VerifiedPayload
{
public:
    ByteView bytes() const noexcept { return data; }

private:
    friend class TrustRoot;
    explicit VerifiedPayload (Bytes b) : data (std::move (b)) {}
    Bytes data;
};

// The payload, as the client reads it:
//
//   { "product": "com.t0audio.tubamp", "version": "0.4.0", "minimumMacOS": "11.0",
//     "archive": { "sha256": "<64 lowercase hex>", "size": 17345678 } }
//
// Unknown fields are ignored, so fields can be added inside v1. No URL is read.

struct ArchiveSpec
{
    std::uint64_t size = 0;   // exact byte count of the universal zip, > 0
    Sha256 sha256 {};

    /** Absolute ceiling, whatever a signed payload claims. */
    static constexpr std::uint64_t maxBytes = 128ull << 20;

    bool operator== (const ArchiveSpec&) const = default;
};

enum class ManifestError { notJson, missingField, badVersion, badMacOS, badHash, badSize };

class Manifest;
Result<Manifest, ManifestError> parseManifest (const VerifiedPayload&);

/** What a verified payload says. Only parseManifest constructs one, so every Manifest
    came from signed bytes. */
class Manifest
{
public:
    Version version() const noexcept { return v; }
    const std::string& product() const noexcept { return productId; }
    OsVersion minimumMacOS() const noexcept { return minOs; }
    const ArchiveSpec& archive() const noexcept { return spec; }

private:
    friend Result<Manifest, ManifestError> parseManifest (const VerifiedPayload&);
    Manifest (Version ver, std::string product, OsVersion os, ArchiveSpec a)
        : v (ver), productId (std::move (product)), minOs (os), spec (a) {}

    Version v;
    std::string productId;
    OsVersion minOs;
    ArchiveSpec spec;
};

inline constexpr std::string_view pinnedBundleId = "com.t0audio.tubamp";

struct OfferContext
{
    Version running;                      // compiled-in
    std::optional<Version> highestSeen;   // version inside the cached envelope
    OsVersion os;
};

enum class NoOffer
{
    wrongProduct,
    oversized,
    replayed,          // version below highestSeen: an older signed file served again
    upToDate,
    needsNewerMacOS,   // minimumMacOS above this Mac. Not an error: nothing to offer
};

class Offer;
using Evaluation = std::variant<Offer, NoOffer>;
Evaluation evaluate (const Manifest&, const OfferContext&);

/** A verified release this copy may install.

    Invariant, established by evaluate (the only producer): the version is above the
    running one and not below the highest seen, the product is pinned, this Mac's OS is
    supported, and the size and hash are the signed ones, within the ceiling. */
class Offer
{
public:
    Version version() const noexcept { return v; }
    const ArchiveSpec& archive() const noexcept { return spec; }
    bool operator== (const Offer&) const = default;

private:
    friend Evaluation evaluate (const Manifest&, const OfferContext&);
    Offer (Version version, ArchiveSpec archive) : v (version), spec (archive) {}
    Version v;
    ArchiveSpec spec;
};

/** May `incoming` replace the cached envelope? Only if its version is not below the
    cached one, which keeps the replay floor monotonic. */
bool supersedes (const Manifest& incoming, const std::optional<Version>& cached) noexcept;

// URLs. Built only here, from compiled-in constants and a Version. Nothing that was
// downloaded supplies a URL, and a Url cannot be made from a string elsewhere.

class Url
{
public:
    const std::string& text() const noexcept { return value; }
    bool operator== (const Url&) const = default;

private:
    friend Url envelopeUrl();
    friend Url archiveUrl (const Offer&);
    friend Url releasePageUrl (Version);
    friend std::optional<Url> repositoryUrl (std::string_view);
    explicit Url (std::string s) : value (std::move (s)) {}
    std::string value;
};

/** https://github.com/IamP5/tubamp/releases/latest/download/update-v1.signed
    The same for every user, version and CPU type. */
Url envelopeUrl();

/** https://github.com/IamP5/tubamp/releases/download/v{V}/tubamp-{V}-macos-universal.zip */
Url archiveUrl (const Offer&);

/** https://github.com/IamP5/tubamp/releases/tag/v{V} */
Url releasePageUrl (Version);

/** https://github.com/IamP5/tubamp/{path}, for `tubamp_update_tool fetch-check` only. The
    path is letters, digits, '.', '_', '-' and '/' with no empty or dot-dot segment, so the
    host and repository stay the compiled-in ones. The plugin never calls it. */
std::optional<Url> repositoryUrl (std::string_view path);

/** True only for an https URL whose host is exactly one entry of Hosts.def, with no
    user info and no port. Applied to the first URL and to every redirect hop. */
bool hostAllowed (std::string_view url) noexcept;

// Archive entry policy. The unpacker obeys the plan; it decides nothing.

inline constexpr std::string_view bundleRoot = "tubamp.component";
inline constexpr std::size_t maxArchiveEntries = 512;
inline constexpr std::uint64_t maxUnpackedBytes = 512ull << 20;

enum class EntryKind { file, directory, symlink, other };

struct ArchiveEntry
{
    std::string path;                   // as stored in the archive; "/" ends a directory
    std::uint64_t uncompressedSize = 0;
    EntryKind kind = EntryKind::other;
};

struct PlannedEntry
{
    std::vector<std::string> components;   // relative to the staging directory
    EntryKind kind = EntryKind::file;      // file or directory, nothing else
    std::uint64_t size = 0;
    unsigned mode = 0644;                  // assigned here; archive modes are ignored

    bool operator== (const PlannedEntry&) const = default;
};

enum class ArchiveViolation
{
    tooManyEntries,
    tooLarge,
    absolutePath,
    dotSegment,
    emptyComponent,
    badCharacter,        // backslash, control character or non-ASCII byte
    outsideBundle,       // first component is not tubamp.component, or the root is a file
    symlink,             // none at all: the bundle has none
    specialFile,
    duplicate,           // two entries name one path, also case-insensitively
    conflict,            // an entry lies under a path that is a file
    missingExecutable,   // no tubamp.component/Contents/MacOS/tubamp file
};

/** Validates every entry before one byte is written and returns exactly what may be
    created. Directories and Contents/MacOS/tubamp get 0755, other files 0644, so setuid
    bits and odd permissions cannot be expressed. */
Result<std::vector<PlannedEntry>, ArchiveViolation> planExtraction (std::span<const ArchiveEntry>);
} // namespace tubamp::update::detail
