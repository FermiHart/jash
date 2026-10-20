#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/jash-perf-env-fschange.XXXXXX")"
trap 'rm -rf "$T"' EXIT
# Private unit build only; never touches a shared CMake tree or shell binary.
flags=(-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -g -O1)
if [[ "${SANITIZE:-0}" == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CC:-cc}" "${flags[@]}" -I"$ROOT/c" "$ROOT/tests/test_perf_env_fschange.c" -o "$T/test"
"$T/test" "$T"
