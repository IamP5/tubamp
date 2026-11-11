#!/bin/zsh
# render_gr6.sh <component: p9|ss> <probe dir> <out dir> [id=value ...]
# Builds a Guitar Rig 6 state holding one rack component (evidence/tools/grstate.py) on top of
# $W/gr6_default.aupreset (made once by: auhost state aufx 'Ni$R' -NI- gr6_default.aupreset),
# then renders every tone_XX.wav of <probe dir> into <out dir>, 6 renders in parallel, each
# under a 180 s alarm, and re-renders serially any file that came back with NaN.  Phaser Nine base: n=4 c=0.4217 r=0.5 e=0.5 p=0.5 x=0.5 o=0.5 i=0 s=0.
# Stoned Phaser base: Sy=0 Not=2 Rt=0.5 Cl=0 CS=0.74 DW=0.5 F1=0.5 F2=0.5 Rot=0 In=0.
comp=$1; inp=$2; out=$3; shift 3
F=${0:A:h}/..
mkdir -p $out
if [[ $comp == p9 ]]; then
  python3 $F/tools/grstate.py $W/gr6_default.aupreset $F/tools/comp_Phaser_Nine.xml $out/state.aupreset n=4 c=0.4217 r=0.5 e=0.5 p=0.5 x=0.5 o=0.5 i=0 s=0 "$@" || exit 1
else
  python3 $F/tools/grstate.py $W/gr6_default.aupreset $F/tools/comp_Stoned_Phaser.xml $out/state.aupreset Sy=0 Not=2 Rt=0.5 Cl=0 CS=0.74 DW=0.5 F1=0.5 F2=0.5 Rot=0 In=0 "$@" || exit 1
fi
for f in $inp/tone_*.wav; do
  b=${f:t}
  ( perl -e 'alarm 180; exec @ARGV' -- $RE_STATE/bin/auhost render aufx 'Ni$R' -NI- $f $out/$b --state $out/state.aupreset >/dev/null 2>&1 || echo "FAIL $b" ) &
  while (( $(jobs -r | wc -l) >= 6 )); do sleep 0.2; done
done
wait
# Parallel Guitar Rig 6 instances occasionally emit NaN blocks (not reproducible serially):
# re-render any file with NaN, Inf or a wrong length, one at a time, up to three passes.
for pass in 1 2 3; do
  bad=($(python3 $F/fit/track.py check $inp $out))
  (( ${#bad} == 0 )) && break
  echo "re-rendering ${#bad} file(s): $bad" >&2
  for b in $bad; do
    perl -e 'alarm 180; exec @ARGV' -- $RE_STATE/bin/auhost render aufx 'Ni$R' -NI- $inp/$b $out/$b --state $out/state.aupreset >/dev/null 2>&1
  done
done
