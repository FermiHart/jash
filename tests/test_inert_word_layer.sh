#!/usr/bin/env bash
set -euo pipefail
if [[ "$(uname -s)" != Linux ]]; then
    printf '%s\n' 'SKIP: isolated word-layer unit requires GNU linker section collection'
    exit 0
fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/jash-word-layer.XXXXXX")"
trap 'rm -rf "$T"' EXIT
flags=(-std=gnu11 -D_GNU_SOURCE -O1 -g -ffunction-sections -fdata-sections)
if [[ "${SANITIZE:-0}" == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
fi
"${CC:-cc}" "${flags[@]}" -I"$ROOT/c" "$ROOT/tests/test_inert_word_layer.c" \
    "$ROOT/c/core/glob.c" "$ROOT/c/parser/cmdsub.c" -Wl,--gc-sections -o "$T/test"
"$T/test" "$T" "$@"
