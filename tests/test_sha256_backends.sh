#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/jash-sha-backends.XXXXXX")"
trap 'rm -rf "$T"' EXIT
flags=(-std=gnu11 -O2 -g -pthread -I"$ROOT/c")
if [[ "${SANITIZE:-0}" == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
fi
for mode in auto portable beyond-guard; do
    defines=()
    [[ "$mode" != portable ]] || defines+=(-DJASH_SHA256_PORTABLE_ONLY)
    [[ "$mode" != beyond-guard ]] || defines+=(-DJASH_TARGET_BEYOND)
    "${CC:-cc}" "${flags[@]}" "${defines[@]}" "$ROOT/tests/test_sha256_backends.c" \
        "$ROOT/c/module/blake3.c" -o "$T/$mode"
    "$T/$mode"
done
