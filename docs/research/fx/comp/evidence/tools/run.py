#!/usr/bin/env python3
"""Run black-box compressor tests for one plugin configuration.

    run.py <name> <type,subtype,manu> <label> [--set id=v ...] [--tests curve,tbA,...]
           [--lo -40] [--hi -10] [--hi2 -20] [--on 0.5] [--post 3] [--lag N|auto]

Writes $W/ev/<name>/<label>.<test>.tsv; the first lines of each file are the commands.
"""
import argparse, os, subprocess, sys, shlex
W = os.environ["W"]; RE = os.environ["RE"]; AUH = os.environ["AUH"]
SIG = f"python3 {RE}/sig.py"; CB = f"python3 {W}/cb.py"

def sh(cmd):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    return r.stdout, r.stderr

def need(path, cmd):
    if not os.path.exists(path):
        sh(cmd)
    return cmd

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name"); ap.add_argument("codes", help="type,subtype,manu"); ap.add_argument("label")
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--tests", default="curve,tbA,tbB,thd")
    ap.add_argument("--lo", type=float, default=-40.0); ap.add_argument("--hi", type=float, default=-10.0)
    ap.add_argument("--hi2", type=float, default=-20.0)
    ap.add_argument("--on", type=float, default=0.5); ap.add_argument("--post", type=float, default=3.0)
    ap.add_argument("--long", type=float, default=5.0)
    ap.add_argument("--lag", default="auto")
    ap.add_argument("--state", default="")
    a = ap.parse_args()
    ev = f"{W}/ev/{a.name}"; wav = f"{W}/out/{a.name}"; os.makedirs(ev, exist_ok=True); os.makedirs(wav, exist_ok=True)
    sets = " ".join(f"--set {s}" for s in a.set)
    if a.state: sets += f" --state {shlex.quote(a.state)}"
    t_, s_, m_ = a.codes.split(","); codes = f"{t_} {shlex.quote(s_)} {m_}"
    def render(inp, tag):
        out = f"{wav}/{a.label}.{tag}.wav"
        cmd = f"{AUH} render {codes} {inp} {out} {sets}"
        o, e = sh(cmd)
        if not os.path.exists(out): print("RENDER FAILED", cmd, e[-300:], file=sys.stderr)
        return out, cmd
    # latency
    lag = 0
    if a.lag == "auto":
        g = need(f"{W}/sig/noise_m50.wav", f"{SIG} gen noise {W}/sig/noise_m50.wav --db -50 --secs 2")
        out, cmd = render(f"{W}/sig/noise_m50.wav", "lag")
        o, _ = sh(f"{CB} lag {W}/sig/noise_m50.wav {out}")
        try: lag = int(o.split("\t")[1])
        except Exception: lag = 0
    else:
        lag = int(a.lag)
    def write(test, cmds, body):
        with open(f"{ev}/{a.label}.{test}.tsv", "w") as fh:
            for c in cmds: fh.write("# " + c.replace(W, "$W").replace(RE, "$RE").replace(AUH, "$AUH") + "\n")
            fh.write(body)
    for test in a.tests.split(","):
        if test == "curve":
            inp = f"{W}/sig/steps1k.wav"
            g = need(inp, f"{SIG} gen steps {inp} --freq 1000 --levels -60:0:3 --hold 1.0")
            out, cmd = render(inp, test)
            an = f"{SIG} curve {inp} {out} --hold 1.0"
            write(test, [f"{SIG} gen steps {inp} --freq 1000 --levels -60:0:3 --hold 1.0", cmd, an], sh(an)[0])
        elif test.startswith("tb"):
            kind = test[2:]
            freq, lo, hi, on, post = 1000.0, a.lo, a.hi, a.on, a.post
            if kind == "B": hi = a.hi2
            if kind == "L": on, post = a.long, max(a.post, 6.0)
            if kind == "F": freq, on = 8000.0, 0.1
            if kind == "FB": freq, on, hi = 8000.0, 0.1, a.hi2
            inp = f"{W}/sig/tb_{freq:g}_{lo:g}_{hi:g}_{on:g}_{post:g}.wav"
            g = f"{CB} gen-tb {inp} --freq {freq:g} --lo {lo:g} --hi {hi:g} --pre 1 --on {on:g} --post {post:g}"
            need(inp, g)
            out, cmd = render(inp, test)
            an = f"{CB} timing {inp} {out} --freq {freq:g} --pre 1 --on {on:g} --lag {lag}"
            write(test, [g, cmd, an], sh(an)[0])
        elif test.startswith("thd"):
            # thd=1k at hi ; thdlo=1k at lo ; thd100=100 Hz at hi ; thd100lo
            freq = 100.0 if "100" in test else 1000.0
            lvl = a.lo if test.endswith("lo") else a.hi
            inp = f"{W}/sig/sine_{freq:g}_{lvl:g}.wav"
            g = f"{SIG} gen sine {inp} --freq {freq:g} --db {lvl:g} --secs 4"
            need(inp, g)
            out, cmd = render(inp, test)
            an = f"{SIG} thd {out} --freq {freq:g}"
            an2 = f"{SIG} curve {inp} {out} --hold 4.0"
            an3 = f"{CB} ripple {inp} {out} --freq {freq:g} --lag {lag}"
            write(test, [g, cmd, an, an2, an3], sh(an)[0] + sh(an2)[0] + sh(an3)[0])
        elif test.startswith("fsteps"):
            lvl = a.lo if test.endswith("lo") else a.hi
            inp = f"{W}/sig/fsteps_{lvl:g}.wav"
            g = f"{CB} gen-fsteps {inp} --db {lvl:g} --hold 1.5"
            need(inp, g)
            out, cmd = render(inp, test)
            an = f"{CB} fgain {inp} {out} --hold 1.5 --lag {lag}"
            write(test, [g, cmd, an], sh(an)[0])
        elif test == "fr":
            inp = f"{W}/sig/noise_m50.wav"
            need(inp, f"{SIG} gen noise {inp} --db -50 --secs 2")
            out, cmd = render(inp, test)
            an = f"{SIG} fr {inp} {out} --points 24"
            write(test, [f"{SIG} gen noise {inp} --db -50 --secs 2", cmd, an], sh(an)[0])
    print(f"{a.name}/{a.label}: lag={lag}")

if __name__ == "__main__":
    main()
