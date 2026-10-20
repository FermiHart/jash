#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
MINIX_ROOT=${MINIX_SRC:-/home/alf/OS/minix}
CC=${CC:-cc}
: "${TMPDIR:?set TMPDIR to an isolated scratch directory}"
TMP=$(mktemp -d "$TMPDIR/jash-minix-surface.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
MINIX_BEAR_INCLUDE="$TMP/minix-bear-include"
mkdir -p "$MINIX_BEAR_INCLUDE/minix_surface_api"
cp "$MINIX_ROOT/blibc/include/bear/surface.h" \
    "$MINIX_BEAR_INCLUDE/minix_surface_api/surface.h"

"$CC" -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -Wall -Wextra -Werror \
    -I"$MINIX_ROOT/minix/include" -I"$MINIX_BEAR_INCLUDE" \
    -I"$ROOT/c/osif/minix" \
    "$ROOT/c/osif/minix/surface.c" "$ROOT/tests/test_minix_surface.c" \
    -o "$TMP/test_minix_surface"
"$TMP/test_minix_surface"
