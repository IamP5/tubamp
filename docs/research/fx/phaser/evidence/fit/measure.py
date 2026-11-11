#!/usr/bin/env python3
"""measure.py: render the Guitar Rig 6 takes and write targets.tsv.

    export RE_STATE=...  W=$RE_STATE/work/phaser-fix
    $RE_STATE/bin/auhost state aufx 'Ni$R' -NI- $W/gr6_default.aupreset      (once)
    python3 measure.py probes            # track.py gen $W/probe{3,4,6,11,14,16} --secs N
    python3 measure.py render            # render_gr6.sh for every take into $W/gr/<tag>
    python3 measure.py analyse           # track.py on every take -> targets.tsv (this dir)
    python3 measure.py run               # render + analyse one take at a time, deleting each take's
                                         # WAVs once its row is written (disk-friendly; same result)
    python3 measure.py color             # Color on only: re-render the three ss_on takes, the Rate-0
                                         # quasi-static take and the six static frames (static.py), and
                                         # rewrite those rows of targets.tsv, keeping every other row
Takes are listed in TAKES (multitone) and QUASI/STATIC (periodic probe) below; the exact commands
are copied into the targets.tsv header.
"""
import os, shutil, subprocess, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import track  # noqa: E402

W = os.environ.get("W") or os.path.join(os.environ.get("RE_STATE", "."), "work", "phaser-fix")
# tag, reference, component, probe secs, overrides, analysis band low (Hz), gain (dB)
P9 = "GR6 Phaser Nine (n=4 r=0.5 e=0.5 x=0.5 o=0.5)"
SS = "GR6 Stoned Phaser (Not=2 CS=0.74 DW=0.5 F1=0.5 F2=0.5)"
TAKES = [
    ("p9_block_p010", P9 + " Color 0.4217 Rate 0.10", "p9", 16, "p=0.1", 300, -5.40),
    ("p9_block_p025", P9 + " Color 0.4217 Rate 0.25", "p9", 14, "p=0.25", 300, -5.40),
    ("p9_block_p050", P9 + " Color 0.4217 Rate 0.50 (defaults)", "p9", 6, "p=0.5", 300, -5.40),
    ("p9_block_p075", P9 + " Color 0.4217 Rate 0.75", "p9", 4, "p=0.75", 300, -5.40),
    ("p9_block_p090", P9 + " Color 0.4217 Rate 0.90", "p9", 3, "p=0.9", 300, -5.40),
    ("p9_block_p100", P9 + " Color 0.4217 Rate 1.00 (fastest)", "p9", 3, "p=1.0", 300, -5.40),
    ("p9_c0_p010", P9 + " Color 0 Rate 0.10", "p9", 16, "p=0.1 c=0", 300, -5.40),
    ("p9_c0_p050", P9 + " Color 0 Rate 0.50", "p9", 6, "p=0.5 c=0", 300, -5.40),
    ("p9_c0_p100", P9 + " Color 0 Rate 1.00", "p9", 3, "p=1.0 c=0", 300, -5.40),
    ("ss_off_r040", SS + " Color off Rate 0.40", "ss", 14, "Rt=0.4", 60, 0.0),
    ("ss_off_r050", SS + " Color off Rate 0.50 (defaults)", "ss", 11, "Rt=0.5", 60, 0.0),
    ("ss_off_r075", SS + " Color off Rate 0.75", "ss", 4, "Rt=0.75", 60, 0.0),
    ("ss_on_r040", SS + " Color on Rate 0.40", "ss", 14, "Rt=0.4 Cl=1", 60, 0.0),
    ("ss_on_r050", SS + " Color on Rate 0.50", "ss", 11, "Rt=0.5 Cl=1", 60, 0.0),
    ("ss_on_r075", SS + " Color on Rate 0.75", "ss", 4, "Rt=0.75 Cl=1", 60, 0.0),
]
# Periodic-probe takes (static.py), Stoned Phaser at Rate 0 = 0.01 Hz: one 212 s render each ($W/pprobe.wav).
# QUASI: quasi-static notch trajectory over one LFO cycle (row in the main table).
# STATIC: |H| at the sweep turning points (t = 50 s top, 100 s bottom) of three sweep windows set by
# Sweep Min/Max (F1/F2); a window narrower than the stock one moves the break frequency more slowly,
# so each 8192-sample frame sees it change by at most 0.45 %.
QUASI = ("ss_on_r000", SS + " Color on Rate 0 (0.01 Hz)", "Rt=0 Cl=1")
STATIC = [("ss_on_full", "F1=0.5 F2=0.5"), ("ss_on_mid", "F1=0.75 F2=0.25"), ("ss_on_narrow", "F1=1 F2=0")]
STATIC_T = (("bot", 100.0), ("top", 50.0))
COLS = ["tag", "n1_min_hz", "n1_max_hz", "n2_min_hz", "n2_max_hz", "period_s", "rate_hz", "depth_db",
        "peak_db", "peak_max_db", "cycles", "frames", "gain_removed_db", "reference"]


