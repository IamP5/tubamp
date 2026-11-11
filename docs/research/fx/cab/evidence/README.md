# Cab study: evidence index

Setup used by every command in these files:

```
export RE_STATE=<shared state dir>      # holds bin/auhost, thin/, symbols/, work/cab/
export RE=<repo>/tools/re
C=/Library/Audio/Plug-Ins/Components
GR="$C/Guitar Rig 6.component/Contents/MacOS/Guitar Rig 6"
AT="$C/AmpliTube 5.component/Contents/MacOS/AmpliTube 5"
T3="$C/TONE3000.component/Contents/MacOS/TONE3000"
```

| file | content |
| --- | --- |
| `gr6_convolution.md` | Guitar Rig 6 Convolver, ConvolutionBase, ConvolutionCabBase, Air, impedance hand-off (decompiled) |
| `gr6_cab_mixers.md` | Guitar Rig 6 Control Room Pro / IR Loader mixing, Matched Cabinet Pro, RigCabinet (decompiled) |
| `gr6_blackbox.tsv` | Guitar Rig 6 cabinet components measured headless |
| `at5_cab_notes.txt` | AmpliTube 5 cabinet, helper's reading notes with addresses (decompiled) |
| `at5_impedance.md` | AmpliTube 5 speaker impedance module (decompiled) |
| `tone3000_ir.md` | TONE3000 IR block (decompiled) |
| `tonex_strings.md`, `tonex_cab_params.tsv` | TONEX strings and AU parameters |
| `tonex_rates.tsv`, `tonex_mic.tsv`, `tonex_res.tsv`, `tonex_res_diff.tsv`, `tonex_cabs.tsv`, `tonex_blend.txt` | TONEX VIR cab measured |
| `pa_blackbox.tsv` | Ampeg SVTVR Classic and bx_rockrack V3 Player (amp in the path) |
| `ours_level.tsv`, `ours_default_ir_transient.tsv`, `ours_resampler.tsv`, `ours_resampler_bright.tsv`, `ours_cuts.tsv`, `ours_cpu.tsv` | tubamp's own cab measured through the installed AU |
| `proto_norm.tsv`, `proto_align.tsv`, `proto_load.tsv`, `proto_resample.tsv`, `proto/cabproto.py` | numeric prototypes of the proposed designs |
| `literature.md` | sources opened, and what was not readable |
| `bb/` | scripts: `bb.py`, `tonex4.py`, `tonex_analysis.py` (TONEX); `pa.py`; `ours.py`, `make_irs.py`, `t1.py`, `t2.py`, `ours_cuts.py` (tubamp); `gr6cabstate.py`, `ngrc_xml.py`, `gr6cab.py`, `gr6cab_analysis.py` (Guitar Rig, state tools copied from the tremolo study); `readints.py`, `readstr.py`, `xref.py` (binary readers) |

Working files (WAVs, states, dumps) are in `$RE_STATE/work/cab/{bb,ours,gr6bb,gr6,at5,t3k,tonex,lit}`.
