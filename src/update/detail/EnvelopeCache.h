#pragma once

// PRIVATE to src/update. latest.signed: the newest envelope this account accepted, byte
// for byte. It is the offer cache and the replay floor. Implemented in EnvelopeCache.cpp.

#include "Prefs.h"
#include "Trust.h"

namespace tubamp::update::detail
{
/** Read the file and open it with openEnvelope, exactly as a fresh download. A missing,
    damaged or untrusted file is "nothing cached". */
std::optional<Manifest> loadCachedEnvelope (const SupportPaths&, const TrustRoot&);

/** Open `file` with openEnvelope and, if it opens and supersedes() the cached version,
    replace latest.signed (temporary file and rename, under the lock). Unverified bytes
    are never written, and the cached version never goes down. */
void storeEnvelope (const SupportPaths&, ByteView file, const TrustRoot&);
} // namespace tubamp::update::detail
