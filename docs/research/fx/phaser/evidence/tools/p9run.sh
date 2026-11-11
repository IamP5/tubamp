#!/bin/zsh
# p9run.sh <tag> <probe.wav> [id=value ...] : build a Phaser Nine state, render, track, summarise
tag=$1; probe=$2; shift 2
python3 $W/tools/grstate.py gr6_default.aupreset presets/comp_Phaser_Nine.xml st_$tag.aupreset n=4 c=0.4217 r=0.5 e=0.5 p=0.5 x=0.5 o=0.5 i=0 s=0 "$@" || exit 1
$RE_STATE/bin/auhost render aufx 'Ni$R' '-NI-' $probe out_$tag.wav --state st_$tag.aupreset 2>&1 | tail -1 | grep -v rendered
python3 $W/tools/ptrack.py track $probe out_$tag.wav ${=TRACKARGS} > trk_$tag.tsv
echo "== $tag $@"; python3 $W/tools/ptrack.py summary trk_$tag.tsv --n ${NN:-2} | cut -f1-13,22-
