#include "EnvelopeCache.h"

#include "Files.h"

namespace tubamp::update::detail
{
namespace
{
    fs::path cacheFile (const SupportPaths& support) { return support.dir / "latest.signed"; }
}

std::optional<Manifest> loadCachedEnvelope (const SupportPaths& support, const TrustRoot& trust)
{
    const auto bytes = readFile (cacheFile (support), maxEnvelopeBytes);
    if (! bytes)
        return std::nullopt;
    auto opened = openEnvelope (*bytes, trust);
    if (! opened)
        return std::nullopt;
    return std::move (opened).value();
}

void storeEnvelope (const SupportPaths& support, ByteView file, const TrustRoot& trust)
{
    const auto incoming = openEnvelope (file, trust);
    if (! incoming)
        return;
    const auto lock = SupportLock::acquire (support);
    if (! lock)
        return;
    const auto cached = loadCachedEnvelope (support, trust);
    if (supersedes (incoming.value(), cached ? std::optional<Version> (cached->version()) : std::nullopt))
        replaceFile (cacheFile (support), file);
}
} // namespace tubamp::update::detail
