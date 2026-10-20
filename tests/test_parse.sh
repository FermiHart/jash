#!/usr/bin/env bash
# Exercise the native graph parser directly; no nonexistent `jash parse` CLI.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/jash-native-parse.XXXXXX")
trap 'rm -rf "$T"' EXIT
cc -std=gnu11 -g -I"$ROOT/c" \
    ${JASH_PARSE_SANITIZERS:+-fsanitize=address,undefined -fno-omit-frame-pointer} \
    "$ROOT/tests/test_native_parse.c" "$ROOT/c/graph/jash_parse.c" \
    "$ROOT/c/graph/jash_nodes.c" "$ROOT/c/graph/jash_arena.c" \
    -o "$T/test_parse"
"$T/test_parse"
