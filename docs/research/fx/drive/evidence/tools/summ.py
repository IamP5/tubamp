#!/usr/bin/env python3
"""summ.py [models...]: one summary row per model from the battery TSVs in $W/meas."""
import os, sys
W = os.environ["W"]
def tsv(model, name):
    p = os.path.join(W, "meas", model, name)
    if not os.path.exists(p): return None
    rows = [l.rstrip("\n").split("\t") for l in open(p) if not l.startswith("#")]
    return rows[0], rows[1:]
models = sys.argv[1:] or open(os.path.join(W, "meas_all.txt")).read().split()
print("model\tfr_peak_hz(mid)\tbass110_rel\tlo80_rel\thi5k_rel\tssgain_lo\tssgain_mid\tssgain_hi\th2@1k-18mid\th3@1k-18mid\tthd%@-30/-18/-6 mid\th2@110-18hi\talias5k-6hi_worst\talias5k-6hi_sum\talias1244-6hi_sum\tout_pk@0dB hi")
for m in models:
    fr = tsv(m, "fr.tsv"); th = tsv(m, "thd.tsv"); al = tsv(m, "alias.tsv"); lv = tsv(m, "level_vs_drive.tsv"); dy = tsv(m, "dynamics.tsv")
    if not (fr and th and al and lv and dy): print(m, "\tMISSING"); continue
    f = [float(r[0]) for r in fr[1]]; mid = [float(r[2]) for r in fr[1]]
    pk = max(mid); pkf = f[mid.index(pk)]
    g = lambda hz: mid[f.index(hz)] - pk
    t = {(r[0], r[1], r[2]): r for r in th[1]}; ds = sorted({r[2] for r in th[1]}, key=float); dm, dh = ds[1], ds[-1]
    a = {(r[0], r[1], r[2]): r for r in al[1]}
    ss = [r[4] for r in lv[1]]
    print("\t".join(str(x) for x in [m, int(pkf), f"{g(110):.1f}", f"{g(80):.1f}", f"{g(5000):.1f}", ss[0], ss[4], ss[-1],
          t[("1000", "-18", dm)][4], t[("1000", "-18", dm)][5], "/".join(t[("1000", l, dm)][10] for l in ("-30", "-18", "-6")), t[("110", "-18", dh)][4],
          a[("5000", "-6", dh)][5], a[("5000", "-6", dh)][7], a[("1244", "-6", dh)][7], dy[1][-1][-2]]))
