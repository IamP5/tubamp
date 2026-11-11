#!/usr/bin/env python3
"""trajectory.py: whole notch-1 trajectory f1(t) over one LFO period (numpy only).

    export RE_STATE=<scratch>/re RE=<repo>/tools/re W=$RE_STATE/work/phaser-round3
    cd $W && $RE_STATE/bin/auhost state aufx 'Ni$R' -NI- gr6_default.aupreset            (once)
    python3 $RE/ngrc_xml.py ".../Component Presets/Phaser Nine/Phaser Nine Factory.ngrc" $W/p9      (once: p9_doc0.xml)
    python3 $RE/ngrc_xml.py ".../Component Presets/Stoned Phaser/Stoned Phaser Factory.ngrc" $W/ss  (once: ss_doc0.xml)
    python3 trajectory.py gr <tag> [<tag> ...]   render Guitar Rig 6 over an impulse train, track, save $W/traj/<tag>.gr.npz
    python3 trajectory.py tsv                    fold every saved take and write trajectories.tsv (this directory)
    python3 trajectory.py selftest               tracker + fold error on a synthetic phaser -> selftest_traj.txt

Method (the independent verifier's, verification-refit.md section 1 and its traj.py; tracker copied
as itrack.py): the probe is a train of single-sample impulses (amplitude 0.25), one every P samples,
in M files shifted by P/M, so snapshots are P/M samples apart. Each impulse returns the response at
one instant of the LFO; notch 1 is the lowest local minimum with 3 dB prominence, refined on the
DTFT. Guitar Rig 6 states: tools/re/gr6state.py on the component's INIT document with the study's
reference settings (below) and the rack gate off (Rack.G=0: the gate chops an impulse train).
Rendering: auhost render aufx 'Ni$R' -NI- in.wav out.wav --state <state>, three at a time; a file
with NaN or a short length is re-rendered serially. Every WAV is deleted after tracking.

Fold: snapshots before `skip` (1 s) are dropped; phase = t / period (period from the track, itrack's
level-crossing estimate). The phase origin is set so that the fundamental of ln f1 over the fold
has its minimum at phase 0 (robust where notch 1 fades near the bottom, Small Stone Color on). The
fold is cut into NB = 200 bins; each row of trajectories.tsv is the median f1 of one bin.

Trajectory error (traj_error): our folded trajectory against a reference fold, after the best common
circular phase shift (the LFOs' start phases are arbitrary), over bins where both are resolved:
rms and largest of 100 * (ours / ref - 1).
"""
import math, os, subprocess, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import itrack as T  # noqa: E402
import track  # noqa: E402  (read_wav / write_wav)

W = os.environ.get("W") or os.path.join(os.environ.get("RE_STATE", "."), "work", "phaser-round3")
RE = os.environ.get("RE", os.path.join(HERE, "..", "..", "..", "..", "..", "..", "tools", "re"))
RE_STATE = os.environ.get("RE_STATE", "")
FS = 48000
AMP = 0.25
NB = 200
SKIP = 1.0
P9 = "n=4 c=0.4217 r=0.5 e=0.5 p=0.5 x=0.5 o=0.5 i=0 s=0"
SS = "Sy=0 Not=2 Rt=0.5 Cl=0 CS=0.74 DW=0.5 F1=0.5 F2=0.5 Rot=0 In=0"
# tag: component, overrides, seconds, P, M, band low (Hz), level offset (dB), prototype voicing
TAKES = {
    "p9_block_p010": ("p9", "p=0.1", 16.5, 512, 8, 300, -5.40, dict(voicing="phase90", block=True)),
    "p9_block_p025": ("p9", "p=0.25", 10.5, 512, 8, 300, -5.40, dict(voicing="phase90", block=True)),
    "p9_block_p050": ("p9", "p=0.5", 5.0, 512, 8, 300, -5.40, dict(voicing="phase90", block=True)),
    "p9_block_p075": ("p9", "p=0.75", 4.0, 512, 8, 300, -5.40, dict(voicing="phase90", block=True)),
    "p9_block_p090": ("p9", "p=0.9", 3.0, 512, 16, 300, -5.40, dict(voicing="phase90", block=True)),
    "p9_block_p100": ("p9", "p=1.0", 3.0, 512, 16, 300, -5.40, dict(voicing="phase90", block=True)),
    "ss_off_r040": ("ss", "Rt=0.4", 15.0, 2048, 16, 60, 0.0, dict(voicing="smallstone", color=False)),
    "ss_off_r050": ("ss", "Rt=0.5", 8.5, 2048, 16, 60, 0.0, dict(voicing="smallstone", color=False)),
    "ss_off_r075": ("ss", "Rt=0.75", 3.5, 2048, 32, 60, 0.0, dict(voicing="smallstone", color=False)),
    "ss_on_r040": ("ss", "Rt=0.4 Cl=1", 15.0, 2048, 16, 60, 0.0, dict(voicing="smallstone", color=True)),
    "ss_on_r050": ("ss", "Rt=0.5 Cl=1", 8.5, 2048, 16, 60, 0.0, dict(voicing="smallstone", color=True)),
    "ss_on_r075": ("ss", "Rt=0.75 Cl=1", 3.5, 2048, 32, 60, 0.0, dict(voicing="smallstone", color=True)),
}


