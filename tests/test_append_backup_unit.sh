#!/usr/bin/env bash
set -euo pipefail
if [[ "$(uname -s)" != Linux ]]; then
    printf '%s\n' 'SKIP: isolated timeline unit requires GNU linker section collection'
    exit 0
fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/jash-append-backup.XXXXXX")"
trap 'rm -rf "$T"' EXIT
flags=(-std=gnu11 -D_GNU_SOURCE -O0 -g -ffunction-sections -fdata-sections)
if [[ "${SANITIZE:-0}" == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
fi
"${CC:-cc}" "${flags[@]}" -I"$ROOT/c" "$ROOT/tests/test_append_backup_unit.c" \
    "$ROOT/c/module/jash_crypto.c" -Wl,--gc-sections -o "$T/test"
"$T/test" "$T" "$@"
