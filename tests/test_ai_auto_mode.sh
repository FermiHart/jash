#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$ROOT/build/jash}"
MODULE="$ROOT/modules/ai/ai.modjash"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

for home in "$TMP/display" "$TMP/auto"; do
    mkdir -p -m 0700 "$home/.jash/modules"
    cp "$MODULE" "$home/.jash/modules/ai.modjash"
    chmod 0600 "$home/.jash/modules/ai.modjash"
done

printf '? on\n' > "$TMP/display.jsh"
display="$(HOME="$TMP/display" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 \
    "$JASH" "$TMP/display.jsh")"
[[ "$display" == *"DISPLAY ONLY"* ]]
[[ "$display" == *"? on auto"* ]]

printf '? on\n? on auto\n? status\n' > "$TMP/auto.jsh"
auto="$(HOME="$TMP/auto" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 \
    "$JASH" "$TMP/auto.jsh")"
[[ "$auto" == *"AUTO ON"* ]]
[[ "$auto" == *"active=1 auto=1"* ]]

printf '? on auto\n? status\n' > "$TMP/direct-auto.jsh"
direct="$(HOME="$TMP/auto" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 \
    "$JASH" "$TMP/direct-auto.jsh")"
[[ "$direct" == *"active=1 auto=1"* ]]

echo "PASS: ? on explains display mode and ? on auto enables execution idempotently"
