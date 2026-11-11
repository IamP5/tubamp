"""TONEX cab black-box. States are authored with `auhost state --set` and replayed with `render --state`,
because cab/model switches set with `render --set` do not land before the first block."""
import bb, numpy as np, subprocess, os, sys, json, time
AU = ["aufx", "Txmm", "Ikmm"]
P = dict(MODEL=73532169, CABTYPE=1257642206, CABMODEL=325428709, M1X=73351854, M1Z=73351856, M2X=73351885, M2Z=73351887,
         M1MODEL=1745627743, M2MODEL=1774256894, BLEND=1249623658, RES=2079943946, PRES=1653443835, DEPTH=64934563)
def state(name, sets):
    path = os.path.join(bb.HERE, f"txs_{name}.aupreset")
    cmd = [bb.AUHOST, "state", *AU, path]
    for k, v in sets.items():
        cmd += ["--set", f"{P[k]}={v}"]
    subprocess.run(cmd, capture_output=True, timeout=240)
    return path
def run(name, sets, inp="imp48.wav", t0=4800, amp=0.1):
    st = state(name, sets)
    bb.render(AU, inp, f"txo_{name}.wav", None, ["--state", st, "--tail", "0.5"])
    d, sr = bb.load(f"txo_{name}.wav")
    h = d[0][t0:] / amp
    a = bb.analyse(d[0], sr, t0, amp)
    # energy centroid delay
    e = h ** 2; cen = float(np.sum(np.arange(len(h)) * e) / np.sum(e))
    lr = float(np.max(np.abs(d[0] - d[1]))) if d.shape[0] > 1 else 0.0
    row = dict(name=name, sr=sr, onset=a["onset"], ipk=a["ipk"], peak=round(float(a["peak"]), 4), l2_db=round(20 * np.log10(a["l2"]), 2),
               t30=a["t30"], t60=a["t60"], centroid=round(cen, 1), lr_diff=round(lr, 6))
    return row, h, sr
if __name__ == "__main__":
    which = sys.argv[1]
    base = dict(MODEL=0, CABTYPE=0.5)
    rows = []; mags = {}
    def go(name, sets, **kw):
        r, h, sr = run(name, sets, **kw)
        rows.append(r); mags[name] = bb.mag(h, sr, bb.FREQS); np.save(os.path.join(bb.HERE, f"txh_{name}.npy"), h)
        print(r, flush=True)
    if which == "rates":
        for sr, inp, t0 in [(44100, "imp44.wav", 4410), (48000, "imp48.wav", 4800), (96000, "imp96.wav", 9600)]:
            go(f"bypass_{sr}", dict(MODEL=0, CABTYPE=1), inp=inp, t0=t0)
            go(f"vir_{sr}", base, inp=inp, t0=t0)
    if which == "mic":
        for x in [0, 0.25, 0.5, 0.75, 1]:
            go(f"m1x_{x}", dict(base, BLEND=0, M1X=x, M1Z=0))
        for z in [0, 0.5, 1]:
            go(f"m1z_{z}", dict(base, BLEND=0, M1X=0.15, M1Z=z))
        go("blend0_m1", dict(base, BLEND=0, M1X=0.15, M1Z=0, M2X=0.15, M2Z=1))
        go("blend1_m2", dict(base, BLEND=1, M1X=0.15, M1Z=0, M2X=0.15, M2Z=1))
        go("blend05", dict(base, BLEND=0.5, M1X=0.15, M1Z=0, M2X=0.15, M2Z=1))
        go("blend025", dict(base, BLEND=0.25, M1X=0.15, M1Z=0, M2X=0.15, M2Z=1))
    if which == "res":
        for r in [0, 0.25, 0.5, 0.75, 1]:
            go(f"res_{r}", dict(base, RES=r))
        for p in [0, 1]:
            go(f"pres_{p}", dict(base, PRES=p))
            go(f"depth_{p}", dict(base, DEPTH=p))
    if which == "cabs":
        for c in [0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.641026, 0.75, 0.85, 1.0]:
            go(f"cab_{c}", dict(base, CABMODEL=c))
        for m in [0, 0.25, 0.5, 0.75, 1]:
            go(f"mic1model_{m}", dict(base, BLEND=0, M1MODEL=m))
    with open(os.path.join(bb.HERE, f"tonex_{which}.tsv"), "w") as fh:
        keys = list(rows[0].keys())
        fh.write("\t".join(keys + [f"{f}Hz" for f in bb.FREQS]) + "\n")
        for r in rows:
            fh.write("\t".join(str(r[k]) for k in keys) + "\t" + "\t".join(f"{v:.1f}" for v in mags[r["name"]]) + "\n")
