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

/** A "-----BEGIN PUBLIC KEY-----" block holding a P-256 SubjectPublicKeyInfo. Text
    before the block (a comment line) is ignored. nullopt for anything else, including
    a point that is not on the curve. */
std::optional<TrustedKey> parsePublicKeyPem (std::string_view pem);

enum class SignatureError
{
    unknownKey,   // no signature line names a trusted key
    invalid,      // a line names a trusted key and its signature does not verify
};

/** The keys whose signature makes an envelope authoritative. */
class TrustRoot
{
public:
    /** The release keys, compiled in from the .pub.pem files in
        src/update/keys. A plugin built with
        TUBAMP_UPDATES=ON has at least two (CMake refuses fewer); other builds may have
        none, and then nothing verifies. */
    static const TrustRoot& release();

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

/** Why an envelope gave no Manifest. Each stage reports its own error type. */
using Refusal = std::variant<EnvelopeError, SignatureError, ManifestError>;

/** split, then verify, then parse: the one path from bytes to a Manifest, shared by a
    fresh download, the cached envelope and the release gate. */
Result<Manifest, Refusal> openEnvelope (ByteView file, const TrustRoot&);

/** The bundle is signed (ad hoc is enough) and every sealed file, every architecture
    and Info.plist still match the signature: SecStaticCodeCheckValidity with
    kSecCSCheckAllArchitectures | kSecCSStrictValidate. Reads the files; loads nothing. */
bool codeSealIntact (const std::filesystem::path& bundle);
} // namespace tubamp::update::detail
