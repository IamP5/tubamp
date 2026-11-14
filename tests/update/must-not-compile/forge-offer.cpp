// expect: private constructor
// Rule: an Offer exists only when evaluate found the release newer, not replayed, pinned and supported.
#include "detail/Rules.h"
using namespace tubamp::update;
using namespace tubamp::update::detail;

void f (const Manifest& m, const OfferContext& ctx)
{
#ifdef VIOLATE
    const Offer forged (Version { 9, 0, 0 }, ArchiveSpec {});
    (void) archiveUrl (forged);
#else
    const auto evaluation = evaluate (m, ctx);
    if (const auto* offer = std::get_if<Offer> (&evaluation))
        (void) archiveUrl (*offer);
#endif
}
