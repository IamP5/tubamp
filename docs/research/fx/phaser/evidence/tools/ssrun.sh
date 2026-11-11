#!/bin/zsh
# ssrun.sh <tag> <probe.wav> [id=value ...] : build a Stoned Phaser (RigPhaserBT) state, render, track, summarise
tag=$1; probe=$2; shift 2
python3 $W/tools/grstate.py gr6_default.aupreset presets/comp_Stoned_Phaser.xml st_$tag.aupreset Sy=0 Not=2 Rt=0.5 Cl=0 CS=0.74 DW=0.5 F1=0.5 F2=0.5 Rot=0 In=0 "$@" || exit 1
$RE_STATE/bin/auhost render aufx 'Ni$R' '-NI-' $probe out_$tag.wav --state st_$tag.aupreset 2>&1 | tail -1 | grep -v rendered
python3 $W/tools/ptrack.py track $probe out_$tag.wav ${=TRACKARGS} > trk_$tag.tsv
echo "== $tag $@"; cut -f2 trk_$tag.tsv | sort | uniq -c | sort -rn | head -3 | tr '\n' ' '; echo; python3 $W/tools/ptrack.py summary trk_$tag.tsv --n ${NN:-2} | cut -f1-13,22-
