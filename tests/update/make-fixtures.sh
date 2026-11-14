#!/bin/sh
# Regenerates tests/update/fixtures: two TEST keys and envelopes signed with them by
# openssl, the tool the release pipeline signs with. The private keys exist only in a
# temporary directory for the length of this script; only public halves are written.
#
# Needs OpenSSL 3.2 or later (for -sigopt nonce-type:1): brew install openssl@3
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out="$here/fixtures"
openssl=${OPENSSL:-openssl}
"$openssl" version | grep -q '^OpenSSL 3' || { echo "make-fixtures: need OpenSSL 3, set OPENSSL=" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
umask 077
mkdir -p "$out"

magic='tubamp-update-v1'

for k in a b; do
    "$openssl" ecparam -name prime256v1 -genkey -noout -out "$tmp/$k.pem"
    {
        echo "TEST KEY: tubamp_update_tool tests only. Never a release key; CMake refuses it in src/update/keys."
        "$openssl" ec -in "$tmp/$k.pem" -pubout 2>/dev/null
    } > "$out/test-key-$k.pub.pem"
done

kid() { "$openssl" ec -in "$1" -pubout -outform DER 2>/dev/null | tail -c 65 | shasum -a 256 | cut -c1-16; }
sig() { "$openssl" dgst -sha256 -sign "$1" -sigopt nonce-type:1 "$2" | base64 | tr -d '\n'; }

# envelope <out> <payload file> <prefix or ""> <key>...
envelope() {
    file=$1 payload=$2 prefix=$3
    shift 3
    if [ -n "$prefix" ]; then printf '%s\n' "$prefix" > "$tmp/msg"; else : > "$tmp/msg"; fi
    cat "$payload" >> "$tmp/msg"
    {
        printf '%s\n' "$magic"
        for key in "$@"; do printf 'sig %s %s\n' "$(kid "$key")" "$(sig "$key" "$tmp/msg")"; done
        printf '.\n'
        cat "$payload"
    } > "$out/$file"
}

printf '%s' '{"product":"com.t0audio.tubamp","version":"0.4.0","publishedAt":"2026-11-20T10:00:00Z","minimumMacOS":"11.0","archive":{"sha256":"0c47c115a3f4cf9bd3e0b2e0f1e1c7b0d1f2a3b4c5d6e7f8091a2b3c4d5e6f70","size":17345678},"notes":"Plain text notes."}' > "$tmp/payload.json"
printf '%s' 'not json at all' > "$tmp/garbage.json"

envelope good-0.4.0.signed "$tmp/payload.json" "$magic" "$tmp/a.pem"
envelope rotated-0.4.0.signed "$tmp/payload.json" "$magic" "$tmp/b.pem" "$tmp/a.pem"
envelope no-prefix-0.4.0.signed "$tmp/payload.json" "" "$tmp/a.pem"
envelope garbage-payload.signed "$tmp/garbage.json" "$magic" "$tmp/a.pem"

chmod 644 "$out"/*
echo "make-fixtures: wrote $(ls "$out" | wc -l | tr -d ' ') files to $out"
