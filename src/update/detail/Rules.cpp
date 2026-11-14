#include "Rules.h"

#include <json.hpp>

#include <algorithm>
#include <map>

namespace tubamp::update::detail
{
namespace
{
    constexpr std::string_view repoRoot = "https://github.com/IamP5/tubamp/";
    constexpr std::string_view repoBase = "https://github.com/IamP5/tubamp/releases/";

    constexpr std::string_view allowedHosts[] = {
#define TUBAMP_UPDATE_HOST(h) h,
#include "Hosts.def"
#undef TUBAMP_UPDATE_HOST
    };

    std::string_view asText (ByteView b) noexcept
    {
        return { reinterpret_cast<const char*> (b.data()), b.size() };
    }

    int hexValue (char c) noexcept
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;   // uppercase is refused: one spelling per value
    }

    template <std::size_t N>
    std::optional<std::array<std::uint8_t, N>> decodeHex (std::string_view s) noexcept
    {
        if (s.size() != N * 2)
            return std::nullopt;
        std::array<std::uint8_t, N> out {};
        for (std::size_t i = 0; i < N; ++i)
        {
            const int hi = hexValue (s[2 * i]), lo = hexValue (s[2 * i + 1]);
            if (hi < 0 || lo < 0)
                return std::nullopt;
            out[i] = std::uint8_t (hi * 16 + lo);
        }
        return out;
    }

    std::optional<Signature> parseSignatureLine (std::string_view line)
    {
        constexpr std::string_view prefix = "sig ";
        constexpr std::size_t maxBase64 = 128;   // a DER P-256 signature is at most 72 bytes

        if (! line.starts_with (prefix))
            return std::nullopt;
        line.remove_prefix (prefix.size());

        if (line.size() < 17 || line[16] != ' ')
            return std::nullopt;
        const auto key = decodeHex<8> (line.substr (0, 16));
        const auto b64 = line.substr (17);
        if (! key || b64.empty() || b64.size() > maxBase64)
            return std::nullopt;

        auto der = decodeBase64 (b64);
        if (! der || der->size() < 8 || der->size() > 72)
            return std::nullopt;
        return Signature { *key, std::move (*der) };
    }

    bool isLowerHex (const std::string& s) noexcept
    {
        return std::all_of (s.begin(), s.end(), [] (char c) { return hexValue (c) >= 0; });
    }

    std::string lowered (std::string s)
    {
        for (auto& c : s)
            if (c >= 'A' && c <= 'Z')
                c = char (c - 'A' + 'a');
        return s;
    }
}

