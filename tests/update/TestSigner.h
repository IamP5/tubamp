#pragma once

// A P-256 key made in memory by Security.framework for one test run. It never touches
// the keychain or the disk, so no private key exists outside this process.

#include "detail/Trust.h"

#include <initializer_list>
#include <memory>

namespace tubamp::update::test
{
using namespace tubamp::update::detail;

class TestSigner
{
public:
    TestSigner();

    const TrustedKey& publicKey() const noexcept { return pub; }
    KeyId id() const noexcept { return keyIdOf (pub); }

    /** DER ECDSA-P256-SHA256 over `message`, as `openssl dgst -sha256 -sign` writes it. */
    Bytes sign (ByteView message) const;

private:
    std::shared_ptr<const void> key;
    TrustedKey pub;
};

Bytes toBytes (std::string_view);
std::string toText (ByteView);
std::string hex (ByteView);
std::string base64 (ByteView);

/** "sig <kid hex> <base64 der>" */
std::string signatureLine (const KeyId&, ByteView der);

/** A well-formed update-v1.signed with one line per signer, each over magic, LF, payload. */
Bytes makeEnvelope (std::string_view payload, std::initializer_list<const TestSigner*> signers);
} // namespace tubamp::update::test
