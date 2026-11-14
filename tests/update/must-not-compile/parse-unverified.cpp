// expect: private constructor
// Rule: the payload is parsed only after its signature verified.
#include "detail/Rules.h"
using namespace tubamp::update::detail;

void f (const VerifiedPayload& verified, Bytes untrusted)
{
#ifdef VIOLATE
    (void) parseManifest (VerifiedPayload (untrusted));
#else
    (void) untrusted;
    (void) parseManifest (verified);
#endif
}
