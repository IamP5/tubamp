Evidence tools for the drive study. Everything runs from a scratch directory $W (see env.example.sh).

rigcall.cpp     in-process class harness: dlopen a plugin binary, call DSP class methods by vm address, render a WAV.
                Build: clang++ -std=c++17 -O1 -o $W/tools/rigcall rigcall.cpp
meas.py         measurement battery (thd, alias, fr, lvl, dyn, pre, one); models.py defines every model.
summ.py         one summary row per model.   extra.py  latency, idle noise, cost.
ann.py          annotated arm64 disassembly (literal pools and float immediates resolved).
vt.py, cstrs.py, xref.py, tabstat.py, at5_params.py   small static helpers (see their docstrings).
alias_schemes.py, ts_proto.py   numerical prototypes behind Tables 6 and 7 of the report.
gr6state.py, ngrc_xml.py        copied from docs/research/fx/tremolo/evidence/tools (tremolo study): author a Guitar Rig 6
                AU state holding one rack component from the plugin's own factory component preset.
gr6au/          the factory INIT documents cut out of '<Name> Factory.ngrc' by ngrc_xml.py (parameter ids and values only).
ours_drive_only.aupreset   tubamp state whose chain is the drive block alone.

gr6au_* models need, in $W/gr6au/: gr6_default.aupreset ($RE_STATE/bin/auhost state aufx 'Ni$R' -NI- gr6_default.aupreset)
and the ngrc_*_doc0.xml files (copy from gr6au/ here, or regenerate with ngrc_xml.py).
