import ours, numpy as np, os
inp = os.path.join(ours.HERE, "noise120.wav"); AUDIO = 120 + 30
rows = ["# CPU of tubamp's cab block (juce::dsp::Convolution, zero-latency uniform partitions) against IR length and host block size",
        "# auhost render aufx Tamp Tuba noise120.wav o.wav --state st_cpu_*.aupreset --block B --tail 0 --warmup 30 ; wall time minus the cab-off baseline, / 150 s of audio",
        "# python3 t2.py in $RE_STATE/work/cab/ours (Apple Silicon, offline render, both channels)",
        "ir_file\tir_samples\tblock\twall_s\tbaseline_s\tcpu_percent_of_realtime"]
def t(state, block):
    best = 1e9
    for _ in range(3):
        dt, _ = ours.render(state, inp, "o_cpu.wav", block=block, tail=0, warmup=30); best = min(best, dt)
    return best
base = {}
st0 = ours.make_state("cpu_off", os.path.join(ours.HERE, "ir_delta48.wav"), cab_on=0.0)
for b in (32, 64, 128, 512, 1024):
    base[b] = t(st0, b)
for ir, n in (("ir_delta48.wav", 1), ("ir_cab48.wav", 2229), ("ir_noise_0.05.wav", 2400), ("ir_noise_0.2.wav", 9600), ("ir_noise_0.5.wav", 24000), ("ir_noise_1.0.wav", 48000), ("ir_noise_2.0.wav", 96000)):
    st = ours.make_state("cpu_" + ir[3:-4], os.path.join(ours.HERE, ir))
    for b in (32, 64, 128, 512, 1024):
        w = t(st, b); cpu = 100 * (w - base[b]) / AUDIO
        rows.append(f"{ir}\t{n}\t{b}\t{w:.3f}\t{base[b]:.3f}\t{cpu:.3f}"); print(rows[-1], flush=True)
open("ours_cpu.tsv", "w").write("\n".join(rows) + "\n")