# ----------------------------------------------------------------------------- tracking
def track_take(ys, pos, P, lo, gain):
    """ys: list of M output arrays. Returns t, n1 (Hz, NaN where unresolved), period (s)."""
    ts, f, H, _, segs = T.snapshots(ys, pos, P, AMP)
    tr = T.track(ts, f, H, segs, AMP, lo, 12000.0, gain_db=gain)
    r = T.summarise(tr, lo, 12000.0)
    return ts, r["n1"], r["period"], r


def fold(t, f1, period, nb=NB, skip=SKIP):
    """Median ln f1 per phase bin, phase origin at the minimum of the fit's fundamental.
    Returns bin centres (cycles), f1 per bin (Hz, NaN where empty), counts."""
    ok = (t >= skip) & np.isfinite(f1)
    tt, y = t[ok], np.log(f1[ok])
    ph = (tt / period) % 1.0
    A = np.column_stack([np.ones_like(ph), np.cos(2 * np.pi * ph), np.sin(2 * np.pi * ph)])
    co = np.linalg.lstsq(A, y, rcond=None)[0]
    ph0 = (math.atan2(co[2], co[1]) / (2 * np.pi) + 0.5) % 1.0     # where b cos + c sin is lowest
    ph = (ph - ph0) % 1.0
    k = np.minimum((ph * nb).astype(int), nb - 1)
    med = np.full(nb, np.nan); cnt = np.bincount(k, minlength=nb)
    for b in range(nb):
        if cnt[b]: med[b] = np.median(y[k == b])
    return (np.arange(nb) + 0.5) / nb, np.exp(med), cnt


def traj_error(ref_f, ours_f, max_shift=0.15, steps=3001):
    """Both on the same NB-bin grid. Best circular shift of ours (linear interpolation in ln f over
    the periodic grid). Returns rms %, signed largest %, shift (cycles), bins used."""
    nb = len(ref_f); ph = (np.arange(nb) + 0.5) / nb
    a = np.log(ref_f); b = np.log(ours_f); okb = np.isfinite(b)
    pb, yb = ph[okb], b[okb]
    pb3 = np.concatenate([pb - 1, pb, pb + 1]); yb3 = np.concatenate([yb, yb, yb])
    gapb = np.concatenate([np.diff(pb3), [1.0]])
    best = None
    for s in np.linspace(-max_shift, max_shift, steps):
        q = (ph + s) % 1.0
        bi = np.interp(q, pb3, yb3)
        gap = np.interp(q, pb3, gapb) > 3.5 / nb             # ours unresolved nearby
        e = (bi - a)[np.isfinite(a) & ~gap]
        if len(e) < 0.5 * nb: continue
        r = math.sqrt(float(np.mean(e * e)))
        if best is None or r < best[0]: best = (r, s, e)
    r, s, e = best; i = int(np.argmax(np.abs(e)))
    return 100 * (math.exp(r) - 1), 100 * (math.exp(e[i]) - 1), s, len(e)


