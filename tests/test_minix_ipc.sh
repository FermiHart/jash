#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
CC=${CC:-cc}
: "${TMPDIR:?set TMPDIR to an isolated scratch directory}"
TMP=$(mktemp -d "$TMPDIR/jash-minix-ipc.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

"$CC" -std=c11 -D__minix -U__linux__ -U__linux -Ulinux \
    -D_GNU_SOURCE -D_DEFAULT_SOURCE -D_NETBSD_SOURCE \
    -Wall -Wextra -Werror \
    -include "$ROOT/tests/minix_ipc_host.h" \
    -I"$ROOT/c" -I"$ROOT/c/osif" \
    "$ROOT/c/osif/ipc.c" "$ROOT/tests/test_minix_ipc.c" \
    -o "$TMP/test_minix_ipc"
TMPDIR="$TMP" "$TMP/test_minix_ipc"
