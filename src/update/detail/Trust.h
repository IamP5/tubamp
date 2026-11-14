#pragma once

// PRIVATE to src/update. The cryptographic boundary: which keys are trusted, signature
// verification and hashing. Security.framework and CommonCrypto only; juce::RSAKey is
// never used. Nothing outside Trust.cpp names a Sec* type.

#include "Rules.h"

#include <filesystem>

namespace tubamp::update::detail
{
/** One ECDSA P-256 public key: the raw 65-byte X9.63 point (04 || X || Y). */
struct TrustedKey
{
    std::array<std::uint8_t, 65> point {};
    bool operator== (const TrustedKey&) const = default;
};

/** First 8 bytes of SHA-256 over the point. Derived, never listed by hand, so the
    trusted list and the signer cannot disagree about ids. */
KeyId keyIdOf (const TrustedKey&) noexcept;

enum class KeyFileError
{
    notOnePublicKeyBlock,   // no PEM block, a private key, or more than one block
    notUncompressedP256,    // another curve, a compressed point, or a point off the curve
};

/** The text of one key file: exactly one "-----BEGIN PUBLIC KEY-----" block holding a
    P-256 SubjectPublicKeyInfo with an uncompressed point. Text before the block (a
    comment line) is ignored. CMake's key guard runs this same function
    (tools/update/keycheck.cpp), so a file it accepts is a file the plugin trusts. */
Result<TrustedKey, KeyFileError> parsePublicKeyPem (std::string_view pem);

struct KeyFile
{
    std::string_view name, pem;
};

struct RefusedKeyFile
{
    std::string_view name;
    KeyFileError why;
};

enum class SignatureError
{
    unknownKey,   // no signature line names a trusted key
    invalid,      // a line names a trusted key and its signature does not verify
};

/** The keys whose signature makes an envelope authoritative. */
class TrustRoot
{
public:
    /** The release keys, compiled in from the .pub.pem files in src/update/keys. A
        plugin built with TUBAMP_UPDATES=ON has at least two (CMake refuses fewer, and
        any file parsePublicKeyPem refuses); other builds may have none, and then nothing
        verifies. If a file ever failed to parse here, the set would be empty, never
        partial. */
    static const TrustRoot& release();

    /** One key per file, or the first file that does not parse. A partial set is never
        built: a recovery key that dropped out silently would leave one key between the
        channel and its death. */
    static Result<TrustRoot, RefusedKeyFile> fromKeyFiles (std::span<const KeyFile>);

    /** Tests and `tubamp_update_tool --trust` only. */
    explicit TrustRoot (std::vector<TrustedKey> keys);

    const std::vector<TrustedKey>& keys() const noexcept { return trusted; }

    /** Accepts when at least one line names a trusted key and verifies under it with
        kSecKeyAlgorithmECDSASignatureMessageX962SHA256 over signedMessage(). A line that
        names a trusted key and fails rejects the envelope outright, even if another line
        would pass. Lines naming unknown keys are skipped, so a release signed by an old
        and a new key verifies on both generations of client. The only producer of
        VerifiedPayload. */
    Result<VerifiedPayload, SignatureError> verify (const SignedEnvelope&) const;

private:
    std::vector<TrustedKey> trusted;
};

/** Incremental SHA-256 (CC_SHA256). The archive is hashed as its bytes are written. */
class Sha256Stream
{
public:
    Sha256Stream() noexcept;
    void update (ByteView) noexcept;
    Sha256 finish() noexcept;

private:
    alignas (8) std::array<std::uint8_t, 128> state;   // CC_SHA256_CTX storage
};

Sha256 sha256 (ByteView) noexcept;

using Refusal = std::variant<EnvelopeError, SignatureError, ManifestError>;

/** split, then verify, then parse: the one path from bytes to a Manifest, shared by a
    fresh download, the cached envelope and the release gate. */
Result<Manifest, Refusal> openEnvelope (ByteView file, const TrustRoot&);

/** The bundle is signed (ad hoc is enough) and every sealed file, every architecture
    and Info.plist still match the signature: SecStaticCodeCheckValidity with
    kSecCSCheckAllArchitectures | kSecCSStrictValidate. Reads the files; loads nothing. */
bool codeSealIntact (const std::filesystem::path& bundle);
} // namespace tubamp::update::detail
