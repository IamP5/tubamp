// expect: no matching function
// Rule: only an archive whose size and hash matched the signed payload is unpacked.
#include "detail/Bundle.h"
#include <utility>
using namespace tubamp::update::detail;

void f (VerifiedArchive& archive, int rawDescriptor, const Staging& staging)
{
#ifdef VIOLATE
    (void) archive;
    (void) unpack (rawDescriptor, staging);
#else
    (void) rawDescriptor;
    (void) unpack (std::move (archive), staging);
#endif
}
