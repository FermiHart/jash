#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$ROOT/build/jash}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir "$TMP/tree"
touch "$TMP/tree/BWattLedger" "$TMP/tree/BearNXA" \
      "$TMP/tree/BearNXA.tar.gz" "$TMP/tree/BearNews" \
      "$TMP/tree/aufer" "$TMP/tree/aufer.tar.gz" \
      "$TMP/tree/axiphi" "$TMP/tree/bytecoin"

LC_ALL=en_US.UTF-8 /usr/bin/ls -1 "$TMP/tree" > "$TMP/expected"
LC_ALL=en_US.UTF-8 JASH_SIGIL=0 JASH_NUDGE=0 \
    "$JASH" -c "ls -1 $TMP/tree" > "$TMP/actual"
cmp "$TMP/expected" "$TMP/actual"

LC_ALL=en_US.UTF-8 /usr/bin/ls -1r "$TMP/tree" > "$TMP/expected-reverse"
LC_ALL=en_US.UTF-8 JASH_SIGIL=0 JASH_NUDGE=0 \
    "$JASH" -c "ls -1r $TMP/tree" > "$TMP/actual-reverse"
cmp "$TMP/expected-reverse" "$TMP/actual-reverse"

echo "PASS: ls follows locale collation, including reverse order"
