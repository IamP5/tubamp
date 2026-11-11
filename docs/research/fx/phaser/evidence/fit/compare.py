#!/usr/bin/env python3
"""compare.py: prototype against Guitar Rig 6 targets, same probes, same tracker.

    export W=$RE_STATE/work/phaser-round3     (holds the probe dirs: measure.py probes; $W/pprobe.wav: static.py gen $W/pprobe.wav --secs 212)
    python3 compare.py [--proto ../../prototype/phaser_ref.py] [--targets targets.tsv] > compare.txt

For every row of targets.tsv the matching prototype voicing is rendered over the same
multitone probe files at rate = 1 / measured period, tracked with track.py using the same
settings, and printed next to the target: notch-1 min, max, period, notch depth, peak, with
errors in % (frequencies, period) or dB (levels). Rows:
    p9_block_*  -> phase90 --block   (GR6 Phaser Nine, Color 0.4217)
    p9_c0_*     -> phase90 (script)  (GR6 Color 0 keeps its stage-2 gain, so it is not a
                                      true script: informational only, not a fit target)
    ss_off_*    -> smallstone        ss_on_* -> smallstone --color
    ss_on_r000 (Rate 0, 0.01 Hz) is rendered over the periodic probe $W/pprobe.wav for 102 s and
    analysed with static.py like the Guitar Rig 6 take; its period column is the prototype's set
    rate, not a measurement. The static frames are compared by fit.py (structure step).
Trajectory columns (round 3): for every row with a Guitar Rig 6 trajectory in trajectories.tsv the
prototype is rendered over the same impulse-train probe (trajectory.py TAKES) at rate = 1 / that
period, tracked with itrack.py, folded into the same 200 phase bins, and compared with
trajectory.traj_error(): rms and largest of ours / GR6 - 1 over the whole cycle, after the best
common phase shift. Rows without a trajectory print "-".
"""
import argparse, importlib.util, os, sys, tempfile, shutil
from multiprocessing import Pool
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import track  # noqa: E402
import measure  # noqa: E402
import trajectory as TJ  # noqa: E402

W = measure.W
VOICE = {"p9_block": dict(voicing="phase90", block=True), "p9_c0": dict(voicing="phase90", block=False),
         "ss_off": dict(voicing="smallstone", color=False), "ss_on": dict(voicing="smallstone", color=True)}


def load_proto(path):
    spec = importlib.util.spec_from_file_location("phaser_ref_cmp", path)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m


def read_targets(path):
    rows = []
    for line in open(path):
        if line.startswith("#") or line.startswith("tag\t") or line.startswith("static"): continue
        c = line.rstrip("\n").split("\t"); rows.append(c)
    return rows


def job_quasi(proto_path, tag, rate):
    """Rate-0 row: the periodic probe through the prototype for 102 s (top at 50 s, bottom at
    100 s), analysed by static.py exactly like the Guitar Rig 6 take."""
    import static
    P = load_proto(proto_path); kw = VOICE[tag.rsplit("_", 1)[0]]
    x = track.read_wav(f"{W}/pprobe.wav")[0][0][:int(102 * track.FS)]
    y = P.render(x, track.FS, rate=rate, **kw)
    k = static.bins(); X = np.fft.rfft(x[:static.N])[k]
    starts = np.arange(int(1.0 * track.FS), len(x) - static.N, 2048)
    H = np.array([np.abs(np.fft.rfft(y[s:s + static.N])[k] / X) for s in starts])
    r = static.summary(k * track.FS / static.N, (starts + static.N / 2) / track.FS, H)
    return tag, dict(n1_min=r["n1"]["min"], n1_max=r["n1"]["max"], period=1.0 / rate, depth=r["n1"]["depth"], peak=r["peak_db"],
                     n2_min=r["n2"]["min"] if r["n2"] else float("nan"), n2_max=r["n2"]["max"] if r["n2"] else float("nan"))


