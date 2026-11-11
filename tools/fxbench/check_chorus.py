#!/usr/bin/env python3
"""Acceptance for the chorus engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.5.7). Prints a PASS/FAIL table; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_chorus.py [--quick] [--types 1,2]

Skeleton from the scaffold: the chorus engineer fills in specific_checks().
"""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxcheck as fx  # noqa: E402

EFFECT = "chorus"
TYPE_KEY = "mode"
TYPES = {0: "Classic", 1: "Wide", 2: "CE", 3: "Dimension", 4: "Tri"}
# Float keys at their documented limits, for the finite-output test.
RANGES = {"rate": (0.01, 20.0), "depth": (0.0, 1.0), "mix": (0.0, 1.0), "width": (0.0, 1.0), "tone": (0.0, 1.0)}
# Percent of real time per instance (§2.10); None = Classic as today (<= 1.2 x legacy).
BUDGETS = {0: None, 1: 0.5, 2: 0.25, 3: 0.25, 4: 0.4}
VARIANTS = {}
NULL_KEYS = [{}, {"rate": 10, "depth": 1, "mix": 1}, {"rate": 0.05, "depth": 0.1, "mix": 0.5}]


def specific_checks(report, args):
    report.todo("CH2-CH14", "the effect's own acceptance table (docs/FX-ENGINES.md §4.5.7)")


def main():
    args = fx.parse_args(__doc__)
    report = fx.Report(EFFECT)
    fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                     quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
    specific_checks(report, args)
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
