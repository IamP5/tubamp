#!/usr/bin/env python3
"""Measurement battery for a drive model, through either the in-process class harness (rigcall)
or the AU host (auhost). Models are defined in models.py next to this file.

    meas.py <model> [thd] [alias] [fr] [lvl] [dyn]      (default: all)
    meas.py <model> one --set drive=0.5 --freq 1000 --db -18   one render, prints harmonics

Writes TSVs to $W/meas/<model>/. Every TSV starts with comment lines giving the exact commands."""
import os, subprocess, sys, shlex
import numpy as np
sys.path.insert(0, os.environ["RE"]); sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sig
from models import MODELS

W = os.environ["W"]; SR = 48000
RIG = os.path.join(W, "tools", "rigcall"); AUHOST = os.path.join(os.environ["RE_STATE"], "bin", "auhost")
TMP = os.path.join(W, "wav"); os.makedirs(TMP, exist_ok=True)

def wav_in(name, x):
    path = os.path.join(TMP, name); sig.write_wav(path, x.astype(np.float32), SR); return path

def render(model, settings, in_path, tag="t"):
    m = MODELS[model]; out = os.path.join(TMP, f"{model}_{tag}_out.wav")
    vals = dict(m.get("defaults", {})); vals.update(settings)
    if m["kind"] == "rig":
        lines = list(m["pre"])
        for k, v in vals.items():
            t = m["params"][k]
            lines += [s.format(v=v) for s in (t if isinstance(t, list) else [t])]
        lines += list(m["post"])
        rec = os.path.join(TMP, f"{model}_{tag}.rec"); open(rec, "w").write("\n".join(lines) + "\n")
        cmd = [RIG, m["bin"], rec, in_path, out]
    elif m["kind"] == "gr6au":
        # Guitar Rig 6 AU with one rack component: state authored from the factory component preset (gr6state.py)
        st = os.path.join(TMP, f"{model}_{tag}.aupreset"); g = os.path.join(W, "gr6au")
        mk = [sys.executable, os.path.join(W, "tools", "gr6state.py"), os.path.join(g, "gr6_default.aupreset"), os.path.join(g, m["doc"]), m["name"], st]
        mk += [f"{m['params'][k]}={v}" for k, v in vals.items()] + [f"{k}={v}" for k, v in m.get("fixed", {}).items()] + ["Rack.G=0"]
        r0 = subprocess.run(mk, capture_output=True, text=True)
        if r0.returncode != 0: sys.exit("gr6state failed: " + r0.stderr)
        cmd = [AUHOST, "render", "aufx", "Ni$R", "-NI-", in_path, out, "--state", st]
        pre_cmd = " ".join(shlex.quote(c) for c in mk) + " && "
    else:
        cmd = [AUHOST, "render", *m["codes"], in_path, out]
        for pid, v in m.get("fixed", {}).items(): cmd += ["--set", f"{pid}={v}"]
        for k, v in vals.items(): cmd += ["--set", f"{m['params'][k]}={v}"]
        cmd += m.get("extra", [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        sys.exit(f"render failed: {' '.join(shlex.quote(c) for c in cmd)}\n{r.stdout}{r.stderr}")
    y, sr = sig.read_wav(out)
    return y[m.get("ch", 0)], (pre_cmd if m["kind"] == "gr6au" else "") + " ".join(shlex.quote(c) for c in cmd)

def sine(freq, db, secs=2.0):
    n = int(SR * secs); x = 10 ** (db / 20) * np.sin(2 * np.pi * freq * np.arange(n) / SR)
    x[:480] *= np.linspace(0, 1, 480); return x

def spectrum(y):
    """Amplitude spectrum (dB re full-scale sine peak) of the last second, 1 Hz bins."""
    seg = y[-SR:]; w = np.hanning(len(seg)); s = np.abs(np.fft.rfft(seg * w)) / (w.sum() / 2)
    return s

def comp(s, f):
    f = int(round(f)); lo, hi = max(0, f - 2), f + 3
    return float(np.sqrt(np.sum(s[lo:hi] ** 2) / 1.5))   # Hann ENBW 1.5 bins

def db(v): return 20 * np.log10(max(v, 1e-12))

def harmonics(y, f0, nh=9):
    s = spectrum(y); h = [comp(s, f0 * k) for k in range(1, nh + 1) if f0 * k < SR / 2 - 5]
    thd = 100 * np.sqrt(sum(v * v for v in h[1:])) / max(h[0], 1e-12)
    dc = float(np.mean(y[-SR:]))
    return h, thd, dc

def header(fh, cmds):
    for c in cmds: fh.write(f"# {c}\n")

def run_thd(model, outdir):
    m = MODELS[model]; dk = m["drive"]; rows = []; cmds = []
    for f0 in (1000, 110):
        for lvl in (-30, -18, -6):
            x = wav_in(f"sine_{f0}_{-lvl}.wav", sine(f0, lvl))
            for dv in m["drives"]:
                y, cmd = render(model, {dk: dv}, x); h, thd, dc = harmonics(y, f0)
                cmds.append(cmd) if len(cmds) < 2 else None
                rows.append([f0, lvl, dv, f"{db(h[0]):.2f}"] + [f"{db(v) - db(h[0]):.1f}" for v in h[1:7]] + [f"{thd:.2f}", f"{dc:.5f}"])
    with open(os.path.join(outdir, "thd.tsv"), "w") as fh:
        header(fh, [f"meas.py {model} thd ; input: numpy sine, 2 s, 48 kHz, analysed over the last 1 s (Hann, 1 Hz bins)", "example render: " + cmds[0]])
        fh.write("freq\tin_dbfs\tdrive\tfund_db\th2\th3\th4\th5\th6\th7\tthd_pct\tdc\n")
        for r in rows: fh.write("\t".join(str(c) for c in r) + "\n")
    print("== thd (freq in_db drive | fund h2 h3 h4 h5 | thd% dc)")
    for r in rows: print(f"{r[0]:>5} {r[1]:>4} {r[2]:>5} | {r[3]:>7} {r[4]:>6} {r[5]:>6} {r[6]:>6} {r[7]:>6} | {r[10]:>6} {r[11]}")

def run_alias(model, outdir):
    m = MODELS[model]; dk = m["drive"]; rows = []; cmds = []
    for f0 in (5000, 1244):
        for lvl in (-18, -6):
            x = wav_in(f"sine_{f0}_{-lvl}.wav", sine(f0, lvl))
            for dv in (m["drives"][1], m["drives"][-1]):
                y, cmd = render(model, {dk: dv}, x); s = spectrum(y); cmds.append(cmd)
                fund = comp(s, f0); mask = np.ones(len(s), bool); mask[:20] = False
                for k in range(1, int(SR / 2 // f0) + 1): mask[max(0, f0 * k - 6): f0 * k + 7] = False
                inh = s.copy(); inh[~mask] = 0
                below = inh[: f0 - 6] if f0 == 5000 else inh[:20000]
                worst = int(np.argmax(below)); worst_db = db(comp(s, worst)) - db(fund)
                tot = db(np.sqrt(np.sum(inh[:20000] ** 2) / 1.5)) - db(fund)
                totb = db(np.sqrt(np.sum(below ** 2) / 1.5)) - db(fund)
                rows.append([f0, lvl, dv, f"{db(fund):.2f}", worst, f"{worst_db:.1f}", f"{totb:.1f}", f"{tot:.1f}"])
    with open(os.path.join(outdir, "alias.tsv"), "w") as fh:
        header(fh, [f"meas.py {model} alias ; sine 2 s at 48 kHz, last 1 s analysed (Hann, 1 Hz bins); inharmonic = every bin not within 6 Hz of k*f0",
                    "worst_* = strongest inharmonic component below the fundamental (f0=5000) or below 20 kHz (f0=1244); sums are RSS of inharmonic bins", "example render: " + cmds[0]])
        fh.write("freq\tin_dbfs\tdrive\tfund_db\tworst_hz\tworst_rel_db\tinharm_below_f0_rel_db\tinharm_to_20k_rel_db\n")
        for r in rows: fh.write("\t".join(str(c) for c in r) + "\n")
    print("== alias (freq in_db drive | fund | worst_hz worst_rel | sum_below_f0 sum_to_20k)")
    for r in rows: print(f"{r[0]:>5} {r[1]:>4} {r[2]:>5} | {r[3]:>7} | {r[4]:>6} {r[5]:>7} | {r[6]:>7} {r[7]:>7}")

FR_PTS = [30, 50, 80, 110, 160, 220, 320, 450, 640, 720, 900, 1280, 1800, 2500, 3600, 5000, 7000, 10000, 14000, 18000]
def fr_curve(model, settings, lvl_db, tag):
    rng = np.random.default_rng(7); n = SR * 6; x = rng.standard_normal(n); x *= 10 ** (lvl_db / 20) / np.sqrt(np.mean(x * x))
    xin = wav_in(f"noise_{-lvl_db}.wav", x); y, cmd = render(model, settings, xin, tag)
    nfft = 16384; hop = nfft // 2; w = np.hanning(nfft); pxx = 0; pxy = 0
    for st in range(SR, n - nfft, hop):
        X = np.fft.rfft(x[st:st + nfft] * w); Y = np.fft.rfft(y[st:st + nfft] * w); pxx = pxx + np.abs(X) ** 2; pxy = pxy + np.conj(X) * Y
    H = np.abs(pxy) / pxx; fax = np.fft.rfftfreq(nfft, 1 / SR)
    out = []
    for f in FR_PTS:
        lo, hi = np.searchsorted(fax, f / 1.06), max(np.searchsorted(fax, f * 1.06), np.searchsorted(fax, f / 1.06) + 1)
        out.append(db(float(np.sqrt(np.mean(H[lo:hi] ** 2)))))
    return out, cmd, H, fax

def run_fr(model, outdir):
    m = MODELS[model]; dk = m["drive"]; cols = {}; cmds = []
    lvl = m.get("fr_db", -70)
    for dv in m["drives"]:
        c, cmd, H, fax = fr_curve(model, {dk: dv}, lvl, "fr"); cols[dv] = c; cmds.append(cmd)
        band = (fax > 20) & (fax < 20000); k = np.argmax(H * band); pk = db(H[k])
        above = np.where((H >= H[k] * 10 ** (-3 / 20)) & band)[0]
        print(f"== fr drive={dv}: peak {pk:.1f} dB at {fax[k]:.0f} Hz, -3 dB band {fax[above[0]]:.0f}..{fax[above[-1]]:.0f} Hz")
    with open(os.path.join(outdir, "fr.tsv"), "w") as fh:
        header(fh, [f"meas.py {model} fr ; white noise {lvl} dBFS RMS, 6 s, 48 kHz; |Pxy|/Pxx, nfft 16384, Hann, 50% overlap, first 1 s skipped; values are gain in dB, averaged over +-6% around each frequency", "example render: " + cmds[0]])
        fh.write("freq_hz\t" + "\t".join(f"drive_{d}" for d in m["drives"]) + "\n")
        for i, f in enumerate(FR_PTS): fh.write(f"{f}\t" + "\t".join(f"{cols[d][i]:.2f}" for d in m["drives"]) + "\n")
    print("   f:   " + " ".join(f"{f:>6}" for f in FR_PTS))
    for d in m["drives"]: print(f"   {d:<4} " + " ".join(f"{v:>6.1f}" for v in cols[d]))

def run_lvl(model, outdir):
    m = MODELS[model]; dk = m["drive"]; rows = []
    lo, hi = m["drives"][0], m["drives"][-1]
    xs = wav_in("sine_1000_18.wav", sine(1000, -18)); xq = wav_in("sine_1000_70.wav", sine(1000, m.get("fr_db", -70)))
    for i in range(9):
        dv = round(lo + (hi - lo) * i / 8, 4)
        y, cmd = render(model, {dk: dv}, xs); h, thd, _ = harmonics(y, 1000); rms = db(float(np.sqrt(np.mean(y[-SR:] ** 2))) * np.sqrt(2))
        y2, _ = render(model, {dk: dv}, xq); h2, _, _ = harmonics(y2, 1000)
        rows.append([dv, f"{db(h[0]) + 18:.2f}", f"{rms + 18:.2f}", f"{thd:.2f}", f"{db(h2[0]) - m.get('fr_db', -70):.2f}"])
    with open(os.path.join(outdir, "level_vs_drive.tsv"), "w") as fh:
        header(fh, [f"meas.py {model} lvl ; 1 kHz sine at -18 dBFS (large signal) and at {m.get('fr_db', -70)} dBFS (small signal), drive swept", "example render: " + cmd])
        fh.write("drive\tfund_gain_db_at_-18\trms_gain_db_at_-18\tthd_pct_at_-18\tsmall_signal_gain_db\n")
        for r in rows: fh.write("\t".join(str(c) for c in r) + "\n")
    print("== level vs drive (drive | fund gain@-18 | rms gain@-18 | thd% | small-signal gain)")
    for r in rows: print(f"{r[0]:>6} | {r[1]:>7} | {r[2]:>7} | {r[3]:>6} | {r[4]:>7}")

def run_dyn(model, outdir):
    m = MODELS[model]; dk = m["drive"]; rows = []
    for lvl in range(-60, 1, 6):
        x = wav_in(f"sine_1000_{-lvl}.wav", sine(1000, lvl)); r = [lvl]
        for dv in m["drives"]:
            y, cmd = render(model, {dk: dv}, x); pk = db(float(np.max(np.abs(y[-SR:])))); h, thd, _ = harmonics(y, 1000)
            r += [f"{pk:.2f}", f"{thd:.1f}"]
        rows.append(r)
    with open(os.path.join(outdir, "dynamics.tsv"), "w") as fh:
        header(fh, [f"meas.py {model} dyn ; 1 kHz sine, input level stepped; output peak (dBFS) and THD% per drive setting", "example render: " + cmd])
        fh.write("in_dbfs\t" + "\t".join(f"peak_db_drive_{d}\tthd_pct_drive_{d}" for d in m["drives"]) + "\n")
        for r in rows: fh.write("\t".join(str(c) for c in r) + "\n")
    print("== dynamics (in | [peak thd%] per drive " + str(m["drives"]) + ")")
    for r in rows: print(f"{r[0]:>4} | " + "  ".join(f"{r[i]:>7} {r[i+1]:>5}" for i in range(1, len(r), 2)))

PRE_PTS = [60, 80, 110, 160, 220, 320, 450, 640, 900, 1280, 1800, 2500, 3600, 5000, 7000]
def run_pre(model, outdir):
    """Input level at which the fundamental's gain has dropped 1 dB below its small-signal value, per frequency.
    A linear filter after the clipper scales small and large signals alike, so this isolates what happens BEFORE
    (and inside) the clipper: the lower the level, the more gain that frequency sees ahead of the nonlinearity."""
    m = MODELS[model]; dk = m["drive"]; dv = m["drives"][1]; lo_db = m.get("fr_db", -70); rows = []; cmd = ""
    def gain(f, lvl):
        y, c = render(model, {dk: dv}, wav_in("pre.wav", sine(f, lvl, 1.5)), "pre"); s = spectrum(y); return db(comp(s, f)) - lvl, c
    for f in PRE_PTS:
        g0, cmd = gain(f, lo_db); lo, hi = lo_db, 0.0
        ghi, _ = gain(f, hi)
        if ghi > g0 - 1.0: rows.append([f, f"{g0:.2f}", "none", ""]); continue
        for _ in range(11):
            mid = 0.5 * (lo + hi); g, _ = gain(f, mid)
            if g > g0 - 1.0: lo = mid
            else: hi = mid
        rows.append([f, f"{g0:.2f}", f"{0.5 * (lo + hi):.2f}", ""])
    vals = [float(r[2]) for r in rows if r[2] != "none"]; ref = min(vals) if vals else 0.0
    for r in rows: r[3] = f"{ref - float(r[2]):.2f}" if r[2] != "none" else "n/a"
    with open(os.path.join(outdir, "precomp.tsv"), "w") as fh:
        header(fh, [f"meas.py {model} pre ; drive {dv}; per frequency: small-signal gain at {lo_db} dBFS, then the input level (dBFS, bisected, 11 steps) where the fundamental gain is 1 dB below it",
                    "pre_emphasis_rel_db = (lowest such level over all frequencies) - (level at this frequency): 0 at the frequency that clips first, negative where more input is needed", "example render: " + cmd])
        fh.write("freq_hz\tsmall_signal_gain_db\tin_dbfs_at_1db_compression\tpre_emphasis_rel_db\n")
        for r in rows: fh.write("\t".join(str(c) for c in r) + "\n")
    print(f"== pre-clip emphasis at drive {dv} (1 dB compression level per frequency; rel = emphasis relative to the first-clipping frequency)")
    print("   f:    " + " ".join(f"{r[0]:>6}" for r in rows)); print("   P1dB  " + " ".join(f"{r[2]:>6}" for r in rows)); print("   rel   " + " ".join(f"{r[3]:>6}" for r in rows))

def main():
    model = sys.argv[1]; what = sys.argv[2:] or ["thd", "alias", "fr", "lvl", "dyn"]
    outdir = os.path.join(W, "meas", model); os.makedirs(outdir, exist_ok=True)
    if what[0] == "one":
        import argparse
        ap = argparse.ArgumentParser(); ap.add_argument("--set", action="append", default=[]); ap.add_argument("--freq", type=float, default=1000); ap.add_argument("--db", type=float, default=-18)
        a = ap.parse_args(what[1:]); st = {k: float(v) for k, v in (s.split("=") for s in a.set)}
        y, cmd = render(model, st, wav_in("one.wav", sine(a.freq, a.db)), "one"); h, thd, dc = harmonics(y, a.freq)
        print(cmd); print("fund %.2f dB | rel: %s | thd %.2f%% dc %.5f peak %.2f" % (db(h[0]), " ".join(f"{db(v)-db(h[0]):.1f}" for v in h[1:]), thd, dc, db(float(np.max(np.abs(y[-SR:]))))))
        return
    for w in what: {"thd": run_thd, "alias": run_alias, "fr": run_fr, "lvl": run_lvl, "dyn": run_dyn, "pre": run_pre}[w](model, outdir)

if __name__ == "__main__":
    main()