# ------------------------------------------------------------------------ Guitar Rig 6
def state(comp, ov, out):
    base = dict(kv.split("=") for kv in (P9 if comp == "p9" else SS).split())
    base.update(dict(kv.split("=") for kv in ov.split()))
    doc, name = ("p9_doc0.xml", "Phaser Nine") if comp == "p9" else ("ss_doc0.xml", "Stoned Phaser")
    subprocess.run([sys.executable, f"{RE}/gr6state.py", f"{W}/gr6_default.aupreset", f"{W}/{doc}", name, out]
                   + [f"{k}={v}" for k, v in base.items()] + ["Rack.G=0"], check=True)


def gr_render(x, st, tag, par=3):
    d = f"{W}/tmp_{tag}"; os.makedirs(d, exist_ok=True); M = x.shape[0]
    for j in range(M): track.write_wav(f"{d}/in_{j:02d}.wav", x[j])
    def cmd(j):
        return [f"{RE_STATE}/bin/auhost", "render", "aufx", "Ni$R", "-NI-", f"{d}/in_{j:02d}.wav", f"{d}/out_{j:02d}.wav", "--state", st]
    for a in range(0, M, par):
        ps = [subprocess.Popen(cmd(j), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) for j in range(a, min(M, a + par))]
        for p in ps: p.wait()
    ys = []
    for j in range(M):
        for attempt in range(4):
            ok = os.path.exists(f"{d}/out_{j:02d}.wav")
            if ok:
                y = track.read_wav(f"{d}/out_{j:02d}.wav")[0][0]
                ok = len(y) >= x.shape[1] and bool(np.isfinite(y).all())
            if ok: break
            print(f"  re-render {tag} file {j} (attempt {attempt + 1})", flush=True)
            subprocess.run(cmd(j), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            raise SystemExit(f"render failed: {tag} file {j}")
        ys.append(y[:x.shape[1]])
    for fn in os.listdir(d): os.remove(f"{d}/{fn}")
    os.rmdir(d)
    return ys


def gr(tag):
    comp, ov, secs, P, M, lo, gain, _ = TAKES[tag]
    x, pos = T.probe(int(secs * FS), P, M, AMP)
    st = f"{W}/st_{tag}.aupreset"; state(comp, ov, st)
    ys = gr_render(x, st, tag)
    t, n1, per, r = track_take(ys, pos, P, lo, gain)
    os.makedirs(f"{W}/traj", exist_ok=True)
    np.savez_compressed(f"{W}/traj/{tag}.gr.npz", t=t, n1=n1, n2=r["n2"], period=per)
    print(f"{tag}\tperiod {per:.5f} s\tresolved {np.mean(np.isfinite(n1[t >= SKIP])):.3f}\t"
          f"n1 {np.nanmin(n1[t >= SKIP]):.1f}-{np.nanmax(n1[t >= SKIP]):.1f} Hz", flush=True)


def proto_track(P_mod, tag, rate):
    """The prototype over the same probe at `rate`, tracked identically. Returns t, n1, period."""
    comp, ov, secs, P, M, lo, gain, kw = TAKES[tag]
    x, pos = T.probe(int(secs * FS), P, M, AMP)
    y = P_mod.render(x.T.copy(), FS, rate=rate, **kw)
    t, n1, per, _ = track_take(list(y.T), pos, P, lo, 0.0)
    return t, n1, per


# --------------------------------------------------------------------------- tsv
def read_tsv(path=os.path.join(HERE, "trajectories.tsv")):
    """Returns {tag: dict(period=..., phase=array, f1=array)}."""
    out = {}
    for line in open(path):
        if line.startswith("#") or line.startswith("tag\t"): continue
        c = line.rstrip("\n").split("\t")
        d = out.setdefault(c[0], dict(period=float(c[1]), phase=[], f1=[]))
        d["phase"].append(float(c[2])); d["f1"].append(float(c[3]) if c[3] != "nan" else np.nan)
    for d in out.values():
        d["phase"] = np.array(d["phase"]); d["f1"] = np.array(d["f1"])
    return out


def tsv():
    lines = ["# Guitar Rig 6 notch-1 trajectories over one LFO period, 48 kHz, impulse-train tracker (itrack.py).",
             "# Reproduce (see trajectory.py header for the one-time state files):",
             "#   export RE_STATE=<scratch>/re RE=<repo>/tools/re W=$RE_STATE/work/phaser-round3",
             "#   python3 trajectory.py gr " + " ".join(TAKES) + "   (then: python3 trajectory.py tsv)",
             "# which per take builds the state",
             "#   python3 $RE/gr6state.py $W/gr6_default.aupreset $W/<p9|ss>_doc0.xml '<Phaser Nine|Stoned Phaser>' $W/st_<tag>.aupreset <base> <overrides> Rack.G=0",
             f"#   base Phaser Nine: {P9}",
             f"#   base Stoned Phaser: {SS}",
             "# renders M impulse-train files (one 0.25 impulse every P samples, file j shifted by j*P/M, first at sample 4800)",
             "#   $RE_STATE/bin/auhost render aufx 'Ni$R' -NI- in_<j>.wav out_<j>.wav --state $W/st_<tag>.aupreset",
             "# and tracks notch 1 (lowest minimum, 3 dB prominence, band <lo> Hz to 12 kHz). Takes (tag: overrides, seconds, P, M, lo):"]
    for k, v in TAKES.items():
        lines.append(f"#   {k}: {v[1]}, {v[2]} s, P {v[3]}, M {v[4]}, lo {v[5]}")
    lines += [f"# Fold: snapshots after {SKIP} s, phase = t / period, origin at the minimum of the fundamental of ln f1;",
              f"# {NB} bins, f1_hz = median per bin (nan: notch 1 not resolved in that bin, Small Stone Color on near the bottom).",
              "# Tracker + fold error on a synthetic phaser: selftest_traj.txt.",
              "tag\tperiod_s\tphase\tf1_hz\tsnapshots"]
    for tag in TAKES:
        p = f"{W}/traj/{tag}.gr.npz"
        if not os.path.exists(p): continue
        z = np.load(p); ph, f1, cnt = fold(z["t"], z["n1"], float(z["period"]))
        for a, b, c in zip(ph, f1, cnt):
            lines.append(f"{tag}\t{float(z['period']):.6f}\t{a:.4f}\t{b:.2f}\t{c}")
    open(os.path.join(HERE, "trajectories.tsv"), "w").write("\n".join(lines) + "\n")
    print(f"wrote trajectories.tsv ({len(lines)} lines)")


# --------------------------------------------------------------------------- selftest
def synth_run(x, fc, dry, wet):
    """Own synthetic phaser: four first-order allpasses, transposed direct form, coefficient per sample.
    Frozen notch k: fs/pi * atan(tan(pi fc/fs) * tan((2k-1) pi/8))."""
    g = np.tan(np.pi * fc / FS); a = (g - 1) / (g + 1)
    M, n = x.shape; s = np.zeros((4, M)); y = np.empty_like(x)
    for i in range(n):
        u = x[:, i]; ai = a[i]
        for k in range(4):
            v = ai * u + s[k]; s[k] = u - ai * v; u = v
        y[:, i] = dry * x[:, i] + wet * u
    return y


def synth_sweep(n, rate, lo, hi, kind):
    ph = (rate * np.arange(n) / FS) % 1.0
    if kind == "smooth":       # curved triangle (power 1.6 on each half) through a 4.4 Hz one-pole, log f
        tri = np.where(ph < 0.5, 2 * ph, 2 - 2 * ph) ** 1.6 * 2 - 1
        k = 1 - math.exp(-2 * math.pi * 4.4 / FS); s = tri[0]; v = np.empty(n)
        for i in range(n):
            s += k * (tri[i] - s); v[i] = s
    else:                      # RC triangle, sharp turning points
        up = ph < 0.5; u = np.where(up, 2 * ph, 2 * ph - 1); c = (1 - np.exp(-0.7 * u)) / (1 - math.exp(-0.7))
        v = np.where(up, 2 * c - 1, 1 - 2 * c)
    return np.exp(0.5 * math.log(lo * hi) + 0.5 * math.log(hi / lo) * v)


def selftest():
    cases = [("smooth", 0.1429, 16.5, 512, 8, 300, 930.0, 3530.0), ("smooth", 1.071, 5.0, 512, 8, 300, 930.0, 3530.0),
             ("smooth", 1.930, 4.0, 512, 8, 300, 930.0, 3530.0), ("smooth", 9.756, 3.0, 512, 16, 300, 930.0, 3530.0),
             ("sharp", 0.3162, 8.5, 2048, 16, 60, 355.0, 8300.0), ("sharp", 1.778, 3.5, 2048, 32, 60, 355.0, 8300.0)]
    out = ["# trajectory.py selftest: synthetic four-stage phaser (own code, synth_run), known per-sample notch 1.",
           "# Tracked and folded exactly like the Guitar Rig 6 takes; the truth is folded the same way (same period, same origin).",
           "# err = tracked fold against the true fold, no shift; 'shift' column: traj_error() with its free shift.",
           "case\trate_hz\tP\tM\tperiod_err%\trms%\tmax%\ttraj_error rms%\tmax%\tshift_cycles"]
    for kind, rate, secs, P, M, lo, flo, fhi in cases:
        n = int(secs * FS); x, pos = T.probe(n, P, M, AMP)
        fc = synth_sweep(n, rate, flo, fhi, kind)
        y = synth_run(x, fc, 0.5, 0.465) if kind == "smooth" else synth_run(x, fc, 0.75, 0.5)
        t, n1, per, _ = track_take(list(y), pos, P, lo, 0.0)
        true_n1 = FS / np.pi * np.arctan(np.tan(np.pi * fc / FS) * math.tan(math.pi / 8))
        tt = np.arange(n) / FS
        # truth sampled at the snapshot times, folded with the tracked period and origin
        ph, f_trk, _ = fold(t, n1, per)
        ok = (t >= SKIP) & np.isfinite(n1)
        phs = (t[ok] / per) % 1.0
        A = np.column_stack([np.ones_like(phs), np.cos(2 * np.pi * phs), np.sin(2 * np.pi * phs)])
        co = np.linalg.lstsq(A, np.log(n1[ok]), rcond=None)[0]; ph0 = (math.atan2(co[2], co[1]) / (2 * np.pi) + 0.5) % 1.0
        ts = t[t >= SKIP]; tv = np.interp(ts, tt, true_n1)
        k = np.minimum((((ts / per) % 1.0 - ph0) % 1.0 * NB).astype(int), NB - 1)
        f_true = np.array([math.exp(np.median(np.log(tv[k == b]))) if np.any(k == b) else np.nan for b in range(NB)])
        e = np.log(f_trk / f_true); e = e[np.isfinite(e)]
        r2 = traj_error(f_true, f_trk)
        out.append(f"{kind}\t{rate}\t{P}\t{M}\t{100 * (per * rate - 1):+.4f}\t{100 * (math.exp(math.sqrt(np.mean(e * e))) - 1):.3f}\t"
                   f"{100 * (math.exp(e[np.argmax(np.abs(e))]) - 1):+.3f}\t{r2[0]:.3f}\t{r2[1]:+.3f}\t{r2[2]:+.5f}")
        print(out[-1], flush=True)
    open(os.path.join(HERE, "selftest_traj.txt"), "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "gr":
        for tg in sys.argv[2:]: gr(tg)
    elif cmd == "tsv": tsv()
    elif cmd == "selftest": selftest()
    else: raise SystemExit(__doc__)
