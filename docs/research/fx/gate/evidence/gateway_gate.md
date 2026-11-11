# Gateway (Atkinson Advanced Modeling) — gate (decompiled + measured)

Binary: /Library/Audio/Plug-Ins/Components/Gateway.component/Contents/MacOS/Gateway (arm64, no symbols).
RTTI strings `N3dsp10noise_gate7TriggerE` and `N3dsp10noise_gate4GainE` are present
(`python3 $RE/disasm.py "$GW" --strings noise_gate`): it links AudioDSPTools.

dsp::noise_gate::Trigger::Process is the function at 0xaaf38 (the only caller of _log10,
at 0xab0c0). Read with `objdump -d --no-show-raw-insn --start-address=0xaaf38 --stop-address=0xab290 <thin slice>`.
- alpha = exp2(-1/(time*sr)) 0xaaf8c..0xaafa0 (the compiler's form of pow(0.5, 1/(time*sr)))
- clamp of the smoothed power to [1e-12, 1000] 0xab028..0xab0b8, level = 10*log10 0xab0c0
- max reduction -ratio*(-120 - thr)^2 0xaafac..0xaafd0, quadratic law 0xab110..0xab12c
- 0.5*(target - last) clamped by the open / close slew 0xab140..0xab1c0, HOLDING state 0xab0c8..0xab10c
dsp::noise_gate::Gain::Process is 0xab6ec: out = exp10(GR/10) * in (0xab7a0..0xab7b4).
Both match build/_deps/.../AudioDSPTools/dsp/NoiseGate.cpp line for line.

Call site 0xc671c..0xc6760 (inside _Gateway_Factory+...): time = 0.01 (immediate),
{ratio, open} = {0.1, 0.005} from literal 0xedd80, {hold, close} = {0.01, 0.05} from
literal 0xedd90, then Trigger::Process on the pre-model buffer; Gain::Process is called at
0xc682c on the post-model buffer. Same constants and same pre/post split as tubamp.

Measured (bb/res_gw.txt, command `./run.sh gw aufx 72RG AAMa --set 1=-40 --set 7=0 --set 8=0 --set 11=0`):
static curve identical to the tubamp harness to 0.1 dB at every level
(-40:-1.6 -45:-12.4 -50:-33.1 -55:-63.9 dB), opening identical (-1 dB after 1.2 ms).
The only difference is a residue below -90 dBFS after the gate shuts that decays with
tau = 31.8 ms, which is the 5 Hz DC blocker after the gate (1/(2*pi*5) = 31.8 ms) [inferred].
