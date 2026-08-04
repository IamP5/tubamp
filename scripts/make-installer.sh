#!/usr/bin/env bash
set -euo pipefail

# Builds an unsigned .pkg installer for the tubamp AU into dist/.
# Usage: scripts/make-installer.sh [build-dir]   (default: build)
#
# The pkg is unsigned: recipients get a one-time Gatekeeper warning and
# approve it via System Settings > Privacy & Security > "Open Anyway".
# Files installed by Installer.app carry no quarantine flag, so no
# xattr cleanup is needed on the receiving end.

repo="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${1:-$repo/build}"
component="$build_dir/tubamp_artefacts/Release/AU/tubamp.component"

if [[ ! -d "$component" ]]; then
    echo "error: $component not found — build the tubamp_AU target first" >&2
    exit 1
fi

version="$(sed -nE 's/^project\(tubamp VERSION ([0-9.]+).*/\1/p' "$repo/CMakeLists.txt")"
archs="$(lipo -archs "$component/Contents/MacOS/tubamp" | tr ' ' '+')"

mkdir -p "$repo/dist"
pkg="$repo/dist/tubamp-$version-$archs.pkg"

pkgbuild \
    --component "$component" \
    --install-location "/Library/Audio/Plug-Ins/Components" \
    --identifier com.t0audio.tubamp.au \
    --version "$version" \
    "$pkg"

echo "installer: $pkg"
