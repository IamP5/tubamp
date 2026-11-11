#!/usr/bin/env python3
"""Acceptance for the gate engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.1.10). Prints a PASS/FAIL table; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_gate.py [--quick] [--types 1,2]

Skeleton from the scaffold: the gate engineer fills in specific_checks().
"""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxcheck as fx  # noqa: E402

EFFECT = "gate"
TYPE_KEY = "mode"
TYPES = {0: "Classic", 1: "Tight", 2: "Smooth"}
# Float keys at their documented limits, for the finite-output test.
RANGES = {"threshold": (-100.0, 0.0), "release": (5.0, 500.0), "hold": (0.0, 250.0), "range": (-90.0, 0.0),
          "sc_hpf": (20.0, 300.0)}
# Percent of real time per instance (§2.10); None = Classic as today (<= 1.2 x legacy).
BUDGETS = {0: 0.25, 1: 0.10, 2: 0.10}
VARIANTS = {}
NULL_KEYS = None


def specific_checks(report, args):
    ok, detail = fx.classic_null(EFFECT, {"deferred": 1, "threshold": -40})
    report.add("G1 deferred null", "Classic", ok, detail)
    report.todo("G2-G18", "the effect's own acceptance table (docs/FX-ENGINES.md §4.1.10)")


def main():
    args = fx.parse_args(__doc__)
    report = fx.Report(EFFECT)
    fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                     quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
    specific_checks(report, args)
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
