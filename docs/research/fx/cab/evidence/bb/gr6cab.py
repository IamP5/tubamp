"""Guitar Rig 6 cabinet components, headless black-box (run in $RE_STATE/work/cab/gr6bb).
Setup:  $RE_STATE/bin/auhost state aufx 'Ni$R' -NI- gr6_default.aupreset
        python3 -I ngrc_xml.py "<Component Presets>/<Name>/<Name> Factory.ngrc" ngrc_<Name>     (factory preset documents)
Per row: python3 gr6cabstate.py gr6_default.aupreset <doc.xml> "<component name>" st_<tag>.aupreset <id>=<v>... Rack.G=0
         $RE_STATE/bin/auhost render aufx 'Ni$R' -NI- imp48.wav o_<tag>.wav --state st_<tag>.aupreset --tail 0.5 --warmup 2"""
import os, subprocess, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.environ["RE"]); import sig
RS = os.environ["RE_STATE"]; AUHOST = RS + "/bin/auhost"; AU = ["aufx", "Ni$R", "-NI-"]
TOOLS = os.path.dirname(os.path.abspath(__file__)); BB = RS + "/work/cab/bb"
FREQS = [60, 100, 160, 250, 500, 1000, 2000, 4000, 8000, 12000]
IMP = {44100: ("imp44.wav", 4410), 48000: ("imp48.wav", 4800), 96000: ("imp96.wav", 9600)}
rows = []
def go(tag, doc, name, sets, sr=48000, amp=0.1):
    st = f"st_{tag}.aupreset"
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "gr6cabstate.py"), "gr6_default.aupreset", doc, name, st, *[f"{k}={v}" for k, v in sets.items()], "Rack.G=0"], capture_output=True, text=True)
    if r.returncode: print("STATE FAIL", tag, r.stderr[-200:]); return None
    inp, t0 = IMP[sr]
    r = subprocess.run([AUHOST, "render", *AU, os.path.join(BB, inp), f"o_{tag}.wav", "--state", st, "--tail", "0.5", "--warmup", "2"], capture_output=True, text=True, timeout=600)
    if r.returncode: print("RENDER FAIL", tag, r.stderr[-200:]); return None
    d, fs = sig.read_wav(f"o_{tag}.wav"); h = d[:, t0:] / amp
    pre = float(np.max(np.abs(d[:, :t0]))) / amp
    m = h[0]; pk = float(np.max(np.abs(m))); ipk = int(np.argmax(np.abs(m))); on = int(np.argmax(np.abs(m) > pk * 0.01)) if pk > 0 else -1
    e = np.cumsum(m[::-1] ** 2)[::-1]; t60 = int(np.nonzero(e < e[0] * 1e-6)[0][0]) if e[0] > 0 else 0
    l2 = [10 * np.log10(np.sum(c ** 2) + 1e-30) for c in h]
    H = np.abs(np.fft.rfft(m[:32768], 65536)) ** 2; f = np.fft.rfftfreq(65536, 1 / fs)
    mag = [10 * np.log10(np.mean(H[(f >= x * 2 ** (-1 / 12)) & (f <= x * 2 ** (1 / 12))]) + 1e-30) for x in FREQS]
    row = f"{tag}\t{fs}\t{on}\t{ipk}\t{pk:.4f}\t{l2[0]:.2f}\t{l2[-1]:.2f}\t{t60}\t{pre:.1e}\t" + "\t".join(f"{v:.1f}" for v in mag)
    rows.append(row); print(row, flush=True); np.save(f"h_{tag}.npy", h)
    return h
print("tag\tsr\tonset\tipk\tpeak\tl2L_db\tl2R_db\tt60\tpre_impulse\t" + "\t".join(f"{x}Hz" for x in FREQS))
MC, MCP, CR, CRP, CM = "ngrc_MatchedCabinet_doc0.xml", "ngrc_MatchedCabinetPro_doc0.xml", "ngrc_ControlRoom_doc0.xml", "ngrc_ControlRoomPro_doc0.xml", "ngrc_CabinetsMics_doc0.xml"
which = sys.argv[1]
if which == "mc":
    go("mc_off", MC, "Matched Cabinet", {"Pwr": 0})
    for sr in (44100, 48000, 96000): go(f"mc_c0_{sr}", MC, "Matched Cabinet", {"a": 0, "c": 0}, sr=sr)
    for c in (0.25, 0.5, 0.75, 1): go(f"mc_c{c}", MC, "Matched Cabinet", {"a": 0, "c": c})
    go("mc_air1", MC, "Matched Cabinet", {"a": 1, "c": 0})
    go("mc_stereo", MC, "Matched Cabinet", {"a": 0, "c": 0, "st": 1})
    for cab in (0, 5, 10, 15, 25): go(f"mc_cab{cab}", MC, "Matched Cabinet", {"a": 0, "c": 0, "Cab": cab})
    for mv in (0.5, 1.0): go(f"mc_mv{mv}", MC, "Matched Cabinet", {"a": 0, "c": 0, "MV": mv})
if which == "crp":
    solo1 = {"s1": 1}
    go("crp_init", CRP, "Control Room Pro", {})
    go("crp_s1", CRP, "Control Room Pro", solo1)
    for ph in (0.25, 0.5, 1): go(f"crp_s1_ph{ph}", CRP, "Control Room Pro", dict(solo1, ph1=ph))
    go("crp_s1_inv", CRP, "Control Room Pro", dict(solo1, pi1=1))
    go("crp_s1_g1", CRP, "Control Room Pro", dict(solo1, g1=1))
    go("crp_s1_pan0", CRP, "Control Room Pro", dict(solo1, p1=0, st=1))
    go("crp_s12", CRP, "Control Room Pro", {"s1": 1, "s2": 1})
    go("crp_s1_96k", CRP, "Control Room Pro", solo1, sr=96000)
    go("crp_s1_bass1", CRP, "Control Room Pro", dict(solo1, b=1))
if which == "other":
    go("cr_init", CR, "Control Room", {})
    go("mcp_init", MCP, "Matched Cabinet Pro", {})
    go("mcp_c0", MCP, "Matched Cabinet Pro", {"c": 0}); go("mcp_c05", MCP, "Matched Cabinet Pro", {"c": 0.5}); go("mcp_c1", MCP, "Matched Cabinet Pro", {"c": 1})
    go("cm_init", CM, "Cabinets & Mics", {})
open(f"gr6cab_{which}.tsv", "w").write("\n".join(rows) + "\n")
