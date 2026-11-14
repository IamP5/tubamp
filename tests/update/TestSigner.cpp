#include "TestSigner.h"

#include <Security/Security.h>

#include <stdexcept>

namespace tubamp::update::test
{
namespace
{
    template <class T>
    struct CFOwned
    {
        T ref = nullptr;
        ~CFOwned() { if (ref != nullptr) CFRelease (ref); }
    };

    Bytes copyData (CFDataRef data)
    {
        const auto* p = CFDataGetBytePtr (data);
        return Bytes (p, p + CFDataGetLength (data));
    }
}

TestSigner::TestSigner()
{
    int bits = 256;
    CFOwned<CFNumberRef> size { CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &bits) };
    const void* names[] = { kSecAttrKeyType, kSecAttrKeySizeInBits, kSecAttrIsPermanent };
    const void* values[] = { kSecAttrKeyTypeECSECPrimeRandom, size.ref, kCFBooleanFalse };
    CFOwned<CFDictionaryRef> attributes { CFDictionaryCreate (kCFAllocatorDefault, names, values, 3,
                                                              &kCFTypeDictionaryKeyCallBacks,
                                                              &kCFTypeDictionaryValueCallBacks) };
    SecKeyRef privateKey = SecKeyCreateRandomKey (attributes.ref, nullptr);
    if (privateKey == nullptr)
        throw std::runtime_error ("SecKeyCreateRandomKey failed");
    key = std::shared_ptr<const void> (privateKey, [] (const void* k) { CFRelease (k); });

    CFOwned<SecKeyRef> publicKey { SecKeyCopyPublicKey (privateKey) };
    CFOwned<CFDataRef> point { SecKeyCopyExternalRepresentation (publicKey.ref, nullptr) };
    const auto bytes = copyData (point.ref);
    if (bytes.size() != pub.point.size())
        throw std::runtime_error ("unexpected public key size");
    std::copy (bytes.begin(), bytes.end(), pub.point.begin());
}

Bytes TestSigner::sign (ByteView message) const
{
    CFOwned<CFDataRef> msg { CFDataCreate (kCFAllocatorDefault, message.data(), CFIndex (message.size())) };
    CFOwned<CFDataRef> sig { SecKeyCreateSignature ((SecKeyRef) key.get(),
                                                    kSecKeyAlgorithmECDSASignatureMessageX962SHA256,
                                                    msg.ref, nullptr) };
    if (sig.ref == nullptr)
        throw std::runtime_error ("SecKeyCreateSignature failed");
    return copyData (sig.ref);
}

Bytes toBytes (std::string_view s) { return Bytes (s.begin(), s.end()); }

std::string toText (ByteView b) { return std::string (b.begin(), b.end()); }

std::string hex (ByteView b)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (auto byte : b)
    {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}

std::string base64 (ByteView b)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < b.size(); i += 3)
    {
        const std::uint32_t n = (std::uint32_t (b[i]) << 16)
                              | (i + 1 < b.size() ? std::uint32_t (b[i + 1]) << 8 : 0)
                              | (i + 2 < b.size() ? std::uint32_t (b[i + 2]) : 0);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += i + 1 < b.size() ? alphabet[(n >> 6) & 63] : '=';
        out += i + 2 < b.size() ? alphabet[n & 63] : '=';
    }
    return out;
}

std::string signatureLine (const KeyId& id, ByteView der)
{
    return "sig " + hex (id) + " " + base64 (der);
}

Bytes makeEnvelope (std::string_view payload, std::initializer_list<const TestSigner*> signers)
{
    const auto message = toBytes (std::string (envelopeMagic) + "\n" + std::string (payload));
    std::string text = std::string (envelopeMagic) + "\n";
    for (const auto* signer : signers)
        text += signatureLine (signer->id(), signer->sign (message)) + "\n";
    text += ".\n";
    text += payload;
    return toBytes (text);
}
} // namespace tubamp::update::test
