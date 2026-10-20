#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/jash-read-regular-unit.XXXXXX")"
trap 'rm -rf "$T"' EXIT

# Compile only this translation unit in a private directory. read.c is included
# with fault macros by the harness; no shell binary or readline link is needed.
flags=(-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -g -O1)
if [[ "${SANITIZE:-0}" == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
fi
"${CC:-cc}" "${flags[@]}" -I"$ROOT/c" \
    "$ROOT/tests/test_read_regular_unit.c" -o "$T/test"
"$T/test" "$T"
