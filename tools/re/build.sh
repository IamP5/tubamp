#!/bin/zsh
# Builds the headless AU host into $RE_STATE/bin/auhost (or ./build-re/auhost).
set -eu
here=${0:A:h}
out=${RE_STATE:-$here/../../build-re}/bin
mkdir -p "$out"
# Built beside the target and renamed over it, so a render that is running keeps its
# own copy of the old binary.
xcrun clang++ -std=c++17 -fobjc-arc -O2 -Wall -Wno-deprecated-declarations \
  "$here/auhost.mm" -o "$out/auhost.new" \
  -framework AudioToolbox -framework AudioUnit -framework CoreAudio \
  -framework CoreFoundation -framework Cocoa
codesign -s - -f "$out/auhost.new" 2>/dev/null
mv -f "$out/auhost.new" "$out/auhost"
print "$out/auhost"
