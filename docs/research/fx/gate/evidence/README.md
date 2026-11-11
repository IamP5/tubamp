# Evidence index (noise gate)

Variables used in recheck commands:
```
export RE_STATE=<shared state dir>; export RE=<repo>/tools/re
C=/Library/Audio/Plug-Ins/Components
GR="$C/Guitar Rig 6.component/Contents/MacOS/Guitar Rig 6"
AT="$C/AmpliTube 5.component/Contents/MacOS/AmpliTube 5"
SD="$C/Solid Dynamics.component/Contents/MacOS/Solid Dynamics"
GW="$C/Gateway.component/Contents/MacOS/Gateway"
T3="$C/TONE3000.component/Contents/MacOS/TONE3000"
A=/Users/tuba/Dev/projects/tubamp/build/_deps/neuralampmodelercore-src/Dependencies/AudioDSPTools/dsp
```
| file | content |
| --- | --- |
| gr6_noisegate.md, gr6_noisereduction.md | Guitar Rig 6 decompiled notes |
| at5_gates.md | AmpliTube 5 Noise Gate, MXR Smart Gate model, Hush IIB model |
| solid_dynamics_ge.md | Solid Dynamics gate/expander |
| gateway_gate.md | Gateway = AudioDSPTools gate |
| tone3000_gate.md | TONE3000 gate |
| blackbox_results.txt | every measurement, with the command that made it |
| bb/gen.py | test signals (run `python3 gen.py sig $RE` inside bb/, after `mkdir -p sig out`) |
| bb/an.py | analyses: static, thr, burst, flips, gaps, sus |
| bb/run.sh | renders all signals through one plugin (`./run.sh <tag> <type> <subtype> <manu> [--set ...]`), `ours:<thr>` runs the harness, `ext:<tag>` analyses existing files. Edit its RE_STATE line if the state directory moved |
| bb/ours.cpp | harness around the real AudioDSPTools gate with tubamp's constants |
| bb/proto.py | prototype of the proposed gate and a model of the TONE3000 gate |
| bb/stepdown.py | step-down test for static hysteresis |

Gateway has no symbols; its functions were read with
`objdump -d --no-show-raw-insn --start-address=0xaaf38 --stop-address=0xab290 $RE_STATE/thin/*Gateway`.
WAV files and full disassembly dumps are in `$RE_STATE/work/gate/`.
