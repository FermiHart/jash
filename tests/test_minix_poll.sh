#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
CC=${CC:-cc}
: "${TMPDIR:?set TMPDIR to an isolated scratch directory}"
TMP=$(mktemp -d "$TMPDIR/jash-minix-poll.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

"$CC" -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -Wall -Wextra -Werror \
    -I"$ROOT/c/osif/minix" \
    "$ROOT/c/osif/minix/poll.c" "$ROOT/tests/test_minix_poll.c" \
    -o "$TMP/test_minix_poll"
"$TMP/test_minix_poll"
printf 'MINIX OSIF poll translation PASS (host poll-provider probe)\n'
