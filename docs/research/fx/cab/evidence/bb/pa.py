"""Plugin Alliance black-box: Ampeg SVTVR Classic recording chains, bx_rockrack V3 Player presets."""
import bb, numpy as np, os
T0, AMP = 4800, 0.1
rows = []; H = {}
def go(name, au, sets, amp=AMP, inp="imp48.wav"):
    cmd = bb.render(au, inp, f"pa_{name}.wav", sets, ["--tail", "0.5"])
    d, sr = bb.load(f"pa_{name}.wav")
    a = bb.analyse(d[0], sr, T0, amp)
    h = d[0][T0:] / amp; H[name] = h
    e = h ** 2; cen = float(np.sum(np.arange(len(h)) * e) / max(np.sum(e), 1e-30))
    lr = float(np.max(np.abs(d[0] - d[1]))) if d.shape[0] > 1 else 0
    r = dict(name=name, onset=a["onset"], ipk=a["ipk"], peak=round(float(a["peak"]), 4), l2_db=round(20 * np.log10(a["l2"] + 1e-12), 2), t30=a["t30"], t60=a["t60"], centroid=round(cen, 1), lr=round(lr, 6))
    rows.append((r, bb.mag(h, sr, bb.FREQS))); print(r, flush=True)
AMV = ["aumf", "AMVC", "Brwx"]
for rc in [0, 1, 2]:
    go(f"ampeg_recchain{rc}", AMV, {4: rc})
go("ampeg_recchain0_lowlevel", AMV, {4: 0}, amp=0.001, inp="imp48_m60.wav")
BXR = ["aufx", "bxrp", "Brwx"]
for p in [4, 5, 24]:
    go(f"rockrack_preset{p}", BXR, {2: p})
with open(os.path.join(bb.HERE, "pa.tsv"), "w") as fh:
    keys = list(rows[0][0].keys()); fh.write("\t".join(keys + [f"{f}Hz" for f in bb.FREQS]) + "\n")
    for r, m in rows: fh.write("\t".join(str(r[k]) for k in keys) + "\t" + "\t".join(f"{v:.1f}" for v in m) + "\n")
# recording-chain IR = chain0 / chain2 (deconvolution in frequency domain)
n = 1 << 16
A = np.fft.rfft(H["ampeg_recchain0"][:n], n); B = np.fft.rfft(H["ampeg_recchain2"][:n], n)
f = np.fft.rfftfreq(n, 1 / 48000)
G = A / (B + 1e-9 * np.max(abs(B)))
print("cab/empty ratio dB:", [(fc, round(20 * np.log10(abs(G[np.argmin(abs(f - fc))])), 1)) for fc in bb.FREQS])
lin = np.max(np.abs(H["ampeg_recchain0"][:4096] - H["ampeg_recchain0_lowlevel"][:4096])) / np.max(np.abs(H["ampeg_recchain0"]))
print("level linearity residual (rel peak) between -20 dB and -60 dB impulses:", lin)