std::optional<Bytes> decodeBase64 (std::string_view s) noexcept
{
    auto value = [] (char c) -> int
    {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    if (s.empty() || s.size() % 4 != 0)
        return std::nullopt;

    std::size_t padding = 0;
    if (s.back() == '=') ++padding;
    if (s.size() >= 2 && s[s.size() - 2] == '=') ++padding;

    Bytes out;
    out.reserve (s.size() / 4 * 3);
    for (std::size_t i = 0; i < s.size(); i += 4)
    {
        const bool last = i + 4 == s.size();
        std::uint32_t chunk = 0;
        for (std::size_t k = 0; k < 4; ++k)
        {
            const char c = s[i + k];
            int v = 0;
            if (c == '=')
            {
                if (! last || k < 4 - padding)
                    return std::nullopt;
            }
            else if ((v = value (c)) < 0)
                return std::nullopt;
            chunk = (chunk << 6) | std::uint32_t (v);
        }
        out.push_back (std::uint8_t (chunk >> 16));
        if (! last || padding < 2) out.push_back (std::uint8_t (chunk >> 8));
        if (! last || padding < 1) out.push_back (std::uint8_t (chunk));
    }
    return out;
}

std::optional<OsVersion> OsVersion::parse (std::string_view s) noexcept
{
    int parts[3] = { 0, 0, 0 };
    int count = 0, digits = 0;
    for (char c : s)
    {
        if (c == '.')
        {
            if (digits == 0 || ++count > 2)
                return std::nullopt;
            digits = 0;
        }
        else if (c >= '0' && c <= '9' && digits < 3)
        {
            parts[count] = parts[count] * 10 + (c - '0');
            ++digits;
        }
        else
            return std::nullopt;
    }
    if (digits == 0)
        return std::nullopt;
    return OsVersion { parts[0], parts[1] };
}

// ---------------------------------------------------------------------------------

Result<SignedEnvelope, EnvelopeError> SignedEnvelope::split (ByteView file)
{
    if (file.size() > maxEnvelopeBytes)
        return EnvelopeError::tooLarge;

    const auto text = asText (file);
    std::size_t pos = 0;

    auto nextLine = [&] () -> std::optional<std::string_view>
    {
        const auto end = text.find ('\n', pos);
        if (end == std::string_view::npos)
            return std::nullopt;
        const auto line = text.substr (pos, end - pos);
        pos = end + 1;
        return line;
    };

    if (nextLine() != envelopeMagic)
        return EnvelopeError::badMagic;

    std::vector<Signature> sigs;
    for (;;)
    {
        const auto line = nextLine();
        if (! line)
            return EnvelopeError::noTerminator;
        if (*line == ".")
            break;
        if (sigs.size() == maxSignatureLines)
            return EnvelopeError::tooManySignatures;
        auto sig = parseSignatureLine (*line);
        if (! sig)
            return EnvelopeError::malformedLine;
        sigs.push_back (std::move (*sig));
    }

    if (sigs.empty())
        return EnvelopeError::noSignature;
    if (file.size() - pos > maxPayloadBytes)
        return EnvelopeError::tooLarge;

    return SignedEnvelope (std::move (sigs), Bytes (file.begin() + std::ptrdiff_t (pos), file.end()));
}

Bytes SignedEnvelope::signedMessage() const
{
    Bytes message (envelopeMagic.begin(), envelopeMagic.end());
    message.push_back ('\n');
    message.insert (message.end(), body.begin(), body.end());
    return message;
}

Result<Manifest, ManifestError> parseManifest (const VerifiedPayload& payload)
{
    using nlohmann::json;
    const auto bytes = payload.bytes();
    const auto doc = json::parse (bytes.begin(), bytes.end(), nullptr, false);
    if (doc.is_discarded() || ! doc.is_object())
        return ManifestError::notJson;

    auto stringField = [] (const json& obj, const char* key) -> const std::string*
    {
        const auto it = obj.find (key);
        return it != obj.end() && it->is_string() ? it->get_ptr<const std::string*>() : nullptr;
    };

    const auto* product = stringField (doc, "product");
    const auto* versionText = stringField (doc, "version");
    const auto* macText = stringField (doc, "minimumMacOS");
    const auto archiveIt = doc.find ("archive");
    if (! product || ! versionText || ! macText || archiveIt == doc.end() || ! archiveIt->is_object())
        return ManifestError::missingField;

    const auto version = Version::parse (*versionText);
    if (! version)
        return ManifestError::badVersion;
    const auto minOs = OsVersion::parse (*macText);
    if (! minOs)
        return ManifestError::badMacOS;

    const auto* hashText = stringField (*archiveIt, "sha256");
    const auto sizeIt = archiveIt->find ("size");
    if (! hashText || sizeIt == archiveIt->end())
        return ManifestError::missingField;

    const auto hash = isLowerHex (*hashText) ? decodeHex<32> (*hashText) : std::nullopt;
    if (! hash)
        return ManifestError::badHash;
    if (! sizeIt->is_number_unsigned() || sizeIt->get<std::uint64_t>() == 0)
        return ManifestError::badSize;

    return Manifest (*version, *product, *minOs, ArchiveSpec { sizeIt->get<std::uint64_t>(), *hash });
}

// ---------------------------------------------------------------------------------

Evaluation evaluate (const Manifest& m, const OfferContext& ctx)
{
    if (m.product() != pinnedBundleId)
        return NoOffer::wrongProduct;
    if (m.archive().size > ArchiveSpec::maxBytes)
        return NoOffer::oversized;
    if (ctx.highestSeen && m.version() < *ctx.highestSeen)
        return NoOffer::replayed;
    if (m.version() <= ctx.running)
        return NoOffer::upToDate;
    if (ctx.os < m.minimumMacOS())
        return NoOffer::needsNewerMacOS;
    return Offer (m.version(), m.archive());
}

bool supersedes (const Manifest& incoming, const std::optional<Version>& cached) noexcept
{
    return ! cached || incoming.version() >= *cached;
}

// ---------------------------------------------------------------------------------

Url envelopeUrl()
{
    return Url (std::string (repoBase) + "latest/download/update-v1.signed");
}

Url archiveUrl (const Offer& offer)
{
    const auto v = offer.version().toString();
    return Url (std::string (repoBase) + "download/v" + v + "/tubamp-" + v + "-macos-universal.zip");
}

Url releasePageUrl (Version version)
{
    return Url (std::string (repoBase) + "tag/v" + version.toString());
}

std::optional<Url> repositoryUrl (std::string_view path)
{
    if (path.empty() || path.front() == '/' || path.back() == '/' || path.find ("//") != std::string_view::npos)
        return std::nullopt;
    for (const char c : path)
        if (! ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/'))
            return std::nullopt;
    for (std::size_t start = 0; start <= path.size();)
    {
        const auto end = std::min (path.find ('/', start), path.size());
        const auto segment = path.substr (start, end - start);
        if (segment == "." || segment == "..")
            return std::nullopt;
        start = end + 1;
    }
    return Url (std::string (repoRoot) + std::string (path));
}

bool hostAllowed (std::string_view url) noexcept
{
    constexpr std::string_view scheme = "https://";
    if (! url.starts_with (scheme))
        return false;
    url.remove_prefix (scheme.size());
    const auto host = url.substr (0, url.find_first_of ("/?#"));
    return std::find (std::begin (allowedHosts), std::end (allowedHosts), host) != std::end (allowedHosts);
}

// ---------------------------------------------------------------------------------

Result<std::vector<PlannedEntry>, ArchiveViolation> planExtraction (std::span<const ArchiveEntry> entries)
{
    static const std::vector<std::string> executable { std::string (bundleRoot), "Contents", "MacOS", "tubamp" };

    if (entries.size() > maxArchiveEntries)
        return ArchiveViolation::tooManyEntries;

    std::vector<PlannedEntry> plan;
    std::map<std::string, EntryKind> byKey;   // lowercased path -> kind
    std::uint64_t total = 0;
    bool hasExecutable = false;

    for (const auto& entry : entries)
    {
        std::string_view path = entry.path;
        if (path.empty())
            return ArchiveViolation::emptyComponent;
        for (const char ch : path)
            if (const auto c = static_cast<unsigned char> (ch); c < 0x20 || c >= 0x7f || c == '\\')
                return ArchiveViolation::badCharacter;
        if (path.front() == '/')
            return ArchiveViolation::absolutePath;
        if (entry.kind == EntryKind::directory && path.back() == '/')
            path.remove_suffix (1);

        std::vector<std::string> components;
        for (std::size_t start = 0;;)
        {
            const auto end = path.find ('/', start);
            const auto part = path.substr (start, end == std::string_view::npos ? std::string_view::npos : end - start);
            if (part.empty())
                return ArchiveViolation::emptyComponent;
            if (part == "." || part == "..")
                return ArchiveViolation::dotSegment;
            components.emplace_back (part);
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }

        if (components.front() != bundleRoot || (components.size() == 1 && entry.kind != EntryKind::directory))
            return ArchiveViolation::outsideBundle;
        if (entry.kind == EntryKind::symlink)
            return ArchiveViolation::symlink;
        if (entry.kind == EntryKind::other)
            return ArchiveViolation::specialFile;

        std::string key;
        for (const auto& c : components)
            key += (key.empty() ? "" : "/") + lowered (c);
        if (! byKey.emplace (key, entry.kind).second)
            return ArchiveViolation::duplicate;

        const bool isFile = entry.kind == EntryKind::file;
        if (isFile)
        {
            if (entry.uncompressedSize > maxUnpackedBytes - total)
                return ArchiveViolation::tooLarge;
            total += entry.uncompressedSize;
        }

        const bool isExecutable = isFile && components == executable;
        hasExecutable = hasExecutable || isExecutable;
        plan.push_back ({ std::move (components), entry.kind, isFile ? entry.uncompressedSize : 0,
                          (! isFile || isExecutable) ? 0755u : 0644u });
    }

    for (const auto& [key, kind] : byKey)
        for (auto slash = key.find ('/'); slash != std::string::npos; slash = key.find ('/', slash + 1))
            if (const auto parent = byKey.find (key.substr (0, slash)); parent != byKey.end() && parent->second == EntryKind::file)
                return ArchiveViolation::conflict;

    if (! hasExecutable)
        return ArchiveViolation::missingExecutable;
    return plan;
}
} // namespace tubamp::update::detail
