// expect: private constructor
// Rule: a Manifest exists only as the parse of a verified payload.
#include "detail/Rules.h"
using namespace tubamp::update;
using namespace tubamp::update::detail;

void f (const Manifest& parsed, const OfferContext& ctx)
{
#ifdef VIOLATE
    const Manifest forged (Version { 9, 0, 0 }, "com.t0audio.tubamp", OsVersion { 11, 0 }, ArchiveSpec {});
    (void) evaluate (forged, ctx);
#else
    (void) evaluate (parsed, ctx);
#endif
}