def probes():
    for s in sorted({t[3] for t in TAKES}):
        subprocess.run([sys.executable, os.path.join(HERE, "track.py"), "gen", f"{W}/probe{s}", "--secs", str(s)], check=True)


def render():
    for tag, _, comp, secs, ov, _, _ in TAKES:
        subprocess.run([os.path.join(HERE, "render_gr6.sh"), comp, f"{W}/probe{secs}", f"{W}/gr/{tag}"] + ov.split(), check=True)


def analyse(out=os.path.join(HERE, "targets.tsv"), render_first=False):
    lines = ["# Guitar Rig 6 notch trajectories, 48 kHz, measured with evidence/fit/track.py (multitone, per-cycle extremes, median over cycles).",
             "# Reproduce:  export RE_STATE=<scratch>/re W=$RE_STATE/work/phaser-fix",
             "#   $RE_STATE/bin/auhost state aufx 'Ni$R' -NI- $W/gr6_default.aupreset",
             "#   python3 evidence/fit/measure.py probes && python3 evidence/fit/measure.py run   (or: render, then analyse)",
             "# which runs, per take:",
             "#   track.py gen $W/probe<secs> --secs <secs>      (268 tones, 60 Hz-12 kHz, ratio 1.02, 36 files, -36 dBFS each)",
             "#   render_gr6.sh <p9|ss> $W/probe<secs> $W/gr/<tag> <overrides>   (grstate.py + auhost render aufx 'Ni$R' -NI- per file)",
             "#   track.py ana $W/probe<secs> $W/gr/<tag> --lo <lo> --gain-db <gain>   (q 8, hop 16, skip 1 s, prom 3 dB)",
             "# Takes (tag: secs, overrides, lo, gain):"]
    for tag, ref, comp, secs, ov, lo, g in TAKES:
        lines.append(f"#   {tag}: {comp} {secs} s, {ov}, lo {lo}, gain {g}")
    lines += ['# Color-on additions (python3 measure.py color, after analyse): the three ss_on rows re-rendered (identical values),', '#   ss_on_r000 = Rate 0 (0.01 Hz) quasi-static take through the periodic probe $W/pprobe.wav (static.py gen --secs 212),', '#   analysed with static.py (5-frame average, 1.01 log grid, same notch rules), and the `static` block at the end:', '#   |H| of six frozen frames (sweep bottom/top of three Sweep Min/Max windows) used by fit.py for the Color-on structure.']
    lines += ["# Columns: depth_db = median notch-1 level; peak_db = median over frames of the highest |H| in band; peak_max_db = its maximum.",
              "# Phaser Nine levels have its x1.862 (+5.40 dB) input make-up removed (gain_removed_db). Stoned Phaser notch 2 leaves the",
              "# 12 kHz probe band at the sweep top, so its n2 columns are not sweep extremes. Stoned Phaser Color on: notch 1 loses",
              "# prominence (< 3 dB) at the sweep bottom, so n1_min is the last resolved value; the period comes from notch 2 there.",
              "\t".join(COLS)]
    for tag, ref, comp, secs, ov, lo, g in TAKES:
        if render_first:
            subprocess.run([os.path.join(HERE, "render_gr6.sh"), comp, f"{W}/probe{secs}", f"{W}/gr/{tag}"] + ov.split(), check=True)
        f, step, t, H, _ = track.H_from_dirs(f"{W}/probe{secs}", f"{W}/gr/{tag}", 8, 16, 0, g)
        r = track.summarise(t, f, step, H, lo, 12000, 3.0, 1.0)
        n1, n2 = r["n1"], r["n2"]; per = track.best_period(r)
        row = [tag, f"{n1['min']:.1f}", f"{n1['max']:.1f}", f"{n2['min']:.0f}" if n2 else "nan", f"{n2['max']:.0f}" if n2 else "nan",
               f"{per:.4f}", f"{1/per:.4f}", f"{n1['depth']:.2f}", f"{r['peak_db']:.2f}", f"{r['peak_max_db']:.2f}",
               str(n1["cycles"]), f"{n1['frames']}/{r['nframes']}", f"{g:.2f}", ref]
        lines.append("\t".join(row)); print("\t".join(row[:12]), flush=True)
        if render_first:
            shutil.copy2(f"{W}/gr/{tag}/state.aupreset", f"{W}/st_{tag}.aupreset"); shutil.rmtree(f"{W}/gr/{tag}")
    open(out, "w").write("\n".join(lines) + "\n")


