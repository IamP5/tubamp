#!/usr/bin/env python3
"""Render audition files for both phaser voicings and print their levels.

    audition.py [outdir] [--copy repo_dir]   default outdir: $RE_STATE/work/phaser/audition

Writes, for each of phase90-script, phase90-block, smallstone, smallstone-color:
    <name>_noise.wav   10 s of white noise at -22 dBFS rms through the voicing
    <name>_riff.wav    a synthesised plucked-string riff (7.6 s) through the voicing
plus dry_noise.wav and dry_riff.wav: 48 kHz mono float. Rates: Phase 90 1.071 Hz and
Small Stone 0.3162 Hz (the Guitar Rig 6 default-knob rates in evidence/fit/targets.tsv).
Every file is checked to peak at or below -1 dBFS; a file above that is scaled down and
flagged. Peaks and rms are printed. The notch trajectories against Guitar Rig 6 are
printed by evidence/fit/compare.py, not here.
"""
import os
import shutil
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import phaser_ref as P   # noqa: E402

FS = 48000

RATES = {"phase90-script": 1.071, "phase90-block": 1.071, "smallstone": 0.3162, "smallstone-color": 0.3162}
CEIL_DB = -1.0
VARIANTS = {
    "phase90-script": dict(voicing="phase90", block=False),
    "phase90-block": dict(voicing="phase90", block=True),
    "smallstone": dict(voicing="smallstone", color=False),
    "smallstone-color": dict(voicing="smallstone", color=True),
}


def pluck(freq, secs, rng, damp=0.996):
    """Karplus-Strong string: noise burst into a delay line with a two-point average."""
    n = int(FS / freq)
    buf = rng.uniform(-1, 1, n)
    out = np.empty(int(secs * FS))
    for i in range(len(out)):
        out[i] = buf[i % n]
        buf[i % n] = damp * 0.5 * (buf[i % n] + buf[(i + 1) % n])
    return out


def riff():
    rng = np.random.default_rng(11)
    notes = [82.41, 123.47, 164.81, 196.0, 246.94, 196.0, 164.81, 123.47,     # Em arpeggio
             110.0, 164.81, 220.0, 261.63, 329.63, 261.63, 220.0, 164.81]     # Am arpeggio
    step = 0.32
    out = np.zeros(int((len(notes) * step + 2.5) * FS))
    for k, f in enumerate(notes * 1):
        s = pluck(f, 2.4, rng)
        a = int(k * step * FS)
        out[a:a + len(s)] += s
    chord = sum(pluck(f, 2.5, rng) for f in (82.41, 123.47, 164.81, 196.0, 246.94, 329.63))
    a = int(len(notes) * step * FS)
    out[a:a + len(chord)] += chord[:len(out) - a]
    return 0.5 * out / np.max(np.abs(out))       # dry peak -6 dBFS


def save(path, y):
    pk = 20 * np.log10(np.max(np.abs(y)) + 1e-12); flag = ""
    if pk > CEIL_DB:
        y = y * 10 ** ((CEIL_DB - 0.1 - pk) / 20); flag = f"  (scaled from {pk:+.2f} dBFS)"
        pk = 20 * np.log10(np.max(np.abs(y)))
    P.write_wav(path, y, FS)
    rms = 10 * np.log10(np.mean(y ** 2) + 1e-20)
    print(f"{os.path.basename(path)}\t{len(y)/FS:.2f} s\tpeak {pk:+.2f} dBFS\trms {rms:+.2f} dBFS{flag}")


def main():
    args = sys.argv[1:]; copy = None
    if "--copy" in args:
        i = args.index("--copy"); copy = args[i + 1]; del args[i:i + 2]
    default = os.path.join(os.environ.get("RE_STATE", "."), "work", "phaser", "audition")
    out = args[0] if args else default
    os.makedirs(out, exist_ok=True)
    rng = np.random.default_rng(5)
    noise = rng.standard_normal(10 * FS)
    noise *= 10 ** (-22 / 20) / np.sqrt(np.mean(noise ** 2))
    gtr = riff()
    print("# file\tlength\tpeak\trms")
    save(os.path.join(out, "dry_noise.wav"), noise)
    save(os.path.join(out, "dry_riff.wav"), gtr)
    for name, kw in VARIANTS.items():
        save(os.path.join(out, name + "_noise.wav"), P.render(noise, FS, rate=RATES[name], **kw))
        save(os.path.join(out, name + "_riff.wav"), P.render(gtr, FS, rate=RATES[name], **kw))
    if copy:
        os.makedirs(copy, exist_ok=True)
        for f in sorted(os.listdir(out)):
            if f.endswith(".wav") and (f.startswith("dry_") or any(f.startswith(v + "_") for v in VARIANTS)):
                shutil.copy2(os.path.join(out, f), os.path.join(copy, f))
        print(f"# copied to {copy}", file=sys.stderr)
    print(f"# audition files in {out}", file=sys.stderr)


if __name__ == "__main__":
    main()
