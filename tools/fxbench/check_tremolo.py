#!/usr/bin/env python3
"""Acceptance for the tremolo engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.7.7). Prints a PASS/FAIL table; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_tremolo.py [--quick] [--types 1,2]

Skeleton from the scaffold: the tremolo engineer fills in specific_checks().
"""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxcheck as fx  # noqa: E402

EFFECT = "tremolo"
TYPE_KEY = "voice"
TYPES = {0: "Classic", 1: "Smooth", 2: "Optical", 3: "Bias", 4: "Harmonic"}
# Float keys at their documented limits, for the finite-output test.
RANGES = {"rate": (0.01, 20.0), "depth": (0.0, 1.0), "mix": (0.0, 1.0), "width": (0.0, 1.0), "tone": (0.0, 1.0),
          "shape": (0.0, 1.0), "wave": (0.0, 1.0)}
# Percent of real time per instance (§2.10); None = Classic as today (<= 1.2 x legacy).
BUDGETS = {0: None, 1: 0.10, 2: 0.10, 3: 0.10, 4: 0.15}
VARIANTS = {}
NULL_KEYS = [{}, {"rate": 10, "depth": 1, "mix": 1}, {"rate": 0.05, "depth": 0.1, "mix": 0.5}]


def specific_checks(report, args):
    report.todo("T2-T17", "the effect's own acceptance table (docs/FX-ENGINES.md §4.7.7)")


def main():
    args = fx.parse_args(__doc__)
    report = fx.Report(EFFECT)
    fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                     quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
    specific_checks(report, args)
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
