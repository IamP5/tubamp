#!/usr/bin/env python3
"""Acceptance for the eq engine: the common checks of docs/FX-ENGINES.md §2.11, then
the effect's own table (§4.4.9). Prints a PASS/FAIL table; exit status 1 on any FAIL.

    python3 -I tools/fxbench/check_eq.py [--quick] [--types 1,2]

Skeleton from the scaffold: the eq engineer fills in specific_checks().
"""
import sys
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxcheck as fx  # noqa: E402

EFFECT = "eq"
TYPE_KEY = "type"
TYPES = {0: "Classic", 1: "Parametric", 2: "Stack", 3: "Graphic"}
# Float keys at their documented limits, for the finite-output test.
RANGES = {"bass": (0.0, 10.0), "mid": (0.0, 10.0), "treble": (0.0, 10.0), "lowcut": (20.0, 400.0),
          "highcut": (2000.0, 20000.0), "mid_freq": (100.0, 8000.0), "mid_q": (0.5, 8.0), "tilt": (-6.0, 6.0),
          "level": (-12.0, 12.0), "g100": (-12.0, 12.0), "g200": (-12.0, 12.0), "g400": (-12.0, 12.0),
          "g800": (-12.0, 12.0), "g1k6": (-12.0, 12.0), "g3k2": (-12.0, 12.0), "g6k4": (-12.0, 12.0)}
# Percent of real time per instance (§2.10); None = Classic as today (<= 1.2 x legacy).
BUDGETS = {0: 0.1, 1: 0.30, 2: 0.15, 3: 0.30}
VARIANTS = {2: [{"voice": v} for v in range(4)]}
NULL_KEYS = [{"bass": 10, "mid": 0, "treble": 7}, {"bass": 0, "mid": 0, "treble": 0}, {"bass": 5, "mid": 5, "treble": 5.1}]


def specific_checks(report, args):
    # E2: Classic at exactly 5/5/5 is an exact pass-through (D2 a), so it is NOT nulled
    # against the legacy biquads; the output must equal the input bit for bit.
    ok, detail = True, ""
    for sr in (44100, 96000, 192000):
        noise = fx.stereo(fx.white(3.0, -12.0, sr)).astype(np.float32).astype(np.float64)
        out = fx.run(EFFECT, noise, {"type": 0, "bass": 5, "mid": 5, "treble": 5}, sr=sr)[0]
        differ = int(np.count_nonzero(out != noise))
        if differ:
            ok, detail = False, f"{sr} Hz: {differ} samples differ from the input"
    report.add("E2 exact flat", "Classic", ok, detail)
    report.todo("E3-E19", "the effect's own acceptance table (docs/FX-ENGINES.md §4.4.9)")


def main():
    args = fx.parse_args(__doc__)
    report = fx.Report(EFFECT)
    fx.common_checks(EFFECT, TYPES, ranges=RANGES, budgets=BUDGETS, variants=VARIANTS, report=report,
                     quick=args.quick, type_key=TYPE_KEY, null_keys=NULL_KEYS, only=args.types)
    specific_checks(report, args)
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
