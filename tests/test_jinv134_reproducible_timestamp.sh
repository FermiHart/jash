#!/usr/bin/env bash
# JINV-134: explicit-revision builds cannot smuggle wall-clock time into bytes.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

REVISION=0123456789abcdef0123456789abcdef01234567
EPOCH=1786925166

if cmake -S "$ROOT" -B "$TMP/missing" -DJASH_SOURCE_REVISION="$REVISION" \
    >"$TMP/missing.out" 2>&1; then
    echo "explicit revision accepted volatile wall-clock provenance" >&2
    exit 1
fi
grep -F 'require numeric SOURCE_DATE_EPOCH' "$TMP/missing.out" >/dev/null

SOURCE_DATE_EPOCH="$EPOCH" cmake -S "$ROOT" -B "$TMP/a" \
    -DJASH_SOURCE_REVISION="$REVISION" >/dev/null
sleep 2
SOURCE_DATE_EPOCH="$EPOCH" cmake -S "$ROOT" -B "$TMP/b" \
    -DJASH_SOURCE_REVISION="$REVISION" >/dev/null

for flags in "$TMP/a/CMakeFiles/jash.dir/flags.make" \
             "$TMP/b/CMakeFiles/jash.dir/flags.make"; do
    grep -F 'JASH_BUILD_TIMESTAMP=\"2026-08-17T00:06:06Z\"' \
        "$flags" >/dev/null
    grep -F 'JASH_BUILD_DATE="\"2026-08-17 00:06:06 UTC\""' \
        "$flags" >/dev/null
done

if command -v ninja >/dev/null 2>&1; then
    SOURCE_DATE_EPOCH="$EPOCH" cmake -G Ninja -S "$ROOT" -B "$TMP/ninja" \
        -DJASH_SOURCE_REVISION="$REVISION" >/dev/null
    SOURCE_DATE_EPOCH="$EPOCH" cmake --build "$TMP/ninja" \
        --target jash_gitversion >/dev/null
fi

echo "PASS: explicit-revision timestamp is source-derived and reproducible"
