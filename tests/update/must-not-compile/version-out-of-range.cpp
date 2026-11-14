// expect: narrow
// Rule: a Version cannot hold a minor above 255, so its order is the AudioComponents order.
#include "UpdateTypes.h"
using namespace tubamp::update;

void f()
{
#ifdef VIOLATE
    const Version v { 1, 256, 0 };
#else
    const Version v { 1, 255, 0 };
#endif
    (void) v;
}
