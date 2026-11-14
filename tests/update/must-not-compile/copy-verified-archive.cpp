// expect: deleted
// Rule: a verified archive is one descriptor, moved along the chain, never duplicated.
#include "detail/Bundle.h"
#include <utility>
using namespace tubamp::update::detail;

void f (VerifiedArchive& archive)
{
#ifdef VIOLATE
    VerifiedArchive copy = archive;
#else
    VerifiedArchive copy = std::move (archive);
#endif
    (void) copy;
}
