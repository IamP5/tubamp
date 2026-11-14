#!/usr/bin/env bash
# Merges the arm64 and x86_64 tubamp.component into one universal bundle and zips it the
# way the in-plugin updater requires (docs/UPDATES.md):
#   scripts/release/build-update-archive.sh <X.Y.Z> <arm64 tubamp.component> <x86_64 tubamp.component> [out dir]
# Writes <out dir>/tubamp-<version>-macos-universal.zip (default dist/). Every Mach-O file
# is merged with lipo; every other file must be byte-identical in the two bundles, apart
# from the code signature, which is made again (ad hoc) for the merged bundle.
set -euo pipefail

version="${1:-}"
arm64="${2:-}"
x86_64="${3:-}"
die() { echo "error: $*" >&2; exit 1; }

[[ "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] \
    || die "usage: $0 <X.Y.Z> <arm64 tubamp.component> <x86_64 tubamp.component> [out dir]"
[[ -d "$arm64" && -d "$x86_64" ]] || die "both bundles must be directories"

repo="$(cd "$(dirname "$0")/../.." && pwd)"
out="$(mkdir -p "${4:-$repo/dist}" && cd "${4:-$repo/dist}" && pwd)"
zip="$out/tubamp-$version-macos-universal.zip"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

# The client's unpacker accepts only regular files and directories.
for bundle in "$arm64" "$x86_64"; do
    odd="$(find "$bundle" ! -type f ! -type d)"
    [[ -z "$odd" ]] || die "$bundle holds something other than files and directories: $odd"
done

listing() { (cd "$1" && find . -type f ! -path './Contents/_CodeSignature/*' | LC_ALL=C sort); }
diff <(listing "$arm64") <(listing "$x86_64") >/dev/null \
    || die "the two bundles hold different files: $(diff <(listing "$arm64") <(listing "$x86_64") | head -5)"

ditto --norsrc --noextattr --noqtn "$arm64" "$stage/tubamp.component"
universal="$stage/tubamp.component"
rm -rf "$universal/Contents/_CodeSignature"

merged=0
while IFS= read -r file; do
    a="$arm64/$file" b="$x86_64/$file"
    if a_archs="$(lipo -archs "$a" 2>/dev/null)"; then
        b_archs="$(lipo -archs "$b" 2>/dev/null)" || die "$file is Mach-O in the arm64 bundle only"
        [[ "$a_archs" == arm64 && "$b_archs" == x86_64 ]] || die "$file is '$a_archs' and '$b_archs', expected arm64 and x86_64"
        lipo -create "$a" "$b" -output "$universal/$file"
        merged=$((merged + 1))
    else
        cmp -s "$a" "$b" || die "$file differs between the two bundles"
    fi
done < <(listing "$arm64")
[[ "$merged" -ge 1 ]] || die "no Mach-O file to merge"

executable="$universal/Contents/MacOS/tubamp"
archs="$(lipo -archs "$executable")"
[[ "$archs" == "arm64 x86_64" || "$archs" == "x86_64 arm64" ]] || die "the merged binary is '$archs'"
bundle_version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$universal/Contents/Info.plist")"
[[ "$bundle_version" == "$version" ]] || die "the bundle carries version '$bundle_version', expected '$version'"

xattr -cr "$universal"
codesign --force --deep --sign - "$universal"
codesign --verify --deep --strict "$universal"

rm -f "$zip"
(cd "$stage" && ditto -c -k --norsrc --keepParent tubamp.component "$zip")

# What the client refuses: another top-level entry, __MACOSX, an AppleDouble file.
bad="$(zipinfo -1 "$zip" | grep -v '^tubamp\.component/' || true)"
[[ -z "$bad" ]] || die "the zip has entries outside tubamp.component/: $bad"
bad="$(zipinfo -1 "$zip" | grep -E '(^|/)(__MACOSX|\._)' || true)"
[[ -z "$bad" ]] || die "the zip has resource-fork entries: $bad"

echo "update archive: $zip ($archs, $merged Mach-O file(s) merged, $(stat -f %z "$zip") bytes)"
