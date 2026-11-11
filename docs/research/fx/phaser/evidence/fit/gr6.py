#!/usr/bin/env python3
"""gr6.py: black-box Guitar Rig 6 renders for snapshots.py and reftakes.py (numpy only).

Environment: AUHOST = the auhost binary (tools/re/build.sh), GR6_STATES = a directory with
  default.aupreset   auhost state aufx 'Ni$R' -NI- default.aupreset
  p9_doc0.xml        tools/re/ngrc_xml.py on the Phaser Nine factory .ngrc, document 0 (INIT)
  ss_doc0.xml        the same for Stoned Phaser
States are authored with tools/re/gr6state.py, the study's settings (voicings.md, "What the
reference is") plus overrides, and Rack.G=0: the Rack's noise gate is on in the default state
and attenuates sparse impulse probes (round-2 verification). Renders are stereo in, channel 0 out.
"""
import os
import struct
import subprocess
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", "..", "..", ".."))
P9_BASE = dict(n=4, c=0.4217, r=0.5, e=0.5, p=0.5, x=0.5, o=0.5, i=0, s=0)
SS_BASE = dict(Sy=0, Not=2, Rt=0.5, Cl=0, CS=0.74, DW=0.5, F1=0.5, F2=0.5, Rot=0, In=0)


def p9_rate(knob):
    """Phaser Nine Rate knob -> Hz: the period is piecewise linear in the knob (voicings.md, Knob curve)."""
    pts = [(0.0, 30.0), (0.1, 7.0), (0.4, 1.1), (1.0, 0.1025)]
    for (a, pa), (b, pb) in zip(pts, pts[1:]):
        if a <= knob <= b:
            return 1.0 / (pa + (pb - pa) * (knob - a) / (b - a))
    raise ValueError(knob)


def ss_rate(knob):
    return 0.01 * 1000.0 ** knob


def state(comp, **over):
    d = os.environ["GR6_STATES"]
    base = dict(P9_BASE if comp == "p9" else SS_BASE); base.update(over)
    tag = comp + "_" + "_".join(f"{k}{v}" for k, v in sorted(base.items()))
    out = os.path.join(d, f"st_{tag}.aupreset")
    if not os.path.exists(out):
        name = "Phaser Nine" if comp == "p9" else "Stoned Phaser"
        subprocess.run(["python3", os.path.join(REPO, "tools", "re", "gr6state.py"), os.path.join(d, "default.aupreset"),
                        os.path.join(d, f"{comp}_doc0.xml"), name, out] + [f"{k}={v}" for k, v in base.items()] + ["Rack.G=0"],
                       check=True)
    return out


def write_wav(path, x, sr, bits=32):
    x = np.atleast_2d(np.asarray(x, float))
    if x.shape[0] < x.shape[1]:
        x = x.T
    frames, ch = x.shape
    if bits == 16:
        body = np.clip(np.round(x * 32767.0), -32768, 32767).astype("<i2").tobytes(); code = 1
    else:
        body = x.astype("<f4").tobytes(); code = 3
    b = bits // 8
    open(path, "wb").write(b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt "
                           + struct.pack("<IHHIIHH", 16, code, ch, int(sr), int(sr) * ch * b, ch * b, bits)
                           + b"data" + struct.pack("<I", len(body)) + body)


def read_wav(path):
    raw = open(path, "rb").read(); pos, fmt, data = 12, None, None
    while pos + 8 <= len(raw):
        tag, size = raw[pos:pos + 4], struct.unpack("<I", raw[pos + 4:pos + 8])[0]
        if tag == b"fmt ":
            fmt = struct.unpack("<HHIIHH", raw[pos + 8:pos + 24])
        elif tag == b"data":
            data = raw[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    code, ch, sr, _, _, bits = fmt
    x = np.frombuffer(data, "<f4").astype(np.float64) if bits == 32 else np.frombuffer(data, "<i2") / 32767.0
    return x.reshape(-1, ch).T, sr


def render(xs, st, sr=48000, parallel=4):
    """Mono inputs -> channel-0 outputs, same length. Parallel instances now and then return NaN
    blocks: such a render is repeated serially."""
    d = tempfile.mkdtemp(); outs = []
    try:
        for j, x in enumerate(xs):
            write_wav(os.path.join(d, f"in_{j}.wav"), np.stack([x, x]), sr)
        cmd = lambda j: [os.environ["AUHOST"], "render", "aufx", "Ni$R", "-NI-", os.path.join(d, f"in_{j}.wav"),  # noqa: E731
                         os.path.join(d, f"out_{j}.wav"), "--state", st]
        for a in range(0, len(xs), parallel):
            ps = [subprocess.Popen(cmd(j), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                  for j in range(a, min(len(xs), a + parallel))]
            for p in ps:
                p.wait()
        for j, x in enumerate(xs):
            for _ in range(4):
                p = os.path.join(d, f"out_{j}.wav")
                if os.path.exists(p):
                    y = read_wav(p)[0][0]
                    if len(y) >= len(x) and np.all(np.isfinite(y)):
                        break
                subprocess.run(cmd(j), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            else:
                raise SystemExit(f"Guitar Rig 6 render failed: {st}")
            outs.append(y[:len(x)])
    finally:
        for fn in os.listdir(d):
            os.remove(os.path.join(d, fn))
        os.rmdir(d)
    return outs
