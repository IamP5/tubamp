#!/usr/bin/env python3
"""Acceptance for the phaser engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.6.6). Prints a PASS/FAIL table; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_phaser.py [--quick] [--types 1,2]

Skeleton from the scaffold: the phaser engineer fills in specific_checks().
"""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxcheck as fx  # noqa: E402

EFFECT = "phaser"
TYPE_KEY = "model"
TYPES = {0: "Classic", 1: "Phase 90", 2: "Small Stone"}
# Float keys at their documented limits, for the finite-output test.
RANGES = {"rate": (0.01, 20.0), "depth": (0.0, 1.0), "mix": (0.0, 1.0)}
# Percent of real time per instance (§2.10); None = Classic as today (<= 1.2 x legacy).
BUDGETS = {0: None, 1: 0.4, 2: 0.5}
VARIANTS = {1: [{"color": 0}, {"color": 1}], 2: [{"color": 0}, {"color": 1}]}
NULL_KEYS = [{}, {"rate": 10, "depth": 1, "mix": 1}, {"rate": 0.05, "depth": 0.1, "mix": 0.5}]


def specific_checks(report, args):
    report.todo("P2-P8", "the effect's own acceptance table (docs/FX-ENGINES.md §4.6.6)")


def main():
    args = fx.parse_args(__doc__)
    report = fx.Report(EFFECT)
    fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                     quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
    specific_checks(report, args)
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
