#!/usr/bin/env python3
"""reftakes.py: Guitar Rig 6 reference takes for the motion rows of tools/fxbench/check_phaser.py.

    export AUHOST=... GR6_STATES=...      (gr6.py)
    python3 reftakes.py                   # writes gr6_takes/*.wav and motion.tsv (this directory)

Null takes: refmeasure.guitar(8 s, seed 7, -6 dBFS peak) through Guitar Rig 6 at 48 kHz, stored as
16-bit mono WAV in gr6_takes/<tag>.wav (the check renders ours over the same take and nulls it).
Motion: a held 196 Hz sine at -12 dBFS, 10 s, through Guitar Rig 6; refmeasure.cents() of the
output (p99 and rms of the pitch deviation in cents) -> motion.tsv. Knob positions: Phaser Nine
Rate (block) and Stoned Phaser Rate, the study's settings otherwise (voicings.md).
"""
import os

import numpy as np

import gr6
import refmeasure as R

HERE = os.path.dirname(os.path.abspath(__file__))
TAKES = os.path.join(HERE, "gr6_takes")
SR = 48000
NULL_SECS = 8.0
MOTION_SECS = 10.0
MOTION_HZ = 196.0
# tag: component, overrides, voicing (model, color)
NULL = {
    "p9_block_p050": ("p9", dict(p=0.5), (1, 1)),
    "p9_block_p090": ("p9", dict(p=0.9), (1, 1)),
    "ss_off_r050": ("ss", dict(Rt=0.5), (2, 0)),
    "ss_off_r075": ("ss", dict(Rt=0.75), (2, 0)),
    "ss_on_r050": ("ss", dict(Rt=0.5, Cl=1), (2, 1)),
    "ss_on_r075": ("ss", dict(Rt=0.75, Cl=1), (2, 1)),
}
MOTION = {
    "p9_block_p025": ("p9", dict(p=0.25), (1, 1)),
    "p9_block_p050": ("p9", dict(p=0.5), (1, 1)),
    "p9_block_p090": ("p9", dict(p=0.9), (1, 1)),
    "ss_off_r050": ("ss", dict(Rt=0.5), (2, 0)),
    "ss_off_r075": ("ss", dict(Rt=0.75), (2, 0)),
    "ss_off_r090": ("ss", dict(Rt=0.9), (2, 0)),
    "ss_on_r050": ("ss", dict(Rt=0.5, Cl=1), (2, 1)),
    "ss_on_r075": ("ss", dict(Rt=0.75, Cl=1), (2, 1)),
    "ss_on_r090": ("ss", dict(Rt=0.9, Cl=1), (2, 1)),
}


def rate_of(comp, over):
    return gr6.p9_rate(over["p"]) if comp == "p9" else gr6.ss_rate(over["Rt"])


def read_motion(path=os.path.join(HERE, "motion.tsv")):
    rows = {}
    for line in open(path):
        if line.startswith("#") or line.startswith("tag\t"):
            continue
        c = line.rstrip("\n").split("\t")
        rows[c[0]] = dict(model=int(c[1]), color=int(c[2]), rate=float(c[3]), note=float(c[4]), p99=float(c[5]), rms=float(c[6]))
    return rows


def main():
    os.makedirs(TAKES, exist_ok=True)
    gtr = R.guitar(int(NULL_SECS * SR), SR, seed=7)
    for tag, (comp, over, _) in NULL.items():
        y = gr6.render([gtr], gr6.state(comp, **over))[0]
        gr6.write_wav(os.path.join(TAKES, f"{tag}.wav"), y, SR, bits=16)
        print(f"{tag}: {len(y)} samples, peak {20 * np.log10(np.max(np.abs(y))):.2f} dBFS")
    x = R.sine(MOTION_HZ, int(MOTION_SECS * SR), SR, -12.0)
    with open(os.path.join(HERE, "motion.tsv"), "w") as out:
        out.write("# reftakes.py: pitch deviation of a held note through Guitar Rig 6, 48 kHz (refmeasure.cents()).\n")
        out.write("tag\tmodel\tcolor\trate_hz\tnote_hz\tp99_cents\trms_cents\n")
        for tag, (comp, over, (model, color)) in MOTION.items():
            y = gr6.render([x], gr6.state(comp, **over))[0]
            p99, rms = R.cents(y, MOTION_HZ, SR)
            out.write(f"{tag}\t{model}\t{color}\t{rate_of(comp, over):.6f}\t{MOTION_HZ}\t{p99:.3f}\t{rms:.3f}\n")
            print(f"{tag}: p99 {p99:.2f} cents, rms {rms:.2f}")


if __name__ == "__main__":
    main()
