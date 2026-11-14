// expect: no matching function
// Rule: only a bundle whose plist, slices and seal were inspected is swapped in.
#include "detail/Bundle.h"
#include <utility>
using namespace tubamp::update::detail;

void f (UnpackedBundle& unpacked, InspectedBundle& inspected, const Staging& staging, const SelfUpdatable& site)
{
#ifdef VIOLATE
    (void) inspected;
    (void) commit (std::move (unpacked), staging, site);
#else
    (void) unpacked;
    (void) commit (std::move (inspected), staging, site);
#endif
}
