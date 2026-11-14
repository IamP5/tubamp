// expect: private constructor
// Rule: only InstallSite::detect decides that this copy may replace itself.
#include "detail/Bundle.h"
using namespace tubamp::update::detail;

void f (const SelfUpdatable& detected)
{
#ifdef VIOLATE
    const SelfUpdatable forged (Layout { "/Library/Audio/Plug-Ins/Components", "/tmp", {} });
    (void) forged.layout();
#else
    (void) detected.layout();
#endif
}
