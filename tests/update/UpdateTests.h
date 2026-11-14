#pragma once

#include "detail/Rules.h"

namespace tubamp::update::test
{
/** Runs every table, prints failures and a summary, returns the number of failures. */
int runUpdateTests();

/** The state machine tables (MachineTests.cpp), counted into the caller's totals. */
void runMachineTests (int& passed, int& failed);

/** The disk tables (DiskTests.cpp): prefs, cache, site, install chain, reconcile, crashes. */
void runDiskTests (int& passed, int& failed);

/** `tubamp_update_tool child <command> ...`: the processes the disk tests race and kill. */
int runDiskChild (int argc, char** argv);

/** An Offer made the only way one can be: a signed payload, opened and evaluated. */
std::optional<detail::Offer> signedOffer (std::string_view version, std::string_view running);
}
