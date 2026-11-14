#!/usr/bin/env bash
# Builds, tests and packages the tubamp AU for one CPU type:
#   scripts/release/build-installer.sh <arm64|x86_64> <X.Y.Z> [extra cmake args...]
# Writes dist/tubamp-<version>-macos-<arch>.pkg, a package that installs for the current
# user only, into ~/Library/Audio/Plug-Ins/Components, and asks for no password. The build
# tree is build/rel-<arch> unless BUILD_DIR is set. Nothing is copied into ~/Library or
# /Library. Pass -DTUBAMP_UPDATES=ON to compile the in-plugin updater in.
set -euo pipefail

arch="${1:-}"
version="${2:-}"
shift 2 || true

die() { echo "error: $*" >&2; exit 1; }

[[ "$arch" == arm64 || "$arch" == x86_64 ]] || die "usage: $0 <arm64|x86_64> <X.Y.Z> [cmake args...]"
[[ "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || die "version must be X.Y.Z, got '$version'"

repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="${BUILD_DIR:-$repo/build/rel-$arch}"
out="${OUT_DIR:-$repo/dist}"
jobs="${JOBS:-$(sysctl -n hw.ncpu)}"

if [[ "$arch" == x86_64 ]] && ! arch -x86_64 /usr/bin/true 2>/dev/null; then
    die "the x86_64 smoke test needs Rosetta: softwareupdate --install-rosetta --agree-to-license"
fi

cmake -S "$repo" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DTUBAMP_VERSION="$version" \
    -DTUBAMP_COPY_AFTER_BUILD=OFF \
    "$@"
cmake --build "$build" --config Release --target tubamp_AU tubamp_smoke -j "$jobs"

# An empty target builds for the build machine's macOS, which older Macs refuse to load.
deployment_target="$(sed -n 's/^CMAKE_OSX_DEPLOYMENT_TARGET:STRING=//p' "$build/CMakeCache.txt")"
[[ -n "$deployment_target" ]] || die "CMAKE_OSX_DEPLOYMENT_TARGET is empty in $build/CMakeCache.txt; delete $build and build again"
case "$(sed -n 's/^TUBAMP_UPDATES:BOOL=//p' "$build/CMakeCache.txt")" in
    ON | on | TRUE | true | 1) updates=true ;;
    *) updates=false ;;
esac

component="$build/tubamp_artefacts/Release/AU/tubamp.component"
smoke="$build/tubamp_smoke_artefacts/Release/tubamp_smoke"

check_component() {
    local bundle="$1" archs bundle_version
    archs="$(lipo -archs "$bundle/Contents/MacOS/tubamp")"
    [[ "$archs" == "$arch" ]] || die "$bundle is '$archs', expected '$arch'"
    bundle_version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$bundle/Contents/Info.plist")"
    [[ "$bundle_version" == "$version" ]] || die "$bundle carries version '$bundle_version', expected '$version'"
    echo "component: $bundle archs=$archs version=$bundle_version"
}

check_component "$component"

# The engine tests pick up wavenet.nam next to the model, so point at NAM's own examples.
nam_src="$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_NEURALAMPMODELERCORE:PATH=//p' "$build/CMakeCache.txt")"
nam_src="${nam_src:-$build/_deps/neuralampmodelercore-src}"
model="$nam_src/example_models/A2.nam"
[[ -f "$model" ]] || die "test model not found at $model"
arch "-$arch" "$smoke" "$model"

codesign --force --deep --sign - "$component"
codesign --verify --deep --strict "$component"

stage="$build/pkg"
rm -rf "$stage"
mkdir -p "$stage/root" "$out"
ditto "$component" "$stage/root/tubamp.component"

# A relocatable bundle makes Installer write over any other copy of the same bundle id it
# finds on disk (a build tree, say) instead of ~/Library/Audio/Plug-Ins/Components.
pkgbuild --analyze --root "$stage/root" "$stage/component.plist" >/dev/null
plutil -replace 0.BundleIsRelocatable -bool NO "$stage/component.plist"

pkgbuild \
    --root "$stage/root" \
    --component-plist "$stage/component.plist" \
    --install-location /Library/Audio/Plug-Ins/Components \
    --identifier com.t0audio.tubamp.au \
    --version "$version" \
    "$stage/component.pkg"

# Only the current user's home is enabled, so the install location resolves inside it and
# Installer runs as the user: no password, and the updater can later replace the bundle.
cat > "$stage/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>tubamp</title>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_anywhere="false" enable_currentUserHome="true" enable_localSystem="false"/>
    <choices-outline><line choice="default"/></choices-outline>
    <choice id="default" title="tubamp"><pkg-ref id="com.t0audio.tubamp.au"/></choice>
    <pkg-ref id="com.t0audio.tubamp.au" version="$version">component.pkg</pkg-ref>
</installer-gui-script>
EOF

pkg="$out/tubamp-$version-macos-$arch.pkg"
productbuild --distribution "$stage/distribution.xml" --package-path "$stage" "$pkg"

domains="$(installer -dominfo -pkg "$pkg" | tr -d '[:space:]')"
[[ "$domains" == CurrentUserHomeDirectory ]] || die "$pkg installs into '$domains', expected CurrentUserHomeDirectory only"

pkgutil --expand-full "$pkg" "$stage/expanded"
payload="$stage/expanded/component.pkg/Payload"
check_component "$payload/tubamp.component"
codesign --verify --deep --strict "$payload/tubamp.component"
[[ "$(find "$payload" -mindepth 1 -maxdepth 1)" == "$payload/tubamp.component" ]] || die "the payload holds more than tubamp.component"

minos="$(vtool -show-build "$payload/tubamp.component/Contents/MacOS/tubamp" | awk '$1 == "minos" { print $2; exit }')"
[[ "$minos" == "$deployment_target" ]] || die "the binary needs macOS '$minos', expected '$deployment_target'"
printf '{ "version": "%s", "arch": "%s", "minimumMacOS": "%s", "updates": %s }\n' "$version" "$arch" "$minos" "$updates" \
    > "${pkg%.pkg}.build.json"

echo "installer: $pkg (macOS $minos+, current user only, updates $([[ $updates == true ]] && echo on || echo off))"
