#!/bin/zsh
# mk.sh <tag> <doc.xml> <component name> [id=value ...] : write $W/gr6/st_<tag>.aupreset (noise gate of the rack off)
source ${0:h}/../env.sh
tag=$1; doc=$2; name=$3; shift 3
python3 $W/gr6/gr6state.py $W/gr6/gr6_default.aupreset $W/gr6/$doc "$name" $W/gr6/st_$tag.aupreset "$@" Rack.G=0 >/dev/null 2>$W/gr6/st_$tag.err || echo "state failed $tag: $(tail -1 $W/gr6/st_$tag.err)"
