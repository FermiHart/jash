#!/usr/bin/env bash
# JINV-133: a vendored source tree must identify the pinned upstream revision,
# not the host repository containing the vendor directory.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

REVISION=0123456789abcdef0123456789abcdef01234567
SOURCE_DATE_EPOCH=1786925166 cmake -S "$ROOT" -B "$TMP/good" \
    -DJASH_SOURCE_REVISION="$REVISION" \
    >/dev/null
grep -F '#define JASH_GITHASH "0123456"' "$TMP/good/jash_gitversion.h" \
    >/dev/null
if grep -F '+dirty' "$TMP/good/jash_gitversion.h" >/dev/null; then
    echo "explicit source revision inherited host dirty state" >&2
    exit 1
fi

if cmake -S "$ROOT" -B "$TMP/bad" -DJASH_SOURCE_REVISION=0123456 \
    >"$TMP/bad.out" 2>&1; then
    echo "short source revision was accepted" >&2
    exit 1
fi
grep -F 'must be exactly 40 lowercase hex characters' "$TMP/bad.out" >/dev/null

echo "PASS: vendored build identity is explicit and validated"
