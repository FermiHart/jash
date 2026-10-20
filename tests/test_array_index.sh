#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build}"
cmake --build "$BUILD_DIR" --target test_array_index jash >/dev/null
"$BUILD_DIR/test_array_index"

out="$(/usr/bin/env -i HOME="${TMPDIR:-/tmp}" PATH=/usr/bin:/bin \
    JASH_SIGIL=0 JASH_BRIEF=0 JASH_NUDGE=0 \
    "$BUILD_DIR/jash" -c \
    'a=([9]=nine [1]=one [key]=assoc); echo "${#a[@]}:${a[9]}:${a[key]}"; unset "a[1]"; echo "${#a[@]}:${a[9]}:${a[key]}"')"
if [ "$out" != $'3:nine:assoc\n2:nine:assoc' ]; then
    echo "FAIL: indexed aggregate expansion changed array semantics: [$out]" >&2
    exit 1
fi

out="$(/usr/bin/env -i HOME="${TMPDIR:-/tmp}" PATH=/usr/bin:/bin \
    JASH_SIGIL=0 JASH_BRIEF=0 JASH_NUDGE=0 \
    "$BUILD_DIR/jash" -c \
    'a[01]=leading; a[1]=canonical; echo "${!a[@]}|${a[@]}"')"
if [ "$out" != '01 1|leading canonical' ]; then
    echo "FAIL: equal numeric indices changed env-table order: [$out]" >&2
    exit 1
fi

out="$(/usr/bin/env -i HOME="${TMPDIR:-/tmp}" PATH=/usr/bin:/bin \
    JASH_SIGIL=0 JASH_BRIEF=0 JASH_NUDGE=0 \
    "$BUILD_DIR/jash" -c \
    'a=([9]=foo [1]=bar); echo "${a[@]/o/O}"; echo "${a[-1]}"')"
if [ "$out" != $'bar fOo\nfoo' ]; then
    echo "FAIL: element-wise aggregate operator changed ordering: [$out]" >&2
    exit 1
fi

echo "PASS: indexed aggregate expansion preserves sparse/associative semantics"
