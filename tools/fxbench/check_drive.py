#!/usr/bin/env python3
"""Acceptance for the drive engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.3.9). Prints a PASS/FAIL table; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_drive.py [--quick] [--types 1,2]

Skeleton from the scaffold: the drive engineer fills in specific_checks().
"""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxcheck as fx  # noqa: E402

EFFECT = "drive"
TYPE_KEY = "type"
TYPES = {0: "Classic", 1: "Screamer", 2: "Distortion", 3: "Fuzz", 4: "Boost", 5: "Treble"}
# Float keys at their documented limits, for the finite-output test.
RANGES = {"gain": (0.0, 36.0), "tone": (500.0, 12000.0), "level": (-24.0, 12.0)}
# Percent of real time per instance (§2.10); None = Classic as today (<= 1.2 x legacy).
BUDGETS = {0: 0.35, 1: 2.0, 2: 2.0, 3: 3.0, 4: 0.05, 5: 0.05}
VARIANTS = {}
NULL_KEYS = [{}, {"gain": 36, "tone": 12000, "level": -6}, {"gain": 0, "tone": 500}]


def specific_checks(report, args):
    ok, detail = fx.classic_null(EFFECT, ["gain@1=30", "tone@2=800", "level@3=-6", "on@4=0", "on@5=1"])
    report.add("D1 steps", "Classic", ok, detail)
    report.todo("D2-D18", "the effect's own acceptance table (docs/FX-ENGINES.md §4.3.9)")


def main():
    args = fx.parse_args(__doc__)
    report = fx.Report(EFFECT)
    fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                     quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
    specific_checks(report, args)
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
