#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
CC=${CC:-cc}
: "${TMPDIR:?set TMPDIR to an isolated scratch directory}"
TMP=$(mktemp -d "$TMPDIR/jash-minix-osif.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

"$CC" -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -D_NETBSD_SOURCE -Wall -Wextra -Werror \
    -I"$ROOT/c" -I"$ROOT/c/osif" \
    "$ROOT/c/osif/minix/syscall.c" "$ROOT/tests/test_minix_osif.c" \
    -o "$TMP/test_minix_osif"
"$TMP/test_minix_osif"
printf 'MINIX OSIF syscall dispatch PASS (host libc behavioral probe)\n'
