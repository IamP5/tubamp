#!/usr/bin/env python3
"""Acceptance for the comp engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.2.9). Prints a PASS/FAIL table; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_comp.py [--quick] [--types 1,2]

Skeleton from the scaffold: the comp engineer fills in specific_checks().
"""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxcheck as fx  # noqa: E402

EFFECT = "comp"
TYPE_KEY = "type"
TYPES = {0: "Classic", 1: "Clean", 2: "Squash", 3: "FET", 4: "Opto"}
# Float keys at their documented limits, for the finite-output test.
RANGES = {"threshold": (-60.0, 0.0), "ratio": (1.0, 20.0), "attack": (0.1, 100.0), "release": (10.0, 1000.0),
          "makeup": (0.0, 24.0), "knee": (0.0, 24.0), "mix": (0.0, 100.0), "sc_hpf": (20.0, 300.0)}
# Percent of real time per instance (§2.10); None = Classic as today (<= 1.2 x legacy).
BUDGETS = {0: 0.3, 1: 0.35, 2: 0.35, 3: 0.35, 4: 0.35}
VARIANTS = {}
NULL_KEYS = None


def specific_checks(report, args):
    # Makeup ramps and skip() sequences: a makeup step, a threshold step, off for a
    # second (skip), a makeup change while off, back on.
    ok, detail = fx.classic_null(EFFECT, ["makeup@1.5=12", "threshold@2.2=-35", "on@3=0", "makeup@3.5=3", "on@4=1",
                                          "ratio@0.5..2.5=2..12"])
    report.add("C1 ramps and skip", "Classic", ok, detail)
    report.todo("C2-C23", "the effect's own acceptance table (docs/FX-ENGINES.md §4.2.9)")


def main():
    args = fx.parse_args(__doc__)
    report = fx.Report(EFFECT)
    fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                     quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
    specific_checks(report, args)
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