def job_traj(proto_path, tag, period, ref_f1):
    P = load_proto(proto_path)
    t, n1, _ = TJ.proto_track(P, tag, 1.0 / period)
    _, f1, _ = TJ.fold(t, n1, period)
    r = TJ.traj_error(ref_f1, f1)
    return "traj:" + tag, dict(rms=r[0], max=r[1], shift=r[2])


def job(args):
    if args[0] == "traj": return job_traj(*args[1:])
    proto_path, tag, secs, lo, rate, tmp = args
    if secs is None: return job_quasi(proto_path, tag, rate)
    P = load_proto(proto_path)
    kw = VOICE[tag.rsplit("_", 1)[0]]
    ind = f"{W}/probe{secs}"; outd = os.path.join(tmp, tag); os.makedirs(outd, exist_ok=True)
    m = track.read_grid(ind)[3]
    xs = np.stack([track.read_wav(f"{ind}/tone_{j:02d}.wav")[0][0] for j in range(m)], axis=1)
    y = P.render(xs, track.FS, rate=rate, **kw)
    for j in range(m): track.write_wav(f"{outd}/tone_{j:02d}.wav", y[:, j])
    f, step, t, H, _ = track.H_from_dirs(ind, outd, 8, 16, 0, 0.0)
    r = track.summarise(t, f, step, H, lo, 12000, 3.0, 1.0)
    shutil.rmtree(outd)
    return tag, dict(n1_min=r["n1"]["min"], n1_max=r["n1"]["max"], period=track.best_period(r),
                     depth=r["n1"]["depth"], peak=r["peak_db"],
                     n2_min=r["n2"]["min"] if r["n2"] else float("nan"), n2_max=r["n2"]["max"] if r["n2"] else float("nan"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--proto", default=os.path.join(HERE, "..", "..", "prototype", "phaser_ref.py"))
    ap.add_argument("--targets", default=os.path.join(HERE, "targets.tsv"))
    ap.add_argument("--only", default="", help="compare only rows whose tag starts with this")
    ap.add_argument("--traj-only", action="store_true", help="trajectory measure only (no multitone renders)")
    a = ap.parse_args()
    take = {t[0]: t for t in measure.TAKES}
    rows = [r for r in read_targets(a.targets) if r[0].startswith(a.only)]
    tmp = tempfile.mkdtemp(prefix="cmp_", dir=W)
    jobs = [(os.path.abspath(a.proto), r[0], take[r[0]][3] if r[0] in take else None, take[r[0]][5] if r[0] in take else 60,
             1.0 / float(r[5]), tmp) for r in rows]
    jobs.sort(key=lambda j: j[2] is not None)          # the long Rate-0 render first
    TR = TJ.read_tsv(os.path.join(HERE, "trajectories.tsv"))
    if a.traj_only:
        jobs = [("traj", os.path.abspath(a.proto), r[0], TR[r[0]]["period"], TR[r[0]]["f1"]) for r in rows if r[0] in TR]
        with Pool(min(len(jobs), 4)) as p: res = dict(p.map(job, jobs, chunksize=1))
        shutil.rmtree(tmp, ignore_errors=True)
        print(f"# prototype: {a.proto}   trajectory measure only (see compare.py header)\ntag\trate_hz\ttraj_rms%\ttraj_max%")
        for j in jobs:
            r = res["traj:" + j[2]]; print(f"{j[2]}\t{1 / j[3]:.4f}\t{r['rms']:.2f}\t{r['max']:+.2f}")
        return
    jobs += [("traj", os.path.abspath(a.proto), r[0], TR[r[0]]["period"], TR[r[0]]["f1"]) for r in rows if r[0] in TR]
    with Pool(min(len(jobs), 4)) as p:                 # 4 at a time: each multitone job writes its WAVs to $W
        res = dict(p.map(job, jobs, chunksize=1))
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"# prototype: {os.path.relpath(os.path.abspath(a.proto), HERE)}   targets: {os.path.basename(a.targets)}")
    print("# min/max = notch-1 sweep extremes (Hz); period (s); depth = median notch-1 level (dB); peak = median per-frame maximum (dB);")
    print("# n2 = notch-2 extremes. ss_on_r000 is the Rate-0 (0.01 Hz) quasi-static take, analysed with static.py.")
    print("# error columns: ours/target - 1 in %, or ours - target in dB. p9_c0 rows are informational (GR6 Color 0 is not a true script).")
    print("# traj_rms%, traj_max% = whole notch-1 trajectory against trajectories.tsv (impulse-train tracker, best common phase shift).")
    print("tag\trate_hz\tmin_gr6\tmin_ours\tmin_err%\tmax_gr6\tmax_ours\tmax_err%\tper_gr6\tper_ours\tper_err%\tdepth_gr6\tdepth_ours\tdepth_err_db\tpeak_gr6\tpeak_ours\tpeak_err_db\tn2_gr6\tn2_ours\tn2_min_err%\tn2_max_err%\ttraj_rms%\ttraj_max%")
    worst = {}
    for r in rows:
        tag = r[0]; o = res[tag]; g = dict(n1_min=float(r[1]), n1_max=float(r[2]), period=float(r[5]), depth=float(r[7]), peak=float(r[8]))
        e = [100 * (o["n1_min"] / g["n1_min"] - 1), 100 * (o["n1_max"] / g["n1_max"] - 1), 100 * (o["period"] / g["period"] - 1)]
        grp = tag.rsplit("_", 1)[0]
        n2 = [100 * (o["n2_min"] / float(r[3]) - 1), 100 * (o["n2_max"] / float(r[4]) - 1)]
        w = worst.setdefault(grp, [0, 0, 0, 0, 0, None, None])
        tj = res.get("traj:" + tag)
        if tj:
            w[5] = max(w[5] or 0, tj["rms"]); w[6] = max(w[6] or 0, abs(tj["max"]))
        tjs = f"{tj['rms']:.2f}\t{tj['max']:+.2f}" if tj else "-\t-"
        if grp == "ss_on": w[4] = max(w[4], *(abs(x) for x in n2 if np.isfinite(x)))
        w[0] = max(w[0], abs(e[0]), abs(e[1])); w[1] = max(w[1], abs(e[2]))
        w[2] = max(w[2], abs(o["peak"] - g["peak"])); w[3] = max(w[3], abs(o["depth"] - g["depth"]))
        print(f"{tag}\t{1/g['period']:.4f}\t{g['n1_min']:.1f}\t{o['n1_min']:.1f}\t{e[0]:+.2f}\t{g['n1_max']:.1f}\t{o['n1_max']:.1f}\t{e[1]:+.2f}\t"
              f"{g['period']:.4f}\t{o['period']:.4f}\t{e[2]:+.2f}\t{g['depth']:.2f}\t{o['depth']:.2f}\t{o['depth']-g['depth']:+.2f}\t"
              f"{g['peak']:.2f}\t{o['peak']:.2f}\t{o['peak']-g['peak']:+.2f}\t{r[3]}-{r[4]}\t{o['n2_min']:.0f}-{o['n2_max']:.0f}\t{n2[0]:+.2f}\t{n2[1]:+.2f}\t{tjs}")
    print("# worst per group: |notch-1 min/max err| %, |period err| %, |peak err| dB, |depth err| dB, trajectory rms %, |trajectory max| %,")
    print("# |notch-2 min/max err| % (ss_on only; Stoned Phaser notch 2 leaves the 12 kHz band with Color off, Phaser Nine notch 2 is not a fit target)")
    for k, w in worst.items():
        tw = f"\ttraj {w[5]:.2f} % rms\t{w[6]:.2f} % max" if w[5] is not None else "\ttraj -\t-"
        print(f"# {k}\t{w[0]:.2f} %\t{w[1]:.2f} %\t{w[2]:.2f} dB\t{w[3]:.2f} dB" + tw + (f"\tn2 {w[4]:.2f} %" if k == "ss_on" else ""))


if __name__ == "__main__":
    main()
