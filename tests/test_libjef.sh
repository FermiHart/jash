#!/usr/bin/env bash
# test_libjef.sh — build libjef.a + its standalone unit test and run it.
# Proves the sovereign attestation crypto links and verifies with NO jash present.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
[ -d "$BUILD" ] || BUILD="$ROOT/build-speed"
LIBJEF_TMP="$(mktemp -d "${TMPDIR:-/tmp}/jash-libjef.XXXXXX")" || exit 2
trap 'rm -rf "$LIBJEF_TMP"' EXIT

cmake --build "$BUILD" --target jef_test -j4 >"$LIBJEF_TMP/build.log" 2>&1 || {
    echo "libjef: build failed"; tail -20 "$LIBJEF_TMP/build.log"; exit 1; }

[ -f "$BUILD/libjef.a" ] && echo "libjef.a: $(wc -c < "$BUILD/libjef.a") bytes"
rm -rf "$LIBJEF_TMP"
trap - EXIT
exec "$BUILD/jef_test"
