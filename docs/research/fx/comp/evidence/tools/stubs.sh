#!/bin/zsh
# stubs.sh <binary> : print "0xaddr name" for every __stubs entry of the arm64 slice (cached)
source ${0:h}/env.sh
b="$1"; key=$(echo "$b" | shasum | cut -c1-12); out=$W/sym/stubs_$key.txt
if [ ! -s $out ]; then
  otool -arch arm64 -Iv "$b" 2>/dev/null | awk '/Indirect symbols for \(__TEXT,__stubs\)/{f=1;next} /Indirect symbols for/{f=0} f && $1 ~ /^0x/ {print $1, $3}' > $out.raw
  # demangle
  awk '{print $2}' $out.raw | c++filt > $out.names
  paste -d' ' <(awk '{print $1}' $out.raw) $out.names > $out
fi
echo $out
