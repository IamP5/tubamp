import ours, numpy as np, os
BB = os.path.join(ours.HERE, "..", "bb")
imp = {44100: (os.path.join(BB, "imp44.wav"), 4410), 48000: (os.path.join(BB, "imp48.wav"), 4800), 96000: (os.path.join(BB, "imp96.wav"), 9600)}
AMP = 0.1
def run(name, ir, sr=48000, **kw):
    st = ours.make_state(name, os.path.join(ours.HERE, ir), **kw)
    dt, cmd = ours.render(st, imp[sr][0], f"o_{name}.wav", warmup=60)
    d, r = ours.load(f"o_{name}.wav"); t0 = imp[sr][1]
    h = d[:, t0:] / AMP
    pre = float(np.max(np.abs(d[:, :t0]))) / AMP
    return h, r, pre, cmd
rows = []
def rec(name, h, sr, pre, extra=""):
    l2 = [20 * np.log10(np.sqrt(np.sum(c ** 2)) + 1e-12) for c in h]
    pk = [float(np.max(np.abs(c))) for c in h]; ipk = [int(np.argmax(np.abs(c))) for c in h]
    H = np.fft.rfft(h[0][:32768], 65536); f = np.fft.rfftfreq(65536, 1 / sr)
    g1k = 20 * np.log10(abs(H[np.argmin(abs(f - 1000))]) + 1e-12)
    row = f"{name}\t{sr}\t{ipk[0]}\t{pk[0]:.5f}\t{l2[0]:.2f}\t{l2[-1]:.2f}\t{g1k:.2f}\t{pre:.2e}\t{extra}"
    rows.append(row); print(row, flush=True)
print("name\tsr\tipk\tpeak\tl2L_db\tl2R_db\tgain_1k_db\tpre_impulse\tnote")
h, sr, pre, cmd = run("delta", "ir_delta48.wav"); rec("delta_ir", h, sr, pre, "unit impulse IR, cuts 20 Hz / 20 kHz")
h, sr, pre, _ = run("delta_defcuts", "ir_delta48.wav", lowcut=80.0, highcut=8000.0); rec("delta_ir_default_cuts", h, sr, pre, "cuts 80 Hz / 8 kHz")
np.save("h_delta_defcuts.npy", h[0])
h, sr, pre, _ = run("stereo", "ir_stereo_delta.wav"); rec("stereo_delta", h, sr, pre, "L=1.0, R=0.5 impulse IR")
h, sr, pre, _ = run("quiet", "ir_cab48_quiet.wav"); rec("cab48_quiet(-40dB file)", h, sr, pre, "same IR scaled -40 dB in the file")
ref = {}
for ir in ("ir_cab44.wav", "ir_cab48.wav", "ir_cab96.wav", "ir_cab48_predelay.wav"):
    for sr in (44100, 48000, 96000):
        if ir == "ir_cab48_predelay.wav" and sr != 48000: continue
        nm = f"{ir[3:-4]}_s{sr}"
        h, r, pre, _ = run(nm, ir, sr=sr); rec(nm, h, r, pre); np.save(f"h_{nm}.npy", h[0])
open("ours_level.tsv", "w").write("# " + cmd + "\n# python3 t1.py (state patched by ours.make_state: chainOrder=cab, irPath, cab_lowcut=20, cab_highcut=20000)\nname\tsr\tipk\tpeak\tl2L_db\tl2R_db\tgain_1k_db\tpre_impulse\tnote\n" + "\n".join(rows) + "\n")