def gr_periodic(probe, out, overrides):
    """Render the periodic probe through a Stoned Phaser state (render_gr6.sh base parameters)."""
    st = out.replace(".wav", ".aupreset")
    base = "Sy=0 Not=2 Rt=0.5 Cl=0 CS=0.74 DW=0.5 F1=0.5 F2=0.5 Rot=0 In=0".split()
    subprocess.run([sys.executable, os.path.join(HERE, "..", "tools", "grstate.py"), f"{W}/gr6_default.aupreset",
                    os.path.join(HERE, "..", "tools", "comp_Stoned_Phaser.xml"), st] + base + overrides.split(), check=True)
    subprocess.run([os.path.join(os.environ["RE_STATE"], "bin", "auhost"), "render", "aufx", "Ni$R", "-NI-", probe, out,
                    "--state", st], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def quasi_row(f, t, H, ref):
    """Main-table row from the Rate-0 take (212 s: two upward crossings, so one full cycle)."""
    import static
    r = static.summary(f, t, H); n1, n2 = r["n1"], r["n2"]
    per = track.best_period(static.summary(f, t, H, avg=1))   # crossings need the unaveraged track
    return [QUASI[0], f"{n1['min']:.1f}", f"{n1['max']:.1f}", f"{n2['min']:.0f}", f"{n2['max']:.0f}", f"{per:.3f}", f"{1/per:.5f}",
            f"{n1['depth']:.2f}", f"{r['peak_db']:.2f}", f"{r['peak_max_db']:.2f}", str(n1["cycles"]), f"{n1['frames']}/{r['nframes']}",
            "0.00", ref + " [static.py: periodic probe, 5-frame average, 1.01 log grid, same notch rules; period from the unaveraged track]"]


def color():
    """Re-measure Colour on: three multitone takes, the quasi-static take, six static frames."""
    import static
    path = os.path.join(HERE, "targets.tsv"); old = open(path).read().split("\n")
    rows = {}
    for tag, ref, comp, secs, ov, lo, g in TAKES:
        if not tag.startswith("ss_on"): continue
        subprocess.run([os.path.join(HERE, "render_gr6.sh"), comp, f"{W}/probe{secs}", f"{W}/gr/{tag}"] + ov.split(), check=True)
        f, step, t, H, _ = track.H_from_dirs(f"{W}/probe{secs}", f"{W}/gr/{tag}", 8, 16, 0, g)
        r = track.summarise(t, f, step, H, lo, 12000, 3.0, 1.0); n1, n2 = r["n1"], r["n2"]; per = track.best_period(r)
        rows[tag] = [tag, f"{n1['min']:.1f}", f"{n1['max']:.1f}", f"{n2['min']:.0f}", f"{n2['max']:.0f}", f"{per:.4f}", f"{1/per:.4f}",
                     f"{n1['depth']:.2f}", f"{r['peak_db']:.2f}", f"{r['peak_max_db']:.2f}", str(n1["cycles"]),
                     f"{n1['frames']}/{r['nframes']}", f"{g:.2f}", ref]
        print("\t".join(rows[tag][:12]), flush=True); shutil.rmtree(f"{W}/gr/{tag}")
    probe = f"{W}/pprobe.wav"
    if not os.path.exists(probe): static.gen(probe, 212)
    os.makedirs(f"{W}/st", exist_ok=True)
    out = f"{W}/st/{QUASI[0]}.wav"; gr_periodic(probe, out, QUASI[2])
    f, t, H = static.frames(probe, out); os.remove(out)
    rows[QUASI[0]] = quasi_row(f, t, H, QUASI[1]); print("\t".join(rows[QUASI[0]][:12]), flush=True)
    srows = []
    for name, ov in STATIC:
        out = f"{W}/st/{name}.wav"; gr_periodic(probe, out, "Rt=0 Cl=1 " + ov)
        f, t, H = static.frames(probe, out); os.remove(out)
        for pos, T in STATIC_T:
            d, tt = static.snap(f, t, H, T)
            srows.append("\t".join(["static", f"{name}_{pos}", f"{tt:.3f}", ov.replace(" ", ",")] + [f"{v:.2f}" for v in d]))
    # rewrite: keep header and other rows, replace Colour-on rows, add the quasi-static row and the static block
    main, seen = [], set()
    for line in old:
        if not line or line.startswith("static") or line.startswith("# static"): continue
        tag = line.split("\t")[0]
        if tag in rows: main.append("\t".join(rows[tag])); seen.add(tag)
        else: main.append(line)
    for tag in rows:
        if tag not in seen: main.append("\t".join(rows[tag]))
    import static as S
    blk = ["# static: Stoned Phaser Color on (CS 0.74, DW 0.5), Rate 0, periodic probe (static.py gen, 8192-sample period).",
           "# Columns: static, frame, frame centre time (s), sweep window (F1,F2), |H| in dB at the frequencies of the static_hz row.",
           "# Frames sit on the LFO turning points: t 100 s = sweep bottom, 50 s = sweep top. Reproduce: python3 measure.py color.",
           "static_hz\t\t\t\t" + "\t".join(f"{x:g}" for x in S.STATIC_HZ)]
    open(path, "w").write("\n".join(main + blk + srows) + "\n")


if __name__ == "__main__":
    {"probes": probes, "render": render, "analyse": analyse, "run": lambda: analyse(render_first=True), "color": color}[sys.argv[1]]()
