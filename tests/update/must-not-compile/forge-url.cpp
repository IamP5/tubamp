// expect: private constructor
// Rule: every URL is built from compiled-in constants; none comes from a string.
#include "detail/Rules.h"
using namespace tubamp::update::detail;

void f()
{
#ifdef VIOLATE
    const Url url ("https://evil.example/tubamp.zip");
#else
    const Url url = envelopeUrl();
#endif
    (void) url;
}
