#!/usr/bin/env python3
"""Proves the reverb and delay engines did not change: renders a fixed matrix through
tubamp_fxrender and compares a hash of every render with a stored baseline.

    reverb_delay_guard.py <tubamp_fxrender> --write tests/golden/reverb_delay.json
    reverb_delay_guard.py <tubamp_fxrender> --check tests/golden/reverb_delay.json

The baseline was written with the binary built from main at 221a3cb, before the effect
engines of docs/FX-ENGINES.md were added. A mismatch means Reverb.*, Delay.* or
FxPrimitives.h changed behaviour.
"""
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

RATES = [44100, 48000, 96000]
REVERB = [dict(type=t, stim=s, **extra)
          for t in range(6)
          for s, extra in [("impulse", {}), ("noise:200", dict(mix=0.4, size=0.8, mod=0.6))]]
DELAY = [dict(type=t, mode=m, stim=s, **extra)
         for t in range(6)
         for m in range(3)
         for s, extra in [("impulse", {}), ("burst:440:120", dict(feedback=0.6, mod=0.5, color=0.7, sync=9, bpm=97))]]


def cases():
    for sr in RATES:
        for engine, table in (("reverb", REVERB), ("delay", DELAY)):
            for params in table:
                yield engine, dict(params, sr=sr, seconds=3, block=256)


def render_hash(binary, engine, params, workdir):
    out = Path(workdir) / "take.wav"
    args = [binary, engine, str(out)] + [f"{k}={v}" for k, v in params.items()]
    done = subprocess.run(args, capture_output=True, text=True)
    if done.returncode != 0 or not out.exists():
        return f"FAILED: {done.stderr.strip()[:200]}"
    digest = hashlib.sha256(out.read_bytes()).hexdigest()
    out.unlink()
    return digest


def main():
    if len(sys.argv) != 4 or sys.argv[2] not in ("--write", "--check"):
        sys.exit(__doc__)
    binary, mode, store = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    results = {}
    with tempfile.TemporaryDirectory() as workdir:
        for engine, params in cases():
            key = engine + " " + " ".join(f"{k}={v}" for k, v in sorted(params.items()))
            results[key] = render_hash(binary, engine, params, workdir)
    failed = [k for k, v in results.items() if v.startswith("FAILED")]
    if mode == "--write":
        if failed:
            sys.exit(f"{len(failed)} renders failed, baseline not written; first: {failed[0]}: {results[failed[0]]}")
        store.write_text(json.dumps(results, indent=1, sort_keys=True) + "\n")
        print(f"wrote {len(results)} hashes to {store}")
        return
    baseline = json.loads(store.read_text())
    changed = [k for k in baseline if results.get(k) != baseline[k]]
    missing = [k for k in results if k not in baseline]
    for key in changed[:20]:
        print(f"CHANGED  {key}")
    for key in missing[:5]:
        print(f"NEW CASE {key}")
    if changed or missing:
        sys.exit(f"FAIL: {len(changed)} of {len(baseline)} renders changed, {len(missing)} cases not in the baseline")
    print(f"PASS: {len(baseline)} reverb and delay renders are bit-identical to the baseline")


if __name__ == "__main__":
    main()
